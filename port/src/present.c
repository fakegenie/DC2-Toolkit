#include "port_present.h"
#include "input.h"
#include "port_graphics.h"
#include "render.h"
#include "port_debug.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int started;
static int movie_texture;
static const uint32_t *movie_pixels;
static unsigned movie_width;
static unsigned movie_height;
static int movie_age;
static int movie_new;
static int screenshot_saved;
static char requested_path[1024];

int port_present_init(int enabled) {
    (void)enabled;
    if (!render_init() || !port_input_init()) {
        fprintf(stderr, "display: %s\n", SDL_GetError());
        return 0;
    }
    started = 1;
    return 1;
}

void port_present_frame(void) {
    if (!started) return;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (debug_event(&event)) continue;
        if (event.type == SDL_EVENT_QUIT) exit(0);
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(render_window()))
            exit(0);
    }
    port_input_poll();
    debug_field();
}

void port_present_close(void) {
    if (!started) return;
    port_input_close();
    render_shutdown();
    started = 0;
    SDL_Quit();
}

void port_present_movie(const uint32_t *rgba, unsigned width, unsigned height) {
    if (!rgba || !width || !height || width > 2048 || height > 2048) return;
    movie_pixels = rgba;
    movie_width = width;
    movie_height = height;
    movie_age = 0;
    movie_new = 1;
}

void port_present_movie_clear(void) {
    movie_pixels = NULL;
    movie_width = 0;
    movie_height = 0;
}

static void save_requested(int target, int width, int height) {
    uint32_t *pixels = malloc((size_t)width * (size_t)height * 4);
    render_download(target, 0, 0, width, height, pixels);
    SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, pixels, width * 4);
    if (!surface || !SDL_SaveBMP(surface, requested_path)) fprintf(stderr, "display: %s\n", SDL_GetError());
    SDL_DestroySurface(surface);
    free(pixels);
    fprintf(stderr, "display: saved field %llu to %s\n", (unsigned long long)port_graphics.fields, requested_path);
    requested_path[0] = 0;
}

void port_present_save(const char *path) {
    snprintf(requested_path, sizeof requested_path, "%s", path);
}

static void save_frame(int target, int width, int height) {
    if (requested_path[0]) save_requested(target, width, height);
    const char *path = getenv("DC2TK_SCREENSHOT");
    if (!path || !*path) return;
    const char *every_text = getenv("DC2TK_SCREENSHOT_EVERY");
    unsigned long every = every_text ? strtoul(every_text, NULL, 10) : 0;
    const char *field_text = getenv("DC2TK_SCREENSHOT_FIELD");
    unsigned long field = field_text ? strtoul(field_text, NULL, 10) : 100;
    if (port_graphics.fields < field || (every ? port_graphics.fields % every > 1 : screenshot_saved)) return;
    char name[1024];
    if (every) snprintf(name, sizeof name, "%s%06llu.bmp", path, (unsigned long long)port_graphics.fields);
    else snprintf(name, sizeof name, "%s", path);
    uint32_t *pixels = malloc((size_t)width * (size_t)height * 4);
    render_download(target, 0, 0, width, height, pixels);
    SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, pixels, width * 4);
    if (!surface || !SDL_SaveBMP(surface, name)) fprintf(stderr, "display: %s\n", SDL_GetError());
    SDL_DestroySurface(surface);
    free(pixels);
    screenshot_saved = 1;
    fprintf(stderr, "display: saved field %llu to %s\n", (unsigned long long)port_graphics.fields, name);
}

void port_present_show(int target, int width, int height) {
    if (!started) return;
    if (movie_pixels && ++movie_age > 50) port_present_movie_clear();
    if (movie_pixels) {
        if (!movie_texture || render_width(movie_texture) != (int)movie_width ||
            render_height(movie_texture) != (int)movie_height) {
            render_release(movie_texture);
            movie_texture = render_texture((int)movie_width, (int)movie_height);
            movie_new = 1;
        }
        if (movie_new) render_upload(movie_texture, 0, 0, (int)movie_width, (int)movie_height, movie_pixels);
        movie_new = 0;
        target = movie_texture;
        width = (int)movie_width;
        height = (int)movie_height;
    }
    save_frame(target, width, height);
    debug_present(target, width, height);
    render_present(target, width, height);
}
