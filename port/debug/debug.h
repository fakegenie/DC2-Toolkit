#pragma once

#include "input.h"
#include "port_state.h"
#include "port_stats.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DEBUG_ARG_LIMIT = 16, DEBUG_HISTORY = 600, DEBUG_CLIENT_NONE = -1, DEBUG_TRACE = 256 };

typedef struct {
    int count;
    char keys[DEBUG_ARG_LIMIT][32];
    char values[DEBUG_ARG_LIMIT][256];
} DebugArgs;

typedef struct {
    int client;
    int status;
    char *text;
    size_t length;
    size_t capacity;
    const char *content_type;
    unsigned char *body;
    size_t body_length;
    int deferred;
} DebugReply;

typedef struct {
    const char *name;
    int (*run)(const DebugArgs *args, DebugReply *reply);
    int immediate;
    const char *help;
} DebugCommand;

typedef struct {
    uint64_t field;
    uint32_t frame;
    float frame_ms;
    float game_ms;
    float draw_ms;
    float wait_ms;
    float present_ms;
    PortStats stats;
} DebugFrame;

typedef struct {
    float player[3];
    float camera[3];
    float look[3];
} DebugPoint;

extern DebugFrame debug_frames[DEBUG_HISTORY];
extern uint32_t debug_frame_total;
extern int debug_paused;
extern int debug_steps;
extern float debug_speed;
extern char debug_last_result[4096];

const DebugFrame *debug_frame_back(int back);

const char *debug_arg(const DebugArgs *args, const char *key, const char *fallback);
long debug_arg_int(const DebugArgs *args, const char *key, long fallback);
double debug_arg_number(const DebugArgs *args, const char *key, double fallback);
int debug_map_number(const char *text);
void debug_parse_query(const char *query, DebugArgs *args);
void debug_print(DebugReply *reply, const char *format, ...);
void debug_print_string(DebugReply *reply, const char *text);
int debug_fail(DebugReply *reply, const char *format, ...);
void debug_finish(DebugReply *reply);
void debug_run(const char *line);
void debug_dispatch(const char *name, const DebugArgs *args, int client);
void debug_run_queued(void);
unsigned char *debug_png(const uint32_t *rgba, int width, int height, size_t *length);

void debug_server_start(int port);
void debug_server_poll(void);
void debug_server_send(int client, int status, const char *content_type, const void *data, size_t length);

void debug_log_start(void);
uint32_t debug_log_total(void);
int debug_log_line(uint32_t number, char *line, size_t size);

void debug_window_start(void);
int debug_window_event(const void *event);
void debug_window_draw(void);
void debug_window_toggle(void);
int debug_window_shot(const DebugArgs *args, DebugReply *reply);
int debug_window_tab(const DebugArgs *args, DebugReply *reply);

extern const DebugCommand debug_core_commands[];
extern const DebugCommand debug_scenario_commands[];
extern const DebugCommand debug_capture_commands[];
extern const DebugCommand debug_savestate_commands[];
extern const DebugCommand debug_reload_commands[];
extern const DebugCommand debug_overlay_commands[];
extern const DebugCommand debug_recording_commands[];
extern const DebugCommand debug_memory_commands[];
extern const DebugCommand debug_watch_commands[];
extern const DebugCommand debug_fault_commands[];

void debug_scenario_panel(void);
void debug_capture_panel(void);
void debug_recording_panel(void);
void debug_savestate_panel(void);
void debug_watch_panel(void);
void debug_reload_bar(void);
void debug_helpers_panel(void);
enum {
    DEBUG_BUTTON_L2 = 1 << 0, DEBUG_BUTTON_R2 = 1 << 1, DEBUG_BUTTON_L1 = 1 << 2, DEBUG_BUTTON_R1 = 1 << 3,
    DEBUG_BUTTON_TRIANGLE = 1 << 4, DEBUG_BUTTON_CIRCLE = 1 << 5, DEBUG_BUTTON_CROSS = 1 << 6,
    DEBUG_BUTTON_SQUARE = 1 << 7, DEBUG_BUTTON_SELECT = 1 << 8, DEBUG_BUTTON_L3 = 1 << 9, DEBUG_BUTTON_R3 = 1 << 10,
    DEBUG_BUTTON_START = 1 << 11, DEBUG_BUTTON_UP = 1 << 12, DEBUG_BUTTON_RIGHT = 1 << 13,
    DEBUG_BUTTON_DOWN = 1 << 14, DEBUG_BUTTON_LEFT = 1 << 15
};

void debug_hold(uint16_t buttons, int frames);
void debug_start(void);
int debug_event(const void *event);
void debug_field(void);
void debug_frame_enter(void);
void debug_frame_drawn(void);
void debug_frame_exit(void);
void debug_overlay(void);
void debug_present(int target, int width, int height);
void debug_input(struct port_input_state *state);
void debug_audio(const short *samples, int frames);

void debug_scenario_status(DebugReply *reply);
void debug_capture_frame(void);
void debug_capture_present(int target, int width, int height);
void debug_savestate_frame(void);
void debug_reload_frame(void);
void debug_fault_frame(void);
int debug_menu_up(void);
void debug_fault_panel(void);
void debug_recording_start(void);
void debug_recording_input(struct port_input_state *state);
void debug_recording_frame(void);
void debug_recording_command(const char *name, const DebugArgs *args);
void debug_recording_sample(DebugPoint *point);
void debug_recording_lost(const char *why);
void debug_recording_crash(void);
void debug_recording_hotkey_start(void);
void debug_recording_hotkey_save(void);
int debug_recording_busy(void);
int debug_recording_holds_input(void);
const char *debug_recording_write(const char *prefix, char *name, size_t size);
int debug_scenario_enter(int loop, int map, int floor, int event);
void debug_scenario_frame(void);

#ifdef __cplusplus
}
#endif
