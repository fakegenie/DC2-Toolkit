#pragma target map s18

#include <event.h>
#include <pal.h>
#include <disc.h>
#include "rush.h"

#pragma override
void start_monster_68_fight(void)
{
    original();
    rush_clock();
}

#pragma override
void resume_floor_after_event(void)
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
