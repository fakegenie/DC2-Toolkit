# Change enemy behavior

This guide shows how to speed up a monster's attack, remove its post-attack recovery pause, and widen its hitbox using a `#pragma override` patch. For background on monster AI scripts, see [Monsters](../systems.md#monsters).

## 1. Inspect the monster AI script (Before)

Run `dc2tk xref` to see which AI script and monster variants use the Golem script:

```sh
dc2tk xref extracted/scripts monster golem
```

`MONSTER_GOLEM` (`168`), `MONSTER_RAVE_GOLEM` (`169`), `MONSTER_ROCK_TASTER` (`170`), and `MONSTER_TITAN` (`171`) all run `extracted/scripts/dungeon/monster/e33a_golem.c`.

In `extracted/scripts/dungeon/monster/e33a_golem.c`, `on_init` registers the attack hitboxes and `chase_and_swing_at_target` runs the close-range swing:

```c
// extracted/scripts/dungeon/monster/e33a_golem.c

// Walks up to the target and plays attack 1, a knockback swing registered in on_init.
void chase_and_swing_at_target(void)
{
    loop_motion(MOTION_WALK);
    chase_target_steps(1.25, 3.0, 62.0 + 4.0, 90);
    turn_to_target_wait(3.0);
    set_motion(MOTION_ATTACK_1_2, -1, CHARA_MOTION_HOLD);
    wait_motion_end_update();
    loop_motion(MOTION_STAND_2);
    wait_frames_update(6);
}

// Registers the attack 1 knockback hit windows, loads the Vanguard shockwave effect, sets a photo
// window on attack 2 for grade 0 monsters and starts the AI.
void on_init(void)
{
    int monster_grade;

    add_attack_hit(HIT_KNOCKBACK_ATTACK, 6.5, MOTION_ATTACK_1_2, 0.3, 0.37, 0);
    add_attack_hit(HIT_BEING_BLOWN_AWAY, 6.5, MOTION_ATTACK_1_2, 0.5, 0.6, 2);
    load_effect(EFFECT_VANGUARD_SHOCKWAVE);
    monster_grade = get_monster_grade(-1);
    if (monster_grade == 0) {
        set_photo_window("攻撃２", 0.227, 0.533, 1009);  // attack 2
    }
    init_monster_ai();
}
```

## 2. Write the `#pragma override` patch (After)

Generate a patch targeting the Golem AI script:

```sh
dc2tk mod override extracted/scripts stronger-golem --monster golem chase_and_swing_at_target on_init
```

Edit `mods/stronger-golem/e33a_golem.c` to double the Golem's approach and turn speed (`1.25` -> `2.5`, `3.0` -> `6.0`), remove the `wait_frames_update(6)` recovery pause after the swing, and widen the hitbox radius (`6.5` -> `10.0`):

```c
// mods/stronger-golem/e33a_golem.c
#pragma target monster golem
#include <monster.h>
#include <pal.h>
#include <disc.h>

// Double the chase and turn speed and remove the 6-frame post-attack recovery pause.
#pragma override
void chase_and_swing_at_target(void)
{
    loop_motion(MOTION_WALK);
    chase_target_steps(2.5, 6.0, 62.0 + 4.0, 90);
    turn_to_target_wait(6.0);
    set_motion(MOTION_ATTACK_1_2, -1, CHARA_MOTION_HOLD);
    wait_motion_end_update();
    loop_motion(MOTION_STAND_2);
}

// Increase the swing hitbox radius from 6.5 to 10.0 and widen the active frame windows.
#pragma override
void on_init(void)
{
    int monster_grade;

    add_attack_hit(HIT_KNOCKBACK_ATTACK, 10.0, MOTION_ATTACK_1_2, 0.25, 0.42, 0);
    add_attack_hit(HIT_BEING_BLOWN_AWAY, 10.0, MOTION_ATTACK_1_2, 0.48, 0.65, 2);
    load_effect(EFFECT_VANGUARD_SHOCKWAVE);
    monster_grade = get_monster_grade(-1);
    if (monster_grade == 0) {
        set_photo_window("攻撃２", 0.227, 0.533, 1009);
    }
    init_monster_ai();
}
```

- **Shared library functions**: Thirty-six monster scripts include `lib/dungeon/monster/monster_ai_full_toolkit.c`. If you `#pragma override` a library function (such as `init_monster_ai`) inside a `#pragma target monster golem` patch, `dc2tk build` replaces that function in `e33a_golem.c` only—other monsters are unaffected.
- **Monster family variants**: If you want your change to apply only to `MONSTER_GOLEM` (`168`) and not to Rave Golem, Rock Taster, or Titan, check `if (get_monster_kind() != MONSTER_GOLEM) { original(); return; }`.

## 3. Add `mod.json` and test in-game

Create `mods/stronger-golem/mod.json`. To test your Golem immediately on **New Game**, pair this mod with the `s51` Golem battle mod from [Add a new enemy to a fight](new-enemy.md) using `"dev": {"start_at": "s51:story_50"}`:

```json
{
  "format": 2,
  "id": "you.stronger-golem",
  "version": "1.0.0",
  "name": "Stronger Golem",
  "dev": {"start_at": "s51:story_50"}
}
```

Validate and build:

```sh
dc2tk mod check extracted/scripts you.stronger-golem
dc2tk build extracted/scripts dc2.iso -o test.iso --mod you.castle --mod you.stronger-golem --dev --play
```

> **Note: Modifying base monster stats (HP, attack, defense, ABS, Gilda, and drops)**
> Base stats and item drops are stored in the `data:monsters` table rather than in `.stb` scripts. Export the table into your mod with:
>
> ```sh
> dc2tk mod asset dc2.iso stronger-golem data:monsters
> ```
>
> Edit `mods/stronger-golem/data/monsters.json` (for example, `"hp"` on entry `{"id": 168, ...}`) and run `dc2tk build` (see [Game Data](../../guide/data.md)).
