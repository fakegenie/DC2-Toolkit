# DC2-Toolkit

<p align="center">
  <img src="docs/images/header.png" alt="A Dark Chronicle town map with its trigger points, beside decompiled scripts" width="830">
</p>

<p align="center">
  <a href="https://fakegenie.github.io/DC2-Toolkit/"><b>Documentation</b></a>
  &nbsp;&nbsp;·&nbsp;&nbsp;
  <a href="https://github.com/fakegenie/DC2-Toolkit/releases"><b>Download Releases</b></a>
</p>

**DC2-Toolkit / dc2tk** is a set of tools for modifying the PS2 game Dark Chronicle/Dark Cloud 2. With them you can extract or modify almost everything in the game and even create total conversions. We support extracting, converting and injecting:

- **Scripting** DC2 heavily uses its own scripting language for almost all behaviour in the game. dc2tk can extract all the game's scripts as human readable C code, and compile them back into the game's original bytecode format.
- **Mapping** All game maps can be extracted to modern formats, and we provide a Blender plugin to help editing/creating them and a compiler to convert them to the original DC2 Format.
- **Models/Textures/Audio** We can extract and convert all to gltf/png/wav/midi files, and can convert them all back. 
- **Text/Config/Data** Menus, dialogue, shop items, etc in every language, all editable as text and json files

The goal of this project is to support converting from and to the original game formats, not creating anything new. All tools support doing a full byte-identical round-trip. The tools can output an updated .iso image you can play on a ps2 or emulator, and probably most recomp ports.

> **No game files are included.** This repository holds tools and documentation,
> not decompiled code or assets. You need your own legally obtained copy of a
> PAL PS2 version of the game for anything here to work.


## Quick Start

- Download the latest release.
- Extract the zip somewhere and copy your PAL iso to the folder as "dc2.iso"
- Run `dc2tk setup dc2.iso`. This will extract all the assets on the disc to `extracted/`
- Optionally install the Blender plugin from the releases page.
- Make a mod in `mods/`, or edit the scripts and assets in `extracted/` and package them with `dc2tk mod make dc2.iso my-mod`. See the documentation for how-tos.
- Run `dc2tk build extracted/scripts dc2.iso -o modded.iso --mod local.my-mod` (the id `mod make` gave it) to build a new iso with your changes.

## Playing on PC

Each release also has a native port of the game for macOS (Apple silicon, macOS 13 or later) and Linux (x86-64): `dark-chronicle-<version>-macos-arm64.zip` holds `Dark Chronicle.app`, and `dark-chronicle-<version>-linux-x64.tar.gz` holds `darkchronicle`. It contains no game data and plays from a disc image of your own PAL copy, which you pick in its launcher, where built mods (`.dc2pak`) are turned on and off too. The app is not notarised: the first time, open it from System Settings, Privacy & Security, Open Anyway. The `README.md` in the download covers the controls and where saves and settings are kept. `dark-chronicle-<version>-ios.ipa` is the same for iPhone and iPad (iOS 17 or later): it is unsigned, so install it with a sideloading tool such as AltStore or Sideloadly, which signs it with your own Apple ID. `dark-chronicle-<version>-android-arm64.apk` is the same for 64-bit Android phones and tablets (Android 10 or later, with Vulkan): allow installing apps from your browser or file manager and open it, and later releases install over it. A Windows build is coming.

## Notes

- We only support PAL at the moment
- The engine's code is [ChronicleTwo](https://github.com/TheMoonPeople/ChronicleTwo)'s decompilation, which ChronicleTwo hosts. This repository pins it as a git submodule at `engine/` and holds none of it; `dc2tk engine fetch` checks it out (clone with `--recurse-submodules` to get it with the repository), and `dc2tk engine build` builds the game's executable from it with ChronicleTwo's own build, in its container (Docker or Podman).

## Prior art

[ChronicleTwo](https://github.com/TheMoonPeople/ChronicleTwo), [OpenCloud](https://github.com/Gundorada-Workshop/OpenCloud), [SB2-File-Parser](https://github.com/ArsonAssassin/SB2-File-Parser), [DarkCloud-Two-Reforged](https://github.com/ZeldoKavira/DarkCloud-Two-Reforged), [DarkCloud-Toolkit](https://github.com/Gundorada-Workshop/DarkCloud-Toolkit)

## Licence

dc2tk, its SDK, definitions, Blender add-on, documentation and the port's own code (`port/`) are under the [MIT licence](LICENCE). Dark Chronicle and its content (code, art, text, audio, and screenshots of it such as `docs/images/header.png`) belong to Level-5 and Sony Interactive Entertainment and are not covered by it. The port's engine is [ChronicleTwo](https://github.com/TheMoonPeople/ChronicleTwo)'s decompilation, used with their permission and not covered by the MIT licence either ([port/package/NOTICE.md](port/package/NOTICE.md)). Every release zip carries `THIRD-PARTY-LICENCES.md`: for dc2tk the licences of the crates built into it, and for the port those of SDL3 (zlib), FFmpeg's MPEG-2 decoder (LGPL 2.1: replaceable shared libraries on macOS and Linux, linked into the iOS and Android apps, whose complete source here lets you relink them), the Manrope font (OFL), stb_truetype and Kazarov's libvu0 (MIT).
