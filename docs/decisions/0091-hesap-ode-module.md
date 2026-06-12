# ADR-0091 — `crd-hesap-ode`: the ODE/DAE module — two API layers, deterministic controllers, the work-precision contract

- **Status:** Accepted (2026-06-11)
- **Phase:** 3.1.6 v9 (ODE/DAE cluster)
- **Tags:** `hesap` `ode` `determinism` `architecture`
- **Plan:** `docs/phases/phase-3.1.6-hesap.md` (the v9 block); memory `project_v9_ode_plan`, `project_ode_in_games_layering`

## Context

v9 brings initial-value ODE/DAE solving to the hesap stack. The consumers span every Cerid domain: eylem
(fixed-step symplectic integration, the ADR-0063 replay-determinism contract), robotics/control
(sensitivities feeding the v7 optimizers for shooting/MPC), CFD/FEA method-of-lines (stiff systems whose
Newton solves pull hesap-dense/-direct/-iterative), DAW virtual-analog circuits (real-time stiff,
Rosenbrock), gameplay scripting (events, budgeted adaptive integration). The gold standards are SUNDIALS
(CVODE/CVODES/IDA/IDAS/ARKODE), scipy `solve_ivp` (readable Python steppers ⇒ trajectory-exact gates),
Hairer's Fortran codes (DOPRI5/DOP853/RADAU5/RODAS), and Boost.odeint (same-language wall-clock peer).

Two structural risks had to be settled before the first method shipped:

1. **The hot-loop conflict.** A games/animation/audio consumer integrates thousands of tiny systems per
   frame; a virtual RHS callback per body per substep is an anti-pattern. But scripts/engineering/CLI need
   a type-erased callback driver with options, events, and dense output. One API cannot serve both.
2. **Retrofit landmines.** Dense output (consumed by events, `t_eval`, sensitivity checkpointing, plotting)
   and the linear-solver seam (dense → sparse → Krylov Newton) refactor every method if added late —
   CVODE's SUNLinSol history is the cautionary tale.

## Decision

1. **New module `crd-hesap-ode`** (`engine/hesap-ode/`). v9-a edges: crd-core, crd-containers, crd-memory
   only. Stiff slices add hesap-dense (v9-d LU, v9-e complex LU) and hesap-direct/-iterative (v9-j) at the
   slice that consumes them. NO edge to hesap-opt (opt consumes ode later for shooting — acyclic).

2. **Two API layers** (the ADR-0078 §5 two-layer pattern applied to integration):
   - **Kernel layer** (`steppers.hpp`): raw-span, allocation-free, inlineable templated steppers with
     caller-owned scratch and an inlined callable RHS. In-place safe, fixed per-element FP order
     (bit-deterministic by construction). This is what eylem's fused SoA sweep, animation spring chains,
     and DAW per-sample loops consume — no virtual dispatch anywhere.
   - **Driver layer**: `OdeFunction<T>` (the v7 `Objective<T>` capability contract: queried `has_*`
     capabilities + optional virtuals returning filled/not-filled; **vtable LOCKED, appends at END only** —
     planned appends: mass matrix at v9-h, sparse Jacobian at v9-j), `OdeOptions`/`OdeResult`, and
     `integrate_*` drivers that own ALL work counting.

3. **Deterministic work counters are the scoreboard currency.** `OdeWork`
   (nsteps/naccept/nreject/nfev/njev/nlu/nsol, CVODE semantics) is maintained identically on identical
   runs; the v9-z work-precision diagrams (error vs nfev vs wall at sweeping rtol — the domain's native
   honest-comparison format) plot it against CVODE/scipy/odeint.

4. **Step controllers are pure deterministic FP functions** of the error estimate — no wall-clock, no
   thread-dependent state. Two types, each matching its reference exactly (they disagree on accept-at-1.0,
   so one parametrization would be dishonest): `ElementaryController` (scipy `RungeKutta` semantics, for
   the v9-b trajectory-exact gates) and `PiController` (the Hairer/Gustafsson DOPRI5 form with the facold
   floor, for the Fortran-oracle gates). The WRMS error norm (Hairer II.4 (4.11) ≡ scipy) is a fixed-order
   serial reduction.

5. **The dense-output contract is pinned at v9-a**: after an accepted step, a method writes a
   fixed-width interpolation-coefficient block into caller-owned contiguous storage; a static
   allocation-free eval reconstructs y(t) within the step. Methods with native interpolants (RK45 quartic,
   DOP853 7th-order, Radau collocation) provide their own; the method-agnostic fallback is the cubic
   Hermite over (y0, f0, y1, f1), shipped and gated in v9-a.

6. **Time steps are recomputed, not accumulated** (`t_i = t0 + i·h`; the final step lands on `t1`
   exactly) — no drift, fixed FP sequence.

7. **Scope pins** (honest, named): LSODA-style auto-switching is NOT planned (convenience heuristic;
   revisit on consumer pull). f32 instantiations ship only where honest (explicit RK; f32 stiff-Newton
   tolerances sit at eps(f32) — dishonest, named OUT). Event functions are integration options (v9-c,
   scipy-style), not `OdeFunction` virtuals.

## Consequences

- eylem/animation/DAW get zero-overhead integration primitives without linking the driver machinery;
  scripts/engineering/CLI get the full adaptive driver — one implementation under both.
- Every later method slots into contracts that exist from day 1 (controller, dense output, work counters),
  so v9-b..k add methods, not refactors.
- The determinism moat extends to integration: bit-identical trajectories run-to-run, and (from v9-j)
  across worker counts via the inherited hesap-direct/-iterative moats.
- The apt SUNDIALS 6.4.1 is the correctness/work-precision peer; wall-clock rows note its reference-BLAS
  linkage where relevant (the honest-asterisk rule).
