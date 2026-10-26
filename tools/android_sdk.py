"""Run python3 -I tools/android_sdk.py [--force] to build shared SDL3 and the static LGPL FFmpeg MPEG-2 decoder for
Android arm64-v8a with the NDK into build/port/android-sdk, beside SDL's Java sources. The NDK comes from
ANDROID_NDK_HOME, or the newest in ANDROID_HOME/ndk."""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
PREFIX = ROOT / "build" / "port" / "android-sdk"
WORK = ROOT / "build" / "port" / "android-sdk-work"
API = 29
ABI = "arm64-v8a"
SDL_VERSION = "3.4.18"
SDL_SHA256 = "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"
SDL_URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-{SDL_VERSION}.tar.gz"
FFMPEG_VERSION = "8.0.1"
FFMPEG_SHA256 = "05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41"
FFMPEG_URL = f"https://ffmpeg.org/releases/ffmpeg-{FFMPEG_VERSION}.tar.xz"
FFMPEG_CONFIGURE = [
    "--disable-everything", "--disable-autodetect", "--disable-programs", "--disable-doc", "--enable-static",
    "--disable-shared", "--enable-pic", "--enable-decoder=mpeg2video", "--enable-parser=mpegvideo",
    "--disable-avdevice", "--disable-avformat", "--disable-avfilter", "--disable-swscale", "--disable-swresample",
    "--disable-network", "--enable-cross-compile", "--target-os=android", "--arch=aarch64",
]


def sdk_root():
    for name in ["ANDROID_HOME", "ANDROID_SDK_ROOT"]:
        if os.environ.get(name):
            return Path(os.environ[name])
    return Path.home() / "Library/Android/sdk"


def ndk():
    if os.environ.get("ANDROID_NDK_HOME"):
        return Path(os.environ["ANDROID_NDK_HOME"])
    found = sorted((sdk_root() / "ndk").glob("*"), key=lambda path: [int(part) for part in path.name.split(".")])
    if not found:
        sys.exit("no Android NDK: set ANDROID_NDK_HOME")
    return found[-1]


def toolchain():
    return next((ndk() / "toolchains/llvm/prebuilt").iterdir())


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
    run(["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={PREFIX}",
         f"-DCMAKE_TOOLCHAIN_FILE={ndk() / 'build/cmake/android.toolchain.cmake'}", f"-DANDROID_ABI={ABI}",
         f"-DANDROID_PLATFORM=android-{API}", "-DCMAKE_INSTALL_LIBDIR=lib", "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF",
         "-DSDL_TEST_LIBRARY=OFF", "-DSDL_TESTS=OFF", "-DSDL_EXAMPLES=OFF", "-DSDL_INSTALL_TESTS=OFF"], log)
    run(["cmake", "--build", str(build), "--parallel", str(os.cpu_count())], log)
    run(["cmake", "--install", str(build), "--strip"], log)
    shutil.rmtree(PREFIX / "java", ignore_errors=True)
    shutil.copytree(source / "android-project/app/src/main/java", PREFIX / "java")
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
    tools = toolchain() / "bin"
    run([str(source / "configure"), "--prefix=/", *FFMPEG_CONFIGURE,
         f"--cc={tools / f'aarch64-linux-android{API}-clang'}", f"--ar={tools / 'llvm-ar'}", f"--nm={tools / 'llvm-nm'}",
         f"--ranlib={tools / 'llvm-ranlib'}", f"--strip={tools / 'llvm-strip'}"], log, build)
    run(["make", f"-j{os.cpu_count()}"], log, build)
    run(["make", "install", f"DESTDIR={PREFIX}"], log, build)
    shutil.rmtree(PREFIX / "share" / "ffmpeg", ignore_errors=True)
    shutil.rmtree(PREFIX / "lib" / "pkgconfig", ignore_errors=True)
    licences = PREFIX / "share" / "licenses" / "ffmpeg"
    licences.mkdir(parents=True, exist_ok=True)
    for name in ["COPYING.LGPLv2.1", "LICENSE.md"]:
        shutil.copyfile(source / name, licences / name)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--force", action="store_true", help="rebuild what is already installed")
    options = parser.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    if options.force or not (PREFIX / "lib" / "libSDL3.so").exists():
        build_sdl()
    if options.force or not (PREFIX / "lib" / "libavcodec.a").exists():
        build_ffmpeg()
    print(PREFIX)


if __name__ == "__main__":
    main()
