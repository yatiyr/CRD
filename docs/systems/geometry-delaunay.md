# crd-geometry-delaunay — system overview

Phase 3.1.7 sub-module 9 of 11. Delaunay triangulation / tetrahedralisation
substrate + Voronoi extraction + relaxation/interpolation/refinement
algorithms built on top. The "compute the Delaunay / Voronoi" half of
geometry; downstream consumers in physics, FEA, scientific visualization,
procedural-content generation, terrain interpolation.

## Status

**Closed 2026-05-17** — 11 algorithm slices + 1 cluster-close, all
shipped + verified via per-slice 4-config DoD + 18-config full sweep.

| Slice | Date | What | Decisions |
|---|---|---|---|
| v8a | 2026-05-17 | 2D Bowyer-Watson (Bowyer 1981 / Watson 1981) | D73-D80 |
| v8b | 2026-05-17 | 2D Hilbert-sort BW (delaunator-style) | D81-D84 |
| v8c-pre | 2026-05-17 | `insphere_exact` Stage D paydown (Shewchuk 1997) | D85-D89 |
| v8c | 2026-05-17 | 3D Bowyer-Watson tetrahedralisation | D90-D94 |
| v8d-2d | 2026-05-17 | 2D Voronoi diagram extraction | D95-D97 |
| v8d-3d | 2026-05-17 | 3D Voronoi cells extraction + ConvexHullView helper | D98-D101 |
| v8e | 2026-05-17 | Lloyd's CVT relaxation (2D + 3D, Lloyd 1982) | D102-D108 |
| v8f | 2026-05-17 | Sibson Natural Neighbour Interpolation 2D (Sibson 1981) | D109-D113 |
| v8g | 2026-05-17 | Ruppert quality-bounded refinement 2D (Ruppert 1995) | D114-D118 |
| v8h | 2026-05-17 | 3D dihedral-bounded refinement (3D-Ruppert analog) | D119-D122 |
| v8-close | 2026-05-17 | ADR §23 + this doc + 18-config sweep | — |

Test suite: **112 cases / 1163 assertions**. ADR-0076 §23 locks all 50
design decisions D73-D122.

## When to use what

| Goal | Use | Notes |
|---|---|---|
| Triangulate a 2D point set | `delaunay_2d` (v8a) or `delaunay_2d_hilbert` (v8b) | Bowyer-Watson incremental; v8b's Hilbert-sort is faster for large N due to spatial-locality jump-walks |
| Tetrahedralise a 3D point set | `delaunay_3d` (v8c) | Bowyer-Watson 3D; uses full Stage D `insphere_exact` (v8c-pre) for cocircular robustness |
| Build 2D Voronoi diagram | `voronoi_2d` (v8d-2d) | Returns cells w/ CCW vertex_indices + bounded/unbounded flag + ray dirs for unbounded |
| Build 3D Voronoi cells | `voronoi_3d` (v8d-3d) + optional `convex_hull_for_cell` | DCEL output + `ConvexHullView` helper for bounded cells |
| Relax a point set to a CVT (uniform distribution) | `lloyd_relax_2d` / `lloyd_relax_3d` (v8e) | 2D supports `HullPolicy::ClipToBbox`; 3D is `Fix`-only in v1 |
| Interpolate scattered values at a query point | `sibson_interpolate_2d` (v8f) or `NniInterpolator2<T>` class | C¹ continuous + reproduces linear functions exactly + bounded (no overshoot) |
| Generate a quality 2D mesh for FEA from PSLG | `ruppert_refine_2d` (v8g) | Refines until every triangle min-angle ≥ α; theoretical termination for α ≤ 20.7° |
| Improve 3D tet-mesh quality (raise minimum dihedral) | `tet_refine_3d` (v8h) | Dihedral-bounded refinement; NOT true sliver removal (= v8h-exude follow-on) |
| Test if point is INSIDE the diametral circle of a segment | direct dot product `dot(A-V, B-V) < 0` | v8g uses it; useful for any encroachment check |
| Compute circumcentre of a 2D triangle | `crd::geometry::primitives::circumcenter_2d` | v8d-2d primitive; lifted to f64 for stability on large coords |
| Compute circumcentre of a 3D tet | `crd::geometry::primitives::circumcenter_3d` | v8d-3d primitive; Cramer's 3x3 lifted to f64 |
| Min dihedral angle of a tet | `min_dihedral_of_tet_rad` | v8h public helper; calibration = regular tet returns arccos(1/3) ≈ 70.5288° |

## Architecture

11 algorithms layered on a core Bowyer-Watson substrate. The dependency
arrows below show how each slice consumes earlier ones:

```
┌────────────────────────────────────────────────────────────────────┐
│  crd-geometry-primitives (Phase 3.1.7 v0)                          │
│  ─────────────────────────────────────────                         │
│  • Shewchuk adaptive predicates (orient2d/orient3d/incircle/insphere)
│    — insphere paid down to FULL Stage D at v8c-pre                  │
│  • circumcenter_2d (added v8d-2d, lifted to f64)                   │
│  • circumcenter_3d (added v8d-3d, Cramer's 3x3 lifted to f64)      │
│  • ConvexHullView<T> + Plane<T> (reused by v8d-3d hull helper)     │
└────────────────────────────────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────┬─────────────────────────────────┐
│ v8a delaunay_2d                 │ v8c delaunay_3d                 │
│ Pure 2D Bowyer-Watson           │ 3D Bowyer-Watson tetrahedrali-  │
│ ───────────────────             │ sation                          │
│ • Tri slot (12+12+1 bytes)      │ • Tet slot ≤ 40 bytes (static_  │
│ • TriPool LIFO free-list        │   asserted)                     │
│ • super-tri at 1000× bbox       │ • TetPool LIFO free-list        │
│ • jump-walk via orient2d        │ • face_vertices[4][3] CCW perm  │
│ • cavity BFS via incircle       │   table (transposition-parity   │
│ • fan + O(K²) neighbour wire    │   verified)                     │
│ • lex-sort insertion order      │ • super-tet matches Shewchuk    │
│ • super-tri stripped at output  │   "orient3d > 0 iff d below"    │
│                                 │ • defensive star-shape check    │
│ Internal header:                │ • Coplanar diagnostic (new)     │
│ delaunay_2d_internal.hpp        │                                 │
│ (shared with v8b)               │                                 │
└─────────────────────────────────┴─────────────────────────────────┘
        │                                  │
        ▼                                  ▼
┌─────────────────────┐    ┌─────────────────────────────────────────┐
│ v8b delaunay_2d_    │    │ v8c-pre insphere_exact Stage D paydown  │
│   hilbert           │    │ ───────────────────────────────────     │
│ ──────────────────  │    │ Literal port of Shewchuk insphereexact  │
│ Hilbert-curve sort  │    │ (predicates.c v4.0.0 lines 3346-3601):  │
│ (Skilling 2004      │    │ • 10 pairwise (x, y) 2D minors          │
│ iterative xy2d on   │    │ • 10 trio 24-elt cofactor expansions    │
│ 2^16 grid)          │    │ • 5 quad 96-elt expansions              │
│ + shared BW core    │    │ • 5 lifted 1152-elt dets                │
│ Spatial locality →  │    │ • cascaded 5760-elt final sum           │
│ O(1) jump-walk per  │    │ • thread_local static buffers (~170 KB) │
│ insertion           │    │ Closes Shewchuk-debt entry              │
└─────────────────────┘    └─────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────────┐
│ Voronoi extraction layer (consumes delaunay_2d/3d output)          │
├─────────────────────────────┬──────────────────────────────────────┤
│ v8d-2d voronoi_2d           │ v8d-3d voronoi_3d                    │
│ ─────────────────           │ ─────────────────                    │
│ Sort-and-scan tri adjacency │ Sort-and-scan tet adjacency (4T)     │
│ (3T half-edges)             │ + edge fans (6T half-edges)          │
│ → walk around each site     │ → walk each edge's tet fan via face- │
│ via Delaunay neighbour info │ adjacency                            │
│ → bounded cell or rays      │ → DCEL faces + outward CCW vertices  │
│ + ray dirs for unbounded    │ + ConvexHullView helper (Plane per   │
│                             │   face + offsets prefix-sum)         │
└─────────────────────────────┴──────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────────┐
│ Algorithmic layer (consume Voronoi / Delaunay)                     │
├──────────────────┬──────────────────┬──────────────────────────────┤
│ v8e Lloyd CVT    │ v8f Sibson NNI   │ v8g Ruppert refinement       │
│ ──────────────   │ ──────────────   │ ──────────────────────       │
│ Voronoi → centr- │ Bowyer-Watson    │ PSLG → CDT (v6c) → loop:     │
│ oid → repeat     │ cavity / Belikov-│ encroachment scan + bad-     │
│ HullPolicy:      │ Semenov 1997     │ triangle scan + encroach-    │
│ • Fix (default)  │ NniInterpolator2 │ first prioritisation +       │
│ • ClipToBbox     │ caches Delaunay  │ Steiner at circumcentre +    │
│   (2D only;      │ + adjacency +    │ full CDT rebuild per iter    │
│   3D = v8e-3d-   │ circumcentres    │ (v6c constrained_delaunay)   │
│   clip followon) │ for many queries │ Module gains PRIVATE link    │
│ 2D + 3D both     │ + functional one-│ to crd-geometry-polygon      │
│                  │ shot wrapper     │                              │
├──────────────────┴──────────────────┴──────────────────────────────┤
│ v8h tet_refine_3d (3D-Ruppert analog — dihedral-bounded)           │
│ ──────────────────────────────────────────────────────              │
│ Scope honest: NOT true sliver exudation (= v8h-exude followon).    │
│ Six-dihedral enumeration via edge tuples; out-of-domain skip;      │
│ bbox-scaled near-duplicate eps. 3D termination NOT guaranteed —    │
│ NotConverged is valid on adversarial input.                        │
└────────────────────────────────────────────────────────────────────┘
```

## Module dependency graph

```
crd-core
crd-containers ─┐
crd-memory      ├── crd-geometry-primitives
crd-math        │     (Shewchuk predicates + circumcenter_2d/3d + ConvexHullView/Plane)
crd-units       │           │
                │           ▼
                └─── crd-geometry-delaunay  ◄── PRIVATE link
                       │                          to crd-geometry-polygon
                       │                          (v8g Ruppert uses v6c CDT)
                       │
                       ▼
                   (consumers: physics / FEA / scientific viz / proc-gen)
```

The PRIVATE link to `crd-geometry-polygon` for v8g means consumers of
`delaunay/ruppert_2d.hpp` do NOT need to include `polygon/cdt.hpp` — the
dep is impl-only.

## Public API stencil

```cpp
namespace crd::geometry::delaunay
{

// === v8a / v8b — 2D Delaunay ===
template <MathScalar T> struct DelaunayResult2 { Array<u32> triangle_indices; u32 triangle_count; DelaunayStatus status; };
template <MathScalar T> DelaunayResult2<T> delaunay_2d        (ConstSpan<Vec2<T>>, IAllocator*);
template <MathScalar T> DelaunayResult2<T> delaunay_2d_hilbert(ConstSpan<Vec2<T>>, IAllocator*);

// === v8c — 3D Delaunay ===
template <MathScalar T> struct DelaunayResult3 { Array<u32> tet_indices; u32 tet_count; ...; DelaunayStatus3 status; };
template <MathScalar T> DelaunayResult3<T> delaunay_3d(ConstSpan<Vec3<T>>, IAllocator*);

// === v8d — Voronoi ===
template <MathScalar T> struct VoronoiCell    { u32 site_index; Array<u32> vertex_indices; bool is_bounded; Vec2<T> first_ray_dir, last_ray_dir; };
template <MathScalar T> struct VoronoiResult2 { Array<Vec2<T>> voronoi_vertices; Array<VoronoiCell<T>> cells; VoronoiStatus2 status; };
template <MathScalar T> VoronoiResult2<T> voronoi_2d(ConstSpan<Vec2<T>>, IAllocator*);

template <MathScalar T> struct VoronoiFace3 { u32 neighbor_site_index; Array<u32> vertex_indices; bool is_unbounded; };
template <MathScalar T> struct VoronoiCell3 { u32 site_index; Array<VoronoiFace3<T>> faces; bool is_bounded; };
template <MathScalar T> struct VoronoiResult3{ Array<Vec3<T>> voronoi_vertices; Array<VoronoiCell3<T>> cells; ...; };
template <MathScalar T> VoronoiResult3<T> voronoi_3d(ConstSpan<Vec3<T>>, IAllocator*);
template <MathScalar T> VoronoiCellHull3<T> convex_hull_for_cell(const VoronoiResult3<T>&, ConstSpan<Vec3<T>>, u32 cell_index, IAllocator*);

// === v8e — Lloyd CVT ===
enum class HullPolicy2 { Fix, ClipToBbox };
template <MathScalar T> struct LloydOptions2 { u32 max_iterations; T tolerance; HullPolicy2 hull_policy; ...; };
template <MathScalar T> struct LloydResult2  { Array<Vec2<T>> relaxed_sites; bool converged; LloydStatus2 status; ...; };
template <MathScalar T> LloydResult2<T> lloyd_relax_2d(ConstSpan<Vec2<T>>, const LloydOptions2<T>&, IAllocator*);
// 3D mirror: lloyd_relax_3d w/ HullPolicy3 (ClipToBbox returns NotSupported in v1)

// === v8f — Sibson NNI ===
template <MathScalar T> struct NniResult { T value; NniStatus status; };
template <MathScalar T> class NniInterpolator2 { /* build once */ ...; NniResult<T> interpolate(Vec2<T>) const; };
template <MathScalar T> NniResult<T> sibson_interpolate_2d(ConstSpan<Vec2<T>>, ConstSpan<T>, const Vec2<T>&, IAllocator*);

// === v8g — Ruppert refinement ===
struct RuppertSegment { u32 a, b; };
template <MathScalar T> struct RuppertOptions { T min_angle_degrees; u32 max_iterations, max_steiner; };
template <MathScalar T> struct RuppertResult2 { Array<Vec2<T>> vertices; Array<u32> triangle_indices; Array<RuppertSegment> refined_segments; ...; };
template <MathScalar T> RuppertResult2<T> ruppert_refine_2d(ConstSpan<Vec2<T>>, ConstSpan<RuppertSegment>, const RuppertOptions<T>&, IAllocator*);

// === v8h — 3D dihedral-bounded refinement ===
template <MathScalar T> struct TetRefineOptions { T min_dihedral_degrees; u32 max_iterations, max_steiner; };
template <MathScalar T> struct TetRefineResult  { Array<Vec3<T>> vertices; Array<u32> tet_indices; ...; TetRefineStatus status; };
template <MathScalar T> TetRefineResult<T> tet_refine_3d(ConstSpan<Vec3<T>>, const TetRefineOptions<T>&, IAllocator*);
template <MathScalar T> T min_dihedral_of_tet_rad(const Vec3<T>&, const Vec3<T>&, const Vec3<T>&, const Vec3<T>&);

} // namespace crd::geometry::delaunay
```

All explicit-instantiated for `f32` + `f64`. Two-layer typed wrappers in
`*_typed.hpp` headers (ADR-0078 §5 D34) ship at first typed consumer.

## Determinism contract (ADR-0063 + ADR-0076 §4 pin #11)

- **Lex-tuple ordering everywhere**: `(x, y, original_index)` insertion
  in v8a; `(x, y, z, original_index)` in v8c; `(hilbert, original_index)`
  in v8b; sort-and-scan over half-edges/half-faces/half-records (NOT
  HashMap) in v8d/v8f/v8g adjacency rebuilds.
- **Cavity BFS** pops in monotonic tri-/tet-id order.
- **Jump-walk** picks cross-edge in fixed local-index order on ties.
- **Output emission** in slot-id order at strip-time.
- **Byte-identical** across MSVC / clang-cl / GCC / SIMD-widths /
  Windows / Linux given byte-identical input, verified per-slice via
  determinism tests + cluster 18-config full sweep.

## Robustness contract (ADR-0076 §15)

- Builders REJECT non-finite + duplicate inputs (status enums); queries
  TOLERATE non-finite (defensive `is_finite` short-circuit).
- `Coplanar` diagnostic in 3D Delaunay (v8c) when all input coplanar.
- **Shewchuk Stage D adaptive predicates** (orient2d/orient3d/incircle/
  insphere) — `insphere` fully paid down at v8c-pre.
- Defensive **star-shape check** in v8c cavity (orient3d > 0 on
  boundary faces before fan).
- Defensive **OnSite short-circuit** in v8f (exact-coord match short-
  circuits to that vertex's value).
- Defensive **near-duplicate guards** in v8g (eps² = 1e-12 absolute) +
  v8h (bbox-scaled eps² = (bbox_diag × 1e-6)²).
- Defensive **out-of-domain skip** in v8h (skip bad tets whose
  circumcentre falls outside input bbox+10% pad).
- `NotConverged` status for refinement (v8g) + 3D refinement (v8h) +
  Lloyd (v8e) when max iterations exhausted; result still returned
  with best-so-far positions.

## Performance pins

- v8a/v8c: O(N log N) expected for general-position input.
- v8b: same asymptotic but with O(1) jump-walks vs v8a's O(√N) — 2-4×
  speedup for N ≥ 10,000.
- v8c-pre Stage D `insphere_exact`: ~5760-element worst-case expansion
  per call. Rare cold-path fallthrough — Stage A handles 99%+ of input
  paths. Per-thread TLS cost ~170 KB.
- v8d-2d/3d: O(T log T) for sort-and-scan adjacency + O(T) per cell
  walk = O(T log T) total.
- v8e Lloyd: O(N log N × max_iterations); typical 20-50 iterations to
  convergence.
- v8f NNI: O(N log N) build + O(cavity_size) per query (typically a
  handful of triangles).
- v8g Ruppert: O(N² log N) per iter × O(N) iter = O(N³ log N) total in
  v1 (full CDT rebuild per iter); incremental BW = v8g-perf follow-on
  reduces to O(N² log N).
- v8h: similar O(N² log N) per iter × O(N) iter total.

## Two-layer typed architecture (ADR-0078 §5 D34)

All algorithm bodies are raw `<MathScalar T>` (zero-overhead unit
strip). Typed `Vec2<Length32>` / `Vec3<Length32>` wrappers in
`*_typed.hpp` headers planned to ship at first typed consumer (e.g.,
when `crd-fea` Phase 3.1.12 starts consuming `tet_refine_3d`).
Boundary stencils:
```cpp
auto raw_pts = to_raw_vec<Length32>(typed_sites);  // strip units
auto raw_res = delaunay_2d<f32>(raw_pts, alloc);   // inner loop raw
auto typed_res = from_raw_vec<Length32>(raw_res);  // re-tag at API
```

## Integration touch-points (current + planned)

- **`crd-geometry-polygon`** v6c `constrained_delaunay` — consumed by
  v8g Ruppert (PRIVATE link). v6c-consume-v8a follow-on can refactor
  v6c to use v8a internally (currently re-implements BW locally).
- **`crd-geometry-primitives`** Shewchuk predicates + circumcenters —
  consumed by EVERY v8 slice.
- **eylem (physics)** Phase 3.1 v3 XPBD soft-body — future consumer
  via `voronoi_3d` cells for tet-mesh constraints, and Lloyd CVT for
  particle initialisation.
- **crd-fea (planned, Phase 3.1.12)** — primary consumer of v8c, v8g,
  v8h, v8d-3d for FEA mesh generation pipelines.
- **scientific viz (planned Phase 3.5+)** — `voronoi_3d` cells +
  `lloyd_relax_3d` for blue-noise sampling.
- **Worley / cellular noise (`crd-sdf`)** — consumes `voronoi_2d` /
  `voronoi_3d`.

## Open follow-on slices (scope-honest deferrals)

| ID | What | Estimate | Trigger |
|---|---|---|---|
| v8g-perf | Incremental Bowyer-Watson + segment-protected cavity for Ruppert (replaces full-rebuild v1) | ~500-800 LOC | Profiling shows v8g hot in eylem / FEA pipelines |
| v8e-3d-clip | 3D polyhedron-vs-bbox halfspace clipper for Lloyd 3D ClipToBbox | ~200-300 LOC | Consumer needs closed-domain 3D CVT |
| v8h-exude | True sliver exudation (Cheng-Dey-Edelsbrunner-Facello-Teng 2000 weighted-Delaunay perturbation) | ~1500+ LOC | FEA tetmesh quality requires sliver-free output where v8h's dihedral-bound + NotConverged isn't enough |
| v8c-hilbert | 3D Hilbert-sort variant of `delaunay_3d` (mirror of v8b for 2D) | ~150 LOC | Profiling on large 3D point sets |
| v6c-consume-v8a | Refactor v6c CDT to consume v8a internally (currently duplicates BW) | ~200 LOC cleanup | Cleanup-only; v6c output unchanged |
| v8*-typed | `*_typed.hpp` wrappers for every entry on typed-Vec2/Vec3 inputs | ~50 LOC per entry | First typed consumer (likely `crd-fea`) |

## References

**Algorithms**:
- Bowyer 1981 / Watson 1981 — Bowyer-Watson incremental Delaunay.
- Shewchuk 1997 — Adaptive Precision Floating-Point Arithmetic and Fast
  Robust Geometric Predicates. `predicates.c v4.0.0`.
- Hilbert 1891 / Skilling 2004 — Hilbert space-filling curve iterative
  `xy2d` mapping.
- Skinner-Agafonkin 2017 — Mapbox `delaunator` JS library; popularised
  the Hilbert-sort + Bowyer-Watson combination.
- Sibson 1981 — Natural Neighbour Interpolation. *Interpreting
  Multivariate Data*.
- Belikov-Semenov 1997 — Non-Sibsonian interpolation and efficient
  Sibson coordinate computation.
- Lloyd 1982 — Least Squares Quantization in PCM. *IEEE Trans. IT*.
- Ruppert 1995 — A Delaunay Refinement Algorithm for Quality 2-Dimensional
  Mesh Generation. *J. Algorithms*.
- Shewchuk 1996 — Triangle: Engineering a 2D Quality Mesh Generator and
  Delaunay Triangulator. *Applied Computational Geometry*.
- Cheng-Dey-Edelsbrunner-Facello-Teng 2000 — Sliver exudation.
  *J. ACM* (the algorithm v8h-exude follow-on will port).

**Cerid integration**:
- `docs/decisions/0076-geometry-substrate-architecture.md` §23 amendment.
- `docs/decisions/0063-deterministic-fp-contract.md` — determinism
  guarantee inherited.
- `docs/decisions/0078-units-substrate.md` §5 — two-layer typed
  architecture.
- `docs/phases/phase-3.1.7-geometry.md` — v8 slice ledger.
- `docs/debt.md` — Shewchuk adaptive-predicate debt entry (CLOSED at
  v8c-pre).
- `docs/sessions/2026-05-17-geometry-v8*-*.md` — per-slice session logs
  with full decision rationale.
