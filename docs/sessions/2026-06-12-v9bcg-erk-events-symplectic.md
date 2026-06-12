# 2026-06-12 — v9-b + v9-c + v9-g batch: embedded ERK + events/dense-output + symplectic

**Slices:** Phase 3.1.6 v9-b (embedded explicit RK), v9-c (events + continuous output), v9-g (symplectic) —
implemented as ONE batch per user direction ("implement as many slices as you can, then test and fix in
batches"). ADR-0091 contracts from v9-a consumed unchanged (no retrofits — the day-1 contracts held).

## v9-b — `erk.hpp`: scipy-EXACT embedded adaptive RK

- **Semantics read verbatim from the installed scipy 1.17.1** (`_ivp/rk.py`, `_ivp/common.py`): the
  `rk_step` stage loop (f_new evaluated EVERY attempt, K[s] slot), Hairer `select_initial_step` (order =
  error-estimator order, the d1/d2 ≤ 1e-15 branch included), the `_step_impl` loop (min_step =
  10·|nextafter|, t_bound clamp, scale = atol + max(|y|,|y_new|)·rtol, RMS norm, the v9-a
  ElementaryController), and DOP853's combined 5th/3rd error norm.
- **Tableaus EXTRACTED, not transcribed**: `scripts/gen_erk_tableaus.py` reads RK23/RK45/DOP853 (A/B/C/E,
  E3/E5) out of the installed scipy arrays and emits `detail/erk_tableaus.hpp` at 17 significant digits.
  Cash-Karp = exact rational expressions; Tsit5 = Tsitouras 2011 in the same 6-stage FSAL frame as RK45.
- Driver `integrate_erk` (methods enum, `OdeOptions` + new `atol_vec`, optional `OdeSolution*` recording,
  optional events span); deterministic counters; backward integration; MaxSteps/StepTooSmall/NotFinite.

⭐⭐ **THE TRAJECTORY-EXACTNESS GATE** (`runtime/examples/ode_scipy_difftest.cpp` vs
`scripts/ode_scipy_ref.py`, WSL, identical pinned problems):

| case | Cerid naccept/nfev | scipy naccept/nfev | final y |
|---|---|---|---|
| VdP μ=1 RK23 rtol 1e-8 | 2177 / 6533 | **2177 / 6533** | 14 digits |
| VdP RK45 | 168 / 1106 | **168 / 1106** | y[0] **BIT-IDENTICAL** |
| VdP DOP853 | 55 / 926 | **55 / 926** | 14 digits |
| decay RK45 rtol 1e-10 | 130 / 782 | **130 / 782** | **BIT-IDENTICAL** |
| decay DOP853 | 17 / 206 | **17 / 206** | **BIT-IDENTICAL** |

Identical step sequences across thousands of controller decisions on three methods including the 12-stage
DOP853 — the ODE analog of v7's NM 219=219. (Final-state bit differences where present = numpy pairwise
sums vs our serial loops, named.)

## v9-c — `solution.hpp` + `events.hpp` + `detail/brentq.hpp`

- `OdeSolution<T>`: contiguous (t, y, f) nodes appended per accepted step; direction-aware deterministic
  binary search; Hermite eval between nodes (the v9-a contract; **native per-method interpolants = the
  named follow-up** — the gates encode the Hermite h⁴/384·|y⁗| bound explicitly).
- `OdeEvent<T>` (value/direction/terminal/hits contract + functor adapter) with scipy semantics: sign
  change incl. zero endpoints, direction filtering, brentq refinement at 4·eps over the step interpolant,
  terminal truncation (y ← interp(t_event), status `EventTerminal`, `OdeResult::event_index`).
- Gates: projectile ground-hit at the ANALYTIC t* = √(2y₀/g) (1e-9) · direction-filtered sin crossings at
  π (down) and 2π (up) with hit recording · dense-output exactness at the Hermite bound · backward-
  trajectory segment lookup.

## v9-g — `symplectic.hpp` (the eylem layer)

- Kernel-style (raw-span, allocation-free): `step_symplectic_euler` (kick-drift) ·
  `step_velocity_verlet` (FSAL acceleration, 1 force eval/step) · `step_composition` (palindromic
  velocity-Verlet substeps) with `yoshida4_w` + `yoshida6_w` (Yoshida 1990 solution A). Yoshida-8 = a
  later append (constants fetched, not recalled — the honesty rule).
- Gates: Kepler order slopes **2 / 4 / 6** · ⭐ the ENERGY gate at the game-relevant h = 0.2 over ~1590
  orbits: Verlet |ΔE| BOUNDED with first-tenth max == last-tenth max (no secular growth) while same-h RK4
  drifts past 3× Verlet's bound (at h = 0.05 RK4 shows NO disadvantage in 1500 orbits — measured, the
  honest framing: the symplectic win is a large-h/long-horizon property) · time-reversibility (forward
  1000 + backward 1000 returns to start < 1e-9) · Yoshida-6 run-twice bit identity.

## Batch test-and-fix round (3 gate corrections, 0 engine bugs)

1. Dense-output gates initially demanded integrator-tolerance accuracy from the HERMITE interpolant —
   wrong error model (at rtol 1e-10 on smooth problems RK45 takes h≈0.3 steps; interp error is O(h⁴)).
   Fixed: hmax-capped tests gated at the computed h⁴/384·|y⁗| bound.
2. The energy gate at h = 0.05 was unwinnable — RK4 too accurate over that horizon (measured 3.9e-4 vs
   Verlet 6.8e-4). Moved to h = 0.2 where the symplectic property actually manifests.
3. My comment arithmetic 0.1⁴/384 ≈ 2.6e-8 was 10× off (2.6e-7); gates now carry the right numbers.

Also: 2 nested-ternary + 1 round-by-cast tidy violations fixed (one in `erk.hpp` event firing).

## Verification (additive-module scope: ode suite + guards; CI owns the sweep)

**279 asserts / 24 cases** green on: win-debug · win-shipping · win-asan · win-tidy (build) · WSL gcc
release. All 6 guards green. scipy difftest output above (WSL gcc).

## v9-d — BDF/NDF (same day, batch round two)

`bdf.hpp` (scipy `bdf.py` verbatim: NDF κ-constants, D-array, compute_R/change_D, the 4-iter rate-predicate
Newton, order selection — INCLUDING the stale-LU-after-rejection quirk; a first-draft refactor-on-c-change
condition was caught against the source and removed) + **`ode_linear_solver.hpp` — THE SEAM** (dense
hesap-dense LU now, sparse/Krylov at v9-j; the module's first hesap-dense edge). Analytic-Jacobian policy =
scipy `jac=callable`; plain-FD fallback (named non-num_jac divergence).

⭐⭐ **BDF TRAJECTORY EXACTNESS — every counter identical to scipy** (analytic Jacobians both sides):

| case | Cerid naccept/nfev/njev/nlu | scipy | y |
|---|---|---|---|
| ROBER t=100, rtol 1e-6 | 163 / 431 / 5 / 37 | **identical** | 15 digits |
| VdP μ=1000 t=300 | 79 / 174 / 4 / 24 | **identical** | 14 digits |

njev and **nlu** matching means the Jacobian-refresh and LU-refactor decision sequences are exact — the
deepest semantic verification short of bit-level.

⭐ **THE STIFF CRUSH GATE** (what a stiff solver is FOR): VdP μ=1000 at the same tolerance — **BDF 174
evals (79 steps, 24 LU) vs RK45 1,642,370 evals (234,645 steps) = 9,439× fewer evaluations.**

Other gates: stiff-linear exact solution (fast mode dies cleanly to < 1e-10) · ROBER at the 1e5 horizon on
the SUNDIALS-published decay curve (y1 ∈ (0.005, 0.04); my first sanity band was wrong — the solver was
right, 0.0179 sits exactly on the literature curve) + conservation < 1e-7 · FD ≡ analytic ·
run-twice bit identity over all counters · backward integration. Suite **341 asserts / 30 cases**;
debug/shipping/asan/tidy/WSL-gcc + all 6 guards (the non-ASCII guard caught em-dashes in two test names —
working as designed).

## v9-e — Radau IIA(5) (same day, batch round three)

`radau.hpp` scipy-verbatim: collocation in the eigenbasis (real + COMPLEX LU per Newton iteration — the
hesap-dense `Complex<f64>` LU edge), 6-iter Newton on W=TI·Z, LU_real-stabilized error + rejected-step
re-stabilization, the Gustafsson predictive controller with the keep-LU rule, collocation dense output
(= the Z0 warm start, part of the trajectory). Constants extracted at 17 digits. Port subtlety caught:
scipy's `h_abs_old` stores the PRE-step `self.h_abs`, not the accepted value.

⭐⭐ **RADAU TRAJECTORY EXACTNESS — every counter identical** (analytic Jacobians):

| case | Cerid naccept/nfev/njev/nlu | scipy | y |
|---|---|---|---|
| ROBER t=100 | 88 / 726 / 22 / **110** | **identical** | 16 digits |
| VdP μ=1000 t=300 | 29 / 233 / 5 / 40 | **identical** | y[0] **bit-identical** |

nlu=110 matching means even the real+complex factorization-pair bookkeeping (the keep-LU heuristic
decisions) is exact. Gates: L-stable fast-mode kill (λ=−10⁴ → <1e-12 cleanly), ROBER 1e5
conservation+published-curve, stiff crush >100× vs RK45, run-twice bit identity. Suite **358/34**;
debug/shipping/asan/tidy/WSL-gcc + all guards green.

## v9-f — RODAS4 + TR-BDF2 (same day, batch round four) — ⭐⭐⭐ A REAL BOOST.ODEINT BUG FOUND AND FIXED

`rosenbrock.hpp` = RODAS4, Boost.odeint-verbatim (extracted coefficients, the odeint controller verbatim,
J+LU per attempt, the v9-d seam via (I−γh·J)g = γh·r). The order gate failed at p̂ = 1.04 on the
non-autonomous probe (y′ = −2ty²) — the bisection protocol:
1. AUTONOMOUS twin (y′ = −y², same y(1) = 1/2): p̂ = 4.07 ⇒ stage algebra correct, dfdt path suspect.
2. **odeint ITSELF probed** (analytic jac + dfdt, five h-decades): p̂ = 1.0385 → 1.0002, e1 DIGIT-IDENTICAL
   to our port (0.0011252) ⇒ the port is a perfect replica — of a buggy reference.
3. Hypothesis: odeint's d4 = +0.0362…23 vs Hairer rodas.f D4 = **−0.0362…23** (a dropped sign, invisible
   when ∂f/∂t = 0). Flipped ⇒ **p̂ = 4.15, error 10,000× smaller at the same h.** NAMED divergence,
   gate-locked (the order test runs BOTH probes forever); upstream-report candidate.

`sdirk.hpp` = TR-BDF2 as a stiffly-accurate 3-stage ESDIRK (γ = 2−√2 closed forms, the ARKODE table,
Σb̂ = 1 verified) — both implicit stages share ONE iteration matrix; simplified Newton (BDF machinery);
the SPICE/ode23tb-class circuit workhorse (the DAW consumer) and the ESDIRK core v9-i IMEX reuses.

Gates: slopes 4.07/4.15/2.x · L-stable fast-mode kill · Robertson conservation · **the bounded-cost
contracts as assertions** (RODAS4 nlu == attempts ∧ nsol == 6·attempts — no Newton, the real-time
property; TR-BDF2 nlu ≪ nsol — the shared-matrix economy) · stiff crush >100× vs RK45 · bit identity ·
FD ≡ analytic. Suite **368 asserts / 40 cases**; debug/shipping/asan/tidy/WSL-gcc + all guards green.

## FIRST WALL-CLOCK BOARD (user-demanded, ahead of v9-z) — `runtime/examples/bench_ode_vs_refs.cpp`

WSL g++ -O2, pinned core (taskset -c 4), best-of-30/50, analytic Jacobians, matched tolerances, achieved
error vs a rtol-1e-12 Radau reference. Setup timed on BOTH sides (the one-shot-solve pattern).

**ROBER t=1e5 (rtol 1e-6 / atol 1e-10):** Cerid BDF **0.063 ms** / 895 nfev / 68 nlu / err 1.1e-7 ·
Cerid RODAS4 **0.045 ms** / err 4.1e-11 · Cerid Radau 0.067 ms / err 4.8e-10 · **CVODE 0.371 ms** / 968 /
100 / err 8.0e-8 ⇒ **BDF 5.9× and RODAS4 8.2× FASTER than CVODE** (RODAS4 with 3 decades better accuracy).

**VdP μ=1000 t=300 (rtol 1e-6):** Cerid RODAS4 **0.006 ms** · Radau 0.007 ms (err 3.8e-11) · BDF
**0.010 ms** (err 1.2e-5) · **CVODE 0.058 ms** (err 1.3e-5) ⇒ **5.8× faster at the same accuracy class.**

**NONSTIFF VdP μ=1 t=100 (rtol 1e-8) vs Boost.odeint dopri5:** Cerid DOP853 **0.087 ms** / 9206 nfev /
err 1.6e-8 · Cerid RK45 **0.133 ms** / 10964 · odeint dopri5 0.137 ms / 12973 / err 3.8e-7 ⇒ **win on
wall AND evals; DOP853 1.6× faster with 24× better error.**

HONEST caveats (named): TR-BDF2 at rtol 1e-6 long-horizon = outside its order-2 design envelope (296 ms /
5.7M evals on ROBER — correct, err 4.4e-9, but the wrong tool there; its fair curve = the v9-z
work-precision sweep + the moderate-tolerance real-time regime, exactly ode23tb's home) · tiny-n systems ⇒
setup/indirection overheads matter (part of CVODE's gap = its heap/NVector machinery vs our TLSF — a real
architectural advantage, but v9-z adds a large-n MOL row where per-eval work dominates) · single host,
±10% noise; the exactness-proven eval counts are the noise-free metric.

## Next

v9-h mass/DAE → v9-i IMEX (KenCarp over the TR-BDF2 ESDIRK shape) → v9-j sparse/Krylov → v9-k
sensitivities → v9-z (CLI + THE work-precision scoreboard vs CVODE/scipy/odeint + Hairer oracles +
Testset anchors + the large-n MOL wall row). Named follow-ups: native RK45/DOP853 interpolants, Verner,
Yoshida-8, BDF/Radau events, the odeint d4 upstream report.
