#!/usr/bin/env python3
"""Compiles port/shaders/*.vert and *.frag to SPIR-V and Metal source, written into
port/src/shaders.h. Needs glslangValidator and spirv-cross; the port's build reads the header.

    python3 -I tools/port_shaders.py
"""

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "port" / "shaders"
HEADER = ROOT / "port" / "src" / "shaders.h"
VARIANTS = {"draw.frag": ["SPREAD"]}


def compile_shader(source, directory, define):
    spirv = directory / (source.name + (define or "") + ".spv")
    flags = [f"-D{define}"] if define else []
    subprocess.run(["glslangValidator", "-V", "--quiet", *flags, str(source), "-o", str(spirv)], check=True)
    msl = subprocess.run(["spirv-cross", "--msl", "--msl-version", "20100", "--msl-decoration-binding", str(spirv)],
                         capture_output=True, text=True, check=True).stdout
    return spirv.read_bytes(), msl


def byte_array(name, data):
    rows = [", ".join(f"0x{byte:02x}" for byte in data[start:start + 16]) for start in range(0, len(data), 16)]
    return f"static const unsigned char {name}[] = {{\n    " + ",\n    ".join(rows) + "\n};\n"


def main():
    sources = sorted([*SHADERS.glob("*.vert"), *SHADERS.glob("*.frag")])
    if not sources:
        sys.exit("no shaders in port/shaders")
    parts = ["#pragma once\n"]
    with tempfile.TemporaryDirectory() as temporary:
        for source in sources:
            for define in [None, *VARIANTS.get(source.name, [])]:
                spirv, msl = compile_shader(source, Path(temporary), define)
                stem = f"{source.stem}_{define.lower()}" if define else source.stem
                name = f"{stem}_{source.suffix[1:]}"
                parts.append(byte_array(f"{name}_spirv", spirv))
                parts.append(byte_array(f"{name}_msl", msl.encode("utf-8") + b"\0"))
    HEADER.write_text("\n".join(parts), encoding="utf-8")
    print(f"wrote {HEADER.relative_to(ROOT)} from {len(sources)} shaders")


if __name__ == "__main__":
    main()
