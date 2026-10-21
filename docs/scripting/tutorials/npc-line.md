# Change what an NPC says

This guide shows how to change Gordon's early dialogue line in Palm Brinks using a `#pragma override` patch. For background on how town scripts handle NPC conversations, see [Talking to people](../systems.md#talking-to-people).

## 1. Find the original dialogue function (Before)

Search `extracted/scripts/` for a phrase from Gordon's line (`"taking some nasty bugs"`) or for `talk_gordon`. In `extracted/scripts/lib/map/base_town_villagers_v1.c`, `talk_gordon` calls `talk_gordon_early`:

```c
// extracted/scripts/lib/map/base_town_villagers_v1.c
void talk_gordon_early(int npc, int player)
{
    int active_chr;

    apply_message_preset(0, MES_PRESET_FUKIDASHI);
    active_chr = get_active_chara();
    if (active_chr == 1) {
        set_chara_motion(player, MOTION_STAND_2);
        show_message(0, "Hmm, this tree... It doesn't look\ngood. I've got to do something.");
        show_message_auto(0, -1, npc, player, 0);
    } else {
        show_message(0, "Oh, Max! You scared me.\nI was just taking some nasty bugs\noff this tree.<page>Not all trees grow up fine on\ntheir own, you know. Some, like\nthis one, you have to really watch.");
        talk_to_npc(npc, -1, 0, 0);
    }
}
```

Several Palm Brinks maps (`m01`, `i03h01a`, `i04h01`, and `i04h03`) include `base_town_villagers_v1.c` (and `_v2.c` through `_v5.c` for French, German, Italian, and Spanish).

## 2. Create the `#pragma override` patch (After)

Generate a patch for `talk_gordon_early` on map `m01` with `dc2tk mod override`:

```sh
dc2tk mod override extracted/scripts gordon-line --map m01 talk_gordon_early
```

Because `dc2tk build` merges patches after each script's `#include` files are expanded, `#pragma target map m01` overrides `talk_gordon_early` in all five language variants of `m01` without copying or forking `base_town_villagers_v1.c` (to apply the change across every map that includes the library, use `#pragma target map m01 i03h01a i04h01 i04h03`).

Edit `mods/gordon-line/m01.c`:

```c
// mods/gordon-line/m01.c
#pragma target map m01
#include <event.h>
#include <pal.h>
#include <disc.h>

#pragma override
void talk_gordon_early(int npc, int player)
{
    int active_chr;

    apply_message_preset(0, MES_PRESET_FUKIDASHI);
    active_chr = get_active_chara();
    if (active_chr == 1) {
        set_chara_motion(player, MOTION_STAND_2);
        show_message(0, "Hmm, this tree... It doesn't look\ngood. I've got to do something.");
        show_message_auto(0, -1, npc, player, 0);
    } else {
        show_message(0, "Oh, Max! You scared me.\nThe trees in Palm Brinks are\ngrowing faster than ever!<page>Take a look around the garden.");
        talk_to_npc(npc, -1, 0, 0);
    }
}
```

Use `\n` for a line break and `<page>` for a page break (see [Message text formatting codes](../language.md#message-text-formatting-codes)).

### Multi-language text (optional)

If you want different text in each PAL language instead of showing the same string in all five languages, branch on the built-in `LANGUAGE` constant:

```c
#if LANGUAGE == LANGUAGE_FRENCH
        show_message(0, "Oh, Max ! Les arbres poussent\nplus vite que jamais !");
#else
        show_message(0, "Oh, Max! You scared me.\nThe trees in Palm Brinks are\ngrowing faster than ever!<page>Take a look around the garden.");
#endif
```

### Replacing numeric message IDs (optional)

If an original `show_message` call passes an integer ID instead of a string literal (for example, `show_message(0, 1090010)`), the engine reads that line from an external `.txt` dialogue table. You can replace the integer ID directly with a string literal (`show_message(0, "Your new text")`). If the scene also plays a voice stream (`open_stream_wav` / `play_stream_when_ready`), remove the stream calls so the original voice line does not play over your text.

## 3. Add `mod.json` and test in-game

Create `mods/gordon-line/mod.json` with `"dev": {"start_at": "m01"}` so **New Game** starts directly in Palm Brinks:

```json
{
  "format": 2,
  "id": "you.gordon-line",
  "version": "1.0.0",
  "name": "Gordon Line",
  "dev": {"start_at": "m01"}
}
```

> **Tip:** On a fresh New Game (`SAVE_GET_PROGRESS < 3`), Palm Brinks starts before chapter 1 townspeople spawn outside. To spawn the townspeople immediately on New Game, also override `play_map_start_arrival` in `mods/gordon-line/m01.c` to set `set_save_value(SAVE_SET_PROGRESS, 3)` on entry (see [`spinning-cult/m01.c`](../examples/spinning-cult/m01.c)), or load a Chapter 1 save file.

Validate and build the mod:

```sh
dc2tk mod check extracted/scripts gordon-line
dc2tk build extracted/scripts dc2.iso -o test.iso --mod gordon-line --dev --play
```
