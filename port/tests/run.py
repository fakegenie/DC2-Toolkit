"""Build and run the native boundary checks: python3 -I port/tests/run.py."""

import argparse
import importlib.util
import json
import pathlib
import shutil
import subprocess
import sys

DARWIN = sys.platform == "darwin"
NO_PIE = [] if DARWIN else ["-fno-pie", "-no-pie"]
SEGMENTS = ["-Wl,-segaddr,__PS2,0x110000000", "-Wl,-segprot,__PS2,rw,rw",
            "-Wl,-segaddr,__PS2SPR,0x170000000", "-Wl,-segprot,__PS2SPR,rw,rw"] if DARWIN else []
DEAD_STRIP = "-Wl,-dead_strip" if DARWIN else "-Wl,--gc-sections"
OBJCOPY = (shutil.which("llvm-objcopy") or "/opt/homebrew/opt/llvm/bin/llvm-objcopy") if DARWIN else "objcopy"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iso", type=pathlib.Path, help="also decode the retail logo and tutorial movies")
    options = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    include = ["-Iport/include", "-Iengine/ps2/include", "-Iengine/ps2/include/sce"]
    c_flags = ["clang", "-std=c11", "-pthread", "-Wall", "-Wextra", "-Werror", *(["-Wno-deprecated-declarations"] if DARWIN else []), *include]
    cpp_flags = ["clang++", "-std=c++26", "-pthread", "-O2", "-DPORT", "-DPAL", "-fms-extensions", "-w",
                 "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off", "-fno-strict-return",
                 "-ftrivial-auto-var-init=zero",
                 "-include", "port/include/port.h", *include]
    cases = [
        ("devices", [*c_flags, "-Wno-unused-parameter", "port/tests/devices.c", "port/src/devices.c", "port/src/iop.c", "port/src/audio.c", "port/src/midi.c", "port/src/spu2.c", "port/src/cdvd.c", "port/src/overrides.c", "-lm", "port/src/hooks.c"]),
        ("graphics", [*c_flags, *NO_PIE, *SEGMENTS, "port/tests/graphics.c", "port/src/graphics.c", "port/src/packets.c", "port/src/memory.c", "port/src/hooks.c"]),
        ("kernel", [*c_flags, *NO_PIE, "port/tests/kernel.c", "port/src/fiber.c"]),
        ("memory", [*c_flags, *NO_PIE, *SEGMENTS, "-pthread", "port/tests/memory.c", "port/src/memory.c", "port/src/hooks.c"]),
        ("runtime", [*c_flags, "port/tests/runtime.c"]),
        ("sound", [*c_flags, "port/tests/sound.c", "port/src/audio.c", "port/src/midi.c", "port/src/spu2.c", "-lm", "port/src/hooks.c"]),
        ("midi", [*c_flags, "port/tests/midi.c", "port/src/midi.c", "-lm"]),
        ("memory_card", [*c_flags, "port/tests/memory_card.c", "port/src/memory_card.c"]),
        ("executable", [*c_flags, "port/tests/executable.c", "port/src/executable.c"]),
        ("input", [*c_flags, "port/tests/input.c", "port/src/input.c", "port/src/hooks.c"]),
        ("mglib", [*cpp_flags, "-ffunction-sections", "-fdata-sections", "port/tests/mglib.cpp", "port/src/mglib.cpp", DEAD_STRIP]),
        ("dma", [*cpp_flags, *NO_PIE, *SEGMENTS, "port/tests/dma.cpp", "port/src/dma.cpp", "port/src/memory.c", "port/src/hooks.c"]),
        ("math", [*cpp_flags, "-ffunction-sections", "-fdata-sections", "port/tests/math.cpp",
                  "port/src/frame.cpp", "port/src/collision.cpp",
                  "engine/tools/kazarov/src/cpp/sce/libvu0.cpp", DEAD_STRIP]),
    ]
    temporary = root / "build" / "port" / "tests"
    shutil.rmtree(temporary, ignore_errors=True)
    temporary.mkdir(parents=True)
    movie_object = str(pathlib.Path(temporary) / "movie_decode.o")
    subprocess.run([*c_flags, "-Wno-unused-parameter", "-c", "port/src/movie_decode.c", "-o", movie_object], cwd=root, check=True)
    cases.append(("movie", [*cpp_flags, "port/tests/movie.cpp", "port/src/movie.cpp", movie_object]))
    headers = pathlib.Path(temporary) / "include"
    shutil.copytree(root / "engine/ps2/include", headers)
    patches = json.loads((root / "tools/engine-patches/port.json").read_text())
    for patch in patches:
        if not patch["file"].startswith("ps2/include/"):
            continue
        path = headers / patch["file"].removeprefix("ps2/include/")
        header = path.read_text()
        count = patch.get("count", 1)
        assert count > 0 and header.count(patch["find"]) == count
        path.write_text(header.replace(patch["find"], patch["replace"], count))
    cases.append(("records", [cpp_flags[0], "-I" + str(headers), *cpp_flags[1:], *NO_PIE, "port/tests/records.cpp"]))
    sources = {}
    for name in ["mg_drawprim.cpp", "mg_texture.cpp", "mg_memory.cpp", "mg_tanime.cpp", "mg_math.cpp", "automap.cpp", "runscript.cpp", "runscript_opcodes.cpp", "effscript.cpp"]:
        relative = "ps2/src/" + name
        source = (root / "engine" / relative).read_text()
        for patch in patches:
            if patch["file"] == relative:
                count = patch.get("count", 1)
                assert count > 0 and source.count(patch["find"]) == count
                source = source.replace(patch["find"], patch["replace"], count)
        path = pathlib.Path(temporary) / name
        path.write_text(source)
        sources[name] = str(path)
    cases.append(("drawprim", [cpp_flags[0], "-I" + str(headers), *cpp_flags[1:], "-ffunction-sections", "-fdata-sections", "port/tests/drawprim.cpp", sources["mg_drawprim.cpp"], sources["mg_texture.cpp"], sources["mg_memory.cpp"], DEAD_STRIP]))
    cases.append(("texture_anime", [cpp_flags[0], "-I" + str(headers), *cpp_flags[1:], "-ffunction-sections", "-fdata-sections", "port/tests/texture_anime.cpp", sources["mg_tanime.cpp"], sources["mg_memory.cpp"], DEAD_STRIP]))
    cases.append(("automap", [cpp_flags[0], "-I" + str(headers), *cpp_flags[1:], "-ffunction-sections", "-fdata-sections", "port/tests/automap.cpp", sources["automap.cpp"], DEAD_STRIP]))
    cases.append(("monster_script", [cpp_flags[0], "-I" + str(headers), *cpp_flags[1:], "-ffunction-sections", "-fdata-sections", "port/tests/monster_script.cpp", sources["runscript.cpp"], sources["runscript_opcodes.cpp"], sources["mg_memory.cpp"], DEAD_STRIP]))
    cases.append(("effect_heap", [cpp_flags[0], "-I" + str(headers), *cpp_flags[1:], *NO_PIE, "-ffunction-sections", "-fdata-sections", "port/tests/effect_heap.cpp", sources["effscript.cpp"], sources["runscript.cpp"], sources["mg_memory.cpp"], DEAD_STRIP]))
    collision_objects = []
    for name in ["mg_math.cpp", "collision_asm.cpp", "gameutil_asm.cpp"]:
        target = pathlib.Path(temporary) / ("collision_" + name + ".o")
        subprocess.run([cpp_flags[0], "-I" + str(headers), *cpp_flags[1:],
                        "-Iengine/port/include", "-Iengine/port/src", "-ffunction-sections", "-fdata-sections",
                        "-c", "engine/port/src/" + name, "-o", str(target)], cwd=root, check=True)
        subprocess.run([OBJCOPY, "--weaken", str(target)], cwd=root, check=True)
        collision_objects.append(str(target))
    cases.append(("collision", [cpp_flags[0], "-I" + str(headers), *cpp_flags[1:],
                                "-ffunction-sections", "-fdata-sections", "port/tests/collision.cpp",
                                "port/src/collision.cpp", sources["mg_math.cpp"], *collision_objects,
                                "engine/tools/kazarov/src/cpp/sce/libvu0.cpp", DEAD_STRIP]))
    failed = []
    for name, command in cases:
        binary = pathlib.Path(temporary) / name
        try:
            subprocess.run([*command, "-o", str(binary)], cwd=root, check=True)
            subprocess.run([str(binary)], cwd=root, check=True, timeout=15)
            print(f"{name}: passed", flush=True)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
            print(f"{name}: FAILED ({error})", flush=True)
            failed.append(name)
    math_outputs = []
    for level in ["-O0", "-O2"]:
        target = pathlib.Path(temporary) / ("math_bits" + level)
        subprocess.run([*cpp_flags, *(["-fmath-errno"] if DARWIN else []), level, "-ffunction-sections", "-fdata-sections",
                        "port/tests/math_bits.cpp", "port/src/frame.cpp", "port/src/collision.cpp",
                        "engine/tools/kazarov/src/cpp/sce/libvu0.cpp", DEAD_STRIP, "-o", str(target)], cwd=root, check=True)
        result = subprocess.check_output([str(target)], cwd=root, text=True, timeout=15)
        math_outputs.append(result.split()[0])
    if math_outputs[0] != math_outputs[1]:
        raise RuntimeError("optimized matrix, clipping and collision results differ from the O0 build")
    print("math_bits: O0/O2 outputs match", flush=True)
    movie_test = pathlib.Path(temporary) / "movie_decode"
    movie_flags = []
    movie_libraries = []
    if options.iso:
        spec = importlib.util.spec_from_file_location("port_build", root / "tools/port.py")
        port = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(port)
        movie_cflags, movie_libraries = port.movie_flags(root / "build/port/movie-sdk")
        movie_flags = ["-DPORT_MOVIES", *movie_cflags]
    subprocess.run([*c_flags, "-Wno-unused-parameter", *movie_flags,
                    "port/tests/movie_decode.c", "port/src/movie_decode.c", "port/src/cdvd.c", "port/src/overrides.c", "port/src/hooks.c",
                    *movie_libraries, "-o", str(movie_test)], cwd=root, check=True)
    subprocess.run([str(movie_test)], cwd=root, check=True, timeout=15)
    if options.iso:
        for name in ["L5LOGO.PSS", "TUTO/VTUTO6.PSS"]:
            subprocess.run([str(movie_test), str(options.iso.resolve()), name], cwd=root, check=True, timeout=60)
    print("movie_decode: passed", flush=True)
    subprocess.run([sys.executable, "-I", "port/tests/cdvd.py"], cwd=root, check=True, timeout=30)
    if failed:
        sys.exit("failed: " + ", ".join(failed))


if __name__ == "__main__":
    main()
