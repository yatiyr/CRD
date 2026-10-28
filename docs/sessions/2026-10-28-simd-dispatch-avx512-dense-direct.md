# 2026-10-28 — SIMD-DISPATCH: the dense and direct zmm bodies

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `6210ab6f`. CI read for this batch:
[512-bit types, CI read 2](2026-10-28-simd-dispatch-avx512-types.md#ci-read-2).

## The vector steps

`vector_steps.hpp` maps each type to the level's vector: `Vec8d` and `Vec16f` (one ZMM) at avx512, and `Vec4d` and
`Vec8f` below it. The step stays `simd::fma` from avx2. So every elementwise dense kernel (axpy, gemvt, ger, the
reflector and LDLT updates, the split-complex axpys), and every hesap-direct chain, gives avx2's bits at avx512.

The reductions keep their accumulator counts (eight dot chains, two per complex product, eight gemv rows). The width
grows, so a dot product's lanes and its rotation over whole vectors change. `dense_kernels.hpp` declares this as the
third shape. `horizontal_sum` is the pairwise tree over the w lanes (the two halves' sums), as `Vec8d` and `Vec16f`
compute it. avx512 agrees with avx2 within the inner-product bound, and the differential test checks that bound
against portable on every level.

## The GEMM tile

`six_row_tile` becomes `row_tile`, which unrolls `kTileRows` rows over constant register indices. Below avx512 it
keeps 6 rows. At avx512 it takes 14 rows: 28 accumulators, two B registers and one A broadcast, 31 of 32 ZMM, the
transpose of BLIS's skx shape (f64 14 by 16, f32 14 by 32). Each step prefetches every line of the next panel's B row,
which is two lines at avx512 and one below it, as before. The fused path prefetches every line of each C row. The
element sums are unchanged (one fused step per depth value in depth order), so avx512 gives avx2's GEMM bits on every
path.

`gemm_mc` and `gemm_nc` (`gemm_pack.hpp`) round kGemmMc and kGemmNc down to whole tiles of the level's shape. 480
rows would have left a 4-row panel in every Mc block of 14-row tiles, and 4080 columns a half panel of 32. gemm,
gemm_parallel and the lower syrk use them. The blocks change no bits; only kGemmKc groups a sum. blas3's unused `kMc`
alias is deleted.

## hesap-direct

At avx512 `kReduceColumns` is 16 (16 accumulator ZMM). Each panel column keeps its own accumulator over the rows in
order, so the group size changes no chain. `direct_kernels.hpp` now says avx512 gives avx2's bits. The forward walk's
test lengths add the 512-bit tails.

The supernodal Cholesky's row-block walk places an mr by 64 snapshot after a `row_blk` by 256 block in a buffer of
`max_nr * max_nc` values (`supernodal_cholesky.cpp`). At mr = 14, `row_blk` is 84, so the snapshot ends at 22,400
values, below the 24,960 of mr = 6, and the bound holds. The non-`[kernels]` dense and direct cases have no avx512
run until a hosted runner detects AVX-512; only the `[kernels]` cases run under SDE.

## Tests

- `test_dense_vector_kernels.cpp` models the level's width: `width<T>(level)`, a pairwise `lane_sum` over w lanes,
  and length and column lists that cover the tails of both widths.
- Two test defects showed up at avx512 under SDE.
  - The packer case read a 41-column source with a column stride of 41. That is past the buffer for 2 nr + 1 = 65 f32
    columns, so the case failed with 22 wrong values. The source is now 73 square.
  - The paths case sized m as 6 mr nr + 1, which is 2,689 rows at f32 14 by 32. Its m by m syrk C exhausted the 32 MB
    arena (a TlsfAllocator fatal). m is now `gemm_mc(shape) + 2 mr + 1`, which crosses one Mc block and ends in a
    partial panel on every shape.
- win-debug: dense `[kernels]` 203 assertions, direct `[kernels]` 124,682, on portable, sse2, sse4.2 and avx2.
- SDE 10.13.1 `-skx` with `CRD_CPU_LEVEL=avx512` and `[cpu-level-applied]`: dense 216 assertions in 20 cases (85 s);
  direct 124,683 assertions in 14 cases (414 s).
- Hosted: the win-release and linux-gcc-release SDE arms (`crd-hesap-dense-kernels-at-avx512-sde-skx`, `...direct...`).

## A hosted AVX-512 runner: the Wolfe search at the rounding floor of f

Push run 40662109393 (`7e230e22`), job win-clang-cl-shipping: `v7-f nonlinear CG determinism moat` failed with
`LineSearchFailed` at its first worker count. Hosted Windows runners are drawn from AVX-512 and non-AVX-512 hosts, so
this job was the first to run the dense avx512 dot shape outside SDE, and a lane passes or fails by its runner. It
reproduces on win-debug under SDE skx at avx512 and nowhere else.

- Not a kernel defect. The SELL apply is bit-exact with the scalar CSR at avx512, and the dot stays within its bound.
  The declared avx512 dot rounds differently, so the trajectory differs from iteration 3 in the last bits. portable,
  avx2 and avx512 end at 385, 380 and 360 iterations, and portable stops at a gradient of 9.99e-8 against the 1e-7
  tolerance. All three sit at the edge of what f can resolve.
- The cause is in `WolfeLineSearch`. At iteration 360, with f near -3.07 and φ'(0) near -1.6e-13, every trial's f
  differs from the others by a few ulps. The zoom ranked the trials by f alone: a trial tied with the low end moved
  the high end, and 28 bisections shrank onto α = 0.603, where |φ'| was 1.68e-14 against a bound of 1.56e-14. The
  minimizer was near α = 0.54. A second defect: `φ(α) ≤ φ(0) + c1·α·φ'(0)` rounds back to φ(0) once the decrease is
  below an ulp, so it passed any step that tied φ(0), and a weak search could accept a step past the minimizer.
- Fix: Armijo is evaluated as the difference φ(α) − φ(0). A trial that is at most ε·|φ(0)| above both φ(0) and the
  point it is ranked against, with ε = 1e-6, is judged by φ'. It is accepted under the approximate Wolfe conditions
  of Hager and Zhang (2005, §4, as in CG_DESCENT). Otherwise the sign of φ' picks the side of the interval to keep.
  Trials f can rank take the textbook path unchanged. The search is shared by CG, gradient descent and the Newton
  paths.
- Tests: a 1-D search on a φ that rounds to 3 at every trial (strong and weak, three starting steps), and all four CG
  variants on the moat's quadratic plus 1e4, which reach the gradient tolerance at every level. The old search fails
  both. The whole hesap-opt suite (154 cases) passes on win-debug at portable, sse2 and avx2, and at avx512 under SDE
  skx.
- The jobs assertion that followed under SDE (`WorkerPool::init called twice`, momentum moat) was a consequence: the
  failed REQUIRE left the pool initialised.

## Board candidates (row close; the AVX-512 rows need AVX-512 hardware, SDE times are not data)

- The sse2 and avx2 tile now indexes register arrays through `unroll` (it held twelve named registers): one-thread
  GEMM, f64 and f32, `6210ab6f` against `d1a0cb1d` from one source, interleaved, MSVC first. MSVC once kept such
  arrays in memory (`gemv_rows`). A loss there is this change, not the 14-row shape or the Mc rounding.
- 14 against 12 rows a tile (12 leaves room for a second A broadcast), and the two-line B prefetch distance.
- Eight gemv rows against 16, and two split accumulators against four, on 32 registers.
- direct's 16-column reduction group against 8.
- ZMM frequency licences on the target CPU against the avx2 bodies. A zmm body stays only where it beats avx2.
