# Game data: items, shops, monsters and the rest

This page documents where DC2 stores its gameplay data tables: item and weapon stats, shop inventories, monster attributes, invention recipes, Georama parts and requests, fishing parameters, dungeon floor medals, character stats, and townspeople.
- For exporting and editing these tables from your disc, see [editing game data](../guide/data.md).
- For runtime memory budgets of each data loader, see [memory](memory.md).
- For dungeon floor layouts, room parts, and treasure chests, see [dungeons](dungeons.md).

All engine source paths are relative to `ps2/` in the ChronicleTwo decompilation (with unit notes in `re/docs/<unit>/notes.md`). All corpus counts cover the extracted PAL disc (`SCES_511.90`).
- **`.cfg` scripts**: Plain-text scripts inside `DATA.DAT` (`TAG arg, arg, ...;`, `\r\n` line endings), parsed by `CScriptInterpreter` (`src/scriptinterpreter.cpp`).
- **Executable tables**: Compiled into the `.data` section of `SCES_511.90` (`config/pal/main.symbols.txt`). The single `PT_LOAD` segment maps file offset `0x80` to virtual address `0x100000`, so any `.data` address `A < 0x37CD80` sits at file offset `A - 0xFFF80` (`>= 0x37CD80` is zero-filled `.bss`).

## How the engine reads a cfg script

`CScriptInterpreter::GetArg` parses each `;`-terminated statement in four steps:
1. **Comment stripping**: Strips `//` line comments and `/* ... */` block comments **before** parsing quotes (a `//` or `/*` inside quotes still strips the line or block).
2. **Splitting**: Splits arguments on commas outside double quotes, steps 2 bytes on Shift-JIS lead bytes, and unescapes `\"`. Trailing empty arguments before `;` are ignored.
3. **Typing**: Quoted tokens become strings; unquoted tokens (read up to the first space) with `.` use `atof` (`float`), tokens with only ASCII digits and `-` use `atoi` (32-bit wrapping `int`), and other tokens become invalid (`0` / null).
4. **Argument cap**: Maximum **64 arguments per statement** (`SPI_STACK_SIZE`); `PushStack` drops arguments `>= 65`.

### Limits on counts

| Data Category | Maximum Capacity | Limiting Engine Array or Buffer |
|---|---|---|
| Items | `432` total (IDs `1..511`): `116` weapons, `162` usable items, `38` attachments, `68` Ridepod parts, `20` fish, `40` clothes | `local_com_itemdata`, `local_itemdatano_converttable`, and per-family `local_*` `.bss` arrays |
| Prices and shop inventories | Item IDs `1..511`; max `63` goods per shop | `CShop::price[512]`; `SHOP_ITEM_MAX` (`64`) and `SPI_STACK_SIZE` (`64`) |
| Invention recipes | `234` recipes | Menu `InventTeigiStack`: `0x210` 16-byte units (`0x24` B/recipe) |
| Ideas and scoops | Idea IDs `1..999`; `0x247` 16-byte units for table + strings per language (~11 short names free in French); max `388` combined ideas + scoops; display name `<= 24` B | `pic_name_text_buff_1660` (`0x248` units); `UpdataNetaMemoStr` `sort_keys[0x184]`; `MenuInventNetaMemoDraw` `text[0x20]` |
| Georama parts | `384` parts/town; part IDs `0..127`; name `<= 47` B; `g01<lang>.gpi <= 0x77D0` B | `GEORAMA_PARTS_LIST_MAX`; stock ID loop (`0..0x7F`); `name[0x30]`; `MakeDownLoadAnaunce` buffer |
| Georama placed parts | `256` placed parts per town | `CEditMap::CreateTable` |
| Fishing spots and fish/spot | `8` fish slots per spot; spot count allocated from `FISH_MAP_NUM` | `kFishPlaceMaxFish` (`8`) |
| Georama requests | `16` requests/town; `7` conditions/request; `0x300` 16-byte string units across all 5 towns | `EditAnalyzeSrc::data[16]`, `CON_NO`, and `buff_1271` |
| Townspeople (NPCs) | `180` records (`+3` over retail `177`); name `<= 27` B, model `<= 15` B; `.cfg <= 0x7FD0` B | `NpcBaseData[180]`, `NPC_BASE_DATA`, and `LoadNPCCfg` buffer |
| Monsters per dungeon floor | `32` monster entries per floor | `CMonsterLocateInfo` (section 4) |

## 1. Where each kind of data lives

| Data Table | Location (`DATA.DAT` Path or `SCES_511.90` Symbol) | Format / Struct | Retail Count |
|---|---|---|---|
| Item common data | `menu/cfg7/comdat.cfg` | cfg `COM` | `423` |
| Weapons | `menu/cfg7/wepdat.cfg` | cfg `WEP*` blocks | `112` |
| Usable item effects | `menu/cfg7/itemdat.cfg` | cfg `ITEM` | `160` |
| Attachments | `menu/cfg7/atdat.cfg` | cfg `AT_ST*` blocks | `36` |
| Ridepod parts | `menu/cfg7/robodat.cfg` | cfg `RB_PARTS` | `63` |
| Fish breeding/racing stats | `menu/cfg7/fishdat.cfg` | cfg `FISH` | `18` |
| Clothing guard table (unread) | `menu/cfg7/grddat.cfg` | cfg `GRD` | `34` |
| Item names per language | `menu/cfg7/comdatmes0..6.cfg` | cfg `MES_SYS` | `423`–`428` |
| Spectrumising bonuses | ELF `etcitem_spectol_table` (`0x339820`) | `s8 { slot, value } × 425` | `425` |
| Prices and shop inventories | `menu/shopusa1..8.cfg`, `menu/shoplst1..8.cfg` | cfg `PRICE`, `SHOP` | `428` prices, `38` shops |
| Donny's shop inventory | ELF `dony_shoplist` (`0x358620`) | `{ s16 item, s8 level } × 7` | `7` |
| Monster stats, drops, resistances | ELF `base_monster_define` (`0x341DA0`) | `BASE_MONSTER_TBL` (`0xB8 × 344`) | `343` |
| Monster names per language | `dungeon/cfg_file/mosdata0..6.cfg` | cfg `NAME` | `343` |
| Monster floor spawn lists | `dungeon/cfg_file/mos_place0..6.cfg` | cfg `FLS` / `FL` / `FLE` | `6`–`31` floors |
| Dungeon floor room layouts | `dungeon/cfg_file/d0*.cfg`, `mstage0..8.cfg` ([dungeons](dungeons.md)) | cfg `ROOM_ID`..`ROOM_END` | `291` layouts |
| Treasure chest tables | `dungeon/cfg_file/tbox_d0*.cfg` ([dungeons](dungeons.md)) | cfg `GROUP` / `ITEM` / `FLOOR` | `8` dungeons |
| Dungeon room parts & minimap tiles | ELF `PartsInfoData` (`0x33D440`), `MiniMapInfoData` (`0x33EE40`) ([dungeons](dungeons.md)) | `0x18 × 277`, `0x290 × 18` | `277` parts, `18` maps |
| Invention recipes | `menu/inv6.lst` (and copies in menu `.pac` files) | cfg `DATASET` | `136` |
| Idea and scoop names | `menu/<lang>/neta2.lst` | cfg `PIC_NAME` | `294` |
| Scoop metadata & descriptions | ELF `scoop_table` (`0x352DE0`), `menu/<lang>/scoop.pac` (`scoop.cfg`) | `SCOOP_DATA` (`0x14 × 53`); cfg `STR` | `53` |
| Georama buildable parts | `map/g/gNN/gNN<lang>.gpi` | cfg `EDIT_PARTS` blocks | `13`–`56`/town |
| Georama fixed/init parts, water | `map/g/gNN/gNN.cfg` | cfg `FIX_EPARTS`, `INIT_EPARTS`, `RIVER_*`, `WATER_*` | Per town |
| Georama requests & labels | `geo0..6.cfg` | cfg `GEO_ANALYZE` | `5` towns, `98` conds, `49` reqs |
| Fish species parameters | ELF `FishParam` (`0x361A50`) | `FISH_PARAM` (`0x54 × 19`) | `18` |
| Fishing spots & weights | `sg/fish/fishing.cfg` | cfg `FISH_MAP` blocks | `22` maps, `110` entries |
| Fish race opponents | `sg/gyo/rfd0..6.cfg` | cfg `NUM` / `DAT` | `4` classes, `216` fish |
| Fish race & contest prizes | `gyop.cfg`, `uofp.cfg` | cfg `GRP` / `PRIZE` | `12`, `10` |
| Dungeon floor medal goals | `menu/dngmap/dflr0..6.cfg` | cfg `RF_INFO`, `RF_INFO2` | `9`–`39` floors |
| Max & Monica starting HP | ELF `lifetbl_2854` (`0x37C790`) | `f32[2]` | `2` |
| Monster badge form stats | ELF `mos_henge_param` (`0x339D70`) | `0x1C × 57` | `57` |
| Monster badge evolution trees | ELF `monster_progress_tbl` (`0x358C90`) | `s16[19][5]` | `19` |
| Fish race scaling & terrain | ELF `fish_data` (`0x3630C0`) | `grFISH_DATA` (`0x1C × 18`) | `18` |
| Weapon WHP gain per level-up | ELF `htbl_1662` (`0x33A490`) | `s8[10]` | `10` |
| Starting equipment by language | ELF `weptbl_4503` (`0x33A510`) | `s16[2][2][5]` | `2 × 2 × 5` |
| Townspeople | `npc0..6.cfg` | cfg `NPC_INFO` | `177` |
| Monster badge gift items | ELF `gift_item_tbl` (`0x351660`) | `s16[3] × 14` (`-1` term) | `14` |
| Spheda club parameters | ELF `GolfClubDef` (`0x361340`) | `0xC × 7` (`6` reachable) | `6` |
| Aquarium fish breeding table | ELF `aquafish_mixTable` (`0x3535A0`) | `s8[171][3]` | `171` |

- **Unused legacy files**: `menu/cfg5`, `menu/cfg6`, `menu/0/inv*.lst`, `menu/inv4.lst`, `menu/inv5.lst`, `sg/fish/fish.cfg`, `sg/gyo/racefish*.cfg`, `npc_place.cfg`..`npc_place3.cfg`, `npc.cfg`, `edit.cfg`, and language-less `.gpi` files are never read.
- **Hardcoded growth formulas**: `CUserDataManager::Initialize` sets starting defence to `4` or `8`. Weapon required ABS increases by `+50%` per level, and max WHP increases by a random entry from `htbl_1662` (`0x33A490`).

## 2. Items: `menu/cfg7`

`CGameData::LoadData` (`src/gamedata.cpp`, `0x196980`) parses the files in `menu/cfg7/` using `gamedata_tag` (`0x339B80`). Item IDs (`1..511`) map to `CDataCommon` (`local_com_itemdata[432]`) via `local_itemdatano_converttable`, and `CDataCommon::list` indexes the family table selected by `ConvertUsedItemType(type)`:

| `type` Values | Item Family | `.cfg` File | Struct (`local_*` `.bss` Array) | Capacity (Retail) | 3D Model Path (`GetItemFilePath`) |Owned Name Max |
|---|---|---|---|---|---|---|
| `1..4` | Weapon | `wepdat.cfg` | `CDataWeapon` (`0x4C` B) | `116` (`112`) | `mainchr/<model>.chr` | `31` B |
| `5..10` | Clothing | `grddat.cfg` | `s16` (`local_grddata`, unread) | `40` (`34`) | `mainchr/<model>.chr` | — |
| `11`, `>= 20` (`0x1C` gift, `0x23` roasted fish) | Usable item | `itemdat.cfg` | `CDataItem` (`0x10` B) | `162` (`160`) | `item/<model>.chr` | `21` B (roasted fish) |
| `0x1E` (`30`) | Fish | `fishdat.cfg` | `CDataBreedFish` (`0x14` B) | `20` (`18`) | `item/<model>.chr` | `20` B |
| `12..15` | Ridepod part | `robodat.cfg` | `CDataRoboPart` (`0x24` B) | `68` (`63`) | `dungeon/robo/<model>.chr` | `31` B |
| `16..19`, `0x22` (`34`) | Attachment | `atdat.cfg` | `CDataAttach` (`0x18` B) | `38` (`36`) | `item/<model>.chr` | `25` B |

### 2.1 `COM item, type, list, active_set, stack, max, icon_sheet, icon, message, "model", attribute;`

`_DATACOM` (`0x195BB0`) populates `CDataCommon` (`0x2C` bytes):

| Field | Type | Meaning |
|---|---|---|
| `type` | `u8` | Item category; selects item family and model directory (table above). |
| `list` | `s16` | Zero-based index into the family's data table. |
| `active_set` | `u8` | Non-zero if equippable in an active quick-use slot. |
| `stack` | `s16` | Maximum count per inventory slot stack. |
| `max` | `u16` | Maximum total count carried (weapons `> 100` clamp to `144`). |
| `icon_sheet` | `u8` | Icon sheet in `img/allitem.img` (`GetMenuItemIconTexInfo`: `0` = `wepicon`, `1` = `itemicon`). |
| `icon` | `s16` | Cell index (`0..191`) on the 8×24 grid of 32×32-pixel cells. |
| `message` | `s16` | Base offset for message ID `10000 + message` in `system_N.mes` and `allmenu.mes`. |
| `model` | `char[16]` | Base filename of the `.chr` model pack. |
| `attribute` | `u32` | Bitfield: `0x1` = discardable (`IsTrush`), `0x2` = spectrumisable (`SetItemSpectolPoint`). |

### 2.2 Weapons: `WEP`, `WEP_ST`, `WEP_ST_L`, `WEP_ST2`, `WEP_ST2_L`, `WEP_SPE`, `WEP_BUILD`

`WEPNUM n;` sets the count. Each subsequent block populates the next sequential `CDataWeapon` (`0x4C` bytes, indexed by `list = n`), and `WEP_BUILD` advances the slot index:

| Statement | Fields | Type | Meaning |
|---|---|---|---|
| `WEP` | `whp`, `abs` | `s16[2]` | Base WHP (capped at `255`); ABS required for first level-up. |
| `WEP_ST` | `attack`, `durability` | `s16[2]` | Starting Attack and Durability (`calcWeaponParamWhp`). |
| `WEP_ST_L` | `attack_max`, `durability_max` | `s16[2]` | Maximum Attack and Durability caps for this weapon. |
| `WEP_ST2` | `flame`, `chill`, `lightning`, `cyclone`, `smash`, `exorcism`, `beast`, `scale` | `s16[8]` | Starting elemental/attribute stats. |
| `WEP_ST2_L` | `flame_max` .. `scale_max` | `s16[8]` | Maximum caps for the 8 attributes. |
| `WEP_SPE` | `synthesis_points`, `palette`, `unk_47`, `synth_per_lvl`, `special`, `attack_type`, `model_no` | `u8` (`u32` for `special`) | Starting synthesis points (float in cfg), palette index, unused `unk_47`, synth points/level, special flags (`special`), attack/damage type, and player base-pack `model_no` (`0..3`). |
| `WEP_BUILD` | `build_up[3]`, `build_up_kills[3]` | `s16[6]` | Up to 3 target weapon IDs (`0` = none) and required monster kill IDs (`-1` = none). |

### 2.3–2.7 Other item families and names

| Family / File | Statements & Syntax | Struct & Field Mapping |
|---|---|---|
| 2.3 Usable items (`itemdat.cfg`) | `ITEM item, use_flags, status_flags, target_flags, val0, val1, val2;` | `CDataItem` (`0x10` B): `u32 use_flags` (bit `0x800000` expands to `0x142A8000`), `u32 status_flags`, `u16 target_flags`, `s16 values[3]`. |
| 2.4 Attachments (`atdat.cfg`) | `AT_INIT n;` then blocks of `AT_ST atk, dur; AT_ST2 flame..scale; AT_ST_SP special;` | `CDataAttach[list]` (`0x18` B): `s16` stat bonuses and `u32 special` bitmask. |
| 2.5 Ridepod parts (`robodat.cfg`) | `RB_PARTS item, kind, capacity, file_no, ...;` | `CDataRoboPart` (`0x24` B): `kind 0` (Body: `defence`, unread str, `label`), `kind 1` (Arm: `whp`, `atk`, `dur`, 8 elements, `atk_type`, `label`), `kind 2` (Legs: unread `unk_4`, `atk_type`), `kind 3` (Battery: `energy`). |
| 2.6 Fish (`fishdat.cfg`) | `FISH item, size, unk_4, power, spd0, spd1, spd2, stamina[, unk_10];` | `CDataBreedFish` (`0x14` B): `f32 size`, `s16 power`, `s16 speed[3]`, `s16 stamina` (`unk_4` and `unk_10` are never read). |
| 2.7 Guards (`grddat.cfg`) | `GRD item, value, ...;` | Writes `s16` into `local_grddata` (never read by the engine). |
| 2.7 Names (`comdatmes<lang>.cfg`) | `MES_SYS item, "name";` (`MES_SYSSPE` ignored) | `LoadItemSystemMes` (`0x196A70`) stores strings in `gamedata_sysword_buffer_1073` (`0x280` = 640 16-byte units; PAL uses `479..583`, Korean uses `1,038`). Supplies `GetItemMessage` (owned weapon default names, invention/Georama menus). |
| 2.7 Menu names & help | Message ID `10000 + message` | Inventory/message slots read `meswin/system_N.mes` (`GetItemMessageNo` kind `1`, `0xD000` B max); item descriptions read `allmenu.mes`. |

### 2.8 Limits, icons and models

- **File buffer cap**: Each `.cfg` file in `menu/cfg7/` must fit in `LoadGameDataAnalyze`'s **`0x77C0`-byte** buffer.
- **Spectrumising cap (`item <= 425`)**: Any non-weapon, non-attachment item with `item > 425` must **not** set `attribute & 0x2`, because `SetItemSpectolPoint` indexes `etcitem_spectol_table[item - 1]` (`425` entries) without bounds checking.
- **Icons (`img/allitem.img` and copies in menu `.pac` files)**:
  - `itemicon` (sheet `1`): cells `52` and `191` (`予約`, "reserved") are unused.
  - `wepicon` (sheet `0`): cells `52`, `110`, `150`, and `191` are unused (`56` is the night-time icon for item `56`, Lamb's Sword, selected when `GetMenuItemIconTexGetXY` adds `1` during time band `2`).
- **3D models**: Menu viewers (`MenuItemChrLoadEndCheck`, `CMenuInvent::LoadCharaCheck`) load item `.chr` packs without animations (`0` of `168` `item/*.chr` packs have `.mot` files).

## 3. Shops: `menu/shopusa<chapter>.cfg`

`CShopMenu::InitEnd` (`0x295FC0`) loads `menu/shopusa%d.cfg` (languages `1..6`) or `menu/shoplst%d.cfg` (language `0`) for `chapter = clamp(GetNowChapter(), 1, 8)` using `menu_shop_tag` (`0x358640`):

| Statement | Target Array | Rules & Limits |
|---|---|---|
| `PRICE item, buy, sell;` | `CShop::price[512]` (`s32[2]`, no bounds check) | `428` rows/file. Weapon sell price adds `20 * level + 5 * remaining_synth_points`; gift box adds contained items. |
| `SHOP shop, item_1, ...;` | `CShop::item_no[64]` (`SHOP_ITEM_MAX`) | `38` shops/file. Max **63 items per shop** (`shop` + 63 items = 64 `SPI_STACK_SIZE` args). `CheckEventItem` (`0x295370`) filters owned unique items and upgrades Ridepod cores. |

| Shop ID | Shopkeeper | Currency | Inventory Source |
|---|---|---|---|
| `23`, `28` | Cedric | Ridepod EXP (`ABS`) | `SHOP` line in `shopusa<chapter>.cfg` |
| `32` | Mayor Need | Medals | `SHOP 32` |
| `33` | Donny | Gilda | `dony_shoplist` (`0x358620`) filtered by Photography level (`SHOP 33` ignored) |
| All others | Standard shops | Gilda | `SHOP` line in `shopusa<chapter>.cfg` |

## 4. Monsters: `base_monster_define` in `SCES_511.90`

Located at virtual address `0x341DA0` (file offset `0x241E20`): 343 `BASE_MONSTER_TBL` records (`0xB8` bytes each, `include/monster.hpp`) plus 1 terminator record. `GetMonsterTable(id)` (`0x1DC230`) looks up `id` (`0..243`, `245..280` [245..268 = Mimics], `500..520`, `522..523`, `600..622`, `628..630`, `700..704`, `1000..1006`, `1500..1501`). Padding sits at `0x46`, `0x55`, `0x65`, `0xA6`, `0xB1`, `0xB6`.

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| `0x00` | `2` | `s16` | `id` | Unique monster ID (lookup key). |
| `0x02` | `2` | `s16` | `grade` | Monster rank/tier read by AI scripts. |
| `0x04` | `32` | `char[32]` | `name` | Overwritten at boot by `dungeon/cfg_file/mosdata<lang>.cfg`. |
| `0x24` | `16` | `char[16]` | `model` | Model pack (`dungeon/monster/<model>.chr`). |
| `0x34` | `16` | `char[16]` | `script` | AI script (`dungeon/monster/<script>.stb`). |
| `0x44` | `2` | `s16` | `gift_type` | Row index into `gift_item_tbl` (`0x351660`, section 9). |
| `0x48` | `4` | `s32` | `sound_bank` | Sound bank (`snd2/mon/EN_%03d.snd`; `<= 0` = none). |
| `0x4C` | `4` | `s32` | `unknown_4c` | Copied to monster `+0x10E0`; never read. |
| `0x50` | `4` | `s32` | `hp` | Base HP (overridable via `_SET_MONSTER_LIFE`). |
| `0x54` | `1` | `s8` | `family` | Family (`0..10`: Beast, Windup, Aquatic, ..., `9` = Undead); selects Monster Badge drop. |
| `0x56` | `2` | `u16` | `abs` | ABS dropped on defeat (`_SET_DEAD_OFF`). |
| `0x58` | `2` | `u16` | `money` | Gilda dropped on defeat (`_SET_DEAD_START`). |
| `0x5A` | `2` | `s16` | `unknown_5a` | Copied to monster `+0x10EE` and `+0x1322`; never read. |
| `0x5C` | `4` | `f32` | `weapon_wear` | WHP deducted from player's melee weapon per hit. |
| `0x60` | `2` | `s16` | `rage_hits` | Hits before entering Rage mode (`-1` = never, used for bosses). |
| `0x62` | `1` | `s8` | `guard_rate` | Chance (`0..100%`) to guard (`_SET_GUARD_RATE`). |
| `0x63` | `2` | `s8[2]` | `dodge_rates` | Chance (`0..100%`) to dodge melee and ranged attacks (`_SET_ESCAPE_RATE`). |
| `0x66` | `2` | `s16` | `attack` | Base attack power. |
| `0x68` | `1` | `u8` | `defence` | Flat damage reduction (`_SET_DEF_RATE`). |
| `0x69` | `1` | `s8` | `stagger` | Hit threshold to stagger (`0` = every hit). |
| `0x6A` | `1` | `s8` | `boss` | Non-zero shows full-screen boss health bar. |
| `0x6B` | `1` | `s8` | `sword_after_images` | Number of weapon trail after-image nodes. |
| `0x6C` | `16` | `s16[8]` | `elements` | Damage multiplier (`%`) for the 8 weapon elements (`>= 50` shown as weakness in Monster Book). |
| `0x7C` | `24` | `s16[12]` | `attack_kind_rates` | Damage multiplier (`%`) for the 12 attack categories (`vs_attk_index`, `0x351620`). |
| `0x94` | `4` | `u32` | `reaction_flags` | `CheckDamage` bitfield: `0x1` = never staggers, `0x4` = never knocked down. |
| `0x98` | `4` | `u32` | `flags` | Bit `0x1` = cannot be picked up or thrown. |
| `0x9C` | `4` | `s32` | `companion` | Secondary monster ID loaded alongside (`-1` = none). |
| `0xA0` | `4` | `s16[2]` | `drops` | Item drop IDs: `drops[0]` rolls 1-in-6; if it misses, `drops[1]` rolls 1-in-5. |
| `0xA4` | `2` | `s16` | `rare_drop` | Third item drop ID (rolls 1-in-4). |
| `0xA8` | `4` | `u32` | `status_immunity` | Immunity mask: `0x4` = Poison, `0x8`/`0x8000` = Petrify/Grey, `0x10000` = Slow. |
| `0xAC` | `2` | `s16` | `status_rate` | Susceptibility rate to status ailments. |
| `0xAE` | `2` | `s16` | `percent_damage_scale` | Scaling (`%`) for fractional-HP attacks. |
| `0xB0` | `1` | `s8` | `book_area` | Area category in the Monster Book. |
| `0xB2` | `2` | `s16` | `book_index` | Monster Book entry index (`-1` = hidden). |
| `0xB4` | `2` | `s16` | `shared_model` | Non-zero shares a single loaded model across instances. |

**Floor monster spawn tables (`dungeon/cfg_file/mos_place<dungeon>.cfg`)**:
Parsed by `LoadMonsterFile` (`src/dng_event.cpp`, `0x293BB0`) using `FLS floor, count; FL id_1, param_1, ...; FLE;`:
- `FL` writes `(id, param)` pairs into `CMonsterLocateInfo` (**hard limit of 32 pairs**: `_FL` at `0x292DB0` has no bounds check, and `32 * 2 = 64` hits `SPI_STACK_SIZE`). `AutoSetMonster` spawns up to 24 monsters per random floor.
- `param` sets `locate_param` (`-1` = non-talkable; `>= 0` = talkable via `CheckMonsterTolk`, triggering event `1150` and `_GET_MONSTER_TALK_DATA`).

## 5. Inventions, ideas and scoops

Photo subjects (`USER_PICTURE_INFO.neta_id`) come from map `FUNC_INVENT_DATA` points (`3,039` points across `121` `.map` files; see [maps](maps.md)) or monster AI `_SET_SCOOP` calls. IDs `< 1000` are **Ideas** (`[bulb2]`); IDs `>= 1000` are **Scoops**.

| Table / File | Loader / Symbol | Record Layout & Capacity Limits |
|---|---|---|
| Invention recipes (`menu/inv6.lst` and copies in `inv2_bg.pac`, `photo_bg1.pac`) | `CInventDataManage::LoadAnalyzeInventFile` (`0x201AC0`) | `DATATABLESET n; DATASET item, idea1, idea2, idea3, [mat_item, mat_cnt] × m, unk, scale, x, y, z;` (`m = (args - 9) / 2`, `1..4` materials). Populates `INVENT_DATA_INFO` (`0x24` B). `136` retail recipes (`128` items). **Max `234` recipes** in `InventTeigiStack` (`0x210` 16-byte units; event `CheckItemTable` holds `454`). |
| Scoops (`scoop_table` & `menu/<lang>/scoop.pac/scoop.cfg`) | ELF `0x352DE0` & `STR id, "text";` | `53` `SCOOP_DATA` records (`0x14` B: `s16 id`, `s16 menu_flag`, `s8 info_slot`, `char* text`). |

### 5.1 Ideas: `menu/<lang>/neta2.lst`

`LoadFilePictureName` reads `menu/<LanguageCode>/neta2.lst` (`6` falls back to `1/`) into a `0x5000`-byte buffer (`241` ideas `1..250` + `53` scoops; IDs `30000` and `30001` are ignored/reserved for NPC `260`):

```
PIC_INFO n;
PIC_NAME id, "name", sort;
```

| Buffer / Array | Capacity | Allocation & Ordering Rules |
|---|---|---|
| `pic_name_text_buff_1660` | `0x248` (`584`) 16-byte units (`583` usable) | Shared by `PIC_NAME_INFO` array (`ceil(n * 8 / 16) + 2` units) and strings (`ceil((strlen + 1) / 16)` units each). `ConvertFontCode` collapses `[UNIxxxx]` tags only when `LanguageCode >= 5` (`0x200FCC`), so FR/DE/IT keep 9-byte tags. Retail uses `491` (ja), `508` (en), `566` (fr), `533` (de), `539` (it), `536` (es) units. |
| `UpdataNetaMemoStr` `sort_keys[0x184]` | **`388`** combined ideas + scoops | Sorts only the first `known_ideas_count` entries by `sort = letter_group * 100 + rank` (`A = 0` in EN/FR/DE/IT; `¡ = 0, A = 1` in ES; kana row `0..50` in JA). All ideas (`< 1000`) must precede all scoops (`>= 1000`). |
| `MenuInventNetaMemoDraw` `text[0x20]` | **`24` B** for name (`32` B minus `"[bulb2]"`) | Formats `"[bulb2]" + name` (some retail FR/DE/ES names slightly overflow up to `33` B). |

## 6. Georama

For each town `g01`..`g05` and language `0..6`:

| File | Loader & Tag Table | Statements & Structs |
|---|---|---|
| `map/g/gNN/gNN<lang>.gpi` | `CEditInfoMngr::LoadEditInfo` (`emap_tag__2`, `0x358A30`) | `EDIT_PARTS_NUM n;` then `EDIT_PARTS "name"; ... EDIT_PARTS_END;` -> `CEditPartsInfo` (`0x280` B): `ID` (`0..127`), `PARTS_NAME` (must match `.map` part in `CMap::GetParts`), `PARTS_ATR`, `GROUND_PARTS` / `BLOCK_PARTS` / `RIVER_PARTS` / `FENCE_PARTS`, up to 4 `PARTS_MATERIAL item, count;`, `CPOINT cat, pts;`, `WEIGHT`, `GEO_STONE`, `MAX_NUM`, `PAINT_NUM`, `PAINT_USED`, `PARTS_TYPE`, `PLACE_EPS`, `MAP_NO`, `POLYN`, `PARTS_COMMENT`. |
| `map/g/gNN/gNN.cfg` | `CEditMap::LoadEditInfo` (`emap_tag`, `0x33ABB0`) | `FIX_EPARTS_START n;` / `FIX_EPARTS` and `INIT_EPARTS_START n;` / `INIT_EPARTS id, x, y, z, angle;` (fixed obstacles and starting parts), plus rivers and water (section 6.2). |

- **Georama limits**:
  - **`384` parts/town** (`GEORAMA_PARTS_LIST_MAX`), **part IDs `0..127`** (`UpdateGeoramaPartsList`; stock counts in `CSaveData::build_parts_num[0x100]` are global by ID across all 5 towns), and **`256` placed parts/town** (`CEditMap::CreateTable`).
  - Part name **`<= 47` bytes** (`GEORAMA_PARTS_LIST_ITEM::name[0x30]`). Town `g01` `.gpi` file **`<= 0x77D0` bytes** (`MakeDownLoadAnaunce` `0x7800`-byte stack buffer).
  - Hardcoded part IDs in C++: `7`, `0x13`, `0x1D`, `0x23`, `0x28..0x2B`, `0x2F` (conditions) and `1`, `9`, `0x16`, `0x1F`, `0x38`, `0x46..0x48`, `0x56`, `0x57` (houses).

### 6.1 Requests: `geo<language>.cfg`

`LoadEditAnalyzeData` (`src/editdata.cpp`, `tag__6` at `0x358B20`) reads `geo%d.cfg` into `AnalyzeSrc[5]` (`0x340` B/town). All strings share `buff_1271` (**`0x3000` B** = `0x300` 16-byte units):

| Statement | Arguments & Limits | Description |
|---|---|---|
| `GEO_ANALYZE` / `END_GEO_ANALYZE` | `town` (`0..4`) | Selects and clears town `0..4`. |
| `CONDITION` | `num, "text"[, geostone]` | Sets label for condition `0..63` and unlock floor (`dungeon * 100 + floor`). |
| `ANALYZE` / `END_ANALYZE` | `req, unread, "title"[, geostone]` | Defines request `0..15` (`data[16]`, `0x1C` B each; retail towns have `9, 10, 9, 11, 10`). |
| `CON_NO` | `cond_1, ...` (max **7**) | Condition indices (`0..63`) required to complete the request. |
| `ON_PARTS` / `OFF_PARTS` | `"part;part/piece;..."` | Up to 16 map parts (`64`-byte buffer) shown or hidden when fulfilled (`GetOnOffParts`, `PartsOnOff`). |
| `PERCENT` | `points` | Completion percentage added to the town. |

- **Hardcoded evaluation**: `AnalyzeSharlot`, `AnalyzeStera`, `AnalyzeBenietio`, `AnalyzeHeim`, and `AnalyzeMoonFlower` (`src/editanalyze.cpp`) set `condition[0..63]` booleans in C++. `.cfg` edits can recombine existing conditions or change `ON_PARTS`/`OFF_PARTS`/`PERCENT`, but cannot add new condition logic.

### 6.2 Rivers and water: `gNN.cfg`

| Loader & Tag Table | Statements | Description |
|---|---|---|
| `CEditMap::LoadEditInfo` (`emap_tag`, `0x33ABB0`) | `EDIT_RIVER` .. `EDIT_RIVER_END`, `RIVER_PARTS_NAME slot, "map_part", "model.mds";`, `WATER_PARTS_NAME "model.mds";`, `MASK_PARTS_NAME 0, "model.mds";` | Configures river piece slots `0..7` (`CEditPartsInfo[8]`, used in `g01` and `g05`). |
| `CMap::LoadCfgFile` (`cfg_tag`, `0x338BF0`) | `WATER_SURFACE_NUM n;`, `WATER_SURFACE_START` .. `WATER_SURFACE_END` (`WATER_VERTEX nx, nz, x0, y0, z0, x1, y1, z1;`, `WATER_POS x, y, z;`, `WATER_PARAM a, b, c, d;`, `WATER_SHAKE`), `WATER_DRAW_NUM n;`, `WATER_DRAW surface, "part_or_#followN", x, y, z;` | Animated water grids and draw attachments. |

## 7. Fishing

| Data Source | Location / Loader | Layout, Syntax & Limits |
|---|---|---|
| Species parameters | ELF `FishParam` (`0x361A50`) | `FISH_PARAM` (`0x54` B × 19; row `0` empty): name/model ptrs, item ID, base/min/max cm, weight & race pts/cm, pull strength, `s16[18]` bait preferences (`0..3`, indexed by `EsaInfo` at `0x361A00`), `s16[4]` time preferences. |
| Fishing spots | `sg/fish/fishing.cfg` (`LoadFishPlaceData`) | `FISH_MAP_NUM n; FISH_MAP map[, exclusive]; FISH_PLACE kind, "name", f1..f5; FISH species_idx, rate, wait_bias; FISH_MAP_END;`. Allocates `n` `FISH_PLACE_MAP` (`0x88` B) records (`22` retail blocks, `110` fish). Max **`8` `FISH` rows/spot** (`kFishPlaceMaxFish`, no bounds check) and **1 `FISH_PLACE`/block** (only `kind = 2` checks `x, z, radius`). `GetAppearFish` collects up to 16 fish (falls back to `map = -1`). |
| Finny Frenzy racing | ELF `fish_data` (`0x3630C0`) & `sg/gyo/rfd<lang>.cfg` | `grFISH_DATA` (`0x1C` B × 18) for terrain/scaling. Opponents in `rfd<lang>.cfg` (`4` classes, `216` fish): `NUM class, count; DAT item, "name", colour, power, spd1, spd2, spd3, stamina, size;`. |
| Race & contest prizes | `gyop.cfg` (`12` groups), `uofp.cfg` (`10` groups) (`LoadFishPrize`) | `GRP count, values...; PRIZE val, item1, cnt1, item2, cnt2, item3, cnt3;`. `RefreshFishPrize` picks group `3 * class + min(2, wins)` (race) or contest index and reads **only the first `PRIZE` row**. |
| Aquarium breeding | ELF `aquafish_mixTable` (`0x3535A0`) | `s8[171][3]` (section 9). |

## 8. Floor medals: `menu/dngmap/dflr<dungeon>.cfg`

`CDngFloorManager::LoadDataTable` (`src/dngfloor.cpp`) loads `dmap<dungeon>.cfg` and `dflr<dungeon>.cfg` (`tree_map_tag`, `0x3617B0`; `9, 16, 25, 21, 23, 28, 39` floors in dungeons `0..6`) into `DNGMAP_ROOM_INFO`:

| Statement | Arguments | Field Types & Meaning |
|---|---|---|
| `RF_INFO` | `floor, geostone, fast_time, fishing, fish_record, seal, spheda, item_1, count_1, item_2, count_2, item_3, count_3;` | `s8 geostone`, `s32 fast_time` (in `1/60` s frames), `s8 fishing` (`< 0`: `<= fish_record`; `> 0`: `>= fish_record`), `s16 fish_record` (`cm * 100`), `s8 seal` (`1` = Monica, `2` = Max), `s8 spheda`, and Spheda prizes `s16 item[3]`, `s8 count[3]`. |
| `RF_INFO2` | `floor, type, param;` | Practice/item medal (`IsClearPractice`): `s8 type` (`0` = time, `1..4` = kill-method/character category, `5` = no healing), `s32 param`. |

## 9. Characters and other executable tables

All tables below sit in the `.data` section of `SCES_511.90` at fixed sizes:

| Symbol | Virtual Address | File Offset | Layout | Purpose & Engine Consumer |
|---|---|---|---|---|
| `lifetbl_2854` | `0x37C790` | `0x27C810` | `f32[2]` | Starting and max HP for Max and Monica (`CUserDataManager::Initialize`). |
| `mos_henge_param` | `0x339D70` | `0x239DF0` | `0x1C × 57` | `s16 monster_id`, `s16 attack`, `s16 defence`, and effect names for Monica's 57 Monster Badge forms (`GetMonsterHengeParam`). |
| `monster_progress_tbl` | `0x358C90` | `0x258D10` | `s16[19][5]` | Monster Badge evolution trees (badge ID + monster ID at levels `1..4`). |
| `htbl_1662` | `0x33A490` | `0x23A510` | `s8[10]` | Pool of 10 random WHP increments on weapon level-up. |
| `weptbl_4503` | `0x33A510` | `0x23A590` | `s16[2][2][5]` | Starting equipment item IDs indexed by `[language != 0][chara][slot]` (`GetCharaDefaultWeapon`). |
| `etcitem_spectol_table` | `0x339820` | `0x2398A0` | `s8 { slot, value } × 425` | Spectrumising bonus for item IDs `1..425` (`SetItemSpectolPoint`). `slot`: `0..7` = Flame..Scale, `10` = Attack, `11` = Durability (`bonus = value * count_or_level`). |
| `dony_shoplist` | `0x358620` | `0x2586A0` | `{ s16 item, s8 level } × 7` (`+` term `item <= 0`) | Items unlocked in Donny's shop when photography level `> level` (`GetDonyShopLineUp`). |
| `gift_item_tbl` | `0x351660` | `0x2516E0` | `s16 { gift_type, item_id, unread } × 14` (`+ -1` term) | Monster Badge gift check (`CheckGiftPack`, `src/monster.cpp`, `0x1DF520`): all 3 items in a thrown Gift Capsule (`_COLPRIM_GET_GIFT`) must equal `item_id` for the monster's `gift_type`. |
| `GolfClubDef` | `0x361340` | `0x2613C0` | `{ f32 power, f32 unk_4, s32 unk_8 } × 7` | Spheda club stats (`GetSphidaClubDef` indexes `club_id - 9` for clubs `9..14`; row 7 is unused; `_SPHIDA_GET_CULB_DEF`). |
| `aquafish_mixTable` | `0x3535A0` | `0x253620` | `s8 { parent_a, parent_b, child }[171]` | Aquarium breeding (`GetChildFishNo`, `src/menuaqua.cpp`, `0x20ED80`): each byte is `fish_item_id - 0x136` (fish IDs `0x136..0x150`); matches parents in either order (defaults to `0x136`). |

## 10. Townspeople: `npc<language>.cfg`

`LoadNPCCfg` (`src/npccfg.cpp`) reads `npc%d.cfg` into a 64-byte-aligned `0x8000`-byte buffer (`0x7FD0` usable bytes) using `npc_spitag` (`NPC_NUM n; NPC_INFO number, "name", "model", kind, abilities, points, cost_1, cost_2, cost_3, cost_4, debug;`) and populates `NpcBaseData[180]` (`NPC_BASE_DATA`, `0x36` bytes):

| Argument | Struct Field | Meaning |
|---|---|---|
| `number` | `s16 chara_no` | Unique townsperson ID (`1..32` can join the party or live in Georama houses via `CUserDataManager::party_chara[32]`, `JoinPartyChara`). |
| `"name"` | `char name[0x1C]` | Localized display name (**27 bytes max**; `strcpy` runs before the `"NAME OVER"` check). |
| `"model"` | `char model[0x10]` | Model base name (**15 bytes max**) for `chara/%s.chr`, `event/train/t%s.chr`, and `menu/npc/t%s.chr`. |
| `kind` | `u8 unk_31` | Message category offset returned by `_GET_PARTY_CHARA_MES_NO` (query `4`). |
| `abilities` | `u8 ability_num` | Support ability count in the party menu (`0..4`). |
| `points` | `u8 max_npc_point` | Maximum ability/stamina points (`Kizuna` / Recruitment points). |
| `cost_1..4` | `u8 ability_cost[4]` | Point cost for each of the 4 abilities. |
| `debug` | `u8 debug_flag` | Debug menu selection flag. |

- **Capacity**: `npc_spi_count_num` (`u8`) increments without bounds checking. `NpcBaseData[180]` holds at most **180 records** (`+3` over the retail `177`).
- **Dialogue and placement**: Dialogue IDs in `npcmsg.mes` use `typetbl[type] + number * 100` (`GetPartyCharaMessage`). Town standing positions come from `npc_place*.cfg` (`VlgrPlace[0x200]`).
