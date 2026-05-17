## Session 2026-05-17 — Phase 3.1.7 v7f manifoldness repair

### Goal

Ship `crd-geometry-mesh-processing` v7f: detect + fix the two classical
non-manifold pathologies — non-manifold edges (> 2 incident triangles)
and bowtie vertices (one-ring with > 1 disjoint fan) — and produce a
strictly 2-manifold output. The substrate's "make this triangle soup
2-manifold" primitive that downstream consumers (physics collision
mesh, SDF bake, FEA tessellation, photogrammetry cleanup) all require.

### What we built / changed

- **New `repair_manifoldness` entry point** in
  `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/repair_manifoldness.hpp`
  + `.cpp`:
  - `RepairManifoldnessOptions` — `repair_non_manifold_edges` (default
    on), `repair_bowtie_vertices` (default on), `output_allocator`.
  - `RepairManifoldnessStatus` — `Ok`, `EmptyMesh`, `AlreadyManifold`.
  - `RepairManifoldnessReport` — `status`, `non_manifold_edges_detected`,
    `non_manifold_edges_repaired`, `bowtie_vertices_detected`,
    `bowtie_vertices_repaired`, `duplicated_vertices_added`,
    `output_vertices`, `output_faces`.
  - `HalfEdgeMesh<T> repair_manifoldness(const HalfEdgeMesh<T>&, opts,
    &report)` — input untouched; returns a fresh strictly-2-manifold
    copy.
- **Umbrella header** `mesh_processing.hpp` extended.
- **9 Catch2 tests** in `tests/geometry-mesh-processing/test_repair_manifoldness.cpp`.

### Plain-English explanation

Real-world meshes — scanner outputs, exported CAD models, procedurally-
generated geometry — often violate the 2-manifold property in two
classical ways. v7f detects and fixes both.

**Non-manifold edges**: an edge with more than two adjacent triangles
can't be represented in any 2-manifold surface. v7f groups the triangles
by orientation (which way the shared edge points in each), pairs them
up, and for every pair beyond the first, duplicates one of the edge's
endpoints — the extra triangles now share a parallel copy of the edge
with new endpoints, which becomes its own manifold edge.

**Bowtie vertices**: a vertex where two surface fans meet at a single
point — visually like an hourglass with the pinch at that vertex. Each
fan is connected internally but they don't touch via any edge through
the vertex. v7f detects these by walking the v7a CW fan-walk and
comparing to the total outgoing-HE slot count; if the walk visits fewer
than the slot count, the vertex is a bowtie. To repair, the algorithm
groups the incident triangles into fans via a "share an edge through v"
adjacency BFS, then duplicates the vertex once per fan beyond the first.

The two phases are sequenced: Phase A (edges) runs first because Phase
B's detection requires building a temp half-edge mesh, which itself
requires manifold-EDGE input. The two phases can cascade — Phase A's
vertex duplication can break a previously-connected fan structure,
which Phase B then detects and repairs.

### Decisions made (pinned for ADR-0076 §22 amendment at v7-close)

- **D52.** Phase A operates at INDEX LEVEL via
  `crd::containers::HashMap<u64, Array<u32>>` keyed on the packed
  `(min(u,v), max(u,v))` u64 edge key.
- **D53.** Phase A pairing: forwards (origin < dest) vs backwards
  (origin > dest). First (forward, backward) pair keeps original
  endpoints; subsequent pairs share a single duplicated `min(u,v)`
  vertex. Singletons (orientation imbalance) each get their own
  duplicate.
- **D54.** Vertex-corner finder: `replace_vertex_in_triangle` scans
  the 3 corners of a triangle for the target value and updates it.
- **D55.** Phase B uses a TEMP HalfEdgeMesh for the v7a CW fan-walk
  detection. Walk count vs slot count mismatch → bowtie.
- **D56.** Fan-component BFS uses union-find on triangle-share-edge-
  through-v adjacency. Two triangles are adjacent at `v` iff they
  share another vertex.
- **D57.** Bowtie repair: lowest-min-triangle-id fan keeps `v`; each
  other fan gets a duplicated vertex.
- **D58.** Phase A → Phase B order. Phase B requires manifold edges
  (Phase A ensures this).
- **D59.** `AlreadyManifold` status when both phases detected zero
  pathologies; `Ok` when ≥1 was detected (and repaired).

### Files touched

- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/repair_manifoldness.hpp` — NEW.
- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/mesh_processing.hpp` — added include.
- `engine/geometry-mesh-processing/src/repair_manifoldness.cpp` — NEW (impl + `f32`/`f64` instantiations).
- `tests/geometry-mesh-processing/test_repair_manifoldness.cpp` — NEW (9 cases).
- `tests/geometry-mesh-processing/CMakeLists.txt` — added new test source.

### Tests / verification

- **9 Catch2 cases**; mesh-processing suite total = **67 cases / 1065
  assertions** (v7a 14 + v7b 15 + v7c 9 + v7d 9 + v7e 11 + 4 extra +
  v7f 9 + 4 extra rebuild-determinism).
- Coverage:
  - `EmptyMesh` diagnostic.
  - `AlreadyManifold` on a closed cube.
  - 3-triangle non-manifold edge: Phase-A-only verifies 1 duplicate,
    full repair (A+B) verifies cascade adds Phase B duplicate (= 2).
  - 4-triangle non-manifold edge (2 fwd + 2 bwd paired): same cascade
    pattern.
  - Classic bowtie vertex (2 disjoint tetra fans meeting at one point):
    1 duplicate, output is_manifold.
  - Phase toggle: A=off, B=off behave correctly.
  - Determinism: same input → same counts.
  - f64 precision tier (bowtie test).
- **4-config DoD via `scripts/per-slice-check.ps1` — PASS first try**
  (elapsed 03:45): win-debug + win-asan + win-shipping + win-tidy.

### Bugs found + fixed mid-slice

Initial test assertions for "3-triangle" and "4-triangle" non-manifold
edge tests expected only 1 duplicate from Phase A; the actual repair
ALSO triggers Phase B (because Phase A's vertex duplication
disconnects the bowtie-detection criterion at the OTHER shared
endpoint, which Phase B then repairs). Updated tests to verify both
phases explicitly:
- Phase-A-only run (B disabled): verifies 1 duplicate.
- Full run (A+B): verifies 2 duplicates (1 from each phase).
This is correct elite-tier behavior — a non-manifold edge repair
naturally produces a bowtie that needs Phase B to clean up.

### Next session starts with

v7g self-intersection removal (consumes v6e Bentley-Ottmann). Detect
triangle-triangle intersections via BVH broadphase + Möller 1997 exact
test; resolve via per-face planar projection + v6c CDT inserting the
intersection segments as constraints. First step:
`engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/remove_self_intersections.hpp`.
