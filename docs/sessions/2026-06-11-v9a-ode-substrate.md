# 2026-06-11 — v9-a: `crd-hesap-ode` substrate (kernels + driver contracts)

**Slice:** Phase 3.1.6 v9-a (the first ODE/DAE slice). **ADR-0091 Accepted.** Plan: the v9 block in
`docs/phases/phase-3.1.6-hesap.md`; design inputs: memory `project_v9_ode_plan` +
`project_ode_in_games_layering` (the game-dev brainstorm that mandated the two-layer split).

## What shipped

New module `engine/hesap-ode/` (edges: core/containers/memory ONLY — hesap-dense arrives at v9-d/e,
hesap-direct/-iterative at v9-j, no edge to hesap-opt ever):

- **`steppers.hpp` — the KERNEL layer**: `step_euler` / `step_midpoint` / `step_rk4`, raw-span,
  allocation-free (caller scratch via `*_scratch(n)`), inlined callable RHS, in-place safe (y_out may
  alias y), fixed per-element FP order. This is the layer eylem's fused SoA sweep / animation /
  DAW per-sample loops consume directly — zero virtual dispatch.
- **`ode_function.hpp`**: `OdeFunction<T>` — the v7 `Objective<T>` capability contract (queried `has_*`
  flags + optional virtuals returning filled/not-filled). **Vtable LOCKED**: 0 dtor · 1 rhs · 2 dim ·
  3 jacobian (dense row-major) · 4 jacobian_vector; planned APPENDS: mass (v9-h), sparse_jacobian (v9-j).
  Events are integration options (v9-c), never virtuals. + `FunctorOdeFunction`/`make_ode_function`.
- **`ode_types.hpp`**: `OdeStatus`, **`OdeWork`** (nsteps/naccept/nreject/nfev/njev/nlu/nsol — CVODE
  semantics, deterministic, THE work-precision scoreboard currency), `OdeOptions` (scipy defaults rtol
  1e-3/atol 1e-6, optional atol vector, h0/hmax/max_steps), `OdeResult` (flat POD; state in caller span).
- **`controller.hpp`**: `error_norm_wrms` (Hairer II.4 (4.11) ≡ scipy; fixed-order serial reduction) +
  TWO controllers matching their references EXACTLY (they disagree at err == 1.0, so one parametrization
  would be dishonest): `ElementaryController` (scipy `RungeKutta`: accept err < 1 strictly, err == 0 ⇒
  max_factor, post-rejection growth cap at 1, 0.9/0.2/10) and `PiController` (Hairer/Gustafsson: accept
  err ≤ 1, facold floor 1e-4, never grow on reject). Pure deterministic FP functions.
- **`dense_output.hpp`**: the day-1 continuous-output CONTRACT (per-step fixed-width coefficient blocks
  in caller-owned contiguous storage + static allocation-free eval; native interpolants per method from
  v9-b) + `hermite_eval` (the method-agnostic cubic fallback).
- **`integrate.hpp`**: `integrate_fixed` (Euler/Midpoint/Rk4) — recomputed `t_i = t0 + i·h` (no
  accumulation drift), final step lands on t1 exactly, per-step NotFinite check in the DRIVER (kernels
  stay check-free), driver owns all counting.

## Gates (109 asserts / 11 cases, `[ode]`)

- **Kernel algebra**: single-step amplification == the hand-computed RK polynomials (exact FP where
  representable: euler 0.5, midpoint 0.625 bit-equal at hλ = −0.5); RK4-on-rotation == the degree-4
  Taylor rotation pair (c4, −s4).
- **Empirical order slopes** (the oracle-free certificate): y' = −2ty², halving h ⇒ p̂ ∈ {1, 2, 4} ± 0.25/0.35.
- **In-place aliasing**: y_out ≡ y produces bit-identical results to out-of-place (memcmp).
- **Exact work counters**: nfev == {1,2,4}·nsteps exactly, naccept == nsteps, nreject == 0.
- **Run-twice bit identity**: Lorenz (chaotic — divergence amplifies) 5000 RK4 steps, endpoint memcmp +
  counter equality.
- **Status honesty**: t1 == t0 zero-work success · n == 0 defined · zero-steps-over-span InvalidInput ·
  non-finite bounds InvalidInput · y' = y² blow-up reports NotFinite (10 steps — the exponent doubles per
  step past the t = 1 singularity; 4 steps only reached ~2e174, an honest test-side fix) · backward
  integration (t1 < t0) recovers e^{+1}.
- **Controllers**: hand-computed factor values, clamps both ends, scipy's strict-< vs Hairer's ≤ at
  exactly 1.0, post-rejection cap engage + clear, facold floor, deterministic sequences.
- **Hermite**: exact on two cubic components (interior samples), θ = 0 endpoint bit-exact.

Test-side fixes en route (no engine bugs): midpoint/RK4 accuracy tolerances were guessed tighter than the
measured truth (6.18e-6, 3.09e-11 — loosened to 1e-5/1e-10); the blow-up gate needed 10 steps.

## Verification (the additive-module scope, user-directed)

v9-a touches NO existing code (new module + two `add_subdirectory` lines), so per the user's call the
local pass is the ode suite + the GUARD tests per config; CI owns the full 18-config sweep post-commit.

| Config | Result |
|---|---|
| win-debug | 109/11 + ALL 6 guards green (no-malloc/no-std-math/no-std-sort/no-non-ascii/no-untagged/simd-emission — guards scan the new files) |
| win-shipping | 109/11; `CMAKE_COMMAND` audit = standalone cmake (landmine disarmed) |
| win-asan | 109/11 (DLL PATH protocol) |
| win-tidy | build clean (= tidy clean) |
| WSL gcc release | 109/11 (the `-Wfloat-conversion` discipline held — `static_cast<T>` everywhere) |

Gotcha noted: `crd-simd-emission-check` FAILS OUTSIDE vcvars (`dumpbin` not on PATH) — always run guards
through `scripts/run-ctest.bat`; a bare ctest call produces a phantom guard failure. Also cmd eats `|` in
regexes passed through the bat — run guard groups as separate `-R` calls.

## Next

v9-b: embedded explicit RK — RK23/RK45/DOP853 with scipy's exact semantics (trajectory-exact gates), Tsit5,
Cash-Karp, Verner, per-method dense output, Hairer auto-h0, the first Fortran-oracle harness.
