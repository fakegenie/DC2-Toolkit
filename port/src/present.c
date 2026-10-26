#include "port_present.h"
#include "input.h"
#include "port_graphics.h"
#include "render.h"
#include "port_mod.h"
#include "touch.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { EVENT_LIMIT = 512, TEXT_LIMIT = 64, WAIT_LIMIT_MS = 2000 };

static int started;
static int movie_texture;
static const uint32_t *movie_pixels;
static unsigned movie_width;
static unsigned movie_height;
static int movie_age;
static int movie_new;
static char requested_path[1024];
static struct {
    SDL_Event event;
    char text[TEXT_LIMIT];
} queued[EVENT_LIMIT];
static int queued_first;
static int queued_count;
static SDL_Mutex *lock;
static SDL_Condition *changed;
static int park_wanted;
static int parked;
static int stop_wanted;
static int stopped;

int port_present_init(int enabled) {
    (void)enabled;
    if (!render_init() || !port_input_init()) {
        fprintf(stderr, "display: %s\n", SDL_GetError());
        return 0;
    }
    lock = SDL_CreateMutex();
    changed = SDL_CreateCondition();
    started = 1;
    return 1;
}

static int forwarded(const SDL_Event *event) {
    return !(event->type >= SDL_EVENT_DROP_FILE && event->type <= SDL_EVENT_DROP_POSITION) &&
           event->type != SDL_EVENT_CLIPBOARD_UPDATE && event->type != SDL_EVENT_TEXT_EDITING_CANDIDATES;
}

static void enqueue(const SDL_Event *event) {
    if (!forwarded(event)) return;
    const char *text = event->type == SDL_EVENT_TEXT_INPUT     ? event->text.text
                       : event->type == SDL_EVENT_TEXT_EDITING ? event->edit.text
                                                               : NULL;
    SDL_LockMutex(lock);
    if (queued_count < EVENT_LIMIT) {
        int slot = (queued_first + queued_count++) % EVENT_LIMIT;
        queued[slot].event = *event;
        SDL_strlcpy(queued[slot].text, text ? text : "", TEXT_LIMIT);
    }
    SDL_UnlockMutex(lock);
}

static int dequeue(SDL_Event *event, char *text) {
    SDL_LockMutex(lock);
    int found = queued_count > 0;
    if (found) {
        *event = queued[queued_first].event;
        SDL_strlcpy(text, queued[queued_first].text, TEXT_LIMIT);
        queued_first = (queued_first + 1) % EVENT_LIMIT;
        queued_count--;
    }
    SDL_UnlockMutex(lock);
    if (found && event->type == SDL_EVENT_TEXT_INPUT) event->text.text = text;
    if (found && event->type == SDL_EVENT_TEXT_EDITING) event->edit.text = text;
    return found;
}

static void wait_until_set(const int *flag) {
    Uint64 deadline = SDL_GetTicks() + WAIT_LIMIT_MS;
    while (!*flag && SDL_GetTicks() < deadline) SDL_WaitConditionTimeout(changed, lock, (Sint32)(deadline - SDL_GetTicks()));
}

static void park_game(void) {
    SDL_LockMutex(lock);
    park_wanted = 1;
    wait_until_set(&parked);
    SDL_UnlockMutex(lock);
}

static void release_game(void) {
    SDL_LockMutex(lock);
    park_wanted = 0;
    SDL_BroadcastCondition(changed);
    SDL_UnlockMutex(lock);
}

static void hold_while(const int *wanted, int *held) {
    while (*wanted) {
        *held = 1;
        SDL_BroadcastCondition(changed);
        SDL_WaitCondition(changed, lock);
    }
}

static void park_if_wanted(void) {
    SDL_LockMutex(lock);
    if (park_wanted) render_finish();
    hold_while(&park_wanted, &parked);
    parked = 0;
    SDL_UnlockMutex(lock);
}

static void stop_if_wanted(void) {
    SDL_LockMutex(lock);
    hold_while(&stop_wanted, &stopped);
    SDL_UnlockMutex(lock);
}

static void stop_main(void) {
    SDL_LockMutex(lock);
    stop_wanted = 1;
    SDL_UnlockMutex(lock);
    SDL_Event wake = {0};
    wake.type = render_wake_event();
    SDL_PushEvent(&wake);
    SDL_LockMutex(lock);
    wait_until_set(&stopped);
    SDL_UnlockMutex(lock);
}

static void quit(void) {
    park_game();
    exit(0);
}

static void wait_for_foreground(void) {
    park_game();
    SDL_Event event;
    while (SDL_WaitEvent(&event) && event.type != SDL_EVENT_DID_ENTER_FOREGROUND)
        if (event.type == SDL_EVENT_QUIT) quit();
    release_game();
    render_redraw();
}

static void handle(const SDL_Event *event) {
    if (event->type == render_wake_event()) return;
    if (event->type == SDL_EVENT_QUIT) quit();
    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event->window.windowID == SDL_GetWindowID(render_window()))
        quit();
    if (event->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
        wait_for_foreground();
        return;
    }
    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat &&
        (event->key.key == SDLK_F11 || (event->key.key == SDLK_RETURN && (event->key.mod & SDL_KMOD_ALT)))) {
        render_toggle_fullscreen();
        return;
    }
    if (event->type == SDL_EVENT_WINDOW_RESIZED || event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        event->type == SDL_EVENT_WINDOW_EXPOSED)
        render_redraw();
    enqueue(event);
}

void port_present_run(void) {
    for (;;) {
        SDL_Event event;
        if (SDL_WaitEvent(&event)) handle(&event);
        while (SDL_PollEvent(&event)) handle(&event);
        stop_if_wanted();
        render_show_screens();
    }
}

void port_present_frame(void) {
    if (!started) return;
    SDL_Event event;
    char text[TEXT_LIMIT];
    while (dequeue(&event, text)) {
        if (port_mods_event(&event)) continue;
        port_touch_event(&event);
    }
    park_if_wanted();
    port_input_poll();
    port_mods_field();
}

void port_present_close(void) {
    if (!started) return;
    if (!SDL_IsMainThread()) {
        stop_main();
        return;
    }
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
    fprintf(stderr, "display: saved field %llu to %s\n", (unsigned long long)port_fields, requested_path);
    requested_path[0] = 0;
}

void port_present_save(const char *path) {
    snprintf(requested_path, sizeof requested_path, "%s", path);
}

static void save_frame(int target, int width, int height) {
    if (requested_path[0]) save_requested(target, width, height);
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
    port_mods_present(target, width, height);
    render_present(target, width, height);
}
