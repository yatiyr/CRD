# 2026-10-29 — SIMD-DISPATCH: log to 0.52 ulp, with its SIMD twins

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch);
follows [the Payne–Hanek reduction](2026-10-29-simd-dispatch-payne-hanek.md), whose Left list named this batch.

## CI first

Push run 40826432863 (dd860168) still runs win-shipping-profile, win-asan, linux-gcc-asan and linux-gcc-arm64-asan;
every completed failure in it was traced in earlier batches and is fixed at HEAD. Every later push run was replaced
before any job started (0 jobs). Nothing was cancelled. DIAG.11c and DIAG.12 stay Needs CI: no completed run
carries their hosted evidence.

## Finding

A Python model of `crd_log1` (the same operations through `math.fma`, against 300-bit mpmath) measured 7.0 ulp at
x = 1.41377 and 6.8 ulp at 0.70725, far past the 2 ulp the previous note saw at 0.177. The atanh series stopped at
t^17, which leaves 2^−50 at the seam |t| = 3 − 2√2; the grid had never sampled it. Its rounded t costs ~2 ulp more
near 1 and where e·ln2 and log(m) cancel.

## What landed

- **One reduction for the family.** `crd_log_reduce` returns log(m) as a pair to 2^−58 relative: a degree-7
  Chebyshev fit of (atanh t − t)/t³ ([gen_log_coeffs.py](../../tests/foundation/math/gen_log_coeffs.py), replacing the
  Taylor terms), t's rounding carried through the division's exact remainder and m + 1's exact error, and subnormal
  x scaled by 2^54 (they took the C library before, whose bits differ by platform). `log`, `log2` and `log10` sum the
  exact head with Fast2Sum and round once. The model gives 0.516, 0.517 and 0.517 ulp over the seams at e = 0,
  ±1, ±1000, the cancellation bands, near 1 and 5,000 points over the whole range; without the carried t and the
  exact sum it stays at 1.9, 2.1 and 2.2 ulp, so both stay.
- **The twins.** `crd_log_x2`, `crd_log4` and `crd_log8` run the same operations in the same order; the level probe
  and the forced arms check them against the scalar core bit for bit.
- **pow** renormalises the pair first: the model gives 1.14 ulp on the test's moderate grid and 2.4 ulp for
  pow(1.4142, 2000) and its kind; a subnormal base now takes the same path instead of the C library.
- **Grids.** The log-spaced grids (log, log2, log10, rsqrt) are spaced through mpmath, and the log family adds the
  seams, bands and subnormals (about 990 points each, from 405). The golden checksum is now `0x96d6c9aeb69256a8`,
  from MSVC: IEEE operations and single-rounded fma only, so a lane that disagrees has a defect. The random comparison
  with the C library's log is now an ulp bound (≤1; it was 1e-13 relative).
- A [recipe](../recipes/2026-10-29-scalar-log-and-trig-reduction.md) teaches the log and the Payne–Hanek reductions;
  the previous batch had left the latter without one.

## Checks

win-debug: crd-math-tests (178 cases), its five forced arms (avx512 under SDE), crd-hesap-special-tests and
crd-hesap-stats-tests pass; tidy, clang-format on the added lines and the repository validators.

## Left

- pow is 2.4 ulp at |y·log2 x| ≈ 1000: log(m) to 2^−58 times 1000 is the error. Its ≤1 ulp needs log2 to ~2^−68
  (the leading tail term as a double-double) and an exp2 that takes a pair.
- The exp and expm1 coefficients cite `gen_exp_coeffs.py` and `gen_expm1.py`, and the trig ones `build/_trig_gen.py`;
  none is tracked.
