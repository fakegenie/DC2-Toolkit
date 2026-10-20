"""Run python3 -I tools/imgui_sdk.py to fetch Dear ImGui for the port's debug build into build/port/imgui-sdk."""

import hashlib
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VERSION = "1.92.9b"
SHA256 = "21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99"
PREFIX = ROOT / "build" / "port" / "imgui-sdk"
FILES = ["imgui.cpp", "imgui.h", "imgui_internal.h", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
         "imgui_demo.cpp", "imconfig.h", "imstb_rectpack.h", "imstb_textedit.h", "imstb_truetype.h", "LICENSE.txt",
         "backends/imgui_impl_sdl3.cpp", "backends/imgui_impl_sdl3.h",
         "backends/imgui_impl_sdlgpu3.cpp", "backends/imgui_impl_sdlgpu3.h", "backends/imgui_impl_sdlgpu3_shaders.h"]


def main():
    with tempfile.TemporaryDirectory(prefix="dc2tk-imgui-sdk-") as directory:
        folder = Path(directory)
        archive = folder / "imgui.tar.gz"
        urllib.request.urlretrieve(f"https://github.com/ocornut/imgui/archive/refs/tags/v{VERSION}.tar.gz", archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
            raise SystemExit("Dear ImGui archive checksum mismatch")
        with tarfile.open(archive) as source:
            source.extractall(folder, filter="data")
        unpacked = folder / f"imgui-{VERSION}"
        shutil.rmtree(PREFIX, ignore_errors=True)
        for name in FILES:
            target = PREFIX / Path(name).name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(unpacked / name, target)
    print(f"Dear ImGui {VERSION} installed in {PREFIX}")


if __name__ == "__main__":
    main()
