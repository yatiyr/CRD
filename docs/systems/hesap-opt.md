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

## Method set (planned, consumer-pull sequenced)

Spine (consumer-blocking): a substrate ✅ → b derivatives (FD/forward-AD) → c line searches → **d L-BFGS** →
**e nonlinear-LS (LM/Gauss-Newton, robust losses; vs Ceres)** + j constraint-substrate → **k QP (OSQP-class)** →
**n NLP (SQP / IPOPT-class)**. Then f first-order · g Newton/Newton-CG · h trust-region (incl. exact via hesap-eig)
· i stochastic (Adam/AdamW/…) · l LP · m conic (SCS) · o algebraic-modeling. Slip-candidates (no named consumer
yet): p derivative-free · q global (CMA-ES) · r MIP. v7-z = CLI `hesap.opt.*` + gold-standard bench + system-doc
+ ADR + sweep.

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
    FEA ⇒ panel factor negligible ⇒ parity/win. **THE LEVER (C-level, moat-safe): a better-blocked / recursive
    POTRF + the below-diagonal TRSM (LAPACK dpotrf/dtrsm class) — already improved once (v5a-4: 26→40; OpenBLAS 65);
    NOT asm.** Reach: fixing the panel TRSM takes the lattice ~0.59→0.75–0.8×; closing the secondary gemm-framework
    gap (Goto packing in C) → ~parity, with the `{1..16}` moat the differentiator. (Linking OpenBLAS wins speed but
    forfeits the moat.) Honest claim: matched accuracy + universal `{1..16}` moat + a per-factor win on thin-walled
    FEA — **NOT** "crush Ceres-sparse" (false on dense-3D today). scipy `least_squares` = correctness cross-check only.

## Determinism moat

The optimizer is serial scalar arithmetic; the objective eval may be parallel but bit-exact (`ParallelSparseLinearOp`
/ bit-exact reduction) ⇒ the trajectory + result are bit-identical across {1..16} workers — tested `[moat]` per
slice (asserting convergence too, so the moat isn't vacuous over a maxed-out run). ⚠ stochastic (v7-i): optimizer-
update determinism is in-scope; full shuffled-minibatch reproducibility needs the v12-stats counter-RNG.

## Edges (acyclic; ADR-0090)

`crd-hesap-opt → crd-hesap` (LinearOp + cli) · `crd-hesap-dense` (Vector/blas1) · `crd-hesap-sparse`
(Parallel/SparseLinearOp) · `crd-jobs`. Later: `→ crd-hesap-direct/iterative` (Newton/KKT solves) · `→ crd-hesap-eig`
(exact trust-region). Reverse-mode autodiff plugs into the same `Objective` gradient interface when it ships.

## Status

v7-a substrate + v7-b derivatives + v7-c line searches + v7-d quasi-Newton (L-BFGS/BFGS/SR1 + bench) + v7-d-3
L-BFGS-B (reference-verified) + v7-e-1 nonlinear-LS (LM/GN/robust, dense) shipped. The spine continues: e-2
sparse-LM + the Ceres/scipy crush bench, then j→k→n constrained; the rest land per subslice.
