#pragma target map d03b02

#include <event.h>
#include <pal.h>
#include <disc.h>
#include "rush.h"

#pragma override
void setup_gaspard_boss_fight(void)
{
    original();
    rush_clock();
}

#pragma override
void leave_floor_to_edit_mode(void)
{
    if (!rush_won()) {
        original();
    }
}

#pragma override
void play_boss_defeat_game_over(void)
{
    rush_lost();
    original();
}

#pragma override
void run_boss_loss_game_over(void)
{
    rush_lost();
    original();
}
