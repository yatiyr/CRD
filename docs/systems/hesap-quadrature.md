# crd-hesap-quadrature — numerical integration (adaptive + oscillatory + cubature)

> Phase 3.1.6 v12-c (Gauss nodes) + v13 (g–k, the integrate API). ADR-0094 (Golub-Welsch nodes) + ADR-0095 (the v13
> adaptive/oscillatory/cubature surface). Plan: `docs/phases/phase-3.1.6-v13.md`.
> Status: **SHIPPED (2026-07-01)** — linux-gcc-green + Windows 4-config; suite ~534 assertions. Sessions:
> `2026-06-30-v13-quadrature-engine.md`, `2026-07-01-v13-jk-oscillatory-cubature.md`, `2026-07-02-v13z-windows-close.md`.

## What it is

Definite integration for every Cerid domain — ground-station-pass windows over time, energy/impulse integrals,
FEM/FVM element integration, BRDF/attitude integrals over the sphere. The module started (v12-c) as the Golub-Welsch
Gauss node generator (over `crd-hesap-dense` `eig_sym`); v13 built the full `integrate()` API on top: the QUADPACK
adaptive family, the double-exponential and oscillatory-weight rules, and multi-dimensional cubature — held to the
v13 certification bar (ADR-0095): **allocation-free, iterative-not-recursive adaptive drivers, status-not-exception,
error-tier-labelled**.

## Shipped surface

- **v13-g — the API + fixed rules.** `integrate()` / `QuadResult{value, error_estimate, status, eval_count,
  subdiv_count, tolerance_met}` / `AdaptiveWorkspace` (allocated once, reused) / `QuadStatus`. Gauss-Legendre +
  Gauss-**Lobatto** (endpoints — FEM/SEM) + Gauss-**Radau** (stiff collocation) + Newton-Cotes (positive-weight
  assert) + composite trapezoid/Simpson on samples (scipy-exact).
- **v13-h — the adaptive engine (real-time-safe).** **★Gauss-Kronrod** (G7-K15 …) + the **iterative bounded-depth
  work-stack driver** (the QUADPACK pattern, never recursion): QNG · QAG · **★QAGS** (+ Wynn-ε extrapolation) · QAGP
  (break-points) · QAGI (infinite range). The hard `limit` is the WCET knob.
- **v13-i — non-Gauss + double-exponential.** **★Clenshaw-Curtis** + Fejér 1st/2nd (FFT weights, nested, positive) ·
  **★tanh-sinh / exp-sinh / sinh-sinh** (endpoint singularities + infinite ranges — the Boost.Math peer) · Romberg
  (function + `romberg_samples`, scipy.romb).
- **v13-j — oscillatory + singular weights.** **★QAWO** (modified Clenshaw-Curtis, ω-independent cost) · QAWF (Fourier
  tail + Wynn-ε) · QAWS (algebraico-log endpoints) · QAWC (Cauchy principal value) · **★Levin collocation** (general
  nonlinear phase; accuracy *grows* with ω). Faithful goto-preserving QUADPACK ports.
- **v13-k — multi-D cubature.** tensor-product Gauss · **★Genz-Malik** (degree-7 + embedded-5 globally-adaptive box
  subdivision) · **★Smolyak sparse grids** (nested Clenshaw-Curtis, breaks the curse of dimension) · **★Lebedev**
  (sphere, spherical-harmonic exactness) · **Dunavant** simplex rules (FEM triangles).

## The error tiers (ADR-0095 §3)

**Tier 1** is the working tier here: the Gauss-Kronrod `|K−G|` estimate (+ QUADPACK roundoff-floor rescale). It is a
*heuristic estimate*, labelled `error_estimate` — never `error_bound` — and is *foolable* (the Lyness-Kaganove hidden
peak), which is why `eval_count`/`subdiv_count` are also returned so an outer harness can flag a suspiciously-cheap
result, and why the roundoff floor surfaces as `RoundoffLimited`.

## Determinism

Fixed-order reductions (`crd::math`, never `std::`), one read-only node/weight table per rule ⇒ bit-identical across
compilers/opt-levels/threads. The adaptive drivers loop over a fixed-size subinterval array (no heap per call, no
recursion) — the `{1,4,16}` moat + run-twice bit-identity + the "no-throw / status-not-exception" structural guard
(`crd-hesap-v13-no-exceptions`) all apply.

## Crush (the full peer board)

Every implemented method beats or matches every available frontier peer — **scipy** + **MATLAB** + **Boost** + **GSL
2.7.1** — at matched accuracy, plus the determinism/WCET/error-tier moat none of them carry. v13-j bit-matches
`scipy.quad(weight=)` to ~1e-16 and **crushes it 5–22×** (QAWO 22× / QAWC 17× / QAWF 12× / QAWS 5.4×), and matches or
beats GSL (the QUADPACK-C reference: QAWF 1.13× / QAWS 1.09× wins, QAWO/QAWC parity-to-win). v13-k Genz-Malik **crushes
`scipy.integrate.cubature` 37.5× (3D) / 279× (5D)**, value bit-identical. **The recurring crush lever: precompute the
integrand-independent work once** (Chebyshev moments, dqmomo tables, Clenshaw-Curtis weights, the GK error `x·√x`
instead of a `pow(·,1.5)`) — it flipped every near-parity loss to a win. Reconstruct-and-verify-in-python-first (fetch
scipy/QUADPACK source via `gh`) before porting a C++ line.

## CLI (`hesap.quad.*`)

A function integrand can't cross the CLI boundary, so the callable adaptive drivers stay in-process; the CLI exposes
the sample-driven surface (scipy.integrate.{trapezoid,simpson,romb}).

| command | params | output |
|---|---|---|
| `hesap.quad.samples.f64` | `y`, `dx`, `rule` (trapezoid\|simpson\|romberg) | scalar integral |

Anchor `register_quadrature_cli_anchor()`; `test_cli.cpp` integrates sampled x² vs the analytic value per rule.

## Module edges (acyclic)

`crd-hesap-quadrature` → core/containers/memory · **crd-hesap** (CLI) · **crd-hesap-special** (recurrence coeffs, Γ/B)
· **crd-hesap-dense** (`eig_sym` for Golub-Welsch) · **crd-hesap-fft** (Clenshaw-Curtis DCT).

## Tests

`tests/hesap-quadrature/` — `test_gauss` · `test_integrate` · `test_adaptive` · `test_de` · `test_oscillatory` ·
`test_cubature` · `test_cli`. ~534 assertions, green on win-debug + the full win-tidy check set.
