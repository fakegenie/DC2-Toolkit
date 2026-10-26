"""Run python3 -I tools/port_package.py [--version NAME] [--ios | --android] to build the port with movies and pack it for
players into build/port/package: Dark Chronicle.app in a zip on macOS, or a folder with darkchronicle and its libraries in
a tar.gz on Linux, each beside the README, NOTICE, LICENCE and THIRD-PARTY-LICENCES.md. Build the SDK first with
python3 -I tools/port_sdk.py. With --ios it packs an unsigned DarkChronicle.app for iPhone and iPad in an .ipa, with the
NOTICE and licences inside the app, from the SDKs python3 -I tools/ios_sdk.py builds. With --android it packs an arm64
APK, signed with port/android's public key and with the NOTICE and licences in its assets, from the SDKs
python3 -I tools/android_sdk.py builds."""

import argparse
import importlib.util
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "port"
SDK = OUT / "sdk"
IOS_SDK = OUT / "ios-sdk" / "device"
ANDROID_SDK = OUT / "android-sdk"
ANDROID = ROOT / "port" / "android"
ANDROID_MINIMUM = 29
ANDROID_TARGET = 35
KEY = {"store": ANDROID / "dark-chronicle.keystore", "alias": "dark-chronicle", "password": "dark-chronicle"}
PACKAGE = OUT / "package"
DARWIN = sys.platform == "darwin"
IDENTIFIER = "dc2toolkit.darkchronicle"
MACOS_MINIMUM = "13.0"
ICON_SIZES = {"16x16": 16, "16x16@2x": 32, "32x32": 32, "32x32@2x": 64, "128x128": 128, "128x128@2x": 256,
              "256x256": 256, "256x256@2x": 512, "512x512": 512}


def version():
    return subprocess.check_output(["git", "describe", "--tags", "--always"], cwd=ROOT, text=True).strip()


def needed(binary):
    if DARWIN:
        listing = subprocess.check_output(["otool", "-L", str(binary)], text=True)
        return re.findall(r"^\s+@rpath/(\S+)", listing, re.M)
    listing = subprocess.check_output(["readelf", "-d", str(binary)], text=True)
    return re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", listing)


def copy_libraries(binary, folder):
    folder.mkdir(parents=True, exist_ok=True)
    pending = [binary]
    while pending:
        for name in needed(pending.pop()):
            source = SDK / "lib" / name
            target = folder / name
            if target.exists() or not source.exists():
                continue
            shutil.copyfile(source.resolve(), target)
            pending.append(target)


def stb_licence():
    text = (ROOT / "port/include/stb_truetype.h").read_text(encoding="utf-8")
    return text[text.index("ALTERNATIVE A - MIT License"):text.index("------", text.index("ALTERNATIVE A"))].strip()


def shared_ffmpeg():
    return ("GNU Lesser General Public License 2.1 or later. Its MPEG-2 decoder plays the game's movies, from the shared "
            "libraries libavcodec and libavutil shipped beside the program, which can be replaced with your own build. "
            + (SDK / "share/licenses/ffmpeg/BUILD.txt").read_text().strip())


def static_ffmpeg(tool):
    return ("GNU Lesser General Public License 2.1 or later. Its MPEG-2 decoder plays the game's movies. FFmpeg 8.0.1 "
            f"(https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz), unmodified, is built by tools/{tool} and linked "
            "into the app; the app's complete source, https://github.com/fakegenie/DC2-Toolkit, lets you relink it "
            "with your own build of FFmpeg.")


def licences(sdk, ffmpeg):
    sections = [
        ("SDL 3", "zlib licence. https://libsdl.org", (sdk / "share/licenses/SDL3/LICENSE.txt").read_text()),
        ("FFmpeg", ffmpeg, (sdk / "share/licenses/ffmpeg/COPYING.LGPLv2.1").read_text()),
        ("Manrope", "SIL Open Font License 1.1, the launcher's font. https://github.com/sharanda/manrope",
         (ROOT / "port/launcher/OFL.txt").read_text()),
        ("stb_truetype", "MIT licence or public domain, which draws the launcher's text. https://github.com/nothings/stb",
         stb_licence()),
        ("Kazarov's libvu0", "MIT licence, from ChronicleTwo's Kazarov toolchain.",
         (ROOT / "engine/tools/kazarov/LICENSE").read_text()),
    ]
    lines = ["# Third-party licences", "", "The Dark Chronicle port contains or ships with the following."]
    for name, about, text in sections:
        lines += ["", f"## {name}", "", about, "", "```", text.strip(), "```"]
    return "\n".join(lines) + "\n"


def notices(folder, sdk, ffmpeg):
    shutil.copyfile(ROOT / "port/package/NOTICE.md", folder / "NOTICE.md")
    shutil.copyfile(ROOT / "LICENCE", folder / "LICENCE")
    (folder / "THIRD-PARTY-LICENCES.md").write_text(licences(sdk, ffmpeg), encoding="utf-8")


def icon(resources):
    iconset = OUT / "icon.iconset"
    shutil.rmtree(iconset, ignore_errors=True)
    iconset.mkdir()
    for name, pixels in ICON_SIZES.items():
        subprocess.run(["sips", "-z", str(pixels), str(pixels), str(ROOT / "port/launcher/icon.png"),
                        "--out", str(iconset / f"icon_{name}.png")], check=True, capture_output=True)
    subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(resources / "icon.icns")], check=True)
    shutil.rmtree(iconset)


def bundle_version(name):
    found = re.match(r"v?(\d+(?:\.\d+)*)", name)
    return found.group(1) if found else "0"


def app_info(name):
    return {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": "Dark Chronicle",
        "CFBundleExecutable": "DarkChronicle",
        "CFBundleIconFile": "icon",
        "CFBundleIdentifier": IDENTIFIER,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "Dark Chronicle",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": bundle_version(name),
        "CFBundleVersion": bundle_version(name),
        "LSApplicationCategoryType": "public.app-category.role-playing-games",
        "LSMinimumSystemVersion": MACOS_MINIMUM,
        "NSHighResolutionCapable": True,
    }


def macos(name):
    subprocess.run([sys.executable, "-I", str(ROOT / "tools/port.py"), "--movies"], check=True)
    folder = PACKAGE / f"dark-chronicle-{name}-macos-arm64"
    folder.mkdir(parents=True)
    shutil.copyfile(ROOT / "port/package/README.md", folder / "README.md")
    notices(folder, SDK, shared_ffmpeg())
    contents = folder / "Dark Chronicle.app" / "Contents"
    executable = contents / "MacOS" / "DarkChronicle"
    executable.parent.mkdir(parents=True)
    shutil.copyfile(OUT / "darkchronicle", executable)
    executable.chmod(0o755)
    subprocess.run(["strip", str(executable)], check=True)
    copy_libraries(executable, contents / "Frameworks")
    (contents / "Resources").mkdir()
    icon(contents / "Resources")
    (contents / "Info.plist").write_bytes(plistlib.dumps(app_info(name)))
    for library in sorted((contents / "Frameworks").iterdir()):
        subprocess.run(["codesign", "--force", "--sign", "-", "--timestamp=none", str(library)], check=True)
    subprocess.run(["codesign", "--force", "--sign", "-", "--timestamp=none", str(contents.parent)], check=True)
    archive = PACKAGE / f"{folder.name}.zip"
    subprocess.run(["ditto", "-c", "-k", "--norsrc", "--noextattr", "--keepParent", str(folder), str(archive)], check=True)
    return archive


def linux(name):
    subprocess.run([sys.executable, "-I", str(ROOT / "tools/port.py"), "--movies"], check=True)
    folder = PACKAGE / f"dark-chronicle-{name}-linux-x64"
    folder.mkdir(parents=True)
    shutil.copyfile(ROOT / "port/package/README.md", folder / "README.md")
    notices(folder, SDK, shared_ffmpeg())
    executable = folder / "darkchronicle"
    shutil.copyfile(OUT / "darkchronicle", executable)
    executable.chmod(0o755)
    subprocess.run(["strip", str(executable)], check=True)
    copy_libraries(executable, folder / "lib")
    for library in (folder / "lib").iterdir():
        subprocess.run(["strip", "--strip-unneeded", str(library)], check=True)
    archive = PACKAGE / f"{folder.name}.tar.gz"
    subprocess.run(["tar", "--owner=0", "--group=0", "--numeric-owner", "-czf", str(archive), "-C", str(PACKAGE),
                    folder.name], check=True)
    return archive


def ios(name):
    subprocess.run([sys.executable, "-I", str(ROOT / "tools/port.py"), "--ios", "device"], check=True)
    payload = PACKAGE / "Payload"
    app = payload / "DarkChronicle.app"
    shutil.copytree(OUT / "ios-device" / "DarkChronicle.app", app)
    subprocess.run(["xcrun", "strip", str(app / "DarkChronicle")], check=True)
    info = plistlib.loads((app / "Info.plist").read_bytes())
    info["CFBundleShortVersionString"] = info["CFBundleVersion"] = bundle_version(name)
    (app / "Info.plist").write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_BINARY))
    notices(app, IOS_SDK, static_ffmpeg("ios_sdk.py"))
    archive = PACKAGE / f"dark-chronicle-{name}-ios.ipa"
    subprocess.run(["ditto", "-c", "-k", "--norsrc", "--noextattr", "--keepParent", str(payload), str(archive)], check=True)
    shutil.rmtree(payload)
    return archive


def android_home():
    spec = importlib.util.spec_from_file_location("android_sdk", Path(__file__).with_name("android_sdk.py"))
    android_sdk = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(android_sdk)
    return android_sdk.sdk_root(), android_sdk.toolchain()


def newest(folder):
    return max(folder.iterdir(), key=lambda path: [int(part) for part in re.findall(r"\d+", path.name)])


def version_code(name):
    parts = [int(part) for part in bundle_version(name).split(".")][:3]
    parts += [0] * (3 - len(parts))
    return max(1, parts[0] * 1000000 + parts[1] * 1000 + parts[2])


def android(name):
    subprocess.run([sys.executable, "-I", str(ROOT / "tools/port.py"), "--android"], check=True)
    home, toolchain = android_home()
    tools = newest(home / "build-tools")
    platform = str(newest(home / "platforms") / "android.jar")
    work = OUT / "apk"
    shutil.rmtree(work, ignore_errors=True)
    sources = sorted([*(ANDROID_SDK / "java").rglob("*.java"), *(ANDROID / "java").rglob("*.java")])
    subprocess.run(["javac", "--release", "17", "-nowarn", "-Xlint:none", "-classpath", platform, "-d", str(work / "classes"),
                    *map(str, sources)], check=True)
    subprocess.run([str(tools / "d8"), "--release", "--min-api", str(ANDROID_MINIMUM), "--lib", platform, "--output",
                    str(work), *map(str, sorted((work / "classes").rglob("*.class")))], check=True)
    icons = work / "res" / "mipmap-xxxhdpi"
    icons.mkdir(parents=True)
    shutil.copyfile(ROOT / "port/launcher/icon.png", icons / "icon.png")
    subprocess.run([str(tools / "aapt2"), "compile", "--dir", str(work / "res"), "-o", str(work / "res.zip")], check=True)
    assets = work / "assets"
    assets.mkdir()
    notices(assets, ANDROID_SDK, static_ffmpeg("android_sdk.py"))
    packed = work / "packed.apk"
    subprocess.run([str(tools / "aapt2"), "link", "-o", str(packed), "-I", platform, "--manifest",
                    str(ANDROID / "AndroidManifest.xml"), "--min-sdk-version", str(ANDROID_MINIMUM),
                    "--target-sdk-version", str(ANDROID_TARGET), "--version-code", str(version_code(name)),
                    "--version-name", bundle_version(name), "-A", str(assets), str(work / "res.zip")], check=True)
    shutil.copyfile(OUT / "android" / "libmain.so", work / "libmain.so")
    subprocess.run([str(toolchain / "bin" / "llvm-strip"), "--strip-unneeded", str(work / "libmain.so")], check=True)
    with zipfile.ZipFile(packed, "a") as apk:
        apk.write(work / "classes.dex", "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
        for library in [ANDROID_SDK / "lib" / "libSDL3.so", work / "libmain.so"]:
            apk.write(library, f"lib/arm64-v8a/{library.name}", compress_type=zipfile.ZIP_STORED)
    aligned = work / "aligned.apk"
    subprocess.run([str(tools / "zipalign"), "-P", "16", "-f", "4", str(packed), str(aligned)], check=True)
    archive = PACKAGE / f"dark-chronicle-{name}-android-arm64.apk"
    subprocess.run([str(tools / "apksigner"), "sign", "--ks", str(KEY["store"]), "--ks-key-alias", KEY["alias"],
                    "--ks-pass", f"pass:{KEY['password']}", "--out", str(archive), str(aligned)], check=True)
    archive.with_name(archive.name + ".idsig").unlink(missing_ok=True)
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", default=None, help="the release's name, git describe's by default")
    parser.add_argument("--ios", action="store_true", help="pack an unsigned .ipa for iPhone and iPad")
    parser.add_argument("--android", action="store_true", help="pack an arm64 APK")
    options = parser.parse_args()
    if options.ios and not (IOS_SDK / "lib/libSDL3.a").exists():
        sys.exit("iOS SDL3 and FFmpeg missing: run python3 -I tools/ios_sdk.py")
    if options.android and not (ANDROID_SDK / "lib/libSDL3.so").exists():
        sys.exit("Android SDL3 and FFmpeg missing: run python3 -I tools/android_sdk.py")
    if not options.ios and not options.android and not (SDK / "include/SDL3/SDL.h").exists():
        sys.exit("SDL3 and FFmpeg missing: run python3 -I tools/port_sdk.py")
    name = options.version or version()
    shutil.rmtree(PACKAGE, ignore_errors=True)
    PACKAGE.mkdir(parents=True)
    archive = ios(name) if options.ios else android(name) if options.android else macos(name) if DARWIN else linux(name)
    print(f"packed {archive.relative_to(ROOT)} ({os.path.getsize(archive) >> 20} MB)")


if __name__ == "__main__":
    main()
