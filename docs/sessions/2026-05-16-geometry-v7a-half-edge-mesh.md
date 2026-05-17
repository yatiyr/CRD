## Session 2026-05-16 — Phase 3.1.7 v7a `HalfEdgeMesh<T>` substrate

### Goal

Ship `crd-geometry-mesh-processing`'s v7a slice: the mutable half-edge
mesh data structure that every v7 algorithm (QEM decimation, Loop
subdivision, remeshing, hole filling, repair, self-intersection removal,
Taubin smoothing) consumes. v7 is the 8th `crd-geometry-*` sub-module
and the MUTABLE side of geometry (v4 `-mesh` is the read-only side).

### What we built / changed

- **New module `crd-geometry-mesh-processing`**:
  - `engine/geometry-mesh-processing/CMakeLists.txt` — depends on
    `crd-core`, `crd-containers`, `crd-memory`, `crd-math`, `crd-units`,
    `crd-geometry-primitives`. PCH-enabled.
  - `include/crd/geometry/mesh_processing/half_edge_mesh.hpp` — full
    `HalfEdgeMesh<T>` API (T = MathScalar).
  - `include/crd/geometry/mesh_processing/mesh_processing.hpp` — umbrella.
  - `src/half_edge_mesh.cpp` — implementation + explicit instantiation
    for `f32` and `f64`.
- **New tests `tests/geometry-mesh-processing/`** — 14 Catch2 cases.
- **Edges**:
  - `tests/CMakeLists.txt` — added `add_subdirectory(geometry-mesh-processing)`.

### Plain-English explanation

A half-edge mesh splits every shared edge into two directed
"half-edges," one for each face that touches it. From a single half-edge
you can find its twin (the other side), its next half-edge in the same
face, and the vertex it points out of. With those three pointers you
can walk a face, rotate around a vertex, identify the boundary, and do
local surgery (collapse an edge, split it, flip the diagonal) — all in
constant time per operation. That is what mesh-processing algorithms
need: locality + topology editing. A flat index buffer can't do this
in O(1).

Our `HalfEdgeMesh<T>` stores three flat arrays (vertices, half-edges,
faces) with stable indices and free-lists. Boundary half-edges are
materialised (`face == k_null_face`) so the boundary loop is the same
`.next` traversal as a face loop — no special-case branching in hot
loops. `build_from` takes a flat triangle list and pairs twins via a
lex-tuple sort that's bit-identical across compilers (determinism per
ADR-0063). Topology queries (`is_manifold`, `is_closed`, `Euler`,
`boundary_loop_count`) cost O(N) but are cheap and never on the hot
path. The three atomic edits — `collapse_edge` (gated by Edelsbrunner
2001's link condition), `split_edge`, `flip_edge` — are the building
blocks that v7b–v7h compose into the named algorithms.

### Decisions made (pinned for ADR-0076 §22 amendment at v7-close)

- **D1.** Twin pairing uses lex-tuple `(min(va,vb), max(va,vb),
  he_id)` sort + adjacent-pair sweep. Bit-identical across MSVC / GCC /
  clang. (`build_from` step 3.)
- **D2.** Boundary half-edges are **materialised** (one per unpaired
  interior HE, `face == k_null_face`). The alternative ("twin ==
  k_null") was rejected — materialised boundary HEs keep every
  algorithm's hot loop branchless on the twin pointer.
- **D3.** `prev` is NOT stored. Two `next` hops cost O(1) for triangle
  faces. Memory saving (4 B × ~6 HE/triangle = 24 B/triangle)
  dominates. Revisit when v8+ generalises to n-gons.
- **D4.** Free-list pop is LIFO (last-freed-first-reused). Deterministic
  given a deterministic edit sequence.
- **D5.** `collapse_edge` gated by the Edelsbrunner 2001 link
  condition: 1-rings of `a` and `b` share EXACTLY the two opposite
  apex vertices (interior) or exactly one apex (boundary). Any other
  shared neighbour would create a duplicate edge — reject.
- **D6.** Boundary collapse rejected for v7a (returns false). The
  boundary-collapse path is more delicate and is added in v7f
  (manifoldness repair).
- **D7.** `for_each_outgoing_he` uses uniform CW rotation
  `cur.twin.next` — works the same way for interior and boundary HEs.
  For boundary vertices, `build_from` points the vertex's `outgoing`
  field at the boundary HE so the walk starts there and visits the
  full fan exactly once before wrapping back.
- **D8.** `HalfEdgeSlot` is 16 B (4 × u32: origin, twin, next, face).
  Static-asserted. No flag byte — `origin == k_null_vertex` is the
  dead sentinel.

### Files touched

- `engine/geometry-mesh-processing/CMakeLists.txt` — new module.
- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/half_edge_mesh.hpp` — full API.
- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/mesh_processing.hpp` — umbrella.
- `engine/geometry-mesh-processing/src/half_edge_mesh.cpp` — implementation.
- `tests/geometry-mesh-processing/CMakeLists.txt` — test target.
- `tests/geometry-mesh-processing/test_half_edge_mesh.cpp` — 14 cases.
- `tests/CMakeLists.txt` — `add_subdirectory(geometry-mesh-processing)`.

### Tests / verification

- 14 Catch2 cases / 67 assertions — `All tests passed`.
- Coverage: build round-trip (triangle, cube, quad), diagnostic build
  statuses (degenerate / non-finite / out-of-bounds / odd index count),
  topology queries (closed cube: 8v/18e/12f/Euler=2, open quad:
  1 boundary loop), `for_each_outgoing_he` fan walk (cube interior +
  triangle boundary), `flip_edge` round-trip, `split_edge` count delta,
  `collapse_edge` interior with link-condition gate, f64 precision tier.
- **4-config DoD via `scripts/per-slice-check.ps1` — PASS** (elapsed
  03:51): win-debug + win-asan + win-shipping + win-tidy.

### Bugs found + fixed mid-slice

1. **Fan walk on boundary vertices missed outgoings.** Original code
   used CCW rotation (`he_prev(cur).twin`) for interior and a different
   branch for boundary; on the open quad's boundary vertex the walk
   ping-ponged between two outgoings and missed a third. Fix: switch
   to uniform CW rotation `cur.twin.next` for both interior and
   boundary, and point boundary vertices' `outgoing` at their boundary
   HE so the rotation visits the full half-fan before wrapping.
2. **Post-collapse apex `outgoing` pointed at HEs from the wrong vertex.**
   `apex1` (= c) was redirected to `hp_twin` (which originates from a,
   not c). Fix: redirect to `hn_twin` (= twin of `h_next` = `b→c`
   before, `c→a` after — originates from c). Same fix for apex2 / d /
   `tn_twin`.
3. **Post-collapse `a.outgoing` deletion-check tested `t_prev`** (origin
   d, never matched). Fix: test `t_next` (origin a, the actual deleted
   HE originating from a). Added prefer-rim-survivors fallback ladder.

### Next session starts with

v7b QEM decimation (Garland-Heckbert 1997 — quadric matrix per vertex,
collapse-cost min-heap, link-condition gate already in `collapse_edge`).
First step: `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/qem_decimate.hpp`
with the `Quadric<T>` type (4×4 symmetric — store 10 floats) and the
`qem_decimate(HalfEdgeMesh<T>&, options)` entry point.
