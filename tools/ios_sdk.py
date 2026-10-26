"""Run python3 -I tools/ios_sdk.py [--force] to build static SDL3 and the FFmpeg MPEG-2 decoder for iOS device and simulator into build/port/ios-sdk/<target>/{include,lib}, link a test program per target and print the port's compile and link flags."""

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
PREFIX = ROOT / "build" / "port" / "ios-sdk"
WORK = ROOT / "build" / "port" / "ios-sdk-work"
SDL_VERSION = "3.4.18"
SDL_SHA256 = "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"
SDL_URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-{SDL_VERSION}.tar.gz"
FFMPEG_VERSION = "8.0.1"
FFMPEG_SHA256 = "05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41"
FFMPEG_URL = f"https://ffmpeg.org/releases/ffmpeg-{FFMPEG_VERSION}.tar.xz"
MIN_IOS = "17.0"
TARGETS = {
    "device": {"sdk": "iphoneos", "cmake_sysroot": "iphoneos", "min_flag": f"-mios-version-min={MIN_IOS}"},
    "simulator": {"sdk": "iphonesimulator", "cmake_sysroot": "iphonesimulator", "min_flag": f"-mios-simulator-version-min={MIN_IOS}"},
}
FFMPEG_CONFIGURE = [
    "--disable-everything", "--disable-programs", "--disable-doc", "--enable-decoder=mpeg2video",
    "--enable-parser=mpegvideo", "--disable-avdevice", "--disable-avformat", "--disable-avfilter",
    "--disable-swscale", "--disable-swresample", "--disable-network", "--disable-shared", "--enable-static",
    "--enable-cross-compile", "--target-os=darwin", "--arch=aarch64", "--disable-iconv", "--disable-zlib",
    "--disable-securetransport", "--disable-videotoolbox", "--disable-audiotoolbox", "--disable-coreimage",
    "--disable-appkit", "--disable-avfoundation", "--disable-metal",
]
CANDIDATE_FRAMEWORKS = [
    "AVFoundation", "AudioToolbox", "CoreAudio", "CoreBluetooth", "CoreFoundation", "CoreGraphics",
    "CoreHaptics", "CoreMedia", "CoreMotion", "CoreVideo", "Foundation", "GameController", "ImageIO",
    "Metal", "MobileCoreServices", "OpenGLES", "QuartzCore", "UIKit", "UniformTypeIdentifiers",
]
TEST_SOURCE = """#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
int main(void)
{
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD);
    SDL_GPUDevice *device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, NULL);
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    return device == NULL || codec == NULL;
}
"""


def run(command, cwd=None, log=None):
    started = time.time()
    if log is None:
        subprocess.run(command, cwd=cwd, check=True)
    else:
        with open(log, "w") as out:
            result = subprocess.run(command, cwd=cwd, stdout=out, stderr=subprocess.STDOUT)
        if result.returncode != 0:
            print(Path(log).read_text()[-4000:])
            raise SystemExit(f"failed: {' '.join(map(str, command))} (see {log})")
    return time.time() - started


def capture(command):
    return subprocess.run(command, check=True, text=True, capture_output=True).stdout.strip()


def fetch(url, sha256, name, destination):
    archive = WORK / name
    if not archive.exists():
        urllib.request.urlretrieve(url, archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != sha256:
        archive.unlink()
        raise SystemExit(f"{name} checksum mismatch")
    if not destination.exists():
        with tarfile.open(archive) as source:
            source.extractall(WORK, filter="data")
    return destination


def sdl_installed(target):
    return (PREFIX / target / "lib" / "libSDL3.a").exists()


def ffmpeg_installed(target):
    return (PREFIX / target / "lib" / "libavcodec.a").exists() and (PREFIX / target / "lib" / "libavutil.a").exists()


def allow_simulator_gpu(source):
    path = source / "src" / "gpu" / "metal" / "SDL_gpu_metal.m"
    text = path.read_text()
    old = "hasHardwareSupport = [device supportsFamily:MTLGPUFamilyApple3];"
    new = "hasHardwareSupport = TARGET_OS_SIMULATOR || [device supportsFamily:MTLGPUFamilyApple3];"
    if old in text:
        path.write_text(text.replace(old, new))


def build_sdl(name, source):
    allow_simulator_gpu(source)
    target = TARGETS[name]
    build = WORK / f"sdl-build-{name}"
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir(parents=True)
    log = WORK / f"sdl-{name}.log"
    seconds = run([
        "cmake", "-S", str(source), "-B", str(build), "-G", "Unix Makefiles",
        "-DCMAKE_SYSTEM_NAME=iOS", f"-DCMAKE_OSX_SYSROOT={target['cmake_sysroot']}",
        "-DCMAKE_OSX_ARCHITECTURES=arm64", f"-DCMAKE_OSX_DEPLOYMENT_TARGET={MIN_IOS}",
        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={PREFIX / name}",
        "-DSDL_SHARED=OFF", "-DSDL_STATIC=ON", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_TESTS=OFF",
        "-DSDL_EXAMPLES=OFF", "-DSDL_INSTALL_TESTS=OFF", "-DSDL_DISABLE_INSTALL_DOCS=ON",
    ], log=log)
    seconds += run(["cmake", "--build", str(build), "--parallel", str(os.cpu_count())], log=log)
    seconds += run(["cmake", "--install", str(build)], log=log)
    return seconds


def ffmpeg_configure(name, source, build, log, asm):
    target = TARGETS[name]
    sdk = target["sdk"]
    flags = f"-arch arm64 -isysroot {capture(['xcrun', '-sdk', sdk, '--show-sdk-path'])} {target['min_flag']}"
    command = [
        str(source / "configure"), "--prefix=/", f"--cc=xcrun -sdk {sdk} clang",
        f"--extra-cflags={flags}", f"--extra-ldflags={flags}", *FFMPEG_CONFIGURE,
    ]
    if not asm:
        command.append("--disable-asm")
    return run(command, cwd=build, log=log)


def build_ffmpeg(name, source):
    build = WORK / f"ffmpeg-build-{name}"
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir(parents=True)
    log = WORK / f"ffmpeg-{name}.log"
    try:
        seconds = ffmpeg_configure(name, source, build, log, True)
        seconds += run(["make", f"-j{os.cpu_count()}"], cwd=build, log=log)
    except SystemExit:
        print(f"FFmpeg {name} with asm failed, retrying with --disable-asm")
        shutil.rmtree(build, ignore_errors=True)
        build.mkdir(parents=True)
        seconds = ffmpeg_configure(name, source, build, log, False)
        seconds += run(["make", f"-j{os.cpu_count()}"], cwd=build, log=log)
    seconds += run(["make", "install", f"DESTDIR={PREFIX / name}"], cwd=build, log=log)
    licenses = PREFIX / name / "share" / "licenses" / "ffmpeg"
    licenses.mkdir(parents=True, exist_ok=True)
    for file in ["COPYING.LGPLv2.1", "LICENSE.md"]:
        shutil.copyfile(source / file, licenses / file)
    return seconds


def compile_flags(name):
    target = TARGETS[name]
    return ["-arch", "arm64", "-isysroot", capture(["xcrun", "-sdk", target["sdk"], "--show-sdk-path"]),
            target["min_flag"], f"-I{PREFIX / name / 'include'}"]


def link_test(name, frameworks):
    target = TARGETS[name]
    folder = WORK / f"test-{name}"
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "test.c").write_text(TEST_SOURCE)
    lib = PREFIX / name / "lib"
    command = ["xcrun", "-sdk", target["sdk"], "clang", *compile_flags(name), str(folder / "test.c"),
               "-o", str(folder / "test"), f"-L{lib}", "-lSDL3", "-lavcodec", "-lavutil", "-lm", "-lc++",
               *[arg for framework in frameworks for arg in ("-framework", framework)]]
    result = subprocess.run(command, capture_output=True, text=True)
    return result.returncode == 0, result.stderr


def needed_frameworks(name):
    ok, errors = link_test(name, CANDIDATE_FRAMEWORKS)
    if not ok:
        raise SystemExit(f"{name} test program does not link:\n{errors}")
    needed = []
    for framework in CANDIDATE_FRAMEWORKS:
        without = [other for other in CANDIDATE_FRAMEWORKS if other != framework]
        if not link_test(name, without)[0]:
            needed.append(framework)
    ok, errors = link_test(name, needed)
    if not ok:
        raise SystemExit(f"{name} test program does not link with pruned frameworks:\n{errors}")
    return needed


def report(name, frameworks):
    includes = f"-I{PREFIX / name / 'include'}"
    lib = PREFIX / name / "lib"
    target = TARGETS[name]
    sysroot = capture(["xcrun", "-sdk", target["sdk"], "--show-sdk-path"])
    link = [f"-L{lib}", "-lSDL3", "-lavcodec", "-lavutil", "-lm", "-lc++"]
    link += [arg for framework in frameworks for arg in ("-framework", framework)]
    print(f"\n[{name}]")
    print(f"compile: -arch arm64 -isysroot {sysroot} {target['min_flag']} {includes}")
    print(f"link:    -arch arm64 -isysroot {sysroot} {target['min_flag']} {' '.join(link)}")


def main():
    force = "--force" in sys.argv[1:]
    WORK.mkdir(parents=True, exist_ok=True)
    sdl_source = ffmpeg_source = None
    times = {}
    for name in TARGETS:
        if force or not sdl_installed(name):
            sdl_source = sdl_source or fetch(SDL_URL, SDL_SHA256, "sdl.tar.gz", WORK / f"SDL3-{SDL_VERSION}")
            times[f"SDL3 {name}"] = build_sdl(name, sdl_source)
        if force or not ffmpeg_installed(name):
            ffmpeg_source = ffmpeg_source or fetch(FFMPEG_URL, FFMPEG_SHA256, "ffmpeg.tar.xz", WORK / f"ffmpeg-{FFMPEG_VERSION}")
            times[f"FFmpeg {name}"] = build_ffmpeg(name, ffmpeg_source)
    for label, seconds in times.items():
        print(f"{label}: {seconds:.0f}s")
    for name in TARGETS:
        report(name, needed_frameworks(name))


if __name__ == "__main__":
    main()
