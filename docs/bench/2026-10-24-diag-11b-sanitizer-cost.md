# DIAG.11b — the sanitizer modes' cost, per workload and per lane

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md#slice-diag.11b); current rules: [AGENTS](../../AGENTS.md).

**Date** 2026-10-24 · **Host** i9-14900K (32 logical processors), 64 GiB, Windows 11 Pro 10.0.26200 and WSL2
(Ubuntu 24.04) · **Compilers** MSVC 19.51 (VS 18), GCC 13.3, Clang 18.1 · **Harness** `tests/support/diag-overhead`,
driver [`scripts/diag-overhead.py`](../../scripts/diag-overhead.py) · **Hosted** complete run 39239067340 (head
`1e0574f1`, 2026-10-19), read with [`scripts/ci-evidence.py compare`](../../scripts/ci-evidence.py) · **Session**
[2026-10-24 sanitizer cost](../sessions/2026-10-24-diag-11b-sanitizer-cost.md)

## What is measured

A sanitizer is a separate binary, so its cost is the lane's binary against the same configuration without it, not a
budget. Two measures:

- **Per workload, this host.** The off arm of `crd-diag-overhead` (no diagnostic host) built by the sanitizer lane
  against the reference lane: 6 processes per lane, 100 timed frames after 10 warmup, not pinned; the ratio of the
  p50 frame time and of the CPU per frame, and both lanes' peak resident memory. At these ratios the two-binary
  layout floor (a few percent) does not matter. Command (one lane; Linux through `scripts/wsl-lane.sh`):
  `python scripts/diag-overhead.py run --exe <tree>/tests/diag-overhead/crd-diag-overhead[.exe] --workloads
  alloc,jobs,offline,io,render --arms off --processes 6 --iterations 100 --warmup 10 --scratch <dir> --label <lane>
  --output <lane>.json` (win-asan inside the MSVC environment).
- **Per lane, hosted.** `python scripts/ci-evidence.py compare --mode <evidence> --reference <evidence>` on two lanes'
  `evidence-<preset>` artifacts: each test's time on the mode lane over the reference lane's, over the tests that
  passed on both and took at least 0.05 s on the reference.

Pairs: win-asan against win-debug (MSVC ASan); linux-gcc-asan, which is ASan and UBSan, against linux-gcc-debug;
linux-clang-tsan against linux-gcc-debug, a different compiler, since no preset builds plain Clang Debug on Linux (a
same-compiler reference is [DIAG.11c](../ROADMAP.md#slice-diag.11c) qualification).

## Per workload

Frame time ratio (CPU per frame ratio); peak resident memory, mode / reference. After the two fixes below.

| Workload | win-asan / win-debug | linux-gcc-asan / linux-gcc-debug | linux-clang-tsan / linux-gcc-debug |
|---|---|---|---|
| alloc | 6.83x (6.75x); 17 / 8 MiB | 7.08x (7.09x); 113 / 5 MiB | 4.42x (4.41x); 26 / 5 MiB |
| jobs | 1.77x (1.77x); 24 / 16 MiB | 1.54x (1.56x); 132 / 8 MiB | 4.40x (4.40x); 44 / 8 MiB |
| offline | 3.06x (3.10x); 87 / 13 MiB | 6.95x (6.88x); 155 / 6 MiB | 6.74x (6.70x); 26 / 6 MiB |
| io | 3.53x (2.79x); 65 / 14 MiB | 3.62x (4.53x); 186 / 8 MiB | 8.86x (7.54x); 41 / 8 MiB |
| render | 3.88x (2.84x); 635 / 414 MiB | 1.61x (1.16x); 1232 / 291 MiB | 1.88x (1.34x); 704 / 291 MiB |

Reference p50 frames: win-debug 883, 583, 3700, 954 and 1167 µs; linux-gcc-debug 669, 730, 1776, 195 and 5959 µs.
Render runs on D3D12 hardware on Windows and on lavapipe under WSL2, a driver the sanitizers do not instrument, so
the Linux render ratios cover only the engine's share of the frame.

## Per lane, hosted

| Mode lane | Reference | Tests compared | Median ratio | Quartiles | Compared seconds | Lane test seconds |
|---|---|---:|---:|---|---|---|
| win-asan (ASan) | win-debug | 2111 | 2.19 | 1.83-3.01 | 6946 s / 1688 s | 7316 s / 1783 s |
| linux-gcc-asan (ASan, UBSan) | linux-gcc-debug | 845 | 2.72 | 2.34-3.70 | 8898 s / 3112 s | 9437 s / 3170 s |
| linux-clang-tsan (TSan) | linux-gcc-debug | 845 | 2.59 | 1.57-4.36 | 11769 s / 3112 s | 12184 s / 3170 s |

The compared tests carry 94% to 97% of each lane's test seconds. The TSan row is an upper bound: its test preset
runs four tests at a time where the others run one on the same runner image, besides the compiler difference. The run predates the two fixes; they change the
jobs and I/O tests, a small share of either lane.

## Losses and what they belong to

1. **linux-gcc-asan jobs, 24.3x and 4532 MiB, a defect, fixed.** Resident memory grew 41 MB a frame
   (`detect_stack_use_after_return=0` removed it; the quarantine and leak checking did not). A completed fiber's
   context is rebuilt for its next job, and its trampoline was entered with no fake stack, so ASan mapped a new one
   for every job and the one the fiber saved at completion was never restored or freed. The trampoline now restores
   it: jobs 1.54x, 132 MiB; io 4.77x to 3.62x, 311 to 186 MiB. MSVC has no fake stacks without
   `/fsanitize-address-use-after-return`, so win-asan never grew.
2. **linux-clang-tsan io, 955 MiB after 3600 frames, a defect, fixed.** Resident memory grew 0.26 MiB a frame
   without bound (a plain TSan program reading files the same way stays at 20 MiB). The trampoline, rebuilt for every
   job, never returned, so each job left its frame on the fiber's TSan shadow stack; every later stack trace on that
   fiber was deeper and unique, and TSan's stack depot grew with each file a job opened. The trampoline now pops its
   frame before a completed fiber leaves: io stays at 41 MiB at 50, 1200 and 3600 frames.
3. **linux-gcc-asan io grows to about 440 MiB, the mode's storage.** ASan's quarantine (256 MiB by default) holds the
   freed read buffers; with `quarantine_size_mb=0` the same run peaks at 124 MiB.
4. **linux-clang-tsan against a GCC reference.** The local TSan column and the hosted TSan row compare compilers as
   well as the instrument; DIAG.11c owns a same-compiler reference.
