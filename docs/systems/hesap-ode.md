# crd-hesap-ode — ODE/DAE solvers

> Phase 3.1.6 v9. ADR-0091. Plan: `docs/phases/phase-3.1.6-hesap.md` (the v9 block).
> Status: **CLUSTER COMPLETE (a→z)** — sessions `2026-06-11-v9a-ode-substrate.md`,
> `2026-06-12-v9bcg-erk-events-symplectic.md` (b/c/d/e/f/g/h/j-sparse), `2026-06-13-v9-imex-krylov-sens-dae-batch.md`
> (i/j-Krylov/k/l/z). Explicit RK (`erk.hpp`: RK23/RK45/DOP853 — tableaus extracted from scipy, **step
> sequences proven identical** via `ode_scipy_difftest` — + Cash-Karp + Tsit5) · events + dense output
> (`solution.hpp`/`events.hpp`/`brentq`) · BDF/NDF (`bdf.hpp`, **scipy-counter-exact**) · Radau IIA(5)
> (`radau.hpp`, scipy-counter-exact) · Rosenbrock RODAS4 + TR-BDF2 (`rosenbrock.hpp`/`sdirk.hpp` — found+fixed
> a real odeint d4-sign bug) · symplectic (`symplectic.hpp`) · mass-matrix/index-1 DAE (`bdf.hpp` mass path)
> · sparse + matrix-free Krylov Newton (`ode_sparse_solver.hpp`/`ode_krylov_solver.hpp` = CVODE-KLU / SPGMR)
> · IMEX additive RK (`imex.hpp`: ARK3/4/5 extracted from ARKODE) · sensitivities (`sensitivity.hpp`: forward
> + adjoint, CVODES patterns) · higher-index DAE (`dae_structural.hpp` Pryce Σ-method + `dae.hpp` mechanical
> index reduction) · CLI (`hesap.ode.solve.f64`).

## What it is

Initial-value ODE/DAE integration for every Cerid domain: eylem fixed-step symplectic (ADR-0063 replay),
robotics/control sensitivities (→ v7 opt shooting/MPC), CFD/FEA method-of-lines stiff systems (Newton over
hesap-dense/-direct/-iterative), DAW virtual-analog (real-time Rosenbrock), gameplay scripting (events,
budgeted adaptive integration). Gold standards: SUNDIALS (CVODE/IDA/ARKODE/CVODES), scipy `solve_ivp`
(trajectory-exact gates — readable steppers), Hairer Fortran (DOPRI5/DOP853/RADAU5/RODAS), Boost.odeint.
The scoreboard format is the **work-precision diagram** (error vs nfev vs wall at sweeping rtol).

## The two API layers (ADR-0091 §2 — the load-bearing design)

| Layer | Header | Contract | Consumers |
|---|---|---|---|
| **Kernels** | `steppers.hpp` | Raw-span, allocation-free, caller scratch (`*_scratch(n)`), inlined callable RHS, in-place safe, fixed per-element FP order (bit-deterministic) | eylem fused SoA sweep, animation springs, DAW per-sample — hot loops, zero virtual dispatch |
| **Driver** | `integrate.hpp` + `ode_function.hpp` | `OdeFunction<T>` capability interface (locked vtable, append-at-end; jacobian/jac-vec optional, mass at v9-h, sparse-jac at v9-j) + `OdeOptions`/`OdeResult`; the driver owns ALL work counting | scripts, engineering, cinematic, CLI |

Never call the driver in a per-body hot loop (virtual RHS per substep = the anti-pattern); never hand-roll
integration math in a consumer (the kernels are the proven, gated formulas).

## Shipped surface (v9-a)

- `steppers.hpp` — `step_euler` / `step_midpoint` / `step_rk4` (orders 1/2/4) + `*_scratch(n)` sizes.
- `ode_types.hpp` — `OdeStatus`, `OdeWork` (nsteps/naccept/nreject/nfev/njev/nlu/nsol — deterministic, the
  work-precision currency, CVODE semantics), `OdeOptions<T>` (scipy-default rtol 1e-3 / atol 1e-6, optional
  per-component atol, h0/hmax/max_steps), `OdeResult<T>` (flat POD; state lives in the caller's span).
- `ode_function.hpp` — `OdeFunction<T>` + `FunctorOdeFunction`/`make_ode_function` lambda adapter.
- `controller.hpp` — `error_norm_wrms` (Hairer II.4 (4.11) ≡ scipy; fixed-order serial reduction) +
  `ElementaryController` (scipy-exact: accept err<1 strictly, post-rejection growth cap, 0.9/0.2/10
  constants) + `PiController` (Hairer/Gustafsson form: accept err≤1, facold floor 1e-4, never grow on
  reject). Pure deterministic FP functions — no wall-clock, no thread state.
- `dense_output.hpp` — the continuous-output contract (per-step fixed-width coefficient blocks in
  caller-owned storage + static eval) + `hermite_eval` (the method-agnostic cubic fallback: exact on
  cubics, exact endpoints, O(h⁴)).
- `integrate.hpp` — `integrate_fixed` (Euler/Midpoint/Rk4 over the kernels; recomputed `t_i = t0 + i·h`,
  final step lands on t1 exactly; per-step NotFinite check lives in the driver, NOT the kernels).

## Adaptive + stiff + structured surface (v9-b … v9-z)

- `erk.hpp` — `integrate_erk` (RK23/RK45/DOP853/Cash-Karp/Tsit5), scipy step-sequence-exact; the explicit
  spine + IMEX explicit-stage evaluator.
- `bdf.hpp` — `integrate_bdf` variable-order NDF, scipy-counter-exact; the stiff spine. Branches dense /
  sparse (`has_sparse_jacobian`) / matrix-free (`solver->is_matrix_free()`) / mass-matrix.
- `radau.hpp` / `rosenbrock.hpp` / `sdirk.hpp` — Radau IIA(5), RODAS4, TR-BDF2 (the real-time / SPICE / DAW
  stiff workhorses; complex LU edge for Radau).
- `imex.hpp` — `integrate_imex` (ARK3(2)4L[2]SA / ARK4(3)6L[2]SA / ARK5(4)8L[2]SA, Kennedy-Carpenter,
  coefficients extracted from SUNDIALS v6.4.1 by `scripts/gen_ark_tableaus.py` → `detail/ark_tableaus.hpp`,
  bit-identical to ARKODE). Explicit ⊕ implicit split through the `OdeFunction` slots 7/8/9 (rhs_explicit /
  rhs_implicit / jacobian_implicit); the implicit ESDIRK solves share one iteration matrix.
- `ode_linear_solver.hpp` — THE seam. `DenseOdeLinearSolver` (hesap-dense LU). `ode_sparse_solver.hpp`
  (`SparseOdeLinearSolver` over the v5b multifrontal LU = CVODE-KLU) and `ode_krylov_solver.hpp`
  (`KrylovOdeLinearSolver`, matrix-free FGMRES over `jacobian_vector` + `OdeKrylovPreconditioner`
  PrecSetup/PrecSolve = CVODE-SPGMR) slot in without touching the drivers.
- `sensitivity.hpp` — `ParametricOdeFunction<T>` + `integrate_forward_sensitivities` (CVODES simultaneous
  corrector: augmented `[y;S]` through the existing drivers, block-diagonal `J_y` for the stiff path) +
  `integrate_adjoint_sensitivities` (CVODES ASA: backward `[λ;q]` over stored dense output). Three-oracle
  gate (forward = adjoint = FD).
- `dae_structural.hpp` — `StructuralDae` + `structural_index` (Pryce Σ-method = the Pantelides-equivalent
  structural index + differentiation offsets). `dae.hpp` — `ConstrainedMechanicalSystem<T>` +
  `IndexReducedMechanicalOde` (index-3 → index-1 via the acceleration constraint + λ-elimination; the
  multibody/eylem consumer).
- `src/cli_register_ode.cpp` — `hesap.ode.solve.f64` (canned problems × the full method set; anchor
  `register_ode_cli_anchor`).

## Determinism

Kernels: element-independent loops, fixed per-element op order — bit-identical runs by construction.
Driver: recomputed times, deterministic counters; run-twice bit-identity is a standing gate (Lorenz
endpoint memcmp). Controllers: pure functions of the error estimate. From v9-j, worker-count bit-identity
inherits the hesap-direct/-iterative moats for parallel RHS/Jacobian/Newton.

## Tests

`tests/hesap-ode/` — `[ode]`: single-step amplification vs hand-computed RK polynomials (exact FP where
representable), empirical h-refinement order slopes 1/2/4 (the oracle-free order certificate), in-place
aliasing bit-equality, exact work counters (nfev == cost·steps), Lorenz run-twice bit-identity, status
paths (InvalidInput/NotFinite/backward integration), controller formula values + clamps + history, Hermite
cubic-exactness.
