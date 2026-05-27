# 2026-05-27 — hesap v4k-c: AMG CLI + F-cycle + complex (and complex dense LU)

Phase 3.1.6 `crd-hesap` v4, slice **v4k-c** — the AMG polish slice: close the CLI-per-op debt,
complete the cycle family, and make SA-AMG complex-capable. Recommended next after v4k-b (K-cycle) to
clear the standing CLI debt before another algorithm slice.

## What shipped

1. **CLI — `hesap.amg.{f32,f64,c32,c64}` (4 commands).** AMG-as-SOLVER via a stationary multigrid-cycle
   iteration `x ← x + M⁻¹(b − A x)`, M⁻¹ = one V/W/F/K cycle. Args: matrix (COO) + b + `cycle`
   (v/w/f/k) + `theta` + `rel_tol` + `max_iter`; output `[iters, resid, converged, x]`. **Self-contained:**
   crd-hesap-amg has no crd-hesap-iterative dependency, so this is the textbook AMG standalone solver
   (no Krylov) — AMG-as-preconditioner is already exercised by the iterative solvers' tests/bench. The
   cycle iteration uses a SERIAL spmv ⇒ the CLI is a deterministic oracle. New
   `cli_register_amg.cpp` + `cli_anchor.hpp` mirror the established anchor-symbol pattern
   ([[feedback_static_lib_anchor_symbol]]).

2. **F-cycle.** `Cycle::{V,W,F,K}` (was V/W/K). F = one F-recursion then one V-recursion per level —
   between V and W in cost/robustness. `vcycle` refactored to take the cycle as a parameter (needed so
   F's two recursions differ); K recurses with K, the coarsest is always the exact dense solve.

3. **Complex AMG.** Made the inverse-diagonal `dinv` carry `T` (complex `1/a_ii`) instead of the real
   type, through `amg.hpp` (`Level::dinv`, `fill_dinv`, `gauss_seidel`, `build_restriction`) and
   `prolongator.hpp` (`estimate_drinv_a_radius` power-iteration norms via a `mag2` squared-modulus
   helper; `smoothed_prolongator` coef `T(-ω)·dinv[i]`). `strength.hpp` already used `mag`. The K-cycle's
   `dotc` is already conjugating.

## The blocker: complex AMG needed complex dense LU (a crd-hesap-dense prerequisite)

The AMG coarsest level is an exact dense LU. `crd-hesap-dense`'s `factor_lu`/`solve_lu` were instantiated
for **f32/f64 only**, and the pivot logic was real-only: `abs_value<T>(x) = x < T{0} ? -x : x` (complex
has no `<`). The slice's first consumer of complex dense LU is exactly this AMG coarse solve — the
`lu.cpp` comment literally said *"Complex variants land in v0e-a2 once a consumer needs them"*
([[feedback_ship_at_consumer_template_from_day_one]]).

Fix (contained, real path bit-identical):
- `abs_value<T>` now returns `RealType<T>`: `if constexpr (is_complex_v<T>)` → `sqrt(re²+im²)`, else the
  real branch unchanged. Pivot vars (`max_abs`, `v`) retyped `RealType<T>`. The rest of LU (elimination,
  forward/back substitution) is complex-generic arithmetic.
- `apply_permutation` was overloaded for `float`/`double` only → templated (body is type-agnostic swaps).
- Added `c32`/`c64` `factor_lu`/`solve_lu` (Span + MatrixView) explicit instantiations.

The real path is guarded by `if constexpr` and `RealType<T>`=`T` for real ⇒ **bit-identical** (v3 eig/SVD
which build on dense LU are unaffected; gcc 38/38 dense+AMG tests confirm).

## Not done (deliberate)

**Per-level smoother choice** (different smoother per level) — gold-plating; the global `Smoother` enum
suffices and no consumer needs per-level. Per [[feedback_crush_mandate_bounded_by_importance]], not a defer
of a needed feature.

## Validation

3 new tests (CLI `hesap.amg.f64` solves a 2D Poisson; complex Hermitian AMG-PCG converges <40 iters; V+K
both converge on conservative cd2d where W diverges) → **9 AMG tests**. MSVC win-debug 9/9 + gcc-release
38/38 (AMG + dense-LU regression) + clang-cl clean + ctest guards (no-std-math/sort, no-non-ascii,
no-untagged-physical).

## v4k-e-1 — αSA adaptive candidate + the β=0.3 wall (measured, not assumed)

User committed to v4k-e bootstrap AMG to chase the β=0.3 iteration crush. Applied measure-first WITHIN
the slice: the cheapest increment is single-candidate **αSA** — relax `A·x=0` from a deterministic seed
(ν=4 weighted-Jacobi sweeps, `relax_candidate`) → seed the tentative T from that candidate
(`tentative_prolongator_adaptive`, per-aggregate normalized) instead of the constant. `Options.adaptive_candidate`
(default off).

**Measured: αSA does NOT move β=0.3** (70→112→115 ≈ SA's 70→108→112). Poisson 7 flat held (no diffusion
regression). **Verified it's not a no-op** (the MC64 lesson — [[feedback_measurement_lever_needs_second_matrix_check]]):
a guarded diagnostic printed the candidate range = **1.8** at the finest level (min 0.27, max 2.09) — the
candidate genuinely varies, αSA is doing its job, it just doesn't help. So the conclusion is solid:
**the coarse space is NOT the β=0.3 lever** ⇒ full multi-candidate bootstrap won't help either (it scales
exactly the thing just proven not to matter). Root cause (theory): conv-diff near-nullspace ≈ constant, so
αSA ≈ SA; the regime needs DIRECTIONAL smoothing, not a better coarse basis.

**β=0.3 anisotropic strong-convection is now a measured WALL for aggregation AMG — 5 levers exhausted:**
κ (worse), droptol (v4j-2b), cycle V/W/K (identical), aggregation smoothed/plain (identical), αSA-candidate
(identical). Honest β=0.3 status: **Cerid AMG WINS total wall-time 1.3–1.9×** (12× cheaper factor, 4× lower
fill) but **LOSES iterations 112 vs 10** — ILUPACK's home turf (multilevel-ILU).

αSA kept as shipped infrastructure (correct, default-off, no diffusion regression) — earns its keep on
problems where the near-nullspace genuinely differs from constant (elasticity), and is the foundation of any
future bootstrap. +1 test (αSA converges on Poisson, no regression). 10 AMG tests; MSVC+gcc+clang-cl+guards.

## The β=0.3 iteration crush: the two remaining paths (v4k-e-2, user-directed)

Both multi-day, both genuinely uncertain vs ILUPACK at this Péclet:
- **(a) downwind / line Gauss-Seidel** — convection-aligned smoothing on strong-coupling chains; the
  theoretically-aligned cheap-ish lever (1–2 days). Honest odds: within ~2× of ILUPACK iters, low odds of
  full crush (β=0.3 also has convection-dominance line-GS can't fully tame).
- **(b) v4j-3 deep multilevel-ILU rework** — beat ILUPACK at its own game; but the κ sweep showed the
  current deferral machinery gets WORSE, so it's a substantial ICE + M-version-Schur rework.

## v4k-e-2 — line-GS attempted (user-directed run (a)) → reverted; β=0.3 = the wall

Took run (a): **algebraic line Gauss-Seidel** (block-GS where blocks are strong-coupling chains solved
tridiagonally via Thomas, off-line couplings lagged to the RHS). Line-detection criterion: a coupling is
strong if `|a_ij| ≥ 0.5·max-offdiag(i)`; build a chain only when the node has **≤2 strong connections**
(an isotropic 5-pt node has 4 strong ⇒ singleton ⇒ point-GS, no Poisson regression; an anisotropic node
has 2 strong ⇒ chain follows that axis = the y-line / downwind line).

**Result: Poisson clean (7 iters = point-GS via singletons), but β=0.1 AND β=0.3 DIVERGE** (residual 1e18,
nan). That's a **definite bug, not weakness**: block-GS with exact line solves is *strictly stronger* than
point-GS, which handles β=0.1 at 9 iters — so a correct line-GS cannot diverge there. Diagnosis: the
algebraic chains aren't grid-aligned enough — snake/boundary lines lose the per-line diagonal dominance the
Thomas solve needs, so the "exact" line solve over-amplifies. A correct line-GS needs grid-line detection
(geometric) or a stricter chain-consistency criterion.

**Reverted** (per advisor): don't ship a diverging smoother (debt even default-off), and it's a low-odds
lever (advisor: "low odds of full crush even if fixed"). The durable artifact is the finding, not the code —
it returns when there's a reason. αSA / F-cycle / K-cycle / MC64 / complex all stay (durable wins). 10 AMG
tests intact; MSVC+gcc+clang-cl green after revert.

**β=0.3 iteration crush is now CLOSED as the documented wall — 6 levers exhausted:** κ, droptol, cycle
(V/W/K), aggregation (smoothed/plain), αSA-candidate, line-GS. Cerid AMG **wins β=0.3 total wall-time
1.3–1.9×** (12× cheaper factor, 4× lower fill) but **loses iterations 112 vs 10** — genuinely ILUPACK's
home turf (multilevel-ILU). The discipline held a third time this week (after MC64 byte-identical-fill and
W-cycle two-matrix divergence): measure, verify, don't ship broken/unverified.

## v4j-2c — complex + adjoint + CLI for `InverseBasedIlu` (the v4j-2 completeness gap, filled)

After the β=0.3 wall was characterized, filled the standing v4j-2 completeness gap (user-directed:
"fill v4j-2c then go v4k-d"). Three pieces:

- **Complex** — `InverseBasedIlu<Complex<f32/f64>>` instantiates + solves a non-Hermitian system. The
  only blocker was the leaf's dense LU, already made complex in v4k-c; the Crout factor/CMSW/Schur are
  complex-generic (`mag` + complex division). +1 test (complex non-Herm BiCGSTAB, true resid <1e-7).
- **Adjoint** (`has_adjoint=true`) — `apply_adjoint` = the conjugate-transpose of the folded eq.(20),
  derived block-wise: `b = U_B⁻ᴴ r_B`; `w_C = S̃⁻ᴴ(r_C − Uꜰᴴ b)`; `w_B = L_B⁻ᴴ(D_B⁻ᴴ b − Lᴱᴴ w_C)`. Needed
  two transpose triangular-solves (`solve_unit_lower_transpose` descending / `solve_unit_upper_transpose`
  ascending-propagate, both conj'd) + a `conj` helper; `DenseLuLeaf` gains `apply_adjoint` via a SECOND LU
  of S̃ᴴ (the leaf is ≤64 ⇒ cheap); the recursive leaf adjoint is polymorphic via `LinearOp::apply_adjoint`.
  MC64 path wraps `D_r·B⁻ᴴ·Pᶜᵀ·D_c` (mirrors the adjoint-tested MultilevelIlu). +1 test: **adjoint identity
  ⟨y, M⁻¹x⟩ = ⟨M⁻ᴴy, x⟩ to round-off**, on a complex matrix WITH deferral so Lᴱ/Uꜰ conj-transposes + the
  leaf adjoint are all exercised (conjugation genuinely tested — imaginary off-diagonals ⇒ Aᴴ≠Aᵀ).
- **CLI** — `hesap.precond.mlilu_ib.{f32,f64,c32,c64}` (4 cmds, κ/droptol) + `mlilu_ib` added to all 5
  nonsym solver selectors (fgmres/bicgstab/gcr/gcrot/idrs); the adjoint makes it QMR-class-usable too.

6 InverseBasedIlu tests; MSVC 6/6 + gcc 6/6 + clang-cl + guards. **v4j is now complete** (1a/1b/2a/2b/2c
shipped; v4j-3 deepening = characterized perf-wall, consumer-pulled only).

## v4k-d — classical Ruge-Stüben AMG (the textbook isotropic path)

Built the full classical RS pipeline incrementally (the C/F splitting is intricate — built+tested in
stages, which caught two real bugs):

- `rs_strength.hpp` — classical directed strength: i strongly depends on j iff −a_ij ≥ θ·max-neg(i),
  θ=0.25 (M-matrix measure; Re-based for complex).
- `cf_splitting.hpp` — RS first-pass coloring with a **bucketed max-λ priority** (λ = in-degree in S;
  pick max-λ → C, its dependents → F, their deps' λ bumped; ascending-index tie-break = deterministic)
  + an **interpolation-correctness promotion pass** (every F with strong deps must have a C dep, else
  promote to C; iterate to fixpoint — a simpler always-correct alternative to the classical second pass).
  **Two bugs caught by the structural test:** (1) the λ-buckets must be sized n+2, not initial-max+2 —
  λ GROWS past its initial max via the +1 bumps; (2) leftover-undecided→F points can lack a C dep — the
  promotion pass fixes it. Validated: 2D Laplacian n=900 → 450C/450F (textbook ~2× coarsening).
- `rs_interpolation.hpp` — classical DIRECT interpolation: C-points inject; F-point i interpolates from
  its strong-C set with sign-split lumping (α for negative couplings, β for positive: all off-diagonal
  mass lumped proportionally onto C, `w_ij = −(α|β)·a_ij/a_ii`). Re-based +/- split for complex.

Wired into the AMG class as **`Options.coarsening = {SmoothedAggregation (default), RugeStuben}`** — one
branch in `build()`, REUSING the entire cycle/smoother/Galerkin/coarse machinery (zero duplication; the
name SaAmg is now historical — it's the general AMG class). CLI `coarsening` arg (sa|rs) on `hesap.amg.*`.

**Result: RS-AMG-PCG is mesh-independent on 2D Poisson — 6 iters at n=2500 AND n=10000** (beats SA's
8–10 on isotropic diffusion, exactly as classical RS should). +2 tests (C/F structural + RS
mesh-independence). 12 AMG tests; MSVC+gcc+clang-cl+guards.

## AMG family status
SA ✅ (v4k-a) · convection probe ✅ (v4k-a-2) · K-cycle ✅ (v4k-b) · CLI+F+complex ✅ (v4k-c) · αSA + β=0.3
wall ✅ (v4k-e-1) · line-GS attempted+reverted (v4k-e-2) · **Ruge-Stüben ✅ (v4k-d)**. Bootstrap (v4k-e
full) = consumer-pulled. v4j complete (incl. v4j-2c).

## v4j-3 reopened — deep multilevel-ILU for the β=0.3 convection crush (CFD is a top-priority consumer)

User's final decision: close the strong-convection iteration-quality gap — CFD/fluids needs it, so it's
consumer-pulled, not speculative. Spent this checkpoint on **diagnosis before code** (the κ-sweep already
burned a one-knob assumption once; the user authorized "take your time").

**Two hypotheses ruled out by measurement (advisor-discriminated):**
- **Per-level MC64 scaling is NOT the lever** — `Mc64Mode::EveryLevel` on cd2d β=0.3 is byte-identical to
  None across the κ sweep (the Schur scaling is near-uniform too). Durable finding.
- **The estimator is NOT the lever** — a guarded per-level + max-ICE print proved: at κ=1.5 the top level
  defers 49% (max_estL=2.16 > κ correctly), and the child Schur (n=1217) is **genuinely well-conditioned**
  (max_estL=1.31, max_estU=1.28, both ≤ κ=1.5) so the estimator *correctly* accepts all → 2 levels. ILUPACK
  uses the same "simple variant" estimator (Saad 2004), so the formula isn't the difference.

**Verified root cause: ACCURACY, not depth or estimator.** The Schur is stable (inverse-norms small, κ
test passes) but our factorization is *inaccurate* — aggressive inverse-based dropping (droptol=1e-2) with
**NO diagonal compensation** loses too much, and there's no depth to recover it → 88–930 iters. ILUPACK's
accuracy comes from gentler + **diagonally-compensated (MILU)** dropping and how dropping interacts with the
Schur (S/T/M version), not from a deeper hierarchy here.

**Corrected rework plan (the right levers):** (1) **MILU diagonal compensation** in `insert_sorted_row`
(add dropped row-sum to the diagonal) — cheap, standard for convection, do first; (2) gentler/smarter
dropping; (3) Schur-version (S/T/M) + Lᴱ/Uꜰ dropping interaction. Each measured vs ILUPACK β=0.3. Guarded
`CRD_MLILU_DEBUG` per-level + max-ICE prints kept as infra. **The advisor's discriminating diagnostic saved
weeks** — I was about to rework the estimator (the wrong lever).

## v4j-3 deeper diagnosis — MILU + fill ruled out; the Schur-version is the airtight lever

Implemented **MILU diagonal compensation** (`m_milu` α∈[0,1], default 0; `insert_sorted_row` returns the
dropped unscaled mass, the pivot is fattened by α·(drop_u+drop_l)). **Measured: ~zero effect** on cd2d β=0.3
(581→581, 90→90 at every κ/droptol). SHIPPED anyway as a correct, default-off, standard option (it's a
legitimate technique that helps other regimes; not broken like line-GS, just not the convection lever).

**Per-block-nnz breakdown (advisor-directed) made it airtight.** At κ=5, droptol 1e-2→1e-4: the extra fill
goes **almost entirely into LB/UB (the accepted-block local solve — already easy, wasted)**; LE/UF/Schur grow
only modestly; **deferred count is κ-fixed at ~254 (droptol-independent)**; iters 88→90 unchanged. And the
254-row child Schur has **maxE=1.27 — genuinely easy** ⇒ child accepts all ⇒ stuck at 2 levels.

**Five levers now ruled out by measurement:** per-level scaling, estimator, MILU, fill/droptol, per-level κ
(the last because maxE 1.27 ≪ any sane κ). **Root cause pinpointed:** the **S-version Schur formula**
(`S̃ = C − Lᴱ Dᴮ Uꜰ`) *removes* the ill-conditioning, so the deferred Schur becomes easy and the hierarchy
can't deepen; ILUPACK keeps the Schur HARD → 3–7 levels → 5–12 iters. **The genuine lever is the
Schur-complement version (T/M-version)** — form the Schur update from UN-dropped accepted↔deferred couplings
so it stays hard. That's a core-factorization data-flow change (store the un-dropped couplings for the Schur
update) = the multi-week structural core, best started with fresh focus (not session-tail; core-algorithm
changes are where fatigue-bugs hide — see line-GS, the λ-bucket overflow).

The research saved real time: it definitively scoped the work and ruled out 5 cheaper levers before any
structural rework. MILU shipped as a bonus correct option. Guarded `CRD_MLILU_DEBUG` per-level + block-nnz
prints kept as infra for the rework.

## v4j-3 BREAKTHROUGH — the lever is REORDERING (confirmed by discriminating experiment)

The T-version was ruled out by my own data (droptol=1e-4: the easy Schur is NOT a dropping artifact, and
S/T-versions converge as drop→0). The advisor agreed and reframed. A readable modern paper (HILUCSI,
ar5iv 1911.10139) pinned the actual ILUPACK mechanism: **REORDER each level (RCM on A+Aᵀ at symmetric top
levels, AMD at coarser) before the Crout factorization** — the deferral itself is one-row-at-a-time *like
ours*, so the reordering is the difference.

**Discriminating experiment (`run_cerid_amd` in the bench): AMD-reorder cd2d at the top level, then
InverseBasedIlu.** Result on β=0.3:
| mesh | natural | AMD-reorder | ILUPACK |
|---|---|---|---|
| 2.5k | 88 it / 2 lvl | **44 it / 2 lvl** | 7 |
| 10k | 138 it / 2 lvl | **59 it / 3 lvl** | 9 |
| 22.5k | 165 it / 2 lvl | **71 it / 3 lvl** | 10 |

Top-level reorder alone **halves iterations AND makes the hierarchy deepen to 3 levels** (natural stays at
2); the Schur went 12× harder (1015→12717 nnz). **Reordering is THE lever — confirmed, measured.** Six
wrong levers were ruled out first (estimator, per-level scaling, MILU, fill/droptol, per-level κ, T-version);
`run_cerid_amd` kept in the bench as the proof; MILU shipped as a correct default-off option.

## Wall-time check (user asked) — and a SECOND finding

Folding the reorder cost honestly into `Cerid-AMD`'s factor time: at β=0.3 it's 2.4× faster than ILUPACK at
n=2.5k, ~even at 10k, and **SLOWER at 22.5k (225 vs 174ms)**. Split-timing the cost pinpointed why:
`amd_order=144ms` vs `PAPᵀ build=1.2ms` at n=22.5k — i.e. **~all the cost is our AMD ordering itself, which
scales super-linearly (O(n^1.7), 5.8→27.9→144ms); the reorder-apply is negligible.** A production AMD does
n=22.5k in single-digit ms. With a fast AMD, Cerid-AMD would be ~78ms vs ILUPACK 174ms = **~2× wall win** at
scale, on top of the iteration win. So the wall-time isn't a reordering problem — it's a slow-AMD problem.

## v4j-3(b) DONE — fast AMD (a core O(n²)→O(n) fix that speeds up ALL AMD consumers)

User: "we need to CRUSH, faster everywhere." The slow AMD was a perf BUG, not a fundamental cost.
**Root cause:** `amd_order` selected the pivot by scanning the *whole* min-degree bucket for the lowest
index (O(bucket)/step → O(n²) on structured grids where many nodes share a degree); `camd_order` (the ND
base case) was worse — it also rescanned `d=0..n` every step. **Fix:** O(1) bucket-HEAD selection in
`amd_order`; first-cur-class-member-in-head-order in `camd_order` — still fully deterministic (serial, fixed
insertion order; the lowest-index tie-break was a determinism choice, not quality, and a different valid
ordering gives an identical solve).

**Results:**
- `amd_order`: **144 → 3.4 ms at n=22500 (42×), now near-linear** (1.3/1.6/3.4ms at 2.5k/10k/22.5k).
- **Fill quality PRESERVED** — SuiteSparse FEM (bcsstk13/24/25): OUR AMD = 0.96–1.04× Eigen-AMD (GATE-OK,
  *beats* Eigen on bcsstk24). The tie-break change did not cost any fill.
- **931 win-debug tests pass** (incl. the `nd_order ≡ amd_order` base case — which CAUGHT that `camd_order`
  needed the same fix); gcc 5/5 + clang-cl clean.
- **Speeds up every AMD consumer** (convection reorder, nested dissection, symbolic factorization, the ILUT
  AMD-reorder adapter).

**Unblocks the wall-time win:** with fast AMD, cd2d β=0.3 `Cerid-AMD` total wall = 5.1 / 29 / 80.8 ms vs
ILUPACK 13.9 / 65.6 / 173 ms = **2.1–2.7× FASTER at every mesh size** (still loses iterations 45–75 vs 7–10
— that's v4j-3(a)). So on its home-turf convection problem, Cerid now beats ILUPACK on total wall-time.

## v4j-3(a) DONE — per-level AMD reorder inside InverseBasedIlu

Productionized the proven lever: a `reorder` ctor option that AMD-reorders `B = P·A·Pᵀ` at construction.
Since the recursion builds a fresh `InverseBasedIlu` per Schur, reorder applies at EVERY level
automatically. `build_reorder_transformed` mirrors the MC64 transform infra; `apply`/`apply_adjoint` unwrap
`A⁻¹ = Pᵀ·B⁻¹·P` (symmetric perm ⇒ the adjoint uses the same gather/scatter with `apply_adjoint_core`).

**Measured (cd2d β=0.3, `Cerid-IB-rord`):** iters HALVED 88/138/165 → **45/64/76** (3 levels vs natural's 2);
total wall-time **5.1 / 31 / 85 ms vs ILUPACK 14 / 67 / 174 ms = 2.0–2.8× FASTER at every mesh size.**
reorder+tight-κ cures the old collapse (κ=1.5 → 6 levels, fill stays ~2.9) but κ=5 is the sweet spot (deeper
≠ better here). +2 tests (reorder solves + deepens + adjoint identity; complex reorder adjoint identity to
round-off) → 8/8 InverseBasedIlu tests, MSVC + gcc + clang-cl.

## v4j-3 net result — CFD home-turf, honest scorecard
On cd2d β=0.3 (strong convection, ILUPACK's specialty): **Cerid now WINS total wall-time 2.0–2.8× at every
mesh size** (the metric that matters for CFD solve cost). It still **loses iteration count** (45–76 vs
7–10 — though halved from natural order). The iteration-count gap is diminishing-returns: we win the wall
metric decisively via fast factor + cheap AMD + low fill. Both levers (fast AMD + per-level reorder) shipped,
correct, tested, cross-config. reorder default-off (a clean option); MILU default-off; run_cerid_amd kept as
the top-level proof.

## Next (resume order for a fresh context)
1. **Measure factor-vs-solve split + reuse break-even** on cd2d β=0.3 — quantifies the convection "crush":
   single-solve we win 2.0–2.8×, but transient CFD reuses the preconditioner across timesteps, so the win
   depends on how many resolves before ILUPACK's fewer iters overtake our cheaper factor. The bench already
   times factor + runs the iters; just report factor_ms vs solve_ms separately and compute break-even.
2. **Decide the `reorder` default** (likely on for convection/general nonsym; ILUPACK always reorders) —
   affects consumers + tests, a deliberate choice.
3. **v4z close** — complex+CLI completeness audit + ADR lock (D(amg)/D(mlilu) incl. the AMD head-selection
   **D(ord)-1** + reorder pins) + vs-reference rollup + 18-config sweep.

## Open cleanup before the v4z commit (kept as proof/infra for now)
- `bench_hesap_mlilu_vs_ilupack.cpp`: `run_cerid_amd` (top-level-AMD experiment, now `[[maybe_unused]]`/
  uncalled) + the `CRD_MLILU_DEBUG`-guarded per-level/ICE/block-nnz prints in `inverse_based_ilu.hpp`
  (off by default). Remove or keep deliberately at v4z — diagnostic scaffolding, not shipped paths.
- Re-pin D(mlilu) determinism docs to include the per-level reorder (deterministic serial AMD + serial apply).
