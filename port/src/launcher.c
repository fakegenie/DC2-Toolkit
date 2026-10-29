#include "launcher.h"
#include "cdvd.h"
#include "executable.h"
#include "icon.h"
#include "port_mod.h"
#include "port_overrides.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if TARGET_OS_IPHONE
#include "ios_files.h"
enum { DESKTOP = 0, SAVES_ITEM = 1 };
#define WINDOW_FLAGS (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY)
#define MODS_HINT_TOP "Put built mods (.dc2pak files) in the mods folder:"
#define MODS_HINT_BOTTOM "Files, On My iPhone, DC2, mods."
#define MODS_ACTION "Open mods folder"
#elif defined(__ANDROID__)
#include "android_files.h"
enum { DESKTOP = 0, SAVES_ITEM = 0 };
#define WINDOW_FLAGS (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY)
#define MODS_HINT_TOP "Add built mods (.dc2pak files) from your files,"
#define MODS_HINT_BOTTOM "or copy them to Android/data/.../files/mods."
#define MODS_ACTION "Add a mod"
#else
enum { DESKTOP = 1, SAVES_ITEM = 0 };
#define WINDOW_FLAGS (SDL_WINDOW_BORDERLESS | SDL_WINDOW_TRANSPARENT | SDL_WINDOW_HIGH_PIXEL_DENSITY)
#define MODS_HINT_TOP "Built mods (.dc2pak files) go in the mods folder"
#define MODS_HINT_BOTTOM "next to the game. Drop one here to add it."
#define MODS_ACTION "Open mods folder"
#endif

enum { CANVAS_WIDTH = 960, CANVAS_HEIGHT = 700, HERO_WIDTH = 520, PANEL_LEFT = 520, PANEL_WIDTH = 440, PAD = 48 };
enum { OPTION_CHOICES = 14, OPTION_LIMIT = 16, PATH_SIZE = 1024, LINE_SIZE = 512 };
enum { GLOW_SIZE = 64, WINDOW_RADIUS = 14 };
enum { MENU_TOP = 225, MENU_STEP = 50 };
enum { LARGEST_HEIGHT = 840, DRAG_START = 10, REVEAL_ABOVE = 60, REVEAL_BELOW = 50 };
#define BODY_POINTS 20.0f
enum { ROWS_TOP = 96, ROW_HEIGHT = 30, HEADING_HEIGHT = 28, TOGGLE_WIDTH = 34, TOGGLE_HEIGHT = 20 };
enum { PAGE_MAIN, PAGE_SETTINGS, PAGE_MODS, PAGE_COUNT };
enum { MOD_LIMIT = 32, MOD_ROWS = 7, MOD_TOP = 96, MOD_HEIGHT = 50, ENABLED_SIZE = 2048 };
enum { DISC_NONE, DISC_GOOD, DISC_WRONG };
enum { MENU_PLAY, MENU_MODS, MENU_SETTINGS, MENU_DISC, MENU_LAST, MENU_COUNT };
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
    int face;
    float size;
    float tracking;
} Style;

typedef struct {
    float radius;
    float stroke;
    int inverted;
} ShapeKind;

#if TARGET_OS_IPHONE || defined(__ANDROID__)
static Option options[] = {
    {"DISPLAY", "Aspect ratio", "render.aspect", 5, {"4:3", "16:10", "16:9", "21:9", "Fill screen"},
     {"4:3", "16:10", "16:9", "21:9", "fill"}, 0, 0, 0, 1},
    {NULL, "Render scale", "render.scale", 5, {"Match screen", "1×", "2×", "3×", "4×"}, {"auto", "1", "2", "3", "4"}, 1,
     0, 0, 0},
    {NULL, "Anti-aliasing", "render.msaa", 4, {"Off", "2×", "4×", "8×"}, {"1", "2", "4", "8"}, 0, 0, 0, 0},
    {NULL, "Texture filtering", "render.filter", 2, {"Smooth", "Sharp"}, {"smooth", "nearest"}, 0, 0, 0, 0},
    {"MOTION", "High frame rate", "render.smooth", 4, {"Off", "Match display", "60 Hz", "120 Hz"},
     {"false", "display", "60", "120"}, 0, 0, 0, 1},
    {"CONTROLS", "On-screen controls", "input.touch", 3, {"Off", "Smart", "On"}, {"false", "smart", "true"}, 1, 0, 0, 0},
    {NULL, "Haptics", "input.haptics", 2, {"Off", "On"}, {"false", "true"}, 1, 0, 1, 0},
    {NULL, "Save state buttons", "input.states", 2, {"Off", "On"}, {"false", "true"}, 1, 0, 1, 0},
    {"GAME", "Sound", "sound.mute", 2, {"Off", "On"}, {"true", "false"}, 1, 0, 1, 0},
    {NULL, "In-Game Debug Menu", "debug.menus", 2, {"Off", "On"}, {"false", "true"}, 0, 0, 1, 0},
    {"STARTUP", "Language", "boot.language", 6, {"Ask", "English", "Français", "Deutsch", "Italiano", "Español"},
     {"ask", "1", "2", "3", "4", "5"}, 0, 0, 0, 0},
    {NULL, "Skip intro", "boot.skip_intro", 2, {"Off", "On"}, {"false", "true"}, 0, 0, 1, 0},
    {NULL, "Load save", "boot.save", 14,
     {"Off", "Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5", "Slot 6", "Slot 7", "Slot 8", "Slot 9", "Slot 10",
      "Slot 11", "Slot 12", "Slot 13"},
     {"off", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13"}, 0, 0, 0, 0},
};
#else
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
    {"STARTUP", "Language", "boot.language", 6, {"Ask", "English", "Français", "Deutsch", "Italiano", "Español"},
     {"ask", "1", "2", "3", "4", "5"}, 0, 0, 0, 0},
    {NULL, "Skip intro", "boot.skip_intro", 2, {"Off", "On"}, {"false", "true"}, 0, 0, 1, 0},
    {NULL, "Load save", "boot.save", 14,
     {"Off", "Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5", "Slot 6", "Slot 7", "Slot 8", "Slot 9", "Slot 10",
      "Slot 11", "Slot 12", "Slot 13"},
     {"off", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13"}, 0, 0, 0, 0},
};
#endif

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
static SDL_Texture *canvas;
static float view_width = CANVAS_WIDTH;
static float view_height = CANVAS_HEIGHT;
static float view_left;
static float scroll;
static float scroll_goal;
static int scrolled_page = -1;
static float press_x = -1;
static float press_y;
static float press_scroll;
static int dragging;
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
static char disc_name[PATH_SIZE];
static char settings_path[PATH_SIZE];
static char picked_path[PATH_SIZE];
static SDL_AtomicInt picked;
static int choosing;
static float pointer_x = -1;
static float pointer_y = -1;
static float page_mix[PAGE_COUNT] = {1};
static float bar_middle = MENU_TOP + MENU_STEP / 2.0f;
static float item_shift[MENU_COUNT];
static char saves_name[LINE_SIZE];
static float row_middle = ROWS_TOP;
static float toggle_mix[OPTION_LIMIT];
static float row_focus[OPTION_LIMIT];
static float badge_mix[OPTION_LIMIT];
static int mod_count;
static int mod_on[MOD_LIMIT];
static int mod_selected;
static int mod_first;
static float mod_middle = MOD_TOP;
static float mod_focus[MOD_LIMIT + 1];
static float mod_mark[MOD_LIMIT];

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

static void file_name(const char *path, char *name, size_t size) {
#ifdef __ANDROID__
    if (!strncmp(path, "content://", 10)) {
        port_android_name(path, name, size);
        return;
    }
#endif
    snprintf(name, size, "%s", base_name(path));
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
#if TARGET_OS_IPHONE
    port_ios_disc(disc_path, sizeof disc_path);
    port_ios_saves_name(saves_name, sizeof saves_name);
#endif
}

static void save_options(void) {
    FILE *file = fopen(settings_path, "w");
    for (int index = 0; index < (int)SDL_arraysize(options); index++) {
        const Option *option = &options[index];
        port_setting_set(option->setting, option->values[option->chosen]);
        if (file) fprintf(file, "%s=%s\n", option->setting, option->values[option->chosen]);
    }
    char enabled[ENABLED_SIZE] = "";
    for (int index = 0; index < mod_count; index++) {
        size_t used = strlen(enabled);
        if (mod_on[index]) snprintf(enabled + used, sizeof enabled - used, "%s%s", used ? "," : "", port_pak_text(index, "id"));
    }
    port_setting_set("disc", disc_path);
    port_setting_set("mods.enabled", enabled);
    if (file) fprintf(file, "disc=%s\nmods.enabled=%s\n", disc_path, enabled);
    if (file) fclose(file);
    else fprintf(stderr, "launcher: cannot save %s\n", settings_path);
}

static void check_disc(const char *path) {
    struct sceCdlFILE boot;
    snprintf(disc_path, sizeof disc_path, "%s", path);
    file_name(disc_path, disc_name, sizeof disc_name);
    disc_state = DISC_NONE;
    if (!*disc_path) return;
    disc_state = port_cdvd_init(disc_path) && port_cdvd_find(PORT_EXECUTABLE_PATH, &boot) ? DISC_GOOD : DISC_WRONG;
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
    return SAVES_ITEM ? "Saves" : "Quit";
}

static SDL_FRect menu_box(int item) {
    return (SDL_FRect){PANEL_LEFT + PAD - 20, MENU_TOP + item * (float)MENU_STEP, PANEL_WIDTH - 2 * PAD + 40, MENU_STEP};
}

static SDL_FRect mod_box(int row) {
    return (SDL_FRect){PANEL_LEFT + PAD - 14, MOD_TOP + (row - mod_first) * (float)MOD_HEIGHT, PANEL_WIDTH - 2 * PAD + 28,
                       MOD_HEIGHT - 4};
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
    int on = 0;
    for (int index = 0; index < mod_count; index++) on += mod_on[index];
    char mods_value[32];
    snprintf(mods_value, sizeof mods_value, on ? "%d on" : "None", on);
    draw_value(MENU_MODS, mods_value, ink, alpha, shift);
    draw_value(MENU_DISC, disc_state == DISC_NONE ? "None" : disc_name, disc_state == DISC_WRONG ? bad : ink,
               alpha, shift);
    if (SAVES_ITEM) draw_value(MENU_LAST, saves_name, ink, alpha, shift);
}

static void draw_switch(float mix, float right, float middle, float alpha, float focus) {
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

static int mod_clash(int index, char *why, size_t size) {
    char reason[LINE_SIZE];
    for (int other = 0; other < mod_count; other++)
        if (mod_on[other] && other != index && port_pak_clash(index, other, reason, sizeof reason)) {
            snprintf(why, size, "Clashes with %s: %s", port_pak_text(other, "name"), reason);
            return 1;
        }
    return 0;
}

static void mod_detail(int index, char *out, size_t size) {
    const char *description = port_pak_text(index, "description");
    const char *authors = port_pak_text(index, "authors");
    if (*description) snprintf(out, size, "%s", description);
    else if (*authors) snprintf(out, size, "By %s", authors);
    else snprintf(out, size, port_pak_files(index) == 1 ? "%d game file" : "%d game files", port_pak_files(index));
}

static void draw_mod_row(int row, float alpha, float shift) {
    SDL_FRect box = mod_box(row);
    float middle = box.y + box.h / 2, x = PANEL_LEFT + PAD + shift, right = control_right() + shift;
    float focus = mod_focus[row];
    if (row == mod_count) {
        text_middle(STYLE_BODY, MODS_ACTION, x, middle, faded(accent, (0.75f + 0.25f * focus) * alpha));
        return;
    }
    char title[LINE_SIZE], detail[LINE_SIZE], line[LINE_SIZE];
    snprintf(title, sizeof title, "%s  %s", port_pak_text(row, "name"), port_pak_text(row, "version"));
    int blocked = !mod_on[row] && mod_clash(row, detail, sizeof detail);
    if (!blocked) mod_detail(row, detail, sizeof detail);
    shorten(STYLE_BODY, title, right - x - TOGGLE_WIDTH - 16, line, sizeof line);
    text_middle(STYLE_BODY, line, x, middle - 9, faded(ink, (blocked ? 0.45f : 0.72f + 0.28f * focus) * alpha));
    shorten(STYLE_SMALL, detail, right - x - TOGGLE_WIDTH - 16, line, sizeof line);
    text_middle(STYLE_SMALL, line, x, middle + 11, faded(blocked ? bad : ink, (blocked ? 0.9f : 0.45f) * alpha));
    draw_switch(mod_mark[row], right, middle, alpha * (blocked ? 0.4f : 1), focus);
}

static void draw_mods(void) {
    float alpha = page_mix[PAGE_MODS], shift = 18 * (1 - alpha);
    if (alpha <= 0.01f) return;
    draw_header("Mods", alpha, shift);
    SDL_FRect highlight = mod_box(mod_first);
    highlight.y = mod_middle - highlight.h / 2;
    highlight.x += shift;
    rounded(SHAPE_ROW, highlight, 8, faded(ink, 0.065f * alpha));
    for (int row = mod_first; row <= mod_count && row < mod_first + MOD_ROWS; row++) draw_mod_row(row, alpha, shift);
    if (mod_count) return;
    float x = PANEL_LEFT + PAD + shift, top = MOD_TOP + MOD_HEIGHT + 6;
    text_top(STYLE_SMALL, MODS_HINT_TOP, x, top, faded(ink, 0.5f * alpha));
    text_top(STYLE_SMALL, MODS_HINT_BOTTOM, x, top + 18, faded(ink, 0.5f * alpha));
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
        if (option->toggle) draw_switch(toggle_mix[index], right, middle, alpha, focus);
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

static void draw_canvas(float now) {
    SDL_SetRenderDrawColorFloat(renderer, panel.r, panel.g, panel.b, DESKTOP ? 0 : 1);
    SDL_RenderClear(renderer);
    fill((SDL_FRect){0, 0, CANVAS_WIDTH, CANVAS_HEIGHT}, panel);
    draw_hero(now);
    fill((SDL_FRect){HERO_WIDTH, 0, 1 / pixel_scale, CANVAS_HEIGHT}, faded(ink, 0.07f));
    gradient((SDL_FRect){HERO_WIDTH, 0, 48, CANVAS_HEIGHT}, (SDL_FColor){0, 0, 0, 0.22f}, (SDL_FColor){0, 0, 0, 0}, 1);
    draw_main(now);
    draw_settings();
    draw_mods();
    if (DESKTOP) draw_close();
    float fade = 1 - ease_out(now / 0.6f);
    if (leaving_at >= 0) fade = fmaxf(fade, ease_out((now - leaving_at) / 0.22f));
    if (fade > 0) fill((SDL_FRect){0, 0, CANVAS_WIDTH, CANVAS_HEIGHT}, faded(panel, fade));
    if (DESKTOP)
        SDL_RenderTexture9Grid(renderer, shape_textures[SHAPE_WINDOW], NULL, ceilf(WINDOW_RADIUS * pixel_scale),
                           ceilf(WINDOW_RADIUS * pixel_scale), ceilf(WINDOW_RADIUS * pixel_scale),
                           ceilf(WINDOW_RADIUS * pixel_scale), WINDOW_RADIUS / ceilf(WINDOW_RADIUS * pixel_scale),
                           &(SDL_FRect){0, 0, CANVAS_WIDTH, CANVAS_HEIGHT});
}

static void draw(float now) {
    if (DESKTOP) {
        draw_canvas(now);
        return;
    }
    SDL_SetRenderTarget(renderer, canvas);
    SDL_SetRenderLogicalPresentation(renderer, CANVAS_WIDTH, CANVAS_HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    draw_canvas(now);
    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetRenderLogicalPresentation(renderer, (int)view_width, (int)view_height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderDrawColorFloat(renderer, panel.r, panel.g, panel.b, 1);
    SDL_RenderClear(renderer);
    float hero_width = SDL_max(0, HERO_WIDTH - view_left), hero_top = (CANVAS_HEIGHT - view_height) / 2;
    SDL_FRect hero = {view_left * pixel_scale, hero_top * pixel_scale, hero_width * pixel_scale, view_height * pixel_scale};
    SDL_FRect side = {(CANVAS_WIDTH - (view_width - hero_width)) * pixel_scale, scroll * pixel_scale,
                      (view_width - hero_width) * pixel_scale, view_height * pixel_scale};
    SDL_RenderTexture(renderer, canvas, &hero, &(SDL_FRect){0, 0, hero_width, view_height});
    SDL_RenderTexture(renderer, canvas, &side, &(SDL_FRect){hero_width, 0, view_width - hero_width, view_height});
}

static float page_top(void) { return page == PAGE_MAIN ? MENU_TOP - 20 : 0; }

static float page_bottom(void) {
    if (page == PAGE_SETTINGS) return row_top(shown_count) + ROW_HEIGHT + 24;
    if (page == PAGE_MODS) return mod_box(SDL_min(mod_count, mod_first + MOD_ROWS - 1)).y + MOD_HEIGHT + 60;
    return MENU_TOP + MENU_COUNT * MENU_STEP + 20;
}

static int overflows(void) { return page_bottom() - page_top() > view_height; }

static void reveal(float middle) {
    if (middle - REVEAL_ABOVE < scroll_goal) scroll_goal = middle - REVEAL_ABOVE;
    if (middle + REVEAL_BELOW > scroll_goal + view_height) scroll_goal = middle + REVEAL_BELOW - view_height;
}

static void follow_page(float step) {
    float top = page_top(), bottom = SDL_min(page_bottom(), (float)CANVAS_HEIGHT);
    int changed = page != scrolled_page;
    scrolled_page = page;
    if (changed) scroll_goal = top;
    if (overflows()) scroll_goal = SDL_clamp(scroll_goal, top, bottom - view_height);
    else scroll_goal = SDL_clamp((top + bottom - view_height) / 2, 0, CANVAS_HEIGHT - view_height);
    if (changed || dragging > 0) scroll = scroll_goal;
    else approach(&scroll, scroll_goal, 18, step);
}

static void animate(float step) {
    follow_page(step);
    for (int which = 0; which < PAGE_COUNT; which++) approach(&page_mix[which], page == which, 12, step);
    approach(&mod_middle, mod_box(mod_selected).y + mod_box(mod_selected).h / 2, 20, step);
    for (int row = 0; row <= mod_count; row++) approach(&mod_focus[row], row == mod_selected, 18, step);
    for (int row = 0; row < mod_count; row++) approach(&mod_mark[row], (float)mod_on[row], 16, step);
    approach(&bar_middle, MENU_TOP + menu_selected * (float)MENU_STEP + MENU_STEP / 2.0f, 16, step);
    for (int item = 0; item < MENU_COUNT; item++) approach(&item_shift[item], item == menu_selected, 14, step);
    approach(&row_middle, row_top(row_selected) + ROW_HEIGHT / 2.0f, 20, step);
    for (int index = 0; index < (int)SDL_arraysize(options); index++)
        approach(&toggle_mix[index], (float)options[index].chosen, 16, step);
    for (int row = 0; row <= shown_count; row++) approach(&row_focus[row], row == row_selected, 18, step);
    for (int index = 0; index < (int)SDL_arraysize(options); index++)
        approach(&badge_mix[index], options[index].experimental && options[index].chosen, 14, step);
}

#if TARGET_OS_IPHONE
static void choose_saves(void) {
    port_ios_pick_saves(window);
    port_ios_saves_name(saves_name, sizeof saves_name);
}

static void choose_disc(void) {
    char path[PATH_SIZE] = "";
    port_ios_pick_disc(window, path, sizeof path);
    if (path[0]) check_disc(path);
}
#else
static void choose_saves(void) {}

static void picked_file(void *data, const char *const *files, int filter) {
    (void)data;
    (void)filter;
    if (files && files[0]) snprintf(picked_path, sizeof picked_path, "%s", files[0]);
    else picked_path[0] = 0;
    SDL_SetAtomicInt(&picked, 1);
}

static void choose_file(const SDL_DialogFileFilter *filter, const char *start) {
    if (choosing || SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) return;
    choosing = 1;
    SDL_ShowOpenFileDialog(picked_file, NULL, window, filter, 1, start, false);
}

static void choose_disc(void) {
    static const SDL_DialogFileFilter filter = {"Disc images", "iso;bin;img"};
    choose_file(&filter, disc_path[0] ? disc_path : NULL);
}
#endif

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
    mod_selected = 0;
    mod_first = 0;
}

static void select_mod(int row) {
    mod_selected = row;
    if (mod_selected < mod_first) mod_first = mod_selected;
    if (mod_selected >= mod_first + MOD_ROWS) mod_first = mod_selected - MOD_ROWS + 1;
}

#ifdef __ANDROID__
static void open_mods_folder(void) {
    static const SDL_DialogFileFilter filter = {"Mods", "dc2pak"};
    choose_file(&filter, NULL);
}
#else
static void open_mods_folder(void) {
    char url[PATH_SIZE + 16];
    SDL_CreateDirectory(port_paks_folder());
    snprintf(url, sizeof url, "%s%s", DESKTOP ? "file://" : "shareddocuments://", port_paks_folder());
    SDL_OpenURL(url);
}
#endif

static void toggle_mod(int row) {
    char why[LINE_SIZE];
    if (row == mod_count) open_mods_folder();
    else if (mod_on[row]) mod_on[row] = 0;
    else if (!mod_clash(row, why, sizeof why)) mod_on[row] = 1;
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
    if (item == MENU_LAST && !SAVES_ITEM) leave(0, now);
    if (item == MENU_LAST && SAVES_ITEM) choose_saves();
}

static void press(int key, float now) {
    if (key == KEY_PLAY) play(now);
    if (page == PAGE_MAIN) {
        if (key == KEY_UP) menu_selected = (menu_selected + MENU_COUNT - 1) % MENU_COUNT;
        if (key == KEY_DOWN) menu_selected = (menu_selected + 1) % MENU_COUNT;
        if (key == KEY_ACCEPT) activate_main(menu_selected, now);
        if (key == KEY_BACK && DESKTOP) leave(0, now);
        return;
    }
    if (page == PAGE_MODS) {
        if (key == KEY_UP) select_mod((mod_selected + mod_count) % (mod_count + 1));
        if (key == KEY_DOWN) select_mod((mod_selected + 1) % (mod_count + 1));
        reveal(mod_box(mod_selected).y + MOD_HEIGHT / 2.0f);
        if (key == KEY_ACCEPT || key == KEY_LEFT || key == KEY_RIGHT) toggle_mod(mod_selected);
        if (key == KEY_BACK) page = PAGE_MAIN;
        return;
    }
    if (key == KEY_UP) focus_row((row_selected + shown_count) % (shown_count + 1));
    if (key == KEY_DOWN) focus_row((row_selected + 1) % (shown_count + 1));
    reveal(row_top(row_selected) + ROW_HEIGHT / 2.0f);
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
    if (code == SDLK_ESCAPE || code == SDLK_BACKSPACE || code == SDLK_AC_BACK) return KEY_BACK;
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

static int mod_at(float x, float y) {
    for (int row = mod_first; page == PAGE_MODS && row <= mod_count && row < mod_first + MOD_ROWS; row++)
        if (inside(mod_box(row), x, y)) return row;
    return -1;
}

static int clickable(float x, float y) {
    return (DESKTOP && inside(close_box(), x, y)) || menu_item_at(x, y) >= 0 || row_at(x, y) >= 0 || mod_at(x, y) >= 0 ||
           (page != PAGE_MAIN && inside(back_box(), x, y));
}

static void hover(float x, float y) {
    pointer_x = x;
    pointer_y = y;
    if (menu_item_at(x, y) >= 0) menu_selected = menu_item_at(x, y);
    if (row_at(x, y) >= 0) focus_row(row_at(x, y));
    if (mod_at(x, y) >= 0) mod_selected = mod_at(x, y);
    SDL_SetCursor(clickable(x, y) ? hand_cursor : arrow_cursor);
}

static void click_row(int row, float x) {
    if (row == shown_count) {
        restore_defaults();
        return;
    }
    const Option *option = &options[shown[row]];
    float value_middle = control_right() - 20 - measure(STYLE_BODY, option->names[option->chosen]) / 2;
    change(row, !option->toggle && x < value_middle ? -1 : 1);
}

static void click(float x, float y, int backwards, float now) {
    if (DESKTOP && inside(close_box(), x, y)) leave(0, now);
    else if (menu_item_at(x, y) >= 0) activate_main(menu_item_at(x, y), now);
    else if (page != PAGE_MAIN && inside(back_box(), x, y)) page = PAGE_MAIN;
    else if (mod_at(x, y) >= 0) toggle_mod(mod_selected = mod_at(x, y));
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

static int ends_with(const char *text, const char *end) {
    size_t length = strlen(text), wanted = strlen(end);
    return length >= wanted && !SDL_strcasecmp(text + length - wanted, end);
}

static void read_mods(void) {
    mod_count = SDL_min(port_paks_list(), MOD_LIMIT);
    char why[LINE_SIZE];
    for (int index = 0; index < mod_count; index++) {
        mod_on[index] = 0;
        mod_on[index] = port_pak_enabled(index) && !mod_clash(index, why, sizeof why);
        mod_mark[index] = (float)mod_on[index];
    }
}

static void dropped(const char *path) {
    char name[PATH_SIZE], target[PATH_SIZE * 2];
    file_name(path, name, sizeof name);
    if (!ends_with(name, ".dc2pak")) {
        check_disc(path);
        return;
    }
    SDL_CreateDirectory(port_paks_folder());
    snprintf(target, sizeof target, "%s/%s", port_paks_folder(), name);
    int was[MOD_LIMIT];
    char ids[MOD_LIMIT][128];
    int count = mod_count;
    for (int index = 0; index < count; index++) {
        was[index] = mod_on[index];
        snprintf(ids[index], sizeof ids[index], "%s", port_pak_text(index, "id"));
    }
    if (!SDL_CopyFile(path, target)) {
        fprintf(stderr, "launcher: cannot copy %s: %s\n", path, SDL_GetError());
        return;
    }
    mod_count = SDL_min(port_paks_list(), MOD_LIMIT);
    for (int index = 0; index < mod_count; index++) {
        mod_on[index] = 0;
        for (int old = 0; old < count; old++)
            if (was[old] && !strcmp(ids[old], port_pak_text(index, "id"))) mod_on[index] = 1;
        mod_mark[index] = (float)mod_on[index];
    }
    page = PAGE_MODS;
}

static void touch(SDL_Event *event, float now) {
    float x = event->button.x, y = event->button.y;
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        press_x = x;
        press_y = y;
        press_scroll = scroll_goal;
        dragging = 0;
    }
    if (event->type == SDL_EVENT_MOUSE_MOTION && press_x >= 0) {
        float moved = event->motion.y - press_y;
        if (fabsf(moved) > DRAG_START || fabsf(event->motion.x - press_x) > DRAG_START) dragging = overflows() ? 1 : -1;
        if (dragging > 0) scroll_goal = press_scroll - moved;
    }
    if (event->type == SDL_EVENT_MOUSE_BUTTON_UP && press_x >= 0) {
        if (!dragging) {
            hover(view_left + press_x, scroll + press_y);
            click(view_left + press_x, scroll + press_y, 0, now);
        }
        press_x = -1;
        dragging = 0;
    }
}

static void handle(SDL_Event *event, float now) {
    SDL_ConvertEventToRenderCoordinates(renderer, event);
    if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) leave(0, now);
    if (event->type == SDL_EVENT_KEY_DOWN) press(key_of(event->key.key), now);
    if (event->type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(event->gdevice.which);
    if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) press(pad_key_of(event->gbutton.button), now);
    if (!DESKTOP) {
        touch(event, now);
        return;
    }
    if (event->type == SDL_EVENT_MOUSE_MOTION) hover(event->motion.x, event->motion.y);
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        click(event->button.x, event->button.y, event->button.button == SDL_BUTTON_RIGHT, now);
    if (event->type == SDL_EVENT_DROP_FILE && event->drop.data) dropped(event->drop.data);
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
    pixel_scale = width > 0 ? SDL_min((float)width / view_width, (float)height / view_height) : 1;
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
    if (!DESKTOP)
        canvas = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
                                   (int)ceilf(CANVAS_WIDTH * pixel_scale), (int)ceilf(CANVAS_HEIGHT * pixel_scale));
    if (canvas) SDL_SetTextureScaleMode(canvas, SDL_SCALEMODE_LINEAR);
}

static void forget(SDL_Texture **texture) {
    if (*texture) SDL_DestroyTexture(*texture);
    *texture = NULL;
}

static void fit_view(void) {
    int width = 0, height = 0;
    SDL_GetWindowSize(window, &width, &height);
    if (width <= 0 || height <= 0) return;
    float unit = BODY_POINTS / styles[STYLE_BODY].size;
    view_height = SDL_min(height / unit, (float)CANVAS_HEIGHT);
    view_width = SDL_min(view_height * width / height, (float)CANVAS_WIDTH);
    view_left = CANVAS_WIDTH - view_width;
}

static int open_window(int hidden) {
    float zoom = window_zoom();
    SDL_WindowFlags flags = WINDOW_FLAGS | (hidden ? SDL_WINDOW_HIDDEN : 0);
    if (!SDL_CreateWindowAndRenderer("DC2", (int)(CANVAS_WIDTH * zoom), (int)(CANVAS_HEIGHT * zoom), flags,
                                     &window, &renderer)) {
        fprintf(stderr, "launcher: %s\n", SDL_GetError());
        return 0;
    }
    if (!DESKTOP) fit_view();
    SDL_SetRenderLogicalPresentation(renderer, (int)view_width, (int)view_height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderVSync(renderer, 1);
    if (DESKTOP) SDL_SetWindowHitTest(window, hit_test, NULL);
    port_window_icon(window);
    arrow_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    hand_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    make_textures();
    return 1;
}

static void forget_textures(void) {
    forget(&canvas);
    forget(&backdrop);
    forget(&glow);
    forget(&chevron);
    forget(&stroke_bar);
    for (int shape = 0; shape < SHAPE_COUNT; shape++) forget(&shape_textures[shape]);
    for (int style = 0; style < STYLE_COUNT; style++) forget(&font_textures[style]);
}

static void remake_textures(void) {
    forget_textures();
    make_textures();
}

static void close_window(void) {
    forget_textures();
    SDL_DestroyCursor(arrow_cursor);
    SDL_DestroyCursor(hand_cursor);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    renderer = NULL;
    window = NULL;
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
    draw_canvas(now);
    SDL_Surface *surface = SDL_RenderReadPixels(renderer, NULL);
    if (!surface || !SDL_SaveBMP(surface, path)) fprintf(stderr, "launcher: no picture: %s\n", SDL_GetError());
    SDL_DestroySurface(surface);
    SDL_SetRenderTarget(renderer, NULL);
    SDL_DestroyTexture(canvas);
}

int port_launcher(void) {
    const char *chosen = SDL_getenv("DC2TK_LAUNCHER_SETTINGS");
    char *folder = chosen ? NULL : SDL_GetPrefPath("DC2 Toolkit", "DC2");
    if (chosen) snprintf(settings_path, sizeof settings_path, "%s", chosen);
    else snprintf(settings_path, sizeof settings_path, "%slauncher.cfg", folder ? folder : "");
    SDL_free(folder);
    port_settings_file(settings_path);
    if (!DESKTOP) SDL_CreateDirectory(port_paks_folder());
    read_options();
    read_mods();
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
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_RENDER_DEVICE_RESET) remake_textures();
            handle(&event, now);
        }
        if (SDL_GetAtomicInt(&picked)) {
            SDL_SetAtomicInt(&picked, 0);
            choosing = 0;
            if (picked_path[0]) dropped(picked_path);
        }
        animate(step);
        draw(now);
        SDL_RenderPresent(renderer);
    }
    close_window();
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    if (!playing) return 0;
    save_options();
    return 1;
}
