#include <SDL3/SDL.h>
#include "debug.h"
extern "C" {
#include "cdvd.h"
}
#include "mainloop.hpp"
#include "port_mod.h"
#include "port_graphics.h"
#include "port_present.h"
#include "render.h"
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
extern int DebugFlag;

enum { QUEUE_LIMIT = 64, WAITER_LIMIT = 32 };

typedef struct {
    char name[32];
    DebugArgs args;
    int client;
} QueuedCommand;

typedef struct {
    int used;
    int client;
    uint32_t until_frame;
    int screenshot;
} Waiter;

typedef struct {
    uint16_t buttons;
    int frames;
    int sticks;
    int left_x;
    int left_y;
    int right_x;
    int right_y;
} Injection;

DebugFrame debug_frames[DEBUG_HISTORY];
uint32_t debug_frame_total;
int debug_paused;
int debug_steps;
float debug_speed = 1.0f;
char debug_last_result[4096];

static QueuedCommand queue[QUEUE_LIMIT];
static int queued;
static Waiter waiters[WAITER_LIMIT];
static Injection injection;
static uint64_t time_exit;
static uint64_t time_enter;
static uint64_t time_drawn;
static uint64_t wait_exit;
static uint64_t wait_enter;
static uint64_t wait_drawn;
static uint64_t previous_enter;
static pid_t profiler;
static int profiler_client = DEBUG_CLIENT_NONE;
static char profiler_path[256];
static int at_frame_boundary;
static int shown_target;
static int shown_width;
static int shown_height;

static const char *button_names[16] = {"l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square",
                                       "select", "l3", "r3", "start", "up", "right", "down", "left"};

static uint64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

const DebugFrame *debug_frame_back(int back) {
    if (back < 0 || (uint32_t)back >= debug_frame_total || back >= DEBUG_HISTORY) return NULL;
    return &debug_frames[(debug_frame_total - 1 - (uint32_t)back) % DEBUG_HISTORY];
}

const char *debug_arg(const DebugArgs *args, const char *key, const char *fallback) {
    for (int i = 0; i < args->count; ++i)
        if (!strcmp(args->keys[i], key)) return args->values[i];
    return fallback;
}

long debug_arg_int(const DebugArgs *args, const char *key, long fallback) {
    const char *value = debug_arg(args, key, NULL);
    return value && *value ? strtol(value, NULL, 0) : fallback;
}

double debug_arg_number(const DebugArgs *args, const char *key, double fallback) {
    const char *value = debug_arg(args, key, NULL);
    return value && *value ? strtod(value, NULL) : fallback;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void decode_into(char *out, size_t size, const char *start, const char *end) {
    size_t length = 0;
    for (const char *c = start; c < end && length + 1 < size; ++c) {
        if (*c == '+') out[length++] = ' ';
        else if (*c == '%' && c + 2 < end && hex_value(c[1]) >= 0 && hex_value(c[2]) >= 0) {
            out[length++] = (char)(hex_value(c[1]) * 16 + hex_value(c[2]));
            c += 2;
        } else out[length++] = *c;
    }
    out[length] = 0;
}

void debug_parse_query(const char *query, DebugArgs *args) {
    memset(args, 0, sizeof *args);
    const char *cursor = query;
    while (cursor && *cursor && args->count < DEBUG_ARG_LIMIT) {
        const char *end = cursor + strcspn(cursor, "& ");
        const char *equals = (const char *)memchr(cursor, '=', (size_t)(end - cursor));
        decode_into(args->keys[args->count], sizeof args->keys[0], cursor, equals ? equals : end);
        if (equals) decode_into(args->values[args->count], sizeof args->values[0], equals + 1, end);
        if (args->keys[args->count][0]) args->count++;
        cursor = *end ? end + 1 : end;
    }
}

void debug_print(DebugReply *reply, const char *format, ...) {
    va_list list;
    va_start(list, format);
    va_list copy;
    va_copy(copy, list);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed > 0) {
        if (reply->length + (size_t)needed + 1 > reply->capacity) {
            size_t capacity = reply->capacity ? reply->capacity : 1024;
            while (reply->length + (size_t)needed + 1 > capacity) capacity *= 2;
            reply->text = (char *)realloc(reply->text, capacity);
            reply->capacity = capacity;
        }
        vsnprintf(reply->text + reply->length, (size_t)needed + 1, format, list);
        reply->length += (size_t)needed;
    }
    va_end(list);
}

void debug_print_string(DebugReply *reply, const char *text) {
    debug_print(reply, "\"");
    for (const unsigned char *c = (const unsigned char *)text; *c; ++c) {
        if (*c == '"' || *c == '\\') debug_print(reply, "\\%c", *c);
        else if (*c == '\n') debug_print(reply, "\\n");
        else if (*c < 0x20 || *c >= 0x80) debug_print(reply, "\\u%04x", *c);
        else debug_print(reply, "%c", *c);
    }
    debug_print(reply, "\"");
}

int debug_fail(DebugReply *reply, const char *format, ...) {
    char message[512];
    va_list list;
    va_start(list, format);
    vsnprintf(message, sizeof message, format, list);
    va_end(list);
    reply->length = 0;
    reply->status = 400;
    debug_print(reply, "{\"error\": ");
    debug_print_string(reply, message);
    debug_print(reply, "}");
    return 1;
}

void debug_finish(DebugReply *reply) {
    if (reply->client >= 0) {
        if (reply->body) debug_server_send(reply->client, reply->status, reply->content_type, reply->body, reply->body_length);
        else debug_server_send(reply->client, reply->status, "application/json", reply->text ? reply->text : "{}",
                               reply->text ? reply->length : 2);
    } else {
        snprintf(debug_last_result, sizeof debug_last_result, "%s", reply->text ? reply->text : "");
    }
    free(reply->text);
    free(reply->body);
    memset(reply, 0, sizeof *reply);
}

static const DebugCommand *find_command(const char *name) {
    const DebugCommand *tables[] = {debug_core_commands, debug_scenario_commands, debug_capture_commands,
                                    debug_replay_commands, debug_savestate_commands, debug_reload_commands,
                                    debug_overlay_commands};
    for (const DebugCommand *table : tables)
        for (const DebugCommand *command = table; command->name; ++command)
            if (!strcmp(command->name, name)) return command;
    return NULL;
}

static void execute(const DebugCommand *command, const DebugArgs *args, int client) {
    DebugReply reply;
    memset(&reply, 0, sizeof reply);
    reply.client = client;
    reply.status = 200;
    if (command->run(args, &reply) && reply.status == 200) reply.status = 400;
    if (!reply.deferred) debug_finish(&reply);
    else free(reply.text);
}

void debug_dispatch(const char *name, const DebugArgs *args, int client) {
    const DebugCommand *command = find_command(name);
    if (!command) {
        DebugReply reply;
        memset(&reply, 0, sizeof reply);
        reply.client = client;
        reply.status = 200;
        const char *keys[DEBUG_ARG_LIMIT];
        const char *values[DEBUG_ARG_LIMIT];
        for (int i = 0; i < args->count; ++i) {
            keys[i] = args->keys[i];
            values[i] = args->values[i];
        }
        static char answer[16384];
        answer[0] = 0;
        int result = port_mods_command(name, keys, values, args->count, answer, sizeof answer);
        if (result < 0) debug_fail(&reply, "no command %s; try /help", name);
        else if (result > 0) debug_fail(&reply, "%s", answer);
        else debug_print(&reply, "%s", answer[0] ? answer : "{}");
        debug_finish(&reply);
        return;
    }
    if (command->immediate || at_frame_boundary) {
        execute(command, args, client);
        return;
    }
    if (queued == QUEUE_LIMIT) {
        DebugReply reply;
        memset(&reply, 0, sizeof reply);
        reply.client = client;
        debug_fail(&reply, "command queue full");
        debug_finish(&reply);
        return;
    }
    snprintf(queue[queued].name, sizeof queue[queued].name, "%s", name);
    queue[queued].args = *args;
    queue[queued].client = client;
    queued++;
}

void debug_run(const char *line) {
    char name[32] = {0};
    size_t length = strcspn(line, "? ");
    if (length >= sizeof name) length = sizeof name - 1;
    memcpy(name, line, length);
    DebugArgs args;
    debug_parse_query(line[length] ? line + length + 1 : "", &args);
    debug_dispatch(name, &args, DEBUG_CLIENT_NONE);
}

void debug_run_queued(void) {
    at_frame_boundary = 1;
    for (int i = 0; i < queued; ++i) {
        const DebugCommand *command = find_command(queue[i].name);
        if (command) execute(command, &queue[i].args, queue[i].client);
    }
    queued = 0;
    at_frame_boundary = 0;
}

static int add_waiter(int client, uint32_t until_frame, int screenshot) {
    for (int i = 0; i < WAITER_LIMIT; ++i) {
        if (waiters[i].used) continue;
        waiters[i].used = 1;
        waiters[i].client = client;
        waiters[i].until_frame = until_frame;
        waiters[i].screenshot = screenshot;
        return 1;
    }
    return 0;
}

static void reply_status(int client) {
    DebugReply reply;
    memset(&reply, 0, sizeof reply);
    reply.client = client;
    reply.status = 200;
    debug_print(&reply, "{\"frame\": %u, \"field\": %llu, \"paused\": %d}", debug_frame_total,
                (unsigned long long)port_graphics.fields, debug_paused);
    debug_finish(&reply);
}

static void release_frame_waiters(void) {
    for (int i = 0; i < WAITER_LIMIT; ++i) {
        Waiter *waiter = &waiters[i];
        if (!waiter->used || waiter->screenshot || debug_frame_total < waiter->until_frame) continue;
        int client = waiter->client;
        memset(waiter, 0, sizeof *waiter);
        reply_status(client);
    }
}

static uint32_t crc_table[256];

static uint32_t crc32(uint32_t crc, const unsigned char *data, size_t length) {
    if (!crc_table[1])
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[n] = c;
        }
    crc = ~crc;
    for (size_t i = 0; i < length; ++i) crc = crc_table[(crc ^ data[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

static void put_be32(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static size_t png_chunk(unsigned char *out, const char *type, const unsigned char *data, size_t length) {
    put_be32(out, (uint32_t)length);
    memcpy(out + 4, type, 4);
    if (length) memcpy(out + 8, data, length);
    put_be32(out + 8 + length, crc32(0, out + 4, length + 4));
    return length + 12;
}

unsigned char *debug_png(const uint32_t *rgba, int width, int height, size_t *length) {
    size_t row = (size_t)width * 3 + 1;
    size_t raw = row * (size_t)height;
    size_t blocks = (raw + 65534) / 65535;
    size_t deflated = 2 + raw + blocks * 5 + 4;
    unsigned char *image = (unsigned char *)malloc(raw);
    for (int y = 0; y < height; ++y) {
        unsigned char *line = image + row * (size_t)y;
        line[0] = 0;
        for (int x = 0; x < width; ++x) {
            uint32_t pixel = rgba[(size_t)y * (size_t)width + (size_t)x];
            line[1 + x * 3] = (unsigned char)pixel;
            line[2 + x * 3] = (unsigned char)(pixel >> 8);
            line[3 + x * 3] = (unsigned char)(pixel >> 16);
        }
    }
    unsigned char *zlib = (unsigned char *)malloc(deflated);
    size_t at = 0;
    zlib[at++] = 0x78;
    zlib[at++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t offset = 0; offset < raw; offset += 65535) {
        size_t size = raw - offset < 65535 ? raw - offset : 65535;
        zlib[at++] = offset + size == raw ? 1 : 0;
        zlib[at++] = (unsigned char)size;
        zlib[at++] = (unsigned char)(size >> 8);
        zlib[at++] = (unsigned char)~size;
        zlib[at++] = (unsigned char)(~size >> 8);
        memcpy(zlib + at, image + offset, size);
        at += size;
        for (size_t i = 0; i < size; ++i) {
            a = (a + image[offset + i]) % 65521;
            b = (b + a) % 65521;
        }
    }
    put_be32(zlib + at, (b << 16) | a);
    at += 4;
    free(image);
    unsigned char *png = (unsigned char *)malloc(8 + 25 + at + 12 + 12);
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    memcpy(png, signature, 8);
    unsigned char header[13];
    put_be32(header, (uint32_t)width);
    put_be32(header + 4, (uint32_t)height);
    header[8] = 8;
    header[9] = 2;
    header[10] = header[11] = header[12] = 0;
    size_t out = 8;
    out += png_chunk(png + out, "IHDR", header, 13);
    out += png_chunk(png + out, "IDAT", zlib, at);
    out += png_chunk(png + out, "IEND", NULL, 0);
    free(zlib);
    *length = out;
    return png;
}

static void send_screenshots(int target, int width, int height) {
    int wanted = 0;
    for (int i = 0; i < WAITER_LIMIT; ++i) wanted |= waiters[i].used && waiters[i].screenshot;
    if (!wanted || !target || width <= 0 || height <= 0) return;
    uint32_t *pixels = (uint32_t *)malloc((size_t)width * (size_t)height * 4);
    render_download(target, 0, 0, width, height, pixels);
    size_t length = 0;
    unsigned char *png = debug_png(pixels, width, height, &length);
    free(pixels);
    for (int i = 0; i < WAITER_LIMIT; ++i) {
        if (!waiters[i].used || !waiters[i].screenshot) continue;
        int client = waiters[i].client;
        memset(&waiters[i], 0, sizeof waiters[i]);
        if (client >= 0) debug_server_send(client, 200, "image/png", png, length);
        else {
            FILE *file = fopen("build/port/debug-screenshot.png", "wb");
            if (file) {
                fwrite(png, 1, length, file);
                fclose(file);
            }
            snprintf(debug_last_result, sizeof debug_last_result, "saved build/port/debug-screenshot.png");
        }
    }
    free(png);
}

static void finish_profile(void) {
    if (!profiler) return;
    int status = 0;
    if (waitpid(profiler, &status, WNOHANG) != profiler) return;
    profiler = 0;
    DebugReply reply;
    memset(&reply, 0, sizeof reply);
    reply.client = profiler_client;
    reply.status = 200;
    FILE *file = fopen(profiler_path, "r");
    if (!file) {
        debug_fail(&reply, "sample wrote nothing to %s", profiler_path);
        debug_finish(&reply);
        return;
    }
    char line[1024];
    int in_top = 0, lines = 0;
    debug_print(&reply, "{\"file\": ");
    debug_print_string(&reply, profiler_path);
    debug_print(&reply, ", \"top\": [");
    while (fgets(line, sizeof line, file) && lines < 40) {
        if (strstr(line, "Sort by top of stack")) {
            in_top = 1;
            continue;
        }
        if (!in_top) continue;
        if (line[0] == '\n' || strstr(line, "Binary Images")) break;
        line[strcspn(line, "\n")] = 0;
        char *text = line;
        while (*text == ' ') text++;
        static const char *idle[] = {"__workq_kernreturn", "mach_msg2_trap", "__psynch_cvwait", "semaphore_wait",
                                     "__semwait_signal", "start_wqthread", "semaphore_timedwait_trap", "read  ",
                                     "__select", "kevent", "__ulock_wait"};
        int skip = 0;
        for (const char *name : idle) skip |= strncmp(text, name, strlen(name)) == 0;
        if (skip) continue;
        debug_print(&reply, "%s", lines ? ", " : "");
        debug_print_string(&reply, text);
        lines++;
    }
    fclose(file);
    debug_print(&reply, "]}");
    debug_finish(&reply);
}

static int command_help(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    const DebugCommand *tables[] = {debug_core_commands, debug_scenario_commands, debug_capture_commands,
                                    debug_replay_commands, debug_savestate_commands, debug_reload_commands,
                                    debug_overlay_commands};
    debug_print(reply, "{\"commands\": [");
    int first = 1;
    for (const DebugCommand *table : tables)
        for (const DebugCommand *command = table; command->name; ++command) {
            debug_print(reply, "%s{\"name\": ", first ? "" : ", ");
            debug_print_string(reply, command->name);
            debug_print(reply, ", \"help\": ");
            debug_print_string(reply, command->help);
            debug_print(reply, "}");
            first = 0;
        }
    for (int i = 0; i < port_mods_command_count(); ++i) {
        debug_print(reply, "%s{\"name\": ", first ? "" : ", ");
        debug_print_string(reply, port_mods_command_name(i));
        debug_print(reply, ", \"help\": ");
        debug_print_string(reply, port_mods_command_help(i));
        debug_print(reply, "}");
        first = 0;
    }
    debug_print(reply, "]}");
    return 0;
}

static float average_frame_ms(int frames) {
    float total = 0.0f;
    int counted = 0;
    for (int i = 0; i < frames; ++i) {
        const DebugFrame *frame = debug_frame_back(i);
        if (!frame) break;
        total += frame->frame_ms;
        counted++;
    }
    return counted ? total / (float)counted : 0.0f;
}

static int command_status(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    float frame_ms = average_frame_ms(50);
    debug_print(reply, "{\"frame\": %u, \"field\": %llu, \"fps\": %.2f, \"frame_ms\": %.2f, \"paused\": %d, "
                       "\"speed\": %.2f, \"debug_flag\": %d, \"loop\": %d",
                debug_frame_total, (unsigned long long)port_graphics.fields, frame_ms > 0 ? 1000.0f / frame_ms : 0.0f,
                frame_ms, debug_paused, debug_speed, DebugFlag, GetNowLoopNo());
    debug_scenario_status(reply);
    debug_print(reply, "}");
    return 0;
}

static void print_stats(DebugReply *reply, const PortStats *s) {
    debug_print(reply,
                "{\"draw_calls\": %u, \"mesh_calls\": %u, \"gpu_commands\": %u, \"render_passes\": %u, "
                "\"pipeline_changes\": %u, \"vertices\": %u, \"texture_uploads\": %u, \"upload_bytes\": %u, "
                "\"downloads\": %u, \"texture_decodes\": %u, \"target_creates\": %u, \"texture_renames\": %u, \"mesh_builds\": %u, "
                "\"sorted_draws\": %u, \"allocations\": %u, \"frees\": %u, \"allocation_bytes\": %u, "
                "\"disc_reads\": %u, \"disc_bytes\": %u, \"textures_live\": %u, \"pipelines\": %u, "
                "\"cached_textures\": %u, \"cached_meshes\": %u, \"arena_used\": %llu, \"arena_peak\": %llu, "
                "\"arena_blocks\": %u}",
                s->draw_calls, s->mesh_calls, s->gpu_commands, s->render_passes, s->pipeline_changes, s->vertices,
                s->texture_uploads, s->upload_bytes, s->downloads, s->texture_decodes, s->target_creates, s->texture_renames,
                s->mesh_builds, s->sorted_draws, s->allocations, s->frees, s->allocation_bytes, s->disc_reads,
                s->disc_bytes, s->textures_live, s->pipelines, s->cached_textures, s->cached_meshes,
                (unsigned long long)s->arena_used, (unsigned long long)s->arena_peak, s->arena_blocks);
}

static int command_stats(const DebugArgs *args, DebugReply *reply) {
    int frames = (int)debug_arg_int(args, "frames", 1);
    if (frames < 1) frames = 1;
    if (frames > DEBUG_HISTORY) frames = DEBUG_HISTORY;
    debug_print(reply, "{\"frames\": [");
    for (int i = frames - 1, first = 1; i >= 0; --i) {
        const DebugFrame *frame = debug_frame_back(i);
        if (!frame) continue;
        debug_print(reply, "%s{\"frame\": %u, \"field\": %llu, \"frame_ms\": %.3f, \"game_ms\": %.3f, \"draw_ms\": %.3f, "
                           "\"wait_ms\": %.3f, \"present_ms\": %.3f, \"stats\": ",
                    first ? "" : ", ", frame->frame, (unsigned long long)frame->field, frame->frame_ms, frame->game_ms,
                    frame->draw_ms, frame->wait_ms, frame->present_ms);
        print_stats(reply, &frame->stats);
        debug_print(reply, "}");
        first = 0;
    }
    debug_print(reply, "]}");
    return 0;
}

static int command_pause(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    debug_paused = 1;
    debug_steps = 0;
    debug_print(reply, "{\"paused\": 1, \"frame\": %u}", debug_frame_total);
    return 0;
}

static int command_resume(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    debug_paused = 0;
    debug_steps = 0;
    debug_print(reply, "{\"paused\": 0, \"frame\": %u}", debug_frame_total);
    return 0;
}

static int command_step(const DebugArgs *args, DebugReply *reply) {
    long frames = debug_arg_int(args, "frames", 1);
    if (frames < 1) return debug_fail(reply, "frames must be at least 1");
    debug_paused = 1;
    debug_steps = (int)frames;
    if (!add_waiter(reply->client, debug_frame_total + (uint32_t)frames, 0)) return debug_fail(reply, "too many waiters");
    reply->deferred = 1;
    return 0;
}

static int command_wait(const DebugArgs *args, DebugReply *reply) {
    long frames = debug_arg_int(args, "frames", 1);
    if (frames < 1) return debug_fail(reply, "frames must be at least 1");
    if (debug_paused) return debug_fail(reply, "the game is paused; use /step");
    if (!add_waiter(reply->client, debug_frame_total + (uint32_t)frames, 0)) return debug_fail(reply, "too many waiters");
    reply->deferred = 1;
    return 0;
}

static int command_speed(const DebugArgs *args, DebugReply *reply) {
    double value = debug_arg_number(args, "value", 1.0);
    if (value < 0.0 || value > 64.0) return debug_fail(reply, "speed must be 0 (unlimited) to 64");
    debug_speed = (float)value;
    port_field_period = value > 0.0 ? (uint64_t)(20000000.0 / value) : 0;
    debug_print(reply, "{\"speed\": %.3f}", value);
    return 0;
}

static uint16_t parse_buttons(const char *text, int *bad) {
    uint16_t buttons = 0;
    *bad = 0;
    char copy[256];
    snprintf(copy, sizeof copy, "%s", text);
    for (char *name = strtok(copy, ",+"); name; name = strtok(NULL, ",+")) {
        int found = 0;
        for (int i = 0; i < 16; ++i)
            if (!strcmp(name, button_names[i])) {
                buttons |= (uint16_t)(1u << i);
                found = 1;
            }
        if (!found) *bad = 1;
    }
    return buttons;
}

static int command_press(const DebugArgs *args, DebugReply *reply) {
    int bad = 0;
    uint16_t buttons = parse_buttons(debug_arg(args, "buttons", debug_arg(args, "button", "")), &bad);
    if (bad) return debug_fail(reply, "buttons are l2 r2 l1 r1 triangle circle cross square select l3 r3 start up right down left");
    injection.buttons = buttons;
    injection.frames = (int)debug_arg_int(args, "frames", 6);
    injection.sticks = 0;
    if (debug_arg(args, "lx", NULL) || debug_arg(args, "ly", NULL) || debug_arg(args, "rx", NULL) ||
        debug_arg(args, "ry", NULL)) {
        injection.sticks = 1;
        injection.left_x = (int)debug_arg_int(args, "lx", 0);
        injection.left_y = (int)debug_arg_int(args, "ly", 0);
        injection.right_x = (int)debug_arg_int(args, "rx", 0);
        injection.right_y = (int)debug_arg_int(args, "ry", 0);
    }
    debug_print(reply, "{\"buttons\": %u, \"frames\": %d}", buttons, injection.frames);
    return 0;
}

static int command_release(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    memset(&injection, 0, sizeof injection);
    debug_print(reply, "{\"released\": 1}");
    return 0;
}

static int command_screenshot(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    if (debug_paused && shown_target && reply->client >= 0) {
        uint32_t *pixels = (uint32_t *)malloc((size_t)shown_width * (size_t)shown_height * 4);
        render_download(shown_target, 0, 0, shown_width, shown_height, pixels);
        reply->body = debug_png(pixels, shown_width, shown_height, &reply->body_length);
        reply->content_type = "image/png";
        free(pixels);
        return 0;
    }
    if (!add_waiter(reply->client, 0, 1)) return debug_fail(reply, "too many waiters");
    reply->deferred = 1;
    return 0;
}

static int command_log(const DebugArgs *args, DebugReply *reply) {
    uint32_t total = debug_log_total();
    long since = debug_arg_int(args, "since", -1);
    long limit = debug_arg_int(args, "limit", 200);
    const char *filter = debug_arg(args, "filter", "");
    uint32_t start = since >= 0 ? (uint32_t)since : (total > (uint32_t)limit ? total - (uint32_t)limit : 0);
    debug_print(reply, "{\"total\": %u, \"lines\": [", total);
    char line[512];
    int printed = 0;
    for (uint32_t number = start; number < total && printed < limit; ++number) {
        if (!debug_log_line(number, line, sizeof line)) continue;
        if (*filter && !strstr(line, filter)) continue;
        debug_print(reply, "%s", printed ? ", " : "");
        debug_print_string(reply, line);
        printed++;
    }
    debug_print(reply, "]}");
    return 0;
}

static int command_profile(const DebugArgs *args, DebugReply *reply) {
#ifdef __APPLE__
    long seconds = debug_arg_int(args, "seconds", 3);
    if (profiler) return debug_fail(reply, "a profile is already running");
    if (seconds < 1 || seconds > 60) return debug_fail(reply, "seconds must be 1 to 60");
    snprintf(profiler_path, sizeof profiler_path, "/tmp/darkchronicle-sample-%d.txt", (int)getpid());
    char pid_text[32], seconds_text[32];
    snprintf(pid_text, sizeof pid_text, "%d", (int)getpid());
    snprintf(seconds_text, sizeof seconds_text, "%ld", seconds);
    char *argv[] = {(char *)"/usr/bin/sample", pid_text, seconds_text, (char *)"-mayDie", (char *)"-file",
                    profiler_path, NULL};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    int failed = posix_spawn(&profiler, "/usr/bin/sample", &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (failed) {
        profiler = 0;
        return debug_fail(reply, "cannot run /usr/bin/sample");
    }
    profiler_client = reply->client;
    reply->deferred = 1;
    return 0;
#else
    (void)args;
    return debug_fail(reply, "the profiler uses macOS sample; run perf record on Linux");
#endif
}

static int command_debug_flag(const DebugArgs *args, DebugReply *reply) {
    extern int port_debug;
    long value = debug_arg_int(args, "value", -1);
    if (value == 0 || value == 1) {
        port_debug = (int)value;
        DebugFlag = (int)value;
    }
    debug_print(reply, "{\"debug_flag\": %d}", DebugFlag);
    return 0;
}

static int command_quit(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    debug_print(reply, "{\"quitting\": 1}");
    debug_finish(reply);
    exit(0);
}

static int command_window(const DebugArgs *args, DebugReply *reply) {
    (void)args;
    debug_window_toggle();
    debug_print(reply, "{\"toggled\": 1}");
    return 0;
}

extern const DebugCommand debug_core_commands[] = {
    {"help", command_help, 1, "list commands"},
    {"status", command_status, 1, "frame, field, fps, pause and speed, loop, and what the scenario module reports"},
    {"stats", command_stats, 1, "per-frame timings and counters; frames=N for the last N (up to 600)"},
    {"log", command_log, 1, "captured stdout and stderr; since=N, limit=N, filter=text"},
    {"pause", command_pause, 1, "pause at the end of the current frame"},
    {"resume", command_resume, 1, "resume"},
    {"step", command_step, 1, "run frames=N frames then pause; replies when done"},
    {"wait", command_wait, 1, "reply after frames=N more frames"},
    {"speed", command_speed, 1, "value=x field-rate multiplier; 0 runs unlimited"},
    {"press", command_press, 1, "hold buttons=cross,start for frames=N (default 6); lx ly rx ry sticks -32768..32767"},
    {"release", command_release, 1, "stop injected input"},
    {"screenshot", command_screenshot, 1, "the next shown frame as PNG"},
    {"profile", command_profile, 1, "sample the process for seconds=N and return the hottest functions"},
    {"debugflag", command_debug_flag, 1, "value=0|1 sets the engine's DebugFlag (Start+Select opens the debug menu)"},
    {"window", command_window, 1, "show or hide the debug window"},
    {"windowshot", debug_window_shot, 1, "the debug window as PNG, to check the UI"},
    {"tab", debug_window_tab, 1, "select a debug window tab: name=Performance|Game|Scenario|Capture|Replay|Savestates|Log"},
    {"quit", command_quit, 1, "exit the game"},
    {NULL, NULL, 0, NULL},
};


static FILE *sound_dump;

extern "C" void debug_audio(const short *samples, int frames) {
    if (!sound_dump) return;
    fwrite(samples, sizeof(short) * 2, (size_t)frames, sound_dump);
    fflush(sound_dump);
}

extern "C" void debug_start(void) {
    debug_log_start();
    port_cdvd_log_files(1);
    const char *dump_path = getenv("DC2TK_SOUND_DUMP");
    if (dump_path) sound_dump = fopen(dump_path, "wb");
    const char *port_text = getenv("DC2TK_DEBUG_PORT");
    debug_server_start(port_text ? atoi(port_text) : 7777);
    debug_window_start();
}

extern "C" int debug_event(const void *event) {
    const SDL_Event *e = (const SDL_Event *)event;
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && e->key.windowID == SDL_GetWindowID(render_window())) {
        if (e->key.scancode == SDL_SCANCODE_F1) {
            debug_window_toggle();
            return 1;
        }
        if (e->key.scancode == SDL_SCANCODE_F5) {
            debug_paused = !debug_paused;
            debug_steps = 0;
            return 1;
        }
        if (e->key.scancode == SDL_SCANCODE_F6) {
            debug_paused = 1;
            debug_steps = 1;
            return 1;
        }
    }
    return debug_window_event(event);
}

extern "C" void debug_field(void) {
    debug_server_poll();
    finish_profile();
    debug_window_draw();
}

extern "C" void debug_frame_enter(void) {
    time_enter = now_ns();
    wait_enter = port_stats.wait_ns;
}

extern "C" void debug_frame_drawn(void) {
    time_drawn = now_ns();
    wait_drawn = port_stats.wait_ns;
}

extern "C" void debug_present(int target, int width, int height) {
    shown_target = target;
    shown_width = width;
    shown_height = height;
    send_screenshots(target, width, height);
    debug_capture_present(target, width, height);
}

extern "C" void debug_input(struct port_input_state *state) {
    if (injection.frames > 0 || injection.frames < 0) {
        state->buttons |= injection.buttons;
        if (injection.sticks) {
            state->left_x = injection.left_x;
            state->left_y = injection.left_y;
            state->right_x = injection.right_x;
            state->right_y = injection.right_y;
        }
    }
    debug_replay_input(state);
}

static void record_frame(uint64_t exit_time) {
    DebugFrame *frame = &debug_frames[debug_frame_total % DEBUG_HISTORY];
    frame->field = port_graphics.fields;
    frame->frame = debug_frame_total;
    frame->frame_ms = previous_enter ? (float)(time_enter - previous_enter) / 1e6f : 0.0f;
    frame->game_ms = time_exit ? (float)((time_enter - time_exit) - (wait_enter - wait_exit)) / 1e6f : 0.0f;
    frame->draw_ms = (float)((time_drawn - time_enter) - (wait_drawn - wait_enter)) / 1e6f;
    frame->wait_ms = (float)(port_stats.wait_ns - wait_exit) / 1e6f;
    frame->present_ms = (float)(exit_time - time_drawn) / 1e6f;
    frame->stats = port_stats;
    previous_enter = time_enter;
    debug_frame_total++;
    PortStats kept = port_stats;
    memset(&port_stats, 0, offsetof(PortStats, textures_live));
    port_stats.textures_live = kept.textures_live;
    port_stats.pipelines = kept.pipelines;
    port_stats.cached_textures = kept.cached_textures;
    port_stats.cached_meshes = kept.cached_meshes;
    port_stats.arena_used = kept.arena_used;
    port_stats.arena_peak = kept.arena_peak;
    port_stats.arena_blocks = kept.arena_blocks;
}

extern "C" void debug_frame_exit(void) {
    record_frame(now_ns());
    if (injection.frames > 0) injection.frames--;
    debug_capture_frame();
    debug_replay_frame();
    debug_savestate_frame();
    debug_reload_frame();
    debug_run_queued();
    release_frame_waiters();
    if (debug_steps > 0) debug_steps--;
    while (debug_paused && debug_steps == 0) {
        port_present_frame();
        debug_run_queued();
        debug_savestate_frame();
        SDL_Delay(10);
    }
    time_exit = now_ns();
    wait_exit = port_stats.wait_ns;
}

static const PortMod debug_tools = {
    "dc2tk.debug-tools", debug_start,   debug_event,   debug_field, debug_frame_enter, debug_frame_drawn,
    debug_frame_exit,    debug_overlay, debug_present, debug_input, debug_audio,       debug_host,
};

PORT_MOD(debug_tools)
