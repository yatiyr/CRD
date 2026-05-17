## Session 2026-05-17 — Phase 3.1.7 v7g self-intersection removal

### Goal

Ship `crd-geometry-mesh-processing` v7g: detect triangle-triangle
intersections in 3D and "cut" them — retriangulate the affected
triangles so that the intersection segment becomes an EDGE of the
mesh, eliminating the self-intersection while preserving the surface
topology. Downstream consumers (procedural booleans, photogrammetry
cleanup, animated-deformation post-pass) all require self-intersection-
free input.

### What we built / changed

- **New `remove_self_intersections` entry point** in
  `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/remove_self_intersections.hpp`
  + `.cpp`:
  - `RemoveSelfIntersectionsOptions<T>` — `dedup_epsilon` (per-triangle
    CDT input dedup, default 1e-6), `output_allocator`.
  - `RemoveSelfIntersectionsStatus` — `Ok`, `EmptyMesh`,
    `NoSelfIntersections`.
  - `RemoveSelfIntersectionsReport` — `status`,
    `candidate_pairs_tested`, `intersection_pairs_detected`,
    `intersection_vertices_added`, `triangles_retriangulated`,
    `triangles_skipped_cdt_failure`, `output_vertices`, `output_faces`.
  - `HalfEdgeMesh<T> remove_self_intersections(const HalfEdgeMesh<T>&,
    opts, &report)` — input untouched; returns a fresh
    self-intersection-free copy.
- **Module dep extended**: `crd-geometry-mesh-processing` now
  PUBLIC-links `crd-geometry-polygon` (for `constrained_delaunay`,
  the v6c CDT primitive that retriangulates each cut triangle).
- **Umbrella header** `mesh_processing.hpp` extended.
- **5 Catch2 tests** in `tests/geometry-mesh-processing/test_remove_self_intersections.cpp`.

### Plain-English explanation

When two triangles in a mesh physically intersect (one stabs through
another in 3D), the mesh is "self-intersecting" — a topological lie
that breaks downstream geometric algorithms. v7g finds these
intersections and cuts the offending triangles along the intersection
line so the cut becomes a proper edge of the mesh.

The algorithm has three phases. **Broadphase**: pair up triangles
whose AABBs overlap (and that don't already share a vertex —
adjacent triangles "touch" at their shared edge, which isn't a
self-intersection). **Narrowphase**: for each candidate pair, run
the Möller 1997 triangle-triangle intersection test, gated by exact-
sign Shewchuk `orient3d` predicates so we never compute spurious
intersections from floating-point roundoff at touching vertices.
**Per-triangle retriangulation**: each cut triangle accumulates a
list of intersection segments; we project the triangle to 2D (by
dropping its plane's largest-magnitude normal component), feed the
triangle's vertices + segment endpoints as input + the triangle's
boundary + segments as constraints into v6c's `constrained_delaunay`,
then lift the resulting sub-triangulation back to 3D using the
triangle's plane equation.

Cross-triangle vertex stitching is built in: when triangles T₁ and T₂
share an intersection segment, both endpoints get appended to the
global vertex pool ONCE and shared between the two triangles' CDT
inputs, so the output stays 2-manifold across the cut.

### Decisions made (pinned for ADR-0076 §22 amendment at v7-close)

- **D60.** Brute-force O(n²) broadphase for v7g. The phase-doc spec
  mentions BVH-accelerated broadphase but the static `BvhTree` from
  `TriangleMeshBvh` doesn't expose `find_overlapping_pairs` (only
  `DynamicBvh` does). For the v7g test corpus (small meshes) the
  brute-force form is comfortable; for large meshes the v7g-followon
  optimization will substitute `DynamicBvh::find_overlapping_pairs`.
- **D61.** Möller 1997 with `orient3d` gate. Each candidate pair's
  early-exit "all on one side of the other's plane" test uses
  `crd::geometry::primitives::orient3d` (Shewchuk adaptive — exact
  sign, FP-roundoff-immune). Touch-only cases (≥ 1 sign zero + the
  rest same sign, or any two zeros) explicitly skipped — a vertex
  or edge of T₂ lying ON T₁'s plane is not a transversal cut. This
  was the bug fix on the cube test corpus where axis-aligned faces
  produce many zero-sign orient3d cases.
- **D62.** Cross-triangle vertex stitching via per-pair shared global
  indices. Each intersection event creates 2 endpoint vertices ONCE
  and stores the same indices in BOTH T_i and T_j's segment lists →
  the two independent CDT calls reference the same global vertices
  → output is 2-manifold along the cut.
- **D63.** Per-triangle 2D projection via drop-largest-normal-axis +
  flip-winding-if-normal-negative. Maximally orthogonal projection
  minimises 2D coordinate distortion; CCW preservation matches CDT's
  input expectation.
- **D64.** Epsilon-dedup of per-triangle CDT inputs (default 1e-6).
  Handles the "3+ triangles meet at a common segment" case where
  pair endpoints from different intersection events land on nearly
  coincident 2D points.
- **D65.** Graceful degradation on CDT failure (`ConstraintsCrossing`
  / non-Ok status): keep T's original tessellation and increment
  `triangles_skipped_cdt_failure`. Better than emitting garbage.
- **D66.** Centroid-inside-T sanity filter on CDT output. CDT's
  super-triangle artifacts can produce triangles outside T's
  convex hull; we filter by checking each sub-triangle's centroid
  is inside T (barycentric in/out test).

### Files touched

- `engine/geometry-mesh-processing/CMakeLists.txt` — added `crd-geometry-polygon` PUBLIC dep.
- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/remove_self_intersections.hpp` — NEW.
- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/mesh_processing.hpp` — added include.
- `engine/geometry-mesh-processing/src/remove_self_intersections.cpp` — NEW (impl + `f32`/`f64` instantiations).
- `tests/geometry-mesh-processing/test_remove_self_intersections.cpp` — NEW (5 cases).
- `tests/geometry-mesh-processing/CMakeLists.txt` — added new test source.

### Tests / verification

- **5 Catch2 cases**; mesh-processing suite total = **72 cases / 1087
  assertions** (v7a 14 + v7b 15 + v7c 9 + v7d 9 + v7e 11 + v7f 9 +
  v7g 5).
- Coverage:
  - `EmptyMesh` diagnostic.
  - `NoSelfIntersections` on a closed cube (touch-only orient3d=0
    cases properly rejected).
  - Two crossing triangles (XY-plane triangle + YZ-plane triangle):
    1 intersection pair, 2 endpoints added, both triangles
    retriangulated.
  - Determinism: same input → byte-identical counts.
  - f64 precision tier (cube).
- **4-config DoD via `scripts/per-slice-check.ps1` — PASS on retry**
  (elapsed 03:16): win-debug + win-asan + win-shipping + win-tidy.
  - First attempt failed at win-tidy with transient clang-tidy
    build error on `test_qem_decimate.cpp` — upstream LLVM bug per
    `feedback_transient_clang_tidy_crash.md` (same class as MSVC
    LTCG ICE policy). Retry clean.

### Bugs found + fixed mid-slice

Initial cube test reported `intersection_pairs_detected == 1` instead
of 0. Root-caused: the cube's axis-aligned faces produce many
orient3d == 0 cases (vertex on perpendicular triangle's plane). My
naive sign-comparison in `compute_interval` treated zero distances
inconsistently — for `dB == 0, dC != 0`, the condition
`(dB > 0) == (dC > 0)` returned `(false == false) == true` (treating
zero as "same side"), then computed crossings on edges incident to A
where the denominator `dA - dB == 0` was forced to t=0 — emitting
the wrong segment endpoint.

Fix: in the orient3d gate, explicitly reject touch-only cases:
  - ≥ 1 sign zero + remaining signs same and non-zero → vertex-on-plane
    touch (single point, not transversal cut).
  - ≥ 2 sign zeros → edge-on-plane touch (coplanar edge case).
Both defer to v7f manifoldness repair. After this fix the cube test
correctly reports zero intersections, and only genuine transversal
crossings produce segments.

### Next session starts with

v7h Taubin smoothing (Taubin 1995) — volume-preserving Laplacian
smoothing via the two-pass `λ`-shrink + `μ`-anti-shrink scheme
(`μ ≈ -1.04 · λ`). Removes high-frequency noise while preserving
low-frequency shape (no Gaussian "candy melt" drift). First step:
`engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/taubin_smooth.hpp`.
