# 2026-10-28 — SIMD-DISPATCH: the batched linear algebra moves into the tensor kernels

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `fc5fae7a`.

## CI first

No completed run has a failed job left to fix. Push run 40701630725 (`3c1debc6`) is still running. Its six failed
jobs have causes that are all fixed at HEAD:

- win-tidy: a branch clone in `fs6_transpose_inplace` and a nested conditional in the level probe, both fixed in
  `1597b0a2`.
- linux-gcc-debug: the `Commands` SIGSEGV in test_schedule.cpp:554, fixed in `c98b31fe`, which touches commands.cpp
  and that test.
- The GCC maybe-uninitialized failures of release, shipping and public-checks: fixed in `3dd1607a`.

The other runs among the last 15 are push runs that the concurrency group cancelled with no failed job, so no ARM64
lane has run yet. DIAG.11c and DIAG.12 still wait on a green complete-tier run: 40649898318 ended red on causes fixed
since, and the scheduled 40663338177 is still running.

## What moved

`batched.hpp` no longer holds a vector body. Its drivers check shapes, partition the batch and run the dense GEMM above
the tiny tier. The per-matrix work runs from `TensorKernels::batched_f32/f64` (`BatchedKernels`), in a third unit of the
tensor family, `kernels/tensor_batched_kernels.cpp`. Each entry covers a range of matrices:

- `gemm_tiny`: R <= 4 rows by two column vectors, or 5 to 8 rows by one, with partial loads and stores at the tails.
  At avx512 a column vector is a Vec16f or a Vec8d.
- `cholesky`, `lu`, `svd`: the lane tier packs kEwLanes matrices element-major, one lane each, up to
  `kBatchedCholMaxN` (32) and `kBatchedSvdMaxN` (16). Above that n the scalar tier runs.
- `cholesky_solve`, `lu_solve`.

The scalar tier (`chol_scalar_one`, `lu_scalar_one`, `svd_scalar_sweeps`, `svd_finalize_one`) is in
`batched_core.hpp`. It sits in the level namespace and calls only `libm`, so decomp.hpp, the tests and each kernel
level compile their own copies. nn's two direct GEMMs call `gemm_tiny`. The five copies of the parallel_for setup
became `run_ranges` and `run_groups`.

At avx512 one step takes two groups in a zmm when their staging tile fits the one-group tile: n <= 22 for Cholesky
and LU, n <= 11 for the SVD. The stack stays at 32 KiB per tile. The drivers run on Small jobs fibers, which have
64 KiB, so a two-group tile at n = 32 would not fit. Every lane runs the scalar tier's operations, and a lane that is
done keeps its bits through a select. A matrix therefore gives the same bits whatever its lane, step or level (NaN
payloads aside, ADR-0134 §7).

## Tests

- `test_tensor_kernels.cpp` gains a `[kernels]` case. Every level the host runs is checked against scalar oracles:
  - the tiny GEMM: m 1 to 9, k 0 to 7, n tails across one and two vectors of every width, beta 0 with NaNs in c, and a
    range that leaves matrix 0 untouched;
  - Cholesky and LU against `batched_core.hpp`: every lane-tier edge of n and past it, a batch of 19, poisoned lanes
    (non-SPD, NaN, infinity, subnormal, signed zero, pivot ties, a zero column) and two ranges that split a group;
  - the SVD at 30 and 2 sweeps, with zero and rank-deficient matrices;
  - both solves against plain substitution loops.
  Guard words sit between the matrices and after the last one.
- The archive case checks every batched entry.
- `crd-hesap-tensor-batched-kernels-at-<level>` runs the whole batched suite under each forced level, and under SDE
  where the tree has it.
- `include-budget.json` holds `batched.hpp` with the SSE2 intrinsics headers forbidden.

## Next

sparse and sparse MTTKRP, then nn and sddmm, move the same way. Then the jets need a per-level evaluation boundary,
and then the flip.
