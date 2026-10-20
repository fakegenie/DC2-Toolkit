#!/usr/bin/env python3
"""Builds the native Linux port: the engine's units with tools/engine-patches/port.json applied,
ChronicleTwo's C++ for the MWCC asm (its symbols made weak, so where the engine still has a
function its own definition is used), and port/src, compiled by clang and linked into
build/port/darkchronicle. Every symbol nothing defines yet becomes a stub that names itself and
aborts, and build/port/missing.txt lists them by library.

    python3 -I tools/port.py            build
    python3 -I tools/port.py --run      build, then run it
"""

import argparse
import concurrent.futures
import importlib.util
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / "engine"
PORT = ROOT / "port"
OUT = ROOT / "build" / "port"
COPY = OUT / "copy"
OBJ = OUT / "obj"
BINARY = OUT / "darkchronicle"

UPSTREAM_ASM = ["mg_math.cpp", "collision_asm.cpp", "gameutil_asm.cpp", "mg_drawprim_asm.cpp"]
FLAGS = [
    "-O0", "-g", "-pthread", "-fno-pie", "-fno-pic", "-DPORT", "-DPAL", "-DPORT_SOUND",
    "-fms-extensions", "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off",
    "-fno-strict-return", "-ftrivial-auto-var-init=zero",
    "-Wno-c++11-narrowing", "-Wno-register", "-Wno-return-mismatch", "-w",
]
LINK_FLAGS = ["-pthread"]
LOCALISED = {"mainloop": ["MainBuffer"], "eventedit": ["_Z10VectMatMulPfS_PA4_f"]}
ALIASES = {
    "BuffWorkData__2": "BuffWorkData",
    "EventCamera__2": "EventCamera",
    "_Z8PrimQuadIfEvP11mgCDrawPrim6mgRectIT_ES2_IiE": "_Z11PrimQuad_f_P11mgCDrawPrim9mgRect_f_9mgRect_i_",
    "_Z8PrimQuadIiEvP11mgCDrawPrim6mgRectIT_ES2_IiE": "_Z11PrimQuad_i_P11mgCDrawPrim9mgRect_i_S1_",
}
SPLIT_DATA = {"Vu_progmain", "Vu_prog0", "Vu_prog_sdw", "Vu_prog_3dsp", "Vu_prog_wtr", "My_dma_start0", "_gp"}
LIBRARIES = [
    ("libgraph", r"^sceGs"), ("libdma", r"^sceDma"), ("libvifpk", r"^sceVif1Pk"), ("libgifpk", r"^sceGifPk"),
    ("libvu0", r"^sceVu0"), ("libpad", r"^scePad"), ("libmc", r"^sceMc"), ("libcdvd", r"^sceCd"),
    ("libmpeg", r"^sceMpeg"), ("libsdr", r"^sceSd"), ("sifrpc, sifcmd, IOP heap", r"^sceSif"),
    ("sifdev", r"^sce(Open|Close|Read|Write|Lseek|Ioctl|Getstat|FsReset)\b"), ("libdev", r"^sceDev"),
    ("modmsin", r"^sceMSIn"),
    ("eekernel", r"^(AddDmacHandler|AddIntcHandler|ChangeThreadPriority|CreateSema|CreateThread|DeleteSema"
                 r"|DeleteThread|DIntr|DisableDmac|EIntr|EnableDmac|EnableIntc|Exit__2|FlushCache|GetThreadId"
                 r"|RemoveDmacHandler|RemoveIntcHandler|RotateThreadReadyQueue|SignalSema|StartThread"
                 r"|TerminateThread|WaitSema)\b"),
    ("MW runtime", r"^(mwInit|fptosi|fptoui|__construct_new_array)\b"),
    ("split data from the disc's executable", r"^(Vu_prog\w*|My_dma_start0|_gp)$"),
]


def patched(relative, text, patches):
    for patch in patches:
        if patch["file"] != relative:
            continue
        found = text.count(patch["find"])
        expected = patch.get("count", 1)
        if expected < 1 or found != expected:
            sys.exit(f"tools/engine-patches/port.json: its text for {relative} is there {found} times, expected {expected}:\n"
                     f"{patch['find']}")
        text = text.replace(patch["find"], patch["replace"], expected)
    return text


def write_if_changed(path, text):
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def copy_engine():
    override_path = ENGINE / "tools" / "kazarov" / "scripts" / "build" / "port_override.py"
    spec = importlib.util.spec_from_file_location("port_override", override_path)
    overrides = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(overrides)
    replacements = {}
    for source in sorted((PORT / "src").glob("*.cpp")):
        names = overrides.tagged_names(source.read_text(encoding="utf-8"))
        if names:
            replacements[f"ps2/src/{source.name}"] = names
    patches = json.loads((ROOT / "tools" / "engine-patches" / "port.json").read_text(encoding="utf-8"))
    sources = [path for path in sorted((ENGINE / "ps2").rglob("*"))
               if path.is_file() and path.suffix in (".cpp", ".hpp", ".h", ".inc")
               and "std" not in path.relative_to(ENGINE / "ps2").parts]
    relatives = {path.relative_to(ENGINE).as_posix() for path in sources}
    for patch in patches:
        if patch["file"] not in relatives:
            sys.exit(f"tools/engine-patches/port.json changes {patch['file']}, which the engine does not have")
    for path in sources:
        relative = path.relative_to(ENGINE).as_posix()
        text = patched(relative, path.read_text(encoding="utf-8"), patches)
        if relative in replacements:
            text = overrides.strip(text, replacements[relative], defined=("PORT", "PAL"), long_is_64=True)
        write_if_changed(COPY / relative, text)
    return sorted((COPY / "ps2" / "src").glob("*.cpp"))


def up_to_date(source, target, depfile):
    if not target.exists() or not depfile.exists():
        return False
    built = target.stat().st_mtime
    words = depfile.read_text(encoding="utf-8").replace("\\\n", " ").split()[1:]
    return all(Path(word).exists() and Path(word).stat().st_mtime <= built for word in words)


def compile_one(source, name):
    target = OBJ / f"{name}.o"
    depfile = OBJ / f"{name}.d"
    command_file = OBJ / f"{name}.command.json"
    c = source.suffix == ".c"
    command = ["clang" if c else "clang++", "-std=c11" if c else "-std=c++26", "-c", *FLAGS,
               *(["-O2"] if not source.is_relative_to(COPY) and source.name not in ("memory.c", "kernel.c") else []),
               *(["-O3"] if name == "port_gs_bundle" else []),
               "-MMD", "-MF", str(depfile),
               *([] if c else ["-include", str(PORT / "include" / "port.h")]),
               f"-I{PORT / 'include'}", f"-I{COPY / 'ps2' / 'include'}", f"-I{COPY / 'ps2' / 'include' / 'sce'}",
               *([f"-I{ENGINE / 'port' / 'include'}"] if name.startswith("upstream_") else []),
               f"-I{ENGINE / 'port' / 'src'}", str(source), "-o", str(target)]
    signature = json.dumps([command, LOCALISED.get(name, []), name == "main"])
    if up_to_date(source, target, depfile) and command_file.exists() and command_file.read_text() == signature:
        return name, None
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        return name, result.stderr
    for symbol in LOCALISED.get(name, []):
        subprocess.run(["objcopy", f"--localize-symbol={symbol}", str(target)], check=True)
    if name.startswith("upstream_"):
        subprocess.run(["objcopy", "--weaken", str(target)], check=True)
    if name == "main":
        subprocess.run(["objcopy", "--redefine-sym=main=port_game_main", str(target)], check=True)
    command_file.write_text(signature, encoding="utf-8")
    return name, None


def compile_all(units):
    jobs = [(source, source.stem) for source in units]
    jobs += [(ENGINE / "port" / "src" / name, "upstream_" + Path(name).stem) for name in UPSTREAM_ASM]
    jobs.append((ENGINE / "tools" / "kazarov" / "src" / "cpp" / "sce" / "libvu0.cpp", "kazarov_vu0"))
    grouped = ("gs_memory.c", "gs_clut.c", "gs_raster.c")
    bundle = OUT / "gs_bundle.c"
    write_if_changed(bundle, "".join(f'#include "{PORT / "src" / name}"\n' for name in grouped))
    jobs += [(source, "port_" + source.stem) for source in sorted((PORT / "src").glob("*.c*"))
             if source.name not in grouped]
    jobs.append((bundle, "port_gs_bundle"))
    OBJ.mkdir(parents=True, exist_ok=True)
    workers = len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else os.cpu_count() or 1
    with concurrent.futures.ThreadPoolExecutor(min(workers, len(jobs))) as pool:
        results = list(pool.map(lambda job: compile_one(*job), jobs))
    failed = [(name, errors) for name, errors in results if errors]
    for name, errors in failed:
        print(f"{name} does not compile:\n{errors}", file=sys.stderr)
    if failed:
        sys.exit(f"{len(failed)} of {len(jobs)} files do not compile")
    return [OBJ / f"{name}.o" for _source, name in jobs], len(units), len(jobs) - len(units)


def link(objects, extra):
    command = ["clang++", "-no-pie", "-o", str(BINARY), *map(str, objects), *map(str, extra),
               "-Wl,--no-demangle", *(f"-Wl,--defsym={alias}={target}" for alias, target in ALIASES.items()), *LINK_FLAGS]
    return subprocess.run(command, capture_output=True, text=True)


def undefined(link_errors):
    return sorted(set(re.findall(r"undefined reference to `([^']+)'", link_errors)))


def demangled(names):
    if not names:
        return {}
    result = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True, check=True)
    return dict(zip(names, result.stdout.splitlines()))


def write_stubs(names, readable):
    lines = ["#include <stdio.h>", "#include <stdlib.h>"]
    for number, name in enumerate(names):
        if name in SPLIT_DATA:
            lines.append(f'__attribute__((used, aligned(16))) char stub_data_{number}[64] __asm__("{name}");')
            continue
        text = readable[name].replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'__attribute__((used)) void stub_{number}(void) __asm__("{name}");')
        lines.append(f'void stub_{number}(void) {{ fputs("unimplemented: {text}\\n", stderr); abort(); }}')
    source = OUT / "stubs.c"
    source.write_text("\n".join(lines) + "\n", encoding="utf-8")
    target = OUT / "stubs.o"
    subprocess.run(["clang", "-c", "-O0", "-fno-pie", "-w", str(source), "-o", str(target)], check=True)
    return target


def write_missing(names, readable):
    groups = {}
    for name in names:
        text = readable[name]
        group = next((library for library, pattern in LIBRARIES if re.match(pattern, text)), "the engine and other")
        groups.setdefault(group, []).append(text)
    lines = []
    for group in sorted(groups, key=lambda group: (-len(groups[group]), group)):
        lines.append(f"## {group}: {len(groups[group])}")
        lines.extend(f"  {text}" for text in sorted(groups[group]))
    (OUT / "missing.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return groups


def graphics_flags(prefix):
    include = prefix / "usr/include"
    if not (include / "SDL3/SDL.h").exists():
        include = prefix / "include"
    if (include / "SDL3/SDL.h").exists():
        libraries = []
        for relative in ("usr/lib/x86_64-linux-gnu", "lib/x86_64-linux-gnu",
                         "usr/lib/x86_64-linux-gnu/pulseaudio", "lib"):
            directory = prefix / relative
            if directory.is_dir():
                libraries.extend([f"-L{directory}", f"-Wl,-rpath,{directory}"])
        for library in sorted(prefix.glob("usr/lib/x86_64-linux-gnu/pulseaudio/*.so")):
            libraries.extend(["-Wl,--no-as-needed", str(library), "-Wl,--as-needed"])
        return [f"-I{include}"], [*libraries, "-Wl,--disable-new-dtags", "-lSDL3"]
    try:
        cflags = subprocess.check_output(["pkg-config", "--cflags", "sdl3"], text=True)
        libraries = subprocess.check_output(["pkg-config", "--libs", "sdl3"], text=True)
        return shlex.split(cflags), shlex.split(libraries)
    except (OSError, subprocess.CalledProcessError):
        sys.exit("SDL3 development files missing: run python3 -I tools/port_sdk.py or pass --sdl PREFIX")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--run", action="store_true", help="run the port once it is built")
    parser.add_argument("--graphics", action="store_true", help="enable the software GS and SDL3 display")
    parser.add_argument("--movies", action="store_true", help="enable MPEG-2 movies with the local FFmpeg SDK")
    parser.add_argument("--sdl", type=Path, default=OUT / "sdk", help="local SDL3 installation prefix")
    parser.add_argument("arguments", nargs="*", help="arguments for the port, with --run")
    options = parser.parse_args()
    if options.movies:
        prefix = OUT / "movie-sdk"
        if not (prefix / "include/libavcodec/avcodec.h").exists():
            sys.exit("FFmpeg SDK missing: run python3 -I tools/movie_sdk.py")
        FLAGS.extend(["-DPORT_MOVIES", f"-I{prefix / 'include'}"])
        LINK_FLAGS.extend([f"-L{prefix / 'lib'}", f"-Wl,-rpath,{prefix / 'lib'}", "-lavcodec", "-lavutil"])
    if options.graphics:
        cflags, libraries = graphics_flags(options.sdl.resolve())
        FLAGS.extend(["-DPORT_GRAPHICS", *cflags])
        LINK_FLAGS.extend(libraries)
    if not (ENGINE / "ps2" / "src").is_dir():
        sys.exit("no engine source in engine/: run `dc2tk engine fetch`")
    objects, units, others = compile_all(copy_engine())
    first = link(objects, [])
    names = undefined(first.stderr)
    if first.returncode != 0 and not names:
        sys.exit(first.stderr)
    readable = demangled(names)
    groups = write_missing(names, readable)
    extra = [write_stubs(names, readable)] if names else []
    final = link(objects, extra)
    if final.returncode != 0:
        sys.exit(final.stderr)
    write_if_changed(OUT / "objects.json", json.dumps([str(path) for path in [*objects, *extra]], indent=2) + "\n")
    summary = f"{units} engine units and {others} other files compiled; {len(names)} symbols stubbed"
    if groups:
        summary += f" ({', '.join(f'{group} {len(items)}' for group, items in sorted(groups.items(), key=lambda g: -len(g[1])))})"
    print(summary)
    print(f"linked {BINARY.relative_to(ROOT)}; what is missing is in {(OUT / 'missing.txt').relative_to(ROOT)}")
    if options.run:
        sys.stdout.flush()
        code = subprocess.run([str(BINARY), *(["--graphics"] if options.graphics else []), *options.arguments]).returncode
        sys.exit(128 - code if code < 0 else code)


if __name__ == "__main__":
    main()
