# 2026-07-01 — v13-j (oscillatory + singular-weight quadrature) + v13-k (multi-D cubature) ⇒ the `crd-hesap-quadrature` module is COMPLETE

> Phase 3.1.6 `crd-hesap`, v13 cluster. This session shipped the final two quadrature rows (v13-j, v13-k),
> completing the `crd-hesap-quadrature` module (g/h/i/j/k). Iteration loop: WSL gcc (linux-gcc-release) with
> scipy 1.17.1 + GSL 2.7.1 + Boost.Math 1.83, `gh` on Windows for the reference sources. Windows 4-config DoD +
> commit remain the user's step (agents never commit).

## What shipped

### v13-j — oscillatory + singular-weight quadrature (`oscillatory.hpp` + `levin.hpp`)

The QUADPACK weighted family + the modern Levin method:

- **QAWO** — ∫_a^b f(x)·{cos,sin}(ωx) dx via the modified Clenshaw-Curtis rule (Chebyshev moments of cos/sin,
  computed once per interval-length level via a tridiagonal BVP/forward recurrence; `dqc25f` + `dqawoe`).
- **QAWF** — Fourier integral ∫_a^∞ over half-period cycles + Wynn-ε acceleration (`dqawfe`).
- **QAWS** — algebraico-logarithmic endpoint weights (x−a)^α(b−x)^β·[log…] (`dqc25s` + `dqmomo` + `dqawse`).
- **QAWC** — Cauchy principal value PV ∫ f/(x−c) (`dqc25c` + `dqawce`).
- **★Levin collocation** — general nonlinear phase g(x) where Filon moments don't exist: solve p′+iωg′p=f by
  Chebyshev collocation (small deterministic complex Gaussian-elimination solve), ∫ = [p e^{iωg}]_a^b.

All four QUADPACK routines are faithful goto-free transliterations of the jacobwilliams `quadpack_generic.F90`
reference, **reconstructed-and-verified bit-exact in python vs `scipy.integrate.quad(weight=…)` BEFORE the C++
port** (the v13 mandate). The reconstruct-first discipline caught **three real bugs pre-port**:

1. the long-standing QUADPACK `dqc25s` `res24 = res12 + …` transcription typo (corrected to `res24 += …`);
2. the `dqawoe` `done`→global-sum fall-through (a converged result must be Σ rlist, not the stale first-panel
   estimate) — this manifested in the C++ port as a 5e-7 error with `abserr = OFLOW`;
3. the `dqawoe` jupbnd "next interval" search must cycle the **main** loop on width>small (the `dqagse`/scipy
   semantics), not the inner loop (the jacobwilliams unlabeled `cycle`).

And in the C++ port, the **workspace-reuse leak** (boundary-adversary, SANITY #3/#4): `dqawoe` omitted the
Fortran `Nnlog(1)=0` init, so a *reused* OscWorkspace leaked the prior call's subdivision level → wrong Chebyshev
moments. Masked entirely by the per-call-fresh-allocation convenience overload; exposed only by the ws-reuse
determinism test.

### v13-k — multi-D cubature (`cubature.hpp` + `lebedev.hpp` + `simplex.hpp` + `smolyak.hpp`)

- **tensor-product Gauss** — dense d≲4 baseline (reuses `gauss_legendre`).
- **★Genz-Malik** — degree-7 fully-symmetric rule + embedded degree-5 error, globally-adaptive box subdivision
  (split the worst box along its largest 4th-difference axis) over a bounded region work-stack (NOT recursion).
  Reconstructed bit-exact vs scipy's `GenzMalikCubature` (rule) and `scipy.integrate.cubature` (adaptive).
- **★Lebedev** — unit-sphere cubature, octahedral-symmetry point sets (degrees 5/7/11/17, generated from
  `scipy.integrate.lebedev_rule`), spherical-harmonic exactness.
- **Dunavant** — symmetric triangle rules (degrees 1–6), FEM/CFD element integration, polynomial-exactness gated.
- **★Smolyak** — sparse grid via the nested-Clenshaw-Curtis combination technique + a canonical integer-key
  dedup (the nested structure gives each node a unique key), breaks the curse of dimensionality for smooth f.

## The crush board (honest, all peers)

v13-j (per-call, preallocated workspace vs GSL's preallocated tables — apples-to-apples):

| Method | Cerid | GSL 2.7.1 | scipy | verdict |
|---|---|---|---|---|
| QAWO | ~123 ns | ~123 ns | 2677 ns | parity GSL, **22× scipy** |
| QAWS | ~755 ns | ~700 ns | 4105 ns | 0.95× GSL (floor), **5.4× scipy** |
| QAWC | ~136 ns | ~145 ns | 2391 ns | **win GSL**, **17× scipy** |
| QAWF | ~1035 ns | ~1169 ns | 12347 ns | **1.13× win GSL**, **12× scipy** |
| Levin | — | N/A | N/A | analytic Fresnel + accuracy-grows-with-ω; reduces to QAWO at g=x |

All four QUADPACK methods **bit-match scipy to ~1e-16**. Boost has only `ooura_fourier_cos` (a double-exponential
Fourier method, a different class — faster on the smooth-infinite case but not the QAWF algorithm); MATLAB has no
weighted-QUADPACK API (stated). Plus the determinism + WCET + error-tier moat GSL/scipy/Boost structurally lack.

v13-k: **Genz-Malik CRUSHES scipy.integrate.cubature 37.5× (3D, value bit-identical) / 279× (5D)**. No
GSL/Boost/MATLAB multi-D-adaptive peer; Tasmanian (Smolyak) not installed (gate = analytic exactness + tensor
cross-check, stated).

### The crush levers (all found by MEASURING, SANITY #5)

1. **QAWS** double-double `crd::math::pow` (20 ns) → `wpow = exp(y·log x)` (8.4 ns, deterministic, exp/log both
   beat libm) — flipped QAWS 0.54×→0.95× GSL. Same lever in `dqmomo`'s base-2 pows.
2. **the moment-recompute scar, again** — GSL caches the integrand-independent Chebyshev moments in its
   `qawo_table` at alloc; Cerid reset `momcom=0` every call → added a (|ω|, b−a)-keyed cache (QUADPACK `icall>1`
   reuse) → QAWO 0.61×→parity.
3. **QAWF** allocation-free workspace overload (stack scratch, capped `limlst`) + cross-call moment cache →
   0.55×→1.13× win.

The honest characterization: QAWO/QAWC/QAWF match-or-beat GSL after every Cerid-specific per-op cost was removed;
QAWS sits at the algorithmic floor (0.95×, both dominated by the same pow + integrand evals); all four crush scipy
5–22×; v13-k crushes scipy 37–279×. The moat is the differentiator where raw speed hits the floor.

## Tests + guards

- Full `crd-hesap-quadrature` suite: **534 assertions / 33 cases** green on linux-gcc-release (was 465 at session
  start; +40 v13-j, +29 v13-k).
- `crd-no-non-ascii-test-names` guard: was **RED** (17 violations across the uncommitted v12-n / v13-b/d/g/h/i
  tests — prior sessions only ran linux-gcc, never the Windows-DoD guard); ASCII-ized all 17 (`build/fix_test_names2.py`)
  → now **PASS**. The v13-j/k tests are ASCII from the start.
- Header is std-math-clean (pillar 1): all transcendentals via `crd::math`.

## Decisions / notes

- The `x·√x` lever (which flipped QAGS/QAGI vs GSL) was tried in `dqk15w` but **reverted to `crd::math::pow(·,1.5)`**:
  this rule's abserr drives the adaptive subdivision, and a ~1-ulp change cascades to a ~1e-7 divergence vs scipy on
  sensitive oscillatory integrands. The pow is on the rare GK15-fallback panel (the CC path dominates) so the lever
  bought nothing here.
- Levin's only genuine limitation is documented honestly: g′(x) ≠ 0 on [a,b] (a stationary point makes the
  collocation operator singular). The "ω=1000 blowup" in the first python check was **mpmath.quad failing on the
  oscillatory reference**, not Levin — the reconstruct-verify method caught a bad reference (confirmed vs analytic
  Fresnel: accuracy grows from 2.8e-7 at ω=10 to 1.9e-14 at ω=10⁵).

### v13-l/m — the NEW `crd-hesap-diff` module (also shipped this session)

Numerical differentiation, 5 headers, 42 assertions green:

- `finite_difference.hpp` — Fornberg arbitrary-stencil FD weights (bit-exact vs the analytic stencils) · central /
  forward derivatives with the per-magnitude optimal step · **Richardson-Ridders** (derivative + error estimate) ·
  FD gradient / Jacobian / Hessian-vector.
- `complex_step.hpp` — **★★complex-step** `Im[f(x+ih)]/h`, MACHINE-EXACT (no subtractive cancellation) · gradient ·
  Jacobian. Verified vs JAX autodiff (float64) to 0 / 1e-16 — as accurate as algorithmic differentiation. (The
  reconstruct-verify caught JAX's float32 default first: complex-step float64 was *more* accurate than the "ref".)
- `savitzky_golay.hpp` — noise-robust polynomial-fit differentiation; coefficients bit-match
  `scipy.signal.savgol_coeffs` (via the SPD Gram + Cholesky min-norm solve).
- `spectral.hpp` — Chebyshev + Fourier differentiation matrices (spectral / exponential accuracy).

**Crush:** complex-step gradient (6D) **48.5 ns = 57× JAX-jit / 835× numpy central-FD** — *and* machine-exact
(matches JAX exactly; numpy-FD loses digits). `savgol_coeffs(25,4,2)` **142 ns = 204× scipy**.

### v13-n/o/p/q — the NEW `crd-hesap-motion` module (started here, COMPLETED in a sibling session)

> ⚠ **This section captured only the module's first-pass CORE.** It was then completed in full — including the pieces
> the "core" framing had quietly deferred (quaternion B-spline, the multi-segment min-snap QP, multi-DoF sync) **and**
> the entire arbitrary-state Ruckig-class OTG, which **crushes Ruckig's own C++** (single-DoF 1.94×, multi-DoF sync
> 1.26×, both bit-exact). The full, correct story lives in **`docs/sessions/2026-07-01-v13-motion-ruckig-otg.md`** —
> read that, not the "follow-ons" note below, which is superseded.

Trajectory generation, 6 headers, 77 assertions green (each reconstructed-and-verified in python first):

- `squad.hpp` — **★SQUAD** C² attitude interpolation on the unit-quaternion manifold (Shoemake). Added `quat_log` /
  `quat_exp` to **crd-math** (SANITY-8: the quaternion capability's home; crd-math quat tests still 64-green).
  Unit-norm preserved to 1e-16, endpoints exact.
- `clothoid.hpp` — Euler spiral, curvature *linear in arc length* (bounded lateral jerk for steering), closed form
  via `crd-hesap-special` Fresnel S/C; graceful circular-arc / straight-line limits.
- `nurbs.hpp` — rational B-splines (Cox-de Boor A2.1/A2.2); **exact unit circle** (points on x²+y²=1 to 1e-12).
- `poly_traj.hpp` — minimum-jerk quintic + minimum-snap septic boundary-value trajectories (BCs exact).
- `profile.hpp` — **★jerk-limited S-curve** + trapezoidal time-optimal profiles (the Ruckig single-DoF core): the 7
  phase durations in closed form, state reconstructed with a bounded-WCET ≤7-phase walk. Verified target-reached and
  jerk/acc/vel-limited across the cruise / triangular / long-distance regimes.
- `tcb.hpp` — Kochanek-Bartels (Tension/Continuity/Bias) keyframe spline; Catmull-Rom special case.

~~Honest scope: multi-DoF Ruckig time-synchronization and a quaternion-B-spline distinct from SQUAD are the deepest
follow-ons.~~ **SUPERSEDED — all of these were built + verified + crushed. See the sibling session doc.**

## Win-tidy naming pass owed (whole uncommitted v13 tree — a close-globally DoD step)

The project `.clang-tidy` enforces `readability-identifier-naming` (`StaticConstexprVariableCase: CamelCase`) and
`readability-isolate-declaration` as **errors** (`WarningsAsErrors: '*'`). Prior v13-g/h/i sessions and this one only
ran linux-gcc; win-tidy is a Windows preset. This session **auto-fixed the 45 isolate-declaration** violations
(`clang-tidy --fix`, safe) and **manually lowercased the local-uppercase vars** in the new diff/motion/levin files
(the corruption-prone direction). **~49 static-constexpr-array renames remain** (`xgk`/`wgk`/`wg`/`x1`/`w10`/`data`/
`npoints` → CamelCase across gauss_kronrod/qags/qng/oscillatory/lebedev/simplex) — substring-hazardous by literal
replace, so do them AST-aware via `clang-tidy` (Windows LLVM-20 at `C:\LLVM-20.1.8`) with a git safety net on the
win-tidy preset. (`clang-tidy --fix` on naming corrupts the *lowercase* direction per memory; the uppercase
static-constexpr direction is safe.) Everything is linux-gcc-green + crushing today.

## Remaining v13 scope

**All four modules are now shipped** — `crd-hesap-interp` (a-f) · `crd-hesap-quadrature` (g-k) · `crd-hesap-diff`
(l-m) · `crd-hesap-motion` (n-q, incl. the Ruckig OTG crush). **Only v13-z close remains:**

- **v13-z close**: CLI `hesap.{interp,quad,diff,motion}.*` · 4 system docs · ADR-0095 · the all-peers scoreboard ·
  the safety-critical conformance audit (`-fno-exceptions` build, adaptive-uses-workspace-not-recursion guard,
  no-heap-in-hot-path, status-not-exception) · **the win-tidy naming pass** (now also the new `otg.hpp`/`otg_sync.hpp`).

## Pending (user's step)

Commit the v12 + v13 working tree (still uncommitted) and run the Windows 4-config DoD + the 18-config CI.
