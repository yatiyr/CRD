# 2026-10-29 — SIMD-DISPATCH: the SELL board's one-binary driver and the complex Eigen peer

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left items 3 and 4, the SELL policy and Delta16 boards).
Before: [the ARM64 ASan cost](2026-10-29-simd-dispatch-arm64-asan-cost.md).

## CI first

No completed run has a failed job. The push runs from 40883241613 to 40911547628 were each cancelled by the next push
while queued and hold no job record; host-check runs 40905507091 and 40911546716 passed. Push run 40917343735
(a1901325) is pending and was not touched. DIAG.11c and DIAG.12 stay Needs CI: no complete-tier run has been green
since the red scheduled run 40849112570, and none was dispatched while a push run is queued.

## What landed

- **The complex Eigen peer** (`runtime/examples/peers/eigen_peer.hpp`): `SpC32`/`SpC64` (`SparseMatrix<std::complex>`,
  RowMajor) from `ComplexEntry {row, col, re, im}`, `VecC32`/`VecC64` from (re, im) pairs, their `data()`, and
  `spmv`. No `std::complex` crosses the header.
- **`bench_hesap_spmv_vs_reference`**, rewritten as the board's one binary, for f64, f32, c64 and c32 over uniform-16,
  uniform-4, banded-5 and power-law, at half the L2, between the L2 and the L3 (their geometric mean), half the L3
  (all derived from `sparse_resident_bytes()` and the reported L3, 16 times the L2 where none is reported) and 1M and
  2M rows.
  - `--one-thread`: the production `spmv_sell`, the forced `SellIndex::U32` and `Delta16` builds (the Auto cut's
    arms; the one Auto picked is its own build, so a case holds two), the level's policy forced Resident and Streaming (the sweep cut's), and `Loads` and `Gather` at pair widths
    1, 8, 16, 32 and none, with Eigen on one thread, interleaved round by round. A forced gather and width leave the
    sweep nothing to pick, so those arms need no sweep of their own.
  - `--threads`: the U32 and Delta16 builds at both `SellJobCut`s at 1, 4, 8, 16 and 32 jobs (n/a above the CPUs the
    process may use), then Eigen on as many OpenMP threads in its own block, since libgomp's idle threads spin after a
    region on the CPUs the next arm would use.
  - The gate: every Cerid arm's y, poisoned before its checked run, equals the CSR spmv's bytes; a `Gather` pick is
    refused exactly at portable and sse2; Eigen stays within 4 (len + 2) eps of each row's sum of |a| |x| (twice
    either side's worst in-order error). A miss exits 1. The old bench returned 0 and timed its arms one after another.
- **`scripts/run_bench_spmv.sh`**: per level, the one-thread phase pinned to one CPU, then the thread phase on every
  CPU (`run-lane-bench.sh` pins one CPU by default, which would make a 32-job sweep meaningless).

## Checks

- The bench cannot be configured here (the eigen archive is not among the local pins). Both units compiled
  objects-only on WSL GCC 13 with the tree's `-Werror` warnings: the bench at the baseline, the peer at
  `-march=native -fopenmp` against the eigen sources in `build/linux-gcc-release/_deps`. GCC's
  `-Wformat-truncation` caught an arm-name buffer too short for a 10-digit width; it is now 32 bytes.
- Strict tidy (LLVM 20) is clean on the four files through a scratch database of those two commands (LLVM's
  `omp.h`, since GCC's does not parse under Clang); clang-format has no finding; Allman braces, the plan and
  repository checks pass.

## Left

- The board itself in the row-close pass; its configure needs the eigen archive (cmake/pins.json), a download to ask
  for then, as for the GEMM board.
