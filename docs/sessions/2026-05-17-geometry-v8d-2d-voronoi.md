## Session 2026-05-17 — Phase 3.1.7 v8d-2d 2D Voronoi diagram extraction

### Goal

Ship v8d-2d of the `crd-geometry-delaunay` cluster — geometric dual of
v8a/v8b. Each Delaunay triangle's circumcentre is a Voronoi vertex;
perpendicular bisectors between adjacent circumcentres are Voronoi
edges; cells are emitted as ordered vertex sequences with a bounded/
unbounded flag. Foundation for Lloyd CVT (v8e), Sibson Natural Neighbour
Interpolation (v8f), Worley/cellular noise, and biome partitioning.

### What we built / changed

- **New primitive `crd::geometry::primitives::circumcenter_2d`** in
  `engine/geometry-primitives/include/crd/geometry/primitives/circumcenter.hpp`.
  Computes the circumcentre of a 2D triangle, **lifted to f64** then cast
  back to T (D95 — avoids f32 overflow on coord² products for inputs
  above ~10^4). Returns the centroid as a finite fallback on collinear
  input (graceful degradation; caller is expected to guard via
  `orient2d != 0`). Reusable for v8g Ruppert refinement and future tet
  meshers.
- **New public `engine/geometry-delaunay/include/crd/geometry/delaunay/voronoi_2d.hpp`**:
  `VoronoiStatus2` enum (`Ok` / `TooFewPoints` / `NonFiniteInput` /
  `DuplicatePoint` / `InternalInvariant`), `VoronoiCell<T>` (site_index +
  vertex_indices + is_bounded + first/last_ray_dir), `VoronoiResult2<T>`
  (voronoi_vertices + cells + status), `voronoi_2d<T>` entry.
- **New `engine/geometry-delaunay/src/voronoi_2d.cpp`**:
  - **Phase 1**: run `delaunay_2d(sites)`; propagate diagnostic.
  - **Phase 2**: rebuild Delaunay tri adjacency via **sort-and-scan**
    over 3T half-edges keyed `(min(u,v), max(u,v), tri_id, opp_local_idx)`.
    Pairs of consecutive entries with matching endpoints = interior
    edges; singletons = hull edges. O(T log T) deterministic per ADR-0076
    §4 pin #11 (no HashMap).
  - **Phase 3**: compute circumcentre per Delaunay tri.
  - **Phase 4**: site → any incident tri (lowest tri-id, deterministic).
  - **Phase 5**: per site, walk via Delaunay neighbour info — CCW step =
    cross INCOMING edge to site (`nbr[(csi+2)%3]`), CW step = cross
    OUTGOING edge (`nbr[csi]`). For bounded cells, CCW walk forms a
    closed loop. For unbounded cells, walk both CCW and CW from start tri
    (each halts at a hull edge), splice as `reverse(cw_tris) + ccw_tris`,
    compute ray dirs per D96 (perpendicular to bounding hull edge,
    sign-checked via dot against opposite tri vertex to point AWAY from
    cell interior).
- **Umbrella `delaunay.hpp`** re-exports v8d-2d alongside v8a/v8b/v8c.
- **`engine/geometry-delaunay/CMakeLists.txt`** docstring updated.
- **`tests/geometry-delaunay/CMakeLists.txt`** adds `test_voronoi_2d.cpp`.

### Plain-English explanation

A Voronoi diagram partitions space into cells where each cell contains
all points closest to one input site. Mathematically it's the GEOMETRIC
DUAL of the Delaunay triangulation: each Delaunay triangle's CIRCUMCENTRE
is a Voronoi VERTEX, and each Delaunay edge corresponds to a Voronoi edge
(the perpendicular bisector of the Delaunay edge).

The algorithm runs Delaunay first, then for each input site walks around
its incident triangles in CCW order to collect the surrounding
circumcentres — those form the cell's boundary polygon.

The tricky bits:
- **Adjacency rebuild**: Delaunay output is just triangle indices; the
  triangle neighbour info is internal to v8a. We rebuild it externally
  via sort-and-scan (deterministic, no HashMap).
- **Cell walk direction**: "CCW around site s" means rotating CCW around
  the site. In a CCW-oriented triangulation, the next tri CCW around s
  is the one across the INCOMING edge to s (`nbr[(csi+2)%3]`), not the
  outgoing edge — easy to get backwards.
- **Unbounded cells**: sites on the convex hull (or its boundary edges)
  have cells that extend to infinity. We capture both ends of the cell
  by walking both directions from the start triangle.
- **Ray direction**: the ray from a hull triangle's circumcentre is
  perpendicular to the hull edge, pointing AWAY from the third vertex.
  Don't compute as "circumcentre - midpoint" — that's wrong for obtuse
  hull triangles where the circumcentre is outside the triangle.

### Decisions made (D95-D97, pinned for ADR-0076 §23 amendment at v8-close)

- **D95.** **Circumcentre lifted to f64** regardless of T. Formula has
  products of order coord² which overflow f32 above coord magnitude ~10^4.
  Shipped as `crd::geometry::primitives::circumcenter_2d` — reusable by
  v8g Ruppert + future tet meshers. Pattern mirrors v5b LooseOctree's
  f32-surface-f64-internals raycast precompute.
- **D96.** **Unbounded ray direction = perpendicular to Delaunay hull
  edge**, sign-checked via dot against `(opposite_vertex - midpoint)` to
  point AWAY from cell interior. NOT `circumcentre - midpoint` (can be
  near-zero or wrong-side for obtuse hull triangles).
- **D97.** **Output structure**: `voronoi_vertices[t]` = circumcentre of
  Delaunay tri `t` (dual-graph natural). Cells in input-site order
  (`cells[i].site_index == i`). CCW orientation pinned for bounded cells
  (signed_area > 0).

### Files touched

- `engine/geometry-primitives/include/crd/geometry/primitives/circumcenter.hpp` — NEW.
- `engine/geometry-delaunay/include/crd/geometry/delaunay/voronoi_2d.hpp` — NEW.
- `engine/geometry-delaunay/include/crd/geometry/delaunay/delaunay.hpp` — re-exports v8d-2d.
- `engine/geometry-delaunay/src/voronoi_2d.cpp` — NEW.
- `engine/geometry-delaunay/CMakeLists.txt` — docstring updated.
- `tests/geometry-delaunay/CMakeLists.txt` — added test source.
- `tests/geometry-delaunay/test_voronoi_2d.cpp` — NEW (11 cases / 48 assertions).

### Tests / verification

- **11 cases / 48 assertions on v8d-2d suite**:
  - 3 diagnostics propagated from Delaunay (TooFewPoints / NonFiniteInput
    / DuplicatePoint).
  - 3-site triangle (1 Voronoi vertex, 3 unbounded cells).
  - 4-site square (all 4 cells unbounded).
  - 5-site square+center (1 bounded interior cell + 4 unbounded hull
    cells; CCW signed_area > 0 verified on bounded).
  - **16-site exact-integer 4×4 grid** (`[cocircular]` tag): every unit
    square is cocircular; Stage D incircle + lex-tiebreak resolves
    deterministically; 4 inner bounded + 12 perimeter unbounded.
  - **Cospherical-pathology 9-site mixture** (`[cospherical]` tag): 5
    cocircular sites on r²=5e9 + 4 outsiders.
  - Insertion-order determinism (shuffled input).
  - Large-coord f32 stability (1e5 scale).
  - f64 precision tier.
- **Defining-property validator** = brute-force nearest-input-site check.
  For every cell, verify `sites[cell.site_index]` is the closest input
  site to itself (brute-force over all sites). This is the Voronoi
  DEFINITION — appears in every non-trivial test.
- Combined delaunay suite: **45 cases / 662 assertions** all green.
- 4-config DoD via `scripts/per-slice-check.ps1 -Parallel` (backgrounded).

### Mid-slice bug fixes

- **Walk direction inverted**: my first implementation followed `nbr[si]`
  expecting it to step CCW around site s, but in a CCW-oriented
  triangulation `nbr[si]` (across the OUTGOING edge from s) actually
  steps CW. Symptom: bounded cells had negative signed_area (CW vertex
  order). Fixed by swapping: CCW = `nbr[(csi+2)%3]` (incoming edge),
  CW = `nbr[csi]` (outgoing edge). Ray-direction lookup also updated to
  match the corrected walks.
- **16-grid test assumption**: initial test used 4×4 grid with random
  jitter ±0.05, expecting 4 bounded + 12 unbounded. With jitter some
  perimeter sites end up INSIDE the convex hull of the others (their
  cells become bounded), so the count is non-deterministic. Fixed by
  using exact integer grid — Stage D incircle + lex-tiebreak handle the
  cocircular configs deterministically.

### Next session starts with

v8d-3d — 3D Voronoi cells extraction. Geometric dual of v8c's
tetrahedralisation. Each Delaunay tetrahedron's circumcentre is a
Voronoi vertex; each Delaunay face shared by two tets becomes a Voronoi
edge (perpendicular to the face); each Delaunay edge becomes a Voronoi
face. Cells emitted as bounded `ConvexHullView<T>` (via Quickhull v3
over the cell's Voronoi vertices) + unbounded flag for hull-site cells.
