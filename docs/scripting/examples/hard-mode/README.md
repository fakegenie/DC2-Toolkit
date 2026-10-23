# hard-mode: Hard Mode

A harder game made only of data tables, with two settings a player chooses when they build it.

- **Always:** every monster has half again as much life (`hp` x1.5) and hits 40% harder (`attack` x1.4), bosses included.
- **`fragile_weapons`** (on by default): every weapon starts with 60% of its WHP, so it breaks sooner.
- **`tight_wallet`** (off by default): shops charge half as much again (`buy` x1.5; selling is unchanged).

## Building the mod

`make_mod.py` reads the game's tables from `extracted/data` (written by `dc2tk setup`) and writes the mod into `mods/hard-mode` with this folder's `mod.json`, this README and the mod sets:

```sh
python3 -I docs/scripting/examples/hard-mode/make_mod.py
dc2tk build extracted/scripts dc2.iso -o hard.iso --mod dc2tk.hard-mode
```

Change the numbers at the top of `make_mod.py` and run it again to tune the mod.

## Settings and mod sets

`mod.json` turns each data line on or off with `"when"`:

```json
"data": {
  "monsters": "data/monsters.json",
  "items": {"file": "data/items.json", "when": "fragile_weapons"},
  "shops": {"file": "data/shops.json", "when": "tight_wallet"}
}
```

A `--mod` build uses the defaults. The three sets in `sets/` choose for the player:

| Set | Settings |
| --- | --- |
| `hard.json` | The defaults: tougher monsters, fragile weapons |
| `brutal.json` | Pricier shops too |
| `tough-monsters-only.json` | Tougher monsters, weapons as they were |

```sh
dc2tk build extracted/scripts dc2.iso -o brutal.iso --set mods/hard-mode/sets/brutal.json
dc2tk play dc2.iso --native --set mods/hard-mode/sets/brutal.json
```

Monster stats live in the game's executable, so the native port needs its own binary for them: `dc2tk play --native` builds one for the set. For the same reason Hard Mode cannot be a `.dc2pak`.

## Why not difficulty levels

A data table can have only one line in a mod, and `"when"` takes a true/false setting, so one table cannot be scaled by a chosen number. Each setting here switches a whole table instead. Levels would need one mod per level, marked as `"conflicts"` with the others.
