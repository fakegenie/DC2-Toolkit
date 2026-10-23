#pragma target map d05b01

#include <event.h>
#include <pal.h>
#include <disc.h>
#include "rush.h"

#pragma override
void restart_first_gaspard_fight(void)
{
    original();
    rush_clock();
}

void start_monster_gaspard(void)
{
    set_func_point_enabled("", "e528", 0);
    set_func_point_enabled("d05", "func", 0);
    show_map_piece(0, "d05", "d05ba03-a");
    bind_handle_map_part(0, "d05", "d05b01_1", "takil");
    set_handle_translate(0, -330.0, 0.0, 200.0);
    bind_handle_map_part(0, "d05", "d05b01_1", "takir");
    set_handle_translate(0, 330.0, 0.0, 200.0);
    stop_bgm(0);
    wait_frames(4);
    start_bgm(35);
    load_monster_data(MONSTER_MONSTER_GASPARD, 1);
    place_monster(MONSTER_MONSTER_GASPARD, 0.0, 0.0, -200.0, 0.0, 0.0, 0.0);
    place_chara(CHARA_PLAYER, 0.0, 0.0, 300.0, 3.14);
    set_chara_motion(CHARA_PLAYER, MOTION_STAND_2, CHARA_MOTION_RESTART);
    set_camera_pos(0.0, 40.0, 420.0);
    set_camera_target(0.0, 30.0, 300.0);
    reset_camera(CAMERA_RESET_ACTIVE);
    fade_in(15);
    finish_event();
    rush_clock();
}

#pragma override
void play_backstory_then_warp_to_s49(void)
{
    if (!get_flag(RUSH_ON)) {
        original();
        return;
    }
    rush_fade_out();
    set_counter(RUSH_STAGE, 5);
    start_monster_gaspard();
}

#pragma override
void play_death_scene_then_warp_to_s49(void)
{
    int frames;
    int seconds;
    int hundredths;
    int best;
    int best_hundredths;

    if (!get_flag(RUSH_ON)) {
        original();
        return;
    }
    frames = rush_frames();
    rush_lost();
    rush_fade_out();
    seconds = frames / FRAMES_PER_SECOND;
    hundredths = frames % FRAMES_PER_SECOND * 100 / FRAMES_PER_SECOND;
    best = get_counter(RUSH_BEST_SECONDS);
    best_hundredths = get_counter(RUSH_BEST_HUNDREDTHS);
    apply_message_preset(0, MES_PRESET_WINDOW);
    set_message_window_mode(0, MES_WIN_VERSATILE_1);
    set_message_value(0, 1, seconds / 60);
    set_message_value(0, 2, seconds % 60);
    set_message_value(0, 3, hundredths / 10);
    set_message_value(0, 4, hundredths % 10);
    if (best + best_hundredths == 0 || seconds < best || seconds == best && hundredths < best_hundredths) {
        set_counter(RUSH_BEST_SECONDS, seconds);
        set_counter(RUSH_BEST_HUNDREDTHS, hundredths);
        show_message(0, "Boss Rush cleared!<page>Six bosses in [数値１] min [数値２].[数値３][数値４] sec.\nA new best time!");
    } else {
        set_message_value(0, 5, best / 60);
        set_message_value(0, 6, best % 60);
        set_message_value(0, 7, best_hundredths / 10);
        set_message_value(0, 8, best_hundredths % 10);
        show_message(0, "Boss Rush cleared!<page>Six bosses in [数値１] min [数値２].[数値３][数値４] sec.\nYour best is [数値５] min [数値６].[数値７][数値８] sec.");
    }
    fade_in(30);
    wait_frames(30);
    show_message_at(0, -1, 5, 512 / 2, 416 / 2);
    fade_out(30);
    wait_frames(30);
    go_to_edit_mode(PALM_BRINKS, map_start);
}

#pragma override
int run_boss_game_over(void)
{
    rush_lost();
    return original();
}
