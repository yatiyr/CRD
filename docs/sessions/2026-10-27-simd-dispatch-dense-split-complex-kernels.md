# 2026-10-27 — SIMD-DISPATCH: the split-complex dense sweeps as table entries

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).
CI read for this batch: [DIAG.12 race specimen](2026-10-27-diag-12-race-specimen-meeting.md).

## The entries

The Hermitian tridiagonalization (eig_sym, zhetd2) and the complex Hessenberg reduction and its Q (eig_nonsym,
zgehd2 and zunghr) carry complex matrices as two real arrays. Their O(n³) sweeps are now five entries of
`DenseTypeKernels<T>`, one call per whole operation:

| Entry | Shape | Replaces |
|---|---|---|
| `creflect` | H = I - tau v v^H from the right on rows (u = row v, row - (tau u) conj(v)) | zgehd2's RIGHT loop of `simd_cdot_nc` and `simd_caxpy_conjx` |
| `cgemvt` | y + A^T x, the scale x[r] or conj(x[r]), column blocks held in registers | the `w += conj(v_k) row_k` loops of zgehd2's LEFT and zunghr |
| `cger` | A + alpha x y^T, the scale alpha x[r] | their rank-1 loops (alpha = -conj(tau) or -tau, which gives the negated products' bits) |
| `chemv` | x = A v for the lower Hermitian triangle, one pass a row, as `symv` | zhetd2's four `simd_dot` and four `simd_axpy` calls a row |
| `cher2` | A - v w^H - w v^H on the lower rows, one pass a row, the diagonal kept real | zhetd2's eight `simd_axpy` calls a row |

`detail/dot_simd.hpp` and `detail/dot_simd_complex.hpp` are deleted; eig_sym.cpp and eig_nonsym.cpp include no SIMD
header.

## Arithmetic

`dense_kernels.hpp` declares it. The complex dot sums its four real products in one pass in two accumulators each
from avx2 (eight FMA chains, as HEAD's f64 body) and one below (a vector is two XMM there), whole vector q to
accumulator q % A, then the scalar tail rounding twice. A complex axpy gives each part two steps, the s_re term first,
which is HEAD's order for every zgehd2 and zunghr sweep. Below avx2 every step rounds twice, from avx2 it fuses.

Bits against HEAD:
- Below avx2 they change: HEAD ran `simd::fma`, a per-lane libm fma at the baseline ISA.
- avx2 keeps HEAD's whole-vector bits for f64 zgehd2 and zunghr. Three things change there:
  - the elementwise tails now take the level's step (HEAD rounded them as `y + (s_re x_re - s_im x_im)`);
  - a single whole vector after the unrolled pairs now joins accumulator 0 (HEAD summed it in the scalar tail);
  - f32 dots use two accumulators (HEAD used one).
- zhetd2's imaginary parts now take the s_re term first. HEAD took the s_im term first there and the s_re term
  first in zgehd2, so one rule now covers all five entries.

## Tests

`test_dense_vector_kernels.cpp` models each entry from the declaration on every level the host runs, f64 and f32.
- Lengths cover every remainder of a vector and of the two-accumulator rotation, plus 40 and 100. The cgemvt and cger
  columns cross the register block.
- Both conjugations of cgemvt are covered, and alpha (1, 0) and (-0.75, 0.5) for cger.
- Elements a kernel must not read hold NaN: the upper triangle, the padding, the diagonal's imaginary part and one
  past every vector. Elements it must not write hold a sentinel. chemv's x starts as NaN, since it is written and
  never read.
- Built on win-debug: `[kernels]` passes (172 assertions), and so do the Hermitian, complex and Hessenberg eigen cases
  (221,902 assertions). Forced levels, the Linux lanes and teeth run on hosted CI. The eig_herm and complex eig boards
  run in the row-close pass.
