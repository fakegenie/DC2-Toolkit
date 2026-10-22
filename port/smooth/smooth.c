#include "port_mod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *chosen(const char *name, const char *fallback) {
    char key[96];
    snprintf(key, sizeof key, "dc2tk.smooth.%s", name);
    const char *value = port_setting(key);
    return value ? value : fallback;
}

static void start(void) {
    const char *refresh = chosen("refresh", "display");
    smooth_set_rate(!strcmp(refresh, "display") ? 0 : atoi(refresh));
    render_set_smoothing(strcmp(chosen("enabled", "true"), "false") != 0);
}

static const PortMod smooth = {.id = "dc2tk.smooth", .start = start};
PORT_MOD(smooth)
