## Session 2026-05-17 — Phase 3.1.7 v8e Lloyd's CVT iteration (2D + 3D)

### Goal

Ship v8e of the `crd-geometry-delaunay` cluster — Lloyd 1982 Centroidal
Voronoi Tessellation (CVT) relaxation. Both 2D and 3D forms in a single
slice per the elite-code mandate. Foundation for stippling, blue-noise
sampling, point-cloud regularisation, particle initialisation (SPH/FEM),
isotropic remeshing alternative.

### What we built / changed

- **New public `lloyd_2d.hpp`** + `lloyd_3d.hpp`:
  - `LloydOptions2<T>` / `LloydOptions3<T>` (max_iterations, tolerance,
    hull_policy, bbox).
  - `LloydResult2<T>` / `LloydResult3<T>` (relaxed_sites, telemetry,
    status).
  - `HullPolicy2 = {Fix, ClipToBbox}` (2D supports both).
  - `HullPolicy3 = {Fix, ClipToBbox}` (3D `ClipToBbox` returns
    `BboxClipNotSupported3D` — reserved for v8e-3d-clip follow-on).
  - `LloydStatus2` / `LloydStatus3` enums.
  - Entry: `lloyd_relax_2d(sites, opts, alloc)` and `lloyd_relax_3d(...)`.
- **New `lloyd_2d.cpp`** implementing:
  - Per-iteration `voronoi_2d` call.
  - **Bounded cell centroid** via signed-area polygon formula.
  - **`HullPolicy2::ClipToBbox`** for unbounded cells: extend rays
    (`first_ray_dir` from v_first, `last_ray_dir` from v_last) to bbox
    boundary via slab test; walk CCW bbox corners between exit and entry
    using `bbox_side_of` classification; run Sutherland-Hodgman clip
    against bbox; centroid the resulting closed polygon.
  - **`HullPolicy2::Fix`**: unbounded cells' sites stay put.
  - Atomic Jacobi-style site swap per iteration.
  - Tolerance halt: stop when `final_max_displacement < tolerance`.
- **New `lloyd_3d.cpp`** implementing:
  - Per-iteration `voronoi_3d` call.
  - **Bounded cell centroid via tet decomposition** (D107): pick `ref =
    cell.faces[0].vertex_indices[0]`; for each face's triangle fan, build
    tets `(ref, face_v[0], face_v[i], face_v[i+1])`; sum `tet_centroid *
    signed_volume`; divide by total signed volume.
  - **`HullPolicy3::Fix`**: hull sites stay put.
  - **`HullPolicy3::ClipToBbox`**: returns `BboxClipNotSupported3D` after
    validating bbox (so `BboxInvalid` still wins on malformed input).
- **Umbrella `delaunay.hpp`** re-exports both Lloyd forms.
- **`engine/geometry-delaunay/CMakeLists.txt`** docstring updated.
- **Tests**: `test_lloyd_2d.cpp` (12 cases / 68 assertions) +
  `test_lloyd_3d.cpp` (10 cases / ~50 assertions). Defining-property
  validators include the 2D Lloyd-energy monotonic-decrease test
  (true polygon centroid → ||site - centroid||² sum strictly non-increasing
  per iteration).

### Plain-English explanation

Lloyd's algorithm transforms a point set into one where every point lies
at the centroid of its Voronoi cell. The iteration:

1. Compute the Voronoi diagram of current sites.
2. Move each site to its cell's centroid.
3. Repeat until movements get small.

It's the canonical "make my point distribution uniform" tool. Each step
*can't increase* the global energy `sum(||site - cell_centroid||²)` —
that's Lloyd's mathematical guarantee (the algorithm is gradient-descent
on a non-convex energy). Convergence: typically 20-50 iterations for
well-distributed input.

**Hull policy**: sites on the convex hull have Voronoi cells extending
to infinity — their "centroid" is undefined. Two options:

- **Fix** (default): hull sites don't move. Use when you supply a boundary
  point set you want preserved and only want to relax the interior.
- **ClipToBbox**: clip unbounded cells against a bounding box before
  centroid. Use when you want all sites uniformly relaxed inside a closed
  domain. Bbox auto-derived from input + 10% pad if not specified.

**2D ClipToBbox closure** (D106): for an unbounded cell, the boundary
has rays going from `v_first` and `v_last` to infinity. We:
1. Compute where the (reverse) first ray and the last ray intersect bbox.
2. Walk CCW around bbox corners between those two exit points.
3. Pass the resulting closed polygon through Sutherland-Hodgman just in
   case any cell vertex sits just outside bbox.

**3D ClipToBbox** would need polyhedron-vs-bbox halfspace clipping
(~200 LOC). Reserved for v8e-3d-clip follow-on; the `Fix` mode is the
production-ready v8e API in 3D.

**3D bounded-cell centroid via tet decomposition** (D107): a convex
polyhedron's volumetric centroid = `sum(C_i * V_i) / sum(V_i)` where the
sum is over tets decomposing the polyhedron and `C_i / V_i` are the tet
centroids and signed volumes. We decompose by picking a reference vertex,
then for each face's triangle fan, build one tet per triangle. The
algebraic identity is ref-invariant for closed polyhedra.

### Decisions made (D102-D108, pinned for ADR-0076 §23 at v8-close)

- **D102.** `HullPolicy::Fix` is the DEFAULT — simplest, robust.
  `ClipToBbox` is opt-in for true closed-domain CVT.
- **D103.** Convergence = max per-iteration site displacement < tolerance.
  Absolute (input coord units), not relative. Caller scales tolerance to
  expected coord magnitude.
- **D104.** 2D polygon centroid via standard signed-area formula. Reuses
  the same math as `crd::geometry::polygon::centroid`.
- **D105.** 2D ClipToBbox via Sutherland-Hodgman against axis-aligned bbox.
  Four edges in CCW order (bottom, right, top, left).
- **D106.** 2D unbounded-cell closure: ray extension + CCW bbox-corner
  walk + Sutherland-Hodgman.
- **D107.** 3D bounded-cell centroid via tet decomposition. Ref-invariant
  sum.
- **D108.** 3D ClipToBbox deferred to v8e-3d-clip follow-on. Returns
  `BboxClipNotSupported3D` if requested; documented in `lloyd_3d.hpp`.
  Users requiring closed-domain 3D CVT can pad with sentinel hull sites.

### Files touched

- `engine/geometry-delaunay/include/crd/geometry/delaunay/lloyd_2d.hpp` — NEW.
- `engine/geometry-delaunay/include/crd/geometry/delaunay/lloyd_3d.hpp` — NEW.
- `engine/geometry-delaunay/include/crd/geometry/delaunay/delaunay.hpp` — re-exports both.
- `engine/geometry-delaunay/src/lloyd_2d.cpp` — NEW.
- `engine/geometry-delaunay/src/lloyd_3d.cpp` — NEW.
- `engine/geometry-delaunay/CMakeLists.txt` — docstring updated.
- `tests/geometry-delaunay/CMakeLists.txt` — added test sources.
- `tests/geometry-delaunay/test_lloyd_2d.cpp` — NEW (12 cases / 68 assertions).
- `tests/geometry-delaunay/test_lloyd_3d.cpp` — NEW (10 cases / ~50 assertions).

### Tests / verification

- **22 Lloyd cases total / ~118 assertions.**
- 2D suite:
  - 4 diagnostics + already-relaxed-converges-in-≤1-iter + off-centre-
    relaxes-to-symmetric-centre + Lloyd-energy-monotonic-decrease
    (TRUE polygon centroid — hallmark CVT property) + ClipToBbox keeps
    sites in bbox + NotConverged-when-max-iters-too-small + determinism +
    large-coord f32 stability + f64.
- 3D suite:
  - 4 diagnostics (incl. `BboxClipNotSupported3D` for ClipToBbox) +
    already-centred + off-centre-relaxes + 10-site-symmetric-converges +
    NotConverged + f64.
- Combined delaunay suite: **80 cases / 888 assertions** all green.
- **Mid-slice fixes**:
  - Missing `<crd/containers/sort.hpp>` include in `lloyd_2d.cpp`.
  - Large-coord f32 test had tolerance too loose (1e-2·kScale ≈ 10) for
    the 1-unit-accuracy check; tightened to 1e-4·kScale + 60 iters + 0.5
    threshold so Lloyd actually converges to true centroid.
  - 3D Lloyd-energy proxy test: vertex-average ≠ true volume-weighted
    centroid, so Lloyd (which moves sites toward TRUE centroid) actually
    INCREASES the proxy energy. Replaced with symmetric-sum convergence
    test on a 2-interior-site config (sites must settle symmetrically
    about the cube centre).
- 4-config DoD via `scripts/per-slice-check.ps1 -Parallel`.

### Next session starts with

v8f — Sibson 1981 Natural Neighbour Interpolation. Scattered-data
interpolation via "stolen Voronoi area" weighting. Crushes inverse-
distance-weighted for terrain reconstruction, scientific-field
interpolation. C¹ continuous. Builds on v8d-2d Voronoi infrastructure.

Algorithm: given a query point `q` outside the input site set, compute
its hypothetical Voronoi cell (the area `q` would steal from each
neighbour site). The fraction of cell `i`'s original area that `q` would
steal is the Sibson weight `w_i`. Interpolated value = `sum(w_i * f_i)`
where `f_i` is the value at site `i`. Tricky bit: efficient computation
of "stolen area" without rebuilding Voronoi from scratch each query.
