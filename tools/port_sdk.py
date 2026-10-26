"""Run python3 -I tools/port_sdk.py [--force] to build shared SDL3 and the LGPL FFmpeg MPEG-2 decoder from pinned sources
into build/port/sdk."""

import argparse
import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
PREFIX = ROOT / "build" / "port" / "sdk"
WORK = ROOT / "build" / "port" / "sdk-work"
MACOS_MINIMUM = "13.0"
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


def run(command, log, cwd=None):
    with open(log, "a") as out:
        result = subprocess.run(command, cwd=cwd, stdout=out, stderr=subprocess.STDOUT)
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
    folder = WORK / name.removesuffix(".tar.gz").removesuffix(".tar.xz")
    if not folder.exists():
        with tarfile.open(archive) as source:
            source.extractall(WORK, filter="data")
    return folder


def build_sdl():
    source = fetch(SDL_URL, SDL_SHA256, f"SDL3-{SDL_VERSION}.tar.gz")
    build = WORK / "sdl"
    shutil.rmtree(build, ignore_errors=True)
    log = WORK / "sdl.log"
    log.unlink(missing_ok=True)
    darwin = ["-DCMAKE_OSX_ARCHITECTURES=arm64", f"-DCMAKE_OSX_DEPLOYMENT_TARGET={MACOS_MINIMUM}"]
    run(["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={PREFIX}",
         "-DCMAKE_INSTALL_LIBDIR=lib", "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_TESTS=OFF",
         "-DSDL_EXAMPLES=OFF", "-DSDL_INSTALL_TESTS=OFF", *(darwin if sys.platform == "darwin" else [])], log)
    run(["cmake", "--build", str(build), "--parallel", str(os.cpu_count())], log)
    run(["cmake", "--install", str(build), "--strip"], log)
    licences = PREFIX / "share" / "licenses" / "SDL3"
    licences.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / "LICENSE.txt", licences / "LICENSE.txt")


def build_ffmpeg():
    source = fetch(FFMPEG_URL, FFMPEG_SHA256, f"ffmpeg-{FFMPEG_VERSION}.tar.xz")
    build = WORK / "ffmpeg"
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir(parents=True)
    log = WORK / "ffmpeg.log"
    log.unlink(missing_ok=True)
    flags = f"-arch arm64 -mmacosx-version-min={MACOS_MINIMUM}"
    darwin = [f"--extra-cflags={flags}", f"--extra-ldflags={flags}", "--install-name-dir=@rpath"]
    run([str(source / "configure"), "--prefix=/", "--cc=clang", *FFMPEG_CONFIGURE,
         *(darwin if sys.platform == "darwin" else [])], log, build)
    run(["make", f"-j{os.cpu_count()}"], log, build)
    run(["make", "install", f"DESTDIR={PREFIX}"], log, build)
    shutil.rmtree(PREFIX / "share" / "ffmpeg", ignore_errors=True)
    licences = PREFIX / "share" / "licenses" / "ffmpeg"
    licences.mkdir(parents=True, exist_ok=True)
    for name in ["COPYING.LGPLv2.1", "LICENSE.md"]:
        shutil.copyfile(source / name, licences / name)
    (licences / "BUILD.txt").write_text(
        f"FFmpeg {FFMPEG_VERSION}, {FFMPEG_URL} (sha256 {FFMPEG_SHA256}), built unmodified with\n"
        f"./configure {' '.join(FFMPEG_CONFIGURE)}\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--force", action="store_true", help="rebuild what is already installed")
    options = parser.parse_args()
    machine = platform.machine()
    if (sys.platform, machine) not in (("darwin", "arm64"), ("linux", "x86_64")):
        sys.exit(f"the port builds on macOS arm64 and Linux x86-64, not {sys.platform} {machine}")
    WORK.mkdir(parents=True, exist_ok=True)
    if options.force or not (PREFIX / "include" / "SDL3" / "SDL.h").exists():
        build_sdl()
    if options.force or not (PREFIX / "include" / "libavcodec" / "avcodec.h").exists():
        build_ffmpeg()
    print(PREFIX)


if __name__ == "__main__":
    main()
