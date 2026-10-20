#include "cdvd.h"
#include "port_kernel.h"
#include "split_data.h"
#include "port_present.h"
#include "executable.h"
#include "iop_sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PORT_GRAPHICS
#include "gs.h"
#endif

extern int port_game_main(void);

int main(int argc, char **argv) {
    int graphics = argc > 1 && !strcmp(argv[1], "--graphics");
    if (argc != 2 + graphics) {
        fprintf(stderr, "usage: %s [--graphics] DISC.iso\n", argv[0]);
        return 1;
    }
    if (!port_cdvd_init(argv[1 + graphics]) || !port_executable_check() || !port_split_data_init()) return 1;
#ifdef PORT_GRAPHICS
    port_gs_reset();
#endif
    if (!port_present_init(graphics)) return 1;
    atexit(port_present_close);
    if (!port_sound_init(graphics)) return 1;
    atexit(port_sound_shutdown);
    return port_kernel_run(port_game_main);
}
