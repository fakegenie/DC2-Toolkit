# GS performance checks

The software renderer uses up to twelve workers, queues compatible primitives, and splits their rows by estimated pixel work. Each worker processes primitives in their original order. Texture, CLUT, framebuffer, and depth aliases force serial execution or a queue drain. Transfers, palette loads, readback, GIF submission completion, and `port_gs_sync()` drain pending work.

| Variable | Default | Effect |
| --- | --- | --- |
| `DC2TK_GS_THREADS` | Online CPUs, capped at `12` | Worker count, clamped to 1–12. CPU detection failure selects one worker. Only the configured worker count is created. |
| `DC2TK_GS_BATCH` | `1` | Set to `0` to render each primitive immediately. |
| `DC2TK_GS_BALANCE` | `1` | Set to `0` to split queued work into equal row ranges. |
| `DC2TK_GS_BAND_PIXELS` | `4096` | Minimum estimated work for parallel rasterization, clamped to at least 4096. |

The raw `port_gs_memory_*` functions access local memory directly. Call `port_gs_sync()` before inspecting memory after queued draws. Production transfers and RGBA readback already synchronize.

Run the boundary checks with `python3 -I port/tests/run.py`. The GS checks include 384 recorded scene hashes, framebuffer/depth/texture alias cases, queue capacity and dependencies, exhaustive fixed-factor add/subtract blending, and comparisons between the specialized offset blend path and the general rasterizer.

Run the capture checks with `python3 -I tools/porttests.py --self-test --gs-unity`. Replay a real capture with `python3 -I tools/porttests.py build/port/combat.trace --threads 4 --gs-unity`. The grouped build compiles GS memory, CLUT, and rasterization together at `-O3 -ffp-contract=off`.

Capture version 1 checks API return values and the final 4 MiB VRAM hash. It does not record intermediate STORE or RGBA output contents. Version 2 also verifies successful STORE output and visible RGBA row hashes. The replay reports the format version and number of intermediate output hashes checked.

Recorded combat measurements and validation logs are under `build/validation/gs-performance/`. `results.json` records flags, settings, the inclusive field range 934–1074, timing, and the final VRAM hash. Early passes ran alongside other gameplay processes; worker scaling trials pause those processes and repeat the comparison. GS replay times exclude capture I/O and do not establish full-game PAL speed.
