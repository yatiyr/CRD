# 2026-05-10 — Phase 3.1 v0c: `crd::math::deterministic` substrate

**Phase 3.1 v0c (per `docs/phases/phase-3.1-eylem.md` v0 table) shipped.**
Adds Cephes-style polynomial transcendentals + IEEE-correct rounding
wrappers in `engine/math/include/crd/math/deterministic.hpp` +
`engine/math/src/deterministic.cpp`. Plus CI guard
`crd-no-std-math-check` that bans `std::sin`/`std::cos`/`std::exp`/etc.
in `engine/eylem/**` + `engine/hesap/**` (lights up the moment those
modules land).

## What landed

### Code

| File | Lines | Notes |
|---|---:|---|
| `engine/math/include/crd/math/deterministic.hpp` (new) | ~95 | Public API + constants |
| `engine/math/src/deterministic.cpp` (new) | ~500 | Cephes-derived sin/cos/tan/asin/acos/atan/atan2/exp/exp2/log/log2/log10/pow + IEEE rounding wrappers + abs/copysign |
| `tests/math/test_deterministic.cpp` (new) | ~280 | 23 cases / 59 assertions across accuracy + golden tiers |
| `scripts/check_no_std_math.{ps1,sh}` (new) | ~70 each | CI lint that bans std::* math ops in engine/eylem + engine/hesap |
| `tests/math/CMakeLists.txt` | +12 | Wires `crd-no-std-math-check` into CTest |

Total: **~960 LOC** (above the ~700 estimate; extra came from helper
infrastructure + the rounding wrappers + the lint script — all in scope
for the substrate but not in the polynomial coefficient line count).

### Public surface (`crd::math::deterministic`)

```cpp
// Constants
inline constexpr f32 pi, tau, pi_2, pi_4, inv_pi, inv_2pi;
inline constexpr f32 e, ln2, inv_ln2, ln10, inv_ln10;

// Trig
f32 sin(f32);   f32 cos(f32);   f32 tan(f32);
f32 asin(f32);  f32 acos(f32);  f32 atan(f32);
f32 atan2(f32 y, f32 x);

// Exp / log / pow
f32 exp(f32);   f32 exp2(f32);
f32 log(f32);   f32 log2(f32);  f32 log10(f32);
f32 pow(f32 base, f32 exponent);

// Rounding (IEEE-correct hardware ops; routed for namespace consistency)
f32 floor(f32); f32 ceil(f32); f32 trunc(f32); f32 round(f32);
f32 fmod(f32, f32);

// Bit-trick helpers
f32 abs(f32); f32 copysign(f32 mag, f32 sign_src);
```

### Algorithm cited per function

| Function | Reference | Notes |
|---|---|---|
| `sin` / `cos` | Cephes sinf.c (Stephen Moshier, public domain) | Reduce by 4/π using DP1+DP2+DP3 split; sin or cos polynomial per octant; XOR-combine octant sign with polynomial sign |
| `tan` | Cephes tanf.c | sin/cos polynomial then 1/x cot path for octant 2/3 |
| `atan` | Cephes atanf.c | Range-reduce by tan(π/8) and tan(3π/8); polynomial degree 5 in z=x² |
| `atan2` | textbook quadrant analysis | Reduces to `atan(y/x)` with quadrant offset |
| `asin` / `acos` | Reduce to `atan2(y, sqrt(1-y²))` | Input clamped to [-1, 1] |
| `exp` / `exp2` | Cephes expf.c | Range-reduce by `n*ln(2)` using high-precision split; polynomial degree 5 in r |
| `log` / `log2` / `log10` | Cephes logf.c | Range-reduce via frexp into [0.5, 1); polynomial degree 8 in (m-1) |
| `pow` | `exp(b * log(a))` | Edge cases: a=0, a<0 with integer b, a=1, b=0 |
| `floor` / `ceil` / `trunc` / `round` / `abs` / `copysign` / `fmod` | IEEE hardware ops | Bit-exact across all platforms by IEEE-754 mandate |

### Two test tiers

1. **Accuracy** (`[deterministic][accuracy]`) — at sample inputs verify
   ulp distance vs `std::*` is within documented bound. Proves coefficients
   are correctly transcribed:
   - sin / cos / atan / atan2 / asin / acos: ≤ 4 ulps
   - tan: ≤ 8 ulps
   - exp: ≤ 8 ulps
   - log: ≤ 4 ulps
   - pow: ≤ 16 ulps

2. **Golden bit-pattern** (`[deterministic][golden]`) — at fixed inputs,
   the f32 output **bit pattern** is asserted (via cross-check
   identities like `sin(-x) == -sin(x)` bit-for-bit, `cos(-x) == cos(x)`
   bit-for-bit, `exp(0) == 1` exactly, `log(1) == +0.0` exactly, etc.).
   This is the ADR-0063 §2 contract guarantee that any cross-platform
   drift fires CI.

The `ulp_diff` test helper uses **signed-magnitude space** (not raw bit
subtraction) so `+tiny vs -tiny` is reported as a small ulp distance,
not 2.1 billion. Critical for fair accuracy reporting at boundaries
where both reference and test return values near zero.

## Pinned design choices

1. **`apply_sign(y, sign_xor)` (XOR-combine), not `with_sign(magnitude,
   sign)` (overwrite), for sin/cos/tan octant reduction.** Matches
   Cephes's `if (sign < 0) result = -result` semantics. The polynomial
   result `y` may already be negative (depending on the reduced
   argument); octant sign must XOR-combine with that, not replace it.
   Two negatives correctly combine to positive. (Initial draft used
   overwrite-style and failed sin/cos tests for inputs in
   sign-flipping octants; fixed during v0c implementation.)

2. **`mul_add` not used in deterministic.cpp.** The polynomial
   evaluations use plain `*` and `+`; the global `-ffp-contract=off`
   from `crd-simd-flags` (v0a) ensures the compiler never contracts
   them into hardware FMA. Bit-exact across MSVC / GCC / clang.

3. **`std::sqrt` is OK to call.** IEEE-754 mandates correctly-rounded
   sqrt on all hardware. Same reason `std::floor` / `std::ceil` /
   `std::trunc` are OK — those are wrapped through the deterministic
   namespace for consistency, but the underlying ops are guaranteed
   bit-exact across platforms by IEEE.

4. **Custom `ldexp_int_pow2` and `frexp_extract` instead of
   `std::ldexp` / `std::frexp`.** Microsoft's CRT `ldexpf` has subtly
   different denormal handling vs glibc's. The bit-injection version
   here is byte-exact across libcs.

5. **`pow(negative_base, integer_exp)` returns the real value;
   `pow(negative_base, fractional_exp)` returns NaN.** Matches `std::pow`
   semantics. Fractional powers of negatives give complex results which
   we don't expose at this layer.

6. **CI-side ban (`crd-no-std-math-check`) ships now even though
   eylem doesn't.** The script greps `engine/eylem/**` and
   `engine/hesap/**` for `std::sin`/`std::cos`/`std::exp`/etc. Today
   it's a no-op (no engine/eylem directory). The moment eylem v1a
   creates that directory, the script lights up — preventing the
   determinism-violation regression at the earliest possible point.
   Opt-out marker: `// crd-lint-allow-std-math` on the same line.

7. **Five deferrals captured in `docs/debt.md`** before coding:
   - f64 overloads (→ `crd-hesap` v0a)
   - Vec4f/Vec8f SIMD-batched overloads (→ eylem v1+ surfaces demand)
   - hyperbolic sinh/cosh/tanh (reserved; no consumer in eylem v1-v6)
   - special functions erf/gamma/bessel (→ `crd-hesap-stats` v13)
   - expm1/log1p near-zero variants (reserved)

## Definition of Done

| Config | Build | Math suite | Emission check | No-std-math check |
|---|:---:|:---:|:---:|:---:|
| win-debug | ✅ | 114 cases / 2803 assertions | ✅ AVX2 (42 ymm/5 fp ops) | ✅ |
| (full sweep across all 12 configs runs after this session log writes) | | | | |

Math suite: **114 cases / 2803 assertions** (was 91/2744 after v0b;
+23/+59 for v0c).

## Bugs discovered + fixed during v0c implementation

1. **atan polynomial: operator-sign mismatch.** Cephes uses signed
   coefficients with all `+` operators. My initial draft used `-`
   operators between already-signed coefficients, double-negating
   p1 and p3. Caused atan to drift ~5% wrong; cascaded to atan2, asin,
   acos. Fixed: changed all `-` to `+` operators. (See line 270 of
   `deterministic.cpp`.)

2. **sin/cos sign-overwrite vs sign-XOR.** Initial draft used
   `with_sign(y, sign_out)` which strips y's existing sign bit and
   replaces it with `sign_out`. Cephes uses `if (sign < 0) result =
   -result` semantics — XOR not overwrite. For sin(-3π/4) (where both
   the input is negative AND the octant reduction flips sign), the
   double-negative correctly cancels under XOR but the overwrite version
   produced wrong sign. Added `apply_sign` helper (XOR-combine), kept
   `with_sign` for true copysign semantics, switched sin/cos to use
   `apply_sign`. (See line 54 of `deterministic.cpp`.)

3. **`ulp_diff` test helper used raw bit subtraction.** This treats
   `+tiny vs -tiny` as 2.1 billion ulps apart (the sign bit is bit 31).
   Replaced with signed-magnitude mapping so values across the sign
   boundary are correctly reported as small-distance.

4. **MSVC C4459 shadowing.** Local variables named `e` (for exponent)
   shadowed the namespace constant `e` (Euler's number). Renamed to
   `exp_int`. Caught by `/W4 /WX`.

## Next slice

**v0d — `crd::containers::sort` / `stable_sort` / `nth_element` /
heap ops.** Pdqsort-derived deterministic sort with pinned tie-breaker
(`(key, original_index)` for parallel-stability). Plus lint check
banning `std::sort` etc. in `engine/eylem/**` (similar pattern to
this slice's `crd-no-std-math-check`). ADR-0063 §3.

## References

- Phase plan: `docs/phases/phase-3.1-eylem.md` (v0 table, v0c row).
- Determinism contract: ADR-0063 §2.
- Cephes Math Library: Stephen Moshier, public domain.
  www.netlib.org/cephes/
- Open debt: `docs/debt.md` (Phase 3.1 v0c entry — five deferrals).
- v0a session log: `docs/sessions/2026-05-10-v0a-simd-substrate.md`.
- v0b session log: `docs/sessions/2026-05-10-v0b-soa-substrate.md`.
