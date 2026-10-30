# 2026-10-30 — SIMD-DISPATCH: the SELL driver's narrow-slice cost against the pre-row engine

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (row-close boards, the SELL family). Before:
[the fft codelet pool](2026-10-30-simd-dispatch-fft-codelet-pool.md).

## CI first

No completed run has a failed job: the cancelled push runs 41096064108, 41102050677 and 41102258715 hold none, and
push run 41059057789 (e0c70766, in progress, not cancelled) failed only linux-clang-tsan and
linux-gcc-relwithdebinfo, both fixed on main. Needs CI: DIAG.11c and DIAG.12 still wait on a green complete-tier run;
none dispatched (push runs pending). Repository variables `FTL_GCP_PROJECT`, `FTL_WIF_PROVIDER` and
`FTL_SERVICE_ACCOUNT` exist (names only); no secrets, so the AMD and Mac lanes wait on their accounts.

## The probe

One source, `~/sell-board/sell_probe.cpp` (WSL, untracked like the GEMM board's probe), built with each tree's own
commands for `bench_hesap_spmv_vs_reference` (its object in place of the bench's): the pre-row tree
`~/prerow-tree-build/linux-gcc-release` (`2f2aed72`, SELL at avx2 only) and the HEAD mirror tree. It runs the bench's
generator, seeds, five bands (L2 2 MiB, L3 36 MiB as the bench reads them) and inner counts through the production
`to_sell`, `spmv_sell` (one thread, CPU 4) and `spmv_sell_parallel` (1 to 32 jobs), best of 15 rounds, and exits 1
when a run leaves the first run's bits.

First single pass at avx2, pre-row time over HEAD's (above 1: HEAD faster): f64 uniform-4 0.65 to 0.85 at every band
but 1M, banded-5 0.77 to 0.90 below DRAM, f32 uniform-4 0.66 to 0.87, banded-5 0.84 to 0.91; uniform-16 and the
complex types level or faster (c32 1.15x to 3.9x). The bench's own arms put no pick near pre-row (f64 uniform-4 in
half the L2: every arm 0.031 to 0.036 ms against pre-row's 0.024), so the cost sat in the driver, not a policy.

## Cause

`~/sell-board/sell_ab.cpp` ran pre-row's f64 AVX2 loop on HEAD's own U32 matrix (same bits): 0.0235 ms against HEAD's
0.032 in half the L2 for uniform-4, 0.0175 against 0.025 for banded-5, level for uniform-16; a cost per slice, about
15 cycles, that only narrow slices notice.

- Under GCC `finish` and `write_slice` were called out of line from `sell_spmv` for every slice (objdump of the
  avx2 unit). Forcing them inline alone did not move the time.
- Every slice ran through one loop that also carried the Delta16 walks and their pair checks, with the slice state
  spilled across it. A U32-only loop for ranges without a Delta16 slice: 0.032 to 0.0295 ms.
- Every slice scanned its C row lengths for its shortest row (`full_columns`): with that scan replaced by a stored
  value 0.0285 to 0.0248 ms (the experiment's 13%).

## Fix (5afef104)

- `to_sell` stores `slice_full`, each slice's shortest row (a pad row's 0 included); `SellSlices` carries it and the
  kernels read it once (`full_columns` gone). `stored_bytes` counts it. The to_sell case checks it against the row
  lengths and the stored words.
- `sell_spmv` runs `sell_slices<Body, false>` when `delta_ptr` counts no Delta16 slice in the range.
- `finish`, `run_slice`, `run_pair`, `write_slice` and the slice loop are `CRD_FORCEINLINE`; the row writer is a
  by-value copy in the loop.
- `bench_hesap_spmv_vs_reference` warms each arm with one untimed call: in the interleaved rounds the first arm after
  Eigen paid for bringing its matrix and x back (production `spmv_sell`, always first, read 1.54 ms against its own
  matrix's 1.03 to 1.37 under the other arms at f64 uniform-4 half the L3).

After it, one pass: f64 uniform-4 half the L2 0.026 against pre-row's 0.0229 in the A/B, the probe's f64 cells 0.84
to 1.42, f32 uniform-4 0.75 to 1.06 (single passes on a contended host; the board decides).

Checks: win-debug `crd-hesap-sparse-tests` whole (113 cases) and `[kernels]` at portable, sse2, sse4.2, avx2 and
avx512 under SDE 10.13.1 (`-spr`); the bench's bit gate on the WSL release tree; strict tidy clean (the bench and
kernel on the WSL database, the rest on win-debug); clang-format findings no more than HEAD's; the ctest guards (19);
Allman, plan and repository checks.

## Also seen

`spmv_sell_parallel` below the L3 costs about 0.36 ms at 8 to 32 jobs on both engines (pool wake and join), against
0.02 to 0.3 ms for the whole spmv on one thread. The sparse LinearOps already pick serially below their cut
(`OperatorPick`); the explicit-jobs entry is the caller's choice. The retune's thread sweep judges it.

## Left

The SELL board itself (five interleaved passes: the probe on both engines, then `scripts/run_bench_spmv.sh` at every
level, `~/sell-board` holding the driver and the parser): not run, the host stayed contended for 30 minutes
(`build-bench.py host`: other processes 6.1 to 6.6 cores, dota2 3.0 to 3.3). Then permute, the sse2 bodies and the
retune's thread sweep.
