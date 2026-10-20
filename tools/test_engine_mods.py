"""Run synthetic engine mod checks: python3 -I tools/test_engine_mods.py."""

import importlib.util
import json
import pathlib
import shutil
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parent.parent


def load():
    spec = importlib.util.spec_from_file_location("engine_mods", ROOT / "tools/engine_mods.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fixture(copy):
    path = copy / "tools/kazarov/scripts/build/port_override.py"
    path.parent.mkdir(parents=True)
    shutil.copyfile(ROOT / "engine/tools/kazarov/scripts/build/port_override.py", path)
    (copy / "ps2/src").mkdir(parents=True)
    (copy / "ps2/include").mkdir(parents=True)
    (copy / "ps2/config/pal").mkdir(parents=True)
    (copy / "scripts/build").mkdir(parents=True)
    (copy / "ps2/CMakeLists.txt").write_text('if(MISSING_SOURCES)\nendif()\nlist(APPEND LINK_ORDER ${DATA_OBJS})\n')
    (copy / "ps2/config/pal/SCES_511.90.lcf").write_text('*(.text)\n*(.data)\n*(.bss)\n')
    (copy / "scripts/build/satansfiddle.json").write_text(json.dumps({"functions": [{"translation_unit": "sample.cpp"}, {"translation_unit": "other.cpp"}], "translation_units": [{"name": "sample.cpp"}, {"name": "other.cpp"}]}))


def run_apply(module, copy, rows):
    entries = []
    for index, row in enumerate(rows):
        path = copy / f"input-{index}"
        path.write_text(row[2])
        entries.append({"module": row[0], "unit": row[1], "path": str(path)})
    path = copy / "manifest.json"
    path.write_text(json.dumps(entries))
    module.apply(copy, path)


def compile_run(copy):
    compiler = shutil.which("clang++") or shutil.which("g++")
    if compiler is None:
        raise RuntimeError("clang++ or g++ is required for synthetic engine tests")
    result = subprocess.run([compiler, "-std=c++98", "-I", str(copy / "ps2/include"), str(copy / "ps2/src/sample.cpp"), "-o", str(copy / "sample")], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(copy / "sample")], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def originals_compile_and_run(module, copy):
    fixture(copy)
    header = 'class Counter { public: int Add(int value); int Add(double value) const; static int Twice(int value); };\n'
    source = '#include "counter.hpp"\nint Add(int value) { return value + 2; }\nint Counter::Add(int value) { return value + 3; }\nint Counter::Add(double value) const { return (int)value + 4; }\nint Counter::Twice(int value) { return value * 2; }\nint main() { Counter counter; const Counter other; return Add(1) == 6 && counter.Add(1) == 12 && other.Add(1.0) == 10 && Counter::Twice(2) == 5 ? 0 : 1; }\n'
    (copy / "ps2/include/counter.hpp").write_text(header)
    (copy / "ps2/src/sample.cpp").write_text(source)
    override = 'PORT_OVERRIDE int Add(int value) { return original(value) * 2; }\nPORT_OVERRIDE int Counter::Add(int value) { return original(value) * 3; }\nPORT_OVERRIDE int Counter::Add(double value) const { return original(value) * 2; }\nPORT_OVERRIDE int Counter::Twice(int value) { return original(value) + 1; }\n'
    run_apply(module, copy, [("money", "sample.cpp", override)])
    compile_run(copy)
    first = (copy / "ps2/src/sample.cpp").read_bytes()
    (copy / "ps2/include/counter.hpp").write_text(header)
    (copy / "ps2/src/sample.cpp").write_text(source)
    run_apply(module, copy, [("money", "sample.cpp", override)])
    assert (copy / "ps2/src/sample.cpp").read_bytes() == first
    profile = json.loads((copy / "scripts/build/satansfiddle.json").read_text())
    assert profile["functions"] == [{"translation_unit": "other.cpp"}]
    assert (copy / "edited-units.txt").read_text() == "sample\n"


def rejects_missing_and_clashing_overrides(module, copy):
    fixture(copy)
    (copy / "ps2/src/sample.cpp").write_text('int Add(int value) { return value; }\n')
    rows = [("one", "sample.cpp", 'PORT_OVERRIDE int Add(double value) { return 1; }\n')]
    try:
        run_apply(module, copy, rows)
    except ValueError as error:
        assert "no definition" in str(error)
    else:
        raise AssertionError("missing signature accepted")
    rows = [("one", "sample.cpp", 'PORT_OVERRIDE int Add(int value) { return 1; }\n'), ("two", "sample.cpp", 'PORT_OVERRIDE int Add(int value) { return 2; }\n')]
    try:
        run_apply(module, copy, rows)
    except ValueError as error:
        assert "one" in str(error) and "two" in str(error) and "Add(int)" in str(error)
    else:
        raise AssertionError("duplicate override accepted")


def headers_and_new_units(module, copy):
    fixture(copy)
    (copy / "ps2/include/counter.hpp").write_text('int Value();\n')
    (copy / "ps2/src/sample.cpp").write_text('int Value() { return 1; }\n')
    rows = [("one", "counter.hpp", 'int Value(int value);\n'), ("one", "extra.cpp", 'int Added() { return 7; }\n')]
    run_apply(module, copy, rows)
    assert (copy / "ps2/include/counter.hpp").read_text() == 'int Value(int value);\n'
    assert 'extra.cpp.o' in (copy / "ps2/CMakeLists.txt").read_text()
    assert 'extra.cpp.o (.bss)' in (copy / "ps2/config/pal/SCES_511.90.lcf").read_text()
    assert 'Added' in (copy / "ps2/src/extra.cpp").read_text()
    assert set((copy / "edited-units.txt").read_text().splitlines()) == {"sample", "extra"}
    rows = [("one", "counter.hpp", 'int Value();\n'), ("two", "counter.hpp", 'int Value();\n')]
    try:
        run_apply(module, copy, rows)
    except ValueError as error:
        assert "one" in str(error) and "two" in str(error)
    else:
        raise AssertionError("duplicate header accepted")


def exports_selected_overload_without_overwriting(module, copy):
    fixture(copy)
    source = 'int Add(int value) { return value + 1; }\nint Add(double value) { return (int)value + 2; }\n'
    (copy / "ps2/src/sample.cpp").write_text(source)
    mod = copy / "mods/money"
    try:
        module.export(copy, "Add", mod)
    except ValueError as error:
        assert "ambiguous" in str(error) and "Add(int)" in str(error) and "Add(double)" in str(error)
    else:
        raise AssertionError("ambiguous overload exported")
    assert not (mod / "engine/sample.cpp").exists()
    path = module.export(copy, "Add(int)", mod)
    assert path.read_text() == 'PORT_OVERRIDE int Add(int value) { return value + 1; }\n'
    original = path.read_bytes()
    try:
        module.export(copy, "Add(double)", mod)
    except ValueError as error:
        assert "already exists" in str(error)
    else:
        raise AssertionError("existing mod file overwritten")
    assert path.read_bytes() == original
    assert (copy / "ps2/src/sample.cpp").read_text() == source


def static_tables_and_nested_profiles(module, copy):
    fixture(copy)
    (copy / "ps2/src/sample.cpp").write_text('static int values[] = {1, 2};\nint main() { return values[0] == 3 && values[1] == 4 ? 0 : 1; }\n')
    path = copy / "scripts/build/satansfiddle.json"
    path.write_text(json.dumps({"floating_point": {"expression_overrides": [{"translation_unit": "sample.cpp", "name": "change"}, {"translation_unit": "other.cpp", "name": "keep"}]}, "translation_units": [{"name": "sample.cpp"}, {"name": "other.cpp"}]}))
    run_apply(module, copy, [("one", "sample.cpp", 'PORT_OVERRIDE static int values[] = {3, 4};\n')])
    compile_run(copy)
    data = json.loads(path.read_text())
    assert data["floating_point"]["expression_overrides"] == [{"translation_unit": "other.cpp", "name": "keep"}]
    assert data["translation_units"] == [{"name": "other.cpp"}]


def native_table_sees_new_native(module, copy):
    fixture(copy)
    source = 'struct RS_STACK { int value; };\nstruct RS_EXTFUNC_INFO { int (*function)(RS_STACK* stack); int id; };\nint Retail(RS_STACK* stack) { return stack->value; }\nstatic RS_EXTFUNC_INFO ext_func_info[] = {{Retail, 1}};\nint main() { RS_STACK stack = {7}; return ext_func_info[0].id == 900 && ext_func_info[0].function(&stack) == 10 ? 0 : 1; }\n'
    (copy / "ps2/src/sample.cpp").write_text(source)
    override = 'int ModNative(RS_STACK* stack) { return stack->value + 3; }\nPORT_OVERRIDE static RS_EXTFUNC_INFO ext_func_info[] = {{ModNative, 900}};\n'
    run_apply(module, copy, [("one", "sample.cpp", override)])
    compile_run(copy)


def exported_static_function_applies(module, copy):
    fixture(copy)
    source = 'static int Add(int value) { return value + 1; }\nint main() { return Add(1) == 2 ? 0 : 1; }\n'
    (copy / "ps2/src/sample.cpp").write_text(source)
    path = module.export(copy, "Add(int)", copy / "mods/one")
    assert path.read_text().startswith("PORT_OVERRIDE int Add")
    run_apply(module, copy, [("one", "sample.cpp", path.read_text())])
    compile_run(copy)


def main():
    module = load()
    for test in (originals_compile_and_run, rejects_missing_and_clashing_overrides, headers_and_new_units, exports_selected_overload_without_overwriting, static_tables_and_nested_profiles, native_table_sees_new_native, exported_static_function_applies):
        with tempfile.TemporaryDirectory() as directory:
            test(module, pathlib.Path(directory))
        print(test.__name__ + ": passed")


if __name__ == "__main__":
    main()
