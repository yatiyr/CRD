# crd-hesap-ordering

Fill-reducing reorderings + symbolic factorisation — the bridge from v1 sparse
storage (`crd-hesap-sparse`) to v5 sparse direct solvers. A good permutation `P`
(factor `PAPᵀ`) cuts Cholesky/LU fill-in by orders of magnitude; this module
computes those permutations and the symbolic structure of the factor.

Pure **integer / graph + structure** work on `SparsePattern` (no SIMD floats).
Integer-deterministic and bit-reproducible across platforms.

## Status (Phase 3.1.6 v2)

| Sub-slice | Ships |
|---|---|
| **v2a** ✅ | `AdjacencyGraph` + `Permutation`/`apply_symmetric` + **RCM** + minimal `nnz(L)` fill metric (etree + column counts) + bandwidth/profile + 7 CLI commands. |
| **v2b** ✅ | **AMD** (approximate minimum degree) — the fill gate. Faithful `cs_amd` port; beats/ties Eigen-AMD fill. |
| **v2c** ✅ | **Full symbolic factorisation** — `postorder` + `SymbolicFactor` (etree + colcounts + full L pattern CSC + fundamental supernodes) + `symbolic_factorize`. Pattern bit-exact vs Eigen; symbolic analysis beats Eigen at scale. |
| **v2d** ✅ | **Multilevel-ND scaffold** — `WeightedGraph` + heavy-edge-matching coarsening + re-seeding BFS bisection + uncoarsen-project + `nd_bipartition`. Valid, near-perfectly-balanced 2-way partitions; no refinement yet. |
| **v2e** ✅ | **Multilevel-ND ordering + CAMD** — FM refinement + König separator + node-FM + recursive `nd_order` (separator-tree cmember → constrained AMD). **Beats Eigen-AMD fill on 2/3 FEM matrices.** |

## v2a surface

- **`AdjacencyGraph build_adjacency(pattern, alloc)`** — symmetrised (`A ∪ Aᵀ`),
  diagonal-free, ascending-sorted CSR adjacency (METIS-style `xadj`/`adjncy`).
- **`Permutation`** (`perm[new]=old`, `inv_perm[old]=new`) + **`apply_symmetric`**
  (`PAPᵀ` on a pattern's structure, canonical output).
- **`rcm_order`** — Reverse Cuthill-McKee: George-Liu pseudo-peripheral start
  (capped at 5 iterations), BFS with neighbours enqueued by ascending
  `(degree, index)`, reversed. Bandwidth/profile reducer.
- **Symbolic fill metrics** (`elimination_tree`, `column_counts`, `nnz_l`) —
  ported line-for-line from Davis's CSparse (`cs_etree`/`cs_post`/`cs_counts`),
  kept in signed `i32` with `-1` sentinels (the `cs_counts` `first[j] <=
  maxfirst[i]` test requires `-1` to be the minimum — see D(ord)-below). `nnz_l`
  is the fill metric the AMD gate (v2b) and ND (v2e) are judged by.
- **CLI** (type-agnostic — orderings read structure only): `bandwidth`,
  `profile`, `nnz_l`, `rcm`, `rcm_bandwidth`, `rcm_nnz_l`, `etree`.

## v2b surface — AMD

- **`Permutation amd_order(pattern | graph, alloc)`** — Approximate Minimum Degree
  (Amestoy/Davis/Duff 1996). Quotient-graph elimination on the v2b-1 machinery:
  doubly-linked degree buckets, Amestoy approximate external degree, supervariable
  merge (indistinguishable-node detection via sum-mod-n hash + structural compare),
  **mass elimination** (a node whose external degree is 0 folds into the pivot —
  the gate-closer), **aggressive absorption** (`w[e]==0 ⟺ Le ⊆ Lme`), and
  **dense-node-last** (`deg > min(n-2, max(16, 10√n))` removed from the active
  graph, ordered last). A faithful port of CSparse `cs_amd`; the approximate-degree
  formula is algebraically identical (`min(prev, d_ext) + |Lme| - nv`, clamped by
  `n - nel - nv`).
- **CLI**: `amd` (permutation BinaryBlob), `amd_nnz_l` (fill scalar) — 9 ordering
  commands total.

### Result (vs Eigen-AMD on SuiteSparse SPD)

| matrix | our nnz(L) | Eigen-AMD | ratio | ordering |
|---|---|---|---|---|
| bcsstk13 | 255,305 | 258,179 | **0.989×** (beats) | 3.4 ms |
| bcsstk24 | 286,337 | 285,671 | **1.002×** (ties) | 3.1 ms |
| bcsstk25 | 1,507,688 | 1,443,995 | **1.044×** (within gate) | 14.3 ms |

Gate (`nnz(L) ≤ 1.05×` SuiteSparse/cs_amd + faster ordering) met on all three.
Mass elimination was decisive (took bcsstk13 1.083× → 0.989×).

**Why a step-by-step elimination-order diff vs Eigen is invalid:** `cs_amd`
**postorders** its assembly (elimination) tree, so its returned permutation's
first node is a postorder leaf, not the first-eliminated. Postordering does not
change `nnz(L)`, so the fill gap was real elimination-quality (closed by mass
elimination), not a postorder artifact. The bcsstk25 1.044× residual is an
un-isolated tie-break / iteration-order divergence from cs_amd's incidental
internal order — not a missing algorithm step, and fill is a downstream-perf knob
(never correctness), so it is a tracked follow-on, not a defer.

## v2c surface — full symbolic factorisation

The v5 sparse-direct hand-off. Built on the v2a CSparse port (`cs_etree`/`cs_post`/
`cs_counts`), adding the full L row pattern and the supernode partition.

- **`Array<u32> postorder(etree, alloc)`** — postorder of the elimination forest
  (CSparse `cs_post`, now public): children precede parents, sibling subtrees
  emitted in ascending root index (deterministic).
- **`SymbolicFactor`** — the result struct. `parent` (etree, `kNoParent` roots) +
  `post` (postorder) + `colcount` (nnz per column of L) + **`lp`/`li`** (the full
  L pattern, **compressed CSC lower-triangular incl. diagonal**, row indices
  ascending + diagonal-first per column) + **`super`/`nsuper`** (the fundamental
  supernode partition: supernode `s` owns columns `[super[s], super[s+1])`,
  forming an etree path with a shared row pattern). `nnz()` == `lp[n]` == the
  `nnz_l` metric.
- **`SymbolicFactor symbolic_factorize(pattern, alloc)`** — the driver. One
  `build_adjacency`, shared scratch: etree → postorder → column counts (cs_counts,
  cheap O(nnz(A)) — used to size `lp`) → full L pattern via a faithful **`cs_ereach`
  row-subtree** port (one pass, cursor-fill into CSC, asserts each column lands
  exactly on `lp[j+1]`) → fundamental supernodes (Liu-Ng-Peyton; CHOLMOD
  `super_symbolic` test: column `j` starts a new supernode iff `parent[j-1] != j`
  **or** `colcount[j-1] != colcount[j]+1` **or** `nchild[j] > 1`). Pure integer,
  no tie-breaks → no new D(ord) pin needed.
- **CLI** (+4 → 13 total): `postorder`, `symbolic_nnz_l` (nnz via the full path),
  `supernode_count`, `supernodes` (column boundaries).

### Result (vs Eigen on SuiteSparse SPD — bench `bench_hesap_ordering_vs_reference`)

**Pattern gate (the contract): bit-exact MATCH.** Column-by-column L row-index diff
vs `Eigen::SimplicialLLT<Lower, NaturalOrdering>` factor on bcsstk13/24/25 — every
column's row pattern identical. (Stronger than the v2a `nnz` check: the actual
structure, not just the count.)

| matrix | n | L-pattern | nsuper (cols/snode) | analyze ours vs Eigen | full symbolic (+Li+snode) |
|---|---|---|---|---|---|
| bcsstk13 | 2003 | MATCH | 501 (4.0) | 1.65 ms vs 1.32 ms (**0.80×**) | 3.51 ms |
| bcsstk24 | 3562 | MATCH | 445 (8.0) | 2.51 ms vs 4.44 ms (**1.77×**) | 11.97 ms |
| bcsstk25 | 15439 | MATCH | 6139 (2.5) | 3.90 ms vs 5.82 ms (**1.49×**) | 14.51 ms |

"analyze" compares our `nnz_l` (etree+post+counts) to Eigen `analyzePattern`
(etree+counts+Lp) — the only apples-to-apples symbolic comparator, because **Eigen
defers the L row pattern (`Li`) to `factorize`** (`SimplicialCholesky_impl.h:143`),
so the full `symbolic_factorize` (Li + supernodes) has no Eigen twin. Our symbolic
**scales better** (2.4× over the 7.7× n-range vs Eigen's 4.4×) → wins at every n ≥
3562. The bcsstk13 0.80× is a `build_adjacency` alloc/sort constant-factor at small
n, tracked as `v2c-small-n-analyze-constant-factor` (not algorithmic).

## v2d surface — multilevel nested-dissection scaffold

The METIS multilevel paradigm (pure-C++ port, not a wrap of METIS the library):
**coarsen → bipartition the coarsest graph → uncoarsen-project.** v2d ships the
chassis producing a *valid, un-refined* 2-way partition; v2e bolts on
Fiduccia-Mattheyses refinement + separator extraction + the recursive `nd_order`.

- **`struct WeightedGraph`** — one coarsening level: CSR `xadj`/`adjncy` (symmetric,
  ascending, diagonal-free) + `adjwgt` (edge weights ‖ adjncy) + `vwgt` (vertex
  weights). Base level all-unit; coarse levels accumulate, so Σ`vwgt` is conserved
  (== base n) at every level.
- **`detail::coarsen_match`** — heavy-edge matching: ascending visit, match each
  unmatched vertex to its heaviest-edge unmatched neighbour (ties → lowest index).
- **`detail::contract`** — build the coarser graph: sum parallel-edge `adjwgt`, sum
  `vwgt`, drop self-loops, sort each row ascending.
- **`detail::coarsen`** — level stack; stop at `n ≤ 100` OR matching stall
  (`n_coarse ≥ 0.9·n`) OR `30` levels (hard cap vs pathological non-coarsening).
- **`detail::bisect_coarsest`** — **re-seeding BFS region-grow**: grow part 0 from
  the lowest-index seed to half the total vertex weight; when a component is
  exhausted before reaching half, re-seed from the lowest-index unassigned vertex
  (the disconnected-graph contract, D(ord)-7). Remainder → part 1.
- **`detail::project_down`** — lift a coarse partition to the finer level.
- **`detail::edge_cut`** — weighted cut (each undirected edge once); the metric FM
  minimises in v2e.
- **Public `Array<u8> nd_bipartition(pattern, alloc)`** — full pipeline → `part[v]
  ∈ {0,1}`. n=0 → empty; n=1 → {0}.
- **CLI**: `nd_bipartition` (part vector BinaryBlob) — 14 ordering commands total.

### Scaffold quality (no FM refinement yet)

Region-grow to half-weight gives essentially **perfect balance** out of the box:
50/50 (n=50), 72/72 (n=144), 112/113 (n=225, coarsened to 57 over 3 levels), 90/90
(n=180). A **disconnected** 54-vertex two-block graph splits **27/27** — the
re-seeding contract colours every vertex and balances across components rather than
dumping one block into a single part. A tridiagonal bisects with edge-cut **1**
(optimal). Edge-cut on 2-D grids (longer than optimal for an un-refined region
boundary) is the baseline v2e's Fiduccia-Mattheyses will straighten.

## v2e surface — nested-dissection ordering + constrained AMD

The fill-reducing payoff: a recursive-bisection nested-dissection ordering whose
quality match needs **constrained AMD** to handle the subdomain↔separator interface.

- **`fm_refine(g, part)`** — Fiduccia-Mattheyses bipartition refinement: gain
  buckets (O(1) max extraction), best-prefix rollback over up to `kFmPasses`
  passes, equal-gain ties → lowest index (D(ord)-1), balance kept within
  `kBalanceTol`=1.03 of half *and* both sides non-empty. Reaches the **optimal**
  cut on grids (40×40 → 20; recovers a 90-edge stripe to 10) and cuts the FEM
  matrices' bisection 51–65%.
- **`vertex_separator(g, part)`** — minimum vertex cover of the cut edges via
  König's theorem (bipartite max-matching), i.e. the minimum separator for the
  cut. **`node_fm_refine`** then shrinks it (uphill moves + best-prefix rollback).
- **`nd_order(pattern|graph)`** — recursive bisection assigns each vertex a
  **separator-tree postorder class** (`cmember`: a separator is always a higher
  class than the interior it borders), then one **`camd_order`** pass orders the
  whole graph constrained to that class order.
- **`camd_order(g, cmember)`** — constraint-aware copy of the `cs_amd` port: pivot
  = lowest-degree principal of the lowest non-empty class; min-degree runs on the
  FULL graph (so it is **interface-aware** — separator-adjacent vertices carry
  their separator edges and are not eliminated early into a live separator);
  dense-node handling off; supervariable merge + mass elimination gated by
  `cmember` equality. Validated: **CAMD with one class == AMD exactly**.
- **CLI** (+5 → 16 ordering commands): `nd_bipartition`, `nd_order`, `nd_nnz_l`
  (+ the v2c `postorder`/`supernode*`).

### Result (vs Eigen-AMD on SuiteSparse SPD — the fill gate)

| matrix | n | ND nnz(L) | vs our AMD | **vs Eigen-AMD** |
|---|---|---|---|---|
| bcsstk13 | 2003 | 253,698 | 0.994× | **0.983× WIN** |
| bcsstk24 | 3562 | 285,450 | 0.997× | **0.999× WIN** |
| bcsstk25 | 15439 | 1,671,434 | 1.109× | 1.158× (follow-on) |

**ND fill is regime-dependent** — not a universal win. On a **1D path** minimum
degree is provably optimal, so AMD wins and ND is legitimately worse (the path was
the diagnostic that proved CAMD correct, not a bug). The ND advantage shows on
2D/3D FEM, where it **beats Eigen-AMD on bcsstk13 + bcsstk24**. bcsstk25 (large 3D
multi-DOF) loses pending vertex-weighted graph compression (`v2e-weighted-
compression` in `docs/debt.md`). Critically, **fill is a downstream-perf knob, never
correctness**: any valid permutation yields the identical solve, and the v5
consumer picks the better of AMD/ND per matrix.

## Validation (v2a, vs Eigen on SuiteSparse SPD)

`bench_hesap_ordering_vs_reference` (gated `CRD_BUILD_HESAP_VS_REFERENCE`):

- **Symbolic-Cholesky port is bit-exact**: our `nnz_l` (NaturalOrdering) **==**
  `Eigen::SimplicialLLT<Lower,NaturalOrdering>` factor `nnz(L)` on bcsstk13/24/25
  (434214 / 2031722 / 2940220 — exact match). This certifies the CSparse port.
- **RCM bandwidth reduction**: bcsstk13 1250→422, bcsstk24 3333→251. (RCM targets
  bandwidth, not fill — fill moved 0.24×–1.08×; fill-minimisation is AMD/ND's
  job in v2b/v2e, not RCM's.)
- **v2b AMD target recorded**: Eigen-AMD `nnz(L)` = 258179 / 285671 / 1443995 —
  the numbers v2b's AMD must reach (≤ 1.05×) and beat on ordering time.

## Determinism pins (D(ord))

Graph algorithms compound tie-points across hundreds of operations; one fuzzy
rule pollutes the whole pipeline, so these are pinned before any code:

- **D(ord)-1** — all tie-breaks resolve by **ascending original-graph vertex
  index** (RCM equal-level sort, AMD min-degree, FM equal-gain move, heavy-edge
  matching ties, separator-vertex selection).
- **D(ord)-2** — iterate hash-like structures (quotient-graph element lists,
  supervariable members, FM gain buckets) **by sorted key, never slot/insertion
  order**.
- **D(ord)-3** — pseudo-peripheral / seed selection is **structure-derived**
  (lowest-index unvisited vertex), never RNG state.
- **D(ord)-4** — re-sort each vertex's adjacency ascending before use →
  coarsening / traversal is input-adjacency-order-independent.
- **D(ord)-5** (AMD) — a supervariable's **principal is the lowest-index member**
  of its merge group; the merged supervariable inherits that lowest-index
  identity (so indistinguishable-variable merges are run-independent).
- **D(ord)-6** (AMD) — aggressive element absorption iterates prior elements in
  **ascending element-id order** (the absorbed set is order-invariant, but the
  workspace layout — and downstream perf — must be deterministic).
- **D(ord)-7** (ND) — multilevel **coarse vertices are numbered by ascending
  lowest-index constituent fine vertex** (a matched pair takes its lower member's
  rank), and the **bisection re-seeds from the lowest-index unassigned vertex** when
  a component is exhausted before the weight target. Together these make coarsening
  + bisection run-independent and give a deterministic, valid colouring even on
  disconnected graphs.

These will be locked in ADR-0065 §16 at v2-close.
