## Session 2026-05-17 — Phase 3.1.7 v7c Loop subdivision (Loop 1987)

### Goal

Ship `crd-geometry-mesh-processing` v7c: Charles Loop's classical
triangle-mesh subdivision scheme. Each triangle splits 1 → 4 via
edge-midpoint insertion plus a smoothing mask on existing vertex
positions. The limit surface is C² almost everywhere (C¹ at extraordinary
vertices, the only exception) — the canonical "smooth a coarse triangle
mesh" operation that cinematic, procgen, scanner-cleanup, and CAD-viz
pipelines reach for.

### What we built / changed

- **New `loop_subdivide` entry point** in
  `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/loop_subdivide.hpp`
  + `.cpp`:
  - `LoopSubdivideOptions` — `n_levels`, `output_allocator`.
  - `LoopSubdivideStatus` — `Ok`, `EmptyMesh`, `NonManifoldInput`.
  - `LoopSubdivideReport` — `status`, `levels_applied`,
    `output_vertices`, `output_faces`.
  - `HalfEdgeMesh<T> loop_subdivide(const HalfEdgeMesh<T>&, opts,
    &report)` — input untouched; returns a fresh subdivided mesh on the
    requested allocator.
- **Umbrella header** `mesh_processing.hpp` extended.
- **9 Catch2 tests** in `tests/geometry-mesh-processing/test_loop_subdivide.cpp`.
- **Test target CMakeLists.txt** extended with new source.

### Plain-English explanation

Loop subdivision is the algorithm every modern smoothing pipeline learns
first. Take a triangle mesh, insert a midpoint on every edge, then carve
each triangle into 4 sub-triangles. The new vertex positions aren't just
geometric midpoints — they're carefully chosen weighted averages so that,
after many subdivision rounds, the surface converges to a smooth limit
surface that closely approximates a Bézier-spline-like ideal.

There are two masks. **The edge-vertex mask** decides where a new
midpoint sits: for an interior edge `(A, B)` shared by faces `(A, B, C)`
and `(B, A, D)`, the midpoint is `3/8·A + 3/8·B + 1/8·C + 1/8·D` — biased
toward the two endpoints but pulled gently toward the apex vertices.
**The vertex-update mask** repositions every existing vertex: an interior
vertex of valence `n` with neighbours `u_i` becomes
`(1 - n·β)·V + β·Σ u_i` where `β` depends transcendentally on `n` (the
famous Loop formula). Boundary vertices use a separate cubic-B-spline
mask `3/4·V + 1/8·u_left + 1/8·u_right` so the silhouette refines as
a smooth curve.

A cube becomes 4-subdivided at the corners but rounds out everywhere
else; after a few levels it looks like a smoothed dice. An open patch
keeps its boundary (the boundary loop count never changes — each old
boundary edge becomes 2 new ones, still forming the same loop).

### Decisions made (pinned for ADR-0076 §22 amendment at v7-close)

- **D17.** Indexed-form pipeline. Each level: extract input HalfEdgeMesh
  to (positions, indices) → build temp half-edge mesh for topology
  queries → emit new (positions, indices) → repeat. Final `build_from`
  creates the output mesh. Avoids HalfEdgeMesh move-semantics questions
  and keeps the work serial-deterministic.
- **D18.** Vertex numbering: output = [n_old old vertices remapped via
  slot order] ++ [n_edges new midpoint vertices in canonical-HE-id
  order]. Canonical HE = `min(h, h.twin)` per undirected edge.
- **D19.** Sub-triangle CCW emission. Each face (v₀, v₁, v₂) → four
  sub-triangles: corner@v₀ (v₀, m₀₁, m₂₀); corner@v₁ (v₁, m₁₂, m₀₁);
  corner@v₂ (v₂, m₂₀, m₁₂); central (m₀₁, m₁₂, m₂₀). All CCW so
  orientation propagates.
- **D20.** Interior edge midpoint = `3/8·A + 3/8·B + 1/8·C + 1/8·D`
  (Loop 1987 §2.2; C, D = apex of the two incident faces).
- **D21.** Boundary edge midpoint = `(A + B)/2` (limit position on the
  cubic-B-spline subdivision curve; Loop 1987 §2.3).
- **D22.** Interior vertex update = `(1 - n·β)·V + β·Σ neighbours`,
  with `β = (1/n)·(5/8 - (3/8 + 1/4·cos(2π/n))²)`. Uses
  `crd::math::deterministic::cos` for bit-identical FP across compilers
  and architectures (ADR-0063 — eylem replay-hash CI doesn't apply here,
  but the determinism contract is the same).
- **D23.** Boundary vertex update = `3/4·V + 1/8·u_left + 1/8·u_right`
  (Loop 1987 §2.3; cubic-B-spline boundary curve mask).
- **D24.** Multi-level = repeated application of single-level
  subdivision. `n_levels = 0` returns a fresh clone of input (via
  `to_indexed → build_from` on the requested allocator).
- **D25.** Boundary-neighbour detection. For each outgoing HE at v,
  the edge `(v, dest)` is boundary iff `he_is_boundary(ho)` OR
  `he_is_boundary(twin(ho))`. The CW fan walk from v7a guarantees
  exactly two such hits for a 2-manifold boundary vertex.

### Files touched

- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/loop_subdivide.hpp` — NEW.
- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/mesh_processing.hpp` — added new include.
- `engine/geometry-mesh-processing/src/loop_subdivide.cpp` — NEW (impl + explicit `f32`/`f64` instantiations).
- `tests/geometry-mesh-processing/test_loop_subdivide.cpp` — NEW (9 cases).
- `tests/geometry-mesh-processing/CMakeLists.txt` — added new test source.

### Tests / verification

- **9 Catch2 cases / ~70 assertions** for Loop; mesh-processing suite
  total = **38 cases / 605 assertions** (v7a 14 + v7b 15 + v7c 9).
- Coverage:
  - `EmptyMesh` diagnostic.
  - `n_levels = 0` no-op (clone of input).
  - Cube L=1: V=26, F=48, E=72, χ=2 (sphere preserved), closed manifold.
  - Cube L=2: V=98, F=192, E=288, χ=2 still.
  - Open quad L=1: V=9, F=8, E=16, χ=1 (disk), 1 boundary loop.
  - Boundary edge midpoint = geometric midpoint check.
  - Determinism: same input → byte-identical positions, two runs.
  - f64 precision tier cube subdivision.
  - Cube smoothing: corner moves inward toward centroid (Loop pulls
    corners in; doesn't overshoot past the centroid).
- **4-config DoD via `scripts/per-slice-check.ps1` — PASS** (elapsed
  03:47): win-debug + win-asan + win-shipping + win-tidy.
- One clang-tidy warning fixed mid-pass: local constexpr
  `LocalConstexprVariableCase = CamelCase, Prefix = 'k'` (project
  convention disagrees with `CLAUDE.md` lower_case spec — `.clang-tidy`
  is authoritative). Renamed `k_two_pi` → `kTwoPi`. Retry clean.

### Next session starts with

v7d Botsch-Kobbelt 2004 isotropic remeshing. Iterates 5-10 passes of:
(1) split edges longer than `4/3 · target_len`; (2) collapse edges
shorter than `4/5 · target_len`; (3) flip edges to improve valence
toward 6 (interior) / 4 (boundary); (4) tangential relaxation (move
each vertex toward area-weighted centroid of one-ring, projected back
to the input surface via closest-point on BVH). First step:
`engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/isotropic_remesh.hpp`
with the `isotropic_remesh(HalfEdgeMesh<T>&, target_length, n_iters,
input_bvh_or_self)` entry point. Closest-point projection requires
building a `TriangleMeshBvh` from the input surface
(crd-geometry-bvh + crd-geometry-mesh).
