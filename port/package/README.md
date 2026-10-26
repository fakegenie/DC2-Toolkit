# DC2 for PC

A native port of the PlayStation 2 game DC2 for macOS, Linux and Windows, from
[DC2 Toolkit](https://github.com/fakegenie/DC2-Toolkit). It is built from
[ChronicleTwo](https://github.com/TheMoonPeople/ChronicleTwo)'s decompilation of the game's engine, with its code used
with permission.

**No game data is included.** The port plays from a disc image of your own copy of the PAL release, `SCES-51190`. Other
releases do not work yet.

## Starting

- **macOS** (Apple silicon, macOS 13 or later): move `DC2.app` to Applications and open it. The app is not
  notarised, so the first time macOS refuses to open it: go to System Settings, Privacy & Security, and choose Open
  Anyway. Running `xattr -dr com.apple.quarantine "/Applications/DC2.app"` in Terminal does the same.
- **Linux** (x86-64, with a Vulkan driver): run `./dc2` from this folder.
- **Windows** (x64, Windows 10 version 1903 or later, with a Vulkan driver): unzip the folder and open
  `DC2.exe`. It is not signed, so Windows may say it protected your PC: choose More info, then Run anyway.

The launcher opens first. Pick your disc image in its Disc row, choose the window, widescreen, frame rate and sound
settings, and start the game. They are remembered for the next time. On Linux, `./dc2 dc2.iso` skips the
launcher and starts the game straight away, as `DC2.exe dc2.iso` does on Windows.

## Controls

A controller works as a DualShock 2. On the keyboard:

| PS2 | Key | PS2 | Key |
|---|---|---|---|
| Left stick | W A S D | Right stick | T F G H |
| D-pad | Arrow keys | Cross, Circle, Square, Triangle | K, L, J, I |
| L1, R1 | Q, E | L2, R2 | 1, 3 |
| L3, R3 | C, V | Start, Select | Return, Backspace |

## Your files

| | macOS | Linux | Windows |
|---|---|---|---|
| Saves | `~/Library/Application Support/DC2 Toolkit/DC2/saves`, or `DC2/saves` in iCloud Drive when iCloud Drive has a `DC2` folder | `~/.local/share/DC2 Toolkit/DC2/saves` | `%APPDATA%\DC2 Toolkit\DC2\saves` |
| Settings (`launcher.cfg`) | `~/Library/Application Support/DC2 Toolkit/DC2` | `~/.local/share/DC2 Toolkit/DC2` | `%APPDATA%\DC2 Toolkit\DC2` |
| Mods (`.dc2pak`) | `mods` in the settings folder | `mods` beside `dc2` | `mods` beside `DC2.exe` |

Each memory card is a folder, `card0` and `card1`, in the saves folder. Set `DC2_SAVE_ROOT` to keep saves somewhere
else. Mods built with `dc2tk mod build` go in the mods folder, or drop one on the launcher, and its Mods page turns each
one on and off.

## Licences

The port's own code is under the MIT licence in `LICENCE`. `NOTICE.md` says who owns the rest, and
`THIRD-PARTY-LICENCES.md` holds the licences of the libraries and fonts it uses.
