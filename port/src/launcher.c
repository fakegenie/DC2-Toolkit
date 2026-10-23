#define _POSIX_C_SOURCE 200809L
#include "launcher.h"
#include "cdvd.h"
#include "icon.h"
#include "port_mod.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { CANVAS_WIDTH = 960, CANVAS_HEIGHT = 600, HERO_WIDTH = 520, PANEL_LEFT = 520, PANEL_WIDTH = 440, PAD = 48 };
enum { OPTION_CHOICES = 8, OPTION_LIMIT = 16, PATH_SIZE = 1024, LINE_SIZE = 512 };
enum { GLOW_SIZE = 64, WINDOW_RADIUS = 14 };
enum { MENU_TOP = 175, MENU_STEP = 50 };
enum { LARGEST_HEIGHT = 720 };
enum { ROWS_TOP = 96, ROW_HEIGHT = 30, HEADING_HEIGHT = 28, TOGGLE_WIDTH = 34, TOGGLE_HEIGHT = 20 };
enum { PAGE_MAIN, PAGE_SETTINGS, PAGE_MODS, PAGE_COUNT };
enum { SET_LIMIT = 9, SET_NAME = 128, SET_TOP = 96, SET_HEIGHT = 50 };
enum { DISC_NONE, DISC_GOOD, DISC_WRONG };
enum { MENU_PLAY, MENU_MODS, MENU_SETTINGS, MENU_DISC, MENU_QUIT, MENU_COUNT };
enum { KEY_NONE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ACCEPT, KEY_BACK, KEY_PLAY };
enum { STYLE_MENU, STYLE_BODY, STYLE_CAPTION, STYLE_SMALL, STYLE_BADGE, STYLE_COUNT };
enum { SHAPE_ROW, SHAPE_PILL, SHAPE_RING, SHAPE_WINDOW, SHAPE_COUNT };

typedef struct {
    const char *heading;
    const char *label;
    const char *setting;
    int count;
    const char *names[OPTION_CHOICES];
    const char *values[OPTION_CHOICES];
    int chosen;
    int debug_tools;
    int toggle;
    int experimental;
} Option;

typedef struct {
    char name[SET_NAME];
    char mods[LINE_SIZE];
    int own_binary;
} ModSet;

typedef struct {
    int face;
    float size;
    float tracking;
} Style;

typedef struct {
    float radius;
    float stroke;
    int inverted;
} ShapeKind;

static Option options[] = {
    {"DISPLAY", "Window size", "render.window", 8,
     {"640 × 480", "960 × 720", "1280 × 720", "1280 × 960", "1600 × 900", "1920 × 1080", "2560 × 1440", "3840 × 2160"},
     {"640x480", "960x720", "1280x720", "1280x960", "1600x900", "1920x1080", "2560x1440", "3840x2160"}, 0, 0, 0, 0},
    {NULL, "Fullscreen", "render.fullscreen", 2, {"Off", "On"}, {"false", "true"}, 0, 0, 1, 0},
    {NULL, "Aspect ratio", "render.aspect", 4, {"4:3", "16:10", "16:9", "21:9"}, {"4:3", "16:10", "16:9", "21:9"}, 0,
     0, 0, 1},
    {NULL, "Render scale", "render.scale", 5, {"Match window", "1×", "2×", "3×", "4×"},
     {"auto", "1", "2", "3", "4"}, 1, 0, 0, 0},
    {NULL, "Anti-aliasing", "render.msaa", 4, {"Off", "2×", "4×", "8×"}, {"1", "2", "4", "8"}, 0, 0, 0, 0},
    {NULL, "Texture filtering", "render.filter", 2, {"Smooth", "Sharp"}, {"smooth", "nearest"}, 0, 0, 0, 0},
    {NULL, "Vertical sync", "render.vsync", 2, {"Off", "On"}, {"false", "true"}, 0, 0, 1, 0},
    {"MOTION", "High frame rate", "render.smooth", 8,
     {"Off", "Match display", "60 Hz", "120 Hz", "144 Hz", "165 Hz", "240 Hz", "Unlimited"},
     {"false", "display", "60", "120", "144", "165", "240", "1000"}, 0, 0, 0, 1},
    {"GAME", "Sound", "sound.mute", 2, {"Off", "On"}, {"true", "false"}, 1, 0, 1, 0},
    {NULL, "In-Game Debug Menu", "debug.menus", 2, {"Off", "On"}, {"false", "true"}, 0, 0, 1, 0},
    {NULL, "Port Developer Panel", "debug.window", 2, {"Hidden", "Shown"}, {"false", "true"}, 0, 1, 1, 0},
};

static const Style styles[STYLE_COUNT] = {
    {LAUNCHER_REGULAR, 26, 0},
    {LAUNCHER_REGULAR, 15, 0},
    {LAUNCHER_SEMIBOLD, 11, 0.24f},
    {LAUNCHER_SEMIBOLD, 12, 0.02f},
    {LAUNCHER_SEMIBOLD, 9, 0.14f},
};

static const ShapeKind shape_kinds[SHAPE_COUNT] = {
    {8, 0, 0}, {10, 0, 0}, {10, 1, 0}, {WINDOW_RADIUS, 0, 1},
};

static const unsigned char backdrop_png[] = {
#embed "../launcher/backdrop.png"
};

static const SDL_FColor ink = {0.96f, 0.95f, 0.93f, 1};
static const SDL_FColor accent = {1.0f, 113 / 255.0f, 9 / 255.0f, 1};
static const SDL_FColor panel = {0.055f, 0.058f, 0.075f, 1};
static const SDL_FColor bad = {0.96f, 0.43f, 0.37f, 1};

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *backdrop;
static SDL_Texture *glow;
static SDL_Texture *chevron;
static SDL_Texture *stroke_bar;
static SDL_Texture *shape_textures[SHAPE_COUNT];
static SDL_Texture *font_textures[STYLE_COUNT];
static LauncherFont fonts[STYLE_COUNT];
static SDL_Cursor *arrow_cursor;
static SDL_Cursor *hand_cursor;
static float pixel_scale = 1;
static int shown[OPTION_LIMIT];
static int standard[OPTION_LIMIT];
static int shown_count;
static int page;
static int menu_selected;
static int row_selected;
static int playing;
static float leaving_at = -1;
static int disc_state;
static char disc_path[PATH_SIZE];
static char settings_path[PATH_SIZE];
static char picked_path[PATH_SIZE];
static SDL_AtomicInt picked;
static int choosing;
static float pointer_x = -1;
static float pointer_y = -1;
static float page_mix[PAGE_COUNT] = {1};
static float bar_middle = MENU_TOP + MENU_STEP / 2.0f;
static float item_shift[MENU_COUNT];
static float row_middle = ROWS_TOP;
static float toggle_mix[OPTION_LIMIT];
static float row_focus[OPTION_LIMIT];
static float badge_mix[OPTION_LIMIT];
static ModSet mod_sets[SET_LIMIT];
static int set_count;
static int set_chosen;
static int set_selected;
static char set_wanted[SET_NAME];
static char sets_folder[PATH_SIZE];
static char executable[PATH_SIZE];
static char **arguments;
static int argument_count;
static float set_middle = SET_TOP;
static float set_focus[SET_LIMIT];
static float set_mark[SET_LIMIT];

static float clamp_unit(float value) { return value < 0 ? 0 : value > 1 ? 1 : value; }

static float ease_out(float t) {
    t = clamp_unit(t);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

static void approach(float *value, float target, float rate, float step) {
    *value += (target - *value) * (1 - expf(-rate * step));
}

static SDL_FColor faded(SDL_FColor colour, float alpha) {
    colour.a *= alpha;
    return colour;
}

static SDL_FColor mixed(SDL_FColor from, SDL_FColor to, float amount) {
    return (SDL_FColor){from.r + (to.r - from.r) * amount, from.g + (to.g - from.g) * amount,
                        from.b + (to.b - from.b) * amount, from.a + (to.a - from.a) * amount};
}

static int inside(SDL_FRect box, float x, float y) {
    return x >= box.x && y >= box.y && x < box.x + box.w && y < box.y + box.h;
}

static float snap(float value) { return roundf(value * pixel_scale) / pixel_scale; }

static int option_shown(const Option *option) {
    return !option->debug_tools || port_mod_loaded("dc2tk.debug-tools");
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (backslash > slash) slash = backslash;
    return slash ? slash + 1 : path;
}

static void read_options(void) {
    shown_count = 0;
    for (int index = 0; index < (int)SDL_arraysize(options); index++) {
        Option *option = &options[index];
        const char *value = port_setting(option->setting);
        standard[index] = option->chosen;
        for (int choice = 0; value && choice < option->count; choice++)
            if (!strcmp(option->values[choice], value)) option->chosen = choice;
        if (option_shown(option)) shown[shown_count++] = index;
        toggle_mix[index] = (float)option->chosen;
        badge_mix[index] = (float)(option->experimental && option->chosen);
    }
    const char *disc = port_setting("disc");
    snprintf(disc_path, sizeof disc_path, "%s", disc ? disc : "");
    const char *wanted = port_setting("mods");
    snprintf(set_wanted, sizeof set_wanted, "%s", wanted ? wanted : "");
}

static void save_options(void) {
    FILE *file = fopen(settings_path, "w");
    for (int index = 0; index < (int)SDL_arraysize(options); index++) {
        const Option *option = &options[index];
        port_setting_set(option->setting, option->values[option->chosen]);
        if (file) fprintf(file, "%s=%s\n", option->setting, option->values[option->chosen]);
    }
    port_setting_set("disc", disc_path);
    port_setting_set("mods", set_chosen ? mod_sets[set_chosen].name : "");
    if (file) fprintf(file, "disc=%s\nmods=%s\n", disc_path, set_chosen ? mod_sets[set_chosen].name : "");
    if (file) fclose(file);
    else fprintf(stderr, "launcher: cannot save %s\n", settings_path);
}

static void check_disc(const char *path) {
    struct sceCdlFILE boot;
    snprintf(disc_path, sizeof disc_path, "%s", path);
    disc_state = DISC_NONE;
    if (!*disc_path) return;
    disc_state = port_cdvd_init(disc_path) && port_cdvd_find("SCES_511.90;1", &boot) ? DISC_GOOD : DISC_WRONG;
    port_cdvd_close();
    if (disc_state == DISC_GOOD && page == PAGE_MAIN) menu_selected = MENU_PLAY;
}

static SDL_Texture *texture_of(LauncherPicture *picture) {
    SDL_Texture *texture = picture->pixels ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                                               SDL_TEXTUREACCESS_STATIC, picture->width,
                                                               picture->height)
                                           : NULL;
    if (texture) {
        SDL_UpdateTexture(texture, NULL, picture->pixels, picture->width * 4);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    }
    free(picture->pixels);
    picture->pixels = NULL;
    return texture;
}

static LauncherPicture blank_picture(int width, int height) {
    return (LauncherPicture){width, height, calloc((size_t)width * (size_t)height, 4)};
}

static void put_white(LauncherPicture *picture, int x, int y, float alpha) {
    unsigned char *pixel = picture->pixels + 4 * ((size_t)y * (size_t)picture->width + (size_t)x);
    pixel[0] = pixel[1] = pixel[2] = 255;
    pixel[3] = (unsigned char)(255 * clamp_unit(alpha));
}

static float rounded_distance(float x, float y, float half, float radius) {
    float inner = half - radius;
    float qx = fabsf(x - half) - inner, qy = fabsf(y - half) - inner;
    float outside = hypotf(fmaxf(qx, 0), fmaxf(qy, 0));
    return outside + fminf(fmaxf(qx, qy), 0) - radius;
}

static SDL_Texture *shape_texture(ShapeKind kind) {
    float radius = kind.radius * pixel_scale, stroke = kind.stroke * pixel_scale;
    int size = (int)ceilf(radius) * 2 + 2;
    LauncherPicture picture = blank_picture(size, size);
    for (int y = 0; picture.pixels && y < size; y++)
        for (int x = 0; x < size; x++) {
            float distance = rounded_distance(x + 0.5f, y + 0.5f, size / 2.0f, radius);
            float cover = clamp_unit(0.5f - distance);
            if (stroke > 0) cover -= clamp_unit(0.5f - distance - stroke);
            put_white(&picture, x, y, kind.inverted ? 1 - cover : cover);
        }
    return texture_of(&picture);
}

static float segment_distance(float x, float y, float x0, float y0, float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0;
    float t = clamp_unit(((x - x0) * dx + (y - y0) * dy) / (dx * dx + dy * dy));
    return hypotf(x - x0 - t * dx, y - y0 - t * dy);
}

static SDL_Texture *polyline_texture(const float *points, int count, float box, float width) {
    int size = (int)ceilf(box * pixel_scale);
    LauncherPicture picture = blank_picture(size, size);
    for (int y = 0; picture.pixels && y < size; y++)
        for (int x = 0; x < size; x++) {
            float px = (x + 0.5f) / pixel_scale, py = (y + 0.5f) / pixel_scale, distance = box;
            for (int point = 0; point + 1 < count; point++)
                distance = fminf(distance, segment_distance(px, py, points[point * 2], points[point * 2 + 1],
                                                            points[point * 2 + 2], points[point * 2 + 3]));
            put_white(&picture, x, y, (width / 2 - distance) * pixel_scale + 0.5f);
        }
    return texture_of(&picture);
}

static SDL_Texture *glow_texture(void) {
    LauncherPicture picture = blank_picture(GLOW_SIZE, GLOW_SIZE);
    for (int y = 0; picture.pixels && y < GLOW_SIZE; y++)
        for (int x = 0; x < GLOW_SIZE; x++) {
            float dx = (x + 0.5f) / GLOW_SIZE * 2 - 1, dy = (y + 0.5f) / GLOW_SIZE * 2 - 1;
            float fall = clamp_unit(1 - hypotf(dx, dy));
            put_white(&picture, x, y, fall * fall);
        }
    SDL_Texture *texture = texture_of(&picture);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_ADD);
    return texture;
}

static void fill(SDL_FRect box, SDL_FColor colour) {
    SDL_SetRenderDrawColorFloat(renderer, colour.r, colour.g, colour.b, colour.a);
    SDL_RenderFillRect(renderer, &box);
}

static void gradient(SDL_FRect box, SDL_FColor first, SDL_FColor second, int across) {
    SDL_Vertex corners[4] = {
        {{box.x, box.y}, first, {0, 0}},
        {{box.x + box.w, box.y}, across ? second : first, {0, 0}},
        {{box.x + box.w, box.y + box.h}, second, {0, 0}},
        {{box.x, box.y + box.h}, across ? first : second, {0, 0}},
    };
    int order[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer, NULL, corners, 4, order, 6);
}

static void tinted(SDL_Texture *texture, SDL_FColor colour) {
    SDL_SetTextureColorModFloat(texture, colour.r, colour.g, colour.b);
    SDL_SetTextureAlphaModFloat(texture, clamp_unit(colour.a));
}

static void rounded(int shape, SDL_FRect box, float radius, SDL_FColor colour) {
    SDL_Texture *texture = shape_textures[shape];
    if (!texture) return;
    float source = ceilf(shape_kinds[shape].radius * pixel_scale);
    radius = fminf(radius, fminf(box.w, box.h) / 2);
    tinted(texture, colour);
    SDL_RenderTexture9Grid(renderer, texture, NULL, source, source, source, source, radius / source, &box);
}

static void spark(float x, float y, float size, SDL_FColor colour) {
    tinted(glow, colour);
    SDL_RenderTexture(renderer, glow, NULL, &(SDL_FRect){x - size / 2, y - size / 2, size, size});
}

static void chevron_at(float x, float middle, int left, SDL_FColor colour) {
    tinted(chevron, colour);
    SDL_RenderTextureRotated(renderer, chevron, NULL, &(SDL_FRect){x - 6, middle - 6, 12, 12}, left ? 180 : 0, NULL,
                             SDL_FLIP_NONE);
}

static int next_code(const char **cursor) {
    const unsigned char *at = (const unsigned char *)*cursor;
    if (at[0] < 0x80) {
        *cursor += 1;
        return at[0];
    }
    if ((at[0] & 0xE0) == 0xC0 && (at[1] & 0xC0) == 0x80) {
        *cursor += 2;
        return (at[0] & 0x1F) << 6 | (at[1] & 0x3F);
    }
    *cursor += 1;
    return '?';
}

static const LauncherGlyph *glyph_of(const LauncherFont *font, int code) {
    int index = code - LAUNCHER_FIRST;
    return index >= 0 && index < LAUNCHER_GLYPHS ? &font->glyphs[index] : NULL;
}

static float measure(int style, const char *words) {
    const LauncherFont *font = &fonts[style];
    float width = 0, tracking = styles[style].tracking * styles[style].size;
    int previous = 0;
    for (const char *cursor = words; *cursor;) {
        int code = next_code(&cursor);
        const LauncherGlyph *glyph = glyph_of(font, code);
        if (!glyph) continue;
        if (previous) width += launcher_font_kern(font, previous, code) / pixel_scale + tracking;
        width += glyph->advance / pixel_scale;
        previous = code;
    }
    return width;
}

static void run(int style, const char *words, float x, float baseline, SDL_FColor colour) {
    const LauncherFont *font = &fonts[style];
    SDL_Texture *texture = font_textures[style];
    if (!texture || colour.a <= 0) return;
    float tracking = styles[style].tracking * styles[style].size;
    int previous = 0;
    tinted(texture, colour);
    baseline = snap(baseline);
    for (const char *cursor = words; *cursor;) {
        int code = next_code(&cursor);
        const LauncherGlyph *glyph = glyph_of(font, code);
        if (!glyph) continue;
        if (previous) x += launcher_font_kern(font, previous, code) / pixel_scale + tracking;
        SDL_FRect source = {glyph->x0, glyph->y0, glyph->x1 - glyph->x0, glyph->y1 - glyph->y0};
        SDL_FRect place = {snap(x + glyph->left / pixel_scale), baseline + glyph->top / pixel_scale,
                           source.w / pixel_scale, source.h / pixel_scale};
        if (source.w > 0) SDL_RenderTexture(renderer, texture, &source, &place);
        x += glyph->advance / pixel_scale;
        previous = code;
    }
}

static void text_top(int style, const char *words, float x, float top, SDL_FColor colour) {
    run(style, words, x, top + fonts[style].ascent / pixel_scale, colour);
}

static void text_middle(int style, const char *words, float x, float middle, SDL_FColor colour) {
    run(style, words, x, middle + styles[style].size * 0.36f, colour);
}

static void text_right(int style, const char *words, float right, float middle, SDL_FColor colour) {
    text_middle(style, words, right - measure(style, words), middle, colour);
}

static const char *menu_name(int item) {
    if (item == MENU_PLAY) return "Play";
    if (item == MENU_MODS) return "Mods";
    if (item == MENU_SETTINGS) return "Settings";
    if (item == MENU_DISC) return "Disc";
    return "Quit";
}

static SDL_FRect menu_box(int item) {
    return (SDL_FRect){PANEL_LEFT + PAD - 20, MENU_TOP + item * (float)MENU_STEP, PANEL_WIDTH - 2 * PAD + 40, MENU_STEP};
}

static SDL_FRect set_box(int row) {
    return (SDL_FRect){PANEL_LEFT + PAD - 14, SET_TOP + row * (float)SET_HEIGHT, PANEL_WIDTH - 2 * PAD + 28, SET_HEIGHT - 4};
}

static SDL_FRect close_box(void) { return (SDL_FRect){CANVAS_WIDTH - 46, 16, 30, 30}; }

static SDL_FRect back_box(void) { return (SDL_FRect){PANEL_LEFT + PAD - 20, 34, 170, 40}; }

static float row_top(int row) {
    float top = ROWS_TOP;
    for (int index = 0; index <= row && index < shown_count; index++) {
        if (options[shown[index]].heading) top += HEADING_HEIGHT;
        if (index < row) top += ROW_HEIGHT;
    }
    return row == shown_count ? top + 12 : top;
}

static SDL_FRect row_box(int row) {
    return (SDL_FRect){PANEL_LEFT + PAD - 14, row_top(row), PANEL_WIDTH - 2 * PAD + 28, ROW_HEIGHT};
}

static float control_right(void) { return PANEL_LEFT + PANEL_WIDTH - PAD; }

static void draw_hero(float now) {
    float width, height;
    SDL_GetTextureSize(backdrop, &width, &height);
    float crop = height * HERO_WIDTH / CANVAS_HEIGHT, travel = width - crop;
    float pan = 0.5f + 0.45f * sinf(now * 0.012f);
    float dim = 1 - 0.18f * (1 - page_mix[PAGE_MAIN]);
    SDL_SetTextureColorModFloat(backdrop, dim, dim, dim);
    SDL_RenderTexture(renderer, backdrop, &(SDL_FRect){travel * pan, 0, crop, height},
                      &(SDL_FRect){0, 0, HERO_WIDTH, CANVAS_HEIGHT});
}

static void shorten(int style, const char *words, float width, char *out, size_t size) {
    snprintf(out, size, "%s", words);
    while (strlen(out) > 4 && measure(style, out) > width) strcpy(out + strlen(out) - 4, "...");
}

static void draw_value(int item, const char *words, SDL_FColor colour, float alpha, float shift) {
    char value[LINE_SIZE];
    float room = control_right() - (PANEL_LEFT + PAD + 10 + measure(STYLE_MENU, menu_name(item)) + 32);
    shorten(STYLE_BODY, words, room, value, sizeof value);
    text_right(STYLE_BODY, value, control_right() + shift, menu_box(item).y + MENU_STEP / 2.0f,
               faded(colour, (0.38f + 0.3f * item_shift[item]) * alpha));
}

static void draw_main(float now) {
    float alpha = page_mix[PAGE_MAIN], shift = -18 * (1 - alpha);
    if (alpha <= 0.01f) return;
    float pulse = 0.85f + 0.15f * sinf(now * 3);
    rounded(SHAPE_PILL, (SDL_FRect){PANEL_LEFT + PAD - 18 + shift, bar_middle - 12, 3, 24}, 1.5f,
            faded(accent, alpha * pulse));
    spark(PANEL_LEFT + PAD - 16.5f + shift, bar_middle, 40, faded(accent, 0.22f * alpha));
    for (int item = 0; item < MENU_COUNT; item++) {
        SDL_FRect box = menu_box(item);
        int enabled = item != MENU_PLAY || disc_state == DISC_GOOD;
        float lit = item_shift[item];
        SDL_FColor colour = faded(ink, enabled ? 0.42f + 0.58f * lit : 0.2f);
        text_middle(STYLE_MENU, menu_name(item), PANEL_LEFT + PAD + 10 * lit + shift, box.y + box.h / 2,
                    faded(colour, alpha));
    }
    draw_value(MENU_MODS, set_chosen ? mod_sets[set_chosen].name : "None", ink, alpha, shift);
    draw_value(MENU_DISC, disc_state == DISC_NONE ? "None" : base_name(disc_path), disc_state == DISC_WRONG ? bad : ink,
               alpha, shift);
}

static void draw_toggle(int index, float right, float middle, float alpha, float focus) {
    float mix = toggle_mix[index];
    SDL_FRect track = {right - TOGGLE_WIDTH, middle - TOGGLE_HEIGHT / 2.0f, TOGGLE_WIDTH, TOGGLE_HEIGHT};
    rounded(SHAPE_PILL, track, 10, faded(mixed(faded(ink, 0.14f + 0.08f * focus), accent, mix), alpha));
    float knob_x = track.x + 10 + mix * (TOGGLE_WIDTH - 20);
    SDL_FColor knob = mixed(faded(ink, 0.9f), (SDL_FColor){0.1f, 0.08f, 0.06f, 1}, mix);
    rounded(SHAPE_PILL, (SDL_FRect){knob_x - 8, middle - 8, 16, 16}, 8, faded(knob, alpha));
}

static void draw_choice(const Option *option, float right, float middle, float alpha, float focus) {
    const char *value = option->names[option->chosen];
    float value_right = right - 20 * focus;
    text_right(STYLE_BODY, value, value_right, middle, faded(ink, (0.55f + 0.4f * focus) * alpha));
    if (focus < 0.01f) return;
    chevron_at(right - 4, middle, 0, faded(accent, alpha * focus));
    chevron_at(value_right - measure(STYLE_BODY, value) - 14, middle, 1, faded(accent, alpha * focus));
}

static void draw_badge(float x, float middle, float alpha) {
    const char *words = "EXPERIMENTAL";
    SDL_FRect box = {x, middle - 8, measure(STYLE_BADGE, words) + 14, 16};
    rounded(SHAPE_PILL, box, 8, faded(accent, 0.14f * alpha));
    rounded(SHAPE_RING, box, 8, faded(accent, 0.55f * alpha));
    text_middle(STYLE_BADGE, words, x + 7, middle, faded(accent, alpha));
}

static void draw_header(const char *title, float alpha, float shift) {
    SDL_FRect back = back_box();
    int back_lit = inside(back, pointer_x, pointer_y);
    chevron_at(back.x + 26 + shift, back.y + back.h / 2 + 2, 1, faded(ink, (back_lit ? 0.95f : 0.6f) * alpha));
    text_middle(STYLE_MENU, title, back.x + 40 + shift, back.y + back.h / 2, faded(ink, alpha));
}

static void draw_mods(void) {
    float alpha = page_mix[PAGE_MODS], shift = 18 * (1 - alpha);
    if (alpha <= 0.01f) return;
    draw_header("Mods", alpha, shift);
    SDL_FRect highlight = set_box(0);
    highlight.y = set_middle - highlight.h / 2;
    highlight.x += shift;
    rounded(SHAPE_ROW, highlight, 8, faded(ink, 0.065f * alpha));
    for (int row = 0; row < set_count; row++) {
        SDL_FRect box = set_box(row);
        float middle = box.y + box.h / 2, x = PANEL_LEFT + PAD + shift, right = control_right() + shift;
        float focus = set_focus[row], mark = set_mark[row];
        char line[LINE_SIZE];
        shorten(STYLE_SMALL, mod_sets[row].mods, right - x - 40, line, sizeof line);
        text_middle(STYLE_BODY, row ? mod_sets[row].name : "No mods", x, middle - 9,
                    faded(ink, (0.72f + 0.28f * focus) * alpha));
        text_middle(STYLE_SMALL, line, x, middle + 11, faded(ink, 0.45f * alpha));
        SDL_FRect ring = {right - 18, middle - 9, 18, 18};
        rounded(SHAPE_RING, ring, 9, faded(mixed(faded(ink, 0.35f), accent, mark), alpha));
        if (mark > 0.01f)
            rounded(SHAPE_PILL, (SDL_FRect){right - 9 - 5 * mark, middle - 5 * mark, 10 * mark, 10 * mark}, 5 * mark,
                    faded(accent, alpha));
    }
    if (set_count == 1)
        text_top(STYLE_SMALL, "Mod sets built with dc2tk play --native --set appear here.", PANEL_LEFT + PAD + shift,
                 SET_TOP + SET_HEIGHT + 16, faded(ink, 0.45f * alpha));
}

static int at_defaults(void) {
    for (int index = 0; index < (int)SDL_arraysize(options); index++)
        if (options[index].chosen != standard[index]) return 0;
    return 1;
}

static void restore_defaults(void) {
    for (int index = 0; index < (int)SDL_arraysize(options); index++) options[index].chosen = standard[index];
}

static void draw_settings(void) {
    float alpha = page_mix[PAGE_SETTINGS], shift = 18 * (1 - alpha);
    if (alpha <= 0.01f) return;
    draw_header("Settings", alpha, shift);
    SDL_FRect highlight = row_box(0);
    highlight.y = row_middle - ROW_HEIGHT / 2.0f;
    highlight.x += shift;
    rounded(SHAPE_ROW, highlight, 8, faded(ink, 0.065f * alpha));
    for (int row = 0; row < shown_count; row++) {
        int index = shown[row];
        const Option *option = &options[index];
        SDL_FRect box = row_box(row);
        float middle = box.y + box.h / 2, x = PANEL_LEFT + PAD + shift, right = control_right() + shift;
        float focus = row_focus[row];
        if (option->heading)
            text_middle(STYLE_CAPTION, option->heading, x, box.y - HEADING_HEIGHT / 2.0f + 2,
                        faded(ink, 0.38f * alpha));
        text_middle(STYLE_BODY, option->label, x, middle, faded(ink, (0.72f + 0.28f * focus) * alpha));
        if (badge_mix[index] > 0.01f) draw_badge(x + measure(STYLE_BODY, option->label) + 10, middle, alpha * badge_mix[index]);
        if (option->toggle) draw_toggle(index, right, middle, alpha, focus);
        else draw_choice(option, right, middle, alpha, focus);
    }
    SDL_FRect restore = row_box(shown_count);
    float strength = at_defaults() ? 0.35f : 0.75f + 0.25f * row_focus[shown_count];
    text_middle(STYLE_BODY, "Restore default settings", PANEL_LEFT + PAD + shift, restore.y + restore.h / 2,
                faded(accent, strength * alpha));
}

static void draw_close(void) {
    SDL_FRect box = close_box();
    int lit = inside(box, pointer_x, pointer_y);
    float middle_x = box.x + box.w / 2, middle_y = box.y + box.h / 2;
    if (lit) rounded(SHAPE_PILL, box, 15, faded(ink, 0.08f));
    tinted(stroke_bar, faded(ink, lit ? 0.95f : 0.5f));
    for (int turn = 0; turn < 2; turn++)
        SDL_RenderTextureRotated(renderer, stroke_bar, NULL, &(SDL_FRect){middle_x - 6, middle_y - 6, 12, 12},
                                 turn ? 45 : -45, NULL, SDL_FLIP_NONE);
}

static void draw(float now) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    fill((SDL_FRect){0, 0, CANVAS_WIDTH, CANVAS_HEIGHT}, panel);
    draw_hero(now);
    fill((SDL_FRect){HERO_WIDTH, 0, 1 / pixel_scale, CANVAS_HEIGHT}, faded(ink, 0.07f));
    gradient((SDL_FRect){HERO_WIDTH, 0, 48, CANVAS_HEIGHT}, (SDL_FColor){0, 0, 0, 0.22f}, (SDL_FColor){0, 0, 0, 0}, 1);
    draw_main(now);
    draw_settings();
    draw_mods();
    draw_close();
    float fade = 1 - ease_out(now / 0.6f);
    if (leaving_at >= 0) fade = fmaxf(fade, ease_out((now - leaving_at) / 0.22f));
    if (fade > 0) fill((SDL_FRect){0, 0, CANVAS_WIDTH, CANVAS_HEIGHT}, faded(panel, fade));
    SDL_RenderTexture9Grid(renderer, shape_textures[SHAPE_WINDOW], NULL, ceilf(WINDOW_RADIUS * pixel_scale),
                           ceilf(WINDOW_RADIUS * pixel_scale), ceilf(WINDOW_RADIUS * pixel_scale),
                           ceilf(WINDOW_RADIUS * pixel_scale), WINDOW_RADIUS / ceilf(WINDOW_RADIUS * pixel_scale),
                           &(SDL_FRect){0, 0, CANVAS_WIDTH, CANVAS_HEIGHT});
}

static void animate(float step) {
    for (int which = 0; which < PAGE_COUNT; which++) approach(&page_mix[which], page == which, 12, step);
    approach(&set_middle, set_box(set_selected).y + set_box(set_selected).h / 2, 20, step);
    for (int row = 0; row < set_count; row++) {
        approach(&set_focus[row], row == set_selected, 18, step);
        approach(&set_mark[row], row == set_chosen, 16, step);
    }
    approach(&bar_middle, MENU_TOP + menu_selected * (float)MENU_STEP + MENU_STEP / 2.0f, 16, step);
    for (int item = 0; item < MENU_COUNT; item++) approach(&item_shift[item], item == menu_selected, 14, step);
    approach(&row_middle, row_top(row_selected) + ROW_HEIGHT / 2.0f, 20, step);
    for (int index = 0; index < (int)SDL_arraysize(options); index++)
        approach(&toggle_mix[index], (float)options[index].chosen, 16, step);
    for (int row = 0; row <= shown_count; row++) approach(&row_focus[row], row == row_selected, 18, step);
    for (int index = 0; index < (int)SDL_arraysize(options); index++)
        approach(&badge_mix[index], options[index].experimental && options[index].chosen, 14, step);
}

static void picked_file(void *data, const char *const *files, int filter) {
    (void)data;
    (void)filter;
    if (files && files[0]) snprintf(picked_path, sizeof picked_path, "%s", files[0]);
    else picked_path[0] = 0;
    SDL_SetAtomicInt(&picked, 1);
}

static void choose_disc(void) {
    static const SDL_DialogFileFilter filters[] = {{"Disc images", "iso;bin;img"}};
    if (choosing || SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) return;
    choosing = 1;
    SDL_ShowOpenFileDialog(picked_file, NULL, window, filters, 1, disc_path[0] ? disc_path : NULL, false);
}

static void leave(int play, float now) {
    if (leaving_at >= 0) return;
    playing = play;
    leaving_at = now;
}

static void play(float now) {
    if (disc_state == DISC_GOOD) leave(1, now);
    else choose_disc();
}

static void focus_row(int row) { row_selected = row; }

static void open_settings(void) { page = PAGE_SETTINGS; }

static void open_mods(void) {
    page = PAGE_MODS;
    set_selected = set_chosen;
}

static void change(int row, int step) {
    if (row == shown_count) return;
    Option *option = &options[shown[row]];
    option->chosen = (option->chosen + step + option->count) % option->count;
}

static void activate_main(int item, float now) {
    menu_selected = item;
    if (item == MENU_PLAY) play(now);
    if (item == MENU_MODS) open_mods();
    if (item == MENU_SETTINGS) open_settings();
    if (item == MENU_DISC) choose_disc();
    if (item == MENU_QUIT) leave(0, now);
}

static void press(int key, float now) {
    if (key == KEY_PLAY) play(now);
    if (page == PAGE_MAIN) {
        if (key == KEY_UP) menu_selected = (menu_selected + MENU_COUNT - 1) % MENU_COUNT;
        if (key == KEY_DOWN) menu_selected = (menu_selected + 1) % MENU_COUNT;
        if (key == KEY_ACCEPT) activate_main(menu_selected, now);
        if (key == KEY_BACK) leave(0, now);
        return;
    }
    if (page == PAGE_MODS) {
        if (key == KEY_UP) set_selected = (set_selected + set_count - 1) % set_count;
        if (key == KEY_DOWN) set_selected = (set_selected + 1) % set_count;
        if (key == KEY_ACCEPT) set_chosen = set_selected;
        if (key == KEY_BACK) page = PAGE_MAIN;
        return;
    }
    if (key == KEY_UP) focus_row((row_selected + shown_count) % (shown_count + 1));
    if (key == KEY_DOWN) focus_row((row_selected + 1) % (shown_count + 1));
    if (key == KEY_LEFT) change(row_selected, -1);
    if (key == KEY_RIGHT || key == KEY_ACCEPT) change(row_selected, 1);
    if (key == KEY_ACCEPT && row_selected == shown_count) restore_defaults();
    if (key == KEY_BACK) page = PAGE_MAIN;
}

static int key_of(SDL_Keycode code) {
    if (code == SDLK_UP || code == SDLK_W) return KEY_UP;
    if (code == SDLK_DOWN || code == SDLK_S) return KEY_DOWN;
    if (code == SDLK_LEFT || code == SDLK_A) return KEY_LEFT;
    if (code == SDLK_RIGHT || code == SDLK_D) return KEY_RIGHT;
    if (code == SDLK_RETURN || code == SDLK_KP_ENTER || code == SDLK_SPACE) return KEY_ACCEPT;
    if (code == SDLK_ESCAPE || code == SDLK_BACKSPACE) return KEY_BACK;
    if (code == SDLK_P) return KEY_PLAY;
    return KEY_NONE;
}

static int pad_key_of(Uint8 button) {
    if (button == SDL_GAMEPAD_BUTTON_DPAD_UP) return KEY_UP;
    if (button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) return KEY_DOWN;
    if (button == SDL_GAMEPAD_BUTTON_DPAD_LEFT) return KEY_LEFT;
    if (button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) return KEY_RIGHT;
    if (button == SDL_GAMEPAD_BUTTON_SOUTH) return KEY_ACCEPT;
    if (button == SDL_GAMEPAD_BUTTON_EAST) return KEY_BACK;
    if (button == SDL_GAMEPAD_BUTTON_START) return KEY_PLAY;
    return KEY_NONE;
}

static int menu_item_at(float x, float y) {
    for (int item = 0; page == PAGE_MAIN && item < MENU_COUNT; item++)
        if (inside(menu_box(item), x, y)) return item;
    return -1;
}

static int row_at(float x, float y) {
    for (int row = 0; page == PAGE_SETTINGS && row <= shown_count; row++)
        if (inside(row_box(row), x, y)) return row;
    return -1;
}

static int set_at(float x, float y) {
    for (int row = 0; page == PAGE_MODS && row < set_count; row++)
        if (inside(set_box(row), x, y)) return row;
    return -1;
}

static int clickable(float x, float y) {
    return inside(close_box(), x, y) || menu_item_at(x, y) >= 0 || row_at(x, y) >= 0 || set_at(x, y) >= 0 ||
           (page != PAGE_MAIN && inside(back_box(), x, y));
}

static void hover(float x, float y) {
    pointer_x = x;
    pointer_y = y;
    if (menu_item_at(x, y) >= 0) menu_selected = menu_item_at(x, y);
    if (row_at(x, y) >= 0) focus_row(row_at(x, y));
    if (set_at(x, y) >= 0) set_selected = set_at(x, y);
    SDL_SetCursor(clickable(x, y) ? hand_cursor : arrow_cursor);
}

static void click_row(int row, float x) {
    if (row == shown_count) {
        restore_defaults();
        return;
    }
    const Option *option = &options[shown[row]];
    float value_left = control_right() - 18 - measure(STYLE_BODY, option->names[option->chosen]) - 20;
    change(row, !option->toggle && x < value_left ? -1 : 1);
}

static void click(float x, float y, int backwards, float now) {
    if (inside(close_box(), x, y)) leave(0, now);
    else if (menu_item_at(x, y) >= 0) activate_main(menu_item_at(x, y), now);
    else if (page != PAGE_MAIN && inside(back_box(), x, y)) page = PAGE_MAIN;
    else if (set_at(x, y) >= 0) set_chosen = set_selected = set_at(x, y);
    else if (row_at(x, y) >= 0 && backwards) change(row_at(x, y), -1);
    else if (row_at(x, y) >= 0) click_row(row_at(x, y), x);
}

static SDL_HitTestResult hit_test(SDL_Window *where, const SDL_Point *point, void *data) {
    (void)where;
    (void)data;
    float x, y;
    SDL_RenderCoordinatesFromWindow(renderer, (float)point->x, (float)point->y, &x, &y);
    if (clickable(x, y)) return SDL_HITTEST_NORMAL;
    if (x < HERO_WIDTH || y < 30) return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

static void handle(SDL_Event *event, float now) {
    SDL_ConvertEventToRenderCoordinates(renderer, event);
    if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) leave(0, now);
    if (event->type == SDL_EVENT_KEY_DOWN) press(key_of(event->key.key), now);
    if (event->type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(event->gdevice.which);
    if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) press(pad_key_of(event->gbutton.button), now);
    if (event->type == SDL_EVENT_MOUSE_MOTION) hover(event->motion.x, event->motion.y);
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        click(event->button.x, event->button.y, event->button.button == SDL_BUTTON_RIGHT, now);
    if (event->type == SDL_EVENT_DROP_FILE && event->drop.data) check_disc(event->drop.data);
}

static float window_zoom(void) {
    SDL_Rect usable;
    if (!SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable)) return 1;
    float zoom = SDL_min(usable.w * 0.9f / CANVAS_WIDTH, usable.h * 0.9f / CANVAS_HEIGHT);
    return SDL_clamp(zoom, 0.5f, (float)LARGEST_HEIGHT / CANVAS_HEIGHT);
}

static void make_textures(void) {
    static const float chevron_points[] = {4, 2, 8, 6, 4, 10};
    static const float bar_points[] = {1, 6, 11, 6};
    int width = 0, height = 0;
    SDL_GetRenderOutputSize(renderer, &width, &height);
    pixel_scale = width > 0 ? (float)width / CANVAS_WIDTH : 1;
    SDL_Surface *painting = SDL_LoadPNG_IO(SDL_IOFromConstMem(backdrop_png, sizeof backdrop_png), true);
    backdrop = painting ? SDL_CreateTextureFromSurface(renderer, painting) : NULL;
    SDL_DestroySurface(painting);
    SDL_SetTextureScaleMode(backdrop, SDL_SCALEMODE_LINEAR);
    glow = glow_texture();
    chevron = polyline_texture(chevron_points, 3, 12, 1.6f);
    stroke_bar = polyline_texture(bar_points, 2, 12, 1.6f);
    for (int shape = 0; shape < SHAPE_COUNT; shape++) shape_textures[shape] = shape_texture(shape_kinds[shape]);
    SDL_SetTextureBlendMode(shape_textures[SHAPE_WINDOW],
                            SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                                                       SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ZERO,
                                                       SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD));
    for (int style = 0; style < STYLE_COUNT; style++)
        if (launcher_font_bake(styles[style].face, styles[style].size * pixel_scale, &fonts[style]))
            font_textures[style] = texture_of(&fonts[style].atlas);
}

static void forget(SDL_Texture **texture) {
    if (*texture) SDL_DestroyTexture(*texture);
    *texture = NULL;
}

static int open_window(int hidden) {
    float zoom = window_zoom();
    SDL_WindowFlags flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_TRANSPARENT | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                            (hidden ? SDL_WINDOW_HIDDEN : 0);
    if (!SDL_CreateWindowAndRenderer("Dark Chronicle", (int)(CANVAS_WIDTH * zoom), (int)(CANVAS_HEIGHT * zoom), flags,
                                     &window, &renderer)) {
        fprintf(stderr, "launcher: %s\n", SDL_GetError());
        return 0;
    }
    SDL_SetRenderLogicalPresentation(renderer, CANVAS_WIDTH, CANVAS_HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowHitTest(window, hit_test, NULL);
    port_window_icon(window);
    arrow_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    hand_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    make_textures();
    return 1;
}

static void close_window(void) {
    forget(&backdrop);
    forget(&glow);
    forget(&chevron);
    forget(&stroke_bar);
    for (int shape = 0; shape < SHAPE_COUNT; shape++) forget(&shape_textures[shape]);
    for (int style = 0; style < STYLE_COUNT; style++) forget(&font_textures[style]);
    SDL_DestroyCursor(arrow_cursor);
    SDL_DestroyCursor(hand_cursor);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    renderer = NULL;
    window = NULL;
}

static void find_sets(const char *program) {
    const char *base = SDL_GetBasePath();
    const char *name = strrchr(program, '/');
    snprintf(executable, sizeof executable, "%s%s", base ? base : "", name ? name + 1 : program);
    char parent[PATH_SIZE];
    snprintf(parent, sizeof parent, "%s", base ? base : "./");
    parent[strlen(parent) - 1] = 0;
    char *slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    slash = strrchr(parent, '/');
    if (!strcmp(slash ? slash + 1 : parent, "sets")) snprintf(sets_folder, sizeof sets_folder, "%s", parent);
    else snprintf(sets_folder, sizeof sets_folder, "%s/sets", parent);
}

static int is_folder(const char *path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

static int is_file(const char *path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

static void describe_set(ModSet *set) {
    char path[PATH_SIZE];
    snprintf(path, sizeof path, "%s/%s/mods.txt", sets_folder, set->name);
    char *names = SDL_LoadFile(path, NULL);
    set->mods[0] = 0;
    for (char *line = names ? strtok(names, "\r\n") : NULL; line; line = strtok(NULL, "\r\n")) {
        size_t used = strlen(set->mods);
        snprintf(set->mods + used, sizeof set->mods - used, "%s%s", used ? ", " : "", line);
    }
    if (!set->mods[0]) snprintf(set->mods, sizeof set->mods, "Built by dc2tk");
    snprintf(path, sizeof path, "%s/%s/darkchronicle", sets_folder, set->name);
    set->own_binary = is_file(path);
    SDL_free(names);
}

static void read_sets(void) {
    set_count = 1;
    snprintf(mod_sets[0].name, SET_NAME, "%s", "");
    snprintf(mod_sets[0].mods, LINE_SIZE, "%s", "The game as it is on the disc");
    int count = 0;
    char **names = SDL_GlobDirectory(sets_folder, "*", 0, &count);
    for (int index = 0; names && index < count && set_count < SET_LIMIT; index++) {
        char overrides[PATH_SIZE];
        snprintf(overrides, sizeof overrides, "%s/%s/overrides", sets_folder, names[index]);
        if (!strcmp(names[index], "none") || !is_folder(overrides)) continue;
        int at = set_count++;
        while (at > 1 && strcmp(mod_sets[at - 1].name, names[index]) > 0) {
            mod_sets[at] = mod_sets[at - 1];
            at--;
        }
        snprintf(mod_sets[at].name, SET_NAME, "%s", names[index]);
    }
    SDL_free(names);
    set_chosen = 0;
    for (int index = 1; index < set_count; index++) {
        describe_set(&mod_sets[index]);
        if (!strcmp(mod_sets[index].name, set_wanted)) set_chosen = index;
    }
}

static void start_game(void) {
    const ModSet *set = &mod_sets[set_chosen];
    char overrides[PATH_SIZE], binary[PATH_SIZE];
    snprintf(overrides, sizeof overrides, "%s/%s/overrides", sets_folder, set_chosen ? set->name : "none");
    if (set->own_binary) snprintf(binary, sizeof binary, "%s/%s/darkchronicle", sets_folder, set->name);
    else snprintf(binary, sizeof binary, "%s", executable);
    char *started[64];
    int count = 0;
    started[count++] = binary;
    for (int index = 1; index < argument_count && count < 62; index++) started[count++] = arguments[index];
    started[count++] = "--launched";
    started[count] = NULL;
    setenv("DC2TK_OVERRIDES", overrides, 1);
    execv(binary, started);
    fprintf(stderr, "launcher: cannot start %s\n", binary);
}

static void take_picture(const char *path, const char *keys, float now) {
    SDL_Texture *canvas = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
                                            (int)(CANVAS_WIDTH * pixel_scale), (int)(CANVAS_HEIGHT * pixel_scale));
    for (const char *key = keys; key && *key; key++) {
        if (*key == 'u') press(KEY_UP, now);
        if (*key == 'd') press(KEY_DOWN, now);
        if (*key == 'l') press(KEY_LEFT, now);
        if (*key == 'r') press(KEY_RIGHT, now);
        if (*key == 'a') press(KEY_ACCEPT, now);
        if (*key == 'b') press(KEY_BACK, now);
        if (*key == 'p') press(KEY_PLAY, now);
    }
    for (int frame = 0; frame < 120; frame++) animate(1 / 60.0f);
    leaving_at = -1;
    SDL_SetRenderTarget(renderer, canvas);
    SDL_SetRenderLogicalPresentation(renderer, CANVAS_WIDTH, CANVAS_HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    draw(now);
    SDL_Surface *surface = SDL_RenderReadPixels(renderer, NULL);
    if (!surface || !SDL_SaveBMP(surface, path)) fprintf(stderr, "launcher: no picture: %s\n", SDL_GetError());
    SDL_DestroySurface(surface);
    SDL_SetRenderTarget(renderer, NULL);
    SDL_DestroyTexture(canvas);
}

int port_launcher(int count, char **values, int show) {
    const char *chosen = SDL_getenv("DC2TK_LAUNCHER_SETTINGS");
    char *folder = chosen ? NULL : SDL_GetPrefPath("DC2 Toolkit", "Dark Chronicle");
    if (chosen) snprintf(settings_path, sizeof settings_path, "%s", chosen);
    else snprintf(settings_path, sizeof settings_path, "%slauncher.cfg", folder ? folder : "");
    SDL_free(folder);
    port_settings_file(settings_path);
    if (!show) return 1;
    argument_count = count;
    arguments = values;
    read_options();
    find_sets(values[0]);
    read_sets();
    const char *picture = SDL_getenv("DC2TK_LAUNCHER_PICTURE");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "launcher: %s\n", SDL_GetError());
        return 0;
    }
    if (!open_window(picture != NULL)) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
        return 0;
    }
    char starting[PATH_SIZE];
    snprintf(starting, sizeof starting, "%s", disc_path);
    check_disc(starting);
    menu_selected = disc_state == DISC_GOOD ? MENU_PLAY : MENU_DISC;
    if (picture) {
        const char *time = SDL_getenv("DC2TK_LAUNCHER_TIME");
        take_picture(picture, SDL_getenv("DC2TK_LAUNCHER_KEYS"), time ? (float)atof(time) : 1);
    }
    Uint64 start = SDL_GetTicks(), previous = start;
    while (!picture && (leaving_at < 0 || (SDL_GetTicks() - start) / 1000.0f - leaving_at < 0.22f)) {
        Uint64 ticks = SDL_GetTicks();
        float now = (ticks - start) / 1000.0f, step = (ticks - previous) / 1000.0f;
        previous = ticks;
        SDL_Event event;
        while (SDL_PollEvent(&event)) handle(&event, now);
        if (SDL_GetAtomicInt(&picked)) {
            SDL_SetAtomicInt(&picked, 0);
            choosing = 0;
            if (picked_path[0]) check_disc(picked_path);
        }
        animate(step);
        draw(now);
        SDL_RenderPresent(renderer);
    }
    close_window();
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    if (!playing) return 0;
    save_options();
    start_game();
    return 0;
}
