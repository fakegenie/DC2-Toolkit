# DC2-Toolkit

<p align="center">
  <img src="images/header.png" alt="A DC2 town map with its trigger points, beside decompiled scripts" width="830">
</p>

<p align="center">
  <a href="https://github.com/fakegenie/DC2-Toolkit/"><b>GitHub Repository</b></a>
  &nbsp;&nbsp;·&nbsp;&nbsp;
  <a href="https://github.com/fakegenie/DC2-Toolkit/releases"><b>Download Releases</b></a>
</p>

**DC2-Toolkit (`dc2tk`)** is a toolset for modding the PlayStation 2 game DC2 (PAL version). It extracts the game into standard formats (`.png`, `.wav`, `.mid`, `.gltf`, `.json`, and `.c` scripts) and builds your changes into a playable `.iso` disc image.

> **Note:** No game files are included. You need your own legally obtained PAL PlayStation 2 `.iso` of DC2 (`SCES_511.90`).

## Quick Start

1. Download and extract the latest release from [GitHub Releases](https://github.com/fakegenie/DC2-Toolkit/releases).
2. Copy your PAL DC2 `.iso` into the folder and name it `dc2.iso`.
3. Extract the game files:
   ```sh
   dc2tk setup dc2.iso
   ```
4. Create a mod in `mods/my-mod/` (see the [Getting Started Guide](guide/README.md)).
5. Build a new `.iso` with your mod and open it in [PCSX2](https://pcsx2.net/):
   ```sh
   dc2tk build extracted/scripts dc2.iso -o modded.iso --mod local.my-mod
   ```

## Documentation

- **[Guide](guide/README.md)** — How to get started, build your first starter mod, use the CLI, and edit game data, textures, text, audio, 3D models, maps, and dungeons.
- **[Scripting](scripting/README.md)** — How scripts work and their lifecycle, what a `PROGRAM` is, step-by-step example guides, and the full [API Reference](scripting/api/README.md).
- **[DC2 Internals](internals/README.md)** — Technical specifications of the original game's binary file formats, bytecode VM, data tables, and memory layout for reverse engineers.
