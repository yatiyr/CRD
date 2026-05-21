# 2026-05-21 — Phase 3.1.6 `crd-hesap` v2d: multilevel-ND scaffold

> Module: `crd-hesap-ordering`. The METIS multilevel chassis — coarsen →
> bipartition coarsest → uncoarsen-project — producing a valid, un-refined 2-way
> partition. The frame v2e bolts Fiduccia-Mattheyses refinement + the recursive
> `nd_order` driver onto. **CLOSED — 4-config DoD PASS.**

## What shipped

- **`struct WeightedGraph`** — a coarsening level: CSR `xadj`/`adjncy` (symmetric,
  ascending, diagonal-free) + `adjwgt` (edge weights ‖ adjncy) + `vwgt` (vertex
  weights). Base level all-unit; coarse levels accumulate → Σ`vwgt` conserved
  (== base n) at every level.
- **`detail::coarsen_match`** — heavy-edge matching (ascending visit; heaviest-edge
  unmatched neighbour, ties → lowest index, D(ord)-1); coarse ids numbered by
  ascending lowest fine member (D(ord)-7).
- **`detail::contract`** — coarser graph via counting-sort grouping + timestamped
  marker accumulator: sum parallel-edge `adjwgt`, sum `vwgt`, drop self-loops, sort
  each row ascending (D(ord)-2/-4).
- **`detail::coarsen`** — level stack; stop at `n ≤ 100` OR matching stall
  (`n_coarse ≥ 0.9·n`) OR `30` levels.
- **`detail::bisect_coarsest`** — **re-seeding BFS region-grow**: grow part 0 from
  the lowest-index seed to half the total vertex weight; re-seed from the lowest-
  index unassigned vertex when a component is exhausted early (D(ord)-7, the
  disconnected-graph contract); remainder → part 1.
- **`detail::project_down`** + **`detail::edge_cut`** (weighted cut, the v2e FM metric).
- **Public `nd_bipartition(pattern, alloc)`** — full pipeline → `part[v] ∈ {0,1}`;
  n=0 → empty, n=1 → {0}.
- **CLI** `hesap.ordering.nd_bipartition` → **14 ordering commands**. Shipped now,
  NOT deferred to v2e (user directive: don't defer without asking). `nd_bipartition`
  is a real op, so the per-op-CLI rule applies even though it's scaffold-stage.

## Results — scaffold quality

Region-grow to half-weight gives essentially **perfect balance** with zero
refinement:

| graph | n | part0 / part1 | notes |
|---|---|---|---|
| tridiagonal | 50 | 25 / 25 | cut = 1 (optimal) |
| grid 12×12 | 144 | 72 / 72 | |
| grid 15×15 | 225 | 112 / 113 | multilevel: coarsest 57 over 3 levels |
| grid 9×20 | 180 | 90 / 90 | |
| **disconnected** 2×tridiag | 54 | 27 / 27 | re-seeding colours both blocks + balances across components |

The 2-D grid edge-cut (longer than optimal for an un-refined region boundary) is
the baseline v2e's Fiduccia-Mattheyses will straighten.

## Decisions / notes

- **No new fill numbers at v2d.** Fill (`nnz_l`) needs an ND *ordering* (separator
  extraction), which is v2e. The scaffold's intrinsic quality metric is edge-cut +
  balance (published above) — exactly what v2e's FM improves.
- **+D(ord)-7 pinned** (system doc): coarse vertices numbered by ascending lowest
  fine member; bisection re-seeds from the lowest-index unassigned vertex. Locks
  coarsening + bisection determinism incl. disconnected graphs. ADR-0065 §16 will
  lock D(ord)-1..7 at v2-close.
- **CLI shipped now, not deferred** — per user directive. (Contrast: v2b-1's
  `quotient_fill` stayed `detail::`-only because it's pure internal machinery;
  `nd_bipartition` is a user-facing op.)
- `column_counts` (v2a-public) still has no CLI — the one remaining gap for the
  v2e CLI-completeness audit.

## Traps hit

- **`detail::WeightedGraph` mis-qualification** — `WeightedGraph` lives in
  `crd::hesap::ordering`, not `detail`; qualifying it `detail::` in `nd_bipartition`
  produced a cascade of misleading `operator[]` errors (the real first error was
  the bad qualified lookup). Fixed by dropping the `detail::` prefix.
- **clang-tidy `readability-identifier-naming`** — a *local* `const kUnmatch`
  tripped the rule (k-prefix is reserved for namespace/global constants; locals are
  `lower_case`). Renamed to `unmatched`. (Namespace-scope `kCoarsestMax`/`kMaxLevels`
  are correctly k-prefixed.)
- **clang-tidy `readability-isolate-declaration`** — `const u32 n1=…, n2=…, n=…;`
  in a test split into one declaration per line.

## Verification

- `crd-hesap-ordering-tests`: 25 cases / 5664 assertions PASS (+10 v2d cases).
- Invariants: matching validity (≤2 fine per coarse) · Σvwgt conservation per level
  · coarse-graph well-formedness (symmetric/sorted/no-self-loop/wgt≥1) · coarsening
  termination (≤30 levels) · partition validity + balance · disconnected-graph
  contract · n=0/1/2 · determinism bit-identical.
- 4-config per-slice DoD (debug + asan + shipping + tidy): **PASS.**

## Next

**v2e** — Fiduccia-Mattheyses refinement (gain buckets, equal-gain by D(ord)-1)
during uncoarsening + vertex-separator extraction from the edge cut +
recursive-bisection `nd_order` driver + AMD-hybrid small-graph threshold. **Gate:
beats AMD fill on the structured-mesh (FEM) SuiteSparse subset.** v2-close: CLI
completeness (incl. `column_counts`) + ADR-0065 §16 (lock D(ord)-1..7) + 18-config
sweep.
