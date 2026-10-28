# 2026-10-28 — SIMD-DISPATCH: the sparse tensor walks move into the tensor kernels

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `72642da4`.

## CI first

No completed run has a failed job left to fix. Push run 40701630725 (`3c1debc6`) is still running; its six failed
jobs (win-tidy, win-clang-cl-shipping and four GCC lanes) all have causes fixed at HEAD, as the
[batched note](2026-10-28-simd-dispatch-tensor-batched-kernels.md#ci-first) records. The other completed runs among
the last 15 are push runs the concurrency group cancelled before any job started. No new run is green, so no Needs CI
row changes.

## What moved

`sparse.hpp` and `sparse_mttkrp.hpp` no longer hold a vector body and no longer include `simd.hpp` or `<cmath>`.
Their drivers validate, zero the outputs, partition by the shape-only grain, snap to mode-0 runs, allocate the tiles,
parts and accumulator rows, and make one table call per task. The walks run from `TensorKernels::sparse_f32/f64`
(`SparseTensorKernels`) in a fourth unit of the tensor family, `kernels/tensor_sparse_kernels.cpp`:

- `ttm_scatter`: a range of nonzeros into the output, contiguous rows for the last mode, a scalar fma per element for
  the strided modes.
- `ttm_staged`: an inner mode's prefix runs through the zeroed tile and its blocked transpose.
- `fold`: the parts into the output in part order, in blocks of 2048 values so a block stays in L1 across the parts.
- `mttkrp_csf`: a range of root fibers, with the fused leaf fibers (four registers of sums) and the accumulator levels
  above them.
- `mttkrp_coo`: the whole serial COO walk, the CSF walk's operations found from the canonical order.

The walks step `Vec8f` or `Vec4d`, and two groups in a `Vec16f` or `Vec8d` at avx512, with partial tails. Each output
element is one chain of single-rounded fmas in the canonical nonzero order, so the register width changes no bit, and
the CSF and COO walks still agree bit for bit. The debug check that a canonical COO has no duplicate neighbours stays
in `mttkrp_coo`'s driver (`distinct_neighbours`). `sparse_cp.hpp` now includes `<cmath>` for its own `std::fma`.

ARM64 does not configure hesap-tensor yet (it links crd-jobs), so nothing changes there; NEON tensor units belong to
JOBS-PORT.2.

## Tests

- `test_tensor_kernels.cpp` gains a `[kernels]` case. Every level the host runs is checked against scalar `std::fma`
  oracles with guard words after the output, the tile and the scratch rows:
  - TTM at ranks 1 to 4, every mode, f across one, two and many vectors with tails, the whole range and a sub-range;
    the staged tile on every inner mode;
  - the fold at 1, 2 and 7 parts, around a vector, a register pair and the 2048-value block;
  - MTTKRP at ranks 2, 3 and 5, r from 1 to 16 lane groups plus one (so the four-register chunk repeats at avx512),
    all roots and a sub-range, and the COO walk against the same oracle.
  Values and factors carry NaNs, infinities, signed zeros, subnormals and extremes.
- The archive case checks every sparse entry.
- `crd-hesap-tensor-sparse-kernels-at-<level>` runs the whole sparse and sparse-CP suite (the csf-coo identity and the
  {1..16} moat included) under each forced level, and under SDE where the tree has it.
- `include-budget.json` holds `sparse_mttkrp.hpp` (and through it `sparse.hpp`) with the SSE2 intrinsics headers
  forbidden.

Local: win-debug native (portable, sse2, sse4.2, avx2, avx2+AVX-VNNI) and SDE 10.13.1 `-skx` at avx512 pass the new
case, the archive case and the sparse suite.

## Next

nn and sddmm move the same way, then the jets need a per-level evaluation boundary, then the flip. Row-close board
candidate: the strided mode-0 scatter at sse2 calls the exact fma per element; a vector form across f with strided
lanes would give the same bits.
