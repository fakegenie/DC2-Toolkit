#include "port_present.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PORT_GRAPHICS
#include "gs.h"
#include "input.h"
#include "port_graphics.h"
#include <SDL3/SDL.h>

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static unsigned texture_width;
static unsigned texture_height;
static uint32_t pixels[2048 * 2048];
static const uint32_t *movie_pixels;
static unsigned movie_width;
static unsigned movie_height;
static int screenshot_saved;

void port_present_movie(const uint32_t *rgba, unsigned width, unsigned height) {
    if (!rgba || !width || !height || width > 2048 || height > 2048) return;
    movie_pixels = rgba;
    movie_width = width;
    movie_height = height;
}

void port_present_movie_clear(void) {
    movie_pixels = NULL;
    movie_width = 0;
    movie_height = 0;
}

static void present_error(void) {
    fprintf(stderr, "display: %s\n", SDL_GetError());
    exit(1);
}

int port_present_init(int enabled) {
    if (!enabled) return 1;
    if (!SDL_Init(SDL_INIT_VIDEO) || !port_input_init()) {
        fprintf(stderr, "display: %s\n", SDL_GetError());
        return 0;
    }
    if (!SDL_CreateWindowAndRenderer("Dark Chronicle", 960, 720, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        fprintf(stderr, "display: %s\n", SDL_GetError());
        SDL_Quit();
        return 0;
    }
    if (!SDL_SetRenderLogicalPresentation(renderer, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX)) present_error();
    return 1;
}

static void save_frame(unsigned width, unsigned height) {
    const char *path = getenv("DC2TK_SCREENSHOT");
    if (!path || !*path) return;
    const char *field_text = getenv("DC2TK_SCREENSHOT_FIELD");
    unsigned long field = field_text ? strtoul(field_text, NULL, 10) : 100;
    if (screenshot_saved || port_graphics.fields < field) return;
    SDL_Surface *surface = SDL_CreateSurfaceFrom((int)width, (int)height, SDL_PIXELFORMAT_RGBA32,
                                                pixels, (int)(width * sizeof(*pixels)));
    if (!surface || !SDL_SaveBMP(surface, path)) present_error();
    SDL_DestroySurface(surface);
    screenshot_saved = 1;
    fprintf(stderr, "display: saved field %llu to %s\n", (unsigned long long)port_graphics.fields, path);
}

void port_present_frame(void) {
    if (!renderer) return;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) exit(0);
    }
    port_input_poll();
    uint64_t framebuffer = port_graphics.display[2];
    uint64_t display = port_graphics.display[3];
    if (!movie_pixels && !((framebuffer >> 9) & 63)) return;
    unsigned width = (((display >> 32) & 4095) + 1) / (((display >> 23) & 15) + 1);
    unsigned height = (((display >> 44) & 2047) + 1) / (((display >> 27) & 3) + 1);
    if (port_graphics.interlace && port_graphics.frame_mode) height /= 2;
    if (movie_pixels) {
        width = movie_width;
        height = movie_height;
        memcpy(pixels, movie_pixels, (size_t)width * height * sizeof(*pixels));
    }
    if (!width || !height || width > 2048 || height > 2048) return;
    if (!movie_pixels && port_gs_read_rgba((unsigned)(framebuffer & 511) * 32, (unsigned)(framebuffer >> 9) & 63,
                          (unsigned)(framebuffer >> 15) & 31, (unsigned)(framebuffer >> 32) & 2047,
                          (unsigned)(framebuffer >> 43) & 2047, width, height, pixels, width) < 0) {
        fprintf(stderr, "display: %s\n", port_gs_error());
        exit(1);
    }
    if (width != texture_width || height != texture_height) {
        SDL_DestroyTexture(texture);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                    (int)width, (int)height);
        if (!texture || !SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST)) present_error();
        texture_width = width;
        texture_height = height;
    }
    SDL_FRect target = {0, 0, 640, 480};
    if (!SDL_UpdateTexture(texture, NULL, pixels, (int)(width * sizeof(*pixels))) ||
        !SDL_RenderClear(renderer) || !SDL_RenderTexture(renderer, texture, NULL, &target) ||
        !SDL_RenderPresent(renderer)) present_error();
    save_frame(width, height);
}

void port_present_close(void) {
    port_present_movie_clear();
    port_input_close();
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    texture = NULL;
    renderer = NULL;
    window = NULL;
    SDL_Quit();
}

#else

int port_present_init(int enabled) {
    if (enabled) fputs("display: rebuild with graphics enabled\n", stderr);
    return !enabled;
}

void port_present_frame(void) {}
void port_present_close(void) {}
void port_present_movie(const uint32_t *rgba, unsigned width, unsigned height) {
    (void)rgba;
    (void)width;
    (void)height;
}
void port_present_movie_clear(void) {}

#endif
