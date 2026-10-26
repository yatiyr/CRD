# SIMD-DISPATCH: level-aware SIMD types

<!-- doc-role: historical -->
> Dated evidence. Live owner: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Contract:
> [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md). Previous:
> [kernel units](2026-10-26-simd-dispatch-kernel-units.md). Rules: [AGENTS](../../AGENTS.md).

## CI read

- The completed runs 40341016749 to 40390536674 are cancelled with no failed job. The one failed job of the
  in-progress run 40351178841 (win-clang-cl, head `e4c56fb8`, `test_diag_flight_rings.cpp:324`) is the post-window
  flight-ring case that `13f8e651`, an ancestor of HEAD, repaired
  ([races](2026-10-26-ci-audit-and-flight-ring-races.md)).
- Needs CI audit: no complete-tier run has finished since 40307186916; DIAG.11c and DIAG.12 stay as that audit left
  them. Push run 40398746887 was pending, so nothing was dispatched.

## Mechanism

- `crd/math/simd/backend.hpp` keys the backend on the unit's CPU level: a kernel unit's `CRD_KERNEL_LEVEL` (portable
  scalar, sse2, sse4.2, avx2 and avx512 on the 256-bit backend, neon), general code's `CRD_SIMD_TARGET` otherwise. The
  rungs `CRD_SIMD_HAS_SSE2/SSE42/AVX2/NEON` and `CRD_SIMD_IS_SCALAR` replace `CRD_SIMD_BACKEND` and its constants;
  the hesap, test and bench sites read the rungs.
- Every type and function of `crd/math/simd/*.hpp`, `exp_log_core.hpp` and `simd/transcendental.hpp` lives in
  `crd::math::simd::inline CRD_MATH_LEVEL_NS` (`crd::math::inline ...` for the scalar cores); `deterministic.hpp`
  declares `Vec4f`/`Vec8f` there. They call no inline function of the standard library: `crd/math/libm.hpp` wraps
  sqrt, fma, floor and log as compiler builtins (GCC, Clang, clang-cl) or the CRT's external functions (MSVC), the
  bit casts are the builtin, NaN and finiteness tests read the bits. `<cmath>`'s float overloads and
  `numeric_limits` are inline bodies that an AVX2 unit would compile under general code's symbol.
- The SSE4.2 backend: `select` is one `blendv` (also on AVX2 for `Vec8f` and `Vec4d`), `truncate` and
  `round_nearest` are one `roundps`.
- `kNativeLaneWidth` is 8 on every level and build (it was 4 below AVX2): the AoSoA layout is data. The eylem 4-lane
  body and collider chunks are gone.
- `crd_add_cpu_kernels` takes `INCLUDES <target>...`, whose interface include directories the units see (a test or
  hesap family includes crd-math's headers).

## Cross-level defects the probe exposed, fixed

| Operation | Before | Now |
|---|---|---|
| `Vec8f::fnmadd` below AVX2 | `c - a*b`, two roundings; AVX2 fuses (254 of 4,096 words differ) | per-lane fused |
| `round_nearest`, `truncate` | i32 round trip: -2^31 from 2^31 and for NaN, +0 for -0.3 | IEEE roundToIntegral |
| `Vec4f::fma` on AVX2 | per-lane library call | `vfmadd` (same bits) |
| unary minus | `0 - a`: +0 for +0, NaN quieted | sign-bit flip, as scalar `-x` |
| scalar `Vec4d::abs` | `a < 0 ? -a : a`: -0 stays | sign bit cleared |
| scalar `Vec4i` +/-, `convert_truncate` | signed overflow, out-of-range casts (undefined) | wrap; INT32_MIN |

The last row has no tooth on x86: MSVC and GCC already emit the wrapping and indefinite-result instructions.

## Verification

`tests/foundation/math/kernels/simd_level_probe.cpp` is compiled per level (portable, sse2, sse4.2, avx2, avx512;
neon on ARM64) into crd-math-tests. Its table runs 94 operations of `Vec4f`, `Vec8f`, `Vec4d`, `Vec4i`, `Vec8i`,
`Mat4f`, `Quatf` and the f64 exp/log cores over 64 input sets (signed zeros, rounding boundaries, the i32 range,
subnormals, extremes, infinities, quiet and signalling NaNs, random patterns and values). `[simd-levels]`: each
level's backend name and lane width, and every level the host executes against the portable level bit for bit (NaN
payloads aside for two-input arithmetic). Three `[simd]` cases prove the IEEE semantics in general code (against
`nearbyint`/`trunc`, the INT32_MIN lanes, negation and single-rounded `fnmadd`).

| Tooth (win-debug) | Failed |
|---|---|
| sse4.2 level given the SSE2 backend | the backend case at sse4.2 |
| SSE4.1 blend operands swapped | select at sse4.2 and avx2, the `[simd]` select case |
| `fnmadd` back to `c - a*b` | fnmadd at avx2 |
| SSE2 `round_nearest` without the 2^23 and sign handling | round_nearest at sse2 |
| lane width back to 4 below AVX2 | the layout case at portable, sse2, sse4.2 |
| AVX2 `Vec4f::fma` as mul + add | fma at avx2 |
| scalar round without the input's sign | truncate and round at sse2, sse4.2 and avx2 |
| SSE negation as `0 - a` | negate at sse2, sse4.2 and avx2 (Vec4f), the `[simd]` negation case |

On linux-gcc-debug the blend tooth failed the same cases; after each restore the lane was rebuilt and passed.
Symbols: the avx2 and sse4.2 probe objects define nothing outside their level namespace but the table (MSVC
`llvm-nm` adds only constant-pool data, GCC `nm` only `DW.ref.__gxx_personality_v0`). The touched suites (math, eylem,
eylem-rigid3d, geometry bvh/bvh-gpu/primitives/mesh, hesap dense/fft/special/stats/tensor/tensor-nn/tensor-batched/
sparse/autodiff/direct) pass on win-debug, a subset on linux-gcc-debug; the ctest guards, strict tidy (the touched
headers' older findings fixed: naming, redundant `inline`, forwarding references, an unused macro, a confusable name),
clang-format against HEAD, Allman and the standalone header check pass.

## Left in the row

The hesap and geometry kernels as per-level units (their raw `__AVX2__` tests in philox, threefry, dtypes, permute and
reduce go with them); eylem's integrators as kernels before the baseline flip, since its chunks now hold `Vec8f`
columns that general code runs at the baseline; SSE2/SSE4.2 bodies where they beat the portable loop (a `Vec4d` SSE2
backend, the conversions) and AVX-512 bodies under SDE; NEON parity with the x86 semantics (no ARM64 lane compiles
it); the baseline flip with the `-sse2` lanes as forced arms; the isolation guard; the level in the doctor, host
tuple and board records; the boards per level.
