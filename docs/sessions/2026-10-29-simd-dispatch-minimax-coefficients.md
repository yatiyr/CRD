# 2026-10-29 — SIMD-DISPATCH: generated minimax cores for exp, expm1, sin and cos

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch);
follows [log to 0.52 ulp](2026-10-29-simd-dispatch-log-accuracy.md), whose Left list named the untracked generators.

## CI first

Push run 40826432863 (dd860168) still runs win-shipping-profile, win-asan, linux-gcc-asan and linux-gcc-arm64-asan.
Its one completed failure not yet read here, linux-gcc-relwithdebinfo (job 123291443135), is the
`test_eviction.cpp:374` null-dereference link error fixed in ff1949c4. Every later push run was replaced before any
job started (0 jobs). Nothing was cancelled. DIAG.11c and DIAG.12 stay Needs CI.

## Finding

The exp, expm1, sin and cos coefficients cited generators that were never tracked (`gen_exp_coeffs.py`,
`gen_expm1.py`, `build/_trig_gen.py`). Measured against mpmath, the committed polynomials were far from minimax:
their approximation error alone was 0.59 (exp), 0.51 (expm1), 0.48 (sin) and 0.43 (cos) units of
2^−53, so the ≤1 ulp claims rested on the final rounding's luck. cos's r^14 term had the series' magnitude with its
sign reversed, and its r^16 term was 200 times the series' with the sign reversed too.

## What landed

- [gen_minimax_coeffs.py](../../tests/foundation/math/gen_minimax_coeffs.py): a Remez exchange for the best
  relative error of each evaluation's exact form over the domain its reduction reaches, rounding lowest degree first
  with a refit of the higher terms after each rounding. Errors of the rounded polynomials: exp 0.034, expm1 0.073,
  sin 0.034, cos 0.0005 (units of 2^−53), checked again by an independent evaluation.
- sin and cos drop one term each (now r^13 and r^14, fdlibm's shape): five terms measured 45 and 0.83 of 2^−53, seven
  0.0008 and 0.000015. exp keeps degree 11 and expm1 its eleven Q terms. The SIMD exp twins share the constants,
  so the forced-level arms still compare bit for bit.
- The golden checksums move (`0x9179c815fca024a6`, Payne–Hanek range `0x6caf0b0834f6ea14`, from MSVC win-debug,
  stable over two runs); every ulp gate passes unchanged.
- A local constant from the Payne–Hanek batch (`kLimbWeight`) took the lower_case local-constant name tidy requires.

## Checks

win-debug: crd-math-tests (178 cases) and its five forced arms (avx512 under SDE), crd-hesap-special-tests,
crd-hesap-stats-tests and crd-hesap-autodiff-tests pass; tidy and clang-format on the changed lines; the
repository validators. This host misexecuted 5 of 6 long Python runs (segfaults and impossible interpreter errors,
on Windows CPython 3.14 and 3.11 and on WSL's 3.12); every complete run printed the same coefficients (four for
exp and expm1, six for sin and cos).

A [recipe](../recipes/2026-10-29-scalar-log-and-trig-reduction.md) section teaches the fit.

## Left

- **The jobs frame arena**: landed, [frame arena ownership](2026-10-29-jobs-frame-arena-ownership.md).
- pow is 2.4 ulp at |y·log2 x| ≈ 1000 (from the log note); its ≤1 ulp needs log2 to ~2^−68 and an exp2 that takes
  a pair.
