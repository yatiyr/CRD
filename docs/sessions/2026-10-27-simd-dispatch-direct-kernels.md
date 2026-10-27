# 2026-10-27 — SIMD-DISPATCH: hesap-direct's supernodal solve loops as a kernel family

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).

## CI read

Run 40515770381 (`31e2b9ce`, complete tier): linux-gcc-asan stopped at link with `No space left on device` (73 MB
free). Every test program links each kernel family once per CPU level, and the ASan debug tree holds 11 GB of test
programs on the WSL mirror (code and the sanitizers' per-site data; compressing the debug sections saves 14%). Both
Linux jobs now drop the image's preinstalled toolchains no step uses (Android, .NET, GHC, Swift, CodeQL) before
anything else and print the free space before and after.
Needs CI audit: DIAG.11c and DIAG.12 gained no hosted evidence on this read (the tsan arm DIAG.12 needs is in
push run 40524892470 at `671e841f`, still pending).

## The entries

`DirectTypeKernels<T>` (f64, f32) in `crd/hesap/direct/direct_kernels.hpp`, built portable, sse2 and avx2 from
`kernels/direct_kernels.cpp`, one call per supernode, descendant or RHS column:

| Entry | Replaces |
|---|---|
| `forward_below` (dense `GemvtRows`, gemvt's bits, four panel columns a pass) | `solve_fwd_below_acc`'s 4-column groups |
| `forward_diagonal` | `solve_fwd_diag`, `solve_fwd_apply_minus` and the two narrow per-RHS forward loops |
| `block_forward_below`, `block_backward_below` | `solve_mrhs_fwd_below`, `solve_mrhs_back_below` (256-row blocks kept) |
| `block_forward_diagonal`, `block_backward_diagonal` | `solve_mrhs_fwd_diag`, `solve_mrhs_back_diag` |

The single-RHS backward runs dense entries: the below-update is gemv on the panel columns read as rows with
alpha -1 and beta 1 (the bits of x - s), the diagonal solve is the upper trsv on the same view; the narrow per-RHS
backward loop calls it per column. `solve_dot4_f64`, `solve_fma1`, `solve_axpy_minus`, `solve_acc_plus`, the
`vec4d.hpp` include, the empty `if (nrhs > 1)` block and the stale kGemmKc comments are deleted. The vector steps
(`VectorOf`, `madd`, `unroll`, `load_n`, `store_n`, `for_each_vector`, `axpy`) moved unchanged from dense's kernel
unit to `crd/hesap/dense/detail/vector_steps.hpp`, which both units include. Complex keeps its scalar loops.
clang-tidy flagged the split-complex batch's local `kA` in dense's unit; it reads `kSplitAccumulators` now.

## Arithmetic and bits against HEAD

`direct_kernels.hpp` declares it. Every element's chain depends on the column count only, never on the rows or their
offset, so the serial right-looking and level-parallel left-looking solves still agree for any worker count.
- Below avx2 f64 changes: HEAD's backward and multi-RHS paths ran `simd::fma`, a per-lane libm fma there.
- avx2 f64: the forward now fuses (HEAD rounded twice), tails take the level's step; the single-RHS backward takes
  gemv's eight-row and trsv's dot shapes instead of HEAD's two-accumulator 4-column dots.
- f32 runs vector bodies (it was scalar); complex narrow backward now sums its dot before subtracting, as its
  single-RHS path does.

## Tests

`test_direct_kernels.cpp` (`[kernels]`, forced per level by `crd-hesap-direct-kernels-at-<level>`) models every entry
on each level the host runs, f64 and f32: `forward_below` against the model and dense gemvt of the same level,
`block_forward_diagonal` against `forward_diagonal` column by column, the gemv's alpha -1 beta 1 against y - s with
signed zeros. Row counts cross a vector, the 256-row block (256, 257, 600), nc 0 to 9, 13, 17 and 50, nrhs up to 17;
NaN fills the upper triangle, padding and unread scratch, sentinels end every output, and a second pass draws
infinities, signed zeros and subnormals. The seven solve and moat cases of `test_supernodal.cpp` are tagged
`[kernels]`. On win-debug: `[kernels]` passes (124,666 assertions), the whole direct program (195 cases) and dense
`[kernels]` too. The Linux lanes and forced arms run on hosted CI.

The scout brief was right on the sites and shapes. Corrections: its stale `__AVX2__` clause still holds for fft
(`fft.hpp`, `small_n_codelets.hpp`), so the row keeps it there; the backward diagonal solve needs no new shape, as
dense trsv on the panel is exactly L^T.

## Row-close boards

Board the solve single- and 16-RHS (bench_hesap_cholesky_vs_cholmod, _vs_reference) per level against HEAD, and the
trsv backward diagonal against HEAD's 4-column far dots; gemvt against `forward_below` on the forward.

## CI fix: MSVC LTCG crash

Complete run 40515770381 (`31e2b9ce`) failed win-release, win-shipping and win-shipping-profile at the link of
`crd-hesap-direct-tests`: LTCG code generation crashed (C1001, `p2/main.cpp` 262) at `merge_element`'s `c = p`
(`detail/gemm_block.hpp`). It reproduces at HEAD on the local win-release tree. Bisected by relinking with one swapped
object: a non-LTCG `supernodal_cholesky.cpp` links; dropping its `Complex<f32>` instantiations links, `Complex<f64>`
does not matter; forcing every block to one merge crashes only for Assign (the row-block solve's assign block, the
only complex Assign GEMM block in the tree). Taking `p` and `alpha` by const reference instead of by value links; the
release direct program then passes (195 cases). The same expression, so the same bits.
