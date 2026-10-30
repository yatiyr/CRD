# 2026-10-30 — SIMD-DISPATCH: the sparse LinearOps' crd-jobs picks

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 9, the crd-jobs retune folded in from
[HESAP-JOBS-RETUNE](../ROADMAP.md#slice-hesap-jobs-retune)). Before:
[SaAmg's levers](2026-10-30-simd-dispatch-jobs-amg-picks.md).

## CI first

- Push run 40976392370 (c28aaa54), still running: linux-gcc-debug, -relwithdebinfo, -debug-scalar, -public-checks,
  -asan, -release, -shipping and linux-clang-tsan failed, every one on the test_cholesky.cpp:340 double promotion fixed
  at bc2bdb83. Every completed run since was cancelled before a job started; 40985293535 (f295f52a) is pending.
  Nothing cancelled or dispatched.
- Needs CI audit: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none has run. Nothing flips.

## What landed

hesap-iterative has no jobs code of its own: every parallel step of a Krylov solve is inside its operator. The
retune's levers for it are therefore the three hesap-sparse ops the solvers run on.

- **`detail::OperatorPick`** (hesap-sparse `detail/operator_arms.hpp`): `parallel_min_bytes` (default the L2,
  `sparse_resident_bytes()`) and `workers` (0 is the pool). `ParallelSparseLinearOp`, `ParallelSpmvLeastSquaresOp` and
  `ParallelSpmmLinearOp` take it in a new constructor; each bytes constructor forwards with the cut alone, so the
  three-argument call sites and the defaults keep their behaviour. The two SELL ops share one apply (`apply_sell`);
  `spmv_dispatch` is gone.
- One job, or a pool that is down, now applies the serial kernel with no `parallel_for` (`operator_jobs`), as gemm and
  `spgemm_arm` do. Before, a down pool past the cut called `parallel_for` and a one-worker pool dispatched one job;
  both give the same bits.
- **Open for the board:** the spmm op measures A's values only (`nnz * sizeof(T)`), the SELL ops `stored_bytes()`
  (values and every index array), and neither counts x and y; the pick's comment records it, and production keeps
  each measure until the board shows where the crossover is.
- **Kept serial:** every Krylov reduction and vector update (KBN-pairwise blas1 on the calling thread); the board
  reports each solve's time outside the operator. FEAST's forced cut 0 (feast.hpp:154) is unchanged.
- **Outside the six retune modules, named for the row close:** hesap-sparse's other `parallel_for` sites (the
  level-scheduled triangular solve, BSR, DIA, ELL, SDDMM and the parallel spgemm's hash body) and
  hesap-preconditioners' SPAI, FSPAI and Schwarz. Neither taken in nor dropped here.

Determinism: both levers are scheduling. Each output row is one job's, in the serial order, so every pick gives the
serial spmv's and spmm's bits (ADR-0065 D(sparse)-3).

## Tests

- `test_operator_arms.cpp` (hesap-sparse), f64, f32, c64, c32, with and without NaN, +-Inf, -0 and subnormals in A
  and x: square n 1, 3, 7, 9, 33, 65 and 500 (slice tails, fewer rows than jobs, skewed and empty rows) and 300 x 170,
  90 x 260 for the least-squares op; cuts 0, never and the L2 by workers 0, 1, 2, 4, 8, 16, 32. Every A x, A^H y and
  A X (s 1, 4, 8, ldx and ldy past s) against the serial kernel byte for byte, the CSR spmv on finite inputs,
  `is_parallel()` against each measure, guard words after every output and in the block padding, the bytes
  constructor against the pick, and a 0 x 0 matrix. The arm values and matrices moved from test_spgemm_parallel.cpp to
  `sparse_arm_fixture.hpp`, shared by both.
- CG (test_cg.cpp), LSQR (test_lsqr.cpp) and block CG at s 1, 4, 8 (test_block_cg.cpp) on 1 to 32 forced jobs repeat
  the serial op's iterations, residual history and solution bit for bit.

win-debug: the operator and spgemm cases 1.4 s; the three solver cases 1.0 s.

## Board

`bench_hesap_operator_parallel` (`scripts/run_bench_operator_parallel.sh`): 2D and 3D Laplacians and a uniform
16-per-row pattern from 256 rows (a few KiB) to 1M rows (DRAM), f32 and f64; each op's apply on the serial kernel, on
2 to N forced jobs and at production's pick with its measure against the cut, and a 40-iteration CG, LSQR and block
CG (s 4, 8) with the time outside the operator; every arm gated bit for bit, a miss exits 1. No bench ran: the board
runs at row close. Peers: the serial op in the same binary.

## Checks

win-debug builds of crd-hesap-sparse-tests, crd-hesap-iterative-tests and the bench; the new cases pass; strict tidy
and clang-format on the changed files (added lines); Allman braces; the ctest guards (include budget with the new
entry, source routes); check-master-plan; check-repository.

## Left

hesap-autodiff and -sched.
