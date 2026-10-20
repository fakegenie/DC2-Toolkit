"""Link input replay against an existing native build: python3 -I port/tests/gameplay.py [--build build/port]."""

import argparse
import importlib.util
import json
import pathlib
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=pathlib.Path, default=pathlib.Path("build/port"))
    options = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    build = options.build.resolve()
    out = build / "replay"
    out.mkdir(exist_ok=True)
    spec = importlib.util.spec_from_file_location("port_build", root / "tools/port.py")
    port = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(port)
    objects = []
    exports = {"title": ["_ZL9TitleInfo", "_ZL10TitlePhase", "_ZL16title_lang_phase"],
               "menuchr": ["_ZL10MenuCosPtr"], "nowload": ["_ZL8InitFlag"],
               "editloop": ["_ZL11ControlMode", "_ZL8LoopMode", "_ZL5MapNo"]}
    exports["dngmenu"] = ["_ZL11DngTreeMode"]
    exports["menuop"] = ["_ZL11SaveMenuPtr", "_ZL13MemoryCardPtr"]
    manifest = build / "objects.json"
    sources = [pathlib.Path(name) for name in json.loads(manifest.read_text())] if manifest.exists() else sorted((build / "obj").glob("*.o"))
    for source in sources:
        unit = source.stem
        if unit.startswith("engine_"):
            unit = unit.removeprefix("engine_")
        if unit in exports:
            target = out / source.name
            shutil.copyfile(source, target)
            subprocess.run([port.OBJCOPY, *["--globalize-symbol=" + port.PREFIX + name for name in exports[unit]], str(target)], check=True)
            objects.append(target)
        elif unit == "port_input":
            target = out / source.name
            shutil.copyfile(source, target)
            subprocess.run([port.OBJCOPY, f"--redefine-sym={port.PREFIX}port_input_read={port.PREFIX}port_input_read_device", str(target)], check=True)
            objects.append(target)
        else:
            objects.append(source)
    replay = out / "gameplay.o"
    subprocess.run(["clang++", "-std=c++26", *port.FLAGS, "-O2", f'-DSYMBOL_PREFIX="{port.PREFIX}"', "-include", str(root / "port/include/port.h"),
                    "-I" + str(root / "port/include"), "-I" + str(build / "copy/ps2/include"),
                    "-I" + str(build / "copy/ps2/include/sce"), "-c", str(root / "port/tests/gameplay.cpp"),
                    "-o", str(replay)], check=True)
    _, libraries = port.graphics_flags(build / "sdk")
    _, movie_libraries = port.movie_flags(build / "movie-sdk")
    link = [f"-Wl,-alias,_{target},_{alias}" for alias, target in port.ALIASES.items()] if port.DARWIN else \
        ["-no-pie", *(f"-Wl,--defsym={alias}={target}" for alias, target in port.ALIASES.items())]
    subprocess.run(["clang++", "-o", str(out / "darkchronicle"), *map(str, objects), str(replay),
                    *link, *port.LINK_FLAGS, *libraries, *movie_libraries], check=True)
    print(out / "darkchronicle")


if __name__ == "__main__":
    main()
