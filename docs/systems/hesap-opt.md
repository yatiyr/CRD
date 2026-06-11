# crd-hesap-opt — Optimization Substrate (Phase 3.1.6 v7)

> Matrix-free optimization over `crd::hesap::opt::Objective<T>` — the universal "find the best X." The
> differentiator: the optimization **trajectory** is bit-identical across {1..16} workers (a serial optimizer over
> a bit-exact objective eval) — no mainstream optimizer carries it. ADR-0090, ADR-0065 D14.

## Purpose

`crd-hesap-opt` is the optimization domain (unconstrained + bound-constrained + constrained), consumed across
Cerid: eylem (constrained dynamics / IK / trajectory / powered-ragdoll motors), estimation+control (MPC / LQR /
Kalman / SLAM-bundle-adjustment), FEA/CFD (topology/shape/design opt, parameter ID), CAD (sketch constraint
solving), ML (training / differentiable sim), rendering (calibration / BRDF-fit / photogrammetry-BA), games
(AI/RL/CSP/balancing), robotics (inverse dynamics / motion planning / calibration), DAW (filter design). Backed by
the hesap dense/sparse/direct/iterative/eig modules (every Newton step = a linear solve; exact trust-region = an
eig subproblem) + autodiff (gradients, future).

## Substrate (v7-a, shipped)

- **`Objective<T>`** — `value(x)` + `n()` (pure) + `gradient(x,g)` / `hessian_vector(x,v,hv)` (virtual, default
  not-provided); `has_gradient()`/`has_hessian_vector()` capability flags. Raw lower-layer f32/f64.
- **`OptOptions<T>`** (max_iters, grad/step/func tolerances, history) · **`OptResult<T>`** (x*, fx, grad_norm,
  iterations, status, converged) · **`OptStatus`** · `check_convergence(...)`.
- **`LineSearch<T>`** interface + `BacktrackingArmijo<T>` · **`WolfeLineSearch<T>`** (weak/strong Wolfe via N&W
  bracketing-zoom) · **`MoreThuenteLineSearch<T>`** (MINPACK `dcsrch`/`dcstep` verbatim — the liblbfgs/Ceres
  default; v7-c) — the curvature-enforcing searches set `grad_at_new_valid=true`.
- **`QuadraticObjective<T>`** (½xᵀAx − bᵀx over a LinearOp A; the canonical test + moat vehicle) +
  **`minimize_gradient_descent<T>`** (the first end-to-end optimizer — steepest descent + line search).

## Derivatives (v7-b, shipped)

- **`Dual<T>`** — the forward-mode AD scalar (value + one tangent; algebra + sin/cos/tan/exp/log/sqrt/tanh/abs/pow
  chain rules; comparisons act on the value). Lives here for v7-b; may migrate when the ADR-0065 autodiff module
  lands (header-only ⇒ mechanical).
- **Finite differences** — `finite_difference_gradient` (forward / central, **scale-relative step**
  `√ε·max(|xᵢ|,1)` / `ε^⅓·max(|xᵢ|,1)` with true-step recovery — a fixed absolute step is silently wrong on
  badly-scaled variables) + **`FiniteDiffObjective<T>`** decorator (wraps a value-only objective to expose
  `gradient()` via FD, so it drops into any gradient method). The fallback for every method from v7-c on.
- **Forward-mode AD** — `forward_ad_gradient<T>(functor, x, g, alloc)` (EXACT gradient, fused value+gradient, n
  Dual sweeps) over a scalar-generic functor (`DiffFunctor` concept) + **`make_objective_from_functor<T>`** /
  `FunctorObjective` adapter (AD plugs into the virtual `Objective` gradient interface — ADR-0090 §6).
- **`gradient_check`** — analytic gradient vs central FD, returns the worst componentwise error (the harness every
  elite optimizer ships; catches a wrong hand-derived gradient). Diagnostic, not a moat gate.
- **Deferred:** complex-step differentiation (a redundant, holomorphic-only alternative to forward-AD that also
  needs complex transcendentals `crd::hesap::Complex` lacks — exact 1st derivatives are already delivered by
  forward-AD, so this is a clean deferral, not a gold-standard gap). Reverse-mode AD is the separate ADR-0065
  module (the BA/ML workhorse) and plugs into the same `Objective` interface.

## Quasi-Newton (v7-d, shipped — L-BFGS-B pending)

- **`minimize_lbfgs`** — limited-memory BFGS (Nocedal two-loop recursion over m (s,y) pairs, γ-scaling,
  curvature-skip; the large-scale workhorse). Default line search = More-Thuente. `OptResult::fn_evals`/`grad_evals`
  are populated (the L-BFGS verdict metric). **GOLD-STANDARD VERDICT:** vs liblbfgs (its line search is also
  More-Thuente) + scipy L-BFGS-B on Rosenbrock-N — all three agree on iteration + point-eval count (ratio
  1.00–1.02×) and minimizer; eval+wall-clock PARITY with the compiled peer. Plain L-BFGS is the same algorithm ⇒
  parity is the ceiling; the **{1..16} determinism moat liblbfgs/scipy lack is the differentiator**.
- **`minimize_bfgs` / `minimize_sr1`** (`minimize_quasi_newton` + `QuasiNewtonUpdate`) — dense inverse-Hessian
  rank-2 BFGS / rank-1 SR1 (small-n; SR1 falls back to steepest descent if H·g isn't a descent direction).
- **`minimize_lbfgsb<T>`** (v7-d-3) — bound-constrained L-BFGS-B (`l ≤ x ≤ u`, ±1e30 = unbounded side; `factr`
  relative-f tolerance; `grad_tol` = pgtol). A **faithful routine-for-routine port** of the Zhu-Byrd-Lu-Nocedal
  reference (the exact code scipy wraps), **differentially verified vs the reference C** — every routine bit-identical
  + the driver end-to-end vs `setulb` (`runtime/examples/lbfgsb_difftest.cpp`, gated `CRD_BUILD_HESAP_VS_LBFGSB`,
  `scripts/setup-lbfgsb-ref.sh`; 55 checks). ⚠ `OptResult::grad_norm` here is the **projected**-gradient ∞-norm
  (the correct bound-constrained optimality cert) — a different quantity than the plain `‖∇f‖∞` the unconstrained
  optimizers report in the same field.
- Bench: `runtime/examples/bench_hesap_lbfgs_vs_reference.cpp` (WSL-gated `CRD_BUILD_HESAP_VS_LBFGS`,
  `scripts/setup-lbfgs-ref.sh`) + `scripts/lbfgs_ref_scipy.py`.

## The full method set (v7-f → v7-r, ALL SHIPPED 2026-06-10; per-slice detail in the phase rows)

| Family | Headers | What ships |
|---|---|---|
| First-order | `conjugate_gradient.hpp` `momentum.hpp` | nonlinear CG (FR/PR⁺/HS/DY + restarts), heavy-ball + Nesterov/FISTA w/ O'Donoghue-Candès restart (acceleration-theorem gate) |
| Newton | `newton.hpp` `newton_cg.hpp` `newton_sparse.hpp`* | dense N&W-3.4 τ·I, truncated Newton-CG (Eisenstat-Walker — scipy's rule), sparse supernodal Newton (symbolic-once) |
| Trust region | `trust_region.hpp` | N&W 4.1 over SIX subproblems (Cauchy/dogleg/2D/Steihaug/**GLTR**/**exact Moré-Sorensen via eig_sym**); `solve_trust_region_subproblem_exact` is PUBLIC |
| Stochastic/ML | `stochastic.hpp` `minibatch.hpp` | the TEN PyTorch-semantics steppers (SGD/Adam/AdamW/Nadam/RAdam/RMSprop/Adagrad/Adadelta/Lion/LAMB), LR schedules, clipping, the **epoch-keyed reproducible `MinibatchSampler`** (Philox) |
| KKT substrate | `constraints.hpp` `kkt.hpp` `merit.hpp` `sqp_equality.hpp` | the pinned N&W conventions, the 4-part KKT certificate, the **inertia-corrected Bunch-Kaufman saddle solve** (IPOPT's own correction mechanism), ℓ1 merit + SOC |
| QP ⭐ | `qp.hpp` `qp_active_set.hpp` | OSQP-class ADMM (certified infeasibility, deterministic adaptive-ρ, **modified-Ruiz equilibration + cost normalization — OSQP §5, default-on, 54→31 iters on the scoreboard QP**, sign-checked polish) · Mehrotra IPM · Goldfarb-Idnani — ONE OSQP form, uniform duals, three-way cross-adjudication |
| LP | `lp.hpp` | bounded-variable revised simplex (two-phase, Bland anti-cycling, bound flips) · Mehrotra-at-P=0 |
| Conic | `conic.hpp` | SCS-class ADMM (Zero/Nonneg/SOC/**PSD-via-eig_sym** cones, duality-gap termination, conic infeasibility certificates) |
| NLP ⭐ | `nlp_sqp.hpp` `nlp_auglag.hpp` `nlp_interior_point.hpp` | damped-BFGS SQP (GI subproblems) · PHR auglag (L-BFGS inner) · the **Wächter-Biegler filter IPM** — one battery (HS6/HS14/disk/circle) through all three |
| Modeling | `model.hpp` | `Model<T>` — declarative vars/objective/constraints as scalar-generic lambdas, exact forward-AD derivatives, deterministic dispatch (wiring gate = bit-identical to direct calls) |
| Derivative-free | `nelder_mead.hpp` `powell.hpp` `pattern_search.hpp` `cobyla.hpp` `newuoa.hpp` `bobyqa.hpp` | NM (scipy semantics + Gao-Han) · Powell+Brent · GPS/OrthoMADS(Philox) · **the three Powell-code ports, DIFFERENTIALLY VERIFIED vs the compiled NLopt oracle: COBYLA 2050/0 · NEWUOA 3773/0 · BOBYQA 3045/0** (per-routine bit-exact + e2e identical eval counts; `scripts/setup-nlopt-ref.sh`) |
| Global | `cmaes.hpp` `global_search.hpp` | **Hansen-tutorial-faithful CMA-ES incl. ACTIVE CMA (negative weights, pycma's default, default-on)** · DE (scipy best/1/bin) · PSO · SA · basin-hopping (AdaptiveStepsize + the **LbfgsFd local mode** = scipy's default L-BFGS+2-point-FD minimizer w/ the factr flat-f exit) · multi-start — all Philox-deterministic (`crd-hesap-stats/normal.hpp` Box-Muller) |
| MIP | `mip.hpp` | branch and bound over the v7-l simplex (best-bound + most-fractional, deterministic ties, PROVEN optima; exhaustive-oracle-scanned) |
| CLI | `cli_anchor.hpp` + `src/cli_register_opt.cpp` | **`hesap.opt.{qp,lp,mip,conic}.f64`** — the data-defined families on the command layer (the nonlinear members need callables and stay API-level) |

\* = NOT in the `opt.hpp` umbrella (the hesap-opt→hesap-direct edge is explicit-include + explicit-link).

Honest scope notes (named, not hidden): QP/LP/conic/MIP are DENSE scope (the sparse backend lands with the
MPC consumer); no Gomory cuts (simplex-tableau access not exposed); no dual-annealing Tsallis machinery; no
restoration phase in the filter IPM; NEWUOA is the classic unconstrained algorithm (the reference's
MMA-nested bound variant excluded — bounds are BOBYQA's job); the modeling layer dispatches Hessian-free
members only.

## Nonlinear least-squares (v7-e-1, shipped — sparse + bench = v7-e-2)

- **`ResidualFunction<T>`** — a vector residual r(x)∈R^m + dense row-major Jacobian J=∂r/∂x (capability flag),
  distinct from `Objective<T>` because Gauss-Newton needs J (JᵀJ ≈ ∇²f). Vtable append-at-end; reserved sparse-J +
  fused slots.
- **`minimize_levenberg_marquardt<T>`** — damped normal equations (JᵀJ+λ·diagJᵀJ)δ=−Jᵀr with Marquardt scaling and
  the **Madsen-Nielsen-Tingleff ν damping update** (trust-region gain ratio — the lmder/Ceres rule, so iteration
  count is comparable to the gold standard); inline SPD Cholesky. **`minimize_gauss_newton`** = λ≡0. Robust losses
  (Huber/Cauchy/Tukey) via IRLS reweight. ⚠ `OptResult::fx` = ½‖r(x*)‖²; `grad_norm` = ‖Jᵀr‖∞.
- **`minimize_levenberg_marquardt_sparse`** (v7-e-2, NOT in the umbrella — include explicitly, link crd-hesap-direct):
  sparse Jacobian → JᵀJ (transpose+spgemm) → the **moat-proven hesap-direct `SupernodalCholesky`** factor+solve. The
  crush vehicle — the supernodal factor beat CHOLMOD (v5a) and Ceres-sparse *is* CHOLMOD, and it carries the
  cross-thread `{1..16}` determinism moat (bit-identical trajectory across worker counts) Ceres lacks. Honest claim:
  matched iters + per-iter factor faster + the moat, not "beat Ceres at NLS generally."
  - **GATE DONE (symbolic-once / numeric-per-trial).** `SupernodalCholesky::factorize(..., reuse_symbolic)` wraps only
    the expensive symbolic phase (AMD + etree + supernode amalgamation — the v5a CHOLMOD-gap cost) in `if(!reuse)`;
    the cheap O(nnz) rebuilds (panel layout, update-lists, etree levels) rerun every call (negligible vs the numeric
    factor in the factorization-dominated regime). `refactorize(pattern, values, num_workers)` reuses the stored
    `m_sym` for a numeric-only re-factorization on a structurally-identical pattern (a `CRD_ASSERT` guards the
    fixed-sparsity contract). The driver analyzes once and refactorizes every λ-trial — symbolic paid once across the
    whole solve (Ceres caches its symbolic the same way). Verified **bit-identical**: `reuse=false` byte-unchanged
    (v5a moat green), `refactorize == fresh factorize`, and the sparse-LM `{1..16}` moat survives the reuse
    (`test_supernodal.cpp` v7-e-2 gate test + `test_lm_sparse.cpp`).
  - **Bench DONE (vs CHOLMOD on the actual NLS JᵀJ) — honest, structure-dependent verdict.** `dump_nls_lattice_jtj`
    generates a 3D elastic-lattice NLS JᵀJ (genuinely NLS, deformation/FEA-class) → fed to the existing
    `bench_hesap_cholesky_vs_cholmod` (`CRD_BUILD_HESAP_VS_CHOLMOD`; CHOLMOD = what Ceres-sparse uses). **Result:
    the per-factor win is STRUCTURE-DEPENDENT.** Cerid BEATS CHOLMOD on thin-walled FEA JᵀJ (hood 1.53×, ldoor
    1.57×, bcsstk25 1.66×) but LOSES on the high-fill 3D-volume lattice (0.6–0.8× across n=5K..98K, 1T & 8T).
    Root cause (deep dig, dig in `docs/sessions/2026-06-07-v7e2-gate-and-nls-cholmod-bench.md`): NOT amalgamation
    (histograms identical), NOT skinny-K (cmod 96% fat-K), **NOT asm** (ADR-0088 built+reverted hand-tuned asm:
    ≈0.98× intrinsic — asm tuned the GEMM, the WRONG kernel), and NOT a per-thread "kernel wall." CHOLMOD verified
    genuinely single-thread (`taskset -c 0`: pinning to one core leaves its time unchanged; cc.fl=210 GFLOP/3.19s =
    66 GF/s single-core): clean **1-thread** rates are nls_lat32 **0.59×** (Cerid 38.6 vs CHOLMOD 65 GFLOP/s) but
    hood **0.97× parity** — the gap is **panel-size-dependent**. Decomposition: the loss is the
    **within-supernode PANEL FACTORIZATION (cdiv), and 83% of cdiv is the below-diagonal TRSM** (35% of the whole
    factor) at ~40 GF/s vs **OpenBLAS dtrsm 65** (the code's own v5a-4 comment). The cmod GEMM is the *competitive*
    path (≈0.7× OpenBLAS). Large-supernode dense-3D ⇒ big panel factorization ⇒ TRSM dominates ⇒ loss; small-panel
    FEA ⇒ panel factor negligible ⇒ parity/win. **2026-06-11 UPDATE (the lattice kernel dig, session
    `2026-06-11-lattice-kernel-crush.md`): the serial gap was ⅓ wall, ⅔ fixable.** Three levers landed: the
    `syrk_lower_minus` elementwise-pack pathology (rebuilt as a Goto-blocked triangular gemm — gemm-value-identical,
    heals a latent serial-vs-parallel value divergence at knc>256), the ColMajor merge stride fix (bit-identical),
    and the below-outer TRSM in bit-identical in-place wide-N RowMajor form. **lat32 serial 0.73× → 0.85-class;
    lat24 8T = 0.99× PARITY; FEA improved (hood 1.57×, bcsstk25 1.85× WIN); residuals unchanged.** The ONE named
    remaining lever for lat28/32: a dedicated packed-TRSM driver (the B1 M=64-skinny shape is B-restream
    bandwidth-bound at ~50 GF/s; OpenBLAS-style fused solve kernel, C-level, NOT asm). Honest claim: matched
    accuracy + universal `{1..16}` moat + per-factor wins on FEA + 8T parity through lat24 —
    **NOT yet** "crush Ceres-sparse on dense-3D." scipy `least_squares` = correctness cross-check only.

## Determinism moat

The optimizer is serial scalar arithmetic; the objective eval may be parallel but bit-exact (`ParallelSparseLinearOp`
/ bit-exact reduction) ⇒ the trajectory + result are bit-identical across {1..16} workers — tested `[moat]` per
slice (asserting convergence too, so the moat isn't vacuous over a maxed-out run). ⚠ stochastic (v7-i): optimizer-
update determinism is in-scope; full shuffled-minibatch reproducibility needs the v12-stats counter-RNG.

## The FULL-CRUSH scoreboard (2026-06-11; pinned problems, both sides same machine)

Final verdict rows after the crush pass (Ruiz ADMM · scipy-coupled Powell · BH LbfgsFd · active CMA · the
live torch row — session `2026-06-11-v7-full-crush.md`): QP obj EXACT = OSQP/quadprog, ADMM **31 iters vs
OSQP 25** (was 54) · LP/SOCP/MIP exact, proven · NM/trust-ncg/trust-exact **identical scipy trajectories** ·
Powell **494 vs scipy 792 evals (WIN)** · GLTR **39 vs trust-krylov 401 (WIN)** · DE **finds the Rastrigin-4
global, scipy defaults stall at 0.995 (WIN)** · BH **9624 vs 8881 evals (parity)** · CMA ros5 **1816 vs
pycma 2254 (WIN)**, sph8 1590 vs 1391 (1.14×) · **torch Adam + AdamW: 12-digit-identical 200-step
trajectories** (ours ~0 ms vs torch 22–780 ms) · **IPOPT (landed 2026-06-11; 3.11.9 + cyipopt installed):
Rosenbrock-in-disk f 0.045674808/16 iters vs Cerid filter IPM f 0.045674809/22 iters, x identical to 7
decimals — solution agreement + iteration parity-class; no pending reference rows remain**. Plus by
reference: L-BFGS-B eval-parity vs the exact code scipy wraps; the three NLopt ports bit-exact (8868 oracle
checks). And over it all the {1..16} determinism moat no peer carries. The hesap-direct lattice frontier
moved the same day (syrk-pack root fix: lat32 serial 0.73→0.85-class, lat24 8T parity — §NLS above); its one
named remaining lever is the packed-TRSM driver.

## Edges (acyclic; ADR-0090)

`crd-hesap-opt → crd-hesap` (LinearOp + cli) · `crd-hesap-dense` (Vector/blas1) · `crd-hesap-sparse`
(Parallel/SparseLinearOp) · `crd-jobs`. Later: `→ crd-hesap-direct/iterative` (Newton/KKT solves) · `→ crd-hesap-eig`
(exact trust-region). Reverse-mode autodiff plugs into the same `Objective` gradient interface when it ships.

## Status

**ALL v7 content slices shipped (a → r, 2026-06-10).** Opt suite 3711+ asserts / 145+ cases; hesap-stats
230660/7; 4-config + guards green per slice. Verification style: sharp algorithm-specific gates (one-step
exactness, KKT certificates checked directly, the acceleration theorem, quadratic-exactness at full npt,
published HS optima, scipy reference points), cross-adjudication of INDEPENDENT algorithms on shared Philox
families (three QP solvers · simplex-vs-IPM · conic-vs-simplex · B&B-vs-exhaustive-enumeration), differential
harnesses vs compiled reference oracles for the four faithful ports (L-BFGS-B + the three NLopt Powell
codes), and run-twice / {1..16} bit-identity moats. Gate-caught real bugs are recorded in the phase rows
(τ-stuck-at-β, the GI J-transpose, the SQP p=0 stale multipliers, the MIP dangling pointer, the Ruiz-exposed
polish NaN/dual-sign acceptance, the BH-LbfgsFd missing flat-f exit, …). v7-z CLOSED (CLI + scoreboard +
docs), then the **FULL-CRUSH pass (2026-06-11)** closed every scoreboard gap honestly — see the scoreboard
section above. Suite 3782/152 (+ stats 230660/7); 4 configs + guards green. User-side remainder: the IPOPT
row, the v7 commit, the 18-config CI sweep.
