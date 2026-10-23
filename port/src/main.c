#include "cdvd.h"
#include "port_kernel.h"
#include "split_data.h"
#include "port_present.h"
#include "executable.h"
#include "audio.h"
#include "port_mod.h"
#include "port_overrides.h"
#include "render.h"
#include "launcher.h"
#include "touch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if TARGET_OS_IPHONE
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <unistd.h>
#include "ios_files.h"
#endif

extern int port_game_main(void);
extern int port_debug;

#if TARGET_OS_IPHONE
static void enter_documents(void) {
    if (chdir(port_ios_documents())) fprintf(stderr, "cannot enter %s\n", port_ios_documents());
}

static int touch_default(void) { return TOUCH_SMART; }
#else
static void enter_documents(void) {}
static void port_ios_saves(void) {}
static int touch_default(void) { return TOUCH_OFF; }
#endif

static int scale_of(const char *text) {
    return !strcmp(text, "auto") ? RENDER_SCALE_AUTO : atoi(text);
}

static int setting_number(const char *name, int fallback) {
    const char *text = port_setting(name);
    if (!text) return fallback;
    if (!strcmp(text, "true")) return 1;
    if (!strcmp(text, "false")) return 0;
    return atoi(text);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int first = 1;
    int scale = 0, samples = 0, mute = 0, background = 0, fullscreen = -1, vsync = -1, smooth = -1;
    const char *aspect = NULL;
    int window_width = 0, window_height = 0;
    for (; first < argc; ++first) {
        if (!strcmp(argv[first], "--debug")) port_debug = 1;
        else if (!strcmp(argv[first], "--mute")) mute = 1;
        else if (!strcmp(argv[first], "--background")) background = 1;
        else if (!strcmp(argv[first], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[first], "--vsync")) vsync = 1;
        else if (!strcmp(argv[first], "--mods") && first + 1 < argc) port_setting_set("mods.enabled", argv[++first]);
        else if (!strcmp(argv[first], "--smooth") && first + 1 < argc) smooth = atoi(argv[++first]);
        else if (!strcmp(argv[first], "--aspect") && first + 1 < argc) aspect = argv[++first];
        else if (!strcmp(argv[first], "--window") && first + 1 < argc) sscanf(argv[++first], "%dx%d", &window_width, &window_height);
        else if (!strcmp(argv[first], "--scale") && first + 1 < argc) scale = scale_of(argv[++first]);
        else if (!strcmp(argv[first], "--msaa") && first + 1 < argc) samples = atoi(argv[++first]);
        else if (strcmp(argv[first], "--graphics")) break;
    }
    enter_documents();
    int launcher = first == argc;
    if (launcher && !port_launcher()) return 0;
    const char *disc = first == argc - 1 ? argv[first] : launcher ? port_setting("disc") : NULL;
    if (launcher) {
        port_debug |= setting_number("debug.menus", 0);
        mute |= setting_number("sound.mute", 0);
    }
    if (!disc) {
        fprintf(stderr,
                "usage: %s [--debug] [--mute] [--background] [--fullscreen] [--vsync] [--smooth HZ] [--aspect W:H|fill] [--window WxH] [--scale 1-4|auto] "
                "[--msaa 1|2|4|8] [--mods ID,ID] [DISC.iso]\n"
                "Without a disc the launcher opens.\n",
                argv[0]);
        return 1;
    }
    port_overrides_scan();
    if (!aspect) aspect = port_setting("render.aspect");
    const char *set_scale = port_setting("render.scale");
    const char *set_samples = port_setting("render.msaa");
    if (!scale) scale = set_scale ? scale_of(set_scale) : 1;
    if (!samples) samples = set_samples ? atoi(set_samples) : 1;
    render_set_quality(scale, samples);
    const char *set_window = port_setting("render.window");
    if (!window_width && set_window) sscanf(set_window, "%dx%d", &window_width, &window_height);
    render_set_window(window_width, window_height, fullscreen >= 0 ? fullscreen : setting_number("render.fullscreen", 0));
    render_set_vsync(vsync >= 0 ? vsync : setting_number("render.vsync", 0));
    render_set_filter(port_setting("render.filter") && !strcmp(port_setting("render.filter"), "nearest"));
    int numerator = 0, denominator = 0;
    if (aspect && !strcmp(aspect, "fill")) render_fill_screen();
    else if (aspect && sscanf(aspect, "%d:%d", &numerator, &denominator) == 2) render_set_aspect(numerator, denominator);
    const char *set_smooth = port_setting("render.smooth");
    if (smooth < 0 && set_smooth && strcmp(set_smooth, "false")) smooth = atoi(set_smooth);
    if (smooth >= 0) {
        smooth_set_rate(smooth);
        render_set_smoothing(1);
    }
    render_set_background(background);
    const char *touch = port_setting("input.touch");
    port_touch_set_mode(!touch                    ? touch_default()
                        : !strcmp(touch, "smart") ? TOUCH_SMART
                        : !strcmp(touch, "true")  ? TOUCH_ON
                                                  : TOUCH_OFF);
    port_touch_set_haptics(setting_number("input.haptics", 1));
    if (!port_cdvd_init(disc) || !port_executable_check() || !port_split_data_init()) return 1;
    if (!port_present_init(1)) return 1;
    port_ios_saves();
    atexit(port_present_close);
    if (!audio_init(!mute)) return 1;
    atexit(audio_shutdown);
    port_mods_start();
    return port_kernel_run(port_game_main);
}
