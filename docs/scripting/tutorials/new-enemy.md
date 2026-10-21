# Add a new enemy to a fight

This guide shows how to spawn extra or replacement monsters in a scripted battle using a `#pragma target map` patch and `mod.json`. For background on how scripted battles work, see [Story fights and boss arenas](../systems.md#story-fights-and-boss-arenas).

## Approaches for adding or replacing enemies

| Goal | Method |
| --- | --- |
| Add an extra monster to a fight | Call `load_monster_data(kind, 0)` and `place_monster(kind, x, y, z, rx, ry, rz)` after `original()`. |
| Replace the monsters in a scripted fight | Override `start_castle_battle` / `start_monica_gear_battle` (or wrap `load_monster_data` and `place_monster` with `#pragma override <native>`). |
| Create a custom enemy with unique AI | Repurpose an event-only monster kind (ID `500`+, which has its own `b*` AI script) and edit its AI script and `data:monsters` entry. |

## 1. Inspect the original battle setup (Before)

Run `dc2tk xref` on monster kind `523` (`MONSTER_GRIFFON_SOLDIER_523`, the castle prologue soldier):

```sh
dc2tk xref extracted/scripts monster 523
```

Map `s51` (`extracted/scripts/map/s/s51_raybrandt_castle/s51_1.c` and its included library `lib/map/s/s51_raybrandt_castle/monica_gear_battle.c`) spawns four Griffon Soldiers in `start_castle_battle` (program `500`) and `start_monica_gear_battle` (program `story_50`):

```c
// extracted/scripts/map/s/s51_raybrandt_castle/s51_1.c
PROGRAM(500, start_castle_battle);
PROGRAM(510, start_georama_with_starter_items);
PROGRAM(player_dead, trigger_silent_game_over);
PROGRAM(debug_test, on_debug_test);
PROGRAM(story_50, start_monica_gear_battle);
MAIN(do_nothing);

// extracted/scripts/lib/map/s/s51_raybrandt_castle/monica_gear_battle.c
void start_monica_gear_battle(void)
{
    add_item(ITEM_ROYAL_SWORD);
    add_item(ITEM_PRINCESS_ORB);
    add_item(ITEM_PRINCESS_BOOTS);
    add_item(ITEM_PRINCESS_DRESS);
    equip_chara_item(USER_CHARA_MONICA, ITEM_ROYAL_SWORD);
    equip_chara_item(USER_CHARA_MONICA, ITEM_PRINCESS_ORB);
    equip_chara_item(USER_CHARA_MONICA, ITEM_PRINCESS_BOOTS);
    equip_chara_item(USER_CHARA_MONICA, ITEM_PRINCESS_DRESS);
    change_player_chara(USER_CHARA_MONICA);
    play_event_50();
    set_dungeon_pause(DNG_PAUSE_BATTLE_MUSIC, 1);
    load_monster_data(MONSTER_GRIFFON_SOLDIER_523, 1);
    place_monster(MONSTER_GRIFFON_SOLDIER_523, -382.5, -0.2, 120.2, 0.0, 1.61, 0.0);
    place_monster(MONSTER_GRIFFON_SOLDIER_523, -428.0, -0.2, 202.7, 0.0, 1.71, 0.0);
    place_monster(MONSTER_GRIFFON_SOLDIER_523, -452.5, -0.2, 287.0, 0.0, 1.98, 0.0);
    place_monster(MONSTER_GRIFFON_SOLDIER_523, -324.2, -0.2, 260.5, 0.0, 2.34, 0.0);
    WAIT();
    fade_in(20);
    finish_event();
}
```

## 2. Write the `#pragma override` patch (After)

Generate a patch for map `s51`:

```sh
dc2tk mod override extracted/scripts castle --map s51 start_monica_gear_battle
```

### Option A: Spawn an extra monster alongside the existing enemies

Pass `0` as the second argument to `load_monster_data` so the engine keeps the already-loaded Griffon Soldiers in memory, then call `place_monster` with the new monster's position `(x, y, z)` and rotation `(rx, ry, rz)`:

```c
// mods/castle/s51.c
#pragma target map s51
#include <event.h>
#include <pal.h>
#include <disc.h>

#pragma override
void start_monica_gear_battle(void)
{
    add_item(ITEM_ROYAL_SWORD);
    add_item(ITEM_PRINCESS_ORB);
    add_item(ITEM_PRINCESS_BOOTS);
    add_item(ITEM_PRINCESS_DRESS);
    equip_chara_item(USER_CHARA_MONICA, ITEM_ROYAL_SWORD);
    equip_chara_item(USER_CHARA_MONICA, ITEM_PRINCESS_ORB);
    equip_chara_item(USER_CHARA_MONICA, ITEM_PRINCESS_BOOTS);
    equip_chara_item(USER_CHARA_MONICA, ITEM_PRINCESS_DRESS);
    change_player_chara(USER_CHARA_MONICA);
    play_event_50();
    set_dungeon_pause(DNG_PAUSE_BATTLE_MUSIC, 1);
    load_monster_data(MONSTER_GRIFFON_SOLDIER_523, 1);
    place_monster(MONSTER_GRIFFON_SOLDIER_523, -382.5, -0.2, 120.2, 0.0, 1.61, 0.0);
    place_monster(MONSTER_GRIFFON_SOLDIER_523, -428.0, -0.2, 202.7, 0.0, 1.71, 0.0);
    place_monster(MONSTER_GRIFFON_SOLDIER_523, -452.5, -0.2, 287.0, 0.0, 1.98, 0.0);
    place_monster(MONSTER_GRIFFON_SOLDIER_523, -324.2, -0.2, 260.5, 0.0, 2.34, 0.0);
    load_monster_data(MONSTER_SEWER_RAT, 0);
    place_monster(MONSTER_SEWER_RAT, -360.0, -0.2, 180.0, 0.0, 1.61, 0.0);
    WAIT();
    fade_in(20);
    finish_event();
}
```

### Option B: Replace all soldiers with Golems and wire the victory trigger

In the retail fight, `b254a_griffon_soldier.c` counts deaths in shared monster variable `127` and calls `run_event_script(510)` when the fourth soldier dies. Because `e33a_golem.c` does not call `run_event_script(510)`, register `PROGRAM(monsters_cleared, start_georama_with_starter_items);` (`1500`) in the patch so the engine runs the post-fight event automatically when all monsters on the floor die:

```c
// mods/castle/s51.c
#pragma target map s51
#include <event.h>
#include <pal.h>
#include <disc.h>

// Run program 510's handler automatically when the last monster on the floor dies.
PROGRAM(monsters_cleared, start_georama_with_starter_items);

#pragma override
void start_monica_gear_battle(void)
{
    add_item(ITEM_ROYAL_SWORD);
    add_item(ITEM_PRINCESS_ORB);
    add_item(ITEM_PRINCESS_BOOTS);
    add_item(ITEM_PRINCESS_DRESS);
    equip_chara_item(USER_CHARA_MONICA, ITEM_ROYAL_SWORD);
    equip_chara_item(USER_CHARA_MONICA, ITEM_PRINCESS_ORB);
    equip_chara_item(USER_CHARA_MONICA, ITEM_PRINCESS_BOOTS);
    equip_chara_item(USER_CHARA_MONICA, ITEM_PRINCESS_DRESS);
    change_player_chara(USER_CHARA_MONICA);
    play_event_50();
    set_dungeon_pause(DNG_PAUSE_BATTLE_MUSIC, 1);
    load_monster_data(MONSTER_GOLEM, 1);
    place_monster(MONSTER_GOLEM, -382.5, -0.2, 120.2, 0.0, 1.61, 0.0);
    place_monster(MONSTER_GOLEM, -428.0, -0.2, 202.7, 0.0, 1.71, 0.0);
    place_monster(MONSTER_GOLEM, -452.5, -0.2, 287.0, 0.0, 1.98, 0.0);
    place_monster(MONSTER_GOLEM, -324.2, -0.2, 260.5, 0.0, 2.34, 0.0);
    WAIT();
    fade_in(20);
    finish_event();
}
```

> **Tip:** You can also swap a monster across every function in `s51` (`start_castle_battle` and `start_monica_gear_battle`) at once by overriding the `load_monster_data` and `place_monster` natives with `#pragma override load_monster_data` and `#pragma override place_monster` (see [Creating Mods](../../guide/mods.md#patches)).

## 3. Add `mod.json` and test in-game

Create `mods/castle/mod.json` with `"dev": {"start_at": "s51:story_50"}` to jump directly into `start_monica_gear_battle` when selecting **New Game**:

```json
{
  "format": 2,
  "id": "you.castle",
  "version": "1.0.0",
  "name": "Castle Battle",
  "dev": {"start_at": "s51:story_50"}
}
```

Validate and build:

```sh
dc2tk mod check extracted/scripts castle
dc2tk build extracted/scripts dc2.iso -o test.iso --mod castle --dev --play
```
