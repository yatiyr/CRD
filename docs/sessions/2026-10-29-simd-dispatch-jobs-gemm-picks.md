# 2026-10-29 — SIMD-DISPATCH: gemm_parallel's crd-jobs levers as forced arms

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 9, the crd-jobs retune folded in from
[HESAP-JOBS-RETUNE](../ROADMAP.md#slice-hesap-jobs-retune)). Before:
[the sse2 board gates](2026-10-29-simd-dispatch-sse2-board-gates.md).

## CI first

- No completed run has a failed job, the cancelled push runs after 40917343735 included (each replaced while
  queued or mid-run; none of their finished jobs failed). Push run 40929108987 (1ebe9dc4) is still in progress; its
  finished jobs passed. It predates the win-asan bound (c364ab0d), so the bound's first run is still to come.
- Needs CI audit: DIAG.11c and DIAG.12 wait on a green complete-tier run; none has completed. Nothing cancelled or
  dispatched.

## What landed

The retune's first consumer, dense GEMM. Each lever is a field of `detail::GemmParallelPick`
(`crd/hesap/dense/detail/gemm_arms.hpp`) whose default is today's behaviour; `gemm_parallel` is
`detail::gemm_parallel_arm(GemmParallelPick{}, ...)`, so production and the arms share one body.

- `resident_mnk`: the resident-A threshold (200M, was `kParallelResidentThreshold` in blas3.cpp).
- `mc`: `PerWorker` (the Mc block shrunk to ceil(m / workers)) or the shape's whole `Block`.
- `pack_b`: the packed B serial (v0d's "measured net-negative", never saved as a board) or split into whole
  nr-column panels across the jobs, on both the packed and the resident path.
- `beta_only`: C = beta C at k = 0 serial (the kept-serial beta scaling the user named) or row-split.

Each split writes disjoint rows of C or disjoint panels of the packed B, so every arm gives serial gemm's bits.

- **Defect fixed:** `gemm_parallel_auto` gated on `a.rows() * b.cols() * a.cols()`. With op(B) transposed
  (`apply_q_block.cpp:121`) that is m k k, not m n k. It now uses C's dimensions and op(A)'s depth. The scout brief
  said "m*k*k" in general; only the op(B)-transposed case was wrong (a transposed A keeps m k).
- **Tests** (`test_blas3_parallel.cpp`): every lever combination at 1, 4, 8, 16 and 32 jobs against serial gemm,
  bit for bit over C and a ring of guard words. Shapes: resident and packed past one depth group, op(B)
  transposed, complex conjugate transposes, NaN, infinities, -0 and subnormals in A, beta 0 over a NaN C, and
  k = 0. The existing worker-count loops now reach 32.
- **Board:** `bench_hesap_gemm_parallel` was exploratory, with no gate. It now runs one process per `--threads N`,
  times serial gemm and the six arms round by round over 13 products per type (both sides of the 256K auto
  threshold, the resident threshold and the Mc block; skinny shapes, op(B) transposed, k = 0), gates each arm bit for
  bit and exits 1 on a miss. `scripts/run_bench_gemm_parallel.sh` runs it at 4, 8, 16 and 32 workers at each level.

## Checks

win-debug builds of crd-hesap-dense-tests and the bench; the dense test program once (373 cases pass, the lever
case included); strict tidy (LLVM 20) clean on the four C++ files; clang-format on the added lines; Allman braces; the
ctest guards; `bash -n` on the driver. No bench ran: the board runs at row close.

## Left

The retune's other consumers, one batch each: dense factor picks (lu and cholesky block and trsm split, syrk's
parallel threshold, eig_sym multisection and mrrr chunking, each with a forced worker count), hesap-direct
(supernodal, multifrontal, dense LU kernels), hesap-amg's serial Galerkin product, hesap-iterative's operator
threshold, hesap-autodiff's batch gradient and hesap-sched (a measured consumer or the unused link removed).
