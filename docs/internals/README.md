# DC2 Internals

This section documents the engine architecture and binary file formats of DC2 (PAL PS2 `SCES_511.90`). These specifications describe the retail game files and engine behavior, independent of any modding tool.

- For instructions on how to modify game files, see [Guide](../guide/README.md).
- For instructions on how to write script code, see [Scripting](../scripting/README.md).

All specifications come from the [ChronicleTwo](https://github.com/TheMoonPeople/ChronicleTwo) engine decompilation and verification against the PAL disc files. Each page cites the relevant engine source files (paths are relative to a ChronicleTwo checkout) and provides counts across the entire PAL disc.

## Engine and scripts

The game divides work between the native engine and bytecode scripts:

- **Engine** (the PS2 executable): Renders graphics, runs physics and collision detection, plays animations and audio, and manages the static data tables.
- **Scripts**: Control game logic, including cutscenes, conversations, doors, treasure chests, monster AI, player weapon combos, and spell or explosion trajectories.

Scripts call into the engine through approximately 1,100 built-in functions called *natives*. The engine calls into scripts by starting numbered script *programs* when specific events occur. The game uses five script categories, and each category binds its own table of native functions:

- Map and event scripts
- Monster AI scripts
- Player action scripts
- Effect scripts
- Argument files

See [the big picture](../scripting/README.md#the-big-picture) for details on what each script category controls.

## The pages

| Page | Contents |
|---|---|
| [The disc](disc.md) | ISO 9660 layout, the `DATA.DAT` archive, the `DATA.HD4` index, and pack files |
| [The script format](scripts.md) | `.stb` bytecode files, the virtual machine (VM) instruction set, and native function tables |
| [How scripts run](running.md) | Script programs, event mode, shared state (save flags, save counters, monster variables, and script globals), language variants, dungeon-triggered events, and message text control codes |
| [Maps](maps.md) | `.map`, `.pcp`, `.mpk`, and `.ipk` formats, collision meshes, part placement, trigger points, animated pieces, and map categories |
| [Dungeon floors](dungeons.md) | Procedural floor generation, room configuration scripts, part templates, treasure chests, and monster spawns |
| [Character models](models.md) | `.mds` meshes, skinning data (`.wgt` and `.bbp`), `.mot` motion files, `info.cfg`, and `.clo` clothing files |
| [Textures](textures.md) | `.img` archives of TIM2 images, pixel formats, palettes, and engine texture lookup |
| [Sound banks](sounds.md) | `.snd` packs, internal `.cfg`, `.vol`, `.hd`, and `.bd` files, and sound effect voice allocation |
| [Music](music.md) | `.sq` sequences for background music and ambient loops, and sequence playback |
| [Message text](text.md) | `.mes` files for menus, shops, maps, and NPCs, including font glyphs and layout codes |
| [Game data](data.md) | File locations and binary record layouts for items, shops, monsters, inventions, Georama, fishing, medals, and character stats |
| [Memory](memory.md) | Fixed memory buffers and current memory usage across the PAL disc |
