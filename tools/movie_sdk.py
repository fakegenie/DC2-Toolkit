"""Run python3 -I tools/movie_sdk.py to build the local shared MPEG-2 decoder with Kazarov."""

import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VERSION = "8.0.1"
SHA256 = "05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41"
PREFIX = ROOT / "build" / "port" / "movie-sdk"


def main():
    PREFIX.mkdir(parents=True, exist_ok=True)
    image = os.environ.get("DC2TK_BUILD_IMAGE")
    if not image:
        result = subprocess.run(["bash", "tools/kazarov/scripts/kazarov", "image"],
                                cwd=ROOT / "engine", check=True, text=True, capture_output=True)
        image = result.stdout.strip().splitlines()[-1]
        if not image.startswith("kazarov-dev:"):
            raise SystemExit("Kazarov did not report its container image")
    with tempfile.TemporaryDirectory(prefix="dc2tk-movie-sdk-") as directory:
        folder = Path(directory)
        archive = folder / "ffmpeg.tar.xz"
        urllib.request.urlretrieve(f"https://ffmpeg.org/releases/ffmpeg-{VERSION}.tar.xz", archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
            raise SystemExit("FFmpeg archive checksum mismatch")
        with tarfile.open(archive) as source:
            source.extractall(folder, filter="data")
        commands = "\n".join([
            "set -e",
            "cd /source",
            "./configure --cc=clang --prefix=/sdk --disable-everything --disable-programs --disable-doc --disable-x86asm --disable-static --enable-shared --enable-decoder=mpeg2video --enable-parser=mpegvideo --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample --disable-network",
            "make -j8",
            "make install",
            "mkdir -p /sdk/share/licenses/ffmpeg",
            "cp COPYING.LGPLv2.1 LICENSE.md /sdk/share/licenses/ffmpeg/",
        ])
        subprocess.run([
            "docker", "run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}",
            "--entrypoint", "sh", "-v", f"{folder / ('ffmpeg-' + VERSION)}:/source",
            "-v", f"{PREFIX}:/sdk", image, "-c", commands,
        ], check=True)
    print(f"FFmpeg shared MPEG-2 decoder installed in {PREFIX}")


if __name__ == "__main__":
    main()
