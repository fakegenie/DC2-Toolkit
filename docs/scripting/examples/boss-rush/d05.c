#pragma target script map/d/d05/d05_1.c
#pragma target script map/d/d05/d05_2.c
#pragma target script map/d/d05/d05_3.c
#pragma target script map/d/d05/d05_4.c
#pragma target script map/d/d05/d05_5.c

#include <event.h>
#include <pal.h>
#include <disc.h>

PROGRAM(boss_gaspard_2, enter_gaspard_room);

void enter_gaspard_room(void)
{
    init_event();
    fade_out(0);
    WAIT();
    skip_next_bgm_load();
    load_dungeon_map("d05b01");
    load_event_script("d05b01/d05b01.stb", boss_gaspard_2);
    finish_event();
}
