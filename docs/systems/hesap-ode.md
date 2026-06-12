# crd-hesap-ode — ODE/DAE solvers

> Phase 3.1.6 v9. ADR-0091. Plan: `docs/phases/phase-3.1.6-hesap.md` (the v9 block).
> Status: **v9-a substrate + v9-b embedded ERK + v9-c events/dense-output + v9-g symplectic shipped**
> (sessions `2026-06-11-v9a-ode-substrate.md`, `2026-06-12-v9bcg-erk-events-symplectic.md`). Shipped on
> top of the v9-a surface: `erk.hpp` (`integrate_erk`: RK23/RK45/DOP853 scipy-EXACT — tableaus extracted
> from the installed scipy by `scripts/gen_erk_tableaus.py`, **step sequences proven identical to scipy**
> via `ode_scipy_difftest` — + Cash-Karp + Tsit5), `solution.hpp` (`OdeSolution` Hermite dense output),
> `events.hpp` + `detail/brentq.hpp` (scipy event semantics, terminal truncation), `symplectic.hpp`
> (symplectic Euler / velocity Verlet / Yoshida-4/6 compositions — the eylem kernel layer). Remaining
> cluster: v9-d BDF → v9-e Radau → v9-f Rosenbrock/SDIRK → v9-h DAE → v9-i IMEX → v9-j sparse/Krylov →
> v9-k sensitivities → v9-z CLI + work-precision scoreboard.

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
