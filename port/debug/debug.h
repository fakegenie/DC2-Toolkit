#pragma once

#include "input.h"
#include "port_stats.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DEBUG_ARG_LIMIT = 16, DEBUG_HISTORY = 600, DEBUG_CLIENT_NONE = -1 };

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
extern const DebugCommand debug_replay_commands[];
extern const DebugCommand debug_savestate_commands[];
extern const DebugCommand debug_reload_commands[];

void debug_scenario_panel(void);
void debug_capture_panel(void);
void debug_replay_panel(void);
void debug_savestate_panel(void);
void debug_reload_bar(void);

void debug_scenario_status(DebugReply *reply);
void debug_capture_frame(void);
void debug_capture_present(int target, int width, int height);
void debug_replay_input(struct port_input_state *state);
void debug_replay_frame(void);
void debug_savestate_frame(void);
void debug_reload_frame(void);

#ifdef __cplusplus
}
#endif
