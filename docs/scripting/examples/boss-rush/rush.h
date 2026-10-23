#include <event.h>

NEW_FLAG(RUSH_ON);
NEW_COUNTER(RUSH_STAGE);
NEW_COUNTER(RUSH_START_HIGH);
NEW_COUNTER(RUSH_START_LOW);
NEW_COUNTER(RUSH_BEST_SECONDS);
NEW_COUNTER(RUSH_BEST_HUNDREDTHS);
NEW_PROGRAM(RUSH_START);

#define PALM_BRINKS 10
#define FRAMES_PER_SECOND 50
#define TIME_SPLIT 30000

void rush_begin(void)
{
    int now;

    get_save_value(SAVE_GET_PLAY_TIME, &now);
    set_counter(RUSH_START_HIGH, now / TIME_SPLIT);
    set_counter(RUSH_START_LOW, now % TIME_SPLIT);
    set_flag(RUSH_ON, 1);
}

int rush_frames(void)
{
    int now;

    get_save_value(SAVE_GET_PLAY_TIME, &now);
    return now - get_counter(RUSH_START_HIGH) * TIME_SPLIT - get_counter(RUSH_START_LOW);
}

void rush_clock(void)
{
    if (get_flag(RUSH_ON)) {
        run_stopwatch(STOPWATCH_LAYOUT, 32, 364, 0);
        run_stopwatch(STOPWATCH_START);
    }
}

void rush_stop_clock(void)
{
    int minutes;
    int seconds;
    int hundredths;
    int in_time;

    run_stopwatch(STOPWATCH_STOP, &minutes, &seconds, &hundredths, &in_time);
}

void rush_enter(int stage)
{
    set_counter(RUSH_STAGE, stage);
    if (stage == 0) {
        go_to_dungeon(0, 7, boss_halloween);
    } else if (stage == 1) {
        go_to_dungeon(2, 18, boss_memo_eater);
    } else if (stage == 2) {
        go_to_dungeon(2, 23, boss_gaspard_and_fire);
    } else if (stage == 3) {
        go_to_dungeon(3, 20, boss_jaming_and_shingala);
    } else {
        go_to_dungeon(4, 22, boss_gaspard_2);
    }
}

void rush_fade_out(void)
{
    init_event();
    play_default_motion(0);
    fade_out_bgm(30);
    fade_out(30);
    wait_frames(30);
    clear_monsters(-1);
}

int rush_won(void)
{
    if (!get_flag(RUSH_ON)) {
        return 0;
    }
    rush_fade_out();
    rush_enter(get_counter(RUSH_STAGE) + 1);
    return 1;
}

void rush_lost(void)
{
    set_flag(RUSH_ON, 0);
    rush_stop_clock();
}
