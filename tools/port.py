#!/usr/bin/env python3
"""Builds the native port for Linux x86-64 or macOS arm64: the engine's units with tools/engine-patches/port.json applied,
ChronicleTwo's C++ for the MWCC asm (its symbols made weak, so where the engine still has a
function its own definition is used), and port/src, compiled by clang and linked into
build/port/darkchronicle. Every symbol nothing defines yet becomes a stub that names itself and
aborts, and build/port/missing.txt lists them by library.

    python3 -I tools/port.py            build
    python3 -I tools/port.py --run      build, then run it
    python3 -I tools/port.py --debug    also link build/port/darkchronicle-debug with the debug tools mod (port/debug) and Dear ImGui
    python3 -I tools/port.py --mod DIR  link a mod's port code (its mod.json's "port" sources) into the binaries
    python3 -I tools/port.py --ios simulator
                                        build build/port/ios-simulator/DarkChronicle.app
    python3 -I tools/port.py --ios device
                                        build build/port/ios-device/DarkChronicle.app, signed for an iPhone
"""

import argparse
import concurrent.futures
import datetime
import hashlib
import importlib.util
import json
import os
import plistlib
import re
import shlex
import shutil
import struct
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
DEBUG_BINARY = OUT / "darkchronicle-debug"
IMGUI = OUT / "imgui-sdk"
IMGUI_SOURCES = ["imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp", "imgui_demo.cpp",
                 "imgui_impl_sdl3.cpp", "imgui_impl_sdlgpu3.cpp"]
SDL_CFLAGS = []
TARGET = []
LINKER = "clang++"
DARWIN = sys.platform == "darwin"
PREFIX = "_" if DARWIN else ""
OBJCOPY = (shutil.which("llvm-objcopy") or "/opt/homebrew/opt/llvm/bin/llvm-objcopy") if DARWIN else "objcopy"
NM = OBJCOPY.replace("objcopy", "nm")

UPSTREAM_ASM = ["mg_math.cpp", "collision_asm.cpp", "gameutil_asm.cpp", "mg_drawprim_asm.cpp"]
FLAGS = [
    "-O0", "-g", "-pthread", *([] if DARWIN else ["-fno-pie", "-fno-pic"]), "-DPORT", "-DPAL",
    "-fms-extensions", "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off",
    "-fno-strict-return", "-ftrivial-auto-var-init=zero",
    "-Wno-c++11-narrowing", "-Wno-register", "-Wno-return-mismatch", "-w",
]
LINK_FLAGS = ["-pthread", *(["-lobjc"] if DARWIN else [])]
MAC_IMAGE_BASE = 0x3E9100000000
SEGMENTS = {"__TEXT": 0, "__DATA_CONST": 0xC00000, "__DATA": 0x1000000, "__PS2RAM": 0x4000000, "__PS2PORT": 0xC000000,
            "__PS2": 0x10000000, "__PS2SPR": 0x70000000}
IOS = {
    "simulator": {"sdk": "iphonesimulator", "target": "arm64-apple-ios17.0-simulator", "platform": "iPhoneSimulator"},
    "device": {"sdk": "iphoneos", "target": "arm64-apple-ios17.0", "platform": "iPhoneOS"},
}
PROFILES = [Path.home() / "Library/Developer/Xcode/UserData/Provisioning Profiles",
            Path.home() / "Library/MobileDevice/Provisioning Profiles"]
PROFILE_PROJECT = """// !$*UTF8*$!
{
    archiveVersion = 1;
    classes = {};
    objectVersion = 56;
    objects = {
        D0000000000000000000000A = {isa = PBXBuildFile; fileRef = D0000000000000000000000B; };
        D0000000000000000000000B = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.c; path = main.c; sourceTree = "<group>"; };
        D0000000000000000000000C = {isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = Profile.app; sourceTree = BUILT_PRODUCTS_DIR; };
        D0000000000000000000000D = {isa = PBXGroup; children = (D0000000000000000000000B, D0000000000000000000000E); sourceTree = "<group>"; };
        D0000000000000000000000E = {isa = PBXGroup; children = (D0000000000000000000000C); name = Products; sourceTree = "<group>"; };
        D0000000000000000000000F = {isa = PBXSourcesBuildPhase; buildActionMask = 2147483647; files = (D0000000000000000000000A); runOnlyForDeploymentPostprocessing = 0; };
        D00000000000000000000010 = {isa = PBXNativeTarget; buildConfigurationList = D00000000000000000000011; buildPhases = (D0000000000000000000000F); buildRules = (); dependencies = (); name = Profile; productName = Profile; productReference = D0000000000000000000000C; productType = "com.apple.product-type.application"; };
        D00000000000000000000011 = {isa = XCConfigurationList; buildConfigurations = (D00000000000000000000012); defaultConfigurationIsVisible = 0; defaultConfigurationName = Debug; };
        D00000000000000000000012 = {isa = XCBuildConfiguration; buildSettings = {
            CODE_SIGN_STYLE = Automatic;
            DEVELOPMENT_TEAM = @TEAM@;
            GENERATE_INFOPLIST_FILE = YES;
            IPHONEOS_DEPLOYMENT_TARGET = 17.0;
            PRODUCT_BUNDLE_IDENTIFIER = @IDENTIFIER@;
            PRODUCT_NAME = Profile;
            SDKROOT = iphoneos;
            TARGETED_DEVICE_FAMILY = "1,2";
        }; name = Debug; };
        D00000000000000000000013 = {isa = PBXProject; buildConfigurationList = D00000000000000000000014; compatibilityVersion = "Xcode 14.0"; developmentRegion = en; hasScannedForEncodings = 0; knownRegions = (en, Base); mainGroup = D0000000000000000000000D; productRefGroup = D0000000000000000000000E; projectDirPath = ""; projectRoot = ""; targets = (D00000000000000000000010); };
        D00000000000000000000014 = {isa = XCConfigurationList; buildConfigurations = (D00000000000000000000015); defaultConfigurationIsVisible = 0; defaultConfigurationName = Debug; };
        D00000000000000000000015 = {isa = XCBuildConfiguration; buildSettings = {SDKROOT = iphoneos; }; name = Debug; };
    };
    rootObject = D00000000000000000000013;
}
"""
IOS_FRAMEWORKS = ["AVFoundation", "AudioToolbox", "CoreBluetooth", "CoreGraphics", "CoreHaptics", "CoreMedia",
                  "CoreMotion", "CoreVideo", "Foundation", "GameController", "Metal", "OpenGLES", "QuartzCore", "UIKit",
                  "UniformTypeIdentifiers"]
IOS_ICONS = {"AppIcon60x60@2x.png": 120, "AppIcon60x60@3x.png": 180, "AppIcon76x76@2x~ipad.png": 152,
             "AppIcon83.5x83.5@2x~ipad.png": 167}
LOCALISED = {"mainloop": ["MainBuffer"], "eventedit": ["_Z10VectMatMulPfS_PA4_f"]}
ALIASES = {
    "BuffWorkData__2": "BuffWorkData",
    "EventCamera__2": "EventCamera",
    "_Z8PrimQuadIfEvP11mgCDrawPrim6mgRectIT_ES2_IiE": "_Z11PrimQuad_f_P11mgCDrawPrim9mgRect_f_9mgRect_i_",
    "_Z8PrimQuadIiEvP11mgCDrawPrim6mgRectIT_ES2_IiE": "_Z11PrimQuad_i_P11mgCDrawPrim9mgRect_i_S1_",
}
ENGINE_PORT_FILES = ["memory", "split_data"]
RESTORED_PORT_FILES = ["kernel", "graphics", "cdvd", "spu2", "iop", "devices", "mg_shadow", "mg_drawprim", "frame", "draw",
                       "mg_visual", "mg_sprite", "water"]
OPERATORS = {
    **dict.fromkeys(["_Znwm", "_Znam", "_ZnwmRKSt9nothrow_t", "_ZnamRKSt9nothrow_t"], "port_new"),
    **dict.fromkeys(["_ZnwmSt11align_val_t", "_ZnamSt11align_val_t", "_ZnwmSt11align_val_tRKSt9nothrow_t",
                     "_ZnamSt11align_val_tRKSt9nothrow_t"], "port_new_aligned"),
    **dict.fromkeys([f"_Zd{kind}Pv{size}{rest}" for kind in "la" for size in ("", "m")
                     for rest in ("", "RKSt9nothrow_t", "St11align_val_t", "St11align_val_tRKSt9nothrow_t")], "port_delete"),
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
    c = source.suffix in (".c", ".m")
    if name.startswith("imgui_"):
        return compile_command(source, target, depfile, command_file,
                               ["clang++", *TARGET, "-std=c++17", "-c", "-O2", "-g", "-w", "-MMD", "-MF", str(depfile),
                                f"-I{IMGUI}", *SDL_CFLAGS, str(source), "-o", str(target)], [])
    debug = [f"-I{IMGUI}", f"-I{source.parent}"] if name.startswith("mod_") else []
    compiler = LINKER.replace("clang++", "clang") if source.suffix == ".m" else "clang" if c else "clang++"
    objc = ["-fobjc-arc"] if source.suffix == ".m" else []
    command = [compiler, *TARGET, *objc, "-std=c11" if c else "-std=c++26", "-c", *FLAGS, *debug,
               *(["-O2"] if not source.is_relative_to(COPY) and source.name not in ("memory.c", "kernel.c") else []),
               "-MMD", "-MF", str(depfile),
               *([] if c else ["-include", str(PORT / "include" / "port.h")]),
               f"-I{PORT / 'include'}", f"-I{COPY / 'ps2' / 'include'}", f"-I{COPY / 'ps2' / 'include' / 'sce'}",
               *([f"-I{ENGINE / 'port' / 'include'}"] if name.startswith("upstream_") else []),
               f"-I{ENGINE / 'port' / 'src'}", str(source), "-o", str(target)]
    changes = [f"--localize-symbol={PREFIX}{symbol}" for symbol in LOCALISED.get(name, [])]
    if name.startswith("upstream_"):
        changes.append("--weaken")
    if name == "main":
        changes.append(f"--redefine-sym={PREFIX}main={PREFIX}port_game_main")
    return compile_command(source, target, depfile, command_file, command, changes,
                           operators=not c and not name.startswith(("mod_", "port_")),
                           segment=data_segment(name) if DARWIN else None)


def data_segment(name):
    if name.startswith("port_") and name.removeprefix("port_") in ENGINE_PORT_FILES:
        return "__PS2RAM"
    if name.startswith("port_"):
        return "__PS2PORT" if name.removeprefix("port_") in RESTORED_PORT_FILES else None
    return None if name.startswith(("mod_", "imgui_")) else "__PS2RAM"


def move_data(target, segment):
    data = bytearray(target.read_bytes())
    commands = struct.unpack_from("<I", data, 16)[0]
    at = 32
    for _ in range(commands):
        kind, size = struct.unpack_from("<II", data, at)
        if kind == 0x19:
            sections = struct.unpack_from("<I", data, at + 64)[0]
            for index in range(sections):
                header = at + 72 + index * 80
                if data[header + 16:header + 32].rstrip(b"\0") == b"__DATA" and \
                        data[header:header + 16].rstrip(b"\0") in (b"__data", b"__bss", b"__common"):
                    data[header + 16:header + 32] = segment.encode().ljust(16, b"\0")
        at += size
    target.write_bytes(data)


def compile_command(source, target, depfile, command_file, command, changes, operators=False, segment=None):
    name = target.stem
    signature = json.dumps([command, changes, operators, segment])
    if up_to_date(source, target, depfile) and command_file.exists() and command_file.read_text() == signature:
        return name, None
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        return name, result.stderr
    if changes:
        subprocess.run([OBJCOPY, *changes, str(target)], check=True)
    if operators:
        used = subprocess.run([NM, "-u", str(target)], capture_output=True, text=True, check=True).stdout.split()
        for old, new in OPERATORS.items():
            if f"{PREFIX}{old}" in used:
                subprocess.run([OBJCOPY, f"--redefine-sym={PREFIX}{old}={PREFIX}{new}", str(target)], check=True)
    if segment:
        move_data(target, segment)
    command_file.write_text(signature, encoding="utf-8")
    return name, None


def port_mod_jobs(mod_dir):
    manifest = json.loads((mod_dir / "mod.json").read_text(encoding="utf-8"))
    port = manifest.get("port", {})
    prefix = "mod_" + "".join(c if c.isalnum() else "_" for c in manifest["id"]) + "_"
    sources = sorted({source for pattern in port.get("sources", []) for source in mod_dir.glob(pattern)})
    return [(source, prefix + source.stem) for source in sources]


def compile_all(units, debug=False, mods=()):
    jobs = [(source, source.stem) for source in units]
    jobs += [(ENGINE / "port" / "src" / name, "upstream_" + Path(name).stem) for name in UPSTREAM_ASM]
    jobs.append((ENGINE / "tools" / "kazarov" / "src" / "cpp" / "sce" / "libvu0.cpp", "kazarov_vu0"))
    jobs += [(source, "port_" + source.stem) for source in sorted((PORT / "src").glob("*.c*"))]
    if TARGET:
        jobs += [(source, "port_" + source.stem) for source in sorted((PORT / "src").glob("*.m"))]
    if debug:
        jobs += port_mod_jobs(PORT / "debug")
        jobs += [(IMGUI / name, "imgui_" + Path(name).stem) for name in IMGUI_SOURCES]
    for mod_dir in mods:
        jobs += port_mod_jobs(mod_dir)
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


def link(objects, extra, binary):
    if DARWIN:
        platform = [f"-Wl,-alias,_{target},_{alias}" for alias, target in ALIASES.items()]
        base = 0x100000000 if TARGET else MAC_IMAGE_BASE
        for segment, offset in SEGMENTS.items():
            platform += [f"-Wl,-segaddr,{segment},{base + offset:#x}"]
            platform += [f"-Wl,-segprot,{segment},rw,rw"] if segment.startswith("__PS2") else []
    else:
        platform = ["-no-pie", "-Wl,--no-demangle", *(f"-Wl,--defsym={alias}={target}" for alias, target in ALIASES.items())]
    fresh = binary.with_name(binary.name + ".linking")
    command = [LINKER, *TARGET, "-o", str(fresh), *map(str, objects), *map(str, extra), *platform, *LINK_FLAGS]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode == 0:
        os.replace(fresh, binary)
    return result


def undefined(link_errors, objects):
    if not DARWIN:
        return sorted(set(re.findall(r"undefined reference to `([^']+)'", link_errors)))
    printed = set(re.findall(r'^\s+"(.+)", referenced from', link_errors, re.M))
    listing = subprocess.run([OBJCOPY.replace("objcopy", "nm"), "-u", "-j", *map(str, objects)],
                             capture_output=True, text=True, check=True).stdout.split()
    symbols = sorted({name[1:] for name in listing if name.startswith("_")})
    return [name for name, text in demangled(symbols).items() if f"_{name}" in printed or text in printed]


def demangled(names):
    if not names:
        return {}
    result = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True, check=True)
    return dict(zip(names, result.stdout.splitlines()))


def write_stubs(names, readable):
    lines = ["#include <stdio.h>", "#include <stdlib.h>"]
    for number, name in enumerate(names):
        if name in SPLIT_DATA:
            lines.append(f'__attribute__((used, aligned(16))) char stub_data_{number}[64] __asm__("{PREFIX}{name}");')
            continue
        text = readable[name].replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'__attribute__((used)) void stub_{number}(void) __asm__("{PREFIX}{name}");')
        lines.append(f'void stub_{number}(void) {{ fputs("unimplemented: {text}\\n", stderr); abort(); }}')
    source = OUT / "stubs.c"
    source.write_text("\n".join(lines) + "\n", encoding="utf-8")
    target = OUT / "stubs.o"
    subprocess.run(["clang", *TARGET, "-c", "-O0", *([] if DARWIN else ["-fno-pie"]), "-w", str(source), "-o", str(target)], check=True)
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


def movie_flags(prefix):
    if (prefix / "include/libavcodec/avcodec.h").exists():
        return [f"-I{prefix / 'include'}"], [f"-L{prefix / 'lib'}", f"-Wl,-rpath,{prefix / 'lib'}", "-lavcodec", "-lavutil"]
    try:
        cflags = subprocess.check_output(["pkg-config", "--cflags", "libavcodec", "libavutil"], text=True)
        libraries = subprocess.check_output(["pkg-config", "--libs", "libavcodec", "libavutil"], text=True)
        return shlex.split(cflags), shlex.split(libraries)
    except (OSError, subprocess.CalledProcessError):
        sys.exit("FFmpeg SDK missing: run python3 -I tools/movie_sdk.py")


def ios_flags(kind):
    settings = IOS[kind]
    prefix = ROOT / "build" / "port" / "ios-sdk" / kind
    if not (prefix / "lib" / "libSDL3.a").exists():
        sys.exit("iOS SDL3 and FFmpeg missing: run python3 -I tools/ios_sdk.py")
    sysroot = subprocess.check_output(["xcrun", "--sdk", settings["sdk"], "--show-sdk-path"], text=True).strip()
    TARGET.extend(["-target", settings["target"], "-isysroot", sysroot])
    frameworks = [flag for name in IOS_FRAMEWORKS for flag in ("-framework", name)]
    return [f"-I{prefix / 'include'}"], [f"-L{prefix / 'lib'}", "-lSDL3", "-lavcodec", "-lavutil", *frameworks]


def app_info(kind, identifier):
    settings = IOS[kind]
    return {
        "CADisableMinimumFrameDurationOnPhone": True,
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": "Dark Chronicle",
        "CFBundleExecutable": "DarkChronicle",
        "CFBundleIdentifier": identifier,
        "CFBundleIcons": {"CFBundlePrimaryIcon": {"CFBundleIconFiles": ["AppIcon60x60"]}},
        "CFBundleIcons~ipad": {"CFBundlePrimaryIcon": {"CFBundleIconFiles": ["AppIcon60x60", "AppIcon76x76", "AppIcon83.5x83.5"]}},
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "DarkChronicle",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "1.0",
        "CFBundleVersion": "1",
        "CFBundleSupportedPlatforms": [settings["platform"]],
        "DTPlatformName": settings["sdk"],
        "LSRequiresIPhoneOS": True,
        "LSSupportsOpeningDocumentsInPlace": True,
        "MinimumOSVersion": "17.0",
        "UIDeviceFamily": [1, 2],
        "UIFileSharingEnabled": True,
        "UILaunchScreen": {},
        "UIRequiresFullScreen": True,
        "UIStatusBarHidden": True,
        "UISupportedInterfaceOrientations": ["UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight"],
    }


def bundle(kind, executable, identifier):
    app = OUT / "DarkChronicle.app"
    shutil.rmtree(app, ignore_errors=True)
    app.mkdir(parents=True)
    shutil.copy2(executable, app / "DarkChronicle")
    (app / "Info.plist").write_bytes(plistlib.dumps(app_info(kind, identifier), fmt=plistlib.FMT_BINARY))
    for name, pixels in IOS_ICONS.items():
        subprocess.run(["sips", "-z", str(pixels), str(pixels), str(PORT / "launcher" / "icon-ios.png"), "--out", str(app / name)],
                       check=True, capture_output=True)
    if kind == "simulator":
        subprocess.run(["codesign", "--force", "--sign", "-", "--timestamp=none", str(app)], check=True)
    return app


def profile_contents(path):
    data = subprocess.run(["security", "cms", "-D", "-i", str(path)], capture_output=True, check=True).stdout
    return plistlib.loads(data)


def find_profile(team, identifier):
    now = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
    paths = [path for folder in PROFILES for path in folder.glob("*.mobileprovision")]
    for path in sorted(paths, key=lambda path: path.stat().st_mtime, reverse=True):
        contents = profile_contents(path)
        if contents["Entitlements"].get("application-identifier") == f"{team}.{identifier}" and contents["ExpirationDate"] > now:
            return path, contents
    return None, None


def request_profile(team, identifier):
    folder = OUT / "profile"
    project = folder / "Profile.xcodeproj"
    project.mkdir(parents=True, exist_ok=True)
    (folder / "main.c").write_text("int main(void) { return 0; }\n", encoding="utf-8")
    text = PROFILE_PROJECT.replace("@TEAM@", team).replace("@IDENTIFIER@", identifier)
    (project / "project.pbxproj").write_text(text, encoding="utf-8")
    subprocess.run(["xcodebuild", "-project", str(project), "-target", "Profile", "-sdk", "iphoneos",
                    "-allowProvisioningUpdates", "-allowProvisioningDeviceRegistration", f"SYMROOT={folder / 'build'}",
                    "build"], check=True, stdout=subprocess.DEVNULL)


def signing_identity(contents):
    wanted = {hashlib.sha1(certificate).hexdigest().upper() for certificate in contents["DeveloperCertificates"]}
    listing = subprocess.check_output(["security", "find-identity", "-v", "-p", "codesigning"], text=True)
    for words in (line.split() for line in listing.splitlines()):
        if len(words) > 1 and words[1] in wanted:
            return words[1]
    sys.exit("no signing identity in the keychain matches the provisioning profile's certificates")


def sign_for_device(app, team, identifier):
    path, contents = find_profile(team, identifier)
    if not path:
        request_profile(team, identifier)
        path, contents = find_profile(team, identifier)
    if not path:
        sys.exit(f"Xcode made no provisioning profile for {team}.{identifier}; sign in to Xcode with that team's Apple ID")
    shutil.copyfile(path, app / "embedded.mobileprovision")
    entitlements = OUT / "entitlements.plist"
    entitlements.write_bytes(plistlib.dumps(contents["Entitlements"]))
    subprocess.run(["codesign", "--force", "--sign", signing_identity(contents), "--entitlements", str(entitlements),
                    "--generate-entitlement-der", "--timestamp=none", str(app)], check=True)
    return contents["ExpirationDate"]


def main():
    global OUT, OBJ, BINARY, DEBUG_BINARY, LINKER
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--run", action="store_true", help="run the port once it is built")
    parser.add_argument("--graphics", action="store_true", help="accepted for compatibility; the port always opens a window")
    parser.add_argument("--movies", action="store_true", help="enable MPEG-2 movies with the local FFmpeg SDK")
    parser.add_argument("--debug", action="store_true", help="also link the debug build with its window and API")
    parser.add_argument("--mod", type=Path, action="append", default=[], help="a mod folder whose port code goes into the binaries")
    parser.add_argument("--sdl", type=Path, default=OUT / "sdk", help="local SDL3 installation prefix")
    parser.add_argument("--ios", choices=sorted(IOS), help="build DarkChronicle.app for the iOS simulator or an iPhone")
    parser.add_argument("--bundle-id", default="dc2toolkit.darkchronicle", help="the iOS app's bundle identifier")
    parser.add_argument("--team", help="the Apple development team that signs the iPhone app, as Xcode lists it")
    parser.add_argument("arguments", nargs="*", help="arguments for the port, with --run")
    options = parser.parse_args()
    if options.ios == "device" and not options.team:
        parser.error("--ios device needs --team: the team ID from Xcode's Signing & Capabilities")
    if options.ios:
        OUT = ROOT / "build" / "port" / f"ios-{options.ios}"
        OBJ = OUT / "obj"
        BINARY = OUT / "darkchronicle"
        DEBUG_BINARY = OUT / "darkchronicle-debug"
        LINKER = subprocess.check_output(["xcrun", "--find", "clang++"], text=True).strip()
        cflags, libraries = ios_flags(options.ios)
        FLAGS.append("-DPORT_MOVIES")
    else:
        if options.movies:
            movie_cflags, movie_libraries = movie_flags(OUT / "movie-sdk")
            FLAGS.extend(["-DPORT_MOVIES", *movie_cflags])
            LINK_FLAGS.extend(movie_libraries)
        cflags, libraries = graphics_flags(options.sdl.resolve())
    SDL_CFLAGS.extend(cflags)
    FLAGS.extend(["-DPORT_GRAPHICS", *cflags])
    LINK_FLAGS.extend(libraries)
    if not (ENGINE / "ps2" / "src").is_dir():
        sys.exit("no engine source in engine/: run `dc2tk engine fetch`")
    if options.debug and not (IMGUI / "imgui.h").exists():
        sys.exit("Dear ImGui missing: run python3 -I tools/imgui_sdk.py")
    every, units, others = compile_all(copy_engine(), options.debug, [mod.resolve() for mod in options.mod])
    debug_objects = [path for path in every if path.stem.startswith(("mod_dc2tk_debug_tools_", "imgui_"))]
    objects = [path for path in every if path not in debug_objects]
    first = link(objects, [], BINARY)
    names = undefined(first.stderr, objects)
    if first.returncode != 0 and not names:
        sys.exit(first.stderr)
    readable = demangled(names)
    groups = write_missing(names, readable)
    extra = [write_stubs(names, readable)] if names else []
    final = link(objects, extra, BINARY)
    if final.returncode != 0:
        sys.exit(final.stderr)
    write_if_changed(OUT / "objects.json", json.dumps([str(path) for path in [*objects, *extra]], indent=2) + "\n")
    if options.debug:
        debugged = link([*objects, *debug_objects], extra, DEBUG_BINARY)
        if debugged.returncode != 0:
            sys.exit(debugged.stderr)
    summary = f"{units} engine units and {others} other files compiled; {len(names)} symbols stubbed"
    if groups:
        summary += f" ({', '.join(f'{group} {len(items)}' for group, items in sorted(groups.items(), key=lambda g: -len(g[1])))})"
    print(summary)
    print(f"linked {BINARY.relative_to(ROOT)}; what is missing is in {(OUT / 'missing.txt').relative_to(ROOT)}")
    if options.debug:
        print(f"linked {DEBUG_BINARY.relative_to(ROOT)}")
    if options.ios:
        app = bundle(options.ios, DEBUG_BINARY if options.debug else BINARY, options.bundle_id)
        print(f"bundled {app.relative_to(ROOT)}")
        if options.ios == "device":
            print(f"signed for team {options.team}; the profile expires {sign_for_device(app, options.team, options.bundle_id)}")
        return
    if options.run:
        sys.stdout.flush()
        code = subprocess.run([str(BINARY), *(["--graphics"] if options.graphics else []), *options.arguments]).returncode
        sys.exit(128 - code if code < 0 else code)


if __name__ == "__main__":
    main()
