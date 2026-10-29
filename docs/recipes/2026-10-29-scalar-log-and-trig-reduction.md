# Argument reduction for crd::math's log, pow and trig

<!-- doc-role: reference -->
> Technique reference. Live owner: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch).

## Parameters and limits

| Quantity | Meaning | Value | Contract |
|---|---|---|---|
| log seam | m = x·2^−e is kept in [√½, √2) | `kSqrt2` | t = (m − 1)/(m + 1) stays in ±(3 − 2√2) ≈ ±0.1716 |
| `kLogR0..7` | R(s) = (atanh t − t)/t³, s = t² | degree-7 fit on [0, (3 − 2√2)²] | relative error 3e-18 ([generator](../../tests/foundation/math/gen_log_coeffs.py)) |
| `kLn2Hi` / `kLn2Lo` | ln 2 split | 355/512 and its remainder | ed·kLn2Hi exact for every exponent (11 × 9 bits) |
| log subnormal scale | 2^54 | before the exponent read | exact; the exponent bias grows by 54 |
| Cody-Waite bound | \|k\| ≤ 2^20, k = round(x·2/π) | \|x\| ≲ 1.647e6 | k times each 33-bit part of π/2 is exact |
| `kTwoOverPiBits` | 2/π in 37 32-bit words | 1,184 bits | the largest double reads up to word 36 |
| Payne–Hanek window | 224 bits of 2/π per exponent | 7 words | dropped bits add < 2^−138; the fraction keeps ≥ 97 bits |
| `kE`, `kQ`, `kSin`, `kCos` | exp, expm1, sin, cos cores | degree 11, 12, 13 and 14 in r | relative error 0.034, 0.073, 0.034, 0.0005 of 2^−53 ([generator](../../tests/foundation/math/gen_minimax_coeffs.py)) |
| `kLogPairR3..9` | pow's atanh(t)/t = 1 + s/3 + s²/5 + s³·R3(s) | degree 9 in s | relative error 1.05e-6 of 2^−53 (same generator) |
| pow's log2 pair | log2 x as hi + lo | 2^−69 relative | y·log2 x keeps ~2^−59 absolute up to \|y·log2 x\| = 1076 |
| Accuracy | against mpmath | log, log2, log10 ≤ 0.52 ulp; pow ≤ 0.58 (subnormal results ≤ 0.76); sin, cos ≤ 1; tan ≤ 2 | identical bits on every compiler and level |

## Why the obvious reductions fail

Both functions reduce x to a small argument and evaluate a polynomial there. The reduction is where accuracy is lost.

- **log.** log(x) = e·ln2 + 2·atanh(t). The old core truncated the atanh series at t^17 and rounded t once. At the
  seam (|t| = 0.1716) the dropped terms were 2^−50 relative, 7 ulp at x = √2 for e = 0. The reference grid never
  sampled the seam, so the 7 ulp went unseen; only a point at 0.177 showed 2 ulp. Its rounded t
  alone costs ~2 ulp near 1 and in the bands where e·ln2 and log(m) cancel (x ∈ (√2, 2) and (½, √½)).
- **pow.** 2^(y·log2 x) amplifies log2's relative error by |y·log2 x|, up to 1076: log's 2^−58 pair cost 2.4 ulp at
  |y·log2 x| ≈ 1000, and an exp2 that rounds f·ln2 before its core drops the exponent's tail. A product y·log2 x
  that overflows went on to its tail: pow(10, 1e308) returned −∞ and pow(0.1, 1e308) NaN.
- **trig.** x − k·π/2 with π/2 in three 33-bit parts is exact only while k·part is. Past |k| = 2^20 those products
  round, and from 2^53 on k itself cannot be formed in a double: sin(1e300) returned +∞.

## The maths

**log.** m − 1 is exact (Sterbenz). m + 1 rounds: u = RN(m + 1) and ul = m − (u − 1) are exact, so m + 1 = u + ul.
t = RN((m − 1)/u), and the exact remainder r = fma(−t, u, m − 1) gives tl = (r − t·ul)/u, so t + tl is the quotient
to ~2^−104. log(m) = 2t + 2t·s·R(s) + 2·tl/(1 − s), and 2·tl·(1 + s) keeps the last factor's first two terms
(the rest is below 2^−62). The pair (hi, lo) = (2t, the rest) is log(m) to 2^−58 relative; |lo| ≤ 0.011·|hi|.
The result adds e·ln2: e·kLn2Hi is exact and, since |e·ln2| ≥ ln2 > |log m| whenever e ≠ 0, Fast2Sum (Dekker 1971)
makes it and hi an exact pair; one rounding of the head plus the small terms remains. log2 and log10 multiply the
pair by log2(e) and log10(e) as double-doubles (an fma gives each product's error) before the same exact sum.

**pow.** log(m) = 2t·(1 + S) + 2·tl/(1 − s) with S = s/3 + s²/5 + s³·R3(s). The terms from s²/5 up cost more than
2^−69 if any of them rounds, so S is a double-double built inside out (A = 1/5 + s·R3, B = 1/3 + s·A, S = s·B),
with s = t² as an exact pair and 1/3, 1/5 as pairs; R3 stays in double (s³·R3 ≤ 2^−18 of the whole). tl's factor
1/(1 − s) keeps four terms. Times log2(e) as a pair and plus the exponent (Fast2Sum), log2 x is hi + lo to 2^−69.
u = y·log2 x is formed as a pair through an fma; its head alone decides overflow (≥ 1025) and underflow (< −1076).
Then 2^u: k = round(uh), f = uh − k exactly, r = (f + ul)·ln2 as an exact pair rh + rl, and exp(r) = 1 + rh +
rh²·P(rh) + rl·exp(rh) on the exp core's P, with 1 + rh an exact pair and rh² carried, rounded once and scaled by
2^k (two half-steps outside the normal range, so a subnormal result rounds twice).

**trig.** Payne & Hanek (1983, "Radian reduction for trigonometric functions", SIGNUM Newsletter 18(1)): write
|x| = m·2^E. The bits of 2/π above 2^(2−E) add multiples of 4 to x·2/π, which leave the quadrant unchanged, so
m (53 bits) times the next 224 bits of 2/π gives k mod 4 and the fraction in integer arithmetic. Muller's worst
case over all doubles (0x1.6ac5b262ca1ffp+849, |r| ≈ 2^−61; J.-M. Muller, *Elementary Functions*, 3rd ed., the
range-reduction chapter) still keeps 97 significant bits. The fraction becomes a double-double and is multiplied by π/2 as one.

**Polynomials.** Each core is the best relative-error polynomial of its exact evaluation form (exp: 1 + r + r²·P;
expm1: x·Q; sin: r + r³·S(r²); cos: 1 − r²/2 + r⁴·C(r²)) over the domain its reduction reaches, found by the Remez
exchange (Remez 1934; Cheney, *Introduction to Approximation Theory*, ch. 3). Rounding the coefficients to doubles
one by one, lowest degree first, and refitting the higher ones after each, recovers most of what a plain rounding
loses (the idea of Brisebarre & Chevillard, "Efficient polynomial L∞-approximations", ARITH 2007, without their
lattice step). The relative error is 0/0 at r = 0 for sin and cos, so their fit starts at 2^−40.

## Assembly

1. log: read e and m, halve m above √2; u, ul, t, tl; s = t²; R(s) by Horner (seven fmas); lo =
   fma(2t·s, R, fma(2tl, s, 2tl)); then the exact head sum. Twins `crd_log_x2` (SSE2/SSE4.2, fma through
   `fma_exact.hpp`), `crd_log4` (AVX2) and `crd_log8` (AVX-512) run the same operations in the same order; zero,
   negative, NaN, +∞ and subnormal lanes take the scalar `crd_log1`.
2. pow: `crd_log_t` (the reduction's front end, shared with `crd_log_reduce`), `crd_log2_pair`, the u pair and its
   range check, `crd_exp2_pair`, then the sign of a negative base with an odd y; 0, ±∞, NaN and a negative base with
   a non-integer y take the C library's pow (constant results).
3. trig: k = round(x·2/π); while |k| ≤ 2^20 Cody-Waite with three exact parts and a fourth for the tail; above,
   `reduce_pio2_large`: the 2×7-limb product, k from the two bits above the point, a fraction of ½ or more rounds k
   up and is negated, then the leading limbs convert exactly and scale by powers of two.

## Traps

- A cheaper log that keeps the new polynomial but drops tl and the exact sum stays at ~2 ulp near 1 and in the
  cancellation bands (the Python model in the [session](../sessions/2026-10-29-simd-dispatch-log-accuracy.md)).
- `crd_log_reduce`'s `lo` is not normalised; a caller that adds it where a tail is expected silently loses digits.
  pow takes `crd_log2_pair`, which is.
- pow's tail term is rl·exp(rh), not rl·(1 + rh): rl reaches 2^−44 at |u| ≈ 1000, and rl·rh²/2 is then 2^−48.
- The grids must sample the hard inputs: the seams at e = 0, ±1, ±1000, the bands, subnormals, and for trig the
  doubles nearest k·π/2. A uniform log-spaced grid missed a 7 ulp error.
- Space the grids through mpmath, not numpy's exp, whose last bits differ by platform.
- A fit that only looks minimax is not: the earlier cores' approximation error alone was 0.43 to 0.59 of 2^−53
  (measure the rounded polynomial against mpmath; never trust the fit's own level).
- The SSE2 fma emulation is exact only inside its proven region; outside, a lane calls `libm::fma`, which still rounds
  once. Negate with a sign-bit xor, never `0 − t` (−0).

## Measured numbers

Accuracy only so far: the gates in [test_transcendental.cpp](../../tests/foundation/math/test_transcendental.cpp).
Timings come with SIMD-DISPATCH's row-close board pass (the lgamma and Student-t boards run these twins).

## Where the code lives

[exp_log_core.hpp](../../engine/foundation/math/include/crd/math/exp_log_core.hpp),
[transcendental.hpp](../../engine/foundation/math/include/crd/math/transcendental.hpp),
[simd/transcendental.hpp](../../engine/foundation/math/include/crd/math/simd/transcendental.hpp),
[power.hpp](../../engine/foundation/math/include/crd/math/power.hpp),
[trig.hpp](../../engine/foundation/math/include/crd/math/trig.hpp); references from
[gen_transcendental_refs.py](../../tests/foundation/math/gen_transcendental_refs.py).
