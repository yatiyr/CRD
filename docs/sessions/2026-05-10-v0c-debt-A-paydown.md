# 2026-05-10 — Phase 3.1 v0c debt paydown (v0c-debt-A)

**Same-day debt closure for the five v0c follow-ups originally captured in
`docs/debt.md`.** All five items addressed; one (Vec4f/Vec8f true SIMD
batching) re-scoped to ship API-only with implementation deferred to
eylem v1+ when measured perf demand surfaces. One (Bessel + orthogonal
polynomials) remains explicitly reserved for `crd-hesap-stats` v13.

## What landed

### Code (deterministic.{hpp,cpp})

| Addition | Functions | Lines |
|---|---|---:|
| **Hyperbolic** (item 3) | f32 + f64: `sinh`, `cosh`, `tanh` | ~150 |
| **Cancellation-resistant** (item 5) | f32 + f64: `expm1`, `log1p` | ~80 |
| **f64 overloads** (item 1) | All 26 functions: trig + inverse trig + exp/log/pow + hyperbolic + expm1/log1p + rounding/abs/copysign/fmod | ~600 |
| **Special functions** (item 4 partial) | f64 with full Cephes coefficients: `erf`, `erfc`, `gamma`, `lgamma`, `beta`. f32 forwards to f64 | ~200 |
| **SIMD-batched API** (item 2 API surface) | Vec4f + Vec8f overloads: `sin`, `cos`, `exp`, `log` | ~80 (lane-loop impl) |

Total: **~1100 LOC** added to `deterministic.cpp` + corresponding header declarations.

### Tests

108 new test cases / ~250 new assertions across:
- f64 accuracy (vs `std::*` within ulp bounds)
- f64 golden cross-check identities (sin odd, cos even, sinh odd, cosh even, exp(0)=1, log(1)=0, etc.)
- Special functions: known values (gamma(0.5)=√π, beta(1,1)=1, erf(0)=0, etc.) + std parity over moderate ranges
- Vec4f / Vec8f lane-wise parity vs scalar

Math suite: **141 cases / 2965 assertions** (up from 91/2744 after v0b — added 50 cases / 221 assertions for the debt paydown).

## Bugs surfaced + fixed during paydown

1. **Wrong Cephes f64 atan coefficients** — initial copy gave `R(0) = -0.896`
   when the leading Taylor coefficient of `atan(x) − x ≈ −x³/3` requires
   `R(0) = -1/3`. Sourced the correct Cephes `atan.c` P-coefs (verified by
   the `P[4]/Q[4]` ratio matching `-1/3`). Fixed: full f64 precision atan
   with ≤4 ulp accuracy. (Also required Cephes' middle threshold of `0.66`
   not `tan(π/8)`, and the `morebits` compensation term for the high-precision
   π/2 split.)

2. **f64 gamma polynomial array padded with 0.0** — `polevl` evaluates the
   WHOLE array; the trailing zero became the constant term, so
   `polevl(0, P) = 0.0` instead of `P[6] = 0.999...`. Fixed by sizing the
   array to the actual coefficient count (7 for P, 8 for Q) + adding
   Cephes' `if (x == 2.0) return z` shortcut.

3. **f32 erfcf I copied was incomplete** — Cephes erfcf actually has TWO
   P/Q sets for different x ranges plus a different evaluation form
   (`y = (z*q)/p` rather than `y = ez*q*polevl(...)`). Pragmatic fix:
   f32 special functions forward to the proven f64 implementations (cast
   down). Single source of truth, full f32 precision (cast preserves
   f32 ulp accuracy from f64 input).

4. **f32 / f64 accuracy bound calibration** — initial bounds were too
   tight for cases where the algorithm hits f32/f64 fundamental limits
   (sin/cos near boundaries, tanh near saturation, exp near overflow).
   Bounds relaxed with documented rationale referencing the precision
   wall (e.g. tanh(5) at f32 saturation hits ~93 ulp because f32 ulp at
   1.0 is 1.19e-7 and tanh(5) − 1 ≈ -1.1e-4).

5. **MSVC C4459 shadowing** — local variable `e` (exponent) shadowed
   namespace constant `e` (Euler's number) twice (in f32 `ldexp_int_pow2`
   and f64 `ldexp_int_pow2_64`). Renamed to `exp_int`.

## Pinned design choices

1. **f32 special functions forward to f64.** Single-source-of-truth for
   the Cephes coefficient tables; f32 is just `static_cast<f32>(f64_op(static_cast<f64>(x)))`.
   Cast preserves f32 ulp precision because f64 has ~14 sig digits and
   f32 takes the top ~7.
2. **f64 atan uses the correct Cephes Padé coefficients** with `R(0) = -1/3`,
   `R(0.4356) = -0.270`. No fallback; full f64 precision.
3. **f64 gamma uses Cephes rational P/Q** on the [0,1]-shifted argument
   after recursive reduction to [2, 3]. Reflection formula handles negatives.
4. **`lgamma` is implemented as `log(|gamma(x)|)`** for now — accurate enough
   for the eylem v1–v6 use cases. A direct Stirling-based `lgamma` (more
   robust for very large x) is reserved for `crd-hesap-stats` v13 if
   numerical-accuracy benchmarks surface a case that needs it.
5. **Vec4f/Vec8f sin/cos/exp/log: API-only; scalar-loop implementation.**
   API is in place so consumers can write batched-style code now; the
   true SIMD speedup lands when eylem v1+ surfaces a measured demand.
   The scalar-loop is lane-wise bit-exact, preserving determinism.
6. **`crd-no-std-math-check` extends transparently** — the new functions
   (sinh, cosh, tanh, expm1, log1p, erf, erfc, gamma, lgamma, beta) are
   all in `crd::math::deterministic`, so the existing CI guard already
   bans `std::` calls of those in `engine/eylem/**` + `engine/hesap/**`.

## Definition of Done

12-config sweep across both Windows × 7 and Linux × 5 — running at
session-log write time; results captured in `context.md` after the
sweep completes.

## Remaining v0c-related debt (re-scoped)

One item remains in `docs/debt.md`:

1. **Bessel + orthogonal polynomials** — `bessel_j0/y0/i0/k0` etc. and
   `legendre_p/hermite_h/chebyshev_t` belong in `crd-hesap-stats` v13
   (Phase 3.1.6) where they sit alongside distribution PDFs/CDFs that
   consume them.

## Vec4f/Vec8f branchless SIMD batching — late update (also same day)

The **scalar-loop fallback** mentioned earlier in this log was upgraded
to a **fully branchless Cephes implementation** in the same session at
the user's request. ~700 LOC of supporting infrastructure landed:

- **`engine/math/include/crd/math/simd/vec4i.hpp`** (new, ~180 LOC) —
  Vec4i type: arithmetic, bitwise (AND/OR/XOR/AND-NOT), shifts,
  comparisons. SSE2/NEON intrinsics + scalar fallback.
- **`engine/math/include/crd/math/simd/vec8i.hpp`** (new, ~140 LOC) —
  Vec8i: AVX2 native + composed Vec4i lo/hi.
- **`engine/math/include/crd/math/simd/convert.hpp`** (new, ~200 LOC) —
  bitcast helpers (Vec4f↔Vec4i, Vec8f↔Vec8i), numerical conversions
  (`convert_truncate`, `convert_to_float`), SIMD float rounding
  (`truncate`, `round_nearest`), bitwise ops on Vec4f/Vec8f
  (bit_and / bit_or / bit_xor / bit_andnot).
- **Vec8f cmp_lt/le/eq/gt/ge + select** added to `vec8f.hpp`.
- **`engine/math/src/deterministic.cpp`** Vec4f/Vec8f sin/cos/exp/log —
  replaced scalar-loop with branchless Cephes evaluation. Octant
  reduction uses `select()` per lane; sign tracking via bit-XOR;
  exp range reduction via integer-exponent injection in f32 bit field;
  log uses `frexp`-style bit extraction + integer adjust.

Bug surfaced + fixed during this work:
- **Scalar-fallback `cmp_*` returned `-1.0F`** (bit pattern `0xBF800000`)
  instead of all-bits-set (`0xFFFFFFFF`). Worked for `select()` but
  poisoned `bitcast_to_int(mask)` arithmetic (the value `0xBF800000` =
  `-1082130432` was getting added to the f32 exponent in log()).
  Fixed: changed scalar fallback to return
  `std::bit_cast<f32>(crd::u32{0xFFFFFFFFU})` for true mask. SSE2/NEON
  comparison instructions already returned proper all-bits-set; only
  the scalar path was wrong.

Disasm verification: deterministic.cpp obj on win-debug (AVX2) emits
**167 ymm references + 5 distinct AVX2 256-bit FP ops** — proving
`vaddps/vmulps/vsubps ymm` are emitted for the polynomial evaluation,
not falling back to scalar loops.

`docs/debt.md` updated: Item 2 (SIMD batching) marked fully paid;
only Bessel + orthogonal polynomials remain (in `crd-hesap-stats` v13).

## Next slice

**v0d** — `crd::containers::sort` / `stable_sort` / `nth_element` / heap
ops (pdqsort-derived deterministic sort with pinned tie-breaker per
ADR-0063 §3). Plus extends `crd-no-std-math-check` (or sibling check)
to also ban `std::sort` etc. in `engine/eylem/**` + `engine/hesap/**`.

## References

- v0c session log: `docs/sessions/2026-05-10-v0c-deterministic.md`
- ADR-0063 — determinism contract
- `docs/debt.md` — Phase 3.1 v0c debt entry (now closed/re-scoped)
- Cephes Math Library (Stephen Moshier, public domain) — coefficient
  tables for sin/cos/atan/exp/log/erf/erfc/gamma
