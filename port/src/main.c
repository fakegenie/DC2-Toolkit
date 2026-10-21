#include "cdvd.h"
#include "port_kernel.h"
#include "split_data.h"
#include "port_present.h"
#include "executable.h"
#include "audio.h"
#include "port_mod.h"
#include "render.h"
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
#endif

extern int port_game_main(void);
extern int port_debug;

#if TARGET_OS_IPHONE
void port_ios_saves(void);

static const char *bundled_disc(void) {
    static char path[2048];
    const char *documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    if (!documents || chdir(documents)) return NULL;
    snprintf(path, sizeof path, "%sdc2.iso", SDL_GetBasePath());
    return path;
}
#else
static const char *bundled_disc(void) { return NULL; }
static void port_ios_saves(void) {}
#endif

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int first = 1;
    int scale = 0, samples = 0, mute = 0, background = 0;
    for (; first < argc - 1; ++first) {
        if (!strcmp(argv[first], "--debug")) port_debug = 1;
        else if (!strcmp(argv[first], "--mute")) mute = 1;
        else if (!strcmp(argv[first], "--background")) background = 1;
        else if (!strcmp(argv[first], "--scale") && first + 2 < argc) scale = atoi(argv[++first]);
        else if (!strcmp(argv[first], "--msaa") && first + 2 < argc) samples = atoi(argv[++first]);
        else if (strcmp(argv[first], "--graphics")) break;
    }
    const char *disc = first == argc - 1 ? argv[first] : first == argc ? bundled_disc() : NULL;
    if (first == argc && port_mod_loaded("dc2tk.debug-tools")) port_debug = 1;
    if (!disc) {
        fprintf(stderr, "usage: %s [--debug] [--mute] [--background] [--scale 1-4] [--msaa 1|2|4|8] DISC.iso\n", argv[0]);
        return 1;
    }
    const char *set_scale = port_setting("render.scale");
    const char *set_samples = port_setting("render.msaa");
    if (!scale) scale = set_scale ? atoi(set_scale) : 1;
    if (!samples) samples = set_samples ? atoi(set_samples) : 1;
    render_set_quality(scale, samples);
    render_set_background(background);
    if (!port_cdvd_init(disc) || !port_executable_check() || !port_split_data_init()) return 1;
    if (!port_present_init(1)) return 1;
    port_ios_saves();
    atexit(port_present_close);
    if (!audio_init(!mute)) return 1;
    atexit(audio_shutdown);
    port_mods_start();
    return port_kernel_run(port_game_main);
}
