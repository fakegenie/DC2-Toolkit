#!/usr/bin/env python3
"""Builds the native port for Linux x86-64, macOS arm64 or Windows x64: the engine's units with tools/engine-patches/port.json applied,
ChronicleTwo's C++ for the MWCC asm (its symbols made weak, so where the engine still has a
function its own definition is used), and port/src, compiled by clang and linked into
build/port/dc2, with the shared SDL3 and FFmpeg of tools/port_sdk.py copied into build/port/lib. On Windows
llvm-mingw from tools/port_sdk.py links build/port/windows/dc2.exe, with the DLLs beside it. Every symbol
nothing defines yet becomes a stub that names itself and aborts, and build/port/missing.txt lists them by library.

    python3 -I tools/port.py            build
    python3 -I tools/port.py --run      build, then run it
    python3 -I tools/port.py --debug    also link build/port/dc2-debug with the debug tools mod (port/debug) and Dear ImGui
    python3 -I tools/port.py --mod DIR  link a mod's port code (its mod.json's "port" sources) into the binaries
    python3 -I tools/port.py --ios simulator
                                        build build/port/ios-simulator/DC2.app
    python3 -I tools/port.py --ios device [--team TEAM]
                                        build build/port/ios-device/DC2.app, signed for an iPhone by TEAM
                                        or left unsigned
    python3 -I tools/port.py --android  build build/port/android/libmain.so for arm64 Android with the SDKs of
                                        tools/android_sdk.py
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
BINARY = OUT / "dc2"
DEBUG_BINARY = OUT / "dc2-debug"
IMGUI = OUT / "imgui-sdk"
IMGUI_SOURCES = ["imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp", "imgui_demo.cpp",
                 "imgui_impl_sdl3.cpp", "imgui_impl_sdlgpu3.cpp"]
SDL_CFLAGS = []
TARGET = []
LINKER = "clang++"
DARWIN = sys.platform == "darwin"
WINDOWS = sys.platform == "win32"
MACOS_MINIMUM = "13.0"
PLATFORM_SOURCES = []
PREFIX = "_" if DARWIN else ""
OBJCOPY = ((shutil.which("llvm-objcopy") or "/opt/homebrew/opt/llvm/bin/llvm-objcopy") if DARWIN
           else "llvm-objcopy" if WINDOWS else "objcopy")
NM = OBJCOPY.replace("objcopy", "nm")
CXXFILT = "llvm-cxxfilt" if WINDOWS else "c++filt"
POSITION = [] if DARWIN or WINDOWS else ["-fno-pie", "-fno-pic"]
ANDROID_API = 29
TOOLCHAIN = ROOT / "build" / "port" / "llvm-mingw"
# the import libraries clang's MinGW driver links, and the names they define (windows_names)
WINDOWS_IMPORTS = ["gdi32", "comdlg32", "advapi32", "shell32", "user32", "kernel32"]
WINDOWS_NAMES = set()

UPSTREAM_ASM = ["mg_math.cpp", "collision_asm.cpp", "gameutil_asm.cpp", "mg_drawprim_asm.cpp"]
FLAGS = [
    "-O0", "-g", "-pthread", f"-ffile-prefix-map={ROOT}{os.sep}=", "-DPORT", "-DPAL",
    "-fms-extensions", "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off",
    "-fno-strict-return", "-ftrivial-auto-var-init=zero",
    "-Wno-c++11-narrowing", "-Wno-register", "-Wno-return-mismatch", "-w",
    # MinGW lays bitfields out as MSVC does by default, and the engine's PS2 registers and save data need the GCC
    # layout; the disc image is larger than a 32-bit off_t
    *(["-mno-ms-bitfields", "-D_FILE_OFFSET_BITS=64"] if WINDOWS else []),
]
LINK_FLAGS = ["-pthread", *(["-lobjc"] if DARWIN
                            # a fixed image base, as the non-PIE Linux build has; -mwindows opens no console
                            else ["-static", "-mwindows", "-Wl,--disable-dynamicbase", "-Wl,--disable-high-entropy-va",
                                  "-lws2_32"]
                            if WINDOWS else ["-no-pie", "-static-libstdc++", "-static-libgcc"])]
RPATHS = ["@executable_path/lib", "@executable_path/../Frameworks"] if DARWIN else [] if WINDOWS else ["$ORIGIN/lib"]
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
WINDOWS_MANIFEST = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="DC2Toolkit.DC2" version="1.0.0.0"/>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
    </application>
  </compatibility>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
      <longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
    </windowsSettings>
  </application>
</assembly>
"""
LOCALISED = {"mainloop": ["MainBuffer"], "eventedit": ["_Z10VectMatMulPfS_PA4_f"]}
ALIASES = {
    "BuffWorkData__2": "BuffWorkData",
    "EventCamera__2": "EventCamera",
    "_Z8PrimQuadIfEvP11mgCDrawPrim6mgRectIT_ES2_IiE": "_Z11PrimQuad_f_P11mgCDrawPrim9mgRect_f_9mgRect_i_",
    "_Z8PrimQuadIiEvP11mgCDrawPrim6mgRectIT_ES2_IiE": "_Z11PrimQuad_i_P11mgCDrawPrim9mgRect_i_S1_",
}
UNUSED_UNITS = ["movie.cpp"]
UNUSED_DEFINITIONS = {
    "ps2/src/mg_visual.cpp": ["giftag", "texflush_dma", "set_data_func", "mat_pw", "set_tex0_dma", "set_tex0_giftag",
                              "set_texa_dma", "set_texa_giftag", "mat_vif", "mat_vif_dif", "mat_vif_d", "mat_vif_d_tex",
                              "mgSetPkTEX0(u_int*,u_long,u_long)", "mgSetPkTEX0(u_int*,u_long,u_long,u_long)",
                              "mgSetPkTexFlush_TagCnt(u_int*)", "SetPointLight(u_int*,float(*)[4],float(*)[4])",
                              "mgCVisualMDT::SetMaterialRef(u_long128*,mgMaterial*,int)",
                              "mgCVisualMDT::SetPModeRef(u_long128*,int)",
                              *(f"SetData{kind}(int,int,int**,u_long128*,u_long128*,u_long128*,u_long128*,u_long128*)"
                                for kind in range(8))],
    "ps2/src/visualmotion.cpp": ["set_data_func",
                                 *(f"SetData{kind}(int,int,int**,u_long128*,u_long128*,u_long128*,u_long128*,u_long128*,"
                                   "mgVertexWeight*)" for kind in range(8))],
    "ps2/src/mglib.cpp": ["now_prog_id", "mgSendVuProg(u_int*,int)", "mgFlushRenderInfo()", "mgEndPacket(mgCDrawManager*)",
                          "DmaCH1", "DmaCH2", "DmaCH8"],
    "ps2/src/mg_texture.cpp": ["mgLoadImage(u_int*,int,int,int,u_long128*,int,int,int,int,int)", "SetTexFlush_TagCnt(u_int*)",
                               "mgCTextureManager::ReloadTexture(int,u_int*)",
                               "mgCTextureManager::ReloadCLUT(mgCTexture*,u_int*)"],
    "ps2/src/mg_shadow.cpp": ["SetShadowData(u_int*,float(*)[4])"],
    "ps2/src/outline.cpp": ["DrawDivSprite(mgCDrawPrim*,mgRect<int>,mgCTexture*,int*,int,int,int,int)",
                            "DrawDivSprite4(mgCDrawPrim*,mgRect<int>,mgCTexture*,int*,int,int)"],
}
OPERATORS = {
    **dict.fromkeys(["_Znwm", "_Znam", "_ZnwmRKSt9nothrow_t", "_ZnamRKSt9nothrow_t"], "port_new"),
    **dict.fromkeys(["_ZnwmSt11align_val_t", "_ZnamSt11align_val_t", "_ZnwmSt11align_val_tRKSt9nothrow_t",
                     "_ZnamSt11align_val_tRKSt9nothrow_t"], "port_new_aligned"),
    **dict.fromkeys([f"_Zd{kind}Pv{size}{rest}" for kind in "la" for size in ("", "m")
                     for rest in ("", "RKSt9nothrow_t", "St11align_val_t", "St11align_val_tRKSt9nothrow_t")], "port_delete"),
}
if WINDOWS:
    # size_t is unsigned long long on Win64, which mangles as y
    OPERATORS = {re.sub(r"^(_Zn[wa])m|^(_Zd[la]Pv)m", r"\1\2y", name): target for name, target in OPERATORS.items()}
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
    path.write_text(text, encoding="utf-8", newline="\n")


def write_bytes_if_changed(path, data):
    if path.exists() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


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
        if relative in UNUSED_DEFINITIONS:
            text = overrides.strip(text, UNUSED_DEFINITIONS[relative], defined=("PORT", "PAL"), long_is_64=True,
                                   private_variables=True, private_functions=True)
        write_if_changed(COPY / relative, text)
    return sorted(path for path in (COPY / "ps2" / "src").glob("*.cpp") if path.name not in UNUSED_UNITS)


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
                               ["clang++", *TARGET, *POSITION, "-std=c++17", "-c", "-O2", "-g", "-w", "-MMD", "-MF", str(depfile),
                                f"-I{IMGUI}", *SDL_CFLAGS, str(source), "-o", str(target)], [])
    debug = [f"-I{IMGUI}", f"-I{source.parent}"] if name.startswith("mod_") else []
    compiler = LINKER.replace("clang++", "clang") if source.suffix == ".m" else "clang" if c else "clang++"
    objc = ["-fobjc-arc"] if source.suffix == ".m" else []
    command = [compiler, *TARGET, *objc, "-std=c11" if c else "-std=c++26", "-c", *FLAGS, *POSITION, *debug,
               *(["-O2"] if not source.is_relative_to(COPY) and source.name not in ("memory.c", "kernel.c") else []),
               "-MMD", "-MF", str(depfile),
               *([] if c else ["-include", str(PORT / "include" / "port.h")]),
               f"-I{PORT / 'include'}", f"-I{COPY / 'ps2' / 'include'}", f"-I{COPY / 'ps2' / 'include' / 'sce'}",
               *([f"-I{ENGINE / 'port' / 'include'}"] if name.startswith("upstream_") else []),
               f"-I{ENGINE / 'port' / 'src'}", str(source), "-o", str(target)]
    # llvm-objcopy cannot localise or weaken COFF symbols: a local symbol is renamed to one nothing else names, and link
    # puts the upstream objects last instead
    changes = [f"--redefine-sym={symbol}={symbol}.{name}" if WINDOWS else f"--localize-symbol={PREFIX}{symbol}"
               for symbol in LOCALISED.get(name, [])]
    if name.startswith("upstream_") and not WINDOWS:
        changes.append("--weaken")
    if name == "main":
        changes.append(f"--redefine-sym={PREFIX}main={PREFIX}port_game_main")
    return compile_command(source, target, depfile, command_file, command, changes,
                           operators=not c and not name.startswith(("mod_", "port_")),
                           game=not name.startswith(("mod_", "port_")),
                           ps2_names=WINDOWS and not name.startswith("mod_") and not source.is_relative_to(PORT / "windows"))


def move_mach_data(target):
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
                    data[header + 16:header + 32] = b"__GAME".ljust(16, b"\0")
        at += size
    target.write_bytes(data)


def elf_sections(target):
    data = target.read_bytes()
    table = struct.unpack_from("<Q", data, 40)[0]
    size, count, names = struct.unpack_from("<HHH", data, 58)
    strings = struct.unpack_from("<Q", data, table + names * size + 24)[0]
    offsets = [strings + struct.unpack_from("<I", data, table + index * size)[0] for index in range(count)]
    return {data[offset:data.index(b"\0", offset)].decode() for offset in offsets}


def move_elf_data(target):
    renames = []
    for name in elf_sections(target):
        if name.startswith(".data.rel.ro"):
            continue
        for kind in ("data", "bss"):
            if name == f".{kind}" or name.startswith(f".{kind}."):
                renames.append(f"--rename-section={name}=game_{kind}")
    if renames:
        subprocess.run([OBJCOPY, *sorted(renames), str(target)], check=True)


def coff_name(data, at, strings):
    """Where the name of a section header or symbol record at `at` is, and the name."""
    name = data[at:at + 8]
    if name.startswith(b"//"):
        alphabet = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
        at = strings + sum(alphabet.index(digit) << 6 * place for place, digit in enumerate(reversed(name[2:])))
    elif name.startswith(b"/"):
        at = strings + int(name[1:].rstrip(b"\0"))
    elif name.startswith(b"\0\0\0\0"):
        at = strings + struct.unpack_from("<I", data, at + 4)[0]
    else:
        return at, bytes(name.rstrip(b"\0"))
    return at, bytes(data[at:data.index(b"\0", at)])


def move_coff_data(target):
    # PE section names are at most 8 characters: .data and .bss, with their $ groups, become gdata and gbss. llvm-objcopy
    # cannot rename COFF sections, but the new names are as long as the old, so they are written in place, in the
    # section headers and the section symbols that name them.
    data = bytearray(target.read_bytes())
    if struct.unpack_from("<HH", data) == (0, 0xFFFF):
        count, symbols, symbol_count = struct.unpack_from("<III", data, 44)
        headers, symbol_size = 56, 20
    else:
        count, symbols, symbol_count, optional = struct.unpack_from("<H4xIIH", data, 2)
        headers, symbol_size = 20 + optional, 18
    strings = symbols + symbol_count * symbol_size
    moved = set()
    for index in range(count):
        at, name = coff_name(data, headers + index * 40, strings)
        if any(name == kind or name.startswith(kind + b"$") for kind in (b".data", b".bss")):
            data[at] = ord("g")
            moved.add(name)
    index = 0
    while index < symbol_count:
        record = symbols + index * symbol_size
        at, name = coff_name(data, record, strings)
        if name in moved and data[record + symbol_size - 2] == 3:
            data[at] = ord("g")
        index += 1 + data[record + symbol_size - 1]
    if moved:
        target.write_bytes(data)


def move_data(target):
    if DARWIN:
        move_mach_data(target)
    elif WINDOWS:
        move_coff_data(target)
    else:
        move_elf_data(target)


def compile_command(source, target, depfile, command_file, command, changes, operators=False, game=False, ps2_names=False):
    name = target.stem
    signature = json.dumps([command, changes, operators, game])
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
    if ps2_names:
        # the PS2 kernel's CreateThread, GetThreadId and TerminateThread are Windows functions too: in the engine's and
        # port/src's objects they are renamed, so they never meet Windows' and the static runtime never calls the port's
        # (port/windows, ImGui and mods call Windows' own)
        names = set(subprocess.run([NM, "-j", str(target)], capture_output=True, text=True, check=True).stdout.split())
        clashes = sorted(names & WINDOWS_NAMES)
        if clashes:
            subprocess.run([OBJCOPY, *(f"--redefine-sym={name}=ps2_{name}" for name in clashes), str(target)], check=True)
    if game:
        move_data(target)
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
    jobs += [(source, "port_" + source.stem) for source in PLATFORM_SOURCES]
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


def build_id(inputs, binary):
    digest = hashlib.sha256(json.dumps([TARGET, LINK_FLAGS]).encode())
    for path in inputs:
        digest.update(path.read_bytes())
    source = OBJ / f"build_id_{binary.name}.c"
    target = source.with_suffix(".o")
    write_if_changed(source, f'const char port_build_id[] = "{digest.hexdigest()[:32]}";\n')
    if not target.exists() or target.stat().st_mtime < source.stat().st_mtime:
        subprocess.run(["clang", *TARGET, "-c", "-O0", *POSITION, str(source), "-o", str(target)], check=True)
    return target


def windows_names():
    libraries = [str(TOOLCHAIN / "x86_64-w64-mingw32" / "lib" / f"lib{name}.a") for name in WINDOWS_IMPORTS]
    listing = subprocess.run([NM, "--defined-only", "-j", *libraries], capture_output=True, text=True, check=True).stdout
    return {name for name in listing.split() if not name.endswith(":") and not name.startswith(("@", "__imp_"))}


def link_order(inputs):
    # on Windows the upstream objects come last, where the first definition wins, as if they were weak
    return sorted(inputs, key=lambda path: path.stem.startswith("upstream_")) if WINDOWS else list(inputs)


def binary_sections(binary):
    """The binary's sections as they are mapped: (name, address, size, code, writable)."""
    data = binary.read_bytes()
    sections = []
    if data[:4] == b"\xcf\xfa\xed\xfe":
        commands = struct.unpack_from("<I", data, 16)[0]
        at = 32
        for _ in range(commands):
            kind, size = struct.unpack_from("<II", data, at)
            if kind == 0x19:
                writable = struct.unpack_from("<I", data, at + 60)[0] & 2
                for index in range(struct.unpack_from("<I", data, at + 64)[0]):
                    header = at + 72 + index * 80
                    name = data[header:header + 16].rstrip(b"\0").decode()
                    segment = data[header + 16:header + 32].rstrip(b"\0").decode()
                    address, length = struct.unpack_from("<QQ", data, header + 32)
                    flags = struct.unpack_from("<I", data, header + 64)[0]
                    sections.append((f"{segment},{name}", address, length, bool(flags & 0x80000400), bool(writable)))
            at += size
    elif data[:4] == b"\x7fELF":
        table = struct.unpack_from("<Q", data, 40)[0]
        size, count, names = struct.unpack_from("<HHH", data, 58)
        strings = struct.unpack_from("<Q", data, table + names * size + 24)[0]
        for index in range(count):
            name, _kind, flags, address, _offset, length = struct.unpack_from("<IIQQQQ", data, table + index * size)
            # allocated, and not thread-local, whose addresses are a template's
            if flags & 2 and not flags & 0x400 and address:
                text = data[strings + name:data.index(b"\0", strings + name)].decode()
                sections.append((text, address, length, bool(flags & 4), bool(flags & 1)))
    else:
        header = struct.unpack_from("<I", data, 0x3C)[0] + 4
        count, symbols, symbol_count, optional = struct.unpack_from("<H4xIIH", data, header + 2)
        base = struct.unpack_from("<Q", data, header + 20 + 24)[0]
        strings = symbols + symbol_count * 18
        for index in range(count):
            at = header + 20 + optional + index * 40
            name, length, address = struct.unpack_from("<8sII", data, at)
            flags = struct.unpack_from("<I", data, at + 36)[0]
            if flags & 0x02000000:
                continue
            text = coff_name(data, at, strings)[1].decode()
            sections.append((text, base + address, length, bool(flags & 0x20000000), bool(flags & 0x80000000)))
    return sections


def nm_names(name):
    return name[len(PREFIX):] if PREFIX and name.startswith(PREFIX) else name


def write_symbol_table(binary, inputs):
    """BINARY.symbols, which the debug build's watches and crash reports read: the sections, and every symbol with its
    size and the object it comes from. A name defined in several objects (statics) is given to them in link order, the
    order the linker lays them out in."""
    sections = sorted(binary_sections(binary), key=lambda section: section[1])
    listing = subprocess.run([NM, "--defined-only", "--print-size", "--numeric-sort", str(binary)],
                             capture_output=True, text=True, check=True).stdout.splitlines()
    owners = {}
    objects = [path for path in link_order(inputs)]
    names = subprocess.run([NM, "--defined-only", *map(str, objects)], capture_output=True, text=True, check=True).stdout
    current = None
    by_path = {str(path): path.name for path in objects}
    for line in names.splitlines():
        if line.endswith(":") and line[:-1] in by_path:
            current = by_path[line[:-1]]
        elif current and line.strip():
            owners.setdefault(nm_names(line.split()[-1]), []).append(current)
    taken = {}
    symbols = []
    anchor = None
    for line in listing:
        words = line.split()
        if len(words) < 3 or words[-2] in "AaNUuwv":
            continue
        name = nm_names(words[-1])
        address = int(words[0], 16)
        size = int(words[1], 16) if len(words) == 4 else 0
        if name == "port_build_id":
            anchor = address
        mine = owners.get(name, [])
        object_name = mine[taken.get(name, 0)] if taken.get(name, 0) < len(mine) else "-"
        taken[name] = taken.get(name, 0) + 1
        symbols.append([address, size, name, object_name])
    if anchor is None:
        sys.exit(f"{binary.name} has no port_build_id for its symbol table")
    lines = ["dc2 symbols 1", f"anchor {anchor:x}"]
    for name, address, size, code, writable in sections:
        lines.append(f"section {address:x} {size:x} {'x' if code else ''}{'w' if writable else ''}- {name}")
    following = [None] * len(symbols)
    for index in range(len(symbols) - 2, -1, -1):
        later = symbols[index + 1][0]
        following[index] = later if later > symbols[index][0] else following[index + 1]
    at = 0
    for index, (address, size, name, object_name) in enumerate(symbols):
        while at < len(sections) and sections[at][1] + sections[at][2] <= address:
            at += 1
        section = sections[at] if at < len(sections) and sections[at][1] <= address else None
        if not size and section:
            end = section[1] + section[2]
            size = min(following[index] or end, end) - address
        lines.append(f"symbol {address:x} {size:x} {int(bool(section and section[3]))} {object_name} {name}")
    table = binary.with_suffix(".symbols")
    write_if_changed(table, "\n".join(lines) + "\n")
    return table


def link(objects, extra, binary):
    inputs = [*objects, *extra]
    if DARWIN:
        platform = [f"-Wl,-alias,_{target},_{alias}" for alias, target in ALIASES.items()]
    elif WINDOWS:
        # lld's MinGW driver has no --defsym; lld-link's /alternatename resolves the alias to the target. COFF has no
        # weak definitions to make, and an inline function's COMDAT copy clashes with its out-of-line definition, which
        # ELF lets win: duplicates are allowed, the first wins, and the upstream objects come last, as if they were weak.
        platform = ["-Wl,--no-demangle", "-Wl,--allow-multiple-definition",
                    *(f"-Wl,-Xlink=/alternatename:{alias}={target}" for alias, target in ALIASES.items())]
        inputs = link_order(inputs)
    else:
        platform = ["-Wl,--no-demangle", *(f"-Wl,--defsym={alias}={target}" for alias, target in ALIASES.items())]
    fresh = binary.with_name(binary.name + ".linking")
    command = [LINKER, *TARGET, "-o", str(fresh), *map(str, inputs), str(build_id(inputs, binary)), *platform, *LINK_FLAGS]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode == 0:
        os.replace(fresh, binary)
    return result


def undefined(link_errors, objects):
    if not DARWIN:
        return sorted(set(re.findall(r"(?:undefined reference to `|undefined symbol: )([^'\s]+)", link_errors)))
    printed = set(re.findall(r'^\s+"(.+)", referenced from', link_errors, re.M))
    listing = subprocess.run([OBJCOPY.replace("objcopy", "nm"), "-u", "-j", *map(str, objects)],
                             capture_output=True, text=True, check=True).stdout.split()
    symbols = sorted({name[1:] for name in listing if name.startswith("_")})
    return [name for name, text in demangled(symbols).items() if f"_{name}" in printed or text in printed]


def demangled(names):
    if not names:
        return {}
    result = subprocess.run([CXXFILT], input="\n".join(names), capture_output=True, text=True, check=True)
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
    subprocess.run(["clang", *TARGET, "-c", "-O0", *POSITION, "-w", str(source), "-o", str(target)], check=True)
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


def sdk_flags(prefix, movies):
    packages = ["sdl3", *(["libavcodec", "libavutil"] if movies else [])]
    if (prefix / "include/SDL3/SDL.h").exists() and WINDOWS:
        # by path: -static has -l find only static libraries, not the DLLs' import libraries
        names = ["SDL3", *(["avcodec", "avutil"] if movies else [])]
        return [f"-I{prefix / 'include'}"], [str(prefix / "lib" / f"lib{name}.dll.a") for name in names]
    if (prefix / "include/SDL3/SDL.h").exists():
        libraries = ["-lSDL3", *(["-lavcodec", "-lavutil"] if movies else [])]
        rpaths = [*(f"-Wl,-rpath,{path}" for path in RPATHS), *([] if DARWIN or WINDOWS else ["-Wl,--disable-new-dtags"])]
        return [f"-I{prefix / 'include'}"], [f"-L{prefix / 'lib'}", *rpaths, *libraries]
    try:
        cflags = subprocess.check_output(["pkg-config", "--cflags", *packages], text=True)
        libraries = subprocess.check_output(["pkg-config", "--libs", *packages], text=True)
        return shlex.split(cflags), shlex.split(libraries)
    except (OSError, subprocess.CalledProcessError):
        sys.exit("SDL3 or FFmpeg missing: run python3 -I tools/port_sdk.py or pass --sdk PREFIX")


def copy_libraries(prefix):
    if WINDOWS:
        OUT.mkdir(parents=True, exist_ok=True)
        for library in sorted((prefix / "bin").glob("*.dll")):
            shutil.copy2(library, OUT / library.name)
        return
    folder = OUT / "lib"
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    for library in sorted((prefix / "lib").glob("*.dylib" if DARWIN else "*.so*")):
        shutil.copy2(library, folder / library.name, follow_symlinks=False)


def windows_resources():
    """The icon and a manifest that makes the C runtime's char paths UTF-8, as SDL's are, and the window DPI aware."""
    folder = OBJ / "resources"
    folder.mkdir(parents=True, exist_ok=True)
    png = (PORT / "launcher" / "icon.png").read_bytes()
    # an .ico of one PNG image; 0 by 0 stands for 256 pixels or more
    write_bytes_if_changed(folder / "icon.ico", struct.pack("<HHHBBBBHHII", 0, 1, 1, 0, 0, 0, 0, 1, 32, len(png), 22) + png)
    write_if_changed(folder / "dc2.manifest", WINDOWS_MANIFEST)
    write_if_changed(folder / "dc2.rc", '1 ICON "icon.ico"\n1 24 "dc2.manifest"\n')
    target = OBJ / "resources.o"
    sources = [folder / name for name in ("icon.ico", "dc2.manifest", "dc2.rc")]
    if not target.exists() or any(source.stat().st_mtime > target.stat().st_mtime for source in sources):
        subprocess.run(["llvm-windres", "-I", str(folder), str(folder / "dc2.rc"), "-o", str(target)], check=True)
    return target


def ios_flags(kind):
    settings = IOS[kind]
    prefix = ROOT / "build" / "port" / "ios-sdk" / kind
    if not (prefix / "lib" / "libSDL3.a").exists():
        sys.exit("iOS SDL3 and FFmpeg missing: run python3 -I tools/ios_sdk.py")
    sysroot = subprocess.check_output(["xcrun", "--sdk", settings["sdk"], "--show-sdk-path"], text=True).strip()
    TARGET.extend(["-target", settings["target"], "-isysroot", sysroot])
    frameworks = [flag for name in IOS_FRAMEWORKS for flag in ("-framework", name)]
    return [f"-I{prefix / 'include'}"], [f"-L{prefix / 'lib'}", "-lSDL3", "-lavcodec", "-lavutil", *frameworks]


def android_flags():
    global DARWIN, PREFIX, OBJCOPY, NM, POSITION, LINKER
    spec = importlib.util.spec_from_file_location("android_sdk", Path(__file__).with_name("android_sdk.py"))
    android_sdk = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(android_sdk)
    prefix = android_sdk.PREFIX
    if not (prefix / "lib" / "libSDL3.so").exists():
        sys.exit("Android SDL3 and FFmpeg missing: run python3 -I tools/android_sdk.py")
    toolchain = android_sdk.toolchain()
    DARWIN = False
    PREFIX = ""
    OBJCOPY = str(toolchain / "bin" / "llvm-objcopy")
    NM = str(toolchain / "bin" / "llvm-nm")
    POSITION = ["-fPIC"]
    LINKER = str(toolchain / "bin" / "clang++")
    TARGET.extend([f"--target=aarch64-linux-android{ANDROID_API}", f"--sysroot={toolchain / 'sysroot'}"])
    LINK_FLAGS[:] = ["-shared", "-Wl,-soname,libmain.so", "-Wl,--no-undefined", "-Wl,--error-limit=0",
                     "-Wl,-z,max-page-size=16384", "-static-libstdc++"]
    return [f"-I{prefix / 'include'}"], [f"-L{prefix / 'lib'}", "-lSDL3", "-lavcodec", "-lavutil", "-landroid", "-llog",
                                         "-lm"]


def app_info(kind, identifier):
    settings = IOS[kind]
    return {
        "CADisableMinimumFrameDurationOnPhone": True,
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": "DC2",
        "CFBundleExecutable": "DC2",
        "CFBundleIdentifier": identifier,
        "CFBundleIcons": {"CFBundlePrimaryIcon": {"CFBundleIconFiles": ["AppIcon60x60"]}},
        "CFBundleIcons~ipad": {"CFBundlePrimaryIcon": {"CFBundleIconFiles": ["AppIcon60x60", "AppIcon76x76", "AppIcon83.5x83.5"]}},
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "DC2",
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
    app = OUT / "DC2.app"
    shutil.rmtree(app, ignore_errors=True)
    app.mkdir(parents=True)
    shutil.copy2(executable, app / "DC2")
    if executable.with_suffix(".symbols").exists() and executable == DEBUG_BINARY:
        shutil.copy2(executable.with_suffix(".symbols"), app / "DC2.symbols")
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
    parser.add_argument("--sdk", type=Path, default=OUT / ("windows-sdk" if WINDOWS else "sdk"),
                        help="the prefix tools/port_sdk.py installed SDL3 and FFmpeg in")
    parser.add_argument("--ios", choices=sorted(IOS), help="build DC2.app for the iOS simulator or an iPhone")
    parser.add_argument("--bundle-id", default="dc2toolkit.dc2", help="the iOS app's bundle identifier")
    parser.add_argument("--android", action="store_true", help="build libmain.so for arm64 Android")
    parser.add_argument("--team", help="the Apple development team that signs the iPhone app, as Xcode lists it; without it the app is unsigned")
    parser.add_argument("arguments", nargs="*", help="arguments for the port, with --run")
    options = parser.parse_args()
    if options.ios:
        OUT = ROOT / "build" / "port" / f"ios-{options.ios}"
        OBJ = OUT / "obj"
        BINARY = OUT / "dc2"
        DEBUG_BINARY = OUT / "dc2-debug"
        LINKER = subprocess.check_output(["xcrun", "--find", "clang++"], text=True).strip()
        cflags, libraries = ios_flags(options.ios)
        PLATFORM_SOURCES.extend(sorted((PORT / "src").glob("*.m")))
        FLAGS.append("-DPORT_MOVIES")
    elif options.android:
        OUT = ROOT / "build" / "port" / "android"
        OBJ = OUT / "obj"
        BINARY = OUT / "libmain.so"
        cflags, libraries = android_flags()
        PLATFORM_SOURCES.extend(sorted((PORT / "android").glob("*.c")))
        FLAGS.append("-DPORT_MOVIES")
    elif WINDOWS:
        if not (TOOLCHAIN / "bin" / "clang.exe").exists():
            sys.exit("llvm-mingw missing: run python -I tools/port_sdk.py")
        os.environ["PATH"] = f"{TOOLCHAIN / 'bin'}{os.pathsep}{os.environ['PATH']}"
        WINDOWS_NAMES.update(windows_names())
        OUT = ROOT / "build" / "port" / "windows"
        OBJ = OUT / "obj"
        BINARY = OUT / "dc2.exe"
        DEBUG_BINARY = OUT / "dc2-debug.exe"
        PLATFORM_SOURCES.extend(sorted((PORT / "windows").glob("*.c")))
        if options.movies:
            FLAGS.append("-DPORT_MOVIES")
        cflags, libraries = sdk_flags(options.sdk.resolve(), options.movies)
        copy_libraries(options.sdk.resolve())
    else:
        if DARWIN:
            TARGET.append(f"-mmacosx-version-min={MACOS_MINIMUM}")
        if options.movies:
            FLAGS.append("-DPORT_MOVIES")
        cflags, libraries = sdk_flags(options.sdk.resolve(), options.movies)
        if (options.sdk / "lib").is_dir():
            copy_libraries(options.sdk.resolve())
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
    if WINDOWS:
        objects.append(windows_resources())
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
    manifest = [*objects, *extra, OBJ / f"build_id_{BINARY.name}.o"]
    write_if_changed(OUT / "objects.json", json.dumps([str(path) for path in manifest], indent=2) + "\n")
    if options.debug:
        debugged = link([*objects, *debug_objects], extra, DEBUG_BINARY)
        if debugged.returncode != 0:
            sys.exit(debugged.stderr)
        write_symbol_table(DEBUG_BINARY, [*objects, *debug_objects, *extra, OBJ / f"build_id_{DEBUG_BINARY.name}.o"])
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
        if options.ios == "device" and options.team:
            print(f"signed for team {options.team}; the profile expires {sign_for_device(app, options.team, options.bundle_id)}")
        return
    if options.run:
        sys.stdout.flush()
        code = subprocess.run([str(BINARY), *(["--graphics"] if options.graphics else []), *options.arguments]).returncode
        sys.exit(128 - code if code < 0 else code)


if __name__ == "__main__":
    main()
