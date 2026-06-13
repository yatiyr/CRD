# 2026-06-13 — v9 remainder batch: IMEX · Krylov · sensitivities · Pantelides · close

> User direction: "as many slices as batches, ELITE + GOLD standard, NEVER DEFER ANYTHING, we test and
> bench at the END, FULL CRUSH." Advisor-adjusted into **two verification cadences**: the cheap
> deterministic correctness gates (trajectory/order/determinism) run **per-slice as I go** (that is how
> transcription bugs — the odeint d4-sign class — get caught before the next slice stacks on them); the
> expensive ceremony (the 4-config DoD sweep + the CVODE/ARKODE wall-clock work-precision scoreboard)
> **batches to v9-z**. Sequence: v9-i → v9-j(Krylov) → v9-k → v9-l → v9-z.
>
> Baseline confirmed before piling on `b578f66`: ode suite **405/48 green on win-debug**.

---

## v9-i — IMEX additive Runge-Kutta ✅ (code + per-slice gates green)

**Methods.** Kennedy-Carpenter ARK[2]SA pairs: **ARK3(2)4L[2]SA** (q=3,p=2), **ARK4(3)6L[2]SA**
(q=4,p=3), **ARK5(4)8L[2]SA** (q=5,p=4) — exactly the pairs ARKODE's ARKStep IMEX integrator ships, so
the v9-z work-precision comparison vs ARKODE is apples-to-apples.

**Extraction beats transcription.** `scripts/gen_ark_tableaus.py` fetches SUNDIALS' OWN X-macro coefficient
files (`src/arkode/arkode_butcher_{erk,dirk}.def`) at the pinned **v6.4.1** tag, parses each
`RCONST(p)/RCONST(q)`, and emits `detail/ark_tableaus.hpp` as the exact f64 expression `p.0/q.0` (p,q < 2^53
⇒ represented exactly ⇒ IEEE division reproduces ARKODE's coefficient bit-for-bit). The script ASSERTS the
ARK[2]SA shared-coefficient identity (ERK and DIRK halves share b/d/c) as a parse self-check — passed for
all three.

**Driver** (`imex.hpp`, `integrate_imex`). Additive stages: `Y_i = base_i + γh·f_I(t_i, Y_i)` with
`base_i = y + h·Σ_{j<i}(a^E_ij f_E[j] + a^I_ij f_I[j])`; all implicit stages share the ESDIRK diagonal γ ⇒
**ONE iteration matrix (I − γh·J_I) per step**. Simplified Newton reuses the v9-f/sdirk machinery (rate
predicate, Jacobian-refresh-then-h-halve on failure) through the v9-d `OdeLinearSolver` seam, evaluating
**f_I only**. Solution `y_{n+1} = y + h·Σ b_i(f_E[i]+f_I[i])`; embedded error `h·Σ(b_i−d_i)(f_E[i]+f_I[i])`.
**FSAL**: stage 0 is at the accepted point, so its f_E/f_I carry from the previous step's y_new (also the
dense-output node f) — one stage-0-worth of evals per accepted step. Controller = v9-a scipy
ElementaryController, exponent −1/(p+1). nfev counts half-evaluations (each f_E or f_I call = +1).

**OdeFunction extension** (append-at-end, locked vtable): slots **7 `rhs_explicit`**, **8 `rhs_implicit`**,
**9 `jacobian_implicit`** (a DEDICATED implicit-Jacobian slot — NOT overloading the slot-3 full Jacobian, so
an IMEX function is unambiguous in a non-IMEX driver) + capability flags `has_imex_split` /
`has_implicit_jacobian` with ctor-time setters. INVARIANT: `rhs` = f_E + f_I.

**Gates** (`test_ode_imex.cpp`, 55 assertions / 4 cases):
- **Per-part order slopes** — explicit half, implicit half, AND a genuine split, for each of ARK3/4/5.
  Measured: ARK3 **3.16 / 2.95 / 2.92**, ARK4 **4.01 / 4.00 / ~4**, ARK5 within [4.2,5.9]. Design note: the
  implicit halves use a **LINEAR f_I** so simplified Newton is exact in one iteration and never contaminates
  the order measurement (a nonlinear f_I + loose newton_tol would have capped the ARK5 slope) — the explicit
  half carries the nonlinearity. A transcription error in either tableau drops a slope ⇒ this is the d4-sign
  safety net.
- **L-stability**: slow explicit ⊕ stiff (−1e4) implicit — stiff mode → 0 (<1e-10) for all three methods.
- **Advection-diffusion MOL crush**: 1D periodic, N=64, ν=1 diffusion implicit; IMEX ARK4 matches monolithic
  RK45 to <1e-5 while using <⅓ the accepted steps (not stiffness-limited).
- **Determinism**: run-twice bit-identity (memcmp + nfev/nlu) ; **FD-vs-analytic J_I** agree to <1e-5.

**Verified:** win-debug build clean, ode suite **460/52** (was 405/48; +55/+4), no regression. 4-config sweep
+ ARKODE work-precision scoreboard deferred to v9-z per the cadence plan.

---

## v9-j (Krylov) — matrix-free Newton-Krylov ✅ (code + per-slice gates green)

**The CVODE SPGMR mode.** `KrylovOdeLinearSolver` (`ode_krylov_solver.hpp`) implements the `OdeLinearSolver`
seam matrix-free: the iteration matrix (I − c·J) is NEVER assembled — the inner Newton solve runs
hesap-iterative **FGMRES** on the operator `v ↦ v − c·(J·v)`, with J·v from `OdeFunction::jacobian_vector`
(slot 4, reserved since v9-a). O(n) memory — the large-n MOL path where even a sparse factor's fill is
prohibitive (CVODE-KLU vs CVODE-SPGMR, same fork).

**Seam additions** (append-at-end on `OdeLinearSolver`): `is_matrix_free()` (bool, default false) +
`factor_iteration_matrix_matfree(fn, t, y, c)`. **BDF driver**: a `use_matfree` branch — `build_jacobian`
records the linearization point (no dense J; `jac` array sized 0), the factor call sets up the operator, the
existing `solve` runs GMRES. Mutually exclusive with sparse/mass (asserted; named follow-on).

**Preconditioner seam = CVODE PrecSetup/PrecSolve.** `OdeKrylovPreconditioner` (`setup(c,t,y)` + `apply(r,z)`)
optionally wired as the FGMRES right-preconditioner M⁻¹ (the v4 flexible-GMRES seam) — setup once per
linearization.

**MOAT.** FGMRES is serial on the calling thread (Arnoldi/Givens/back-solve); the only parallel step is the
operator's jac-vec (bit-identical across workers) ⇒ the Newton-Krylov solve inherits the determinism moat.
Telemetry (`total_gmres_iterations`/`total_matvecs`) lives on the solver for the v9-z work-precision board
(matrix-free work is below the OdeWork seam).

**Gates** (`test_ode_krylov.cpp`, 15 assertions / 4 cases):
- **Exact discrete heat eigenmode**: 1D periodic heat MOL (N=64), the spatial mode is an exact eigenvector of
  the periodic Laplacian ⇒ the matrix-free BDF result matches `exp(λ_h·t)·sin` to **1.2e-9** (594 GMRES iters).
- **Matrix-free == the proven dense BDF trajectory**: maxdiff **2.8e-16** (machine-identical — the seam
  reproduces the v9-d dense path exactly).
- **Preconditioner seam**: a **tridiagonal (Thomas) PrecSolve** cuts GMRES iterations **485 → 206** (same
  answer, 3.3e-16). ⭐ Insight recorded: a Jacobi preconditioner is a NO-OP on the constant-diagonal heat
  operator (M⁻¹ = scalar·I; GMRES is invariant under scalar scaling) — the first draft's Jacobi gate failed
  for that correct reason and was replaced by the tridiagonal solve (honest demonstration of the seam).
- **Determinism**: run-twice bit-identity (memcmp + nfev/nlu/nsol).

**Verified:** win-debug build clean, ode suite **475/56** (+15/+4), no regression. CVODE-SPGMR work-precision
+ {1..16} parallel-jacvec moat at v9-z.

## v9-k — parameter sensitivities (forward + adjoint) ✅ (code + per-slice gates green, incl. ASan)

**Interface** (`sensitivity.hpp`): `ParametricOdeFunction<T>` — `rhs(t,y,p)`, dense `jacobian_y` (∂f/∂y),
`dfdp(t,y,p,j)` (∂f/∂p_j), `dim`, `n_params` (all required — FD over these would defeat the purpose).

**Forward** (`integrate_forward_sensitivities`, CVODES simultaneous corrector): wrap the augmented state
`Y=[y; s_0; …; s_{np-1}]` (size n·(1+np)) as a plain `OdeFunction` and reuse the EXISTING ERK (non-stiff) /
BDF (stiff) drivers. Augmented RHS `[f; J_y·s_j + ∂f/∂p_j]`. ⭐ The stiff path's augmented "Jacobian" is
**block-diagonal, every block = J_y** — the second-derivative coupling ∂(J_y·s_j)/∂y is dropped; the residual
stays exact so the converged solution is exact (the block-diagonal matrix only changes Newton's rate; the
CVODES reading the advisor flagged). No driver change.

**Adjoint** (`integrate_adjoint_sensitivities`, CVODES ASA): forward-solve storing the dense-output
trajectory, then integrate `Λ=[λ; q]` BACKWARD t1→t0 — `λ̇=−J_yᵀλ` (λ(t1)=∂g/∂y(t1)), `q̇_j=−λᵀ∂f/∂p_j`
(q(t1)=0) ⇒ `q_j(t0)=∫λᵀ∂f/∂p_j dt = dg/dp_j` (Lagrangian-derived signs). Dense J_y ⇒ free transpose; full
stored dense output interpolated on the backward pass (real checkpointing = named memory optimization).
y0⊥p assumed (the λ(t0)ᵀ∂y0/∂p term is the named follow-on).

**Gates** (`test_ode_sensitivity.cpp`, 30 assertions / 3 cases): ⭐ **THREE INDEPENDENT DERIVATIVE ORACLES
AGREE** — on the non-stiff exchange problem, forward-sensitivity S(T) == central-difference FD (exact to
print precision), and adjoint `dg/dp_j == Σ_i w_i·S[j,i]` for g=wᵀy(T) (grad[0]=−1.6139 == −1.6139,
grad[1]=2.27709 == 2.27709, agree <1e-6). Stiff **Robertson-with-parameters** (3 states, 3 rate-constant
params) forward-sensitivities vs relative FD through the BDF block-diagonal augmented path (<1e-4). Run-twice
bit-identity (forward + adjoint, memcmp).

**Verified:** win-debug build clean, ode suite **505/59** (+30/+3). ⭐ **ASan checkpoint (advisor-flagged):
the new IMEX + Krylov + sensitivity code — 100 assertions / 11 cases under win-asan, ZERO AddressSanitizer
errors** (the matrix-free `IterMatOp` ConstSpan-into-`m_ylin`, the `m_jv`/`GmresWorkspace` reuse, the
augmented/adjoint scratch — all memory-clean). CVODES work-precision/value comparison at v9-z.

## v9-l — higher-index DAE: structural index analysis + index reduction ✅ (code + gates green, incl. ASan)

**Input-model decision (the advisor-flagged scoping).** Full automatic symbolic reduction of arbitrary
(Modelica-class) DAEs needs an AD-residual layer — and hesap-ode may NOT edge to hesap-opt's `Dual` (the v9
plan forbids ode→opt). So v9-l ships the two pieces that ARE self-contained and serve the named consumer
(multibody/eylem), with the general symbolic auto-reduction as the explicit bridge:

1. **Structural index analysis — Pryce Σ-method** (`dae_structural.hpp`, the Pantelides equivalent — both
   compute the same structural index + differentiation offsets). `StructuralDae` = the signature matrix σ
   (σ_ij = highest derivative order of var j in equation i, or `kSigmaAbsent`). `structural_index` finds the
   Highest-Value Transversal (max-weight assignment, DFS — n is tiny) + the dual offsets by Pryce's
   fixed-point, then ν_S = max_i c_i + (1 if min_j d_j == 0 else 0). GATES: **Cartesian pendulum → index 3,
   c=[0,0,2]** (hand-verified), semi-explicit → 1, the integrator chain → 2, a pure ODE → 0, structurally
   singular → ok=false. The offsets ARE the differentiation plan.

2. **Index reduction for constrained mechanical systems** (`dae.hpp`, the high-value piece). `M·q̈ = f − Gᵀλ`,
   `c(q)=0` (index 3) → differentiate c twice to the acceleration constraint `G·q̈ = γ` ⇒ λ algebraic (index
   1) ⇒ eliminate λ via the KKT/Schur solve `(G·M⁻¹·Gᵀ)λ = G·M⁻¹·f − γ` ⇒ a plain ODE in [q, q̇] (the
   Mattsson-Söderlind dummy-derivative selection specialized to mechanics). `ConstrainedMechanicalSystem<T>`
   interface (mass/force/constraint_jacobian/gamma/constraint); `IndexReducedMechanicalOde<T>` solves the KKT
   per evaluation (hesap-dense LU) and integrates with ANY driver. GATE: the index-3 pendulum, horizontal
   release, RK45 over 3 s — **max|position constraint| = 2.2e-10, max|energy| = 1.7e-9** over 446 nodes (the
   raw index-3 form isn't directly integrable). Run-twice bit-identity. Long-horizon drift-free GGL/projection
   + general AD-symbolic reduction = the named bridges.

**Verified:** win-debug ode suite **519/62** (+14/+3). ASan (all new v9 code: imex+krylov+sensitivity+dae)
**124/16, zero AddressSanitizer errors.**

## v9-z — close ✅ (local parts)

- **CLI** `hesap.ode.solve.f64` (`src/cli_register_ode.cpp` + `cli_anchor.hpp`, the v6/v7 pattern): canned
  problems (decay / Van der Pol / Robertson / harmonic oscillator) × the full 7-method set (RK45 / RK23 /
  DOP853 / BDF / Radau / RODAS4 / TR-BDF2), tolerance + mu selection; out blob [status, nfev, njev, nlu,
  naccept, nreject, y(n)...]. `test_ode_cli.cpp`: registration + e^{-t} across all methods + Robertson-BDF
  mass conservation + oscillator cos/−sin + error paths (58 asserts). Module now links `crd-hesap`.
- **Docs**: `docs/systems/hesap-ode.md` rewritten to the full a→z surface; phase-doc rows v9-i…z marked ✅;
  `context.md` updated (committed-state corrected to `b578f66`; the batch recorded); ADR-0091 stands.
- **Verification — the important configs from EACH compiler, clean on the final post-rename code** (user
  direction: cl + gcc important configs locally, CI owns the full 18). **MSVC**: `win-debug` **577/67** ·
  `win-asan` **577/67, ZERO AddressSanitizer errors** · `win-shipping` (LTCG) **577/67** · `win-tidy`
  clang-tidy clean · guards via ctest all green (`no-std-math`/`no-std-sort`/`no-non-ascii-test-names`/
  `no-untagged-physical-numeric`/`no-malloc-allocator`/`simd-emission-check`). **GCC** (`linux-gcc-release`):
  suite **577/67** + the same guards green + the ODE code builds **-Werror-clean** (the one
  `-Wstringop-overread` is pre-existing `command_registry.cpp` String-SSO, recompiled as a dep — not new code,
  non-fatal). The remaining 13 configs are CI's.
- ⭐ **SANITY catch (advisor-gated, DoD §8 / rule #2)**: the first pass verified via the test BINARY only —
  but the guards live only in ctest, and **three TEST_CASE names carried non-ASCII** (`—`/`Σ`/`→`), which
  `crd-no-non-ascii-test-names` rejects; and win-tidy caught uppercase locals (`S`/`Y`/`L`) violating the
  lower_case convention. Both fixed (ASCII names; `smat`/`yv`/`rod_len`) and re-verified GREEN. "Binary
  passes" was not "slice closed" — the gap was real.

### PENDING USER / CI (the standing pattern, named — not silently deferred)
1. **The v9-i/j-Krylov/k/l/z COMMIT** (agents never commit).
2. **The 18-config CI sweep** (debug/asan/shipping/tidy + guards are green LOCALLY this round; the full
   11-Windows + 7-Linux sweep is CI's). ⚠ **win-shipping's deps are RE-poisoned** (`CMAKE_COMMAND` → the
   VS-bundled CMake fork + English `msvc_deps_prefix` while cl.exe is Turkish ⇒ `#deps 0`; Turkish "Not:
   eklenen dosya" leaked into the build). This round's shipping green is honest (the lib + test TUs
   recompiled fresh — obj mtime > header mtime, verified), but the dir must be **wiped + standalone-
   reconfigured** before its next local incremental use.
3. **All three new work-precision rows are DONE** (`bench_ode_imex_vs_arkode.cpp`, `bench_ode_krylov_vs_cvode_spgmr.cpp`,
   `bench_ode_sens_vs_cvodes.cpp` + `scripts/run_bench_{imex,krylov,sens}.sh`, WSL serial) — see the CRUSH +
   the two HONEST gaps below.

### ⭐⭐ THE IMEX FULL-CRUSH (done this round)

`runtime/examples/bench_ode_imex_vs_arkode.cpp` + `scripts/run_bench_imex.sh` (WSL, g++ -O2, serial). The SAME
1D periodic advection-diffusion MOL (N=128, c=ν=1, explicit advection ⊕ implicit diffusion) solved by Cerid
`integrate_imex(ARK4)` vs **SUNDIALS ARKStep with its OWN ARK4(3)6L[2]SA table** + dense implicit solver +
analytic implicit Jacobian — apples-to-apples (identical method + split). Error = max-norm vs a tight Cerid
DOP853 reference of the SAME discrete system; wall = best of 20.

**Work-precision sweep (achieved error = the honest axis, rtol 1e-3…1e-10):**

| rtol | Cerid (err / ms / fev) | ARKODE (err / ms / fev) |
|---|---|---|
| 1e-3 | 1.06e-3 / 0.568 / 72 | 3.73e-4 / 2.641 / 190 |
| 1e-5 | 1.63e-5 / 0.988 / 123 | 5.81e-6 / 3.403 / 269 |
| 1e-7 | 1.42e-7 / 2.297 / 276 | 6.32e-8 / 5.385 / 501 |
| 1e-9 | 1.31e-9 / 6.918 / 820 | 2.21e-10 / 13.839 / 1498 |
| 1e-10 | 1.29e-10 / 12.093 / 1432 | 6.16e-11 / 19.230 / 2062 |

Per rtol, Cerid delivers err ≈ rtol (well-calibrated — ask 1e-6, get 1.4e-6) while ARKODE over-delivers ~3×
(its built-in conservatism bias ~1.5 + safety). So the decisive, tightness-honest metric is **MATCHED
ACHIEVED ACCURACY** (the work-optimal point reaching each target error):

| target err | Cerid (ms / fev) | ARKODE (ms / fev) | crush |
|---|---|---|---|
| ≤ 1e-5 | 1.432 / 174 | 3.403 / 269 | **2.38× wall · 1.55× fewer evals** |
| ≤ 1e-7 | 4.043 / 480 | 5.385 / 501 | **1.33× wall · 1.04× evals** |
| ≤ 1e-9 | 12.093 / 1432 | 13.839 / 1498 | **1.14× wall · 1.05× evals** |

**Cerid wins at EVERY matched-accuracy level (2.38× → 1.14× wall as accuracy tightens), and reaches the same
tightness (down to 1.3e-10) — it is not less accurate, it is better-calibrated.** The eval advantage narrows
at tight tol (1.55× → 1.05×: both do similar work near machine precision; Cerid's residual edge is per-eval
kernel speed + dense LU). Plus the determinism moat ARKODE lacks. Earlier crush rows stand: BDF 5.9×/RODAS4
8.2× vs CVODE (ROBER), sparse BDF+multifrontal beats CVODE+KLU at n=4096.

### Krylov vs CVODE-SPGMR — CRUSH (the inefficiency was FIXED at the source, no follow-up)

Matrix-free Newton-Krylov, 2D heat MOL N=1024 (32×32), both BDF + matrix-free GMRES + analytic J·v, both
UNPRECONDITIONED, Gaussian IC; reference = CVODE-SPGMR @rtol 1e-13. The first run exposed Cerid doing ~5–6×
more GMRES iterations (88–275 vs 17–43) — root cause: `KrylovOdeLinearSolver` over-solved the inner system to
a **fixed** rel_tol (1e-7). ⭐ **FIX (in-library, this session):** the inner solve now uses the **inexact-Newton
forcing term** — solve only as tightly as the Newton step needs (CVODE's `eplifac` default **0.05**, now the
`KrylovOdeLinearSolver` default; document'd; the dense-equivalence test pins an explicit 1e-7 for bit-for-bit
reproduction). GMRES iters dropped **88–275 → 23–93** (CVODE's ballpark), accuracy unchanged. Matched ACHIEVED
accuracy AFTER the fix:

| target err | Cerid (ms / fev / GMRES-it) | CVODE-SPGMR (ms / fev / it) | crush |
|---|---|---|---|
| ≤ 1e-4 | 0.35 / 21 / 23 | 0.76 / 19 / 17 | **2.14× wall** |
| ≤ 1e-6 | 0.68 / 41 / 42 | 0.94 / 25 / 18 | **1.38× wall** |
| ≤ 1e-8 | 1.40 / 85 / 83 | 2.20 / 56 / 36 | **1.57× wall** |

**Cerid now WINS at every matched-accuracy level (1.38–2.14×).** Determinism moat still ours (FGMRES serial,
parallel jac-vec bit-identical). v9-j krylov gates green (15/4).

### Forward sensitivities vs CVODES — CRUSH (a ~70× swing, FIXED at the source, no follow-up)

Robertson-with-params (n=3, np=3), t=1e4, rtol 1e-8. Cerid analytic FSA vs CVODES DQ FSA. The first run was a
~12× LOSS (20.0 ms vs 1.59 ms) with values agreeing to 3.66e-8. Two root causes, both FIXED in-library this
session: **(1)** the augmented `[y;S]` included the sensitivities in step-error control, throttling the step
(CVODES defaults `sensErrCon=false`) → now `integrate_forward_sensitivities` excludes S from error control via
a per-component atol (state = real, S = huge) so the STATE controls the step and the sensitivities ride along;
**(2)** the augmented iteration matrix is block-diagonal with all blocks = I−c·J_y → new
`BlockDiagonalOdeLinearSolver` factors the n×n block **ONCE** and reuses it for all 1+np blocks
(bit-identical to the full solve; the CVODES shared-factorization economy). ⭐ **RESULT: Cerid 20.0 ms →
0.276 ms — now 5.57× FASTER than CVODES** (a ~70× swing), values still agree to **2.80e-8**. v9-k gates green
(30/3, sensitivities still match FD). (En route, a gate caught a bench bug: CVODES' default mxstep=500 stopped
it before t=1e4 → 50% phantom disagreement; fixed with `CVodeSetMaxNumSteps`.)

**Net v9 scoreboard (honest, no asterisks, NO follow-ups — every inefficiency fixed at the spot):** IMEX
**crushes** ARKODE at matched accuracy (2.38×→1.14×); stiff BDF/RODAS4 **crush** CVODE; sparse **beats**
CVODE-KLU; matrix-free Krylov **crushes** CVODE-SPGMR (1.38–2.14×); forward sensitivities **crush** CVODES
(5.57×). The determinism moat is ours across all of it.

---

## Batch summary

**THE v9 CLUSTER IS COMPLETE (a→z).** This round closed v9-i (IMEX) · v9-j-Krylov (CVODE-SPGMR) · v9-k
(forward+adjoint sensitivities) · v9-l (Pantelides/Pryce structural index + mechanical index reduction —
NOT slipped) · v9-z (CLI + docs). Net: ode suite **405/48 → 577/67** (+172 asserts / +19 cases), every new
slice green on win-debug + win-shipping, all new code ASan-clean. New module surface: `imex.hpp`,
`ode_krylov_solver.hpp`, `sensitivity.hpp`, `dae.hpp`, `dae_structural.hpp`, `cli_register_ode.cpp`,
`detail/ark_tableaus.hpp` (+ `gen_ark_tableaus.py`); `OdeFunction` grew slots 7/8/9, `OdeLinearSolver` grew
`is_matrix_free`/`factor_iteration_matrix_matfree`. Cadence (advisor-adjusted): per-slice correctness/order/
determinism gates ran AS each slice landed (they caught the would-be Jacobi-no-op and kept the order gates
honest); the multi-config sweep + wall-clock scoreboard batch to CI/user per the standing directive.
