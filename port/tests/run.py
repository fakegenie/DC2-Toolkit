"""Build and run the native boundary checks: python3 -I port/tests/run.py."""

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iso", type=pathlib.Path, help="also decode the retail logo and tutorial movies")
    options = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    include = ["-Iport/include", "-Iengine/ps2/include", "-Iengine/ps2/include/sce"]
    c_flags = ["clang", "-std=c11", "-pthread", "-Wall", "-Wextra", "-Werror", *include]
    cpp_flags = ["clang++", "-std=c++26", "-pthread", "-O2", "-DPORT", "-DPAL", "-fms-extensions", "-w",
                 "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off", "-fno-strict-return",
                 "-ftrivial-auto-var-init=zero",
                 "-include", "port/include/port.h", *include]
    cases = [
        ("devices", [*c_flags, "-Wno-unused-parameter", "port/tests/devices.c", "port/src/devices.c", "port/src/iop.c", "port/src/iop_sound.c", "port/src/iop_midi.c", "port/src/spu2.c", "port/src/cdvd.c", "-lm"]),
        ("graphics", [*c_flags, "port/tests/graphics.c", "port/src/graphics.c", "port/src/packets.c"]),
        ("kernel", [*c_flags, "-fno-pie", "-no-pie", "port/tests/kernel.c"]),
        ("memory", [*c_flags, "-fno-pie", "-no-pie", "-pthread", "port/tests/memory.c", "port/src/memory.c"]),
        ("runtime", [*c_flags, "port/tests/runtime.c"]),
        ("sound", [*c_flags, "port/tests/sound.c", "port/src/iop_sound.c", "port/src/iop_midi.c", "port/src/spu2.c", "-lm"]),
        ("iop_midi", [*c_flags, "port/tests/iop_midi.c", "port/src/iop_midi.c", "-lm"]),
        ("memory_card", [*c_flags, "port/tests/memory_card.c", "port/src/memory_card.c"]),
        ("executable", [*c_flags, "port/tests/executable.c", "port/src/executable.c"]),
        ("input", [*c_flags, "port/tests/input.c", "port/src/input.c"]),
        ("dma_vif", [*c_flags, "port/tests/dma_vif.c", "port/src/dma_chain.c", "port/src/vif.c"]),
        ("vu1", [*c_flags, "port/tests/vu1.c", "port/src/vu1.c", "-lm"]),
        ("vu1_numbers", [*c_flags, "-O2", "port/tests/vu1_numbers.c", "-lm"]),
        ("vu1_blocks", [*c_flags, "-O2", "port/tests/vu1_blocks.c", "-lm"]),
        ("gs_memory", [*c_flags, "port/tests/gs_memory.c", "port/src/gs_memory.c"]),
        ("gs_clut", [*c_flags, "port/tests/gs_clut.c", "port/src/gs_clut.c", "port/src/gs_memory.c"]),
        ("gs_raster", [*c_flags, "port/tests/gs_raster.c", "port/src/gs_raster.c", "port/src/gs_memory.c", "port/src/gs_clut.c", "-lm"]),
        ("gs_scenes", [*c_flags, "-O2", "port/tests/gs_scenes.c", "port/src/gs_raster.c", "port/src/gs_memory.c", "port/src/gs_clut.c", "-lm"]),
        ("gs_bands", [*c_flags, "-O2", "port/tests/gs_bands.c", "port/src/gs_raster.c", "port/src/gs_memory.c", "port/src/gs_clut.c", "-lm"]),
        ("gs_batch", [*c_flags, "-O2", "port/tests/gs_batch.c", "port/src/gs_raster.c", "port/src/gs_memory.c", "port/src/gs_clut.c", "-lm"]),
        ("gs", [*c_flags, "port/tests/gs.c", "port/src/gs.c", "port/src/gs_raster.c", "port/src/gs_memory.c", "port/src/gs_clut.c", "-lm"]),
        ("mglib", [*cpp_flags, "port/tests/mglib.cpp", "port/src/mglib.cpp"]),
        ("dma", [*cpp_flags, "port/tests/dma.cpp", "port/src/dma.cpp"]),
        ("math", [*cpp_flags, "-ffunction-sections", "-fdata-sections", "port/tests/math.cpp",
                  "port/src/frame.cpp", "port/src/collision.cpp",
                  "engine/tools/kazarov/src/cpp/sce/libvu0.cpp", "-Wl,--gc-sections"]),
    ]
    with tempfile.TemporaryDirectory(prefix="dc2-port-tests-") as temporary:
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
        cases.append(("records", [cpp_flags[0], "-I" + str(headers), *cpp_flags[1:], "-fno-pie", "-no-pie", "port/tests/records.cpp"]))
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
        cases.append(("drawprim", [cpp_flags[0], "-I" + str(headers), *cpp_flags[1:], "-ffunction-sections", "-fdata-sections", "port/tests/drawprim.cpp", sources["mg_drawprim.cpp"], sources["mg_texture.cpp"], sources["mg_memory.cpp"], "-Wl,--gc-sections"]))
        cases.append(("texture_anime", [cpp_flags[0], "-I" + str(headers), *cpp_flags[1:], "-ffunction-sections", "-fdata-sections", "port/tests/texture_anime.cpp", sources["mg_tanime.cpp"], sources["mg_memory.cpp"], "-Wl,--gc-sections"]))
        cases.append(("automap", [cpp_flags[0], "-I" + str(headers), *cpp_flags[1:], "-ffunction-sections", "-fdata-sections", "port/tests/automap.cpp", sources["automap.cpp"], "-Wl,--gc-sections"]))
        cases.append(("monster_script", [cpp_flags[0], "-I" + str(headers), *cpp_flags[1:], "-ffunction-sections", "-fdata-sections", "port/tests/monster_script.cpp", sources["runscript.cpp"], sources["runscript_opcodes.cpp"], sources["mg_memory.cpp"], "-Wl,--gc-sections"]))
        cases.append(("effect_heap", [cpp_flags[0], "-I" + str(headers), *cpp_flags[1:], "-fno-pie", "-no-pie", "-ffunction-sections", "-fdata-sections", "port/tests/effect_heap.cpp", sources["effscript.cpp"], sources["runscript.cpp"], sources["mg_memory.cpp"], "-Wl,--gc-sections"]))
        collision_objects = []
        for name in ["mg_math.cpp", "collision_asm.cpp", "gameutil_asm.cpp"]:
            target = pathlib.Path(temporary) / ("collision_" + name + ".o")
            subprocess.run([cpp_flags[0], "-I" + str(headers), *cpp_flags[1:],
                            "-Iengine/port/include", "-Iengine/port/src", "-ffunction-sections", "-fdata-sections",
                            "-c", "engine/port/src/" + name, "-o", str(target)], cwd=root, check=True)
            subprocess.run(["objcopy", "--weaken", str(target)], cwd=root, check=True)
            collision_objects.append(str(target))
        cases.append(("collision", [cpp_flags[0], "-I" + str(headers), *cpp_flags[1:],
                                    "-ffunction-sections", "-fdata-sections", "port/tests/collision.cpp",
                                    "port/src/collision.cpp", sources["mg_math.cpp"], *collision_objects,
                                    "engine/tools/kazarov/src/cpp/sce/libvu0.cpp", "-Wl,--gc-sections"]))
        for name, command in cases:
            binary = pathlib.Path(temporary) / name
            subprocess.run([*command, "-o", str(binary)], cwd=root, check=True)
            subprocess.run([str(binary)], cwd=root, check=True, timeout=15)
            print(f"{name}: passed", flush=True)
        gs_sources = ["port/src/gs_memory.c", "port/src/gs_clut.c", "port/src/gs_raster.c"]
        gs_bundle = pathlib.Path(temporary) / "gs_bundle.c"
        gs_bundle.write_text("".join(f'#include "{root / source}"\n' for source in gs_sources))
        gs_outputs = []
        for level, sources in [("-O2", gs_sources), ("-O3", [str(gs_bundle)])]:
            target = pathlib.Path(temporary) / ("gs_grouped" + level)
            subprocess.run([*c_flags, level, "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off",
                            "port/tests/gs_scenes.c", *sources, "-lm", "-o", str(target)], cwd=root, check=True)
            gs_outputs.append(subprocess.check_output([str(target)], cwd=root, text=True, timeout=30))
        if gs_outputs[0] != gs_outputs[1]:
            raise RuntimeError("grouped GS output differs from separate O2 units")
        print("gs_grouped: 384 scene outputs match", flush=True)
        math_outputs = []
        for level in ["-O0", "-O2"]:
            target = pathlib.Path(temporary) / ("math_bits" + level)
            subprocess.run([*cpp_flags, level, "-ffunction-sections", "-fdata-sections",
                            "port/tests/math_bits.cpp", "port/src/frame.cpp", "port/src/collision.cpp",
                            "engine/tools/kazarov/src/cpp/sce/libvu0.cpp", "-Wl,--gc-sections", "-o", str(target)], cwd=root, check=True)
            result = subprocess.check_output([str(target)], cwd=root, text=True, timeout=15)
            math_outputs.append(result.split()[0])
        if math_outputs[0] != math_outputs[1]:
            raise RuntimeError("optimized matrix, clipping and collision results differ from the O0 build")
        print("math_bits: O0/O2 outputs match", flush=True)
        movie_test = pathlib.Path(temporary) / "movie_decode"
        movie_sdk = root / "build/port/movie-sdk"
        movie_flags = []
        movie_libraries = []
        if options.iso:
            if not (movie_sdk / "include/libavcodec/avcodec.h").exists():
                parser.error("--iso needs the local movie SDK: python3 -I tools/movie_sdk.py")
            movie_flags = ["-DPORT_MOVIES", f"-I{movie_sdk / 'include'}"]
            movie_libraries = [f"-L{movie_sdk / 'lib'}", f"-Wl,-rpath,{movie_sdk / 'lib'}", "-lavcodec", "-lavutil"]
        subprocess.run([*c_flags, "-Wno-unused-parameter", *movie_flags,
                        "port/tests/movie_decode.c", "port/src/movie_decode.c", "port/src/cdvd.c",
                        "-Wl,--wrap=clock_gettime", *movie_libraries, "-o", str(movie_test)], cwd=root, check=True)
        subprocess.run([str(movie_test)], cwd=root, check=True, timeout=15)
        if options.iso:
            for name in ["L5LOGO.PSS", "TUTO/VTUTO6.PSS"]:
                subprocess.run([str(movie_test), str(options.iso.resolve()), name], cwd=root, check=True, timeout=60)
        print("movie_decode: passed", flush=True)
    subprocess.run([sys.executable, "-I", "port/tests/cdvd.py"], cwd=root, check=True, timeout=30)


if __name__ == "__main__":
    main()
