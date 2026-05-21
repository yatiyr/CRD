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
| v2c | Full symbolic factorisation (etree + colcounts + L-pattern + supernodes). |
| v2d/e | Full multilevel-METIS nested dissection. |

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

These will be locked in ADR-0065 §16 at v2-close.
