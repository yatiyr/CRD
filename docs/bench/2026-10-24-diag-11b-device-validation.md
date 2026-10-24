# DIAG.11b — device validation's cost on the authored rendering workload

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md#slice-diag.11b); current rules: [AGENTS](../../AGENTS.md).

**Date** 2026-10-24 · **Host** i9-14900K (32 logical processors), 64 GiB, RTX 4070 Ti SUPER, Windows 11 Pro 10.0.26200
and WSL2 (Ubuntu 24.04, Mesa lavapipe with the pinned validation layer) · **Harness** `tests/support/diag-overhead`,
driver [`scripts/diag-overhead.py`](../../scripts/diag-overhead.py) · **Session**
[2026-10-24 device validation](../sessions/2026-10-24-diag-11b-device-validation-cost.md)

## What is measured

ADR-0133 accounts device validation apart from the modes' budgets, so its cost is reported, never judged. One
binary, two or three arms: the `render` workload's off mode (no diagnostic host) without and with `--validation`, which
creates the device with that validation under a message capture. Each validated process must find its modes active
and report no error, warning or lost message, or its arm is an instrument failure. Interleaved processes, pinned to
mask `5555`, 200 timed frames after 20 warmup; the cost is the median of per-repetition ratios with a bootstrap 95%
interval, for the frame time and for the CPU per frame. The host was idle (`build-bench.py host`).

`python scripts/diag-overhead.py run --exe <tree>/tests/diag-overhead/crd-diag-overhead[.exe] --workloads render
--arms off,validation-core[,validation-sync|,validation-gpu] --processes <n> --iterations 200 --warmup 20 --affinity
5555 --scratch <dir> --label <lane> --output <board.json>` (Linux through `scripts/wsl-lane.sh run linux-gcc-shipping`,
which exports the pinned layer).

## Results

| Lane, device (binary SHA-256 prefix), processes per arm | Arm | Frame p50 / p99 µs | Frame cost (95%) | CPU per frame | CPU cost (95%) |
| --- | --- | ---: | --- | ---: | --- |
| win-shipping, D3D12 hardware adapter (`27921b0191a0`), 24 | off | 361 / 639 | reference | 0.68 Mcycles | reference |
| | core (debug layer) | 447 / 814 | +22.7% (+21.5 .. +24.5) | 0.95 Mcycles | +40.0% (+37.6 .. +40.8) |
| win-shipping, D3D12 hardware adapter (`502af13693de`), 24, later the same day | off | 365 / 653 | reference | 0.69 Mcycles | reference |
| | core (debug layer) | 442 / 782 | +19.2% (+16.0 .. +26.4) | 0.95 Mcycles | +36.1% (+32.8 .. +43.9) |
| | GPU-based validation | 607 / 1,014 | +66.1% (+63.3 .. +71.0) | 2.27 Mcycles | +227.7% (+220.2 .. +233.4) |
| linux-gcc-shipping, lavapipe (`00c9fbfe5ff0`), 12 | off | 2,823 / 3,272 | reference | 10.78 ms | reference |
| | core | 3,045 / 3,566 | +7.5% (+6.7 .. +9.4) | 10.99 ms | +2.1% (+1.5 .. +3.7) |
| | synchronization | 3,150 / 3,670 | +11.6% (+10.3 .. +13.4) | 11.14 ms | +2.9% (+2.7 .. +4.0) |

Resident growth over the run: 107 MiB off and 118 MiB core on D3D12, then 120, 135 and 214 MiB with GPU-based
validation; 251, 289 and 289 MiB on lavapipe.

- The D3D12 debug layer validates every call on the submitting thread, so its CPU cost (+40%) is larger than the frame
  cost (+23%), part of which the device's own time hides. lavapipe rasterises on the process's threads, which dilutes
  the layer's fixed cost in the CPU figure (+2-3%); the frame figure shows it.
- D3D12 has no synchronization validation (the arm is refused there, `BackendHasNoEquivalent`).
- GPU-based validation (D3D12) runs every shader patched and checks resource states on the device: +66% frame time
  and +228% CPU per frame. Its first run crashed in the driver until the frame recorder stopped binding a null root
  signature ([session](../sessions/2026-10-24-diag-11b-gpu-based-validation.md)). The core arm of that binary reads
  +19.2% against the earlier +22.7%: the intervals overlap.
- Not measured: Vulkan's GPU-assisted validation. On lavapipe the layer reports its own instrumentation cost as
  warnings (`GPUAV-Compile-time-general-buffer`); it is DIAG.11b's next item.
