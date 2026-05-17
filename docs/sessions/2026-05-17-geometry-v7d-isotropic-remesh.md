## Session 2026-05-17 — Phase 3.1.7 v7d isotropic remeshing (Botsch-Kobbelt 2004)

### Goal

Ship `crd-geometry-mesh-processing` v7d: Botsch-Kobbelt 2004 §4 isotropic
remeshing. Turn an arbitrary triangle mesh into one of approximately
uniform triangle size, with interior valences ≈ 6 and boundary
valences ≈ 4 — the canonical "well-conditioned mesh" property that FEA
solvers, GPU shading-rate sampling, and vertex-shader pipelines want.

### What we built / changed

- **New `isotropic_remesh` entry point** in
  `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/isotropic_remesh.hpp`
  + `.cpp`:
  - `IsotropicRemeshOptions<T>` — `target_edge_length`, `n_iterations`,
    `split_factor` (default 4/3), `collapse_factor` (default 4/5),
    `smoothing_lambda`, `keep_boundary_fixed`, `project_to_input`,
    `output_allocator`.
  - `IsotropicRemeshStatus` — `Ok`, `EmptyMesh`, `NonManifoldInput`,
    `InvalidTargetLength`.
  - `IsotropicRemeshReport` — `status`, `iterations_run`,
    `splits_applied`, `collapses_applied`, `flips_applied`,
    `vertices_smoothed`, `output_vertices`, `output_faces`.
  - `HalfEdgeMesh<T> isotropic_remesh(const HalfEdgeMesh<T>&, opts,
    &report)` — input untouched; returns a fresh remeshed copy.
- **`HalfEdgeMesh::set_vertex_position` added** — Jacobi-style smoothing
  needs to update positions in-place without going through
  `to_indexed → build_from` per iteration. Topology unchanged; slot id
  stays stable.
- **Module dep extension**: `crd-geometry-mesh-processing` now links
  PUBLIC against `crd-geometry-bvh` + `crd-geometry-mesh` (for
  `TriangleMeshBvh` + `mesh_closest_point` used by surface projection
  in the smoothing pass).
- **Umbrella header** `mesh_processing.hpp` extended.
- **9 Catch2 tests** in `tests/geometry-mesh-processing/test_isotropic_remesh.cpp`.

### Plain-English explanation

Isotropic remeshing turns an irregular triangle mesh into a uniform one.
It iterates four operations: (1) **split** any edge that's too long;
(2) **collapse** any edge that's too short (gated by manifold-preserving
link condition + a Botsch-Kobbelt safety check that no resulting edge
would immediately violate the long-edge threshold — prevents oscillation);
(3) **flip** any edge whose flip reduces the total deviation of incident
vertex valences from their targets (6 for interior, 4 for boundary);
(4) **smooth** each vertex by moving it tangentially across the surface
toward the centroid of its neighbours, then projecting back onto the
ORIGINAL input mesh's surface via a closest-point BVH query (so the
shape is preserved while the tessellation pattern uniformises).

The trick that makes Botsch-Kobbelt work — and the reason we had to
implement it carefully — is **tangential** smoothing, not arbitrary
Laplacian smoothing. The displacement from current vertex to centroid
gets decomposed into a tangential component (along the surface) and a
normal component (off the surface). Only the tangential part is applied;
the normal part would push the vertex away from the original surface
even after BVH projection, accumulating drift over iterations. Combined
with an **inversion-rejection safeguard** (Hoppe-style: if moving the
vertex would flip any incident face's orientation, keep it where it is),
this keeps the mesh manifold across arbitrarily many iterations.

### Decisions made (pinned for ADR-0076 §22 amendment at v7-close)

- **D26.** Module dep extension. v7d adds PUBLIC deps on
  `crd-geometry-mesh` (`TriangleMeshView` + `TriangleMeshBvh` +
  `mesh_closest_point`) and transitively on `crd-geometry-bvh`. The
  mesh-processing module now has two distinct "geometry backends":
  -primitives (v7a-v7c) + -mesh+-bvh (v7d+).
- **D27.** Input untouched + clone-via-indexed. Same pattern as v7b/v7c.
- **D28.** Operate IN-PLACE on output during iterations. v7d adds
  `set_vertex_position` to HalfEdgeMesh for Jacobi smoothing.
- **D29.** Per-pass canonical-HE snapshot. Each of the 4 passes iterates
  a snapshot taken before mutation. New HEs created within the pass are
  not re-processed — required for convergence (a freshly-split edge is
  already at target length).
- **D30.** Collapse safety pre-check: reject if any post-collapse edge
  to the merged vertex's neighbours would exceed `(4/3)·L` (split
  threshold). Without this, the next pass immediately re-splits.
- **D31.** Flip target valences: 6 for interior (hexagonal-lattice
  optimum), 4 for boundary (Botsch-Kobbelt §4).
- **D32.** Smoothing = Jacobi. New positions for all alive vertices
  computed against OLD; applied atomically.
- **D33.** Input-mesh BVH is f32 (crd-geometry-mesh constraint). For
  T=f64, positions cast to f32 at the BVH query boundary — precision
  loss bounded by f32 ulp × surface scale (negligible vs smoothing
  offset).
- **D34.** Boundary fixed by default (`keep_boundary_fixed = true`).
  Cubic-B-spline boundary mask from v7c could be added later.
- **D35.** Boundary edges skipped in split/collapse/flip passes (v7a's
  atomic ops reject boundary, and flipping boundary creates non-manifold).
- **D36.** **Flip duplicate-edge gate** (`vertices_connected(c, d)`
  pre-check). v7a's `flip_edge` does NOT check this case; without the
  gate, flipping an edge whose flipped diagonal already exists creates
  a duplicate edge (non-manifold). Found via failing manifold test in
  remeshed cube; gate added in v7d.
- **D37.** **Tangential smoothing** (Botsch-Kobbelt §4 actual spec, not
  the naïve "move-to-centroid"). Displacement projected onto the tangent
  plane at v via `d_t = d - (d·n)·n` with `n` the area-weighted vertex
  normal. The normal component would accumulate off-surface drift across
  iterations and produce non-manifold configurations.
- **D38.** **Inversion-rejection safeguard** in smoothing pass. If moving
  v to new_p would flip any incident face's orientation
  (`dot(n_old, n_new) ≤ 0`), keep v fixed. Botsch-Kobbelt §4
  triangle-quality protection.

### Files touched

- `engine/geometry-mesh-processing/CMakeLists.txt` — added `crd-geometry-bvh` + `crd-geometry-mesh` PUBLIC deps.
- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/isotropic_remesh.hpp` — NEW.
- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/mesh_processing.hpp` — added include.
- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/half_edge_mesh.hpp` — added `set_vertex_position`.
- `engine/geometry-mesh-processing/src/isotropic_remesh.cpp` — NEW (impl + explicit `f32`/`f64` instantiations).
- `tests/geometry-mesh-processing/test_isotropic_remesh.cpp` — NEW (9 cases).
- `tests/geometry-mesh-processing/CMakeLists.txt` — added new test source.

### Tests / verification

- **9 Catch2 cases**; mesh-processing suite total = **47 cases / 925
  assertions** (v7a 14 + v7b 15 + v7c 9 + v7d 9).
- Coverage:
  - `EmptyMesh` diagnostic.
  - `InvalidTargetLength` diagnostic.
  - `n_iterations = 0` no-op (clone of input).
  - Subdivided cube refines when target < mean edge: mesh grows; manifold
    preserved across 3 iterations (the case that flushed out D36+D37+D38).
  - Subdivided cube coarsens when target > mean edge: mesh shrinks.
  - Open quad: boundary held fixed (corner positions + boundary loop
    count preserved).
  - Determinism: same input → byte-identical stats + counts.
  - Surface projection keeps cube vertices within `[0, 1]^3` (no
    inward shrink).
  - f64 precision tier.
- **4-config DoD via `scripts/per-slice-check.ps1` — PASS** on retry
  (elapsed 03:13): win-debug + win-asan + win-shipping + win-tidy.
  - First DoD attempt failed at win-tidy build with **transient
    `bugprone-reserved-identifier` clang-tidy access violation** —
    upstream LLVM bug, not code-side. Same policy class as
    `feedback_transient_msvc_ltcg_ice_accept.md` (MSVC LTCG ICE):
    retry-clean accepted; filed `feedback_transient_clang_tidy_crash.md`
    for the new instance.

### Bugs found + fixed mid-slice

The "subdivided cube refines with target < mean" test was failing
manifoldness after iteration 3 (passed at 1 and 2). Bisected:

1. **Disable projection** → manifold preserved (issue traced to
   smoothing's position changes feeding back into later iterations).
2. **Disable smoothing entirely** (λ=0) → manifold preserved (confirmed
   smoothing was the trigger, even though smoothing itself doesn't
   change topology — it changes positions which change what later
   iterations split/collapse/flip).
3. **Root cause #1**: `flip_edge` from v7a doesn't gate on the
   "would-create-duplicate-edge" case. If `c` and `d` (the apex
   vertices of the two faces sharing the flipped edge) were already
   connected by another edge, the flip creates a non-manifold
   duplicate edge `(c, d)`. Fix: `vertices_connected(c, d)` pre-check
   in v7d's flip pass (D36). Did NOT resolve the failure alone, so:
4. **Root cause #2**: pure-Laplacian smoothing ("move to centroid")
   pushes vertices off-surface; BVH projection snaps them back, but
   the projected positions can create degenerate or near-flipped
   triangles in dense regions. Fix: tangential smoothing (D37) +
   inversion-rejection safeguard (D38).

### Next session starts with

v7e Liepa 2003 hole filling. Detect boundary loops; for each loop,
fill via weighted minimum-area triangulation (dynamic programming
over the loop minimising `area(t) + λ · dihedral_angle_penalty(t)`).
Optional second-pass fairing (Laplacian-smooth the patch interior
while constraining boundary). First step:
`engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/fill_holes.hpp`
with the `fill_holes(HalfEdgeMesh<T>&, opts)` entry point.
