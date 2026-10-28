# 2026-10-28 — SIMD-DISPATCH: the scalar crd::math surface moves into the level namespace

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `4aaa3b77`.

## CI first

No completed run among the last 15 has a failed job: every one is a push run the concurrency group cancelled before
any job ran. Push run 40701630725 (`3c1debc6`) is still running; its seven failed jobs have causes fixed at HEAD, as
the [batched note](2026-10-28-simd-dispatch-tensor-batched-kernels.md#ci-first) records. Its linux-gcc-asan Test step
had run 3 h 23 min when read (1 h 54 min on the last green run, about 3 h on the two failed ones), inside the job's
300 min limit; the next batch reads its result first. No new green run, so DIAG.11c and DIAG.12 stay Needs CI.

## Why this comes first

The jets are the last general-code SIMD body before the flip. A jet's value chain calls `crd::math::sincos`, `exp`,
`log`, `tan`, `tanh`, `pow` and `sqrt`; those were inline functions in plain `crd::math` over `<cmath>`, so a kernel
unit could not call them (ADR-0134 §5: an inline body compiled at a higher level shares its symbol with general
code's). This batch makes them callable from every kernel unit, the precedent being `exp_log_core.hpp`. The jets'
own boundary follows in the next batch.

## What changed

- `select.hpp`, `transcendental.hpp`, `trig.hpp`, `power.hpp` and `hyperbolic.hpp` now live in
  `crd::math::inline CRD_MATH_LEVEL_NS`. Callers are unchanged: the inline namespace keeps every `crd::math::<fn>`
  spelling. They include `crd/math/libm.hpp` only, no longer `<cmath>`, `<bit>`, `<limits>`, `<cstdint>` or `<cstring>`.
- `libm.hpp` gains the C library functions those bodies use: `floor`, `ceil`, `round`, `trunc`, `lround` and `fmod` in
  both precisions, and in f64 the edge results of the families (`exp2`, `expm1`, `log2`, `log10`, `log1p`, `cbrt`, the
  trig, inverse trig, hyperbolic and inverse hyperbolic functions, `atan2`, `pow`, `hypot`, `ldexp`). They are compiler
  builtins on GCC, Clang and clang-cl, and the UCRT exports on MSVC; the float names are the `f` exports, never
  `<math.h>`'s inline C++ overloads. `fabs`, `copysign` and `is_inf` are bit operations, like `is_nan`.
- `std::bit_cast`, `std::memcpy` and `std::numeric_limits<T>::min()` became `__builtin_bit_cast` and the hex
  constants `0x1p-1022` and `0x1p-126F`: under `/Od` the standard ones are out-of-line inline functions, the same
  defect class.
- `trig.hpp`'s helper namespace is `trig_detail`: as `crd::math::<level>::detail` it would have made every
  `crd::math::detail::` name in `format.hpp` ambiguous. Strict tidy on the touched headers named the π/2 split
  constants: `kPio2_1` to `kPio2_4` are `kPio2Part1` to `kPio2Part4`.

Results do not change on any level: each body runs the same operations, the classifications are the IEEE bit tests
and the library edges are the same functions.

## Tests

The SIMD-type probe (`tests/foundation/math/kernels/simd_level_probe.cpp`, one unit per level) runs 54 scalar
functions in both precisions: every exp, log, trig, inverse trig, hyperbolic and power function, `sincos`, the
rounding, `lround`, `fmod`, `remainder`, sign, min/max, `fma`, `lerp`, `frexp` and `modf`. Each runs on the probe's
inputs (signed zeros, subnormals, infinities, NaNs with payloads, the exp overflow edges, random bit patterns), on a
value in [-1, 1) built from each input's mantissa and on the input scaled by 2^-40 (2^-20 in f32); every level must
give portable's results, NaNs as a class. Local: win-debug `[simd-levels]` passes natively (portable, sse2, sse4.2,
avx2) and under SDE 10.13.1 `-skx` (avx512), with the whole crd-math suite; the whole win-debug tree builds keep-going
with no error or warning; strict tidy on the changed files and the ctest guards pass.

## Next

The jets' per-level boundary (`jet_simd.hpp` and the forward drivers into the level namespace, an autodiff kernel
family for the engine's own functors, ADR-0134 §5), then the flip.
