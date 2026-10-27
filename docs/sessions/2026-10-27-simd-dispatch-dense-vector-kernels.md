# 2026-10-27 — SIMD-DISPATCH: the hesap-dense vector kernels and an SSE2 Vec4d

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).

## CI read

- No completed run since the last read: 40478910518, 40466574508, 40462897802 and 40458470799 were cancelled before
  any job ran. Scheduled run 40475028681 and push run 40479322092 (`f91847f5`) are in progress with no failed job;
  40495096865 (`5611351b`) is pending. Nothing was dispatched.
- Needs CI audit: no complete run finished since 40455017769, so [DIAG.11c](../ROADMAP.md#slice-diag.11c) and
  [DIAG.12](../ROADMAP.md#slice-diag.12) stay as recorded (their hosted evidence is in place; they follow
  [DIAG.11b](../ROADMAP.md#slice-diag.11b)'s user re-record (8)).

## An SSE2 Vec4d

`Vec4d` was a four-element array on every backend below AVX2, so the sse2 lanes and every sse2 kernel unit ran its
f64 code one lane at a time. On SSE2 and SSE4.2 it is now two `__m128d` (`lo` holds lanes 0 and 1): arithmetic, min
and max, `abs`, `sqrt`, the comparisons, `select` (and, andnot, or), the partial loads and stores (they read and write
only the first `count` lanes, as the mask forms do), the complex deinterleave and interleave, `swap_pairs`, `addsub`
(SSE2 has no `addsubpd`: a `movsd` merge of the difference and the sum), the 128-bit concatenations and the 4 by 4
transpose. `fma`, `fnmadd` and `fmaddsub` keep one rounding through the C library's fma per lane. NEON and portable
keep the array. The level probe (`[simd-levels]`, 94 operations) already compares every Vec4d operation of the sse2
and sse4.2 units with portable's bits; it passes, and swapping `addsub`'s halves and dropping the third partial lane
fail it on win-debug and linux-gcc-debug (`vec4d addsub`, `vec4d pair and partial loads`), restored and rebuilt.

## The vector kernels

The dense family's table gains the BLAS-1 and BLAS-2 real kernels and the unblocked Cholesky: `dot_f64`/`dot_f32`
(also the nrm2 sum of squares), `gemv_*` (`GemvRows`: any row stride), `symv_*` (`SymvLower`), `trsv_*` (`TrsvRows`:
lower or upper, unit or not), `cholesky_*` (the left-looking unblocked factor, returning the failed column) and
`cholesky_trsm_*` (the panel solve over a worker's row range). Each call is a whole operation, never a row: the
factorizations call the Cholesky entries once per block and once per worker range.

- The bodies are written once over `Vec4d` and `Vec8f`, which each level backs with its own registers; a level sets
  only the accumulator count (8 from avx2, 4 below: eight independent adder or FMA chains in sixteen registers) and
  the gemv rows per tile (8 and 4). A step fuses from avx2 and rounds its multiply and add below (SSE2 has no FMA);
  the scalar tails round twice everywhere. `dense_kernels.hpp` declares each reduction's shape: whole vector `v` of a
  dot product goes to accumulator `v % A` (the remainder continues the rotation, so a short dot keeps its chains),
  the accumulators sum as a fixed tree and the lanes as `horizontal_sum`'s; a gemv row has one accumulator; symv,
  trsv and the Cholesky rows use the dot product's shape.
- `blas1.cpp`, `blas2.cpp` and `cholesky.cpp` call the table; `blas2.cpp`'s inline AVX2 gemv and symv tiles and
  their `_mm_prefetch` blocks, `dot_simd.hpp`'s eight-accumulator `simd_dot_f32/f64` and `simd_sumsq_*`, and
  `cholesky.cpp`'s `chol_dot` copy and unused `sqrt_value` are deleted. `eig_sym.cpp`'s private copies of `simd_dot`
  and `simd_axpy` are deleted for the shared ones (identical bodies; the dedup filed on 2026-05-23).
- MSVC: a lambda that also ran the gemv column tail stayed out of line, so the accumulators lived in memory and every
  step stored them; the registers now reduce first. Capturing symv's update state by value and copying the gemv and
  symv views (a store through `y` could otherwise change `alpha` in the compiler's view) brought the avx2 symv to six
  stack references, all prologue. `dumpbin` of the optimized objects shows the dot and gemv loops register-resident on
  both levels; the sse2 symv keeps part of its sixteen XMM live set on the stack (four two-register accumulators, the
  scaled `x[i]` and the row, x and y vectors).

### nrm2

The sum of squares overflowed for large entries (3e200 and 4e200 gave infinity), lost the norm for small ones (their
squares underflow), and for complex vectors the pairwise sum's compensation subtracted infinities into NaN. nrm2 now
redoes a sum that is not finite, or below `count` times the least normal (where the squares may have lost more than an
ulp), as `amax * sqrt(sum (x / amax)^2)`, as LAPACK's xNRM2 scales every input; a NaN gives NaN and an infinity
infinity. The fast path is unchanged.

### Determinism

Each level is bit-exact with its declared shape and arithmetic (ADR-0134 §7); portable and sse2 agree bit for bit.
HEAD's sse2 lanes ran `Vec4d::fma` through `libm::fma` per lane (a single rounding in software); the sse2 level now
rounds twice, like the GEMM blocks. The levels' dot products agree within `2 n eps sum |a x|`.

## Verification

`test_dense_vector_kernels.cpp` (`[kernels]`, so the forced `crd-hesap-dense-kernels-at-<level>` arms run it): on
every level the host runs, f64 and f32, a model of the declared shape gives every result's bits for

- dot and the sum of squares at lengths 0 to 2 * 8 w + w + 2 (every remainder of an eight-accumulator rotation), 255
  and 1000, within the bound of portable's;
- gemv at 0 to 17 rows, 0 to 600 columns (the prefetching rows), row strides 0 or 3 past the columns, two alpha and
  beta pairs;
- symv at orders 0 to 600 with strides 0 or 2 past, NaN above the diagonal;
- trsv lower and upper, unit (NaN diagonal) and not, at orders 1 to 300;
- the unblocked Cholesky (orders 1 to 70, strides 0 or 3 past, and an indefinite column halfway, which must return
  its index with the earlier columns factored) and the panel solve over a row range.

Padding, the other triangle and the element past each end hold NaN; the element after y and the rows outside a
solve's range must not change. `test_blas1_real.cpp` and `test_blas1_complex.cpp` add the scaled nrm2 cases.

Teeth, each built, observed failing, restored, rebuilt and rerun on win-debug (round by round):

| Break | Fails |
|---|---|
| avx2 accumulator tree regrouped; gemv tail skips a column; nrm2 underflow rescale off | dot, symv, trsv, Cholesky (avx2); gemv; nrm2 real, complex |
| the levels below avx2 fuse; nrm2 overflow rescale off | every vector case (sse2, portable); nrm2 real, complex |
| the remaining whole vectors stop one early | dot, symv, trsv, Cholesky |
| gemv ignores the row stride; symv tail update drops alpha; upper trsv reads one past; Cholesky returns the column, not its index; the panel solve writes one row past its range | gemv, symv, trsv, Cholesky |
| symv adds the diagonal outside alpha; symv's dot reads the diagonal; lower unit trsv divides by the diagonal | symv, trsv |

linux-gcc-debug: the avx2 tree, below-avx2 fusing and nrm2 rounds and the `addsub` tooth fail and pass restored.

The full crd-hesap-dense-tests (362 cases) pass on win-debug at the selected level and forced to sse2 and portable,
and on linux-gcc-debug at the selected level and sse2; crd-hesap-direct-tests (190 cases) pass at the selected level
and sse2 on win-debug and on linux-gcc-debug; crd-math-tests passes on both; the forced arms and the 15 guards pass.
Strict tidy is clean on every changed source and on each level entry of the kernel unit and of the sse2 probe unit
(one-entry databases); clang-format reports nothing on added lines; Allman passes.

## Board

No board ran in this slice (user order, 2026-10-27): the blas1 (dot and nrm2, f64 and f32), blas2 and
solvers boards, edited here to print the selected level, run per level against HEAD-before-the-row and the peers in
the row-close benchmark pass.
