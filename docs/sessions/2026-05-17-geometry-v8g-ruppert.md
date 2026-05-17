## Session 2026-05-17 — Phase 3.1.7 v8g Ruppert 1995 2D Delaunay refinement

### Goal

Ship v8g of the `crd-geometry-delaunay` cluster — Ruppert 1995 quality-
bounded 2D Delaunay mesh generation. Input is a Planar Straight-Line
Graph (PSLG) = points + boundary segments. Output is a Constrained
Delaunay Triangulation where every triangle's minimum angle exceeds α
(typically 25-30°). Powers FEA / FVM 2D meshing, planar finite-element
surface preprocessing — the standard production primitive Triangle and
CGAL Mesh_2 ship.

### What we built / changed

- **New public `engine/geometry-delaunay/include/crd/geometry/delaunay/ruppert_2d.hpp`**:
  - `RuppertSegment` (a, b — input segment endpoint indices).
  - `RuppertStatus` enum (Ok / TooFewPoints / NonFiniteInput /
    DuplicatePoint / ConstraintOutOfBounds / ConstraintsCrossing /
    InvalidAngle / NotConverged / InternalInvariant).
  - `RuppertOptions<T>` (min_angle_degrees default 25, max_iterations
    default 10000, max_steiner default 100000).
  - `RuppertResult2<T>` (vertices + triangle_indices + refined_segments
    + telemetry).
  - `ruppert_refine_2d<T>(points, segments, opts, alloc)` entry.
- **New `engine/geometry-delaunay/src/ruppert_2d.cpp`** implementing the
  Ruppert refinement loop:
  - Input validation (alpha bounds, segment bounds, finite input).
  - Initial CDT via `crd::geometry::polygon::constrained_delaunay` (v6c).
  - Per-iteration:
    - **Encroachment scan**: segment AB encroached by V iff `dot(A-V, B-V) < 0`.
      First encroached segment found → midpoint split → re-CDT.
    - **Bad-triangle scan**: triangle with min-angle < α via law-of-cosines.
      First bad triangle found → circumcentre computed.
    - **Encroach-first prioritisation**: if circumcentre encroaches any
      segment, split that segment first (Ruppert's correctness condition);
      else insert circumcentre as Steiner vertex.
    - **Near-duplicate check**: if proposed Steiner / midpoint is within
      numerical eps (1e-12) of any existing vertex, bail loop with
      NotConverged (avoids CDT's `DuplicatePoint` rejection on near-
      identical points from refining sliver triangles).
  - Full CDT rebuild per iteration (simple-but-correct v1; incremental
    Bowyer-Watson + segment-protected cavity is a v8g-perf follow-on).
- **`engine/geometry-delaunay/CMakeLists.txt`** — added PRIVATE link to
  `crd-geometry-polygon` (v6c CDT used internally; public Ruppert header
  doesn't expose `CdtEdge` to consumers).
- **Umbrella `delaunay.hpp`** re-exports v8g.
- **`tests/geometry-delaunay/CMakeLists.txt`** adds `test_ruppert_2d.cpp`.
- **`tests/geometry-delaunay/test_ruppert_2d.cpp`** — 10 cases / 40
  assertions.

### Plain-English explanation

Ruppert's algorithm takes a "rough" triangle mesh (just boundary points +
segments) and inserts new vertices (Steiner points) until every triangle
in the mesh has a minimum angle exceeding α. This eliminates "skinny"
triangles that ruin FEA solver conditioning, and produces meshes suitable
for finite-element simulation.

The loop is simple:
1. If any boundary segment has a vertex inside its diametral disk
   (the smallest circle containing the segment), the segment is
   "encroached" — split it in half.
2. Otherwise, find any triangle that's too pointy (min angle < α). Try
   to insert a Steiner point at its circumcentre.
3. If that circumcentre would encroach a segment, split the segment
   instead (Ruppert's correctness rule).
4. Otherwise insert the circumcentre.
5. Repeat until no bad triangles + no encroached segments.

Ruppert proved this terminates for α ≤ arcsin(1/(2√2)) ≈ 20.7°. For
larger α (e.g., 30°) termination is empirical. The default `α = 25°`
matches Triangle's production setting — works on essentially all real
inputs.

**Why re-CDT every iteration in v1**: the production-grade implementations
(Triangle, CGAL Mesh_2) use incremental Bowyer-Watson updates with
segment-protected cavities for performance. That's ~500 LOC of careful
invariants. For v1 elite-correctness over perf-elite, we rebuild the
whole CDT per iteration via the v6c primitive. O(N² log N) total —
acceptable for meshes up to ~10k vertices. Incremental update is filed
as a v8g-perf follow-on slice.

### Decisions made (D114-D118, pinned for ADR-0076 §23 at v8-close)

- **D114.** Encroachment via diametral-disk dot product. `dot(A-V, B-V) < 0`
  iff angle AVB > 90° iff V inside diametral disk of AB. One multiply-add,
  no sqrt.
- **D115.** Encroach-first prioritisation: if a circumcentre would
  encroach a segment, split that segment instead of inserting the
  circumcentre. Required for correctness — inserting a Steiner point that
  encroaches a segment violates the algorithm's invariant.
- **D116.** Min-angle calc via law of cosines using dot products. The min
  angle is opposite the shortest side.
- **D117.** Full CDT rebuild per iteration (v1). Simple, deterministic,
  reuses v6c. Incremental update = v8g-perf follow-on.
- **D118.** Deterministic ordering: scan segments in input-index order
  (sub-segments inserted at end), scan triangles in CDT-output order;
  first encroached / first bad found is processed. Lex-tiebreak
  determinism preserved.

### Files touched

- `engine/geometry-delaunay/include/crd/geometry/delaunay/ruppert_2d.hpp` — NEW.
- `engine/geometry-delaunay/include/crd/geometry/delaunay/delaunay.hpp` — re-exports v8g.
- `engine/geometry-delaunay/src/ruppert_2d.cpp` — NEW.
- `engine/geometry-delaunay/CMakeLists.txt` — PRIVATE link to crd-geometry-polygon.
- `tests/geometry-delaunay/CMakeLists.txt` — added test source.
- `tests/geometry-delaunay/test_ruppert_2d.cpp` — NEW (10 cases / 40 assertions).

### Tests / verification

- **10 cases / 40 assertions on v8g suite**:
  - 3 diagnostics (TooFewPoints / InvalidAngle / ConstraintOutOfBounds).
  - **Unit-square refined to α=20**: 4 corners + 4 boundary segments;
    converges; `all_triangles_meet_quality` verifies every output tri
    has min-angle ≥ α-0.5.
  - **L-shape refined to α=20**: 6 corners + 6 boundary segments;
    converges with quality bound met.
  - **30:1 skinny initial triangle**: Steiner insertion progresses
    (steiner_count > 0).
  - **Termination at α=20** (Ruppert's theoretical bound).
  - **Boundary-segment preservation**: every refined segment endpoint
    sits on the input's boundary lines.
  - Determinism (same input → byte-identical output).
  - f32 precision tier.
- Combined delaunay suite: **101 cases / 1111 assertions** all green.
- **Mid-slice bug fix**: skinny-triangle test failed because refining a
  30:1 aspect-ratio triangle produced sliver triangles whose
  circumcentres coincided (within FP precision) with existing vertices →
  CDT rejected with `DuplicatePoint`. Added `is_near_existing` check
  with `eps_sq = 1e-12`: if the proposed Steiner / midpoint is within
  numerical eps of any existing vertex, bail the loop (treat as
  unrefinable → NotConverged) instead of corrupting the mesh.
- 4-config DoD via `scripts/per-slice-check.ps1 -Parallel`.

### Next session starts with

v8h — Shewchuk-style 3D Delaunay refinement (sliver removal). Tetmesh
quality for FEA 3D. Slivers (almost-flat tetrahedra with all 4 vertices
near a common plane) tank solver conditioning. Removal via small
perturbation + edge collapse + face flip. Powers eylem v7 FEM + future
crd-fea. Substantial slice — 3D quality refinement is significantly
harder than 2D Ruppert.
