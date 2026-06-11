# 2026-06-10 — hesap v7-p-1 CLOSE: the direct-search trio (same session, part 11)

**Phase:** 3.1.6 `crd-hesap` · v7 optimisation (`crd-hesap-opt`, ADR-0090)
**Slice:** v7-p-1 — derivative-free part 1. **v7-p was DE-SLIPPED by user decision this session**: p/q/r run
before v7-z, and for v7-p the user explicitly chose the **FULL-PORT path** (BOBYQA/NEWUOA/COBYLA included,
via the L-BFGS-B reference-oracle + differential-harness playbook) over direct-search-only. p-1 = the
from-spec trio; p-2/3/4 = the ports (multi-session). Continues the v7-f..o logs.

---

## The trio (all value-only — no gradients anywhere)

- **`nelder_mead.hpp`** — Nelder-Mead with scipy's exact `_minimize_neldermead` semantics: the
  nonzdelt/zdelt initial simplex, scipy's reflect/expand/contract/shrink accept conditions, optional
  **Gao-Han adaptive parameters** (χ = 1+2/n, ψ = 3/4−1/(2n), σ = 1−1/n), both-spreads termination.
  Simplex ordering by an index-tie-broken insertion sort (deterministic; the repo bans std::sort).
- **`powell.hpp`** — Powell's conjugate-direction method (1964) over a **faithful Brent 1-D minimizer**
  (NR `mnbrak` golden-ratio bracket expansion + Brent 1973 golden-section/safeguarded-parabolic — the same
  pair scipy's 'Powell' drives), with the f_E extrapolation test deciding direction replacement.
- **`pattern_search.hpp`** — GPS + OrthoMADS-style poll with the mesh-size certificate termination (the
  direct-search convergence theory hook). OrthoMads draws a fresh orthonormal Householder basis each
  iteration from a **(seed, iteration)-keyed Philox unit vector** — MADS's fresh poll directions WITH
  bit-reproducibility by construction (the v7-i stream discipline). Honest scope: poll-only (no surrogate
  SEARCH step), simple ×2/÷2 mesh update — named in the header.

## Gates (62 asserts / 6 cases; suite 3529/115)

All four variants (NM, NM-adaptive, Powell, GPS, OrthoMADS) at the analytic minimum of a shifted quadratic ·
NM + Powell on Rosenbrock-2 (pattern search honestly scoped out of curved smooth valleys — its domain is
nonsmooth/noisy) · **the nonsmooth ℓ1 gate** — f = Σ|x_i − c_i|, where gradient methods are inapplicable;
NM and both polls converge to 1e-5/1e-6 · **Powell's conjugacy property** on a cross-coupled quadratic
(≤ 12 sweeps where coordinate descent zigzags) · bit-identical run-twice determinism for all three including
the Philox-keyed OrthoMADS stream (+ a different seed still converges) · n = 1 and n = 0 boundaries.
Verified: debug + asan + shipping (3529/115 each) + tidy build + clang-format + the 6 ctest guards.

## The p-2/3/4 oracle probe (de-risk done this slice)

NLopt cloned on WSL (`/tmp/nlopt`); the three reference sources confirmed present and sized:
`cobyla.c` 1,872 · `newuoa.c` 2,583 · `bobyqa.c` 3,278 lines (~7.7K total of f2c-style C — the multi-session
estimate confirmed; L-BFGS-B was ~1,900). Differential-harness granularity identified: the model-based codes
keep their subproblems as separate functions (`trstlp`; `trsapp`/`biglag`/`bigden`; `trsbox`/`altmov`/
`update`/`prelim`/`rescue`) — per-routine diff testing like L-BFGS-B is feasible; the oracle build must
expose the statics (compile per-algorithm with statics externalized to avoid cross-file symbol collisions).
`scripts/setup-nlopt-ref.sh` lands when p-2 starts.

## Next

**v7-p-2 COBYLA port** (the only DFO member with general constraints; linear models + `trstlp`) →
p-3 NEWUOA → p-4 BOBYQA → v7-q global (CMA-ES + DE + dual-annealing + basin-hopping; Philox normals) →
v7-r MIP (B&B over the v7-l bounded simplex) → **v7-z** close.
