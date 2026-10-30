# 2026-10-30 — SIMD-DISPATCH row-close board: one-thread GEMM against the pre-row engine and its peers, every level

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch); session:
> [2026-10-30](../sessions/2026-10-30-simd-dispatch-gemm-board.md); acceptance: the user's order of
> [2026-10-30](../sessions/2026-10-30-simd-dispatch-peer-pins-board-acceptance.md#user-decisions-recorded).

- **Machine:** Intel i9-14900K, WSL2, every run pinned to CPU 4 (`taskset -c 4`), the peers on one thread
  (`openblas_set_num_threads(1)`, `Eigen::setNbThreads(1)`, `OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1`).
  `scripts/build-bench.py host` before and after: other processes 2.0 to 2.9 cores (the busiest Studio One, 0.8),
  Windows memory 9.3 to 14.5 of 63.7 GiB available, below the 25% mark the check flags, held by the user's own
  applications, which this session did not close.
- **HEAD:** `1b0039bd` (before this batch's change), the linux-gcc-release lane tree with
  `CRD_BUILD_HESAP_VS_REFERENCE=ON` (GCC 13.3, `-O3`, program LTO; kernel units per level without LTO, ADR-0134 §6),
  `bench_hesap_gemm_vs_reference --one-thread` as `scripts/run_bench_gemm.sh` runs it at detected (avx2), sse4.2 (the
  dense family's sse2 unit), sse2 and portable.
- **Pre-row:** `2f2aed72`, exported with `git archive` into `~/prerow-tree-2f2aed72` and configured with its own
  linux-gcc-release preset (tests off, `CRD_BUILD_HESAP_VS_REFERENCE=ON`), its archives linked from HEAD's verified
  copies (the same pinned SHA-256): a whole configured tree rather than the fft board's hand compile, since hesap-dense
  is a compiled library. Its dense code builds at avx2 only (`CRD_SIMD_TARGET=2`), so the lower levels have no pre-row
  cell.
- **Probe (one program on both engines):** the bench's fill, four warm-ups and best of three 0.6 s loops over
  `gemm_parallel(1, ...)` (with `crd::jobs::frame_reset()`, which pre-row's loop called), at the bench's seven sizes,
  compiled and linked with each tree's own commands for `bench_hesap_gemm_vs_reference` (its object in place of the
  bench's). It lives in the board's scratch directory, not the tree: a routed source would need a target.
- **Peers:** Eigen 3.4.0 (its peer library at the host's ISA, AVX2 and FMA) and OpenBLAS 0.3.27 built for the host.
  They dispatch on their own CPUID and stay at AVX2 under a forced Cerid level (ADR-0134 §8), so their ratios mean
  something at avx2 only.
- **Board:** five passes of 11 minutes; each runs the probe on pre-row, then on HEAD, then HEAD's bench at the four
  levels. A cell is GFLOPS, the median of the five passes [the lowest - the highest]; the range is the run-to-run
  noise (about 5% either way here).

## avx2 against the pre-row engine (the probe)

f32:

| n | pre-row | HEAD | HEAD/pre-row | passes |
|---|---|---|---|---|
| 64 | 111.1 [105.7-113.7] | 146.3 [140.7-154.7] | 1.317 | 5 |
| 256 | 135.1 [128.4-137.3] | 174.2 [160.0-180.9] | 1.289 | 5 |
| 512 | 141.4 [130.8-144.8] | 171.6 [163.1-178.8] | 1.214 | 5 |
| 600 | 141.0 [134.1-145.0] | 168.4 [164.6-172.7] | 1.194 | 5 |
| 768 | 142.5 [139.2-146.3] | 165.1 [146.9-168.9] | 1.159 | 5 |
| 1000 | 143.6 [139.4-147.4] | 162.2 [152.0-168.8] | 1.129 | 5 |
| 1024 | 143.3 [134.1-151.2] | 160.4 [159.1-166.7] | 1.119 | 5 |

f64:

| n | pre-row | HEAD | HEAD/pre-row | passes |
|---|---|---|---|---|
| 64 | 64.0 [61.9-68.3] | 70.0 [68.4-70.9] | 1.094 | 5 |
| 256 | 77.9 [75.9-83.6] | 83.6 [82.2-88.2] | 1.073 | 5 |
| 512 | 78.8 [76.7-83.4] | 82.0 [76.3-87.2] | 1.040 | 5 |
| 600 | 79.9 [74.7-85.1] | 80.0 [74.0-83.4] | 1.002 | 5 |
| 768 | 79.9 [77.0-84.1] | 78.4 [76.7-84.5] | 0.981 | 5 |
| 1000 | 82.3 [77.5-85.3] | 80.1 [77.0-85.5] | 0.974 | 5 |
| 1024 | 82.5 [80.1-85.7] | 80.5 [77.4-82.9] | 0.976 | 5 |

f32 is faster at every size; f64's every range overlaps pre-row's (1024: [77.4-82.9] against [80.1-85.7]).
**Accepted.**

## avx2 against the peers (the bench)

f32:

| n | Cerid | Eigen | OpenBLAS | C/Eigen | C/OBLAS |
|---|---|---|---|---|---|
| 64 | 134.0 [125.8-147.3] | 146.3 [127.1-149.7] | 143.2 [138.2-149.3] | 0.92 | 0.94 |
| 256 | 164.1 [161.5-175.1] | 129.1 [126.2-139.2] | 162.6 [157.1-170.4] | 1.27 | 1.01 |
| 512 | 170.9 [169.1-177.3] | 153.2 [151.1-159.5] | 170.0 [166.8-178.9] | 1.12 | 1.01 |
| 600 | 164.8 [155.8-176.6] | 166.6 [165.8-179.2] | 168.6 [165.9-181.9] | 0.99 | 0.98 |
| 768 | 164.7 [159.0-175.1] | 166.6 [157.6-177.3] | 168.9 [163.9-177.6] | 0.99 | 0.98 |
| 1000 | 165.8 [163.6-174.1] | 163.0 [161.2-168.3] | 167.5 [158.2-168.7] | 1.02 | 0.99 |
| 1024 | 161.5 [158.2-165.8] | 167.0 [163.0-173.7] | 168.1 [163.7-175.2] | 0.97 | 0.96 |

f64:

| n | Cerid | Eigen | OpenBLAS | C/Eigen | C/OBLAS |
|---|---|---|---|---|---|
| 64 | 70.3 [67.9-74.5] | 69.6 [67.9-76.5] | 68.5 [63.7-70.5] | 1.01 | 1.03 |
| 256 | 86.9 [81.7-89.0] | 86.0 [81.1-88.1] | 80.1 [77.0-85.3] | 1.01 | 1.08 |
| 512 | 81.9 [79.0-87.1] | 82.7 [79.3-86.8] | 81.1 [78.3-86.3] | 0.99 | 1.01 |
| 600 | 79.4 [78.0-85.6] | 80.8 [76.8-82.0] | 80.8 [78.7-81.8] | 0.98 | 0.98 |
| 768 | 79.3 [78.3-80.8] | 80.9 [77.1-82.6] | 81.4 [78.9-82.5] | 0.98 | 0.97 |
| 1000 | 79.4 [77.8-81.2] | 77.6 [76.2-78.8] | 81.3 [80.4-85.9] | 1.02 | 0.98 |
| 1024 | 79.4 [79.1-86.2] | 79.6 [76.6-83.4] | 84.6 [79.8-88.0] | 1.00 | 0.94 |

The 2026-07 lead of OpenBLAS (avx2 f32 0.76x to 0.86x) is gone: f32 0.94x to 1.01x OpenBLAS and 0.92x to 1.27x Eigen,
f64 0.94x to 1.08x and 0.98x to 1.01x, inside the ranges at most sizes; recorded, not chased (the acceptance order).

## The lower levels

Cerid at sse4.2 and sse2 (both run the dense family's sse2 unit): f32 45 to 52, f64 21 to 25 GFLOPS at every size, at
or above the bench's computed 128-bit peak (42 to 48 and 21 to 24); portable 2 by 4: f32 10.7 to 11.7, f64 11.9 to
13.4. The peers' AVX2 rows beside them are not a same-ISA comparison.

## The arms: the serial resident threshold

HEAD's level tile down `gemm`'s forced paths (`detail::gemm_arm`), every arm with the Auto arm's bits. Auto took the
packed path from 600 (the 200M threshold) on; Resident is n/a at 1024 (its B budget).

### avx2

f32:

| n | auto | resident | packed |
|---|---|---|---|
| 64 | 150.7 [143.4-157.2] | 151.1 [143.6-156.5] | 142.9 [136.8-147.4] |
| 256 | 176.0 [166.2-181.8] | 169.6 [164.8-181.3] | 172.6 [162.9-178.2] |
| 512 | 178.6 [167.8-183.1] | 180.4 [167.0-183.6] | 176.8 [165.1-180.0] |
| 600 | 165.3 [163.8-177.1] | 166.4 [163.2-179.6] | 164.0 [161.7-174.9] |
| 768 | 166.5 [163.2-174.7] | 168.8 [166.6-175.7] | 166.5 [162.2-171.6] |
| 1000 | 165.3 [163.4-171.3] | 163.2 [161.9-171.6] | 165.1 [164.0-175.5] |
| 1024 | 162.8 [160.3-168.6] | 164.3 [159.3-168.0] | 164.4 [161.3-168.2] |

f64:

| n | auto | resident | packed |
|---|---|---|---|
| 64 | 73.2 [70.7-75.2] | 71.9 [70.9-74.2] | 68.6 [67.3-70.6] |
| 256 | 85.1 [82.7-89.8] | 85.7 [83.8-89.7] | 81.4 [80.3-88.0] |
| 512 | 83.3 [79.7-87.7] | 84.3 [79.8-88.3] | 85.3 [78.7-86.5] |
| 600 | 82.7 [79.8-86.0] | 84.6 [81.3-87.5] | 80.3 [78.4-85.9] |
| 768 | 80.4 [77.5-86.8] | 80.7 [78.5-86.9] | 80.3 [76.8-87.1] |
| 1000 | 84.8 [80.4-87.1] | 84.5 [80.9-88.2] | 83.6 [81.1-87.1] |
| 1024 | 81.6 [80.9-86.3] | n/a | 81.8 [79.9-86.0] |

### sse4.2 (sse2 unit)

f32:

| n | auto | resident | packed |
|---|---|---|---|
| 64 | 46.6 [45.2-47.8] | 46.8 [45.4-47.7] | 45.8 [44.0-46.4] |
| 256 | 51.0 [49.1-53.6] | 51.4 [48.8-53.9] | 50.4 [48.4-53.5] |
| 512 | 50.9 [49.0-52.8] | 50.8 [48.9-52.9] | 50.4 [48.5-52.4] |
| 600 | 50.5 [50.2-51.3] | 50.9 [50.7-51.2] | 50.6 [49.9-52.9] |
| 768 | 50.7 [49.6-54.0] | 51.0 [49.4-54.4] | 50.7 [49.7-53.5] |
| 1000 | 50.3 [48.8-53.7] | 51.0 [48.9-54.0] | 50.6 [48.4-53.7] |
| 1024 | 50.4 [48.7-53.8] | n/a | 50.8 [49.1-54.0] |

f64:

| n | auto | resident | packed |
|---|---|---|---|
| 64 | 23.4 [21.9-24.0] | 23.6 [21.8-24.0] | 23.2 [21.7-23.6] |
| 256 | 23.5 [23.3-25.7] | 23.6 [23.4-25.6] | 23.6 [23.1-25.4] |
| 512 | 24.9 [23.5-25.4] | 24.9 [23.7-25.5] | 24.7 [23.3-25.3] |
| 600 | 23.6 [22.7-24.8] | 23.8 [22.9-25.2] | 23.4 [22.8-24.0] |
| 768 | 23.8 [23.0-24.4] | 24.4 [23.0-24.6] | 24.4 [22.9-25.2] |
| 1000 | 24.0 [23.0-25.1] | 24.1 [22.9-25.3] | 23.9 [22.9-25.2] |
| 1024 | 23.9 [23.1-25.1] | n/a | 23.7 [23.2-24.9] |

### sse2

f32:

| n | auto | resident | packed |
|---|---|---|---|
| 64 | 47.4 [46.0-49.6] | 47.4 [46.1-49.6] | 46.1 [44.7-48.1] |
| 256 | 50.8 [47.6-51.7] | 50.9 [47.6-51.9] | 50.4 [47.2-51.1] |
| 512 | 50.8 [48.4-54.4] | 50.8 [48.4-54.2] | 50.4 [48.1-53.7] |
| 600 | 49.6 [48.1-54.0] | 50.3 [46.9-54.3] | 50.0 [45.0-53.7] |
| 768 | 49.3 [44.8-50.5] | 50.0 [45.5-51.3] | 49.4 [45.1-51.0] |
| 1000 | 50.9 [49.3-54.7] | 51.6 [49.8-55.0] | 50.6 [49.2-55.0] |
| 1024 | 53.5 [50.5-54.9] | n/a | 53.5 [50.6-55.3] |

f64:

| n | auto | resident | packed |
|---|---|---|---|
| 64 | 22.7 [22.0-24.2] | 22.7 [22.1-23.9] | 22.5 [21.6-23.5] |
| 256 | 24.4 [23.7-25.2] | 24.6 [23.7-25.2] | 24.0 [23.4-24.8] |
| 512 | 24.1 [23.2-25.0] | 23.9 [23.1-24.9] | 23.9 [22.7-24.7] |
| 600 | 23.7 [23.4-24.8] | 24.1 [23.8-25.0] | 23.8 [23.3-24.9] |
| 768 | 23.9 [23.0-24.6] | 23.9 [23.2-24.1] | 23.8 [23.0-24.0] |
| 1000 | 23.8 [22.6-25.1] | 23.9 [22.7-25.3] | 23.5 [22.6-25.0] |
| 1024 | 23.4 [22.7-25.2] | n/a | 23.1 [22.8-25.4] |

### portable

f32:

| n | auto | resident | packed | 4x4 auto | 4x4 resident | 4x4 packed |
|---|---|---|---|---|---|---|
| 64 | 11.2 [10.8-11.7] | 11.2 [10.8-11.7] | 11.1 [10.8-11.6] | 11.5 [11.1-12.0] | 11.5 [11.1-12.0] | 11.4 [11.0-11.9] |
| 256 | 11.4 [10.9-12.0] | 11.3 [10.9-12.0] | 11.3 [10.8-11.9] | 11.6 [11.2-12.2] | 11.6 [11.1-12.2] | 11.5 [11.2-12.1] |
| 512 | 11.8 [11.2-12.0] | 11.8 [11.2-12.0] | 11.9 [11.3-12.1] | 12.1 [11.5-12.3] | 12.0 [11.5-12.3] | 12.1 [11.6-12.3] |
| 600 | 11.7 [11.3-11.9] | 11.7 [11.3-11.9] | 11.8 [11.2-12.0] | 12.1 [11.5-12.3] | 12.1 [11.5-12.4] | 12.1 [11.5-12.1] |
| 768 | 11.7 [11.2-11.9] | 11.8 [11.2-11.8] | 11.7 [11.3-11.9] | 11.9 [11.5-12.1] | 11.9 [11.5-12.1] | 11.8 [11.5-12.1] |
| 1000 | 11.2 [10.9-11.8] | 11.2 [11.0-11.8] | 11.3 [10.9-11.8] | 11.5 [11.2-12.1] | 11.5 [11.2-12.1] | 11.5 [11.1-12.2] |
| 1024 | 11.7 [10.9-12.0] | n/a | 11.7 [10.8-12.0] | 12.1 [11.1-12.3] | n/a | 12.0 [11.1-12.2] |

f64:

| n | auto | resident | packed | 4x4 auto | 4x4 resident | 4x4 packed |
|---|---|---|---|---|---|---|
| 64 | 12.5 [12.1-13.0] | 12.5 [12.0-12.9] | 12.4 [11.8-12.6] | 15.2 [14.7-15.7] | 15.2 [14.6-15.9] | 15.0 [14.4-15.6] |
| 256 | 12.9 [12.5-13.5] | 12.7 [12.4-13.5] | 12.8 [12.3-13.4] | 15.6 [15.0-16.4] | 15.5 [15.1-16.4] | 15.3 [14.9-16.4] |
| 512 | 12.5 [12.2-13.3] | 13.2 [12.5-13.5] | 13.1 [12.6-13.8] | 16.0 [15.2-16.6] | 16.0 [15.0-16.3] | 16.0 [15.3-16.4] |
| 600 | 13.1 [12.4-13.4] | 13.2 [12.5-13.2] | 13.3 [12.7-13.3] | 16.0 [15.3-16.2] | 15.7 [15.2-16.1] | 16.0 [15.2-16.4] |
| 768 | 13.3 [12.7-13.4] | 13.0 [12.5-13.1] | 13.0 [12.7-13.2] | 15.7 [15.4-16.1] | 15.6 [15.3-16.2] | 15.7 [15.3-16.0] |
| 1000 | 12.8 [12.4-13.4] | 12.7 [12.2-13.3] | 12.8 [12.4-13.5] | 15.5 [15.0-16.1] | 15.4 [14.9-16.2] | 15.4 [15.0-16.2] |
| 1024 | 12.9 [12.4-13.7] | n/a | 12.9 [12.4-13.7] | 15.7 [14.9-16.6] | n/a | 15.6 [15.0-16.6] |

The resident median is at or above the packed one in every level and type at 600 and 768 and mixed at 1000 (avx2 f32
packed ahead): `kSerialResidentThreshold` is now 512M, between 768^3 (453M) and 1000^3 (1G). The margins (0.4% to 5%)
lie inside the ranges: the row's threshold set from the board, not a claimed win.

## The portable tile

The 4 by 4 tile beat the 2 by 4 at every size: f64 1.18x to 1.28x, f32 1.02x to 1.03x, same bits (both round twice).
It is now portable's only GEMM tile (ARM64 runs the dense family at portable) and the arm table is gone. A check run
of the new bench at portable after the change printed `portable level (4 by 4 tile)`, every arm `same`, f64 14.2 to
15.8 and f32 10.8 to 12.1 GFLOPS.

avx512 and the ARM64 levels are timed at QUAL.1-HW's hosts (SDE does not time); `detail::gemm_arm` stays as the paths'
bit-test seam and that board's arms.
