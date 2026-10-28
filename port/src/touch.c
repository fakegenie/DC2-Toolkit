#include "touch.h"
#include "input.h"
#include "memory_card.h"
#include "port_state.h"
#include "render.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

enum { FINGER_LIMIT = 10, CONTROL_LIMIT = 20, SIDE_LEFT = 0, SIDE_RIGHT = 1 };
enum { CONTROL_BUTTON, CONTROL_SHOULDER, CONTROL_PILL, CONTROL_DPAD, CONTROL_LEFT_STICK, CONTROL_RIGHT_STICK,
       CONTROL_SAVE, CONTROL_LOAD };
enum { HELD_BUTTONS = -1 };
enum { IDLE_AFTER = 4000, IDLE_FADE = 800, REVEAL_TIME = 3000, NOTICE_TIME = 2000, FADE_TIME = 400 };

#define FONT_POINTS 12.0f
#define DIAGONAL 0.38f
#define DEAD_ZONE 0.1f
#define FULL_TILT 0.6f
#define GAME_DEAD_ZONE (50.0f / 128.0f)
#define IDLE_ALPHA 0.35f
#define ACTIVE_ALPHA 0.55f

typedef struct {
    int kind;
    float x;
    float y;
    float width;
    float height;
    uint16_t button;
    const char *label;
    int symbol;
    float red;
    float green;
    float blue;
} Control;

typedef struct {
    SDL_FingerID id;
    int held;
    float x;
    float y;
    float origin_x;
    float origin_y;
} Finger;

static int mode;
static int haptics = 1;
static Control controls[CONTROL_LIMIT];
static int control_count;
static int stick_control[2];
static Finger fingers[FINGER_LIMIT];
static int finger_count;
static float tap_x[FINGER_LIMIT];
static float tap_y[FINGER_LIMIT];
static int tap_count;
static uint16_t pressed;
static uint16_t pressed_before;
static int stick_held[2];
static float stick_x[2];
static float stick_y[2];
static float knob_x[2];
static float knob_y[2];
static Uint64 touched_at;
static float scale = 1;
static SDL_Rect safe;
static LauncherFont font;
static float font_density;
static TouchShape *out;
static int out_count;
static int out_limit;
static float density = 1;
static float fade = 1;
static int states = 1;
static PortState quick;
static int quick_kind;
static int quick_stored = -1;
static Uint64 revealed_at;
static Uint64 notice_at;
static char notice[96];


#if TARGET_OS_IPHONE
void port_ios_haptic(void);
static void buzz(void) { port_ios_haptic(); }
#elif defined(__ANDROID__)
static SDL_Haptic *open_haptic(void) {
    int count = 0;
    SDL_HapticID *found = SDL_InitSubSystem(SDL_INIT_HAPTIC) ? SDL_GetHaptics(&count) : NULL;
    SDL_Haptic *haptic = count ? SDL_OpenHaptic(found[0]) : NULL;
    SDL_free(found);
    if (haptic && !SDL_InitHapticRumble(haptic)) {
        SDL_CloseHaptic(haptic);
        haptic = NULL;
    }
    return haptic;
}

static void buzz(void) {
    static SDL_Haptic *haptic;
    static int opened;
    if (!opened) haptic = open_haptic();
    opened = 1;
    if (haptic) SDL_PlayHapticRumble(haptic, 0.4f, 12);
}
#else
static void buzz(void) {}
#endif

void port_touch_set_mode(int chosen) { mode = chosen; }

void port_touch_set_haptics(int on) { haptics = on; }

void port_touch_set_states(int on) { states = on; }


static int shown(void) { return mode == TOUCH_ON || (mode == TOUCH_SMART && !SDL_HasGamepad()); }

static void add(int kind, float x, float y, float width, float height, uint16_t button, const char *label, int symbol,
                float red, float green, float blue) {
    if (control_count >= CONTROL_LIMIT) return;
    if (kind == CONTROL_LEFT_STICK) stick_control[SIDE_LEFT] = control_count;
    if (kind == CONTROL_RIGHT_STICK) stick_control[SIDE_RIGHT] = control_count;
    controls[control_count++] = (Control){kind, x, y, width, height, button, label, symbol, red, green, blue};
}

static void layout(SDL_Window *window) {
    int width = 0, height = 0;
    SDL_GetWindowSize(window, &width, &height);
    safe = (SDL_Rect){0, 0, width, height};
    SDL_GetWindowSafeArea(window, &safe);
    float s = scale = SDL_clamp(safe.h / 390.0f, 1.0f, 1.4f);
    float left = safe.x + 24 * s, right = safe.x + safe.w - 24 * s;
    float top = safe.y + 12 * s, bottom = safe.y + safe.h - 12 * s, middle = safe.x + safe.w / 2.0f;
    float face_x = right - 78 * s, face_y = top + 142 * s, spread = 44 * s, radius = 23 * s;
    control_count = 0;
    add(CONTROL_SHOULDER, left + 32 * s, top + 18 * s, 64 * s, 32 * s, PORT_INPUT_L2, "L2", 0, 1, 1, 1);
    add(CONTROL_SHOULDER, left + 104 * s, top + 18 * s, 64 * s, 32 * s, PORT_INPUT_L1, "L1", 0, 1, 1, 1);
    add(CONTROL_SHOULDER, right - 104 * s, top + 18 * s, 64 * s, 32 * s, PORT_INPUT_R1, "R1", 0, 1, 1, 1);
    add(CONTROL_SHOULDER, right - 32 * s, top + 18 * s, 64 * s, 32 * s, PORT_INPUT_R2, "R2", 0, 1, 1, 1);
    add(CONTROL_SHOULDER, left + 32 * s, bottom - 18 * s, 64 * s, 32 * s, PORT_INPUT_L3, "L3", 0, 1, 1, 1);
    add(CONTROL_SHOULDER, right - 32 * s, bottom - 18 * s, 64 * s, 32 * s, PORT_INPUT_R3, "R3", 0, 1, 1, 1);
    add(CONTROL_DPAD, left + 78 * s, top + 142 * s, 60 * s, 0, 0, NULL, 0, 1, 1, 1);
    add(CONTROL_LEFT_STICK, left + 168 * s, bottom - 66 * s, 52 * s, 0, 0, NULL, 0, 1, 1, 1);
    add(CONTROL_RIGHT_STICK, right - 168 * s, bottom - 66 * s, 52 * s, 0, 0, NULL, 0, 1, 1, 1);
    add(CONTROL_BUTTON, face_x, face_y - spread, radius, 0, PORT_INPUT_TRIANGLE, NULL, TOUCH_TRIANGLE_LINE, 0.45f, 0.85f, 0.75f);
    add(CONTROL_BUTTON, face_x + spread, face_y, radius, 0, PORT_INPUT_CIRCLE, NULL, TOUCH_RING, 0.95f, 0.45f, 0.45f);
    add(CONTROL_BUTTON, face_x, face_y + spread, radius, 0, PORT_INPUT_CROSS, NULL, TOUCH_CROSS, 0.55f, 0.65f, 1);
    add(CONTROL_BUTTON, face_x - spread, face_y, radius, 0, PORT_INPUT_SQUARE, NULL, TOUCH_BOX_LINE, 0.95f, 0.6f, 0.85f);
    add(CONTROL_PILL, middle - 48 * s, bottom - 16 * s, 76 * s, 26 * s, PORT_INPUT_SELECT, "SELECT", 0, 1, 1, 1);
    add(CONTROL_PILL, middle + 48 * s, bottom - 16 * s, 76 * s, 26 * s, PORT_INPUT_START, "START", 0, 1, 1, 1);
    if (!states) return;
    add(CONTROL_SAVE, middle - 58 * s, top + 18 * s, 104 * s, 30 * s, 0, "SAVE STATE", 0, 1, 1, 1);
    add(CONTROL_LOAD, middle + 58 * s, top + 18 * s, 104 * s, 30 * s, 0, "LOAD STATE", 0, 1, 1, 1);
}

static int action(const Control *control) { return control->kind == CONTROL_SAVE || control->kind == CONTROL_LOAD; }

static int action_at(float x, float y) {
    float slack = 8 * scale;
    for (int index = 0; index < control_count; index++) {
        const Control *control = &controls[index];
        if (action(control) && fabsf(x - control->x) < control->width / 2 + slack &&
            fabsf(y - control->y) < control->height / 2 + slack)
            return control->kind;
    }
    return 0;
}

static void show_notice(const char *words) {
    snprintf(notice, sizeof notice, "%s", words);
    notice_at = SDL_GetTicks();
}

static void quick_path(char *path, size_t size) { snprintf(path, size, "%s/quick.dc2state", port_memory_card_root()); }

static int stored(void) {
    char path[1100];
    quick_path(path, sizeof path);
    if (quick_stored < 0) quick_stored = access(path, F_OK) == 0;
    return quick.data || quick_stored;
}

static void run_action(int kind) {
    if (quick_kind) return;
    char path[1100];
    quick_path(path, sizeof path);
    if (kind == CONTROL_LOAD && !stored()) {
        show_notice("No state saved yet");
        return;
    }
    if (kind == CONTROL_LOAD && !quick.data && port_state_read(&quick, path)) {
        show_notice("The saved state can't be loaded");
        return;
    }
    int wanted = kind == CONTROL_SAVE ? STATE_SAVE : STATE_LOAD;
    if (!port_state_request(wanted, &quick)) {
        show_notice(port_state_problem());
        return;
    }
    quick_kind = wanted;
    buzz();
}

static const char *finish_message(void) {
    const char *problem = port_state_problem();
    if (problem && strstr(problem, "movie")) return "Can't save during a movie";
    if (problem && strstr(problem, "another build")) return "This state is from an older version";
    if (problem) return quick_kind == STATE_SAVE ? "The state couldn't be saved" : "The state couldn't be loaded";
    if (quick_kind == STATE_LOAD) return "State loaded";
    char path[1100];
    quick_path(path, sizeof path);
    if (port_state_write(&quick, path)) return "The state couldn't be written to the saves folder";
    quick_stored = 1;
    return "State saved";
}

static void finish_action(void) {
    if (!quick_kind || port_state_pending()) return;
    show_notice(finish_message());
    quick_kind = 0;
}

static int revealed(void) { return SDL_GetTicks() - revealed_at < REVEAL_TIME; }

static uint16_t dpad_at(const Control *control, float x, float y) {
    float dx = x - control->x, dy = y - control->y, distance = hypotf(dx, dy);
    if (distance > control->width * 1.4f || distance < control->width * 0.2f) return 0;
    uint16_t found = 0;
    if (dx > distance * DIAGONAL) found |= PORT_INPUT_RIGHT;
    if (dx < -distance * DIAGONAL) found |= PORT_INPUT_LEFT;
    if (dy > distance * DIAGONAL) found |= PORT_INPUT_DOWN;
    if (dy < -distance * DIAGONAL) found |= PORT_INPUT_UP;
    return found;
}

static uint16_t buttons_at(float x, float y) {
    uint16_t found = 0;
    float slack = 8 * scale;
    for (int index = 0; index < control_count; index++) {
        const Control *control = &controls[index];
        if (control->kind == CONTROL_BUTTON && hypotf(x - control->x, y - control->y) < control->width * 1.25f)
            found |= control->button;
        if ((control->kind == CONTROL_SHOULDER || control->kind == CONTROL_PILL) &&
            fabsf(x - control->x) < control->width / 2 + slack && fabsf(y - control->y) < control->height / 2 + slack)
            found |= control->button;
        if (control->kind == CONTROL_DPAD) found |= dpad_at(control, x, y);
    }
    return found;
}

static Finger landed(SDL_FingerID id, float x, float y) {
    Finger finger = {id, HELD_BUTTONS, x, y, x, y};
    if (buttons_at(x, y) || action_at(x, y)) return finger;
    int side = x < safe.x + safe.w / 2.0f ? SIDE_LEFT : SIDE_RIGHT;
    float reach = controls[stick_control[side]].width;
    finger.held = stick_control[side];
    finger.origin_x = SDL_clamp(x, safe.x + reach, safe.x + safe.w - reach);
    finger.origin_y = SDL_clamp(y, safe.y + reach, safe.y + safe.h - reach);
    return finger;
}

static Finger follow(SDL_FingerID id, float x, float y) {
    for (int index = 0; index < finger_count; index++)
        if (fingers[index].id == id) {
            Finger finger = fingers[index];
            finger.x = x;
            finger.y = y;
            return finger;
        }
    return landed(id, x, y);
}

static int stick_axis(float part, float tilt) {
    if (part == 0) return 0;
    float reach = GAME_DEAD_ZONE + (1 - GAME_DEAD_ZONE) * fabsf(part) * tilt;
    return (int)copysignf(reach * 32767, part);
}

static void steer(const Finger *finger, struct port_input_state *state) {
    const Control *control = &controls[finger->held];
    int side = control->kind == CONTROL_RIGHT_STICK;
    float dx = finger->x - finger->origin_x, dy = finger->y - finger->origin_y, length = hypotf(dx, dy);
    if (length > control->width) {
        dx *= control->width / length;
        dy *= control->width / length;
    }
    stick_held[side] = 1;
    stick_x[side] = finger->origin_x;
    stick_y[side] = finger->origin_y;
    knob_x[side] = dx;
    knob_y[side] = dy;
    if (length < control->width * DEAD_ZONE) return;
    float tilt = SDL_min(1, (length / control->width - DEAD_ZONE) / (FULL_TILT - DEAD_ZONE));
    float side_length = SDL_max(fabsf(dx), fabsf(dy));
    int axis_x = stick_axis(dx / side_length, tilt), axis_y = stick_axis(dy / side_length, tilt);
    if (side) {
        state->right_x = axis_x;
        state->right_y = axis_y;
    } else {
        state->left_x = axis_x;
        state->left_y = axis_y;
    }
}

void port_touch_event(const SDL_Event *event) {
    if (event->type == SDL_EVENT_FINGER_DOWN && tap_count < FINGER_LIMIT) {
        tap_x[tap_count] = event->tfinger.x;
        tap_y[tap_count++] = event->tfinger.y;
    }
}

static void update_fade(void) {
    Uint64 now = SDL_GetTicks();
    if (finger_count || tap_count || !touched_at) touched_at = now;
    float idle = (float)(now - touched_at) - IDLE_AFTER;
    fade = idle <= 0 ? ACTIVE_ALPHA : SDL_max(IDLE_ALPHA, ACTIVE_ALPHA - (ACTIVE_ALPHA - IDLE_ALPHA) * idle / IDLE_FADE);
}

void port_touch_read(struct port_input_state *state) {
    pressed = 0;
    memset(stick_held, 0, sizeof stick_held);
    memset(knob_x, 0, sizeof knob_x);
    memset(knob_y, 0, sizeof knob_y);
    SDL_Window *window = render_window();
    finish_action();
    if (!window) return;
    layout(window);
    int width = 0, height = 0, devices = 0;
    SDL_GetWindowSize(window, &width, &height);
    if (!shown()) {
        for (int index = 0; index < tap_count; index++) {
            int kind = action_at(tap_x[index] * width, tap_y[index] * height);
            if (kind && revealed()) run_action(kind);
            if (states) revealed_at = SDL_GetTicks();
        }
        tap_count = 0;
        finger_count = 0;
        return;
    }
    Finger seen[FINGER_LIMIT];
    int seen_count = 0;
    SDL_TouchID *touches = SDL_GetTouchDevices(&devices);
    for (int device = 0; touches && device < devices; device++) {
        int count = 0;
        SDL_Finger **list = SDL_GetTouchFingers(touches[device], &count);
        for (int index = 0; list && index < count && seen_count < FINGER_LIMIT; index++)
            seen[seen_count++] = follow(list[index]->id, list[index]->x * width, list[index]->y * height);
        SDL_free(list);
    }
    SDL_free(touches);
    memcpy(fingers, seen, sizeof seen);
    finger_count = seen_count;
    for (int index = 0; index < finger_count; index++) {
        const Finger *finger = &fingers[index];
        if (finger->held == HELD_BUTTONS) pressed |= buttons_at(finger->x, finger->y);
        else steer(finger, state);
    }
    for (int index = 0; index < tap_count; index++) {
        pressed |= buttons_at(tap_x[index] * width, tap_y[index] * height);
        int kind = action_at(tap_x[index] * width, tap_y[index] * height);
        if (kind) run_action(kind);
    }
    update_fade();
    tap_count = 0;
    if (haptics && (pressed & ~pressed_before)) buzz();
    pressed_before = pressed;
    state->buttons |= pressed;
}

const LauncherPicture *port_touch_atlas(void) {
    float wanted = density;
    if (font_density != wanted) {
        free(font.atlas.pixels);
        font.atlas.pixels = NULL;
        font_density = launcher_font_bake(LAUNCHER_SEMIBOLD, FONT_POINTS * wanted, &font) ? wanted : 0;
    }
    return font_density ? &font.atlas : NULL;
}

static void put(int kind, float x, float y, float half_width, float half_height, float stroke, float radius, float angle,
                float red, float green, float blue, float alpha) {
    if (out_count >= out_limit) return;
    out[out_count++] = (TouchShape){x * density, y * density, half_width * density, half_height * density, angle, red,
                                    green, blue, alpha * fade, stroke * density, radius * density, kind, 0, 0, 0, 0};
}

static void glow(float x, float y, float radius, float red, float green, float blue) {
    put(TOUCH_GLOW, x, y, radius, radius, 0, 0, 0, red, green, blue, 0.7f);
}

static void label(const char *words, float x, float y, float alpha) {
    if (!font_density) return;
    float glyph_scale = 1 / font_density, width = 0, atlas = (float)font.atlas.width;
    for (const char *at = words; *at; at++) width += font.glyphs[*at - LAUNCHER_FIRST].advance * glyph_scale;
    float pen = x - width / 2, baseline = y + FONT_POINTS * 0.36f;
    for (const char *at = words; *at && out_count < out_limit; at++) {
        const LauncherGlyph *glyph = &font.glyphs[*at - LAUNCHER_FIRST];
        float glyph_width = (glyph->x1 - glyph->x0) * glyph_scale, glyph_height = (glyph->y1 - glyph->y0) * glyph_scale;
        float left = pen + glyph->left * glyph_scale, top = baseline + glyph->top * glyph_scale;
        put(TOUCH_TEXT, left + glyph_width / 2, top + glyph_height / 2, glyph_width / 2, glyph_height / 2, 0, 0, 0, 1, 1,
            1, alpha);
        out[out_count - 1].u0 = glyph->x0 / atlas;
        out[out_count - 1].v0 = glyph->y0 / atlas;
        out[out_count - 1].u1 = glyph->x1 / atlas;
        out[out_count - 1].v1 = glyph->y1 / atlas;
        pen += glyph->advance * glyph_scale;
    }
}

static void plate(int kind, float x, float y, float half_width, float half_height, float radius, int down,
                  const Control *tint) {
    float grow = down ? 1.08f : 1;
    if (down) glow(x, y, SDL_max(half_width, half_height) * 1.9f, tint->red, tint->green, tint->blue);
    float shade = down ? 0.45f : 0;
    put(kind, x, y, half_width * grow, half_height * grow, 0, radius * grow, 0, 0.08f + tint->red * shade,
        0.08f + tint->green * shade, 0.1f + tint->blue * shade, down ? 0.6f : 0.32f);
    put(kind == TOUCH_DISC ? TOUCH_RING : TOUCH_BOX_LINE, x, y, half_width * grow, half_height * grow, 1.5f * scale,
        radius * grow, 0, 1, 1, 1, down ? 0.9f : 0.35f);
}

static void draw_symbol(const Control *control, int down) {
    float radius = control->width * (down ? 1.08f : 1), stroke = 2.4f * scale;
    float half = control->symbol == TOUCH_TRIANGLE_LINE ? radius * 0.42f
                 : control->symbol == TOUCH_RING       ? radius * 0.4f
                 : control->symbol == TOUCH_CROSS      ? radius * 0.38f
                                                       : radius * 0.34f;
    put(control->symbol, control->x, control->y, half, half, stroke, 2 * scale, 0, control->red, control->green,
        control->blue, down ? 1 : 0.85f);
}

static void draw_dpad(const Control *control) {
    static const uint16_t directions[4] = {PORT_INPUT_UP, PORT_INPUT_RIGHT, PORT_INPUT_DOWN, PORT_INPUT_LEFT};
    float reach = control->width, gap = reach * 0.1f, length = reach * 0.86f, half = reach * 0.32f;
    for (int turn = 0; turn < 4; turn++) {
        float angle = turn * SDL_PI_F / 2, out_x = sinf(angle), out_y = -cosf(angle);
        float middle = gap + length / 2, mark = gap + length * 0.62f;
        float x = control->x + out_x * middle, y = control->y + out_y * middle;
        int down = (pressed & directions[turn]) != 0;
        float shade = down ? 0.4f : 0;
        if (down) glow(x, y, length * 0.9f, 1, 1, 1);
        put(TOUCH_DPAD_ARM, x, y, half, length / 2, 0, 3 * scale, angle, 0.09f + shade, 0.09f + shade, 0.11f + shade,
            down ? 0.75f : 0.45f);
        put(TOUCH_DPAD_ARM, x, y, half, length / 2, 1.5f * scale, 3 * scale, angle, 1, 1, 1, down ? 0.9f : 0.35f);
        put(TOUCH_ARROW, control->x + out_x * mark, control->y + out_y * mark, half * 0.42f, half * 0.42f, 0, 0, angle,
            1, 1, 1, down ? 1 : 0.6f);
    }
}

static void draw_cap(float x, float y, float knob, float alpha) {
    put(TOUCH_DISC, x, y, knob, knob, 0, 0, 0, 0.16f, 0.16f, 0.18f, alpha);
    put(TOUCH_RING, x, y, knob, knob, 1.5f * scale, 0, 0, 1, 1, 1, alpha * 0.9f);
    put(TOUCH_DISC, x, y, knob * 0.68f, knob * 0.68f, 0, 0, 0, 0.05f, 0.05f, 0.06f, alpha * 0.8f);
    put(TOUCH_RING, x, y, knob * 0.68f, knob * 0.68f, 1.2f * scale, 0, 0, 1, 1, 1, alpha * 0.35f);
}

static void draw_stick(const Control *control) {
    int side = control->kind == CONTROL_RIGHT_STICK, held = stick_held[side];
    float x = held ? stick_x[side] : control->x, y = held ? stick_y[side] : control->y, knob = control->width * 0.55f;
    put(TOUCH_DISC, x, y, control->width, control->width, 0, 0, 0, 0.02f, 0.02f, 0.03f, held ? 0.4f : 0.22f);
    put(TOUCH_RING, x, y, control->width, control->width, 1.5f * scale, 0, 0, 1, 1, 1, held ? 0.45f : 0.2f);
    if (held) glow(x + knob_x[side], y + knob_y[side], knob * 1.7f, 1, 1, 1);
    draw_cap(x + knob_x[side], y + knob_y[side], knob, held ? 0.95f : 0.55f);
}

static float remaining(Uint64 since, int length) {
    Uint64 passed = SDL_GetTicks() - since;
    if (!since || passed >= (Uint64)length) return 0;
    return SDL_min(1.0f, (float)(length - passed) / FADE_TIME);
}

static void draw_action(const Control *control) {
    int busy = (control->kind == CONTROL_SAVE && quick_kind == STATE_SAVE) ||
               (control->kind == CONTROL_LOAD && quick_kind == STATE_LOAD);
    int empty = control->kind == CONTROL_LOAD && !stored();
    plate(TOUCH_BOX, control->x, control->y, control->width / 2, control->height / 2, control->height / 2, busy, control);
    label(control->label, control->x, control->y, empty ? 0.45f : busy ? 1 : 0.85f);
}

static void draw_notice(void) {
    float strength = remaining(notice_at, NOTICE_TIME);
    if (strength <= 0) return;
    float keep = fade;
    fade = strength;
    float width = 0;
    for (const char *at = notice; *at; at++)
        if (*at >= LAUNCHER_FIRST && *at - LAUNCHER_FIRST < LAUNCHER_GLYPHS) width += font.glyphs[*at - LAUNCHER_FIRST].advance;
    width = width / (font_density ? font_density : 1) + 28 * scale;
    float x = safe.x + safe.w / 2.0f, y = safe.y + 62 * scale;
    put(TOUCH_BOX, x, y, width / 2, 15 * scale, 0, 15 * scale, 0, 0.06f, 0.06f, 0.08f, 0.8f);
    label(notice, x, y, 1);
    fade = keep;
}

int port_touch_shapes(TouchShape *shapes, int limit, int pixel_width, int pixel_height) {
    SDL_Window *window = render_window();
    (void)pixel_height;
    if (!window) return 0;
    float reveal = shown() ? 1 : remaining(revealed_at, REVEAL_TIME);
    if (reveal <= 0 && remaining(notice_at, NOTICE_TIME) <= 0) return 0;
    int width = 0, height = 0;
    SDL_GetWindowSize(window, &width, &height);
    density = width > 0 ? (float)pixel_width / width : 1;
    if (!control_count) layout(window);
    port_touch_atlas();
    out = shapes;
    out_count = 0;
    out_limit = limit;
    float idle = fade;
    fade = shown() ? idle : reveal;
    for (int index = 0; index < control_count && reveal > 0; index++) {
        const Control *control = &controls[index];
        int down = (pressed & control->button) != 0;
        if (action(control)) {
            draw_action(control);
            continue;
        }
        if (!shown()) continue;
        if (control->kind == CONTROL_BUTTON) {
            plate(TOUCH_DISC, control->x, control->y, control->width, control->width, 0, down, control);
            draw_symbol(control, down);
        }
        if (control->kind == CONTROL_SHOULDER || control->kind == CONTROL_PILL) {
            float radius = control->kind == CONTROL_PILL ? control->height / 2 : 10 * scale;
            plate(TOUCH_BOX, control->x, control->y, control->width / 2, control->height / 2, radius, down, control);
            label(control->label, control->x, control->y, down ? 1 : 0.85f);
        }
        if (control->kind == CONTROL_DPAD) draw_dpad(control);
        if (control->kind == CONTROL_LEFT_STICK || control->kind == CONTROL_RIGHT_STICK) draw_stick(control);
    }
    fade = idle;
    draw_notice();
    return out_count;
}
