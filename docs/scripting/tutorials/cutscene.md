# Add a cutscene to a town

This guide shows how to add a one-time welcome cutscene to Sindain (`g01`) using a `#pragma override` patch and `mod.json`. For background on retail cutscene structure, see [Cutscenes](../systems.md#cutscenes).

## 1. Inspect the map's entry program (Before)

Run `dc2tk map` to inspect Sindain (`g01`):

```sh
dc2tk map extracted/scripts g01
```

In `extracted/scripts/map/g/g01_sindain/g01_1.c`, `map_start` (`100`) runs `play_map_start_arrival`:

```c
// extracted/scripts/map/g/g01_sindain/g01_1.c
PROGRAM(setup, setup_georama_editing_by_flag);
PROGRAM(map_start, play_map_start_arrival);
```

## 2. Write the cutscene patch (After)

Generate a patch for `play_map_start_arrival` on map `g01`:

```sh
dc2tk mod override extracted/scripts sindain-hello --map g01 play_map_start_arrival
```

Edit `mods/sindain-hello/g01.c` to allocate a save flag (`NEW_FLAG`), run the original `play_map_start_arrival` handler, and play a short welcome cutscene the first time the player enters Sindain:

```c
// mods/sindain-hello/g01.c
#pragma target map g01
#include <event.h>
#include <pal.h>
#include <disc.h>
#include "modkit/text.c"

NEW_FLAG(SINDAIN_HELLO_SEEN);

static void wait_for_actor(int actor)
{
    while (check_actor_busy(actor)) {
        WAIT();
    }
}

static void play_sindain_hello(void)
{
    float x, y, z;

    if (get_flag(SINDAIN_HELLO_SEEN)) {
        return;
    }
    set_flag(SINDAIN_HELLO_SEEN, 1);

    init_event();
    init_chara_actor(0, CHARA_PLAYER);

    get_chara_pos(CHARA_PLAYER, &x, &y, &z);
    cut_camera(x + 120.0, y + 60.0, z + 120.0, x, y + 20.0, z);
    move_camera_to(x + 60.0, y + 30.0, z + 80.0, x, y + 20.0, z, 60);
    play_actor_motion_then_loop(0, MOTION_WAVE, MOTION_STAND_2);
    wait_for_actor(0);

    modkit_say("Sindain looks busier today.");

    reset_camera(CAMERA_RESET_ACTIVE);
    finish_event();
}

#pragma override
int play_map_start_arrival(void)
{
    int result;

    result = original();
    play_sindain_hello();
    return result;
}
```

`play_sindain_hello` follows the standard cutscene lifecycle:
1. Check and set `SINDAIN_HELLO_SEEN` so the cutscene plays only once.
2. Call `init_event()` and bind actor `0` to `CHARA_PLAYER`.
3. Queue the camera movement and wave animation, then wait until actor `0` finishes.
4. Show the dialogue line with `modkit_say`.
5. Restore the gameplay camera with `reset_camera(CAMERA_RESET_ACTIVE)` and exit event mode with `finish_event()`.

## 3. Add `mod.json` and test in-game

Create `mods/sindain-hello/mod.json` with `"dev": {"start_at": "g01"}` so **New Game** spawns directly in Sindain:

```json
{
  "format": 2,
  "id": "you.sindain-hello",
  "version": "1.0.0",
  "name": "Sindain Hello",
  "dev": {"start_at": "g01"}
}
```

Validate and build:

```sh
dc2tk mod check extracted/scripts you.sindain-hello
dc2tk build extracted/scripts dc2.iso -o test.iso --mod you.sindain-hello --dev --play
```

> **Tip: Custom characters, scene skipping, and warps**
> - **Triggering from a warp or map point**: Declare `NEW_PROGRAM(HELLO_SCENE);` and `PROGRAM(HELLO_SCENE, play_sindain_hello);`, then warp to it with `jump_to_map(-1, "g01", HELLO_SCENE)` or set `"dev": {"start_at": "g01:HELLO_SCENE"}` in `mod.json` and build with `--dev`.
> - **Adding NPCs**: Load `.chr` models into unused scene slots with `load_inactive_chara` or `load_scene_chara`, bind actors to those slots, and call `delete_chara` before `finish_event()`.
> - **Skipping with Start**: Wrap the camera and actor sequence between `enable_scene_skip();` and `SKIP_END(); disable_scene_skip();`. Keep all `set_flag`, `delete_chara`, and `finish_event` calls outside the skippable block.
> - **Complete example**: See [`spinning-cult`](../examples/spinning-cult/README.md) for a multi-scene quest mod with custom Blender models and map triggers.
