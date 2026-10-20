import importlib.util
from pathlib import Path
import shutil
import tempfile


def check():
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("native_prepare", root / "tools/native_prepare.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory() as directory:
        scratch = Path(directory)
        engine = scratch / "engine"
        copy = scratch / "copy"
        port = scratch / "port"
        parser = engine / "tools/kazarov/scripts/build/port_override.py"
        parser.parent.mkdir(parents=True)
        shutil.copyfile(root / "engine/tools/kazarov/scripts/build/port_override.py", parser)
        (copy / "ps2/src").mkdir(parents=True)
        (port / "src").mkdir(parents=True)
        (port / "src/test.cpp").write_text("PORT_OVERRIDE int Test() { return 2; }")
        target = copy / "ps2/src/test.cpp"
        target.write_text("int Test() { return 1; }\nint Keep() { return 3; }")
        module.prepare(engine, copy, port, [])
        assert "return 1" not in target.read_text()
        assert "return 3" in target.read_text()
        target.write_text("int Test() { return 1; }\nint Keep() { return 3; }")
        (engine / "input.cpp").write_text("PORT_OVERRIDE int Keep() { return 7; }")
        module.prepare(engine, copy, port, [{"module": "example", "unit": "test.cpp", "path": "input.cpp"}])
        assert "return 7" in target.read_text()
        assert "return 3" not in target.read_text()
        target.write_text("int Test() { return 1; }\nstatic int table[] = {3};")
        (engine / "input.cpp").write_text("PORT_OVERRIDE static int table[] = {7};")
        module.prepare(engine, copy, port, [{"module": "example", "unit": "test.cpp", "path": "input.cpp"}])
        assert "{7}" in target.read_text()
        assert "{3}" not in target.read_text()
        (engine / "input.cpp").write_text("PORT_OVERRIDE int Test() { return 9; }")
        try:
            module.prepare(engine, copy, port, [{"module": "example", "unit": "test.cpp", "path": "input.cpp"}])
        except ValueError as error:
            assert "example" in str(error) and "Test" in str(error)
        else:
            raise AssertionError("a mod replaced a function supplied by the port")
    print("native parser stripping and conflict checks pass")


if __name__ == "__main__":
    check()
