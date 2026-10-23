# Game Data (JSON)

`dc2tk setup` exports the game's data tables (items, weapons, shops, monsters, inventions, Georama, fishing, dungeon floors, and more) to `extracted/data/*.json` for reference. To change a table, copy it into a mod.

## Quick steps

1. Copy a data table into your mod folder:
   ```sh
   dc2tk mod asset dc2.iso my-mod data:items
   ```
   This writes `mods/my-mod/data/items.json` and adds its line to `mods/my-mod/mod.json`:
   ```json
   {
     "data": {
       "items": "data/items.json"
     }
   }
   ```
   A small change can go in `mod.json` itself, as the table's records in place of the file name: `"data": {"items": [{"item": 1, "weapon": {"attack": 12}}]}`. This works for the tables whose records are all in a section named after the table (`items`, `monsters`, `floors`, and so on).
2. Edit `mods/my-mod/data/items.json` and keep only the records and fields you change. Each record keeps its key fields (`item` for items; see the table below) so `dc2tk build` knows which record you mean.
3. Check the mod with `dc2tk mod check extracted/scripts local.my-mod --disc dc2.iso`. It lists every value your tables change.
4. Build the ISO with `dc2tk build`.

## Available data tables

| Table ID | Controls | Sections and key fields |
| --- | --- | --- |
| `data:items` | Items, weapons, attachments, Ridepod parts, and fish | `items`, keyed by `item`: `name`, `type`, `stack`, `max`, `icon`, `model`, and the family's stats under `weapon`, `effect`, `attachment`, `ridepod`, or `fish`; `spectrumise` (`slot`, `value`) |
| `data:shops` | Prices, shop inventories, and Donny's shop | `prices` keyed by `item` (`buy`, `sell`); `shops` keyed by `shop` (`items`); `donny` keyed by `row` (`item`, `level`) |
| `data:monsters` | Monster stats, drops, 3D model, and AI script | `monsters` keyed by `id`: `hp`, `attack`, `defence`, `abs`, `money`, `drops`, `rare_drop`, `elements`, `model`, `script`, and more |
| `data:inventions` | Invention recipes, scoops, and photo ideas | `recipes` keyed by `recipe` (`item`, `ideas`, `materials`); `scoops` keyed by `scoop`; `ideas` keyed by `idea` (`name`, `sort`) |
| `data:georama` | Georama parts, rivers, and water | `parts` keyed by `town` and `part` (`materials`, `culture_points`, `max`, ...); `fixed_parts`, `starting_parts`, `rivers`, `water`, `water_surfaces`, `water_draws` |
| `data:requests` | Georama town requests | `conditions` keyed by `town` and `condition`; `requests` keyed by `town` and `request` |
| `data:townspeople` | Townspeople you can recruit | `townspeople` keyed by `npc` (`model`, `abilities`, `points`, `ability_costs`) |
| `data:floors` | Dungeon floor monster spawns | `floors` keyed by `dungeon` and `floor` (`monsters`: a list of `{"monster", "talk"}`) |
| `data:medals` | Dungeon floor medal goals | `floors` keyed by `dungeon` and `floor` (`fast_time`, `fishing_record`, `spheda`, `spheda_prizes`, ...) |
| `data:chests` | Dungeon treasure chest contents | `chests` keyed by `dungeon` (`groups` of items by rank, and each floor's `groups`) |
| `data:layouts` | Dungeon room layouts | `layouts` keyed by `layout` (`grid_size`, `rooms`) |
| `data:parts` | Dungeon room pieces and minimap tiles | `parts` keyed by `part`; `minimap` keyed by `map` |
| `data:fishing` | Fishing spots, fish, and Finny Frenzy | `spots`, `race_fish`, `prize_groups`, `prizes`, `species`, `race_kinds` |
| `data:characters` | Starting stats, weapon level-up, and Monica's monster forms | `characters`, `weapon_level_up`, `monster_forms`, `badges` |
| `data:gifts` | Gifts that award a monster badge | `gifts` keyed by `row` |
| `data:spheda` | Spheda club power | `clubs` keyed by `club` |
| `data:breeding` | Aquarium fish breeding pairs | `breeding` keyed by `row` |

Name fields such as a monster's `name` are read-only labels, except on items and ideas (see [Item names](#item-names-and-descriptions)). Dungeon numbers start at `0` (the Underground Channel). Unknown fields, values that do not fit the engine's field, and values two mods set differently are build errors.

See [DC2 Internals: Game Data Tables](../internals/data.md) for what every field means and where the game stores it.

## Examples

### Edit an item or weapon (`data:items`)

```json
{
  "items": [
    { "item": 1, "weapon": { "attack": 250, "flame": 100 } }
  ]
}
```

### Add an item

A record whose key is not on the disc adds a new record. Give `"like"` naming an existing record to copy, and only the fields that differ:

```json
{
  "items": [
    { "item": 429, "like": 1, "name": "Mega Wrench", "weapon": { "attack": 250 } }
  ]
}
```

The disc has 423 items, and the game holds at most 432, numbered `1` to `511`. A new item needs a price in `data:shops` before a shop can sell it.

### Item names and descriptions

On items, `"name"` and `"description"` set the text in every language, and `"names"` and `"descriptions"` set it per language (`"ja"`, `"en"`, `"fr"`, `"de"`, `"it"`, `"es"`, `"ko"`):

```json
{ "item": 1, "names": { "en": "Super Wrench", "fr": "Super Clé" } }
```

`dc2tk build` writes the names into every `comdatmes` file and the descriptions into `system_N.mes` and the menus' `allmenu.mes`.

### Shops (`data:shops`)

A shop's `items` is one list for every chapter, or a list of 8 lists for chapters 1 to 8. Every item a shop sells needs a price:

```json
{
  "prices": [
    { "item": 429, "buy": 500, "sell": 250 }
  ],
  "shops": [
    { "shop": 0, "items": [429, 1] }
  ]
}
```

A shop holds at most 63 items.

### Edit a monster (`data:monsters`)

```json
{
  "monsters": [
    { "id": 168, "hp": 5000, "attack": 300, "money": 500 }
  ]
}
```

Monster `168` is the Golem. `dc2tk build` patches the changed bytes into the game's executable (`SCES_511.90`). The 343 monster kinds (ids `0` to `1501`) are fixed: you can edit any of them but not add new ones.

## What can be added

| Records | Limit |
| --- | --- |
| Items (`data:items`) | 432 items in all |
| Prices and shops (`data:shops`) | 63 items per shop |
| Recipes (`data:inventions`) | 234 recipes, 4 materials each |
| Ideas (`data:inventions`) | ids `1` to `999` |
| Georama parts (`data:georama`) | 384 parts per town, ids `0` to `127` |
| Fishing spots (`data:fishing`) | 256 spots, 8 fish each |
| Requests (`data:requests`) | 16 per town |
| Townspeople (`data:townspeople`) | 180 |

Monsters, floor medals, race fish, prizes, Georama conditions, and the tables in the executable (`data:characters`, `data:gifts`, `data:spheda`, `data:breeding`, Donny's shop) are fixed-size: edit them, but do not add rows. A floor's `monsters` holds at most 32 entries.

The [spinning-cult example](../scripting/examples/spinning-cult/README.md) adds an item with its own icon and model, three ideas, and a recipe. The [hard-mode example](../scripting/examples/hard-mode/README.md) scales every monster, weapon and price, with settings that switch its tables on and off.
