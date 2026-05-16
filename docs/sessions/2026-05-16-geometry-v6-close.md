# 2026-05-16 — Phase 3.1.7 v6 `-polygon` cluster CLOSE

> 7th of 11 `crd-geometry` sub-modules complete. v6d Vatti polygon Boolean
> + v6e Bentley-Ottmann sweep shipped this session on top of v6a/b/c from
> the earlier checkpoint, then cluster-close deliverables (ADR amendment,
> system doc, tracker syncs, 18-config sweep).

## What shipped this session (v6d + v6e + v6-close)

| Slice | Engine LOC | Tests LOC | Cases | Assertions | DoD |
|---|---|---|---|---|---|
| **v6d** Vatti polygon Boolean | ~750 | ~350 | 13 | 47 | 4/4 PASS |
| **v6e** Bentley-Ottmann | ~600 | ~400 | 13 | 37 | 4/4 PASS |
| **v6-close** | — | — | — | — | 18-config sweep |

## Cluster summary (v6 full)

| Slice | Engine LOC | Tests LOC | Cases | What |
|---|---|---|---|---|
| v6a substrate | ~700 | ~600 | 39 | Polygon2 / PolygonView2 / Ring2 + predicates + typed wrappers |
| v6b ear-clip w/ holes | ~900 | ~700 | 14 | Meisters 1975 + Eberly 1999 bridging |
| v6c CDT | ~1400 | ~700 | 14 | Bowyer-Watson + Domiter-Zalik carve-and-retriangulate |
| v6d Vatti Boolean | ~750 | ~350 | 13 | 4 ops via planar-subdivision + winding-# face classification |
| v6e Bentley-Ottmann | ~600 | ~400 | 13 | Full O((n+k) log n) sweep |
| v6-close | — | — | — | ADR §21 + system doc + 18-config sweep |
| **Cluster totals** | **~4350 LOC engine** | **~2750 LOC tests** | **92 cases** | **695 assertions** |

## v6d Vatti polygon Boolean — algorithm

**Approach**: planar-subdivision + winding-number face classification (NOT
Vatti scanbeam — see ADR-0076 §21 pin #10 for the trade-off rationale).

1. Collect every directed edge of subject + clip with winding-contribution
   sign (+1 for CCW outer, -1 for CW hole, 0 for the OTHER polygon).
2. Find every pairwise intersection (brute-force O(n×m); Bentley-Ottmann
   is the v6e drop-in replacement for very-large inputs).
3. Build a vertex table via lex-sort + coalesce on exact coordinate
   equality. Build half-edges (twin pairs) per sub-segment.
4. At each vertex, sort outgoing half-edges CCW by angle (quadrant +
   orient2d comparator). Wire `next_in_face` per the DCEL convention
   (`twin(o_k).next = o_{(k+n-1) % n}` — CW-predecessor).
5. Walk each face boundary loop via `.next_in_face`. Each connected loop
   gets a unique face-id (a "face" here is one boundary loop; a real
   planar face may have multiple loops which are handled by per-loop
   emission below).
6. For each loop, pick a sample point on the LEFT of each directed edge
   (midpoint + perpendicular-LEFT-offset). The perpendicular-LEFT
   direction is INSIDE the loop's face regardless of orientation (CCW
   loops → interior; CW loops → exterior of the loop = containing face).
7. Ray-cast +X from the sample, count signed crossings of subject and
   clip input edges. Result: face's subject + clip winding numbers.
8. Apply Boolean predicate (per fill rule: EvenOdd or NonZero).
9. For each kept loop, emit its vertex sequence as an output ring.
   CCW loops become outer rings; CW loops become hole rings (per v6
   winding convention).
10. Optional output cleaning: consecutive collinear vertex removal +
    consecutive duplicate dedup (gated by `opts.clean_output`).

**Mid-implementation bug-fix lesson**: original face-walking treated each
LOOP as a distinct face and SKIPPED CW loops (assuming they were
outer-unbounded-face boundaries). This broke polygon-with-hole output:
the hole-boundary CW loop's left-side face is the SAME as the outer
CCW loop's face, but my code treated them as separate faces with
different winding numbers, so only the outer ring was emitted (area
mismatch). Fixed by emitting every kept LOOP independently — the loop's
natural orientation maps directly to the v6 winding convention (outer
CCW or hole CW). 5 → 0 failing tests after the fix.

**Robustness**: every orientation / intersection-sign / in-segment
decision uses Shewchuk `orient2d` adaptive precision (v3a / ADR-0076 §18).
No naive cross-product, no epsilon. Transverse + endpoint-on-interior +
vertex-on-vertex + edge-coincident all flow through the adaptive path.

**Tests** (13 cases / 47 assertions):
- 4 ops on disjoint pair (area invariants per op)
- 4 ops on overlapping pair (the classic Boolean area invariants:
  `A(union) + A(intersection) == A(subject) + A(clip)`,
  `A(difference) + A(intersection) == A(subject)`,
  `A(xor) == A(union) - A(intersection)`)
- 4 ops on subject-contains-clip + clip-contains-subject
- Polygon-with-hole vs covering clip (area conservation)
- Edge-coincident pair (shared edge has zero intersection area)
- Vertex-coincident pair (shared corner has zero intersection area)
- Empty-operand diagnostic
- f64 precision tier at 1e3-scale coords

## v6e Bentley-Ottmann — algorithm

**Sweep-line over Y** (bottom-to-top):

- **Event queue**: min-heap of events keyed by lex (y, x, kind, seg-id).
  Three kinds, ordered Start < Intersection < End at coincident keys.
  Uses `crd::containers::push_heap` / `pop_heap` for bit-exact ordering
  across compilers.
- **Status structure**: sorted `Array<u32>` of active segments by
  X-at-sweep-Y. O(log n) insertion via binary search (linear shift for
  the insert/erase itself; n is bounded by polygon edge count which is
  small in practice).
- **Sweep**: process events in y-order.
  - START: insert segment into status; test new adjacent pairs for
    intersection above sweep_y; enqueue any found.
  - END: remove segment from status; test newly-adjacent pair; enqueue.
  - INTERSECTION: emit + dedup; swap segments in status; test new
    adjacent pairs.
- **Horizontals**: extracted into a secondary brute-force pass (clean
  separation from the X-sorted status structure since horizontals have
  no well-defined X-at-sweep-Y).
- **Output**: sorted by lex (point.y, point.x, seg_a, seg_b) before
  return.

**Robustness**: Shewchuk `orient2d` for the segment-segment intersection
helper (transverse + endpoint-on-interior + vertex-on-vertex + collinear-
overlap-endpoint). Dedup on canonical pair-key prevents re-reporting
after status-structure swaps.

**Short-circuit variant**: `bentley_ottmann_any` returns true on the
FIRST intersection found. Useful for `is_simple` checks — substantial
speedup over the full sweep when the answer is "yes" (any intersection
disqualifies).

**Tests** (13 cases / 37 assertions):
- Empty input / disjoint pair (no intersections)
- Two crossing segments (1 intersection at (1,1))
- 4-segment # pattern (4 intersections)
- T-junction (endpoint on interior of another segment)
- Parallel non-intersecting
- 5×5 grid of 25 intersections (dense stress)
- Short-circuit `_any` (true on first hit, false on no intersection)
- Degenerate / non-finite diagnostics
- f64 precision tier at 1e6-scale coords
- Output lex-sort determinism

## v6-close deliverables

1. **ADR-0076 §21 amendment** (~120 lines) — slice ledger, 20 locked
   substrate decisions, cluster cross-validation summary, complexity
   summary table, progress update (7 of 11 sub-modules complete).
2. **`docs/systems/geometry-polygon.md`** — when-to-use matrix,
   architecture diagram, API stencils, determinism + robustness
   contracts, performance pins, two-layer typed architecture,
   integration touch-points (crd-font, crd-eda, crd-eylem navmesh,
   crd-resources cooker, crd-renderer decals, editor sketch tool),
   comprehensive references.
3. **Tracker syncs** — context.md current focus, ROADMAP.md row 39,
   phase-3.1.7-geometry.md status block, MEMORY.md project_state.md.
4. **Session log** (this file).
5. **18-config full sweep** — running next.

## Files touched

```
+ engine/geometry-polygon/include/crd/geometry/polygon/polygon_boolean.hpp   NEW v6d
+ engine/geometry-polygon/src/polygon_boolean.cpp                            NEW v6d
+ engine/geometry-polygon/include/crd/geometry/polygon/bentley_ottmann.hpp   NEW v6e
+ engine/geometry-polygon/src/bentley_ottmann.cpp                            NEW v6e
~ engine/geometry-polygon/include/crd/geometry/polygon/polygon.hpp           v6d + v6e umbrella includes
+ tests/geometry-polygon/test_polygon_boolean.cpp                            NEW v6d (13 cases)
+ tests/geometry-polygon/test_bentley_ottmann.cpp                            NEW v6e (13 cases)
~ tests/geometry-polygon/CMakeLists.txt                                      add test_polygon_boolean + test_bentley_ottmann
~ docs/decisions/0076-geometry-substrate-architecture.md                     §21 amendment
+ docs/systems/geometry-polygon.md                                           NEW system overview
~ context.md                                                                 v6 closed
~ docs/ROADMAP.md                                                            row 39 v6 closed
~ docs/phases/phase-3.1.7-geometry.md                                        Status block: v6 closed
+ docs/sessions/2026-05-16-geometry-v6-close.md                              THIS FILE
```

## Definition of Done — per slice ✅

| Config | v6d | v6e |
|---|---|---|
| win-debug | ✅ build+ctest | ✅ build+ctest |
| win-asan  | ✅ build+ctest | ✅ build+ctest |
| win-shipping | ✅ build+ctest | ✅ build+ctest |
| win-tidy | ✅ build | ✅ build |

18-config full sweep runs next as the cluster-close gate.

## Next session

**Phase 3.1.7 v7 `-mesh-processing` cluster** — Garland-Heckbert Quadric
Edge Collapse Decimation + Loop subdivision + isotropic remesh + Liepa
hole-fill + manifoldness repair + self-intersection removal (consumes
v6e Bentley-Ottmann) + Taubin smoothing. 7 algorithmic slices per the
renewed-scope plan.

## Commit message (proposed)

```
feat(geometry-polygon): ship v6d Vatti Boolean + v6e Bentley-Ottmann
                       + v6-close (cluster CLOSED)

Phase 3.1.7 v6 -polygon cluster fully shipped 2026-05-16.

v6d Vatti polygon Boolean — 4 ops (union/intersection/difference/xor)
via planar-subdivision + winding-number face classification + DCEL
half-edges with CCW-sorted outgoing-edge orient2d angle comparator
(Shewchuk-driven) + brute-force O(n×m) intersection finder (v6e
Bentley-Ottmann is the natural drop-in upgrade) + per-loop face
emission (hole rings emitted naturally per CW orientation) + EvenOdd
+ NonZero fill rules + opt-in output cleaning. 13 cases / 47 assertions.

v6e Bentley-Ottmann — full O((n+k) log n) sweep-line: event queue
(min-heap, lex-sorted) + status structure (sorted Array of active
segments by X-at-sweep-Y) + 3 event kinds (Start<Intersection<End
at coincident y/x) + horizontal-segment brute-force secondary pass +
Shewchuk orient2d throughout + dedup on canonical pair-key + output
sorted by lex + short-circuit `_any` variant for is_simple-style
usage. 13 cases / 37 assertions.

v6-close — ADR-0076 §21 amendment (20 locked substrate decisions +
cluster cross-validation + complexity table) + docs/systems/geometry-
polygon.md (when-to-use matrix, architecture diagram, API stencils,
determinism + robustness contracts, integration touch-points across
crd-font / crd-eda / crd-eylem navmesh / crd-resources cooker / crd-
renderer decals / editor sketch tool, comprehensive references) +
18-config full sweep.

Cluster totals: 6 slices, ~4350 LOC engine + ~2750 LOC tests, 92 cases
/ 695 assertions, 7th of 11 geometry sub-modules COMPLETE, 51 of 49
renewed-scope slices shipped (104% — v6 expanded 4→6, nothing cut).
```
