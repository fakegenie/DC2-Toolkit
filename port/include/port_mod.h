#pragma once

#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_MOD_API "1.2.0"

enum { RENDER_SCALE_AUTO = -1 };

typedef struct {
    int auto_scale;
    int fullscreen;
    int vsync;
    int nearest;
    int window_width;
    int window_height;
} RenderDisplay;

typedef struct {
    const char *id;
    void (*start)(void);
    int (*event)(const void *event);
    void (*field)(void);
    void (*frame_enter)(void);
    void (*frame_drawn)(void);
    void (*frame_exit)(void);
    void (*overlay)(void);
    void (*present)(int target, int width, int height);
    void (*input)(struct port_input_state *state);
    void (*audio)(const short *samples, int frames);
    void (*host)(void);
} PortMod;

typedef int (*PortCommandRun)(const char *const *keys, const char *const *values, int count, char *reply,
                              int size);
typedef void (*PortPanelDraw)(void);

void port_mod_register(const PortMod *mod);
int port_mod_loaded(const char *id);
const char *port_setting(const char *name);
void port_settings_file(const char *path);
void port_setting_set(const char *name, const char *value);
void port_settings_beneath(const char *text);
void port_mod_command(const char *name, PortCommandRun run, const char *help);
void port_mod_panel(const char *name, PortPanelDraw draw);

void render_set_quality(int scale, int samples);
void render_set_window(int width, int height, int fullscreen);
void render_toggle_fullscreen(void);
void render_set_vsync(int enabled);
void render_set_filter(int nearest);
void render_display(RenderDisplay *display);
void render_set_aspect(int numerator, int denominator);
void render_fill_screen(void);
void render_set_smoothing(int enabled);
int render_smoothing(void);
void smooth_set_rate(int hz);
int render_frame_width(void);

int port_mods_command(const char *name, const char *const *keys, const char *const *values, int count, char *reply,
                      int size);
int port_mods_command_count(void);
const char *port_mods_command_name(int index);
const char *port_mods_command_help(int index);
int port_mods_panel_count(void);
const char *port_mods_panel_name(int index);
void port_mods_panel_draw(int index);

void port_mods_start(void);
int port_mods_event(const void *event);
void port_mods_field(void);
void port_mods_frame_enter(void);
void port_mods_frame_drawn(void);
void port_mods_frame_exit(void);
void port_mods_overlay(void);
void port_mods_present(int target, int width, int height);
void port_mods_input(struct port_input_state *state);
void port_mods_audio(const short *samples, int frames);
void port_mods_host(void);

#define PORT_MOD(table)                                                       \
    __attribute__((constructor)) static void port_mod_register_##table(void) { \
        port_mod_register(&table);                                            \
    }

#ifdef __cplusplus
}
#endif
