# 2026-05-26 — hesap v4j-2b: multilevel recursion (recursive Schur + dense base)

Phase 3.1.6 `crd-hesap` v4, slice **v4j-2b** — replace the v4j-2a single ILUT leaf with a
RECURSIVE `InverseBasedIlu` on the Schur complement, closed by an exact dense LU base case.
The machinery that turns the inverse-based-pivoting core into a true multilevel method.

## What shipped (recursion machinery — correct, three-compiler green)

- `InverseBasedIlu<T>` leaf is now `std::unique_ptr<LinearOp<T>>`: at each level the Schur
  `S̃ = C − Lᴱ Dᴮ Uꜰ` is factored RECURSIVELY by another `InverseBasedIlu` until it is small
  (`n ≤ kDenseThreshold=64`) or the depth cap (`max_levels=50`) is hit, when a new
  `DenseLuLeaf<T>` (partial-pivot dense LU, `factor_lu`/`solve_lu`) closes it EXACTLY
  (D(mlilu)-6: dense base, droptol-independent terminal). The folded eq.(20) apply is
  unchanged — its `S̃⁻¹ t` step recurses through the leaf polymorphically (the advisor's
  "one-line swap"). With κ ≥ 1 the first pivot of every level always accepts ⇒ the Schur
  strictly shrinks ⇒ termination guaranteed.
- **4 tests / 514 assertions PASS** (MSVC win-debug; gcc -Werror + clang-cl below): accept-all
  (1 level); defer fires (≥2 levels); **genuine multilevel >2 levels on an anisotropic
  convection-diffusion stencil** (semi-coarsening, n=2500) + solves; serial-vs-parallel spmv
  bit-exact. `num_levels()`/`factor_nnz()` recurse through the hierarchy.

## Bench (cd2d-anisotropic, vs ILUPACK V2.4, default condest=5)

Cerid stays at **2 levels**, iters 118/256/387 (n=2500/10000/22500), **mesh-DEPENDENT**;
ILUPACK 3–5 levels, iters 7/9/10, **mesh-INDEPENDENT**. The recursion *engages and solves
correctly* but does not yet reproduce ILUPACK's flat iteration count.

## The crush diagnosis (κ × droptol sweep — measured, not guessed) → v4j-3

It is **not a one-knob fix**, proven by sweeping:
- **fill is not the lever**: droptol 1e-2→1e-4 at κ=5 raises Cerid fill 2.67→5.55 but iters
  barely move (118→121). At κ=1.1 droptol 1e-3 vs 1e-4 give *identical* fill (0.73) — the
  drop saturates (the tight-κ accepted block is the most diagonally-dominant rows, whose
  factor mass is already small — structural, not a dropping bug).
- **depth alone is not the lever**: at 3 levels (κ=1.2) Cerid still needs ~488 iters.
- Cerid sits ~100× off ILUPACK and mesh-dependent at *every* (κ, droptol) ⇒ the gap is the
  **per-level approximation quality + recursion depth at usable κ**, together.

The crush is the **v4j-3 quality cluster**, levers ORDERED (advisor):
1. **ICE histogram** at level 0 (one diagnostic) — confirm the simple CMSW estimator
   under-defers the moderate-difficulty pivots ILUPACK defers, BEFORE writing code.
2. **Improved ICE** (Li-Saad-Chow eq.2, `|ξ_k| + ‖p_k‖₁`) — lifts moderate estimates ⇒ defer
   more at loose κ ⇒ recursion deepens with fill intact. Re-measure.
3. **M-version Schur** (Bollhöfer-Saad §2.2; ILUPACK's default — the S-version is "much more
   sensitive") — per-level approximation quality. Re-measure.
4. droptol/κ̃ calibration to hold ILUPACK-class fill.

Do them one at a time, measuring each on the bench. This is exactly the v4j-3 "quality tuning
+ head-to-head" scope; v4j-2b's deliverable was the recursion machinery, which is complete.

## v4j-3 lever investigation (started — measured, three levers RULED OUT)

Pursued the crush methodically; each hypothesis tested on the ILUPACK bench before coding:

1. **ICE histogram** (level 0, κ=5, n=2500): estimates spread cleanly `<1.05:134 · [1.05,1.5):71 ·
   [1.5,2):344 · [2,5):1697 · [5,10):255(deferred) · >10:0`. Healthy spread, max ~10 — NOT a
   collapsed/bimodal estimator.
2. **Ordering — RULED OUT.** ILUPACK with `ordering="none"` (natural order, same as Cerid) STILL
   gets 3–5 levels / flat 7–10 iters / fill ~5. So ILUPACK's depth is achieved INSIDE the
   factorization, not via nested dissection. (Decisive single-bench test.)
3. **Improved ICE (Li-Saad-Chow eq.2 `|ξ|+‖p+ξ·col‖₁`) — RULED OUT.** Implemented + measured:
   deferral 255 vs 254, iters 114 vs 118 — within noise. The weighted sign only flips in
   near-ties (rare here). Reverted (kept the clean Algorithm-3.1 estimator).
4. **fill / droptol — RULED OUT as the iter lever** (from the 2b sweep): fill 2.67→5.55 leaves
   iters ~118→121, still 2 levels.

**Remaining lever = the M-version (T-version) Schur** (Bollhöfer-Saad §2.2, eq.5/14; ILUPACK's
DEFAULT — the S-version is "much more sensitive"). Evidence: Cerid defers ~10% at level 0 (like
ILUPACK), but Cerid's **level-1 Schur accepts ALL** (→ 2 levels) while ILUPACK's level-1 keeps
deferring (→ 3–5). The difference is the COARSE-OPERATOR quality: my S-version `S̃ = C − Lᴱ Dᴮ Uꜰ`
(= C − E B̃⁻¹ F with the dropped factors) inherits inverse error in both factors and is "too easy"
at level 1; the T-version computes `T̃` by applying the inverse factors to A (no inverse error in
the (2,2) block) → retains the coarse-grid difficulty → defers → deepens to mesh-independence.

**→ next step (below): instrumented the Schur — the crux is now precisely characterized.**

## v4j-3 deep investigation — ~18 measured experiments, crux isolated (crush NOT yet achieved)

Pursued exhaustively per "try everything / deep research." Levers tested + RULED OUT, each on
the ILUPACK bench:
- **Ordering** — ILUPACK `ordering="none"` (natural, like Cerid) STILL 3–5 levels / flat 7–10.
- **Improved ICE** (Li-Saad-Chow eq.2 `|ξ|+‖p+ξ·col‖₁`) — deferral 255 vs 254 (noise); reverted.
- **Fill / droptol** — at κ=5, fill 2.67→5.55 leaves iters 118→121, 2 levels.
- **FGMRES restart** — 30→300 drops 118→88 (n=2500) but still mesh-dependent (88→154) and 17–22×
  off ILUPACK; restart is a minor factor, not the crux.
- **Product condest** (`estL·estU ≤ κ`, condition-number reading) — defers more but WORSE (240→643,
  fill 1.44, still 2 levels); reverted to the faithful each-factor test (encyclopedia figure).

**Schur instrumentation (the key finding).** Printed per-level Schur density:
- Level-0 Schur is a real coarse operator (254×254 @ ~4 nnz/row at κ=5; 1250×1250 @ 2.9/row at κ=1.1).
- **Level-1 Schur collapses to EXACTLY DIAGONAL (avg/row 1.0)** — and this persists at **droptol=1e-14
  (NO dropping)**. So the deep-Schur collapse is STRUCTURAL, not a tunable. A correct coarse operator
  should densify with depth; this one degenerates → trivially accepts all → recursion dies at 2–3
  levels → never mesh-independent (iters grow with n at every κ).

**The crux, precisely:** at κ=5 Cerid's fill (5.55) ALREADY matches ILUPACK's (5.13–9.16), yet Cerid
gets 2 levels / 121 iters vs ILUPACK 3 levels / 4–7 at the SAME fill. The gap is the **coarse-operator
(Schur) quality**, not fill/ordering/estimator/restart. The S-version Schur `S̃ = C − Lᴱ Dᴮ Uꜰ` (even
exact at no-drop) degenerates at depth on this deferred-set structure; ILUPACK's default **M/T-version
Schur** (exact E/F, no inverse error in the (2,2) block; the paper calls the S-version "much more
sensitive") is the documented difference.

**Reframed crux (important):** at ZERO drop, the S-version Schur `C − Lᴱ Dᴮ Uꜰ` provably EQUALS the
M-version = the exact Schur `C − E B⁻¹ F` (since zero-drop ⇒ Lᴱ = E U_B⁻¹ D_B⁻¹ exactly). The
zero-drop Schur STILL degenerated to diagonal at level 1 ⇒ **the M-version Schur will NOT help** —
the exact Schur correction is failing to densify at depth. Two possibilities, and the next test
discriminates:
- (a) the deep deferred-set's true Schur is genuinely near-diagonal (a property), OR
- (b) **the bi-index Crout UNDER-FILLS the coupling blocks Lᴱ/Uꜰ** — the "zero-drop" factor is not
  actually a complete factorization, so the Schur correction is missing fill. (Note: the unit tests
  verify the preconditioner SOLVES, not that `L̃D̃Ũ = A` at zero-drop — an approximate ILU solves fine.)

**Recon-completeness test — DECISIVE (ran it):** at κ=∞ (accept-all) + droptol=1e-16 Cerid converges
in **1 iteration**, fill **19.9/39.4/59.0**, residual 3e-14. ⇒ **the bi-index Crout is COMPLETE and
CORRECT** (full exact LU, massive fill-in). NO fill bug. So branch (a): the exact deep Schur is
genuinely near-diagonal because the inverse-based pivoting selects a deferred set whose TRUE Schur
complement is near-uncoupled on this problem.

## Definitive conclusion (deep research, ~21 experiments)

- v4j-2b recursion machinery: **correct + shipped** (complete-LU verified, solves, deterministic,
  3 compilers, 514 asserts). NOT a bug.
- The crush — **mesh-independent iterations matching ILUPACK** — is NOT achievable via the faithful
  Bollhöfer-Saad S/M-version templates alone: the inverse-based deferred set's exact Schur degenerates
  (near-diagonal) at depth, so the recursion can't build a coupled coarse space. ILUPACK's mesh-
  independence is its "AMG-like" secret sauce (a coarsening that keeps the coarse operator COUPLED).
- **That coupled coarse operator IS the Galerkin `PᵀAP` of v4k (classical Ruge-Stüben AMG)** — the
  next planned cluster. ILUPACK deliberately blurs ILU↔AMG; true mesh-independence lives on the AMG
  side. So the crush on this metric is more naturally **v4k territory** than v4j.
- Remaining v4j options (lower priority, characterized — no debt): reverse-engineer ILUPACK's exact
  deferral/aggregation via the published SISC 2006 convergence data (mat0/e40r*/sherman*/raefsky*);
  or a coupled-coarse-space deferral heuristic. But the high-leverage path to mesh-independence is
  v4k AMG.

**Honest status: v4j-2b machinery COMPLETE; the iteration crush vs ILUPACK is a v4k-AMG-class problem,
now precisely characterized — not a tuning miss.** Ship v4j-2a/2b; pursue mesh-independence in v4k.

## DEFINITIVE root cause (deepest research — 4 primary papers, 21 experiments)

Read Bollhöfer-Mehrmann 2001 ("Some Convergence Estimates For Algebraic Multilevel Preconditioners",
`external/ilupack/papers/preprint-721-2001.pdf`). **Example 4–6 is exactly our problem, with the
proof:**
- "*simply to locally optimize the bounds is not enough... This problem even occurs for the exact QR
  factorization and represents a fundamental point for the pivoting process.*" (§4, Example 4)
- Single-coarse-node-at-a-time selection ⇒ coarsening STOPS early + bad preconditioner (Table 4:
  `129,128,stopped`; Table 5: 1062 CG iters). Selecting a **proper coarse set — a maximal independent
  set in the distance-3 graph ("multiple columns")** ⇒ perfect geometric-multigrid hierarchy
  (512→256→128→…) + **mesh-independent** convergence (Table 5: 22 iters, flops-optimal).

**Our `InverseBasedIlu` defers pivots ONE AT A TIME in natural order — exactly the "single-column"
strategy the paper proves fails.** Confirmed empirically: at tight κ the accepted (fine) block is a
maximal independent set ⇒ **`L_B nnz = 0` (diagonal B, zero smoothing)**; the deep Schur is exactly
diagonal ⇒ recursion dies ⇒ no mesh-independence. The Crout/Schur are CORRECT (1-iter accept-all
complete-LU verified); the defect is the **coarse-space construction**.

**Conclusion: the crush requires a proper algebraic coarse-space (C/F coarsening with strength-of-
connection + a well-coupled Galerkin coarse operator `PᵀAP`) — i.e. the v4k AMG machinery.** ILUPACK
wins because its inverse-based pivoting (the exact Bollhöfer 2004 variant + aggregation) yields a good
coarse space; the faithful Bollhöfer-Saad 2006 ILU *templates* (v4j) alone do not, on hard PDEs. The
v4j-2a/2b recursion + dense-base + `LinearOp` leaf + Krylov infra are exactly what v4k reuses.

**Path to the crush (recommended): build v4k classical Ruge-Stüben AMG** (strength matrix → C/F split
→ interpolation → Galerkin `PᵀAP` via spgemm → V/W-cycle, smoother = Jacobi/Chebyshev/ILU from v4a/i),
as solver AND `LinearOp` preconditioner; head-to-head vs ILUPACK/AMGCL — *that* is where Cerid matches/
beats ILUPACK on mesh-independence. (Optional v4j refinement later: the exact Bollhöfer-2004 estimator +
multiple-column/aggregation deferral — `cs-2004-006.pdf` downloaded, unread.)

The argv knobs on `bench_hesap_mlilu_vs_ilupack` (κ, droptol, ILUPACK ordering, FGMRES restart) are
the kept v4j-3 tuning surface. The kernel is reverted to the clean faithful 2b state (514 asserts,
3 compilers).

## Determinism / scope

Factor serial ⇒ thread-count-independent (moat). Dense base + recursion are deterministic
(D(mlilu)-6). Real only; complex + CLI + adjoint + a cross-compiler bit-exact determinism
test → **v4j-2c**. MC64-wire of `MultilevelIlu` → 2c (with adjoint).
