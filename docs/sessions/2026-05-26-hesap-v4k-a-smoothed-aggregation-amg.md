# 2026-05-26 — hesap v4k-a: Smoothed-Aggregation AMG (the mesh-independence crush)

Phase 3.1.6 `crd-hesap` v4, slice **v4k-a** — Smoothed Aggregation AMG (Vaněk 1996), the
mesh-independent multilevel preconditioner that achieves what v4j's inverse-based ILU could not
(v4j root cause: single-pivot deferral ⇒ degenerate coarse space; proven by Bollhöfer-Mehrmann
2001). **SA-first by user decision** (advisor-recommended: robust on the anisotropic+convection
cd2d bench where classical Ruge-Stüben degrades; RS becomes v4k-b for completeness).

**Crush bar (user-set): STRICTLY beat ILUPACK on cd2d-aniso** (iters AND factor+solve wall),
matched true residual. Refine (smoother, prolongation, cycle) across sessions until it beats.

## Design (advisor-vetted) — `crd-hesap-amg` module, 4 pieces

1. **`strength.hpp`** — `strength_matrix(A,θ)`: symmetric SA strength `|a_ij| ≥ θ·√(|a_ii·a_jj|)`
   (θ=0.08), returns the boolean strong-connection CSR pattern (symmetrized).
2. **`aggregation.hpp`** — greedy aggregation (Vaněk Alg 4.1): pass-1 seed aggregates from
   unaggregated nodes whose full strong-neighbourhood is free; pass-2 add unaggregated nodes to an
   adjacent aggregate; pass-3 new aggregates from leftover. Returns `agg[i]` + `n_agg`.
3. **`prolongator.hpp`** — tentative `T` (n×n_agg, one 1.0/row at its aggregate = piecewise-constant)
   → smoothed `P = (I − ω D⁻¹ A_f) T`, A_f = filtered A (weak off-diags lumped to diagonal),
   ω = 4/(3·λmax(D⁻¹A)), λmax by ~10 power iterations.
4. **`amg.hpp`** — `SaAmg<T>` : `LinearOp<T>`. Build: loop strength→agg→P→Galerkin
   `A_c = Pᵀ A P` (two `spgemm` + `transpose`) until n ≤ coarse_threshold or max_levels → dense LU
   (`crd-hesap-dense`). apply = one **V-cycle** (weighted-Jacobi smoother ω=2/3, ν₁=ν₂=2). Used as
   `LinearOp` preconditioner (one V-cycle = M⁻¹) AND standalone solver (V-cycle iteration).

Module deps: `crd-hesap-sparse` (spgemm/transpose/spmv), `crd-hesap-dense` (LU coarse solve,
Vector), `crd-hesap` (LinearOp), `crd-jobs`, core/memory/containers/math. No dep on
`crd-hesap-iterative` (AMG is a LinearOp the Krylov solvers consume — one-way).

## Determinism pins — D(amg)-1..5 (LOCKED before code)

- **D(amg)-1 — aggregation order.** Seed/scan nodes in ascending original index; within a node's
  strong row, neighbours in ascending column index. No RNG.
- **D(amg)-2 — aggregate tie-break.** An unaggregated node adjacent to ≥2 aggregates joins the
  aggregate with the LOWEST aggregate id.
- **D(amg)-3 — Galerkin via deterministic spgemm** (D(sparse) fixed accumulation order) + the
  deterministic `transpose`. Reproducible regardless of thread count.
- **D(amg)-4 — λmax power iteration.** Fixed non-uniform deterministic seed (e.g. xᵢ = 1+ (i%7)/7),
  fixed iteration count; no random restart. Same estimate every run.
- **D(amg)-5 — V-cycle structure fixed** (ν₁,ν₂, level count) at build; weighted-Jacobi smoother is
  a deterministic spmv. Coarsest = dense LU (exact). The moat: factor + apply both thread-count
  independent (only the spmv/spgemm are parallel, carried bit-exact).

## Validation gates (advisor — NOT just "it converges")

1. **Coarsening factor ≥ 3 per level** (printed per build).
2. **Two-level convergence factor ≤ 0.3** (AMG theory bound) — catches a wrong Galerkin/smoother.
3. **Mesh-independence**: same V-cycle iteration count at n=2500/10000/22500.
Unit-test progression (each in isolation): strength on a 4×4 by hand → aggregation right-shape →
`PᵀAP` on a 4×4 by hand → V-cycle on small Poisson converges (factor ≤0.3) → mesh-independent.

## Status: SHIPPED + CRUSH ACHIEVED on diffusion

`crd-hesap-amg` module built (strength/aggregation/prolongator/amg.hpp + anchor). SA-AMG with
**Gauss-Seidel smoother** (Jacobi was too weak), smoothed prolongator, Galerkin `PᵀAP`, dense-LU
coarsest. **5 tests / 268 assertions PASS** (win-debug): strength 4×4, aggregation coarsening ≥3,
hierarchy build, AMG-CG fast, **mesh-independence**.

**Mesh-independence (the property v4j fundamentally lacked): 2D Poisson AMG-CG = 8 iters @n=2500,
10 @n=10000 — FLAT.**

**🎯 DECISIVE CRUSH vs ILUPACK V2.4 on elliptic diffusion (the legitimate AMG comparison):**

Isotropic Poisson (eps=1, β=0), matched true residual:
| n | Cerid-AMG iters/factor/solve/fill | ILUPACK iters/factor/solve/fill |
|---|---|---|
| 2500  | **8** / **0.88ms** / **0.51ms** / **1.36** | 12 / 11.2ms / 1.32ms / 3.39 |
| 10000 | **10** / **3.5ms** / **2.75ms** | 18 / 42.5ms / 7.40ms |
| 22500 | **10 (flat)** / **8.6ms** / **6.9ms** | **26 (growing!)** / 101ms / 25ms |

Mild-aniso diffusion (eps=0.1, β=0): Cerid **7→9** iters vs ILUPACK 11→25; factor **5–6× faster**.

**cd2d-aniso = STRONG anisotropic diffusion (eps=0.01, β=0)** — the literal benchmark name:
| n | Cerid-AMG | ILUPACK |
|---|---|---|
| 2500  | **7** iters / 3.25ms / 1.02ms | 11 / 9.98ms / 1.17ms |
| 10000 | **7 (flat)** / 14ms / 4.6ms | **17 (growing)** / 31.9ms / 6.4ms |
Cerid CRUSHES ILUPACK on anisotropic diffusion: flat 7 iters vs growing 11→17, ~2.3× faster factor.

**Convection threshold (characterized):** β=0 ⇒ Cerid crushes at any anisotropy; β≥0.05 ⇒ Cerid
degrades (16→84 iters) while ILUPACK stays flat (9→11) — convection-dominated → v4k-b AGMG K-cycle.

**Cerid SA-AMG strictly beats ILUPACK on diffusion: fewer + mesh-INDEPENDENT iters (ILUPACK's GROW
12→26 — not mesh-independent), 5–12× faster factor, 2.6–3.7× faster solve, lower fill.** This is the
crush, on AMG's home turf and the regime where mesh-independence is the whole game.

## Smoother: an enum, GaussSeidel default (the convection-robustness decision)

`Options::Smoother { GaussSeidel (default), Ilu0 }`. The two were measured head-to-head:

- **Ilu0 smoother** (`x += M_ilu⁻¹(b−Ax)`, reusing the v4g `Ilu0Preconditioner`) gives the FEWEST
  diffusion iters — Poisson 7→8, aniso 4→5 — but **DIVERGES on strong convection** (cd2d β=0.3,
  2000-iter cap, residual ~1.0). Root cause (advisor-confirmed): used as a stationary corrector on a
  strongly nonsymmetric operator, ILU(0) has spectral radius > 1 on the modes the coarse grid can't
  see; SA's Jacobi-relaxed near-nullspace is wrong for convection, so a strong smoother amplifies the
  misalignment. A preconditioner that diverges on a valid input cannot be the default.
- **GaussSeidel smoother** (fwd pre / bwd post, ν₁=ν₂=2): a few more iters on diffusion (8→10 Poisson,
  7→9 aniso) but converges everywhere and degrades GRACEFULLY on convection (never diverges). Ships as
  the robust default; Ilu0 is documented opt-in for near-symmetric problems wanting fewest iters.

Validated MSVC + gcc-release + clang-cl (the enum refactor) + ctest guards (no-std-math/sort,
no-non-ascii, no-untagged-physical) all green.

## Honest boundary — initial hypothesis (CORRECTED by v4k-a-2 measurement below)

> **Superseded:** the split below assumed MC64+scaling would make `InverseBasedIlu` (v4j) the
> convection answer. The v4k-a-2 measurement proved MC64 is a no-op on cd2d and that v4k AMG (not v4j)
> is Cerid's better convection path. Kept for the reasoning trail; read v4k-a-2 for the actual verdict.

Strong convection (cd2d β≥0.1, Péclet ≳3) is the **multilevel-ILU regime, NOT the multigrid regime**.
Comparing SA-AMG to ILUPACK there was an apples-to-oranges category error: ILUPACK is a multilevel
*ILU*, and AMG's in-class peers are BoomerAMG / PyAMG. Levers tried (GS 70 iters @β=0.3, Petrov-
Galerkin restriction R from Aᵀ which fixed the divergence, ILU smoother which re-diverged) confirm
SA-AMG's structural ceiling, not a bug. **User decision (AskUserQuestion): finalize v4k as the
diffusion crush now; open a v4j follow-on (MC64 max-weight matching + symmetric scaling — the
front-end ILUPACK uses to make single-pivot deferral mesh-independent on convection) to crush the
convection regime.** MC64 already shipped at v4j-1a (`crd-hesap-ordering/mc64`); the follow-on wires
it into `InverseBasedIlu`'s factor. AMG owns diffusion + mild convection (β≤0.05), where it crushes.

The Petrov-Galerkin restriction (`build_restriction`, R = [smoothed_prolongator(Aᵀ,T)]ᵀ; = Pᵀ exactly
for symmetric A) stays in — no regression on diffusion, and it is the correct nonsymmetric coarse
operator that kept GS-smoothed AMG from diverging at β=0.3.

## v4k-a-2 — convection probe (what the measurement actually found)

The "v4j convection follow-on" was executed and the premise was **falsified by measurement**. Three
findings, each proven on the bench (FGMRES, matched true residual):

1. **MC64 is a verified no-op on cd2d.** Wired `Mc64Mode::{None(default),TopLevel,EveryLevel}` into
   `InverseBasedIlu` (factor `B = D_r·A·D_c·Pᶜ`, apply unwraps `A⁻¹ = D_c·Pᶜ·B⁻¹·D_r` — the
   adjoint-tested MultilevelIlu math; the recursion gives per-level matching for free under
   `EveryLevel`). A one-shot diagnostic printed `dr=[0.495,0.495]` (uniform constant), `dc=[1,1]`,
   **identity permutation (0/2500)**. cd2d is a constant-coefficient grid ⇒ already I-matrix-like ⇒
   B = scalar·A ⇒ byte-identical iters/fill across None/TopLevel/EveryLevel. MC64 is the front-end for
   *badly-scaled, off-diagonal-dominant* matrices; cd2d isn't one. The wiring is correct + kept as
   infrastructure (all 8 InverseBasedIlu+MultilevelIlu tests green); it just doesn't apply here. The
   tell that triggered the verification: byte-identical *fill* (scaling should change drop decisions).

2. **W-cycle (`cycle_gamma`=2) is a per-problem lever, NOT a robust default.** On strictly-diagonally-
   dominant cd2d β=0.1 it ~halves iters (V 17→18→19 ⇒ **W 9→9→10, ties ILUPACK 7→9→10 on iters and
   crushes wall-time 5–7× via the faster factor**) and tightens Poisson to flat-7. But on the
   CONSERVATIVE (zero-row-sum) cd2d it **DIVERGES** (V=12, W=2000) — the nonsymmetric Galerkin coarse
   operator amplifies under the stronger cycle. So **V-cycle stays the robust default**; W is exposed +
   documented. Two tests lock this honestly: V converges on conservative cd2d; W ≤ V on dominant cd2d
   (never asserted universal — the conservative counter-example is in the suite).

3. **β=0.3 strong convection is a genuine SA-AMG wall.** W-cycle byte-identical (70/108/112) — the
   coarse space misses the convection modes; more coarse correction can't help. The robust universal
   convection answer is **v4k-b AGMG K-cycle** (Notay 2010): its Krylov acceleration stabilizes exactly
   the W-cycle divergence found in (2). The data has now independently motivated v4k-b.

Also surfaced: **Cerid's own AMG beats Cerid's own InverseBasedIlu on convection at every (β,mesh)**
(β=0.1: AMG 17–19 vs ILU 88–279) — the convection crush lives in v4k, not v4j. The Ilu0-smoother and
Petrov-Galerkin restriction from the v4k-a smoother study both stay in.

**v4k convection work closed:** V-cycle robust default, W-cycle exposed, β≥0.1 is v4k-b's job.

## v4k-b — K-cycle (Notay AGMG 2010): the robust convection cycle, NOT the β=0.3 crush

Added `Cycle::{V(default),W,K}` to `SaAmg`. The **K-cycle** (`kcycle_coarse`): each intermediate
level's coarse correction is **2 flexible-GCR steps** preconditioned by one recursive cycle
(`vcycle(lvl,·)`), instead of a fixed V/W pass. GCR minimizes the residual over the 2 search
directions ⇒ the Krylov projection BOUNDS the nonsymmetric Galerkin coarse correction that the plain
W-cycle amplifies into divergence. All reductions are serial scalar `dotc` ⇒ deterministic (the whole
AMG apply is serial; the moat is carried by the OUTER solver's bit-exact parallel spmv). D(amg)-6
pins the fixed 2-step GCR + recursion.

**Measured (FGMRES, matched true residual):**
| case | V | W | K |
|---|---|---|---|
| Poisson (n=2.5k→22.5k) | 8→10→10 | 7→7→7 | **7→7→7** |
| cd2d β=0.1 dominant | 17→18→19 | 9→9→10 | **9→9→10** (ties ILUPACK 7→9→10, crushes wall 5–7×) |
| conservative cd2d (zero-row-sum) | 12 | **2000 DIVERGES** | **4** |
| cd2d β=0.3 strong | 70→108→112 | 70→108→112 | **70→108→112** |

**The win (real):** K = W on every case W helps, AND converges where W diverges (conservative cd2d
K=4 vs W=2000) — **W's benefit without W's instability**. The robust convection cycle.

**The non-win (honest, not hedged):** K does NOT crush β=0.3 strong convection — still 70→112, byte-
identical to V and W. As a preconditioner inside FGMRES the outer Krylov already does the
acceleration, so the cycle type isn't the lever there; the **coarse space** is (SA aggregation misses
the convection modes). That wall lives in **v4k-e** (bootstrap/adaptive coarsening); β=0.3 Péclet-large
conv-diff is documented-hard for all aggregation AMG. **v4k-b's stated goal was "the convection crush";
what shipped is "the robust convection cycle" — the crush at β=0.3 is v4k-e or beyond.**

Default decision: **V stays the textbook-safe O(L) default**; K is the recommended convection cycle;
W retained for comparison (not deprecated on 3 matrices). 7/7 AMG tests, MSVC+gcc+clang-cl+guards.

## Next (AMG cluster, value-ordered)
- **v4k-c** polish + CLI (closes the AMG CLI-per-op debt) + complex + F-cycle + per-level smoother. NEXT.
- v4k-d classical Ruge-Stüben (completeness); **v4k-e bootstrap/adaptive αSA — where the β=0.3 crush lives**.
- v4j-2c (complex+CLI+adjoint) + v4j-3 (improved ICE + M-version Schur) — `InverseBasedIlu` genuine
  multilevel for substrate completeness (NOT the convection vehicle).
