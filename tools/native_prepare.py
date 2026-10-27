"""Puts mods' engine files into the native port's engine copy, and strips the functions port/src supplies.

    python3 -I tools/native_prepare.py ENGINE COPY PORT

ENGINE is the engine copy dc2tk prepared, whose build/mod-inputs/manifest.json lists the mods' files.
"""

import argparse
import importlib.util
import json
from pathlib import Path


def prepare(engine, copy, port, manifest):
    spec = importlib.util.spec_from_file_location("port_override", engine / "tools/kazarov/scripts/build/port_override.py")
    parser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(parser)
    supplied = {}
    for source in sorted((port / "src").glob("*.cpp")):
        names = parser.tagged_names(source.read_text())
        if names:
            supplied[source.name] = names
    for entry in manifest:
        text = (engine / entry["path"]).read_text()
        flat, _, _ = parser.flatten(parser.mask(text))
        names = []
        for tag in parser.TAG.finditer(flat):
            names.append(parser.Definition(text[tag.end():], flat[tag.end():]).name)
        conflict = set(names) & set(supplied.get(entry["unit"], ()))
        if conflict:
            raise ValueError(f"mod {entry['module']} overrides functions supplied by the port: {', '.join(sorted(conflict))}")
    spec = importlib.util.spec_from_file_location("engine_mods", Path(__file__).with_name("engine_mods.py"))
    mods = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mods)
    groups = {}
    for entry in manifest:
        unit = entry["unit"]
        text = (engine / entry["path"]).read_text()
        if unit.endswith((".hpp", ".h")):
            mods.write(copy / "ps2/include" / unit, text)
        else:
            groups.setdefault(unit, []).append({**entry, "text": text})
    for unit, entries in groups.items():
        mods.transform_unit(parser, copy, copy / "ps2/src" / unit, entries, defined=("PORT", "PAL"))
    for unit, names in supplied.items():
        target = copy / "ps2/src" / unit
        if target.exists():
            text = parser.strip(target.read_text(), names, defined=("PORT", "PAL"), long_is_64=True)
            if target.read_text() != text:
                target.write_text(text)


def main():
    arguments = argparse.ArgumentParser()
    arguments.add_argument("engine", type=Path)
    arguments.add_argument("copy", type=Path)
    arguments.add_argument("port", type=Path)
    options = arguments.parse_args()
    manifest = options.engine / "build/mod-inputs/manifest.json"
    try:
        prepare(options.engine, options.copy, options.port, json.loads(manifest.read_text()) if manifest.exists() else [])
    except Exception as error:
        arguments.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
