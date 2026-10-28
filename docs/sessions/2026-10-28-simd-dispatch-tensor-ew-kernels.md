# 2026-10-28 — SIMD-DISPATCH: elementwise, Tier D and einsum products move into the tensor kernels

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `c98b31fe`.

## CI first

No completed run has a failed job left to fix. The four failures of push run 40701630725 (`3c1debc6`, still running)
are the fft maybe-uninitialized and the `Commands` SIGSEGV, both fixed in `3dd1607a` and `c98b31fe`. Every completed
run in the last 15 is a push run that the concurrency group cancelled before it started (0 jobs), so the ARM64 lanes
have not run yet; push run 40720280012 (`c98b31fe`) waits for 40701630725. DIAG.11c and DIAG.12 still wait on a green
complete-tier run (40649898318 ended red on causes fixed since; the scheduled 40663338177 is still running).

## Why first

The baseline flip gives general code the SSE2 backend, where every Vec8f is two 128-bit halves and `simd::fma` is the
exact software emulation. The general-code SIMD bodies therefore move into kernel families before the flip. This
batch moves the ones that share the elementwise vector trait.

## What moved

A second unit of the tensor family, `kernels/tensor_ew_kernels.cpp` (declarations in `tensor_ew_kernels.hpp`), holds:

- `ew_binary_f32/f64[BinaryOp]`: the whole collapsed broadcast walk of `ew_binary` (vv, vs, sv and strided runs), one
  call per op.
- `reduce_f32/f64[ReduceOp]` (`ReduceKernels`): Tier D's block partial, the per-output row trees and the column
  folds, which serve `block_partial`, `reduce_fixed_tree`, `reduce_axes`' row and vertical paths and sparse's sums.
- `einsum_thin_*` and `einsum_smallm_*`: einsum's two pack-free products.

`BinaryOp`, `ReduceOp`, `kReduceBlock` and the lane group `kEwLanes` (8 f32, 4 f64 on every level) move into
`tensor_kernels.hpp` as data. `EwSimd`, the op functors' vector halves and the general `run_binary` are gone; the
dispatch gates of einsum read `kEwLanes`. At avx512 a step takes two groups in one Vec16f or Vec8d; Tier D's block
keeps its four accumulators (one zmm holds a0 and a1, another a2 and a3, split before they combine), so every level
gives the portable bits.

## A defect fixed in passing

The scalar Min and Max (`b < a ? b : a`) disagreed with the vector minps/maxps (`a < b ? a : b`) on a NaN and on a
signed-zero tie, so an element's result depended on whether it fell in the vector body or the tail. The scalar side
now uses minps semantics everywhere (kernels, Tier D functors), as the header documented.

## Tests

`test_tensor_kernels.cpp` gains three `[kernels]` cases, every level the host runs against scalar oracles of the
documented order: the binary walk (rank 0, 1 and 3; the four inner forms, reversed and strided operands, an empty dim;
NaN, infinities, signed zeros, subnormals, min/max ties; guard words around dst), Tier D (block lengths around every
stripe and block edge, rows sub-ranges, column folds across a column block, with and without specials) and both einsum
products (m 1 to 17, k 0 to 48, n tails, a transposed). win-debug: all levels to avx2 with VNNI; SDE 10.13.1 `-skx`:
the three cases with avx512, 33 assertions. The whole tensor suite and the einsum, batched, sparse, decomp, tt and
hyperopt programs pass on win-debug, and the nn cases through ctest (their model files resolve from its working
directory).

The elementwise and einsum boards (`run_elementwise_bench.sh`, `run_einsum_bench.sh`) now build on the mirror's lane
tree, link the tensor archive and run each level, as the reduce and permute boards do. `include-budget.json` holds
elementwise, reduce_axes and einsum_exec with `emmintrin.h` and `xmmintrin.h` forbidden on GCC and Clang too.

## Next

batched, sparse, sparse MTTKRP, nn and sddmm move the same way; then the jets need a per-level evaluation boundary
(jet_simd.hpp has no routine to dispatch); then the flip.
