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
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--trace", action="store_true")
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
            subprocess.run(["objcopy", *["--globalize-symbol=" + name for name in exports[unit]], str(target)], check=True)
            objects.append(target)
        else:
            objects.append(source)
    replay = out / "gameplay.o"
    subprocess.run(["clang++", "-std=c++26", *port.FLAGS, "-O2", *(["-DREPLAY_PROFILE"] if options.profile else []), "-include", str(root / "port/include/port.h"),
                    "-I" + str(root / "port/include"), "-I" + str(build / "copy/ps2/include"),
                    "-I" + str(build / "copy/ps2/include/sce"), "-c", str(root / "port/tests/gameplay.cpp"),
                    "-o", str(replay)], check=True)
    _, libraries = port.graphics_flags(root / "build/port/sdk")
    movie = root / "build/port/movie-sdk/lib"
    profile = ["-Wl,--wrap=" + name for name in ["port_gs_raster", "port_gs_raster_enqueue", "port_gs_raster_flush", "port_vu1_start", "port_vu1_continue"]] if options.profile else []
    if options.trace:
        trace = out / "gs_trace.o"
        subprocess.run(["clang", "-std=c11", "-O2", "-fno-pie", "-I" + str(root / "port/include"),
                        "-c", str(root / "port/tests/gs_trace.c"), "-o", str(trace)], check=True)
        objects.append(trace)
        profile += ["-Wl,--wrap=" + name for name in ["port_gs_reset", "port_gif_submit", "port_gif_feed",
                    "port_gs_write_register", "port_gs_image_load", "port_gs_image_store", "port_gs_read_rgba"]]
    subprocess.run(["clang++", "-pthread", "-no-pie", "-o", str(out / "darkchronicle"), *map(str, objects), str(replay),
                    "-Wl,--wrap=port_input_read", *profile, *[f"-Wl,--defsym={a}={b}" for a, b in port.ALIASES.items()],
                    *libraries, "-L" + str(movie), "-Wl,-rpath," + str(movie), "-lavcodec", "-lavutil"], check=True)
    print(out / "darkchronicle")


if __name__ == "__main__":
    main()
