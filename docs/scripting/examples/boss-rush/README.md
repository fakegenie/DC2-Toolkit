# boss-rush: Boss Rush

Six boss fights back to back, timed. At the Underground Channel entrance in Palm Brinks the usual question becomes a choice: **Underground Channel**, **Boss Rush**, or **Stay in town**. The rush runs:

1. Halloween (Underground Channel)
2. Memo Eater (Yorda's Valley)
3. Gaspard and the burning lighthouse (Starlight Canyon)
4. Jaming and Shingala (Shigura Village)
5. Gaspard (Mount Gundor)
6. Monster Gaspard (Mount Gundor, straight after)

Each fight shows its own clock in the corner. After the last one the game shows the total time and the best time, kept in the save, and returns to Palm Brinks. Dying ends the rush. The fights are the game's own, with the characters and gear each one sets up (Gaspard and Jaming are fought as Monica), so the rush is best played with a game that is some way in.

## Building the mod

Copy this folder to `mods/boss-rush`, then build an image:

```sh
dc2tk build extracted/scripts dc2.iso -o boss-rush.iso --mod dc2tk.boss-rush
```

or a built mod for the native port's launcher:

```sh
dc2tk mod build extracted/scripts dc2.iso dc2tk.boss-rush -o build/port/mods
```

Add `--dev` to the image build to make **New Game** start the rush straight away (`"dev": {"start_at": "m01:RUSH_START"}`).

## How it works

Nothing here is new content: the mod chains fights the game already has. Every boss room has a `boss_*` program (`boss_halloween`, `boss_memo_eater`, ...) that the debug menu uses to start the fight without its story scenes, and every boss tells its room it has died by starting a numbered program there (Halloween starts 520, Memo Eater 500). The mod overrides that program's function in each room: while the rush is on it fades out and goes to the next boss, otherwise it runs the game's own.

| File | What it changes |
| --- | --- |
| `rush.h` | The rush's save flag and counters (`NEW_FLAG`, `NEW_COUNTER`), the list of fights (`rush_enter`), and what every room shares: the clock, winning, losing |
| `m01.c` | Palm Brinks: the three-way question at the Channel entrance (program 510), and `RUSH_START`, which starts the rush as Max |
| `d01b01.c`, `s18.c`, `d03b02.c`, `d04b01.c` | One boss room each: start the clock when the fight is set up, go on when the boss dies, end the rush on a game over |
| `d05.c` | Mount Gundor's floor script gets the `boss_gaspard_2` program the other dungeons have, which loads Gaspard's room |
| `d05b01.c` | Gaspard's room: after Gaspard, Monster Gaspard is set up in place (as the story does, without its scene); after Monster Gaspard, the results |

A fight is entered with `go_to_dungeon(dungeon, floor, program)`, the way the debug menu enters it. A town's `jump_to_map` would load the boss room as a town, without the fight. The total time is the save's play time at the end less the play time at the start, which the rush keeps in two counters. The engine's stopwatch, which draws the clock, starts again on every map load, so each fight shows its own time.

## Testing it

The native port's debug build has a `defeat` command that kills every monster on the floor through its own death program, the same path a real win takes:

```sh
python3 -I tools/debug.py town map=m01 event=510
python3 -I tools/debug.py defeat
```

`defeat list=1` lists the living monsters and their life without killing them.

The text is in English in every language.
