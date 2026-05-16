# 2026-05-16 — Phase 3.1.7 v6 `-polygon` cluster: v6a + v6b + v6c

> Half-cluster checkpoint. New `engine/geometry-polygon/` module — the
> 7th `crd-geometry-*` sub-module. v6d (Vatti polygon Boolean) +
> v6e (Bentley-Ottmann) + v6-close deferred to a focused next session
> per the 2026-05-16 checkpoint decision: v6d is the heaviest slice in
> the cluster and deserves dedicated time.

## What shipped

**`engine/geometry-polygon/`** new module. Three slices in this session:

| Slice | Engine LOC | Tests LOC | Cases | Assertions | DoD |
|---|---|---|---|---|---|
| **v6a** substrate (Polygon2 / PolygonView2 / Ring2 + predicates + typed wrappers) | ~700 | ~600 | 39 | 151 | 4/4 PASS |
| **v6b** ear-clip w/ holes (Meisters + Eberly bridging) | ~900 | ~700 | 14 | 50 | 4/4 PASS |
| **v6c** Constrained Delaunay Triangulation | ~1400 | ~700 | 14 | 410 | 4/4 PASS |
| **Totals** | **~3000 LOC engine** | **~2000 LOC tests** | **67 cases** | **611 assertions** | **all 4-config green** |

Full project ctest 2093 → **2160 win-debug** (+67 cases).

## Slice details

### v6a — substrate types + predicates

**Files**
- `engine/geometry-polygon/include/crd/geometry/polygon/polygon_types.hpp` — `Ring2<T>` (non-owning view; closed-ring next/prev wraparound), `PolygonView2<T>` (multi-ring view with ring_offsets prefix-sum), `Polygon2<T>` (owning, add_ring with debug finite-input assertion + winding-convention enforced via caller contract).
- `engine/geometry-polygon/include/crd/geometry/polygon/polygon_predicates.hpp` — `signed_area` (shoelace via f64 accumulator regardless of T), `aabb` (with +∞/−∞ empty sentinel matching ADR-0076 §15 convention), `centroid` (Bourke 1988 area-weighted + arithmetic-mean fallback for zero area), `is_ccw` / `is_cw`, `ensure_orientation` (auto-reverse mismatched rings), `point_in_ring` / `point_in_polygon` (Hormann-Agathos 2001 crossing-number + Shewchuk `orient2d` adverse for boundary cases), `is_simple` (O(n²) brute-force for now; O(n log n) Bentley-Ottmann lands at v6e).
- `engine/geometry-polygon/include/crd/geometry/polygon/polygon_predicates_typed.hpp` — typed `Vec2<Length32>` strip-compute-retag wrappers per ADR-0078 §5 D34. `signed_area` returns `Quantity<DimMul<D,D>, T>` (typed Area); `centroid` / `aabb` retag to `Length32`; `point_in_ring` / `is_ccw` pass through.
- `engine/geometry-polygon/src/polygon_substrate.cpp` — `static_assert` pins for the typed-wrapper contract: `sizeof(Vec2<Quantity<D,T>>) == sizeof(Vec2<T>)` + alignof matches (ADR-0078 §2 D2 layout pin). If a future `crd-math` or `crd-units` refactor breaks the contract this TU fails to build immediately.

**Pinned decisions**
- **Winding convention** (LOCKED): outer ring CCW, hole rings CW. Matches Clipper2 + Vatti 1992 + Eberly 1999. Builders never silently re-orient; `ensure_orientation` is the explicit helper for raw input.
- **Implicit closure**: last vertex IS connected to first; do not duplicate.
- **`ring_offsets` prefix-sum**: size `ring_count + 1`; trailing entry is past-the-end into vertices. Identical convention to `ConvexHullView::face_vertex_offsets` (ADR-0076 §15 v1h pin) — caller code can be polymorphic across the two with one less indirection.
- **`Quantity<D,T>` layout pin**: enforced by `static_assert` in `polygon_substrate.cpp`. Typed wrapper bridges via `reinterpret_cast` at the API boundary; if layout drifts, build fails.

### v6b — ear-clipping triangulation with holes

**Files**
- `engine/geometry-polygon/include/crd/geometry/polygon/triangulate_ear_clip.hpp` — `triangulate_ear_clip(Ring2)` + `triangulate_ear_clip(PolygonView2)` + `TriangulationResult` + 7-state `TriangulateStatus` enum.
- `engine/geometry-polygon/src/triangulate_ear_clip.cpp` — classical Meisters 1975 O(n²) ear-clip + Eberly 1999 cut-and-join hole bridging.

**Algorithm**
- **Doubly-linked vertex list** via twin `Array<u32>` next/prev (no STL list per PRINCIPLES.md no-owning-STL rule).
- **Convex / reflex classification** per vertex via Shewchuk `orient2d`; collinear → reflex (never ears).
- **Ear pick**: scan candidate-ear set, pick smallest current vertex index. Lex-tuple tiebreak → byte-identical output across compilers / SIMD widths / OSes.
- **Hole bridging (Eberly)**: holes processed in DESCENDING max-x order. For each hole: find rightmost vertex M (max-x; max-y tie; min-idx tie); cast +X ray to find closest outer edge; pick Eberly candidate (larger-x endpoint, refined by any reflex vertex inside the search triangle); insert doubled-bridge `[..., V, M, hole-rotated-from-M, M, V, ...]` — the two coincident (V, M) edges become zero-width slivers that ear-clipping eats naturally.

**Validation**
- 14 cases: unit square / triangle / L-shape / 3-toothed comb / 5-point star / 16-gon convex / square-with-hole / square-with-2-holes / f64-large-coord / vertex-rotation determinism / 4 diagnostic statuses (EmptyPolygon / NonSimpleOuter / NonSimpleHole / etc.) / CCW-invariant on every output triangle.
- Area-conservation invariant verified: `Σ tri_area == 2 * polygon signed_area` for all non-degenerate cases.

### v6c — Constrained Delaunay Triangulation

**Files**
- `engine/geometry-polygon/include/crd/geometry/polygon/cdt.hpp` — `constrained_delaunay(points, constraints)` + `constrained_delaunay(PolygonView2)` + `CdtEdge` / `CdtOptions` / `CdtResult` + 7-state `CdtStatus`.
- `engine/geometry-polygon/src/cdt.cpp` — ~1100 LOC of Bowyer-Watson + Domiter-Zalik carve-and-retriangulate.

**Algorithm sketch**
1. **Bowyer-Watson incremental Delaunay** (Bowyer / Watson 1981). Insert input points in lex-sorted (x, y, original-index) order. Super-triangle envelopes the input bbox by 1000×. For each insertion: locate triangle via jump-walk from `hint_tri`; BFS-expand "bad triangles" (incircle > 0) in monotonic ID order; cavity boundary = bad→non-bad edges; re-triangulate cavity by fanning from new vertex.
2. **Constraint recovery — fast path**: `find_edge(va, vb)` — if the edge already exists, mark constrained on both sides.
3. **Constraint recovery — Domiter-Zalik carve-and-retriangulate** (the elite-tier fallback for arbitrary multi-flip cases including non-convex-quad chains):
   - **Chain trace**: walk from `va` toward `vb`. Starting triangle: cone test at `va` (strict orient2d signs). Subsequent triangles: apex-side orient2d on segment determines exit edge. Records `(triangle, exit_edge)` pairs until terminating triangle (containing `vb`) is found.
   - **Partition into sub-polygons**: classify each chain-triangle vertex by side of segment `(va, vb)` via orient2d. Build `upper.verts` (CCW: va → top_i → ... → vb) and `lower.verts` (CCW: va ← bot_i ← ... ← vb, then reversed).
   - **Carve**: free all chain triangles + final triangle.
   - **Re-triangulate** each sub-polygon by ear-clipping (inlined to avoid cross-module dep; lex-tuple ear pick + Shewchuk orient2d).
   - **Re-link neighbours** via canonical-edge match against captured `OuterLink` records; mark the (va, vb) edge constrained on both sides of the shared sub-polygon boundary.
4. **Lawson 1977 Delaunay restoration**: scan every non-constrained edge; if `incircle > 0` for the neighbour's apex, flip. Re-queue four surrounding edges. Terminates in O(n²) worst case.
5. **Finalisation**: strip super-triangle vertices; emit triangle list referencing only input-point indices.
6. **Polygon convenience entry**: ring vertices + ring-adjacent edges → CDT → centroid in/out filter (even-odd ring fill) removes triangles in holes.

**Mid-implementation lessons (preserved for future debugging context)**
- **Flip convexity bug**: original convexity check used `orient(pc, pd, pa) > 0` + `orient(pc, pb, pd) > 0`, which silently rejected valid flips. Correct test: `orient(pa, pd, pc) > 0` AND `orient(pd, pb, pc) > 0` — these are the orient2d signs that confirm new triangles `T' = (va, vd, vc)` and `U' = (vd, vb, vc)` are CCW. Diagnosed by manually tracing the 4-corner cocircular square test.
- **Naive walk-around-vertex insufficient for multi-flip**: if any quad along the chain is non-convex (e.g., interior vertex inside the convex hull of its neighbours), the edge can't be flipped. Pure flip-recovery dead-ends. **Solution**: trace the full chain explicitly, then carve-and-retriangulate the strip with ear-clipping. The Domiter-Zalik 2008 approach.
- **`emit_triangles<T>(...)` needs explicit template arg**: passing `nullptr` for `const PolygonView2<T>*` can't deduce T, so caller specifies it explicitly.
- **`crd::containers::sort` (not `crd::math::sort`)**: the deterministic sort lives in `crd/containers/sort.hpp`. Easy to mis-typo as `math::sort`.
- **`PolygonView2::vertices` is a member variable (not method)**: `poly.vertices` not `poly.vertices()` — caller code that uses the wrong form gets C2064 "term does not evaluate to a function" from MSVC.
- **`[[maybe_unused]]` on for-each binding** for NDEBUG-stripped CRD_ASSERT: `for ([[maybe_unused]] const auto& v : ring) { CRD_ASSERT(is_finite(v)); }` — without it, win-shipping W4 trips on unused-variable.
- **Em-dash in `TEST_CASE` names** trips `crd-no-non-ascii-test-names` guard (engine-wide policy from v3-close debt-paydown). Always use ASCII hyphen.

**Validation**
- 14 cases / 410 assertions: basic Delaunay (3-pt / 4-pt square / 32-pt random + every-triangle-CCW invariant + empty-circumcircle invariant via Shewchuk incircle directly), constraint-already-edge fast path, **interior-cut single-flip** (4-corner cocircular square w/ diagonal constraint — flips when needed, idempotent when already realised), **long-constraint-crossing-multiple-edges multi-flip** (6-pt configuration with constraint cutting through non-convex-quad chain — the carve-retriangulate test), polygon-with-hole in/out filter, 4 diagnostic-status assertions (TooFewPoints / ConstraintOutOfBounds / DuplicatePoint / NonFiniteInput), f64 1e6-coord stability, insertion-order determinism (lex-sort ⇒ shuffled inputs produce same edge set).

## Files touched

```
+ engine/geometry-polygon/CMakeLists.txt                                   NEW module
+ engine/geometry-polygon/include/crd/geometry/polygon/polygon.hpp         NEW umbrella
+ engine/geometry-polygon/include/crd/geometry/polygon/polygon_types.hpp   NEW v6a
+ engine/geometry-polygon/include/crd/geometry/polygon/polygon_predicates.hpp        NEW v6a
+ engine/geometry-polygon/include/crd/geometry/polygon/polygon_predicates_typed.hpp  NEW v6a
+ engine/geometry-polygon/include/crd/geometry/polygon/triangulate_ear_clip.hpp      NEW v6b
+ engine/geometry-polygon/include/crd/geometry/polygon/cdt.hpp             NEW v6c
+ engine/geometry-polygon/src/polygon_substrate.cpp                        NEW v6a
+ engine/geometry-polygon/src/triangulate_ear_clip.cpp                     NEW v6b
+ engine/geometry-polygon/src/cdt.cpp                                      NEW v6c
~ CMakeLists.txt                                                           add_subdirectory(engine/geometry-polygon)
~ tests/CMakeLists.txt                                                     add_subdirectory(geometry-polygon)
+ tests/geometry-polygon/CMakeLists.txt                                    NEW
+ tests/geometry-polygon/test_polygon_types.cpp                            NEW v6a (7 cases)
+ tests/geometry-polygon/test_polygon_predicates.cpp                       NEW v6a (18 cases)
+ tests/geometry-polygon/test_point_in_polygon.cpp                         NEW v6a (8 cases)
+ tests/geometry-polygon/test_polygon_typed.cpp                            NEW v6a (6 cases)
+ tests/geometry-polygon/test_triangulate_ear_clip.cpp                     NEW v6b (14 cases)
+ tests/geometry-polygon/test_cdt.cpp                                      NEW v6c (14 cases)
~ context.md                                                               v6abc shipped + v6d next
~ docs/ROADMAP.md                                                          row 39 v6 progress
~ docs/phases/phase-3.1.7-geometry.md                                      Status block: v6 in progress
+ docs/sessions/2026-05-16-geometry-v6abc-substrate-earclip-cdt.md         THIS FILE
```

## Definition of Done — per-slice ✅

| Config | v6a | v6b | v6c |
|---|---|---|---|
| win-debug | ✅ build+ctest | ✅ build+ctest | ✅ build+ctest |
| win-asan  | ✅ build+ctest | ✅ build+ctest | ✅ build+ctest |
| win-shipping | ✅ build+ctest | ✅ build+ctest | ✅ build+ctest |
| win-tidy | ✅ build | ✅ build | ✅ build |

Cluster-close 18-config sweep is deferred to v6-close (after v6d + v6e ship).

## Next session

**v6d Vatti polygon Boolean** — union / intersection / difference / XOR over multipath multipolygon with holes. Scanbeam-table sweep, Clipper2 vertex-on-edge convention, Shewchuk `orient2d` adverse path. ~1500-2000 LOC engine + ~800 LOC tests + careful sweep-line debugging — the heaviest slice in the v6 cluster.

Then **v6e Bentley-Ottmann line-segment intersection** (~700 LOC), then **v6-close** (ADR-0076 §21 amendment + `docs/systems/geometry-polygon.md` + 18-config full sweep PASS).

## Commit message (proposed)

```
feat(geometry-polygon): ship v6a + v6b + v6c (substrate / ear-clip / CDT)

Three slices of Phase 3.1.7 v6 -polygon cluster shipped 2026-05-16
(half-cluster checkpoint before v6d Vatti).

v6a: new engine/geometry-polygon/ module + Polygon2<T> / PolygonView2<T>
/ Ring2<T> non-owning + owning types (CCW outer + CW holes winding
convention, prefix-sum ring_offsets, layout-pinned via static_assert)
+ signed_area / centroid / aabb / is_ccw / is_simple / ensure_orientation
/ point_in_ring / point_in_polygon (Hormann-Agathos 2001 + Shewchuk
orient2d adverse) + typed Vec2<Length32> strip-compute-retag wrappers
per ADR-0078 §5 D34. 39 cases / 151 assertions.

v6b: ear-clipping triangulation with hole support — Meisters 1975
classical O(n²) ear-clip + Eberly 1999 cut-and-join hole bridging
(rightmost-vertex pick, +X-ray closest-edge visibility, doubled-bridge
insertion). Lex-tuple deterministic ear pick; Shewchuk orient2d for
every reflex / ear-validity decision. 14 cases / 50 assertions.

v6c: Constrained Delaunay Triangulation — Bowyer-Watson incremental
Delaunay (Shewchuk incircle adaptive + lex-sorted insertion order +
jump-walk point location) + Domiter-Zalik 2008-style carve-and-
retriangulate constraint recovery for arbitrary multi-flip including
non-convex-quad chains (trace chain via apex-side orient2d, partition
into upper/lower sub-polygons split by constraint, free chain triangles,
ear-clip each sub-polygon, re-link neighbours via canonical-edge match)
+ Lawson 1977 post-flip Delaunay restoration over non-constrained edges
+ polygon-with-holes variant with even-odd centroid in/out filter.
14 cases / 410 assertions.

All three slices 4-config DoD PASS (win-debug + win-asan + win-shipping
+ win-tidy). Full project ctest 2093 → 2160 win-debug (+67 cases).

v6d Vatti polygon Boolean + v6e Bentley-Ottmann + v6-close deferred to
next session per 2026-05-16 checkpoint decision.
```

Ready for `git add` + commit + push (your call on whether to push now or
batch with v6d next session).
