"""Copy SDL3 and its runtime libraries from the existing Kazarov image into build/port/sdk."""

import pathlib
import subprocess


def main():
    root = pathlib.Path(__file__).resolve().parents[1]
    output = root / "build/port/sdk"
    result = subprocess.run(["bash", "tools/kazarov/scripts/kazarov", "image"],
                            cwd=root / "engine", check=True, text=True, capture_output=True)
    image = result.stdout.strip().splitlines()[-1]
    if not image.startswith("kazarov-dev:"):
        raise SystemExit("Kazarov did not report its container image")
    library = "/usr/lib/x86_64-linux-gnu/libSDL3.so"
    dependencies = subprocess.check_output(["docker", "run", "--rm", "--entrypoint", "ldd", image, library], text=True)
    paths = ["usr/include/SDL3", library.lstrip("/"), library.lstrip("/") + ".0"]
    system = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1"}
    for line in dependencies.splitlines():
        words = line.split()
        if len(words) > 2 and words[1] == "=>":
            path = pathlib.PurePosixPath(words[2])
            if path.is_absolute() and path.name not in system:
                paths.append(str(path).lstrip("/"))
    output.mkdir(parents=True, exist_ok=True)
    archive = subprocess.Popen(["docker", "run", "--rm", "--entrypoint", "tar", image, "-C", "/", "-chf", "-", *paths],
                               stdout=subprocess.PIPE)
    try:
        unpacked = subprocess.run(["tar", "-xf", "-", "-C", str(output)], stdin=archive.stdout)
    finally:
        archive.stdout.close()
    if archive.wait() or unpacked.returncode:
        raise SystemExit("cannot copy SDL3 from Kazarov")
    print(output)


if __name__ == "__main__":
    main()
