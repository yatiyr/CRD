# 2026-10-31 — SIMD-DISPATCH: the Student-t sum and the scalar transcendental table

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (the sse2 boards' findings). Before:
[the sse2 and sse4.2 boards](2026-10-31-simd-dispatch-sse2-boards.md). Evidence:
[sse2 boards, the Student-t attempt](../bench/2026-10-31-simd-dispatch-sse2-boards.md#the-student-t-attempt) and
[the scalar table](../bench/2026-10-31-simd-dispatch-sse2-boards.md#the-scalar-table).

## CI first

- No completed run has a failed job. Scheduled run 41188120176 is queued; it tests GitHub's copy of main, still
  `d9fdeae3`: the GitLab push mirror has carried nothing since then, so no later commit has a hosted run. Nothing was
  cancelled or dispatched.
- Needs CI: DIAG.11c and DIAG.12 still wait on a green complete-tier run. Repository variables `FTL_GCP_PROJECT`,
  `FTL_WIF_PROVIDER` and `FTL_SERVICE_ACCOUNT` exist (names only), no secrets; the Firebase lane is wired in `ci/`,
  the AMD and Mac lanes wait on their accounts.

## The attempt

The Left list named the Student-t sum's cause as `e0f539cb`'s second division in the log reduction and its fix as one
reciprocal for the quotient pair. The fix ran in the scalar core and the SSE2, AVX2 and AVX-512 twins: `ru = 1/u`,
`t = f·ru`, `tl` the FMA remainder times `ru`.

- **Accuracy.** A Python model of `crd_log1` (the same operations through `math.fma`, against 300-bit mpmath, 74,424
  points: the seams at √2, √½ and 1 at exponents 0, ±1, ±1000 and -1022, subnormals and random points) measured
  0.511 ulp against 0.516 for two divisions, `t + tl` within 2^-103.3 of the quotient; 15 results differ. The C++
  accuracy gates passed on win-debug and linux-gcc-release; only the golden checksums moved.
- **Board** (five interleaved rounds, one core, busy host: 14.4 of 63.7 GiB available, other processes 1.2 cores):
  before over after 1.005 [1.00-1.02] at avx2 and 0.991 [0.986-0.993] at sse2. No win, so it was reverted.

## The cause

The two loops' disassemblies: pre-row's sum is inlined into `main`, about 45 operations and two divisions per four
elements; HEAD's avx2 unit runs the same loop with about 65 operations and three divisions. With the divisions
equalised nothing moved, so the cost is the work `e0f539cb` added to take log from 7 ulp to 0.52: `m + 1`'s exact
error, the quotient's remainder, the carried term and the exact head sum. Pre-row over HEAD stays 0.766
[0.758-0.773], its spread within a round about 3%. Accepted under the user's 2026-10-30 board rule ("if we are not
losing too much ground ... it is fine since we got healthier and stronger and more correct"); a second log core for
reductions would duplicate the one log, and a table-driven log is a redesign, not one focused attempt. The sum stays
2.2x ahead of JAX.

Also measured: the sse2 Student-t body runs 49.5 ms against 1.19 at avx2 (the exact FMA emulation of the sse2
twin), the body judged against `portable@sse2` in the previous board.

## The scalar table

The Gamma O(1) gradient ran at 0.122x of pre-row: general code builds at the baseline ISA, so every `libm::fma` of the
scalar log (and of every scalar transcendental) is a call into the C library. A `perf` profile of HEAD's bench put
`lgamma_pos` (the distribution's constructor), `digamma`, glibc's `__fma_fma3` and an out-of-line `libm::fma` wrapper
on top. A probe (the same log and digamma from one baseline unit, one thread) measured the dispatch shapes: the
inline baseline body 20.1 ns, a call through a pointer or an out-of-line accessor and its table 13.1 to 13.3, a direct
call into an avx2 unit 13.0, everything compiled at avx2 (the pre-row shape) 9.0: a call costs about 2 ns.

- **`crd/math/scalar_kernels.hpp`**: `ScalarMathKernels`, the 23 f64 routines of the exp/log, trig, power and
  hyperbolic families whose bodies run fma, built at portable and avx2 (`kernels/scalar_kernels.cpp`, each entry the
  family's own inline body at the unit's level), resolved once (`src/scalar_dispatch.cpp`, the function-local static
  pattern of the other families). General code of an x64 build without FMA (`CRD_MATH_SCALAR_DISPATCH`,
  `kernel_level.hpp`) gets one-line dispatchers in place of the bodies, f32 overloads rounding the f64 routine as the
  bodies do; kernel units and ARM64 general code keep the inline bodies. cbrt, hypot, rsqrt and the select family run
  no fma and stay inline.
- The Lanczos core and `src/lanczos.cpp` called `crd_log1` and `crd_exp1` directly from general code; they call
  `crd::math::log` and `exp` now (the same bits; a kernel unit's `log` is `crd_log1`).
- ADR-0134 §5 amended with the measurement.

Checks: the golden checksums did not move; `crd-math-tests` (three new `[kernels]` cases: the levels the archive
holds, every level against portable bit for bit, NaN payloads aside, over every mpmath grid, 20 specials and random
doubles, and the selected table), `crd-hesap-special-tests` and `crd-hesap-stats-tests` pass at the detected level,
portable, sse2, sse4.2 and avx2 on win-debug and linux-gcc-release, and the avx512 arms under SDE; the whole win-debug
tree builds (the headers reach most units) and the 19 repository guards pass, `crd-check-isa-isolation` again on that
tree; strict tidy on the changed files, clang-format and Allman. No namespace-scope initializer calls a scalar
transcendental, so no table resolves during static initialization.

Board (five interleaved rounds, `bench_logpdf_grad`, one core, busy host: 13.2 of 63.7 GiB available, other
processes 0.7 cores), binaries `c1e226c5` (pre-row), `722437e0` (HEAD) and `60fbb6f1` (the table):

- Gamma at avx2: HEAD over the table 3.45x [3.39-3.47] (88-96 ns to 25.5-28.1); pre-row over the table 0.414
  [0.375-0.426]. At sse2 (glibc without FMA, the integer fma body) 1.012 [0.955-1.121]: no cost on an SSE2 machine.
- Normal and Student-t unchanged (0.993, 1.002).
- The remaining 16 ns: four logs a call (`lgamma_pos`'s two in the constructor, `log(scale)` and `digamma`'s), each
  a call of about 2 ns against pre-row's inline body, and the 0.52 ulp log that replaced the 7 ulp one (the Student-t
  cost).
  The table's log now takes 35% of the bench's samples, the C library's fma 1.6%. Accepted under the 2026-10-30 rule:
  the call is the SSE2 floor's price (one indirect call per routine, the user's 2026-10-26 decision) and the rest is
  the correctness fix.

## Left

The row's Left list keeps the cnn rows' 11% to 16% and nn's gelu, tanh and sigmoid spans (general code; each element
now a table call, the span belongs in the nn unit as layernorm moved), then the jobs-retune thread sweep and fft f32
against MKL.
