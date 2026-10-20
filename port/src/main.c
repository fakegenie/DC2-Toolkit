#include "cdvd.h"
#include "port_kernel.h"
#include "split_data.h"
#include "port_present.h"
#include "executable.h"
#include "audio.h"
#include "port_debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int port_game_main(void);
extern int port_debug;

int main(int argc, char **argv) {
    int first = 1;
    for (; first < argc - 1; ++first) {
        if (!strcmp(argv[first], "--debug")) port_debug = 1;
        else if (strcmp(argv[first], "--graphics")) break;
    }
    if (first != argc - 1) {
        fprintf(stderr, "usage: %s [--debug] DISC.iso\n", argv[0]);
        return 1;
    }
    if (!port_cdvd_init(argv[first]) || !port_executable_check() || !port_split_data_init()) return 1;
    if (!port_present_init(1)) return 1;
    atexit(port_present_close);
    if (!audio_init(1)) return 1;
    atexit(audio_shutdown);
    debug_start();
    return port_kernel_run(port_game_main);
}
