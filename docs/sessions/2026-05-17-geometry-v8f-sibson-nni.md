## Session 2026-05-17 — Phase 3.1.7 v8f Sibson Natural Neighbour Interpolation 2D

### Goal

Ship v8f of the `crd-geometry-delaunay` cluster — Sibson 1981 NNI in 2D.
Scattered-data interpolation via "stolen Voronoi area" weighting. C¹
continuous, reproduces linear functions exactly, bounded result. Crushes
inverse-distance-weighted for terrain reconstruction, scientific-field
interpolation, photogrammetry resampling.

### What we built / changed

- **New `engine/geometry-delaunay/include/crd/geometry/delaunay/nni_2d.hpp`**:
  - `NniStatus` enum (Ok / TooFewPoints / NonFiniteInput / DuplicatePoint
    / QueryNonFinite / OutsideHull / OnSite / NotInitialized /
    InternalInvariant).
  - `NniResult<T>` (value + status; `ok()` returns true for Ok + OnSite).
  - `NniInterpolator2<T>` class: build-once construction caches Delaunay +
    triangle adjacency + per-triangle circumcentres; supports many
    `interpolate(query)` queries with O(cavity_size) cost each.
  - `sibson_interpolate_2d(sites, values, query, alloc)` one-shot
    functional form (builds and discards interpolator).
- **New `engine/geometry-delaunay/src/nni_2d.cpp`** implementing the
  Bowyer-Watson cavity / Belikov-Semenov 1997 algorithm:
  - Sort-and-scan triangle adjacency rebuild (mirror of v8d-2d).
  - Jump-walk locate (`orient2d` per face, lowest-index tiebreak).
  - OnSite detection: exact-coordinate equality with any vertex of the
    containing triangle → return that vertex's value.
  - Cavity BFS via Shewchuk Stage D `incircle` (`> 0` = bad tri joins
    cavity).
  - Cavity boundary edge collection — each cavity tri's edge with
    non-cavity opposite (including null = hull edge) is a boundary edge
    in CCW direction (cavity on the LEFT when walking edge u→v).
  - Boundary cycle walk via `next_v[u] = v` map → CCW-ordered natural
    neighbours.
  - Per-neighbour stolen-area computation: walk cavity tris incident to
    n_i from edge (n_{i-1}, n_i) to edge (n_i, n_{i+1}) via face
    adjacency. Stolen polygon = [circumcenter(q, n_{i-1}, n_i),
    cavity-tri circumcenters in walk order, circumcenter(q, n_i,
    n_{i+1})]. Area via signed-area formula (`|sum|`).
  - Weighted sum: w_i = stolen_i / total_stolen; value = sum(w_i ×
    value[n_i]).
- **Umbrella `delaunay.hpp`** re-exports v8f alongside earlier slices.
- **`engine/geometry-delaunay/CMakeLists.txt`** docstring updated.
- **`tests/geometry-delaunay/CMakeLists.txt`** adds `test_nni_2d.cpp`.
- **`tests/geometry-delaunay/test_nni_2d.cpp`** — 11 cases / 183
  assertions.

### Plain-English explanation

Sibson NNI answers: given a few measurements scattered around a surface
(e.g., elevation samples), what's the best-guess value at a new query
point? The trick: hypothetically insert the query into the Voronoi
diagram of the input sites. The query's new cell would "steal" some area
from each neighbouring site's old cell. The amount each site loses is its
INFLUENCE on the query: closer/more-adjacent sites lose more area, so
their values weigh more heavily.

The implementation never actually rebuilds the Voronoi diagram per query.
Instead it identifies the "Bowyer-Watson cavity" — the set of Delaunay
triangles whose circumcircles contain the query point. The cavity
boundary vertices ARE the natural neighbours. The stolen area for each
neighbour can be computed by tracing the polygon that the neighbour's
new cell would lose: bounded by the old Voronoi vertices (cached
triangle circumcentres) and two new Voronoi vertices (circumcentres of
new triangles formed with the query).

**Sibson's hallmark property**: NNI reproduces linear functions exactly.
If your input values follow f(x, y) = ax + by + c, then NNI at any
interior query returns f(q.x, q.y) to floating-point precision. This is
the standard quality benchmark for scattered-data interpolators (IDW
fails it spectacularly — IDW's value at the midpoint between two equal-
valued sites is NOT equal to that value).

### Decisions made (D109-D113, pinned for ADR-0076 §23 at v8-close)

- **D109.** Triangle adjacency rebuild via sort-and-scan over 3T
  half-edges (mirror of v8d-2d's pattern — no HashMap, deterministic).
- **D110.** Cavity BFS via Stage D `incircle` (`> 0` triggers expansion).
  v8a's adaptive paydown means cocircular configurations are handled
  exactly with lex-tiebreak determinism.
- **D111.** Cavity boundary CCW walk via `next_v[u] = v` map. Each
  boundary edge u→v has cavity on the LEFT (matches CCW triangle
  convention). For q strictly inside the convex hull, the boundary
  forms a single closed cycle.
- **D112.** Stolen polygon vertex order: [new_left, old C_0, old C_1,
  ..., old C_k, new_right] where new_left = circumcenter(q, n_{i-1},
  n_i), new_right = circumcenter(q, n_i, n_{i+1}), and the old C_j are
  cavity-tri circumcentres walked from the (n_{i-1}, n_i) side to the
  (n_i, n_{i+1}) side via face adjacency. Area via standard signed-area
  formula with `|sum|`.
- **D113.** OnSite detection short-circuit: if the located containing
  triangle has a vertex whose coordinates match the query exactly,
  return that vertex's value with status `OnSite`. Distinguishes
  interpolating AT a site (exact answer) from interpolating NEAR a site
  (Sibson math).

### Files touched

- `engine/geometry-delaunay/include/crd/geometry/delaunay/nni_2d.hpp` — NEW.
- `engine/geometry-delaunay/include/crd/geometry/delaunay/delaunay.hpp` — re-exports v8f.
- `engine/geometry-delaunay/src/nni_2d.cpp` — NEW.
- `engine/geometry-delaunay/CMakeLists.txt` — docstring updated.
- `tests/geometry-delaunay/CMakeLists.txt` — added test source.
- `tests/geometry-delaunay/test_nni_2d.cpp` — NEW (11 cases / 183 assertions).

### Tests / verification

- **11 cases / 183 assertions on v8f suite**:
  - 3 diagnostic statuses (TooFewPoints / QueryNonFinite / OutsideHull).
  - OnSite-returns-exact-value (query equals a site → exact value).
  - **Linear-function reproduction** `[linear-reproduction]` —
    Sibson's hallmark; f(x, y) = 2.5x - 1.7y + 5 reconstructed at 5
    interior queries, error < 1e-8.
  - **Convex-hull boundedness** `[bounded]` — NNI result in [min_value,
    max_value] across 49 probe queries on a 9-site config.
  - Continuity near a site (query at site + 1e-4 offset → value within
    0.1 of site's value).
  - Functional form matches class form (byte-identical result).
  - Many-query reuse on cached interpolator.
  - Determinism (3 identical queries → 3 identical values).
  - f32 + f64 precision tiers.
- Combined delaunay suite **91 cases / 1071 assertions** all green.
- **Mid-slice bug**: cavity-touches-hull early bail incorrectly returned
  `OutsideHull` for INTERIOR queries whose cavity included hull-adjacent
  triangles. The cavity boundary still forms a closed CCW cycle for
  interior q even when hull edges are in the cavity — hull edges
  contribute boundary segments naturally. Removed the bail-out; cycle
  walk handles hull-edge boundary segments correctly.
- 4-config DoD via `scripts/per-slice-check.ps1 -Parallel`.

### Next session starts with

v8g — Ruppert 1995 2D Delaunay refinement. Quality bound on minimum
triangle angle for FEA 2D meshing. Takes initial Delaunay (or v6c CDT)
and inserts Steiner points at "skinny" triangles' circumcentres until
every triangle's minimum angle exceeds α (typical 25-30°). Theoretical
termination for α ≤ 20.7°. The standard production primitive for FEA 2D
meshing — Triangle (Shewchuk's CGAL-free tool) ships exactly this
algorithm.
