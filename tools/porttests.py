"""Build and replay a GS capture: python3 -I tools/porttests.py TRACE --threads 1."""

import argparse
import os
import pathlib
import struct
import subprocess
import tempfile


def build(root, out, source, replay, unity=False):
    command = ["clang", "-std=c11", "-O3", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
               "-pthread", "-I" + str(root / "port/include")]
    if replay:
        command += ["-DGS_TRACE_REPLAY"]
    command += [str(root / "port/tests/gs_trace.c")]
    if source:
        command += [str(source)]
    command += [str(root / "port/src/gs.c")]
    if unity:
        grouped = out.parent / "gs-unity.c"
        grouped.write_text("".join('#include "' + str(root / "port/src" / name) + '"\n'
                                   for name in ["gs_memory.c", "gs_clut.c", "gs_raster.c"]))
        command += [str(grouped)]
    else:
        command += [str(root / "port/src" / name) for name in ["gs_memory.c", "gs_clut.c", "gs_raster.c"]]
    if not replay:
        command += ["-Wl,--wrap=" + name for name in ["port_gs_reset", "port_gif_submit", "port_gif_feed",
                    "port_gs_write_register", "port_gs_image_load", "port_gs_image_store", "port_gs_read_rgba"]]
    subprocess.run([*command, "-lm", "-o", str(out)], check=True)


def self_test(root, replay, unity):
    with tempfile.TemporaryDirectory(prefix="dc2-gs-trace-") as temporary:
        directory = pathlib.Path(temporary)
        source = directory / "fixture.c"
        source.write_text('''#include <assert.h>
#include "gs.h"
#include "port_graphics.h"
PortGraphicsState port_graphics;
int main(void) {
    port_gs_reset();
    uint64_t load[9] = {0};
    load[2] = (uint64_t)1 << 48;
    load[6] = 4 | (uint64_t)1 << 32;
    uint32_t pixels[4] = {0x12345678, 0xabcdef01, 0x76543210, 0xff00ff00};
    assert(port_gs_image_load(load, pixels, sizeof(pixels)) == 0);
    uint32_t rgba[4];
    port_graphics.fields = 1;
    assert(port_gs_read_rgba(0, 1, 0, 0, 0, 4, 1, rgba, 4) == 0);
    uint64_t store[13] = {0};
    store[4] = (uint64_t)1 << 16;
    store[8] = 4 | (uint64_t)1 << 32;
    store[12] = 1;
    uint32_t output[6];
    for (int i = 0; i < 6; ++i) output[i] = 0xa5a5a5a5;
    assert(port_gs_image_store(store, output, sizeof(output)) == 0);
    for (int i = 0; i < 4; ++i) assert(pixels[i] == output[i]);
    assert(output[4] == 0xa5a5a5a5 && output[5] == 0xa5a5a5a5);
    uint32_t padded[12];
    for (int i = 0; i < 12; ++i) padded[i] = 0x55555555;
    assert(port_gs_read_rgba(0, 1, 0, 0, 0, 4, 2, padded, 6) == 0);
    assert(padded[4] == 0x55555555 && padded[5] == 0x55555555);
    port_graphics.fields = 2;
    assert(port_gs_write_register(0x4c, (uint64_t)2 << 16) == 0);
    assert(port_gs_write_register(0x4e, (uint64_t)1 << 32) == 0);
    assert(port_gs_write_register(0x40, (uint64_t)127 << 16 | (uint64_t)127 << 48) == 0);
    assert(port_gs_write_register(0x46, 1) == 0);
    assert(port_gs_write_register(0, 6) == 0);
    assert(port_gs_write_register(1, 0x80403020) == 0);
    for (int i = 0; i < 12; ++i) {
        assert(port_gs_write_register(5, 0) == 0);
        assert(port_gs_write_register(5, (uint64_t)2048 | (uint64_t)2048 << 16) == 0);
    }
    assert(port_gs_read_rgba(0, 2, 0, 1, 1, 1, 1, rgba, 1) == 0);
    assert(rgba[0] == 0xff403020);
    assert(port_gs_write_register(0x1a, 1) == 0);
    uint64_t gif[4] = {1 | (uint64_t)1 << 60, 14, 1, 0x1a};
    assert(port_gif_submit(gif, sizeof(gif)) == 0);
    assert(port_gif_feed(gif, 7, 2) == 0);
    assert(port_gif_feed((unsigned char *)gif + 7, sizeof(gif) - 7, 2) == 0);
    return 0;
}
''')
        capture = directory / "capture"
        trace = directory / "scene.trace"
        build(root, capture, source, False, unity)
        subprocess.run([str(capture)], env={**os.environ, "DC2TK_GS_TRACE": str(trace)}, check=True)
        for threads in [1, 4, 12]:
            subprocess.run([str(replay), str(trace), str(threads)], check=True)
        data = trace.read_bytes()
        if data[8] != 2:
            raise RuntimeError("capture did not write version 2")
        legacy = bytearray(data[:16])
        legacy[8] = 1
        offset = 16
        output_records = []
        while offset < len(data):
            operation, size, field, result, reserved = struct.unpack_from("<IIQiI", data, offset)
            payload = data[offset + 24:offset + 24 + size]
            if operation in [6, 7]:
                if result == 0:
                    output_records.append(offset + 24 + size - 1)
                payload = payload[:-8]
            legacy += struct.pack("<IIQiI", operation, len(payload), field, result, reserved) + payload
            offset += 24 + size
        old_trace = directory / "legacy.trace"
        old_trace.write_bytes(legacy)
        subprocess.run([str(replay), str(old_trace), "1"], check=True)
        broken = directory / "broken.trace"
        variants = [b"invalid", data[:-1], data[:-32], data + b"extra",
                    data[:20] + struct.pack("<I", 0xffffffff) + data[24:]]
        damaged = bytearray(data)
        damaged[-1] ^= 1
        variants.append(damaged)
        for contents in variants:
            broken.write_bytes(contents)
            result = subprocess.run([str(replay), str(broken), "1"], capture_output=True)
            if result.returncode == 0:
                raise RuntimeError("invalid capture was accepted")
        if len(output_records) < 3:
            raise RuntimeError("fixture did not exercise store and padded display outputs")
        for offset in output_records:
            damaged = bytearray(data)
            damaged[offset] ^= 1
            broken.write_bytes(damaged)
            result = subprocess.run([str(replay), str(broken), "1"], capture_output=True)
            if result.returncode == 0 or b"output hash differs" not in result.stderr:
                raise RuntimeError("incorrect readback output hash was accepted")
        for limit in ["16", "8589934593", "invalid"]:
            result = subprocess.run([str(replay), str(trace), "1"], capture_output=True,
                                    env={**os.environ, "DC2TK_GS_TRACE_MAX_BYTES": limit})
            if result.returncode == 0:
                raise RuntimeError("invalid capture byte limit was accepted")
        result = subprocess.run([str(capture)], capture_output=True,
                                env={**os.environ, "DC2TK_GS_TRACE": str(directory / "limited.trace"),
                                     "DC2TK_GS_TRACE_MAX_BYTES": "64"})
        if result.returncode == 0:
            raise RuntimeError("capture exceeded its configured byte limit")
        print("GS trace capture/replay and malformed input checks passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", nargs="?", type=pathlib.Path)
    parser.add_argument("--threads", type=int, choices=range(1, 13), default=1)
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("build/port/gs-trace"))
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--gs-unity", action="store_true")
    parser.add_argument("--max-gib", type=int, choices=range(1, 9))
    options = parser.parse_args()
    if options.max_gib:
        os.environ["DC2TK_GS_TRACE_MAX_BYTES"] = str(options.max_gib * 1024 ** 3)
    root = pathlib.Path(__file__).resolve().parents[1]
    out = options.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    replay = out / "gs-replay"
    build(root, replay, None, True, options.gs_unity)
    if options.self_test:
        self_test(root, replay, options.gs_unity)
    if options.trace:
        subprocess.run([str(replay), str(options.trace.resolve()), str(options.threads)], check=True)
    elif not options.self_test:
        parser.error("provide a capture or --self-test")


if __name__ == "__main__":
    main()
