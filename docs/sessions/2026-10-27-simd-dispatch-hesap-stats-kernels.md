# SIMD-DISPATCH: the hesap-stats kernel family

<!-- doc-role: historical -->
> Dated evidence. Live owner: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Contract:
> [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md). Previous:
> [level-aware types](2026-10-27-simd-dispatch-level-aware-types.md). Rules: [AGENTS](../../AGENTS.md).

## CI read

- The completed runs 40341016749 to 40398746887 are cancelled with no failed job. The one failed job of the
  in-progress run 40351178841 (win-clang-cl, head `e4c56fb8`, `test_diag_flight_rings.cpp:324`) is the flight-ring
  case `13f8e651` repaired ([races](2026-10-26-ci-audit-and-flight-ring-races.md)); that run's win-shipping job has
  been in progress for more than six hours and is left running. Push run 40409214033 is pending.
- Needs CI audit: no complete-tier run has finished since 40307186916, so DIAG.11c and DIAG.12 stay as audited.
  Nothing was dispatched while two runs are open.

## Mechanism

- `crd_add_cpu_kernels(crd-hesap-stats NAME stats LEVELS portable avx2 INCLUDES crd-math ...)` builds
  `engine/numerics/hesap-stats/kernels/stats_kernels.cpp` per level; `src/stats_kernel_dispatch.cpp` resolves the
  `StatsKernels` table (`crd/hesap/stats/stats_kernels.hpp`, `stats_dispatch.hpp`). Three routines, every one
  bit-exact across levels: the Philox and Threefry whole-block fills and the Student-t score reduction in nu.
- `counter_rng_core.hpp` holds `philox4x32`, `threefry4x64` and the 256-bit `threefry_counter_add` in
  `CRD_MATH_LEVEL_NS`; `philox_blocks.hpp` holds the block loop (`philox4x32_fill_blocks`, eight blocks per AVX2
  step). `PhiloxRng::fill` and `ThreefryRng::fill` drain the buffered block, call the kernel for whole blocks and draw
  the tail; philox.hpp, threefry.hpp and log_density_grad.hpp no longer test `__AVX2__` or include intrinsics (two
  include-budget entries hold the RNG headers).
- crd-hesap-tensor consumes Philox include-only (ADR-0096 §1, amended in place): `dtypes.hpp` SR draws and the
  `decomp.hpp` sketch fill call the block loop, so the substrate never links hesap-stats; their own per-level unit
  comes with the tensor family. crd-hesap-stats now declares and links crd-math.
- `crd_add_kernel_level_tests(<exe> <filter>)` (cmake/CrdSimd.cmake) replaces the math tests' loop:
  `crd-math-kernels-at-<level>` and the new `crd-hesap-stats-kernels-at-<level>`.
- AVX2 bodies: Philox takes both product halves from two `vpmuludq` (no `vpmulld`) and computes the lane positions
  in registers; Threefry runs its 20 rounds with compile-time rotations (byte and dword shuffles for 16, 32 and 40)
  and keeps one broadcast counter unless a lane carries out of word 0.
- Student-t: both bodies sum `fma(-1/2, log(fma(x^2, 1/nu, 1)), k x^2 / (nu + x^2))` in four lanes in element
  order, then the remainder, with `crd_log1`/`crd_log4` (lane-identical). The `1 + x^2/nu` rounding is about one ulp
  of the term's absolute size, below the rounding of the `n * cst` term that grad[0] adds.

## Defects fixed

| Defect | Effect | Now |
|---|---|---|
| `ThreefryRng::seek` left the counter on the sought block | draws past that block repeated it | the counter names the next block |
| Threefry AVX2 chunk ignored the carry out of word 0 | wrong blocks across 2^64 | per-lane counters for that chunk |
| Student-t: AVX2 body `log(fma)`, tail `log1p(x^2/nu)` | the result depended on `n mod 4` and the build | one formulation on every level |
| Threefry carry stopped at word 2 | not Random123's 256-bit counter | carries through every word |

Touched-header findings fixed: `ThreefryRng`'s key initialiser, an unforwarded `BatchFn&&` in dtypes.hpp, and
`BlockQ8_0`/`BlockQ4_0` renamed `BlockQ8Zero`/`BlockQ4Zero` (naming rule).

## Verification

`test_stats_kernels.cpp` (`[stats][kernels]`): built levels, every level the host runs against the scalar block
functions (block counts 0 to 37, Philox positions across the 2^32 and 2^64 wraps, Threefry counters across the word
0, 1 and 2 carries, a guard word past the output), the Student-t bits against portable (signed zeros, 1e-160 to 1e140,
five nu, n 0 to 41 and 202 from an odd offset), and the resolved level. `test_rng.cpp`: the fill test is
`[kernels]` and checks every length 0 to 40 after 0 to 8 partial draws across the Philox 2^32 wrap; a new case checks
seek then draws and fills against the sequential stream.

| Tooth (win-debug) | Failed |
|---|---|
| Philox multiplier m0 changed (AVX2) | blocks differential, fill cases |
| Philox high position word broadcast (lane carry dropped) | blocks differential, fill across 2^32 |
| Threefry first rotation takes the second constant | blocks differential, fill and seek cases |
| byte rotation reversed | blocks differential, fill and seek cases |
| fast path taken for a chunk that carries | blocks differential (carry counters) |
| per-lane carry dropped | blocks differential |
| Student-t `fma` split into mul and add | Student-t differential |
| table resolved from `detected` | resolved-level case at `CRD_CPU_LEVEL=sse2` |
| seek fix reverted | seek case |
| odd Philox tail skipped | fill cases |

linux-gcc-debug: the multiplier, per-lane carry and Student-t teeth failed the same cases; each restore was rebuilt
and passed. Both lanes pass the stats, tensor, tensor-nn, decomp and cli suites, `smoke_hesap_tensor` (the ADR-0096
isolation gate), the forced arms and the ctest guards; strict tidy, clang-format against HEAD, Allman and the
standalone header check (MSVC and GCC) are clean.

## Board

linux-gcc-release, `taskset -c 4`, nine interleaved rounds (medians), host idle; HEAD binaries compile the same bench
sources with HEAD's three headers first and link the same archives (SHA prefixes: bench_rng `b4c2be36` HEAD,
`6d39c780` new; bench_logpdf_grad `debc459b`, `d78a8fd1`).

| Routine | HEAD | avx2 | portable | Peers (same host) |
|---|---|---|---|---|
| Philox fill, ns/u64 | 2.107 | 1.197 | 2.476 | NumPy 2.4.6 Philox 2.483; MATLAB R2026a philox 11.85 ns/double |
| Threefry fill, ns/u64 | 2.123 | 1.147 | 5.209 | MATLAB threefry 7.02 ns/double |
| Student-t score, 1M points, ms | 0.951 | 0.951 | 17.93 | JAX 0.10.2 jit 2.41; PyTorch 2.12.1 22.44 |

The portable column is what an SSE2-only CPU runs today: Philox is at NumPy parity, the Student-t log core pays a
software `fma` per step (the ADR-0063 single-rounded cores below FMA hardware) and loses to JAX by 7.4x.

## Left in the row

SSE2 bodies for this family (a two-lane exact `fma` for the log core, four-lane Philox, two-lane Threefry); the
tensor (dtypes SR draws and sketches, nn, permute, reduce), fft, dense, sparse, special and geometry kernel families;
eylem's integrators before the flip; the rest as the row lists.
