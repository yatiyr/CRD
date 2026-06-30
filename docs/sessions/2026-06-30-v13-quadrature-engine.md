# 2026-06-30 — v13 quadrature engine (g/h/i): composite + adaptive QUADPACK + DE + non-Gauss

> Session spanned the close of **v13-f** (interpolation — Clough-Tocher) and the **entire 1-D scalar
> quadrature engine** of `crd-hesap-quadrature` (v13-g/h/i). Every method crushes every available
> frontier peer (scipy + MATLAB + Boost + GSL 2.7.1). **Uncommitted** — the user commits + runs the
> Windows DoD. Module not yet complete: v13-j (oscillatory) + v13-k (cubature) still pending.

## What was built

**v13-f close — `crd-hesap-interp` Clough-Tocher C¹** (`clough_tocher.hpp`, over `geometry-delaunay`).
Bit-exact transcription of scipy `_interpnd.pyx` (fetched via `gh`): affine-invariant neighbor-centroid
reduced direction + 19 precomputed Bézier ordinates/triangle + curvature-min Gauss-Seidel gradient; eval =
fast-orient jump-walk locate (+ exact linear-scan fallback ⇒ no interior NaN) + extended-barycentric cubic.
55 asrt; FIT 2.14× scipy, EVAL 85 ns/pt = 1.70× scipy-batch / 35.7× per-point, 2.90× MATLAB griddata('cubic'),
Boost N/A. **⇒ interp module complete (a–f), suite 544.**

**v13-g — `crd-hesap-quadrature` integrate API + nodes + result contract** (`gauss.hpp` += Lobatto/Radau;
`integrate.hpp`). Gauss-Lobatto/Radau (Jacobi-root nodes + closed-form weights, exactness-verified) ·
`QuadResult{value,error_estimate,eval_count,subdiv_count,status,tolerance_met}` error-tier contract ·
integrate_gauss/lobatto/radau + integrate_with_nodes + **integrate_symmetric** (±xᵢ pair fast path) ·
newton_cotes (exact Lagrange-integral) · trapezoid (uniform+non-uniform) · simpson (scipy-exact both parities).
170 asrt; gauss-sym 66 ns = 85× scipy fixed_quad / **1.02× Boost gauss<10>** (the symmetric-pair fix flipped
Boost's initial 4% edge) · simpson 21.9× scipy · trapezoid 70× scipy / 32.7× MATLAB · newton_cotes 2.97× scipy.

**v13-h — the adaptive QUADPACK engine** (`gauss_kronrod.hpp` + `adaptive.hpp` + `qags.hpp` + `qng.hpp`).
GK21 rule (QUADPACK dqk21 constants, fetched from scipy `_rules/_gauss_kronrod.py`; scipy-bit-exact value +
error incl. the roundoff floor; degree-31 exactness) · **QAG** (iterative bounded-depth work-stack — NOT
recursion, hard WCET bound → MaxSubdivisions) · **QAGS** (Wynn-ε extrapolation — faithful goto-preserving port
of dqagse + dqelg + dqpsrt from scipy `__quadpack.c`) · **QAGI** (infinite ranges, dqagie + dqk15i transform,
shared `qags_driver` + reusable `AdaptiveWorkspace`) · **QNG** (non-adaptive Patterson 10/21/43/87 ladder) ·
**QAGP** (break-points → split-at-singularities → QAGS pieces). 110 asrt incl. the Lyness-Kaganove honesty test
(a peak narrower than the node spacing fools the estimate ⇒ error_estimate is Tier-1, not a bound). **Crushes
GSL — the gold-standard QUADPACK reference C — on every method:** QNG 1.09× · QAGS 1.29× · QAGP 1.24× · QAGI
1.36× · QAG 22.7× scipy.

**v13-i — DE + non-Gauss** (`de.hpp` + `nongauss.hpp`). Double-exponential **tanh-sinh / exp-sinh / sinh-sinh**
(precomputed `DeRule` nodes + level refinement) · **Clenshaw-Curtis** (clencurt weights; fixed `ChebyshevRule` +
nested-adaptive `CcAdaptiveRule`) · **Fejér-1** · **Romberg** (function + `romberg_samples` = scipy.romb).
55 asrt. Crushes Boost: exp_sinh 1.17× / sinh_sinh 1.21× / tanh_sinh 1.34×. Crushes GSL: Romberg 1.19×
GSL-romberg · CC-adaptive 2.05× GSL-cquad.

## The crush discipline — the recurring lever (4×)

The user's standing directive — **full crush, no deferrals, never accept near-parity** — drove the session.
Four methods first *lost or tied* a reference, and the cause was the **same** every time: **integrand-independent
work was being recomputed per call** when it should be precomputed once.

1. **GK error estimate `pow(·,1.5)`** — the heavy double-double pow → `x·√x` (one hardware sqrt). QAGS 0.88×→**1.29×**, QAGI 0.89×→**1.36×** GSL. Value unaffected (pow only in the abserr heuristic).
2. **Gauss symmetric-pair** — ⌈n/2⌉ weight-mults via ±xᵢ ⇒ parity→**edge** vs Boost `gauss<10>`.
3. **DE convergence estimate `d²/dₘ₋₁`** — exploits the double-exponential rate (dₘ≈dₘ₋₁²), halving the levels; + per-run tail truncation. exp_sinh 0.48×→**1.17×** Boost.
4. **Clenshaw-Curtis O(N²) clencurt weights** recomputed per call → precomputed `CcAdaptiveRule`. 0.59×→**2.05×** GSL-cquad.

Meta-scar (SANITY Ledger 2026-06-30): the user **refused** "near-parity with the reference C is the ceiling" and
was right every time — parity-with-the-same-algorithm is never the wall; a per-operation cost always is. Method:
**reconstruct-and-verify-in-python FIRST** (fetched scipy/QUADPACK source via `gh`, verified bit-exact before
porting a C++ line — caught a Clough-Tocher gradient sign-flip + the GK roundoff floor pre-port). Full peer board
(scipy + MATLAB + Boost + **GSL 2.7.1**, installed mid-session) on every row; N/A stated with the check.

## Tests

- interp suite **544** (linux-gcc-release), quadrature suite **465** — all green.
- Per-slice gate ran on linux-gcc-release throughout; Windows 4-config DoD is the user's commit-time step.

## Pending / next

- **Quadrature module NOT complete** — **v13-j** (oscillatory: ★QAWO Filon / QAWF / QAWS / QAWC / ★Levin) +
  **v13-k** (multi-D cubature: tensor-Gauss / ★Genz-Malik / ★Smolyak / ★Lebedev / Dunavant simplex) remain.
- Then **`crd-hesap-diff`** (v13-l/m: Fornberg / Richardson-Ridders / **★complex-step** / Savitzky-Golay / spectral)
  → **`crd-hesap-motion`** (v13-n…q: SQUAD / clothoid / NURBS / min-snap / **★Ruckig OTG**) → **v13-z** close
  (CLI + 4 system docs + ADR-0095 + the all-peers scoreboard + the safety-critical conformance audit).
- **USER:** commit (all of v12 + v13 is uncommitted) + Windows 4-config DoD + 18-config CI.
- **System docs** `hesap-quadrature.md` (rewrite) + `hesap-interp.md` (create) are part of v13-z, not yet written.

## Files

New: `engine/hesap-quadrature/include/crd/hesap/quadrature/{integrate,gauss_kronrod,adaptive,qags,qng,de,nongauss}.hpp`
(+ `gauss.hpp` extended) · `engine/hesap-interp/.../clough_tocher.hpp` · tests
`tests/hesap-quadrature/test_{integrate,adaptive,de}.cpp` + `tests/hesap-interp/test_clough_tocher.cpp` ·
benches `runtime/examples/bench_quadrature.cpp` (+ peer scripts under `build/`).
