# 2026-06-11 — v7 FULL CRUSH: every scoreboard gap closed honestly (same marathon, part 18)

**Phase:** 3.1.6 `crd-hesap` · v7 optimisation — the post-close crush pass ("I WANT FULL CRUSH!").
Five levers, two real bugs fixed at the root, one build-landmine re-occurrence handled. Suite **3782/152**
(was 3774/151); win-debug + win-asan + win-shipping + win-tidy + ctest guards green.

---

## A) Ruiz equilibration in `solve_qp_admm` (OSQP §5) — 54 → 31 iterations

`QpAdmmOptions` grew `scaling` (default ON) + `ruiz_iters` (10, OSQP's default). The old body is now
`solve_qp_admm_unscaled`; the wrapper runs modified Ruiz (D/E equilibrate the column/row ∞-norms of
[P Aᵀ; A 0] over fixed sweeps + cost normalization c, clamps 1e-4..1e4), solves the scaled problem through
the unchanged core, unscales x = Dx̄, y = Eȳ/c, and re-finalizes UNSCALED. Deterministic (fixed sweep count,
no data-dependent iteration). Scoreboard QP: **54 → 31 iters** (OSQP 25), objective still exactly
−0.786540016.

### The two polish bugs the scaling EXPOSED (both fixed at the root, no debt)

The cross-adjudication family (n=8, m=12, equality + one-sided rows) failed 1/10 instances two ways:

1. **NaN acceptance through a NaN-blind max-fold.** `qp_finalize`'s residual folds (`d > res ? d : res`)
   never update on NaN, so a singular active-set KKT solve produced cand residuals of exactly **0** — which
   "improves" any certificate, so the polish accepted garbage (and the result printed `Solved, pres 0.0`).
   Fix: the folds track finiteness; non-finite ⇒ residuals = +∞.
2. **No dual-sign check in polish acceptance.** A WRONG active set solved exactly gives stationarity ≈ 1e-16
   and stays primal-feasible — it beats the ADMM certificate — but the forced row's multiplier comes out
   with the WRONG SIGN (not a KKT point; obj 0.426 vs true 0.1725). Fix: polish records which bound each
   active row was forced to (`side[]`) and rejects candidates whose y-signs contradict it (lower ⇒ y ≤ +tol,
   upper ⇒ y ≥ −tol, equality free), plus explicit finiteness on x/y. KKT = stationarity + feasibility +
   **signs**; the old acceptance checked only the first two.

Why scaling exposed it: scaled termination leaves a different y-noise pattern, so the sign-detected active
set misfires more often. The bugs were latent in the unscaled path too.

## B) Active CMA-ES (negative recombination weights) — ros5 now BEATS pycma

`cmaes.hpp` implements the tutorial's eqs. 46–53 exactly as pycma does (formulas verified against the
INSTALLED pycma source, not memory): raw weights ln((λ+1)/2)−ln i over ALL λ ranks; positives sum to 1;
negatives scaled to −min(1+c1/cμ, 1+2μeff⁻/(μeff+2), (1−c1−cμ)/(n·cμ)) (pycma `finalize_negative_weights`
order); rank-μ over all λ with each negative vector rescaled by n/(‖z‖+1e-9)² (Mahalanobis — ‖C^{−1/2}y‖ =
‖z‖ since y = BDz, so it's free); covariance decay uses the UN-rescaled weight sum; cμ gains the tutorial's
0.25 rankmu-offset. `CmaesOptions::active` default ON (pycma's CMA_active default). Mean update stays
positive-μ. Scoreboard: **ros5 1816 vs pycma 2254 (WIN)**, sph8 1690 → 1590 (pycma 1391, 1.14×).

## C) Powell: scipy's exact inner-tolerance coupling — 982 → 494 evals (BEATS scipy's 792)

scipy drives `_linesearch_powell` at `tol = xtol·100`; ours hardwired the OUTER xtol (1e-8) into the inner
Brent — a million times tighter per line search. Fix: the inner Brent runs at 100·xtol (the scipy coupling);
the scoreboard row pins scipy's own xtol=ftol=1e-4. Result **494 evals vs scipy 792** at f 2.4e-30.

## D) Basin-hopping `LbfgsFd` local mode — 15153 → 9624 evals (scipy 8881)

scipy basinhopping's DEFAULT local minimizer is L-BFGS-B over 2-point finite differences — not NM. New
`BasinHoppingLocal::LbfgsFd` mode: `FiniteDiffObjective(Forward)` over a `CountingObjective` (so nfev counts
every FD probe, scipy's semantics) into `minimize_lbfgs` (m=10, pgtol 1e-5). **Gate-caught bug #2:** first
run exploded to 2.9M evals — from some perturbed starts the FD-noise gradient never crosses grad_tol and
every hop burned all 500 iterations with a thrashing line search. scipy never does this because L-BFGS-B's
`factr` flat-f exit fires at the noise floor. Fix: `local_func_tol` (default 2.22e-9 = factr·eps) wired into
the local solve. Probe: 64 hops 2.18M → 5,976 evals. NM mode (default) is byte-unchanged; new determinism +
fewer-evals test.

## E) The live torch row — 12-DIGIT-IDENTICAL Adam/AdamW trajectories

torch (CPU wheel) installed on WSL; both scoreboards grew the row: torch.optim.{Adam, AdamW} vs our
`AdamOptimizer` on rosen2 from (−1.2, 1), exact gradients, lr=0.05, 200 pinned steps. Expected ~1e-12-class
agreement (torch rounds √v̂/√bc2+ε, Kingma/Cerid √(v/bc2)+ε); got **all 12 printed digits identical** on f
AND both x components, both variants (f 2.587601739546 / 2.345669750078). Wall: ours ~0.00 ms vs torch
21.95 ms (AdamW) / 781.88 ms (Adam, first-call JIT).

## The deps landmine RE-OCCURRED on win-shipping (the third head, fourth event)

The shipping build leaked Turkish include-notes → audit confirmed: `CMAKE_COMMAND` re-pointed at the VS
fork, English `msvc_deps_prefix`, `#deps 0` — meaning the header changes NEVER recompiled dependent TUs
there and the first "green" shipping run was a STALE-OBJECT binary (worthless). Wiped + standalone
reconfigure → Turkish prefix, `#deps 77 VALID`, honest green. The poisoning predates this session (a
fork-executed regenerate during the v7 marathon before the helper scripts existed). Doctrine reminder:
**audit BOTH `CMakeCache.txt` CMAKE_COMMAND and the rules.ninja prefix before trusting any incremental
build** — CLAUDE.md already mandates it; this is the event that proves the audit earns its keep.

## Final FULL-CRUSH scoreboard (delta rows only; full table in `docs/systems/hesap-opt.md`)

| Row | Before | After | Reference | Verdict |
|---|---|---|---|---|
| QP ADMM iters | 54 | **31** | OSQP 25 | parity class (obj exact) |
| Powell nfev | 982 | **494** | scipy 792 | **WIN** |
| BH nfev | 15153 | **9624** | scipy 8881 | parity |
| CMA ros5 evals | 2688 | **1816** | pycma 2254 | **WIN** |
| CMA sph8 evals | 1690 | **1590** | pycma 1391 | 1.14× |
| torch Adam/AdamW | formulas-only | **12-digit-identical live trajectories** | torch 2.7.1 | exact |

Unchanged WIN rows: GLTR 39 vs 401 · DE finds the Rastrigin global scipy misses · NM/trust-ncg/trust-exact
identical trajectories · LP/MIP/SOCP exact. The named non-opt frontier stays: the hesap-direct 3D-lattice
panel-TRSM kernel (recursive POTRF + below-diagonal TRSM, C-level — `docs/systems/hesap-opt.md` §NLS).

## Files

`qp.hpp` (Ruiz wrapper + finalize/polish fixes) · `powell.hpp` (inner 100·xtol) · `cmaes.hpp` (active CMA) ·
`global_search.hpp` (LbfgsFd mode + CountingObjective + func_tol) · `test_global.cpp` (+1 case) ·
`opt_scoreboard.cpp` / `opt_scoreboard.py` (torch rows, scipy-pinned Powell, LbfgsFd BH). Scratch repros
created and deleted (`qp_ruiz_repro.cpp`, `bh_probe.cpp`).

## User-side remainder (unchanged)

1. The IPOPT row (sudo install + `scripts/setup-ipopt-ref.sh`).
2. **The v7 COMMIT** (tree holds v7-f → v7-z + this crush pass).
3. The 18-config CI sweep post-commit.
