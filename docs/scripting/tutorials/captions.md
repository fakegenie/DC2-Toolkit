# Show live captions during a fight

This guide shows how to display a new caption every five seconds during an active battle without pausing player control.

This pattern combines four engine rules explained in [How scripts run](../../internals/running.md):

1. [A map event that never executes `WAIT();` finishes in one frame without freezing the player](../../internals/running.md#an-event-that-never-waits-costs-nothing).
2. [Message window `0` stays on screen in dungeon mode after the event returns](../../internals/running.md#message-window-0-stays-on-screen).
3. [Monster AI scripts run every frame during combat and can start map programs with `run_event_script`](../../internals/running.md#monsters-can-start-map-events).
4. [Map and monster scripts share save flags and the floor timer](../../internals/running.md#shared-state).

> **Tip: `modkit` helpers**
> `sdk/modkit/` includes ready-made helpers for this pattern: `modkit_caption` in `"modkit/text.c"` for the map script and `modkit_tick` in `"modkit/tick.c"` for the monster script (see [`modkit`](../language.md#modkit)). The example below calls the underlying natives directly to show how each step works.

## 1. Declare shared IDs in a header

Create `mods/captions/captions.h` so the map patch and monster patch share the same allocated flag and program numbers:

```c
// mods/captions/captions.h
NEW_FLAG(CAPTIONS_ON);          // Set while the battle is active
NEW_PROGRAM(SHOW_NEXT_CAPTION); // Map program started by the monster timer
```

## 2. Write the map patch

Create `mods/captions/s51.c` targeting map `s51`:

```c
// mods/captions/s51.c
#pragma target map s51
#include <event.h>
#include "captions.h"

int caption;

PROGRAM(SHOW_NEXT_CAPTION, show_next_caption);

// Updates message window 0 and returns without executing WAIT();.
void show_next_caption(void)
{
    caption = caption + 1;
    set_message_window_mode(0, MES_WIN_VERSATILE_1);
    set_message_prompt(0, 0);
    if (caption == 1) {
        show_message(0, "The first caption");
    }
    if (caption == 2) {
        show_message(0, "The second caption");
    }
    if (caption <= 2) {
        show_message(0, -1);
        set_message_slot(0, 8);
        place_message_bubble(0, 0, 0, 0, 0);
    }
    if (caption == 3) {
        close_message(0);
        set_message_prompt(0, 1);
    }
}

#pragma override
void start_monica_gear_battle(void)
{
    original();
    set_flag(CAPTIONS_ON, 1);
    reset_dungeon_timer();
    caption = 0;
    show_next_caption();
}

#pragma override
void start_georama_with_starter_items(void)
{
    set_flag(CAPTIONS_ON, 0);
    close_message(0);
    original();
}
```

## 3. Write the monster patch

Because map scripts do not run in the background during active combat, the monsters in the fight drive the timer. Create `mods/captions/soldier.c` targeting `MONSTER_GRIFFON_SOLDIER_523` (`523`) with `#pragma every_frame` so `tick_captions` runs once per frame before every `WAIT();`:

```c
// mods/captions/soldier.c
#pragma target monster 523
#include <monster.h>
#include "captions.h"

// Triggers SHOW_NEXT_CAPTION every 125 frames (5 seconds at 25 FPS on PAL).
#pragma every_frame
void tick_captions(void)
{
    if (get_dungeon_flag(CAPTIONS_ON)) {
        if (get_floor_timer() >= 125) {
            reset_floor_timer();
            run_event_script(SHOW_NEXT_CAPTION);
        }
    }
}
```

Because `get_floor_timer()` is shared across all monsters on the floor, only the first monster to observe `get_floor_timer() >= 125` resets the timer and starts `SHOW_NEXT_CAPTION`.

## 4. Add `mod.json` and test in-game

Create `mods/captions/mod.json` with `"dev": {"start_at": "s51:story_50"}`:

```json
{
  "format": 2,
  "id": "you.captions",
  "version": "1.0.0",
  "name": "Battle Captions",
  "dev": {"start_at": "s51:story_50"}
}
```

Validate and build:

```sh
dc2tk mod check extracted/scripts captions
dc2tk build extracted/scripts dc2.iso -o test.iso --mod captions --dev --play
```
