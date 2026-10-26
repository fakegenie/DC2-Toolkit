# How Scripts Work & Lifecycle

DC2 runs its gameplay logic through bytecode scripts. `dc2tk setup` decompiles all ~2,500 scripts on the disc into C files in `extracted/scripts/`.

## The big picture

The game has two parts:

- **The engine** (the PS2 executable) renders 3D graphics, runs physics and collision, plays animations and audio, and stores static data tables.
- **The scripts** (`.stb` files, decompiled to `.c`) control world events: cutscenes, NPC dialogue, doors, chests, monster AI, player weapon combos, and visual effects.

Scripts call the engine through ~1,100 built-in functions called **natives** (such as `place_monster` or `show_message`). The engine calls scripts by starting numbered entry points called **programs** when an event occurs.

| Script Type | Folder | SDK Header | Controls |
| --- | --- | --- | --- |
| **Map & Event** | `map/` | [`<event.h>`](api/event.md) | Towns, dungeons, cutscenes, NPCs, doors, and chests |
| **Monster AI** | `dungeon/monster/` | [`<monster.h>`](api/monster.md) | Movement, attacks, reactions, and boss phases for one monster |
| **Player Action** | `mainchr/`, `dungeon/act_script/` | [`<action.h>`](api/action.md) | Max, Monica, monster transformations, and the Ridepod |
| **Effect** | `dungeon/eff_script/` | [`<effect.h>`](api/effect.md) | Projectiles, shockwaves, beams, and hit sparks |
| **Argument** | `event/` | [`<arg.h>`](api/arg.md) | Small parameter scripts loaded by a few scenes |

## Anatomy of a script

Every script file has the same structure:

```c
#include <event.h>          // 1. SDK header for this script type
#include <pal.h>            //    Named save flags, music, and sound IDs
#include <disc.h>           //    Named monsters, items, NPCs, and motions
#include "modkit/text.c"    // 2. Shared helper library

int talk_count;             // 3. Global variable (reset to 0 when the script loads)

PROGRAM(map_start, on_enter);               // 4. Entry points the engine can start
PROGRAM(talk_or_dungeon_start, on_talk);
MAIN(on_enter);

void on_enter(void)
{
    // Runs when the player enters the map
}

void on_talk(void)
{
    talk_count = talk_count + 1;
    modkit_say("Hello!");   // Opens a text box and waits for the player to press confirm
}
```

## What a `PROGRAM` is

A standard C program has one `main()` function. A DC2 script has a **program table** declared with `PROGRAM(id, function)` lines.

Each `PROGRAM` maps an event number (or enum name from the SDK header) to a C function. Functions only run when the engine or another script starts one of the script's programs:

- **Town or story map (`<event.h>`)**:
  - `map_start` (`100`): Runs when the player enters the map.
  - `gate` (`300`) / `save_point` (`400`): Runs when the player steps into a time gate or save point.
  - `talk_or_dungeon_start` (`1000`): Runs when the player presses confirm next to an NPC.
  - Numbered programs (`500`, `510`, `11000`, etc.): Cutscenes, doors, or search spots triggered by map points or warps.
- **Dungeon (`<event.h>`)**:
  - `floor_start` (`1010`): Runs when a dungeon floor loads.
  - `treasure_chest` (`1100`): Runs when the player opens a chest.
  - `monsters_cleared` (`1500`): Runs once when the last monster on the floor dies.
- **Monster AI (`<monster.h>`)**:
  - `init` (`100`): Runs once when the monster spawns.
  - `update` (`200`): Runs in a loop every frame while the monster is alive.
  - `damage` (`500`) / `guard` (`700`) / `death` (`1000`): Runs when the monster takes damage, guards, or dies.

Every script also has a `MAIN(function)` line required by the binary header. When a script declares global or `static` variables with initializers (`int count = 3;`), the compiler places that initialization inside `MAIN` and runs it before the first program starts.

## Script lifecycle

### 1. Load
- **Map scripts**: Load when the player enters a map. Only one map script is loaded at a time.
- **Monster scripts**: Load when a monster spawns on a floor. Each active monster runs its own script instance.
- All **global variables** start at `0` when a script loads.

### 2. Run and yield (`WAIT();`)
Active programs run as **coroutines**. A program runs until it executes `WAIT();` or returns:

```text
Frame 1                 Frame 2                 Frame 3
[Script runs...]        [Script resumes...]     [Script resumes...]
  set_motion(...);        check_done();           finish_event();
  WAIT();  ------------->   WAIT();  ------------->   return;
         [Engine draws]          [Engine draws]
```

- Native functions do not block. For example, `set_actor_motion` starts an animation and returns immediately.
- `WAIT();` pauses the script for the rest of the current frame so the engine can render graphics, update physics, and advance animations.
- On the next frame (25 frames per second on PAL), the script resumes after `WAIT();`.
- To pause for 1 second (25 frames), run `WAIT();` in a 25-iteration loop or call `modkit_wait(25)`.

### 3. Event mode vs. gameplay
- **Programs that execute `WAIT();` pause the player**: When a map program executes `WAIT();`, the engine enters **event mode** until that program returns. The player cannot move or attack during event mode.
- **Programs without `WAIT();` run in one frame**: If a map program returns without executing `WAIT();`, the engine runs the entire function in a single frame and does not enter event mode.
- **Monsters can start map programs**: A monster AI script can call `run_event_script(program_number)` during combat to start a program in the active map script (for example, to end a scripted fight when the last enemy dies).

### 4. Unload
- When the player leaves the map, the map script unloads and its global variables reset.
- When a monster finishes its `death` program, its script instance ends.

## Where state lives

Choose storage based on how long the value must persist:

| Storage | Lifetime | Read / Write | Typical Use |
| --- | --- | --- | --- |
| **Local variable** | One function call | Function-scope C variable | Loop counters, temporary coordinates |
| **Global variable** | Until the player leaves the map | File-scope C variable (`int visited;`) | State within a single map visit |
| **Monster variable (`0`–`7`)** | Until that monster dies | `get_monster_var` / `set_monster_var` | Per-monster state or phase |
| **Shared monster variable (`8`–`135`)** | Until the player leaves the floor | `get_monster_var` / `set_monster_var` | Floor-wide kill counters shared by all monsters |
| **Save flag (`0`–`2047`)** | Saved to the memory card | `get_flag` / `set_flag` (map)<br>`get_dungeon_flag` / `set_dungeon_flag` (monster) | Story progress (flags `825`–`2046` are unused by the base game) |
| **Save counter (`0`–`127`)** | Saved to the memory card | `get_counter` / `set_counter` | Multi-step quest values (`-32768` to `32767`; counters `26`–`127` are unused) |

In a mod, declare `NEW_FLAG(NAME)`, `NEW_COUNTER(NAME)`, `NEW_PROGRAM(NAME)`, or `NEW_MONSTER_VAR(NAME)` instead of hardcoding numbers. `dc2tk build` assigns a free ID automatically.

## Script files and languages

```text
extracted/scripts/
├── map/
│   ├── g/   Georama towns (g01_sindain, ...)
│   ├── m/   Palm Brinks exterior maps (m01_palm_brinks, ...)
│   ├── i/   Palm Brinks interiors (shops, houses, station)
│   ├── f/   Future towns (f01_jurak_mall, ...)
│   ├── s/   Story maps and cutscenes (s51_raybrandt_castle, ...)
│   └── d/   Dungeons (d01 to d05), boss arenas (b01, ...), entrances (e01, ...)
├── dungeon/
│   ├── monster/      Monster and boss AI scripts (e33a_golem.c, ...)
│   ├── eff_script/   Visual effect scripts
│   └── act_script/   Monster transforms and Ridepod action scripts
├── mainchr/          Max (c01_max.c) and Monica (c02_monica.c) action scripts
├── lib/              Shared code included by scripts with #include "lib/..."
└── include/disc.h    Constants extracted from your disc (monsters, items, motions)
```

Each map script has **five language files**: `_1.c` (English), `_2.c` (French), `_3.c` (German), `_4.c` (Italian), and `_5.c` (Spanish). Unnumbered files (`g01.c`) are unused Japanese scripts. When you write a mod patch with `#pragma target map g01`, `dc2tk build` applies your change to all five language files.

## Next steps

- **[Map, Monster, and Player Scripts](systems.md)**: How towns, cutscenes, dungeons, monster AI, and player combos work.
- **[C Language, SDK, and Patches](language.md)**: Supported C features, `modkit` helpers, patching scripts, and editor setup.
- **[Scripting Tutorials](tutorials/README.md)**: Step-by-step guides with complete C patches.
- **[API Reference](api/README.md)**: All native functions, helpers, program numbers, and constants.
