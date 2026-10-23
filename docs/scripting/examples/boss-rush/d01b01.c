#pragma target map d01b01

#include <event.h>
#include <pal.h>
#include <disc.h>
#include "rush.h"

#pragma override
void setup_halloween_boss_battle(void)
{
    original();
    rush_clock();
}

#pragma override
void open_exit_and_play_halloween_defeat(void)
{
    if (!rush_won()) {
        original();
    }
}

#pragma override
void trigger_silent_game_over(void)
{
    rush_lost();
    original();
}
