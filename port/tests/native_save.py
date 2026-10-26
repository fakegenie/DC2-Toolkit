import importlib.util
import json
import pathlib
import subprocess
import tempfile


def main():
    root = pathlib.Path(__file__).resolve().parents[2]
    out = root / "build/port"
    spec = importlib.util.spec_from_file_location("native_port", root / "tools/port.py")
    native = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(native)
    manifest = out / "objects.json"
    if manifest.exists():
        objects = [pathlib.Path(path) for path in json.loads(manifest.read_text())]
    else:
        objects = [out / "obj" / (source.stem + ".o") for source in sorted((out / "copy/ps2/src").glob("*.cpp"))]
        objects += [out / "obj" / ("upstream_" + pathlib.Path(name).stem + ".o") for name in native.UPSTREAM_ASM]
        objects += [out / "obj/kazarov_vu0.o"]
        objects += [out / "obj" / ("port_" + source.stem + ".o") for source in sorted((root / "port/src").glob("*.c*"))]
    memcard = [path for path in objects if path.stem in ["memcard", "engine_memcard"]]
    mainloop = [path for path in objects if path.stem in ["mainloop", "engine_mainloop"]]
    if len(memcard) != 1 or len(mainloop) != 1:
        raise SystemExit("Build the native port before running this integration test")
    command_path = memcard[0].with_suffix(".command.json")
    if not command_path.exists():
        raise SystemExit("Build the native port before running this integration test")
    command = json.loads(command_path.read_text())[0]
    if command[0] not in ["clang", "clang++"]:
        command.insert(0, "clang++")
    replaced = ["mainloop", "engine_mainloop", "port_main", "port_memory_card"]
    objects = [path for path in objects if path.stem not in replaced and not (path.parent.name == "port" and path.stem in ["main", "memory_card"])]
    with tempfile.TemporaryDirectory(prefix="dc2-native-save-") as temporary:
        temporary = pathlib.Path(temporary)
        command.remove("-MMD")
        index = command.index("-MF")
        del command[index:index + 2]
        sources = [index for index, value in enumerate(command) if pathlib.Path(value).name == "memcard.cpp"]
        assert len(sources) == 1
        command[sources[0]] = str(root / "port/tests/native_save.cpp")
        command.append(f'-DSYMBOL_PREFIX="{native.PREFIX}"')
        command[command.index("-o") + 1] = str(temporary / "test.o")
        subprocess.run(command, cwd=root, check=True)
        subprocess.run([native.OBJCOPY, f"--globalize-symbol={native.PREFIX}_ZL14ActiveSaveData", str(mainloop[0]), str(temporary / "mainloop.o")], check=True)
        cflags, libraries = native.sdk_flags(out / "sdk", "-DPORT_MOVIES" in command)
        libraries.append(f"-Wl,-rpath,{out / 'sdk/lib'}")
        subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", *([] if native.DARWIN else ["-fno-pie"]), "-Iport/include", *cflags, f"-I{out}/copy/ps2/include", f"-I{out}/copy/ps2/include/sce", "-c", "port/src/memory_card.c", "-o", str(temporary / "memory_card.o")], cwd=root, check=True)
        binary = temporary / "native_save"
        link = [f"-Wl,-alias,_{target},_{alias}" for alias, target in native.ALIASES.items()] if native.DARWIN else \
            ["-no-pie", *(f"-Wl,--defsym={alias}={target}" for alias, target in native.ALIASES.items())]
        subprocess.run(["clang++", "-o", str(binary), *[str(path) for path in objects], str(temporary / "test.o"), str(temporary / "mainloop.o"), str(temporary / "memory_card.o"), *link, *native.LINK_FLAGS, *libraries], cwd=root, check=True)
        saves = temporary / "saves"
        subprocess.run([str(binary), "save", str(saves)], cwd=root, check=True, timeout=15)
        image = (saves / "card0/BESCES-51190dkcl0/BESCES-51190dkcl0").read_bytes()
        assert len(image) == 0x659c0
        assert image[:8] == b"dc2Ver4\0"
        assert image[0x45] == 0
        assert int.from_bytes(image[0x2566c:0x25670], "little") == 0x25fb0 + 2 * 4096 - 0x2566c
        assert image[0x25fb0:0x25fb2] == b"\x34\x12"
        subprocess.run([str(binary), "load", str(saves)], cwd=root, check=True, timeout=15)
        subprocess.run([str(binary), "corrupt", str(saves)], cwd=root, check=True, timeout=15)
    print("native_save: passed")


if __name__ == "__main__":
    main()
