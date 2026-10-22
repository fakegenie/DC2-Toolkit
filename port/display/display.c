#include "port_mod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *chosen(const char *name, const char *fallback) {
    char key[96];
    snprintf(key, sizeof key, "dc2tk.display.%s", name);
    const char *value = port_setting(key);
    return value ? value : fallback;
}

static int on(const char *name) {
    return !strcmp(chosen(name, "false"), "true");
}

static void start(void) {
    int width = 0, height = 0;
    if (sscanf(chosen("window", ""), "%dx%d", &width, &height) != 2) width = height = 0;
    render_set_window(width, height, on("fullscreen"));
    render_set_vsync(on("vsync"));
    const char *quality = chosen("quality", "auto");
    render_set_quality(!strcmp(quality, "auto") ? RENDER_SCALE_AUTO : atoi(quality), atoi(chosen("msaa", "1")));
    render_set_filter(!strcmp(chosen("filter", "smooth"), "sharp"));
    int numerator = 0, denominator = 0;
    if (sscanf(chosen("aspect", "4:3"), "%d:%d", &numerator, &denominator) == 2) render_set_aspect(numerator, denominator);
}

static const PortMod display = {.id = "dc2tk.display", .start = start};
PORT_MOD(display)
