# 2026-10-29 — SIMD-DISPATCH: pow within 0.58 ulp and its overflow defect

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch);
contract: [ADR-0134 §7](../decisions/0134-runtime-cpu-dispatch.md);
follows [the minimax fit](2026-10-29-simd-dispatch-minimax-coefficients.md) (its Left: pow at 2.4 ulp) and
[the dropped-import map entries](2026-10-29-simd-dispatch-map-wrapped-imports.md).

## Defects

- 2.4 ulp at |y·log2 x| ≈ 1000: log2 x carried only log's 2^-58, and exp2 rounded f·ln2 before its core.
- A product y·log2 x that overflowed reached its tail: pow(10, 1e308) returned −∞ (exp2(∞)·(−∞)), pow(0.1, 1e308)
  NaN (0·∞); past ~1e299 the tail y·tlo (~1e283) made the factor 1 + ulo·ln2 negative.

## Method

A Python model of the new cores (the same operations through `math.fma`) against 400-bit mpmath first, then C++.
Over 120,000 random (x, y) with y·log2 x spread over the whole range, the seams and the overflow and underflow
edges: log2's pair is within 2^-69.9 relative, pow within 0.58 ulp for a normal result and 0.76 for a subnormal one
(the scale rounds twice there, as in exp); the exp2 core alone is 0.571 ulp.

## Change

- `crd_log_t` (exp_log_core.hpp) is `crd_log_reduce`'s front end, now shared; `crd_log_reduce`, `crd_log1`, `exp2`
  and the SIMD twins keep their bits (the golden checksum holds).
- `crd_log2_pair`: S = s/3 + s²/5 + s³·R3(s) as a double-double from its s²/5 term up, s = t² as an exact pair, R3
  a new seven-term fit (`gen_minimax_coeffs.py`, 1.05e-6·2^-53 of atanh(t)/t), tl's 1/(1 − s) to four terms.
- `crd_exp2_pair`: r = (f + lo)·ln2 as an exact pair, exp(r) = 1 + rh + rh²·P + rl·exp(rh) on the exp core's P.
  The scout brief's rl·(1 + rh) would cost 2^-48 at |u| ≈ 1000 (rl reaches 2^-44).
- `crd_scale2`: exp's two-step 2^k injection, shared by both cores.
- pow decides overflow (head ≥ 1025) and underflow (< −1076) on the head before forming the tail. The brief's
  pow(x, −1) == 1/x is no exact case: pow(1e-300, −1) is 0.52 ulp off, so the test uses exact reciprocals.

## Checks

The refs gain 1,068 mpmath pow points (the exponent bands, seams, bases next to 1 with huge y, subnormal and
negative bases) and the pair constants; the earlier arrays and both checksums are unchanged, and pow has its own
(`0x45d1f044b54f2901`, win-debug). The std::pow comparison is replaced by the mpmath gate plus the overflow and
C99 Annex F cases. The ulp helper now uses the smallest subnormal as a subnormal's ulp. win-debug: crd-math-tests
(all, the SIMD probe's pow on every level included); tidy and clang-format on the changed lines; the validators.

mpmath's `float()` rounds a subnormal twice (to 53 bits, then to the subnormal grid): five of the pow refs were one
ulp off the correctly rounded value. The generator now converts subnormals once through an exact rational; no other
array changed.

## CI audit

DIAG.11c and DIAG.12 stay Needs CI: both wait on the final complete-tier run; scheduled run 40849112570 is still in
progress, so no run was dispatched.

## Left

- Hosted CI: the pow checksum on every compiler and level, and the dependent suites (stats, special, autodiff).
