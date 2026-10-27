"""Run python3 -I tools/port_sdk.py [--force] to build shared SDL3 and the LGPL FFmpeg MPEG-2 decoder from pinned sources
into build/port/sdk. On Windows they go to build/port/windows-sdk, built by the pinned llvm-mingw it first unpacks into
build/port/llvm-mingw (FFmpeg's configure runs in Git for Windows' bash), which also builds the port."""

import argparse
import hashlib
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
JOBS = len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else os.cpu_count() or 1
WINDOWS = sys.platform == "win32"
PREFIX = ROOT / "build" / "port" / ("windows-sdk" if WINDOWS else "sdk")
WORK = ROOT / "build" / "port" / ("windows-sdk-work" if WINDOWS else "sdk-work")
TOOLCHAIN = ROOT / "build" / "port" / "llvm-mingw"
MACOS_MINIMUM = "13.0"
LLVM_MINGW = "llvm-mingw-20261020-ucrt-x86_64"
LLVM_MINGW_SHA256 = "fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd"
LLVM_MINGW_URL = f"https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/{LLVM_MINGW}.zip"
GIT_BASH = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Git" / "bin" / "bash.exe"
SDL_VERSION = "3.4.18"
SDL_SHA256 = "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"
SDL_URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-{SDL_VERSION}.tar.gz"
FFMPEG_VERSION = "8.0.1"
FFMPEG_SHA256 = "05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41"
FFMPEG_URL = f"https://ffmpeg.org/releases/ffmpeg-{FFMPEG_VERSION}.tar.xz"
FFMPEG_CONFIGURE = [
    "--disable-everything", "--disable-autodetect", "--disable-programs", "--disable-doc", "--disable-x86asm",
    "--disable-static", "--enable-shared", "--enable-decoder=mpeg2video", "--enable-parser=mpegvideo",
    "--disable-avdevice", "--disable-avformat", "--disable-avfilter", "--disable-swscale", "--disable-swresample",
    "--disable-network",
]


def run(command, log, cwd=None, env=None):
    with open(log, "a") as out:
        result = subprocess.run(command, cwd=cwd, stdout=out, stderr=subprocess.STDOUT, env=env)
    if result.returncode != 0:
        print(Path(log).read_text()[-4000:])
        sys.exit(f"failed: {' '.join(map(str, command))} (see {log})")


def fetch(url, sha256, name):
    archive = WORK / name
    if not archive.exists():
        urllib.request.urlretrieve(url, archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != sha256:
        archive.unlink()
        sys.exit(f"{name} checksum mismatch")
    folder = WORK / name.removesuffix(".tar.gz").removesuffix(".tar.xz").removesuffix(".zip")
    if not folder.exists():
        if name.endswith(".zip"):
            with zipfile.ZipFile(archive) as source:
                source.extractall(WORK)
        else:
            with tarfile.open(archive) as source:
                source.extractall(WORK, filter="data")
    return folder


def toolchain():
    """Windows only: llvm-mingw in build/port/llvm-mingw, with its bin on PATH for this process and the builds it runs."""
    if not (TOOLCHAIN / "bin" / "clang.exe").exists():
        WORK.mkdir(parents=True, exist_ok=True)
        shutil.rmtree(TOOLCHAIN, ignore_errors=True)
        fetch(LLVM_MINGW_URL, LLVM_MINGW_SHA256, f"{LLVM_MINGW}.zip").rename(TOOLCHAIN)
    os.environ["PATH"] = f"{TOOLCHAIN / 'bin'}{os.pathsep}{os.environ['PATH']}"


def build_sdl():
    source = fetch(SDL_URL, SDL_SHA256, f"SDL3-{SDL_VERSION}.tar.gz")
    build = WORK / "sdl"
    shutil.rmtree(build, ignore_errors=True)
    log = WORK / "sdl.log"
    log.unlink(missing_ok=True)
    darwin = ["-DCMAKE_OSX_ARCHITECTURES=arm64", f"-DCMAKE_OSX_DEPLOYMENT_TARGET={MACOS_MINIMUM}"]
    windows = ["-G", "MinGW Makefiles", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
               "-DCMAKE_MAKE_PROGRAM=mingw32-make"]
    # CMake's MinGW Makefiles refuse a PATH with an sh.exe on it, as Git's bash has
    path = os.pathsep.join(folder for folder in os.environ["PATH"].split(os.pathsep)
                           if not (Path(folder) / "sh.exe").exists())
    env = {**os.environ, "PATH": path} if WINDOWS else None
    run(["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={PREFIX}",
         "-DCMAKE_INSTALL_LIBDIR=lib", "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_TESTS=OFF",
         "-DSDL_EXAMPLES=OFF", "-DSDL_INSTALL_TESTS=OFF", *(darwin if sys.platform == "darwin" else []),
         *(windows if WINDOWS else [])], log, env=env)
    run(["cmake", "--build", str(build), "--parallel", str(JOBS)], log, env=env)
    run(["cmake", "--install", str(build), "--strip"], log, env=env)
    licences = PREFIX / "share" / "licenses" / "SDL3"
    licences.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / "LICENSE.txt", licences / "LICENSE.txt")


def build_ffmpeg():
    source = fetch(FFMPEG_URL, FFMPEG_SHA256, f"ffmpeg-{FFMPEG_VERSION}.tar.xz")
    build = WORK / "ffmpeg"
    shutil.rmtree(build, ignore_errors=True)
    if WINDOWS:
        # built in a fresh tree of its own, where configure names the sources by relative paths: by absolute paths, its
        # header installs make command lines longer than Windows takes
        shutil.copytree(source, build)
        source = build
    else:
        build.mkdir(parents=True)
    log = WORK / "ffmpeg.log"
    log.unlink(missing_ok=True)
    flags = f"-arch arm64 -mmacosx-version-min={MACOS_MINIMUM}"
    darwin = [f"--extra-cflags={flags}", f"--extra-ldflags={flags}", "--install-name-dir=@rpath"]
    windows = ["--target-os=mingw32", "--arch=x86_64"]
    configure = [str(source / "configure"), "--prefix=/", "--cc=clang", *FFMPEG_CONFIGURE,
                 *(darwin if sys.platform == "darwin" else []), *(windows if WINDOWS else [])]
    make = ["make", f"-j{JOBS}"]
    install = ["make", "install", f"DESTDIR={PREFIX}"]
    if WINDOWS:
        if not GIT_BASH.exists():
            sys.exit(f"FFmpeg's configure needs Git for Windows' bash, not found at {GIT_BASH}")
        # llvm-mingw's mingw32-make, which reads C:/ paths but not bash's /c/ ones
        configure[0] = "./configure"
        make[0] = install[0] = "mingw32-make"
        install[-1] = f"DESTDIR={PREFIX.as_posix()}"
        configure, make, install = ([str(GIT_BASH), "-c", " ".join(map(shlex.quote, command))]
                                    for command in (configure, make, install))
    run(configure, log, build)
    run(make, log, build)
    run(install, log, build)
    shutil.rmtree(PREFIX / "share" / "ffmpeg", ignore_errors=True)
    licences = PREFIX / "share" / "licenses" / "ffmpeg"
    licences.mkdir(parents=True, exist_ok=True)
    for name in ["COPYING.LGPLv2.1", "LICENSE.md"]:
        shutil.copyfile(source / name, licences / name)
    (licences / "BUILD.txt").write_text(
        f"FFmpeg {FFMPEG_VERSION}, {FFMPEG_URL} (sha256 {FFMPEG_SHA256}), built unmodified with\n"
        f"./configure {' '.join([*FFMPEG_CONFIGURE, *(windows if WINDOWS else [])])}\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--force", action="store_true", help="rebuild what is already installed")
    options = parser.parse_args()
    machine = platform.machine()
    if (sys.platform, machine) not in (("darwin", "arm64"), ("linux", "x86_64"), ("win32", "AMD64")):
        sys.exit(f"the port builds on macOS arm64, Linux x86-64 and Windows x64, not {sys.platform} {machine}")
    WORK.mkdir(parents=True, exist_ok=True)
    if WINDOWS:
        toolchain()
    if options.force or not (PREFIX / "include" / "SDL3" / "SDL.h").exists():
        build_sdl()
    if options.force or not (PREFIX / "include" / "libavcodec" / "avcodec.h").exists():
        build_ffmpeg()
    print(PREFIX)


if __name__ == "__main__":
    main()
