#pragma target map m01

#include <event.h>
#include <pal.h>
#include <disc.h>
#include "rush.h"

PROGRAM(RUSH_START, start_rush);

void start_rush(void)
{
    init_event();
    fade_out(30);
    wait_frames(30);
    set_active_chara(0);
    rush_begin();
    rush_enter(0);
}

#pragma override
void enter_d01_if_confirmed(void)
{
    int choice;

    init_event();
    set_chara_motion(CHARA_PLAYER, MOTION_STAND_2);
    WAIT();
    apply_message_preset(0, MES_PRESET_WINDOW);
    set_message_first_choice(0, 0);
    set_message_option(0, MES_OPT_LINE_INDENT, 1);
    show_message(0, "Underground Channel\nBoss Rush\nStay in town");
    choice = ask_question_at(0, -1, 5, 512 / 2, 416 / 2, 3, 0);
    set_message_option(0, MES_OPT_LINE_INDENT, 0);
    set_message_window_mode(0, MES_WIN_FUKIDASHI);
    if (choice == 0) {
        fade_out(30);
        wait_frames(30);
        jump_to_map(-1, "d01e01", map_start);
    } else if (choice == 1) {
        start_rush();
    }
}
