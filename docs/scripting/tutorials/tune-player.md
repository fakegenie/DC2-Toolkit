# Tune the player

This guide shows how to widen Max's melee combo input windows, adjust his charge attack hold threshold, and increase his weapon reach and damage using a `#pragma override` patch on `mainchr/c01_max.c`. For background on player action scripts, see [The player](../systems.md#the-player).

## 1. Inspect Max's combo and hitbox setup (Before)

In `extracted/scripts/mainchr/c01_max.c`, `setup_max_attacks` (`PROGRAM(init, ...)`) registers the hitbox windows for each weapon model class (`0`, `1`, and `2`), and `update_sword_combo_model_0` runs the five-hit combo state machine (`saved_value` `100` to `105`) for weapon model `0`:

```c
// extracted/scripts/mainchr/c01_max.c

void setup_max_attacks(void)
{
    weapon_model = 0;
    weapon_model = get_weapon_model();
    set_object_slot(0, "dcol00");
    set_object_slot(1, "ef00");
    set_object_slot(2, "bcol00");
    set_object_slot(3, "bcol01");
    set_object_slot(4, "keri_R");
    set_object_slot(5, "nage");
    set_object_slot(6, "gun");
    if (weapon_model == 0) {
        add_attack_hit("dcol00", "dcol01", HIT_MINTZ_SWORD_ATTACK, 4.0, 1.0, MOTION_5_HIT_COMBO_1, 0.15, 0.8);
        add_attack_hit("dcol00", "dcol01", HIT_MINTZ_SWORD_ATTACK, 4.0, 1.1, MOTION_5_HIT_COMBO_2, 0.4, 0.5);
        add_attack_hit("dcol00", "dcol01", HIT_MINTZ_SWORD_ATTACK, 4.0, 1.1, MOTION_5_HIT_COMBO_3, 0.375, 0.47);
        add_attack_hit("dcol00", "dcol01", HIT_MINTZ_SWORD_ATTACK, 4.0, 1.15, MOTION_5_HIT_COMBO_4, 0.33, 0.443);
        add_attack_hit("dcol00", "dcol01", HIT_MAX_SWORD_STRONG_ATTACK, 4.0, 1.2, MOTION_5_HIT_COMBO_5, 0.3, 0.4);
        add_attack_hit("dcol00", "dcol01", HIT_MAX_SWORD_STRONG_ATTACK, 4.0, 1.0, MOTION_THIRD_HIT, 0.45, 0.55);
    }
    // ... (weapon_model 1, weapon_model 2, and blade trails)
}

int update_sword_combo_model_0(void)
{
    int attack_presses;
    int status;
    int unused_value;
    int saved_value;
    float progress;
    int pad_button;
    int attack_hold_frames;
    float stick_alignment;

    saved_value = get_saved_value();
    pad_button = get_pad_button(PAD_BTN_ACTION_CONFIRM);
    if (saved_value == 100) {
        play_motion(MOTION_5_HIT_COMBO_1, -1, CHARA_MOTION_HOLD);
        get_motion_progress(&progress);
        if (progress >= 0.05 && progress <= 0.4) {
            set_move_impulse(0.6, 0.0, 1);
        }
        if (progress >= 0.714 && progress <= 1.0) {
            set_move_impulse(0.8, 0.0, 1);
        }
        attack_hold_frames = get_attack_hold_frames();
        if (attack_hold_frames > 8) {
            reset_attack_hold_frames();
            set_saved_value(1200);
            return 0;
        }
        if (progress >= 0.33 && progress <= 0.45) {
            attack_presses = get_attack_presses();
            if (attack_presses) {
                start_combo_hit(MOTION_5_HIT_COMBO_2, saved_value + 1);
            }
        } else {
            get_motion_status(&status);
            if (status == 4) {
                set_saved_value(0);
            }
        }
        return 0;
    }
    // ... (saved_value 101..105)
}
```

During the first swing (`saved_value == 100`), the player must press Attack while `progress` is between `0.33` and `0.45` to chain into `MOTION_5_HIT_COMBO_2`. Subsequent swings check similarly tight windows (`0.464`–`0.545`, `0.5`–`0.55`, and `0.47`–`0.522`).

## 2. Write the `#pragma override` patch (After)

Generate a patch for `mainchr/c01_max.c`:

```sh
dc2tk mod override extracted/scripts max-combo --script mainchr/c01_max.c update_sword_combo_model_0
```

Edit `mods/max-combo/c01_max.c` to widen the combo input windows across all four transitions (`0.15` to `0.75`) and start the charge attack after holding Attack for `5` frames instead of `8`:

```c
// mods/max-combo/c01_max.c
#pragma target script mainchr/c01_max.c
#include <action.h>
#include <pal.h>
#include <disc.h>

#pragma override
int update_sword_combo_model_0(void)
{
    int attack_presses;
    int status;
    int saved_value;
    float progress;
    int attack_hold_frames;
    float stick_alignment;

    saved_value = get_saved_value();
    if (saved_value == 100) {
        play_motion(MOTION_5_HIT_COMBO_1, -1, CHARA_MOTION_HOLD);
        get_motion_progress(&progress);
        if (progress >= 0.05 && progress <= 0.4) {
            set_move_impulse(0.6, 0.0, 1);
        }
        if (progress >= 0.714 && progress <= 1.0) {
            set_move_impulse(0.8, 0.0, 1);
        }
        attack_hold_frames = get_attack_hold_frames();
        if (attack_hold_frames > 5) {
            reset_attack_hold_frames();
            set_saved_value(1200);
            return 0;
        }
        if (progress >= 0.15 && progress <= 0.75) {
            attack_presses = get_attack_presses();
            if (attack_presses) {
                start_combo_hit(MOTION_5_HIT_COMBO_2, saved_value + 1);
            }
        } else {
            get_motion_status(&status);
            if (status == 4) {
                set_saved_value(0);
            }
        }
        return 0;
    }
    if (saved_value == 101) {
        get_motion_progress(&progress);
        if (progress >= 0.0 && progress <= 0.409) {
            set_move_impulse(0.93, 0.0, 1);
        }
        if (progress >= 0.25 && progress <= 0.75) {
            attack_presses = get_attack_presses();
            if (attack_presses) {
                stick_alignment = get_stick_alignment();
                if (stick_alignment > 0.6) {
                    play_motion_once(MOTION_THIRD_HIT);
                    turn_to_target(40.0, 2.0);
                    set_saved_value(105);
                } else {
                    start_combo_hit(MOTION_5_HIT_COMBO_3, saved_value + 1);
                }
            }
        } else {
            get_motion_status(&status);
            if (status == 4) {
                set_saved_value(0);
            }
        }
        return 0;
    }
    if (saved_value == 102) {
        get_motion_progress(&progress);
        if (progress >= 0.0 && progress <= 0.375) {
            set_move_impulse(0.63, 0.0, 1);
        }
        if (progress >= 0.675 && progress <= 1.0) {
            set_move_impulse(0.6, 0.0, 1);
        }
        if (progress >= 0.25 && progress <= 0.75) {
            attack_presses = get_attack_presses();
            if (attack_presses) {
                start_combo_hit(MOTION_5_HIT_COMBO_4, saved_value + 1);
            }
        } else {
            get_motion_status(&status);
            if (status == 4) {
                set_saved_value(0);
            }
        }
        return 0;
    }
    if (saved_value == 103) {
        get_motion_progress(&progress);
        if (progress >= 0.0 && progress <= 0.339) {
            set_move_impulse(0.787, 0.0, 1);
        }
        if (progress >= 0.25 && progress <= 0.75) {
            attack_presses = get_attack_presses();
            if (attack_presses) {
                play_motion_once(MOTION_5_HIT_COMBO_5);
                turn_to_target(40.0, 2.0);
                set_saved_value(saved_value + 1);
                set_menace(MENACE_MELEE, 3);
            }
        } else {
            get_motion_status(&status);
            if (status == 4) {
                set_saved_value(0);
            }
        }
        return 0;
    }
    return original();
}
```

Note how the patch handles `saved_value` `100`–`103` and delegates `104` and `105` back to `original()`. You can override `update_sword_combo_model_1` and `update_sword_combo_model_2` in the same file for the other wrench/hammer model classes, or override `setup_max_attacks` to increase the `radius` (`4.0`) and `power_rate` (`1.0`–`1.2`) arguments in `add_attack_hit`.

## 3. Add `mod.json` and test in-game

Create `mods/max-combo/mod.json` with `"dev": {"start_at": "d01f01"}` so **New Game** starts directly in the Underground Channel dungeon with Max:

```json
{
  "format": 2,
  "id": "you.max-combo",
  "version": "1.0.0",
  "name": "Fast Wrench Combo",
  "dev": {"start_at": "d01f01"}
}
```

Validate and build:

```sh
dc2tk mod check extracted/scripts max-combo
dc2tk build extracted/scripts dc2.iso -o test.iso --mod max-combo --dev --play
```
