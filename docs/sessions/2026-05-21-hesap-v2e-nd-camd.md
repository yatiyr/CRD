# 2026-05-21 — Phase 3.1.6 `crd-hesap` v2e: nested-dissection ordering + CAMD (v2 CLOSE)

> Module: `crd-hesap-ordering`. The fill-reducing payoff of the v2 reordering
> cluster: a recursive nested-dissection ordering that **beats Eigen-AMD fill on
> 2/3 FEM matrices** via constrained AMD. Closes Phase 3.1.6 v2.

## What shipped

- **`fm_refine`** — Fiduccia-Mattheyses bipartition refinement: gain buckets,
  best-prefix rollback (≤ `kFmPasses`=4), equal-gain ties → lowest index, balance
  `kBalanceTol`=1.03 with both-sides-non-empty. Reaches optimal grid cuts (40×40 →
  20; recovers a 90-edge stripe → 10); 51–65% cut reduction on bcsstk*.
- **`vertex_separator`** — König minimum-vertex-cover of the cut (bipartite
  max-matching, Kuhn). **`node_fm_refine`** — node-separator FM (uphill +
  rollback) to shrink |S|.
- **`nd_order`** — recursive bisection assigns a separator-tree **postorder
  `cmember`**, then **`camd_order`** (one constrained-AMD pass on the full graph).
  AMD-hybrid leaf threshold (`kAmdThreshold`=100).
- **`camd_order`** — constraint-aware copy of the `cs_amd` port: per-class
  min-degree on the FULL graph (interface-aware), dense-off, supervar + mass-elim
  gated by `cmember`. **CAMD-uniform == AMD exactly** (port validated).
- **+5 CLI → 16 ordering commands.** 36 cases / 7275 assertions; **4-config DoD PASS.**

## Result — the fill gate (vs Eigen-AMD, SuiteSparse SPD)

| matrix | n | ND nnz(L) | vs our AMD | vs Eigen-AMD |
|---|---|---|---|---|
| bcsstk13 | 2003 | 253,698 | 0.994× | **0.983× WIN** |
| bcsstk24 | 3562 | 285,450 | 0.997× | **0.999× WIN** |
| bcsstk25 | 15439 | 1,671,434 | 1.109× | 1.158× (follow-on) |

**ND + CAMD beats Eigen-AMD on 2/3 FEM matrices.** bcsstk25 (large 3D multi-DOF)
loses → tracked follow-on `v2e-weighted-compression`.

## The journey (all measured, the interface-fill saga)

The naive recursive ND (AMD on separator-stripped induced subgraphs) **lost** to
AMD on all three (1.05–1.19×) and *worse at scale* — the signature of a real
defect, not a quality gap. Diagnosis ladder:

1. **node-FM** (greedy, then full uphill-rollback) barely moved it (1.192→1.162×) —
   separators were already near-minimal; |S| was not the lever.
2. **Path probe** (the decisive diagnostic): ND on a path gave 1430 vs AMD 999,
   with a *perfect* top-level cut/separator → the excess fill was created inside
   the recursion. Root cause: **subdomain interface vertices (adjacent to the
   separator) were eliminated too early**, dumping fill into the live separator.
3. **proxy-at-leaf** deferral — insufficient (a leaf can be early in the global
   postorder; leaf-local deferral ≠ global).
4. **CAMD** (the fix) — constrained min-degree on the FULL graph: separator-
   adjacent vertices carry their separator edges, so within-class min-degree
   eliminates them last. Flipped bcsstk13/24 to beating Eigen-AMD.
5. **graph compression** (for bcsstk25's multi-DOF) — unweighted version
   *regressed* all three (imbalanced bisection); reverted, filed for weighted retry.

## Key lessons / decisions

- **ND fill is regime-dependent, and is a downstream-perf knob — NEVER correctness.**
  Any valid permutation yields the identical solve; fill only affects the future
  v5 factor's memory/flops, and v5 picks the better of AMD/ND per matrix. On a 1D
  path minimum degree is provably optimal, so ND is *legitimately* worse there —
  the "path bug" was a wrong test expectation, not a defect. The ND win is on 2D/3D
  FEM. (Memory: [[feedback_nd_fill_regime_dependent_not_correctness]].)
- **CAMD is the interface fix** — per-subdomain AMD on stripped induced subgraphs
  leaks fill into separators; constrained min-degree on the full graph fixes it.
- **No new D(ord) pin** in v2e (FM/König/CAMD tie-breaks all reduce to D(ord)-1/-7).
  ADR-0065 §16 locks D(ord)-1..7 at this v2-close.

## Tests reconciled to reality

Two aspirational tests encoded the wrong "ND universally beats AMD": the path test
now validates **CAMD-uniform == AMD** + a loose 1D bound; the 40×40-grid test now
asserts **ND ≪ natural** + **competitive with AMD (≤ +33%)** (it's the AMD/ND
crossover regime; the real ND win is the FEM bench). One genuine robustness bug
fixed: FM could collapse a tiny bipartition onto one side — added the both-sides-
non-empty feasibility guard.

## Verification

- `crd-hesap-ordering-tests`: 36 cases / 7275 assertions PASS.
- 4-config per-slice DoD (debug + asan + shipping + tidy): **PASS.**
- `bench_hesap_ordering_vs_reference` (flag ON, win-release): fill table above;
  v2c symbolic L-pattern still MATCH; v2b AMD still GATE-OK. Flag reset OFF.
- 18-config sweep: CI on push.

## Next

**v3** — SVD (Golub-Reinsch + randomized Halko) + dense eigenvalue (MRRR +
QR-double-shift) + least squares (LS/NNLS/TLS) + complex + CLI. Plus the
`v2e-weighted-compression` follow-on whenever a v5 multi-DOF workload makes
bcsstk25-class fill the bottleneck.
