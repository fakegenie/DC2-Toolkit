#pragma target map d04b01

#include <event.h>
#include <pal.h>
#include <disc.h>
#include "rush.h"

#pragma override
void start_boss_battle(void)
{
    original();
    rush_clock();
}

#pragma override
int end_boss_to_edit_mode(void)
{
    if (!rush_won()) {
        return original();
    }
    return 0;
}

#pragma override
int run_boss_game_over(void)
{
    rush_lost();
    return original();
}
