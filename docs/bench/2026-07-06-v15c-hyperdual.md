# 2026-07-06 — v15-c: exact second-order forward AD (hyper-dual) — crush + correctness

**What shipped:** the flat 4-slot hyper-dual `HyperDual<T>` = `{f0, f1, f2, f12}` (Fike & Alonso 2011), exact second
derivatives with NO step and NO subtractive cancellation; the drivers `hessian_entry` / `hessian` (full symmetric,
n(n+1)/2 passes) and — the opt lever — `curvature` = **vᵀHv in ONE pass** (seed both ε with v, read f12). Nested
`Dual<Dual<T>>` works for the arithmetic cross-check (the O(2^K) baseline the flat POD beats). Wired into the umbrella;
gradient (Jet) + exact Hessian feed a Newton step.

## Correctness (`test_hyperdual.cpp`, 6-config green)
- Hessians match the analytic reference (polynomial + transcendental) to ≤1e-12; symmetry exact.
- `curvature` (one pass) == vᵀ·`hessian`·v.
- flat HyperDual f'' == nested `Dual<Dual>` == analytic.
- **No-cancellation:** hyper-dual f'' is exact (≤1e-12) where FD-of-FD holds only ~3 digits — the property it exists for.
- **opt gate:** exact-Hessian Newton step (`H` hyper-dual + `g` Jet) reaches a quadratic's minimizer in ONE step.

## ★ Crush — batched curvature vᵀHv throughput (`external/crd_v15c_hyperdual_bench.cpp`)
ns per point (SIMD across 4 points; tanh via `crd_exp4`); 1T; median-of-15; matched accuracy (all 3 methods agree,
FD-of-FD the independent oracle). Peers: **autodiff.hpp `dual2nd`** (the frontier exact-2nd-order type — a nested dual
under the hood, exactly what the flat POD beats) + FD-of-FD (naive).

| N (inputs) | autodiff `dual2nd` | FD-of-FD | **Cerid BHyperDual** | vs autodiff | vs FD-of-FD |
|--:|--:|--:|--:|--:|--:|
| 4 | 61.30 | 80.37 | **4.68** | **13.1×** | **17.2×** |
| 8 | 124.82 | 157.37 | **9.42** | **13.3×** | **16.7×** |
| 16 | 248.79 | 313.43 | **19.31** | **12.9×** | **16.2×** |

**★ Cerid CRUSHES the frontier `dual2nd` ~13× and FD-of-FD ~17× at EVERY N.** Three multiplied levers: flat 4-slot
POD (beats nested 2^K) × SIMD-across-4-points × SIMD `exp4` transcendental. No losses.

**Fairness gate (self-verifying):** the bench ABORTS unless every peer computes the SAME vᵀHv — Cerid vs autodiff
agrees to **2.8e-16 (bit-exact**; both exact AD), Cerid vs FD-of-FD to ~9e-8. autodiff is driven via its LEANEST path
(direct nested seed `{x,v,v,0}`, no high-level-API overhead), so it gets its best case and the crush is conservative.
(⚠ autodiff's high-level `derivative(g, wrt(t,t), at(t))` mis-seeds a directional 2nd derivative — the gate would now
catch that class of peer-driving error automatically, rather than by luck.)

## Verdict
- **Full crush** vs the frontier 2nd-order peers (autodiff `dual2nd`, FD-of-FD) at every N.
- Exact, cancellation-free 2nd derivatives; curvature vᵀHv in one pass = the TR/Newton lever (the vector HVP is
  forward-over-reverse → v16-e). 6-config DoD green (win-debug/asan/shipping/tidy + clang-cl + gcc); opt zero-regression.
