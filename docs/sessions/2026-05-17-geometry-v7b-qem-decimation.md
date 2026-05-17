## Session 2026-05-17 — Phase 3.1.7 v7b QEM decimation (Garland-Heckbert 1997)

### Goal

Ship `crd-geometry-mesh-processing` v7b: quadric edge collapse decimation
— the cooker LOD ladder primitive. Every shipping game generates 3-5 LODs
per mesh from this algorithm. Position-only quadrics, closed-form `v_opt`
via 3x3 inverse, Garland 1998 boundary preservation, locked vertices,
inversion prevention, lazy-invalidation min-heap.

### What we built / changed

- **New `Quadric<T>` type** in `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/quadric.hpp`:
  - Symmetric 4x4 stored as 10 unique floats (layout pinned by hash-stable serialisation contract).
  - `Quadric::from_plane(a, b, c, d)` outer-product constructor.
  - `operator+` / `operator+=` / `operator*` (scalar).
  - `evaluate(q, v)` expanded form (no matrix multiply — direct quadratic
    in `v.x`, `v.y`, `v.z` exploiting symmetry).
  - `optimal_position(q, det_eps)` — 3x3 inverse via Cramer's rule +
    cofactor expansion; returns `nullopt` on singular system.
- **New `qem_decimate` entry point** in `qem_decimate.hpp` / `qem_decimate.cpp`:
  - `QemDecimateOptions<T>` — `target_face_count`, `max_error_threshold`,
    `locked_vertices`, `boundary_weight` (default 1000), `singular_det_epsilon`
    (default 1e-10), `output_allocator`.
  - `QemDecimateReport` — `status`, `collapses_applied`,
    `collapses_rejected_link`, `collapses_rejected_flip`,
    `singular_fallbacks`.
  - `QemDecimateStatus` — `Ok`, `NoStopCondition`, `NonManifoldInput`,
    `EmptyMesh`, `TargetUnreachable`.
  - `HalfEdgeMesh<T> qem_decimate(const HalfEdgeMesh<T>&, options, &report)`
    — input untouched; returns a fresh decimated mesh on the requested
    allocator.
- **`HalfEdgeMesh::allocator()` getter** added — exposes the underlying
  `IAllocator*` so consumers (like `qem_decimate`) can allocate scratch
  arrays on the same allocator as the mesh.
- **Umbrella header** `mesh_processing.hpp` extended with the new headers.
- **14 Catch2 tests** added across `test_quadric.cpp` (7) and
  `test_qem_decimate.cpp` (8); total mesh-processing suite = 29 cases /
  156 assertions.

### Plain-English explanation

Quadric Edge Collapse Decimation is the algorithm every modern LOD
pipeline uses. For each vertex of a triangle mesh, you accumulate a "cost
matrix" (the quadric) that measures the total squared distance from any
point to all the planes of the faces that originally touched that vertex.
When you collapse an edge, you merge the two endpoints into one new
vertex, and the optimal location for that new vertex is the point that
minimises the combined quadric — a 3x3 linear-algebra problem with a
closed-form solution. You order all the edges by their collapse cost in
a min-heap and repeatedly collapse the cheapest, stopping when you hit a
target face count or an error budget. The result: a lower-poly mesh that
clings tightly to the original silhouette.

We added two refinements that the field considers mandatory: **boundary
preservation** (Garland's 1998 follow-up — add a heavy "wall plane"
quadric perpendicular to each face along its boundary edges, so the
silhouette can't collapse inward) and **inversion prevention** (predict
the result of substituting `v_opt`; if any surviving face would flip its
normal, reject the collapse). Both are off-by-default in some research
implementations but on-by-default in every production cooker, and
shipping QEM without them would be a quality compromise.

Locked vertices fall out naturally as a UI primitive — the cooker can
mark attachment points (sockets, decals, gameplay anchors) as locked
before decimation. Locked-touching edges either constrain `v_opt` to the
locked endpoint (1-locked) or are rejected (2-locked); the locked vertex
survives with bit-identical position.

### Decisions made (pinned for ADR-0076 §22 amendment at v7-close)

- **D9.** Per-vertex quadric `Q_v = Σ_{f ∋ v} K_f` where each
  `K_f = p_f · p_f^T` for the unit-normal face plane `p_f`. Degenerate
  faces (`|cross| < 1e-20`) contribute zero.
- **D10.** Boundary preservation (Garland 1998 §3.1): for each boundary
  HE, add `boundary_weight * p_b · p_b^T` to both endpoints, where
  `n_b = normalize(edge × n_f)`. Default weight = 1000.
- **D11.** Per-edge cost: solve closed-form `v_opt` for combined quadric;
  fall back to midpoint if singular. Locked endpoint constrains `v_opt`
  to its position; both-locked rejected.
- **D12.** Min-heap entries `{cost, canonical_he, generation}` sorted by
  lex `(cost, canonical_he)` — byte-identical ordering across compilers.
  Canonical HE = `min(h, h.twin)` per undirected edge.
- **D13.** Lazy invalidation: per-canonical-HE `generation` counter
  bumped on every push; pop checks `generation == edge_generation[id]`
  AND `he_alive` AND `!he_is_boundary`. No periodic prune (heap waste
  bounded by O(initial_edges × avg_valence)).
- **D14.** Inversion prevention: before each collapse, for every face
  incident to `a` or `b` (excluding `f1`/`f2`, the collapse-deleted
  faces), substitute `v_opt` for both endpoints and check that the new
  cross product has the same sign as the old. Rejects v_opt placements
  that would self-intersect.
- **D15.** Input untouched. Output mesh constructed via
  `input.to_indexed → build_from` on the requested allocator. Locked
  vertex indices reference the INPUT slot space; the `to_indexed` remap
  translates them to output space.
- **D16.** Both stop conditions optional but at least one must be
  specified — else status = `NoStopCondition` (no work done). When both
  are set, the loop stops at whichever fires first.

### Files touched

- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/quadric.hpp` — NEW.
- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/qem_decimate.hpp` — NEW.
- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/mesh_processing.hpp` — added the new includes.
- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/half_edge_mesh.hpp` — added `allocator()` getter.
- `engine/geometry-mesh-processing/src/qem_decimate.cpp` — NEW (implementation + explicit `f32`/`f64` instantiations).
- `tests/geometry-mesh-processing/test_quadric.cpp` — NEW (7 cases).
- `tests/geometry-mesh-processing/test_qem_decimate.cpp` — NEW (8 cases).
- `tests/geometry-mesh-processing/CMakeLists.txt` — added new test sources.

### Tests / verification

- **29 Catch2 cases / 156 assertions** across the mesh-processing suite
  (v7a 14 + v7b 15).
- Coverage: zero quadric / plane-distance invariant / addition + scalar
  multiplication / closed-form `optimal_position` regular case / singular
  fallback / f64 precision; QEM diagnostics (`NoStopCondition`,
  `EmptyMesh` not exercised but reachable); cube decimation by face
  count + by error threshold; locked vertices (all-locked → zero
  collapses; subset-locked → locked corners survive position-intact);
  open quad boundary preservation; determinism (same input → byte-
  identical vertex positions across two runs); f64 cube decimation.
- **4-config DoD via `scripts/per-slice-check.ps1` — PASS** (elapsed
  03:24): win-debug + win-asan + win-shipping + win-tidy.
- First run found 3 fixable issues (em-dash in `determinism` test case
  name → caught by `crd-no-non-ascii-test-names` guard; clang-tidy
  warnings on `K`/`Kb`/`Qe`/`Q` variable case style — renamed to
  `face_q`/`boundary_q`/`combined_q`/`vertex_q`; multi-decl + unchecked
  optional access in tests — split + dereferenced via local).
  All clean on retry.

### Next session starts with

v7c Loop subdivision (Loop 1987). Edge-midpoint insertion with
`HalfEdgeMesh::split_edge` (already shipped in v7a); edge-vertex weights
`3/8·A + 3/8·B + 1/8·C + 1/8·D`; vertex-update weights via the standard
`β(n)` formula; boundary edges use cubic-B-spline weights
`(1/8, 3/4, 1/8)`. First step:
`engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/loop_subdivide.hpp`
with the `loop_subdivide(HalfEdgeMesh<T>&, n_levels)` entry point.
