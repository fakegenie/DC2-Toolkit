#include "port_mod.h"
#include "port_overrides.h"
#include <stdio.h>
#include <string.h>

enum { MOD_LIMIT = 32, SETTING_LIMIT = 256, SETTING_TEXT = 256, COMMAND_LIMIT = 64, PANEL_LIMIT = 32 };

static const PortMod *mods[MOD_LIMIT];
static int mod_count;
static const char *command_names[COMMAND_LIMIT];
static const char *command_helps[COMMAND_LIMIT];
static PortCommandRun command_runs[COMMAND_LIMIT];
static int command_count;
static const char *panel_names[PANEL_LIMIT];
static PortPanelDraw panel_draws[PANEL_LIMIT];
static int panel_count;
static char setting_names[SETTING_LIMIT][SETTING_TEXT];
static char setting_values[SETTING_LIMIT][SETTING_TEXT];
static int setting_count;
static int settings_read;

void port_mod_register(const PortMod *mod) {
    if (mod_count < MOD_LIMIT) mods[mod_count++] = mod;
}

int port_mod_loaded(const char *id) {
    for (int i = 0; i < mod_count; ++i)
        if (!strcmp(mods[i]->id, id)) return 1;
    return 0;
}

static void read_file(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return;
    char line[SETTING_TEXT * 2];
    while (setting_count < SETTING_LIMIT && fgets(line, sizeof line, file)) {
        line[strcspn(line, "\r\n")] = 0;
        char *equals = strchr(line, '=');
        if (!equals || line[0] == '#') continue;
        *equals = 0;
        snprintf(setting_names[setting_count], SETTING_TEXT, "%s", line);
        snprintf(setting_values[setting_count], SETTING_TEXT, "%s", equals + 1);
        setting_count++;
    }
    fclose(file);
}

static void read_settings(void) {
    settings_read = 1;
    char path[1280];
    snprintf(path, sizeof path, "%s/dc2tk-port.cfg", port_overrides_folder());
    read_file(path);
}

void port_settings_beneath(const char *text) {
    if (!settings_read) read_settings();
    char line[SETTING_TEXT * 2];
    for (const char *at = text; *at && setting_count < SETTING_LIMIT;) {
        size_t length = strcspn(at, "\r\n");
        snprintf(line, sizeof line, "%.*s", (int)length, at);
        at += length;
        while (*at == '\r' || *at == '\n') at++;
        char *equals = strchr(line, '=');
        if (!equals || line[0] == '#') continue;
        *equals = 0;
        memmove(setting_names + 1, setting_names, sizeof setting_names[0] * (size_t)setting_count);
        memmove(setting_values + 1, setting_values, sizeof setting_values[0] * (size_t)setting_count);
        snprintf(setting_names[0], SETTING_TEXT, "%s", line);
        snprintf(setting_values[0], SETTING_TEXT, "%s", equals + 1);
        setting_count++;
    }
}

void port_settings_file(const char *path) {
    if (!settings_read) read_settings();
    read_file(path);
}

const char *port_setting(const char *name) {
    if (!settings_read) read_settings();
    for (int i = setting_count - 1; i >= 0; --i)
        if (!strcmp(setting_names[i], name)) return setting_values[i];
    return NULL;
}

void port_setting_set(const char *name, const char *value) {
    if (!settings_read) read_settings();
    for (int i = setting_count - 1; i >= 0; --i)
        if (!strcmp(setting_names[i], name)) {
            snprintf(setting_values[i], SETTING_TEXT, "%s", value);
            return;
        }
    if (setting_count == SETTING_LIMIT) return;
    snprintf(setting_names[setting_count], SETTING_TEXT, "%s", name);
    snprintf(setting_values[setting_count], SETTING_TEXT, "%s", value);
    setting_count++;
}

void port_mods_start(void) {
    for (int i = 0; i < mod_count; ++i)
        if (mods[i]->start) mods[i]->start();
}

int port_mods_event(const void *event) {
    for (int i = 0; i < mod_count; ++i)
        if (mods[i]->event && mods[i]->event(event)) return 1;
    return 0;
}

void port_mods_field(void) {
    for (int i = 0; i < mod_count; ++i)
        if (mods[i]->field) mods[i]->field();
}

void port_mods_frame_enter(void) {
    for (int i = 0; i < mod_count; ++i)
        if (mods[i]->frame_enter) mods[i]->frame_enter();
}

void port_mods_frame_drawn(void) {
    for (int i = 0; i < mod_count; ++i)
        if (mods[i]->frame_drawn) mods[i]->frame_drawn();
}

void port_mods_frame_exit(void) {
    for (int i = 0; i < mod_count; ++i)
        if (mods[i]->frame_exit) mods[i]->frame_exit();
}

void port_mods_overlay(void) {
    for (int i = 0; i < mod_count; ++i)
        if (mods[i]->overlay) mods[i]->overlay();
}

void port_mods_present(int target, int width, int height) {
    for (int i = 0; i < mod_count; ++i)
        if (mods[i]->present) mods[i]->present(target, width, height);
}

void port_mods_input(struct port_input_state *state) {
    for (int i = 0; i < mod_count; ++i)
        if (mods[i]->input) mods[i]->input(state);
}

void port_mods_audio(const short *samples, int frames) {
    for (int i = 0; i < mod_count; ++i)
        if (mods[i]->audio) mods[i]->audio(samples, frames);
}

void port_mods_host(void) {
    for (int i = 0; i < mod_count; ++i)
        if (mods[i]->host) mods[i]->host();
}

void port_mod_command(const char *name, PortCommandRun run, const char *help) {
    if (command_count == COMMAND_LIMIT) return;
    command_names[command_count] = name;
    command_helps[command_count] = help;
    command_runs[command_count] = run;
    command_count++;
}

void port_mod_panel(const char *name, PortPanelDraw draw) {
    if (panel_count == PANEL_LIMIT) return;
    panel_names[panel_count] = name;
    panel_draws[panel_count] = draw;
    panel_count++;
}

int port_mods_command(const char *name, const char *const *keys, const char *const *values, int count, char *reply,
                      int size) {
    for (int i = 0; i < command_count; ++i)
        if (!strcmp(command_names[i], name)) return command_runs[i](keys, values, count, reply, size);
    return -1;
}

int port_mods_command_count(void) { return command_count; }

const char *port_mods_command_name(int index) { return command_names[index]; }

const char *port_mods_command_help(int index) { return command_helps[index]; }

int port_mods_panel_count(void) { return panel_count; }

const char *port_mods_panel_name(int index) { return panel_names[index]; }

void port_mods_panel_draw(int index) { panel_draws[index](); }
