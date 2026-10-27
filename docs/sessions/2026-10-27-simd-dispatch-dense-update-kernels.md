# 2026-10-27 — SIMD-DISPATCH: the hesap-dense update kernels behind one typed table

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).

## CI read

- No completed run failed. Scheduled run 40475028681 (`bed4f492`) finished green on every job (the
  [vector-kernel note](2026-10-27-simd-dispatch-dense-vector-kernels.md) recorded it as in progress); 40478910518
  was cancelled before any job ran. Push runs 40479322092 (`f91847f5`) and 40495096865 (`5611351b`) are in progress
  or queued; nothing was dispatched.
- Needs CI audit: [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) keep their
  hosted evidence and follow [DIAG.11b](../ROADMAP.md#slice-diag.11b)'s user re-record (8); no flip.

## One typed table

`DenseKernels` now holds one `DenseTypeKernels<T>` per element type, and `kernels_of<T>(table)` /
`dense_kernels_of<T>()` (`dense_dispatch.hpp`) return it, with `kHasDenseKernels<T>` for f64 and f32. Every call
site's `if constexpr` pair over `_f64` and `_f32` fields (gemm_pack, blas1, blas2, cholesky, both kernel tests) is
gone.

## The new entries

Each is one call per whole operation, never per row:

| Entry | LAPACK/BLAS shape | Replaces |
|---|---|---|
| `axpy` | xAXPY | the public axpy's scalar loop; eig_sym's dsytrd `w` update |
| `gemvt` | y + A^T x, rows in order, column blocks held in registers | the row-by-row axpys of eig_nonsym's gehd2 and orghr and svd's dlabrd Y |
| `ger` | xGER, row scale alpha x[r] | the public ger's scalar loop (both layouts); the rank-1 sweeps of gehd2 and orghr |
| `rot` | xROT, two roundings on every level | eig_sym's `rot_cols` intrinsics |
| `reflect` | xLARF on rows (implicit unit head) | qr's panel sweep, qr_colpiv's trailing columns, eig_sym's form_q, gehd2's right update |
| `ldlt_update` | xSYTF2's 1 by 1 and 2 by 2 trailing update, row-major (leading) or column-major (trailing) segments, a zero factor skipping its term | dense ldlt's `axpy_negate` sweeps and hesap-direct's real `ldlt_axpy_negate` sweeps (unblocked and blocked fronts) |

dlatrd's lower matvec runs on the existing `symv` entry (alpha 1, beta 1 gives HEAD's association) and svd's X column
on `gemv` into contiguous scratch. The private SIMD copies are deleted: qr's `qr_axpy`/`qr_dot`, qr_colpiv's
`cp_dot`/`cp_axpy`, ldlt's `axpy_negate`, eig_nonsym's `sdot`/`saxpy`, eig_sym's rotation intrinsics and the SIMD
bodies of hesap-direct's `ldlt_axpy_negate` (kept as the complex fronts' scalar loop). hesap-direct's back
substitution takes the dense `dot` entry. `dot_simd.hpp` now serves only the split-complex sweeps, which are the
next slice.

The elementwise kernels end on a partial vector (`load_partial`/`store_partial`), so every element takes the level's
step, fused from avx2 and rounded twice below; `dense_kernels.hpp` declares it. dense LDLT's 2 by 2 step now scales
by the row's factor and reads the original columns (the products and their order are unchanged, the factor is the
row's rather than the column's), and its factor buffers are gone.

## Tests

`test_dense_vector_kernels.cpp` (`[kernels]`, so the forced arms run it) models each new entry on every level the host
runs, f64 and f32: axpy and rot over every remainder of the accumulator rotation; gemvt over row counts 0 to 9,
column counts across a vector, the register block and 300, and x strides 1 and 3 (NaN between the strides and in the
padding); ger in both alphas; reflect over 0 to 4 vectors and tails across a vector; the LDLT update in both
layouts, 1 by 1 and both 2 by 2 forms, with zero factors and an infinity in U at the index every segment reads, so a
skipped term that ran would show as NaN. Elements outside each kernel's reach hold a sentinel and must keep it.

Built on win-debug (crd-hesap-dense-tests, crd-hesap-direct-tests). Per the 2026-10-27 order, tests, teeth and the
Linux lane run on hosted CI; the boards (eig, svd, qr, ldlt, lstsq, the direct LDLT fronts and blas1/blas2) run in
the row-close benchmark pass.
