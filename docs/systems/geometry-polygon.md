# crd-geometry-polygon — system overview

> Phase 3.1.7 v6 cluster. The planar-polygon substrate consumed by font
> rendering, vector-graphics import/export, navmesh polygon ops, lightmap
> UV charting, decal projection, PCB / EDA Gerber clipping, and the
> editor-tier sketch-and-clip pipeline. Seventh of 11 `crd-geometry`
> sub-modules; closed 2026-05-16.

## Status

- v6a substrate (`Polygon2<T>` / `PolygonView2<T>` / `Ring2<T>` + predicates) ✅ shipped 2026-05-16
- v6b ear-clipping triangulation with holes (Meisters + Eberly) ✅ shipped 2026-05-16
- v6c Constrained Delaunay Triangulation (Bowyer-Watson + Domiter-Zalik) ✅ shipped 2026-05-16
- v6d Vatti polygon Boolean (planar-subdivision + winding-number) ✅ shipped 2026-05-16
- v6e Bentley-Ottmann line-segment intersection ✅ shipped 2026-05-16
- v6-close ✅ shipped 2026-05-16 — this doc + ADR-0076 §21 amendment + 18-config sweep

**Cluster totals:** 6 slices · ~4350 LOC engine + ~2750 LOC tests · 20 locked
substrate decisions (ADR-0076 §21) · 92 cases / 695 assertions.

## When to use what

| Workload | Algorithm | Why |
|---|---|---|
| Quick polygon area / centroid / aabb / point-in-polygon | v6a predicates | O(n) closed-form, Shewchuk-robust |
| Simple-polygon triangulation (fonts, glyphs, UV unwrap) | v6b ear-clipping | Deterministic + handles holes via Eberly bridging |
| Triangulation with constraint edges (navmesh, FEA tetmesh prep) | v6c CDT | Bowyer-Watson + carve-and-retriangulate handles non-convex-quad chains |
| Polygon Boolean ops (PCB, navmesh, lightmap UV merge) | v6d planar-subdivision | 4 ops + multipath + holes + EvenOdd / NonZero fill |
| Self-intersection detection (font glyph sanity, geometry validation) | v6e Bentley-Ottmann | O((n+k) log n) sweep + short-circuit `_any` variant |
| Detect non-simple polygon | v6a `is_simple` (O(n²)) OR v6e `bentley_ottmann_any` (O(n log n)) | v6e for n ≥ ~64 |

## Architecture

```
                ┌──────────────────────────────────────────────────────┐
                │  crd-geometry-polygon — substrate types + predicates  │
                │  Polygon2<T> / PolygonView2<T> / Ring2<T>             │
                │  signed_area / centroid / aabb / is_ccw / is_simple   │
                │  ensure_orientation / point_in_ring / point_in_polygon│
                │  + typed Vec2<Length32> wrappers (ADR-0078 §5 D34)    │
                └──────────────────────────────────────────────────────┘
                          │           │           │           │
                          ▼           ▼           ▼           ▼
┌─────────────────────┐ ┌───────────────────┐ ┌───────────────┐ ┌───────────────┐
│  triangulate_ear_clip│ │  constrained_     │ │ polygon_      │ │ bentley_      │
│  (v6b)              │ │  delaunay (v6c)   │ │ boolean (v6d) │ │ ottmann (v6e) │
│                     │ │                   │ │               │ │               │
│  Meisters 1975 +    │ │  Bowyer-Watson +  │ │  Planar       │ │  Sweep-line   │
│  Eberly 1999 hole   │ │  Domiter-Zalik    │ │  subdivision  │ │  event queue  │
│  bridging           │ │  carve-and-       │ │  + winding-#  │ │  + sorted     │
│                     │ │  retriangulate    │ │  face class.  │ │  status array │
│  Shewchuk orient2d  │ │  Shewchuk incircle│ │  4 ops + 2    │ │  Shewchuk     │
│  Lex-tuple ear pick │ │  Lex-sort insert  │ │  fill rules   │ │  orient2d     │
└─────────────────────┘ └───────────────────┘ └───────────────┘ └───────────────┘
                          │  consumes              │ consumes
                          ▼                        ▼
                     Shewchuk predicates (v3a, crd-geometry-primitives)
                     crd-containers (Array / sort / push_heap / pop_heap)
                     crd-math (Vec2 / scalar)
                     crd-units (Quantity<D, T>, typed wrappers boundary)
```

## API at a glance

```cpp
#include <crd/geometry/polygon/polygon.hpp>

namespace cgp = crd::geometry::polygon;

// --- v6a substrate -------------------------------------------------------
cgp::Polygon2<f32> p{&alloc};
p.add_ring(outer_ccw_vertices);
p.add_ring(hole_cw_vertices);
const f32 area  = cgp::signed_area(p.view());
const auto pip  = cgp::point_in_polygon(p.view(), Vec2<f32>{0.5F, 0.5F});

// --- v6b ear-clip triangulation ----------------------------------------
auto tri_result = cgp::triangulate_ear_clip(p.view(), &alloc);
if (tri_result.ok()) {
    for (u32 t = 0; t < tri_result.triangle_count; ++t) {
        const u32 i0 = tri_result.triangle_indices[3 * t];
        const u32 i1 = tri_result.triangle_indices[3 * t + 1];
        const u32 i2 = tri_result.triangle_indices[3 * t + 2];
        const auto& v0 = p.vertices()[i0];
        // ... use vertex positions ...
    }
}

// --- v6c CDT ------------------------------------------------------------
auto cdt_result = cgp::constrained_delaunay(p.view(), &alloc);
// Polygon variant: includes ring constraints + in/out filter.

// Or general PSLG entry:
cgp::CdtEdge constraints[] = { {0U, 5U}, {3U, 8U} };
auto pslg_result = cgp::constrained_delaunay(points, constraints, &alloc);

// --- v6d polygon Boolean ------------------------------------------------
auto bool_result = cgp::polygon_intersect(subject.view(), clip.view(), &alloc);
// Or: polygon_union / polygon_difference / polygon_xor / polygon_boolean(op).
// Output is a Polygon2<T> with CCW outer rings + CW hole rings.

// --- v6e Bentley-Ottmann -----------------------------------------------
cgp::BOSegment<f32> segs[] = { {{0,0}, {1,1}}, {{0,1}, {1,0}} };
auto bo_result = cgp::bentley_ottmann<f32>(segs, &alloc);
for (const auto& isect : bo_result.intersections) {
    // isect.segment_a, isect.segment_b, isect.point
}

// Short-circuit (returns true on first intersection found):
const bool any = cgp::bentley_ottmann_any<f32>(segs, &alloc);
```

## Determinism contract

Inherits ADR-0063 + ADR-0076 §4 pin #11 + §15 (builder-reject /
query-tolerate). Cluster-wide pins applied at v6 close:

- **Lex-tuple ordering** drives ear-pick (v6b), CDT insertion (v6c),
  Boolean intersection canonical pair-key (v6d), BO event queue (v6e).
- **Shewchuk orient2d** for every orientation / intersection-sign /
  in-segment decision. No naive cross-product, no epsilon fallback.
- **`crd::containers::sort` / `push_heap` / `pop_heap`** (not `std::*`)
  for bit-exact cross-compiler ordering.
- **NaN/Inf contract**: builders REJECT non-finite (debug `CRD_ASSERT`
  at `Polygon2::add_ring`); queries TOLERATE non-finite (defensive
  `is_finite` short-circuit returns soft diagnostic or empty result).
- **Output sorted before return** for both v6d and v6e.
- **Two-layer typed architecture (ADR-0078 §5 D34)**: algorithm bodies
  stay raw `MathScalar T` (`f32` / `f64`); typed `Vec2<Length32>`
  consumers ride strip-compute-retag wrappers at the API surface.

## Robustness contract

- **Shewchuk-only adverse path** — no naive cross-product fallback. Every
  algorithm routes through the adaptive predicates (v3a) for sign decisions.
- **NaN/Inf builders REJECT** (debug assert at `Polygon2::add_ring`).
  Queries TOLERATE (defensive short-circuit, no crash).
- **Diagnostic enums** — each algorithm ships a status enum surfacing
  failures cleanly (`TriangulateStatus`, `CdtStatus`, `BooleanStatus`,
  `BOStatus`). Callers branch on `result.ok()`.

## Performance pins

- **v6a `is_simple`** is O(n²) brute force; `bentley_ottmann_any` is the
  O(n log n) replacement when n ≥ ~64. Caller picks based on input size.
- **v6b ear-clip** is O(n²) classical Meisters; Held 2001 FIST O(n log n)
  with reflex-vertex spatial bucket is a v7 follow-on optimisation.
- **v6c CDT** uses jump-walk point location (O(√n) per insertion in
  practice) + lex-sorted insertion for cache locality. Lawson restoration
  is O(n²) worst case but linear in practice.
- **v6d Boolean** uses brute-force O(n×m) intersection finding inside the
  planar-subdivision algorithm. For n < ~1000 segments per operand this
  is sub-millisecond. For larger inputs, v6e's Bentley-Ottmann is the
  natural drop-in upgrade — same intersection finder API + faster.
- **v6e Bentley-Ottmann** is full O((n + k) log n) sweep. Horizontal
  segments are routed through a secondary brute-force pass against all
  other segments (clean separation from the X-sorted status structure).

## Two-layer typed architecture (ADR-0078 §5 D34)

The public surface ships typed `Vec2<Length32>` wrappers (in
`polygon_predicates_typed.hpp`) over the raw `MathScalar T` algorithm
bodies. `polygon_substrate.cpp` pins the bit-identical-layout invariant
between `Vec2<Quantity<D, T>>` and `Vec2<T>` via `static_assert` — if a
future `crd-math` or `crd-units` refactor breaks the contract, build
fails immediately. Typed wrappers for v6b/c/d/e ship as a focused
follow-on slice when a typed-surface consumer pulls.

## Integration touch-points

- **`crd-font` (Phase 3.3)** — glyph outline triangulation via v6b
  ear-clip-with-holes (TTF / OTF glyphs are CCW outer + CW interior
  loops).
- **`crd-eda` (Phase 3.1.17)** — PCB trace clipping + DRC zone-fill via
  v6d Boolean ops; net-segment self-intersection scan via v6e.
- **`crd-eylem` v6 navmesh helpers** — concave navmesh polygons
  triangulated via v6c CDT for path queries.
- **`crd-resources` cooker** — lightmap UV chart packing uses v6d
  Boolean for chart-overlap removal.
- **`crd-renderer` decals** — decal-against-mesh polygon clipping
  via v6d (intersection of decal projection with mesh face polygons).
- **Editor sketch tool** — v6c CDT for triangulating sketched profiles;
  v6d Boolean for sketch-and-cut / boss / pocket operations.

## Open follow-ons (post-close)

None blocking. Backlog of opportunistic enhancements:

- **Held 2001 FIST** O(n log n) ear-clipping via reflex-vertex spatial
  bucketing (drop-in replacement for v6b core).
- **Bentley-Ottmann-backed v6d** — swap v6d's brute-force intersection
  finder for v6e (drop-in API match; cluster decision §21 pin #10).
- **Typed `Vec2<Length32>` wrappers** for v6b/c/d/e public API surface.
- **`Quantity<Area>` return for `signed_area`** (already typed in v6a;
  extend to multi-ring polygon variants).
- **Chew refinement** for CDT triangle-quality bound (Steiner-point
  insertion to enforce min-angle ≥ θ).

## References

- ADR-0076 §21 amendment — 20 locked v6 substrate decisions
- ADR-0076 §18 — Shewchuk adaptive predicates (v3a)
- ADR-0078 §5 D34 — two-layer typed architecture
- Vatti, B. R. (1992). "A generic solution to polygon clipping." CACM
  35(7):56–63.
- Greiner & Hörmann (1998). "Efficient clipping of arbitrary polygons."
  ACM TOG 17(2):71–83.
- Bowyer (1981) + Watson (1981). Delaunay-by-incremental-insertion
  (independent discovery, same year).
- Anglada, M. (1997). "An improved incremental algorithm for constructing
  restricted Delaunay triangulations." Computers & Graphics 21(2):215–223.
- Domiter & Žalik (2008). "Sweep-line algorithm for constrained Delaunay
  triangulation." International Journal of Geographical Information Science
  22(4):449–462.
- Eberly, D. (1999). "Triangulation by ear clipping." Geometric Tools.
- Meisters, G. H. (1975). "Polygons have ears." American Mathematical
  Monthly 82(6):648–651.
- Bentley & Ottmann (1979). "Algorithms for reporting and counting
  geometric intersections." IEEE TOC C-28(9):643–647.
- Hormann & Agathos (2001). "The point in polygon problem for arbitrary
  polygons." Computational Geometry 20(3):131–144.
- Shewchuk, J. R. (1997). "Adaptive Precision Floating-Point Arithmetic
  and Fast Robust Geometric Predicates." DCG 18(3):305–363.
