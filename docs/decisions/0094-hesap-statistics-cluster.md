# ADR-0094 — `crd-hesap-special` + `crd-hesap-stats`: the statistics cluster — special-as-leaf, the inverse-incomplete cdf/ppf engine, the counter-RNG determinism moat, and the two-axis honest gate

- **Status:** Accepted (2026-06-28)
- **Phase:** 3.1.6 v12 (Statistics cluster)
- **Tags:** `hesap` `statistics` `special-functions` `rng` `determinism` `architecture` `module-edges` `substrate`
- **Plan:** `docs/phases/phase-3.1.6-hesap.md` (the v12 DETAILED PLAN); memory `project_v12_stats_plan`; system docs `docs/systems/hesap-stats.md` + `docs/systems/hesap-special.md`

## Context

v12 brings the full statistics subject to the hesap stack — the MAXIMAL surface: special functions (the cdf/ppf
engine), ~50 probability distributions, descriptive statistics, the classical hypothesis-test suite, resampling, KDE/
robust/streaming, MCMC, and regression/GLM/multivariate. Consumers span every Cerid domain: robotics (state
estimation, system ID), scientific/engineering analysis, ML, and the v7 optimization stack (it pulled the Philox RNG
forward in 2026-06-10). Gold standards: **scipy/statsmodels/sklearn** (the free primaries), **MATLAB R2026a** (industry
authority, all toolboxes, installed), **NumPy/Boost/GSL** (kernel-perf), **ArviZ/PyMC/Stan** (Bayesian). Four things had
to be settled.

1. **Where the special functions live** — and whether they form a leaf or tangle with the distributions that need them.
2. **How a distribution computes its quantile (ppf)** without each one reimplementing a root-finder.
3. **What the determinism guarantee is** for stochastic methods (sampling, bootstrap, MCMC) across thread counts.
4. **What "correct" and "fast" mean** when some results are deterministic and some are RNG-driven.

## Decision

1. **A new LEAF module `crd-hesap-special` + expand `crd-hesap-stats` (module-isolation cornerstone).**
   `crd-hesap-special` (gamma/beta/erf + incomplete + inverses · Bessel/Airy · orthogonal polys · the transcendental
   tail) depends only on core/containers/memory/math + `crd-hesap` (Complex). **It is a leaf**: stats, quadrature, and
   dsp consume it; it never references them. This keeps the cdf/ppf primitives reusable by Gauss quadrature (Golub-
   Welsch) and DSP filter design without a cycle.

2. **The inverse incomplete gamma/beta ARE the cdf/ppf engine (no per-distribution quantile code).** Every
   distribution's `ppf` routes through `special::gammainc_p_inv`/`betainc_inv` (+ `ndtri`, the Wichura AS-241 probit).
   Cached-`gln`/`lbeta` overloads amortize lgamma so a frozen distribution beats scipy's per-element ufunc. This single
   surface is also the χ² ppf the v12-z spectral CIs back-wire onto.

3. **The determinism moat = counter-based RNG (the certification enabler).** Philox4x32 / Threefry4x64 are pure
   functions of (counter, key) — no sequential hidden state — so the value at any position exists independently of
   execution order or worker count. **Same seed ⇒ bit-identical, by construction**, including the parallel bootstrap
   and the per-sample MCMC/MV streams. Scoped to certifiable replay (DO-178C/ISO 26262/FDA), the same moat as the
   sparse-direct and filter-application stacks.

4. **The two-axis honest gate (the v10/v11 honest-scoreboard scar applied).** Deterministic cores (distribution
   pdf/cdf, descriptive, hypothesis-test statistics, jackknife/exact-permutation, MCMC diagnostics) gate **BIT-FOR-BIT**
   vs scipy/statsmodels/ArviZ — and where a gold-standard formula is subtle, we read the peer's source (the ArviZ
   R-hat/ESS backend) rather than guess. RNG-driven results gate by **recover-known-target + same-seed bit-identity**.
   Performance crushes **all available peers**, named individually — never a single conflated number. Where a method
   has **no clean gold peer** (the AR-spectral CI is asymptotic-normal with no scipy/MATLAB equivalent), that is stated
   plainly and gated analytically, not papered over with an invented convention.

5. **SANITY rule 8 — reuse the shipped linear algebra, do not reimplement.** Regression rides `dense::lstsq`/`pinv`/
   `eig_sym`; Gauss quadrature rides `eig_sym`; multivariate distributions ride `dense::factor_cholesky`. New acyclic
   edges `stats → {special, dense, quadrature, jobs}`; dense/special never reference stats.

6. **Back-wire the v11 spectral confidence intervals (v12-z, closing the ADR-0093 deferral).** The multitaper PSD χ²
   CI (`[ν·Ŝ/χ²_{ν,1−α/2}, ν·Ŝ/χ²_{ν,α/2}]`, ν=2K / Thomson ν(f)) gates against an **independent** scipy `chi2.ppf`
   reference + the frequency-constant-multiplier invariant. The AR-PSD CI uses Berk's asymptotic-normal `Ŝ·exp(±z·
   √(2p/N))` — no external peer, analytic-gated, honestly labeled.

## Consequences

- `crd-hesap-special` ships first (v12-a…d) so the distribution cdf/ppf, the Gauss quadrature roots, and the DSP
  elliptic functions (hesap-dsp now delegates to the canonical elliptic home here) all consume one surface.
- The scoreboard reports correctness (bit-for-bit / analytic) and speed (named per-peer crush) as separate honest
  columns — never conflated. Crushes land across the cluster (Bessel 24/24, distributions 16/16 vs scipy, NUTS 104×
  ess/s vs PyMC, regression Ridge 47× vs sklearn).
- A curated `hesap.{stats,special}.*` CLI (the v7-z/v10-z/v11-z data-vs-callable split) makes the operations
  agent-reachable.
- Extends ADR-0065 (hesap substrate); consumes ADR-0078 (two-layer typed/raw) and ADR-0094's own special leaf; closes
  the ADR-0093 spectral-CI deferral.
