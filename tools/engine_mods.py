"""Apply engine mods: apply COPY MANIFEST; export a function: export COPY FUNCTION MOD."""

import argparse
import importlib.util
import json
import pathlib
import re
import sys


def read(path):
    return path.read_bytes().decode("utf-8", errors="surrogateescape")


def write(path, text):
    data = text.encode("utf-8", errors="surrogateescape")
    if not path.exists() or path.read_bytes() != data:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def load_parser(copy):
    path = copy / "tools/kazarov/scripts/build/port_override.py"
    spec = importlib.util.spec_from_file_location("dc2tk_port_override", path)
    parser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(parser)
    return parser


def renamed(parser, text, spelled):
    if not re.fullmatch(r"(?:[A-Za-z_]\w*::)*[A-Za-z_]\w*", spelled):
        raise ValueError(f"original() does not support {spelled}")
    parts = spelled.split("::")
    if len(parts) > 1 and parts[-1] == parts[-2]:
        raise ValueError(f"original() does not support constructors: {spelled}")
    flat = parser.mask(text)
    pattern = r"\b" + r"\s*::\s*".join(parts) + r"(?=\s*\()"
    match = re.search(pattern, flat)
    if match is None:
        raise ValueError(f"cannot locate the definition name {spelled}")
    name = match[0] + "__original"
    return text[:match.start()] + name + text[match.end():]


def member_declaration(parser, copy, definition, text):
    if "::" not in definition.spelled:
        return
    owner, method = definition.spelled.rsplit("::", 1)
    if "::" in owner:
        raise ValueError(f"original() needs an unqualified class name: {owner}")
    flat = parser.mask(text)
    opening = flat.find("{")
    signature = text[:opening].strip()
    signature = renamed(parser, signature, definition.spelled)
    signature = re.sub(r"\b" + re.escape(owner) + r"\s*::\s*", "", signature, count=1)
    signature = re.sub(r"\b(?:PORT_OVERRIDE|inline|virtual)\s+", "", signature)
    matches = []
    for path in sorted((copy / "ps2/include").rglob("*")):
        if path.suffix not in (".h", ".hpp"):
            continue
        header = read(path)
        masked = parser.mask(header)
        for match in re.finditer(r"\b(?:class|struct)\s+" + re.escape(owner) + r"\b[^;{]*\{", masked):
            end = parser.closing(masked, match.end() - 1)
            body = masked[match.end():end]
            static = False
            wanted = definition.name[len(owner) + 2:]
            for candidate in re.finditer(r"\b" + re.escape(method) + r"\s*\(", body):
                start = max(body.rfind(";", 0, candidate.start()), body.rfind("}", 0, candidate.start()), body.rfind("{", 0, candidate.start())) + 1
                try:
                    declaration = parser.Definition(body[start:], body[start:], function_declarations=True)
                except parser.Error:
                    continue
                if declaration.name == wanted:
                    static = declaration.static
            declaration = ("static " if static else "") + signature + ";"
            matches.append((path, header[:end] + "\npublic:\n    " + declaration + "\n" + header[end:]))
    if len(matches) != 1:
        raise ValueError(f"{definition.name}: expected one class header for {owner}, found {len(matches)}")
    write(*matches[0])


def transform_unit(parser, copy, target, entries, defined=("PAL",)):
    source = read(target) if target.exists() else ""
    owners = {}
    originals = {}
    variables = {}
    appended = []
    for entry in entries:
        text = entry["text"]
        flat, _, _ = parser.flatten(parser.mask(text))
        patches = []
        for tag in parser.TAG.finditer(flat):
            definition = parser.Definition(text[tag.end():], flat[tag.end():])
            if definition.static and not definition.variable:
                raise ValueError(f"{entry['module']}: a static override replaces nothing")
            name = definition.name
            if name in owners:
                raise ValueError(f"{name} is given two ways, by mod {owners[name]} and mod {entry['module']}")
            owners[name] = entry["module"]
            body = text[tag.end():tag.end() + definition.length]
            if definition.variable:
                variables[name] = body
                patches.append((tag.start(), tag.end() + definition.length, ""))
            calls_original = re.search(r"\boriginal\s*\(", parser.mask(body)) is not None
            if calls_original:
                if definition.variable:
                    raise ValueError(f"{entry['module']}: original() needs a function")
                originals[name] = definition
                replacement = "\n#define original " + definition.spelled.split("::")[-1] + "__original\n"
                replacement += text[tag.start():tag.end() + definition.length]
                replacement += "\n#undef original\n"
                patches.append((tag.start(), tag.end() + definition.length, replacement))
        for start, end, replacement in reversed(patches):
            text = text[:start] + replacement + text[end:]
        appended.append(text)
    prototypes = []
    if variables:
        declared = set()
        for entry in entries:
            text = entry["text"]
            candidates = re.findall(r"\b[A-Za-z_]\w*(?=\s*\()", parser.mask(text))
            for start, definition in parser.definitions(text, candidates, private_functions=True):
                if definition.variable or definition.declaration or "::" in definition.spelled:
                    continue
                if definition.name in owners or definition.name in declared:
                    continue
                declared.add(definition.name)
                flat, _, _ = parser.flatten(parser.mask(text[start:]))
                declaration = parser.Definition(text[start:], flat).replacement
                prototypes.append(declaration)
    found = parser.overridden(source, owners, defined=defined, private_functions=True, private_variables=True)
    seen = set()
    output = []
    last = 0
    for start, definition in found:
        seen.add(definition.name)
        end = start + definition.length
        replacement = variables.get(definition.name, definition.replacement)
        if definition.name in variables and prototypes:
            replacement = "\n".join(prototypes) + "\n" + replacement
            prototypes = []
        if definition.name in originals and not definition.declaration:
            replacement = definition.replacement + "\n" + renamed(parser, source[start:end], definition.spelled)
            member_declaration(parser, copy, definition, source[start:end])
        output.append(source[last:start] + replacement)
        last = end
    missing = set(owners) - seen
    if missing:
        raise ValueError(f"{target}: override has no definition: {', '.join(sorted(missing))}")
    output.append(source[last:])
    output.append("\n#define PORT_OVERRIDE\n")
    output.extend(appended)
    write(target, "\n".join(output))


def add_units(copy, units):
    if not units:
        return
    path = copy / "ps2/CMakeLists.txt"
    text = read(path)
    anchor = 'if(MISSING_SOURCES)'
    if text.count(anchor) != 1:
        raise ValueError(f"{path}: cannot locate the game object list")
    rows = []
    for unit in units:
        name = pathlib.PurePosixPath(unit).stem
        obj = "${BUILD_DIR}/obj/" + unit + ".o"
        rows.extend([f"set(obj {obj})", "list(APPEND CPP_OBJS ${obj})", f"add_cpp_object(${{obj}} ps2/src/{unit} {name})"])
    text = text.replace(anchor, "\n".join(rows) + "\n" + anchor, 1)
    anchor = 'list(APPEND LINK_ORDER ${DATA_OBJS})'
    if text.count(anchor) != 1:
        raise ValueError(f"{path}: cannot locate the link object list")
    rows = []
    for unit in units:
        rows.append("list(APPEND LINK_ORDER ${BUILD_DIR}/obj/" + unit + ".o)")
    write(path, text.replace(anchor, "\n".join(rows) + "\n" + anchor, 1))
    path = copy / "ps2/config/pal/SCES_511.90.lcf"
    text = read(path)
    for section in (".text", ".vutext", ".data", ".vudata", ".rodata", ".init", ".ctor", ".vtables", ".sdata", ".sbss", ".bss"):
        anchor = "*(" + section + ")"
        if anchor not in text:
            continue
        rows = []
        for unit in units:
            rows.append(unit + ".o (" + section + ")")
        text = text.replace(anchor, "\n        ".join(rows) + "\n        " + anchor, 1)
    write(path, text)


def filter_profiles(data, units, parent=""):
    if isinstance(data, list):
        kept = []
        for row in data:
            if isinstance(row, dict):
                name = row.get("translation_unit", row.get("name", "") if parent == "translation_units" else "")
                if pathlib.PurePosixPath(name).stem in units:
                    continue
            kept.append(filter_profiles(row, units, parent))
        return kept
    if isinstance(data, dict):
        filtered = {}
        for key, value in data.items():
            filtered[key] = filter_profiles(value, units, key)
        return filtered
    return data


def remove_profiles(copy, units):
    path = copy / "scripts/build/satansfiddle.json"
    if not path.exists():
        return
    data = filter_profiles(json.loads(read(path)), units)
    write(path, json.dumps(data, indent=2, ensure_ascii=False) + "\n")


def apply(copy, manifest):
    parser = load_parser(copy)
    entries = json.loads(read(manifest))
    groups = {}
    headers = {}
    for entry in entries:
        unit = entry["unit"]
        relative = pathlib.PurePosixPath(unit)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"{entry['module']}: invalid engine path {unit}")
        source = pathlib.Path(entry.get("path", entry.get("source", "")))
        if not source.is_absolute():
            source = copy / source
        entry = dict(entry)
        entry["text"] = read(source)
        if relative.suffix in (".h", ".hpp"):
            if unit.startswith("include/"):
                relative = pathlib.PurePosixPath(unit[8:])
            target = copy / "ps2/include" / relative
            if not target.is_file():
                raise ValueError(f"{entry['module']}: engine header does not exist: {unit}")
            if target in headers:
                raise ValueError(f"{unit} is given two ways, by mod {headers[target]} and mod {entry['module']}")
            headers[target] = entry["module"]
            write(target, entry["text"])
        elif relative.suffix == ".cpp" and len(relative.parts) == 1:
            groups.setdefault(unit, []).append(entry)
        else:
            raise ValueError(f"{entry['module']}: unsupported engine file: {unit}")
    new_units = []
    for unit, unit_entries in groups.items():
        target = copy / "ps2/src" / unit
        if not target.exists():
            new_units.append(unit)
        transform_unit(parser, copy, target, unit_entries)
    add_units(copy, new_units)
    units = []
    for unit in groups:
        units.append(pathlib.PurePosixPath(unit).stem)
    if headers:
        units = []
        for path in sorted((copy / "ps2/src").glob("*.cpp")):
            units.append(path.stem)
    remove_profiles(copy, units)
    write(copy / "edited-units.txt", "".join(unit + "\n" for unit in units))


def extract(copy, function):
    parser = load_parser(copy)
    matches = []
    for path in sorted((copy / "ps2/src").glob("*.cpp")):
        source = read(path)
        for start, definition in parser.definitions(source, (function,), private_variables=False, private_functions=True):
            if not definition.variable and not definition.declaration:
                text = source[start:start + definition.length]
                if definition.static:
                    text = re.sub(r"\bstatic\s+", "", text, count=1)
                matches.append((path.name, definition.name, text))
    if not matches:
        raise ValueError(f"engine function has no definition: {function}")
    if len(matches) > 1:
        names = []
        for unit, name, text in matches:
            names.append(f"{unit}: {name}")
        raise ValueError(f"engine function is ambiguous: {function}: " + ", ".join(names))
    unit, name, text = matches[0]
    return {"unit": unit, "text": "PORT_OVERRIDE " + text + "\n"}


def export(copy, function, mod):
    result = extract(copy, function)
    path = mod / "engine" / result["unit"]
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with path.open("xb") as output:
            output.write(result["text"].encode("utf-8", errors="surrogateescape"))
    except FileExistsError:
        raise ValueError(f"engine override destination already exists: {path}") from None
    return path


def main():
    arguments = argparse.ArgumentParser(description=__doc__)
    commands = arguments.add_subparsers(dest="command", required=True)
    command = commands.add_parser("apply")
    command.add_argument("copy", type=pathlib.Path)
    command.add_argument("manifest", type=pathlib.Path)
    command = commands.add_parser("extract")
    command.add_argument("copy", type=pathlib.Path)
    command.add_argument("function")
    command = commands.add_parser("export")
    command.add_argument("copy", type=pathlib.Path)
    command.add_argument("function")
    command.add_argument("mod", type=pathlib.Path)
    options = arguments.parse_args()
    try:
        if options.command == "apply":
            apply(options.copy, options.manifest)
        elif options.command == "extract":
            print(json.dumps(extract(options.copy, options.function), ensure_ascii=True))
        else:
            print(export(options.copy, options.function, options.mod))
    except Exception as error:
        arguments.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
