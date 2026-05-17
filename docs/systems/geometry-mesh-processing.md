# crd-geometry-mesh-processing — system overview

Phase 3.1.7 sub-module 8 of 11. Mutable triangle-mesh substrate +
canonical mesh-processing algorithms (decimation, subdivision,
remeshing, hole filling, manifoldness repair, self-intersection
removal, smoothing). The "modify a triangle mesh" half of geometry;
the read-only side is `crd-geometry-mesh` (v4).

## Status

**Closed 2026-05-17** — 8 algorithm slices + 1 substrate slice ALL
SHIPPED via per-slice 4-config DoD + cluster 18-config full sweep.

| Slice | Date | What | Decisions |
|---|---|---|---|
| v7a | 2026-05-16 | `HalfEdgeMesh<T>` substrate | D1-D8 |
| v7b | 2026-05-17 | Quadric Edge Collapse Decimation (Garland-Heckbert 1997) | D9-D16 |
| v7c | 2026-05-17 | Loop subdivision (Loop 1987) | D17-D25 |
| v7d | 2026-05-17 | Isotropic remeshing (Botsch-Kobbelt 2004) | D26-D38 |
| v7e | 2026-05-17 | Liepa hole filling (Liepa 2003 §3+§4+§5) | D39-D51 |
| v7f | 2026-05-17 | Manifoldness repair (non-manifold edges + bowtie vertices) | D52-D59 |
| v7g | 2026-05-17 | Self-intersection removal (Möller 1997 + per-tri CDT) | D60-D66 |
| v7h | 2026-05-17 | Taubin smoothing (Taubin 1995) | D67-D72 |
| v7-close | 2026-05-17 | ADR §22 + this doc + 18-config sweep | — |

Test suite: **80 cases / 1180 assertions**. ADR-0076 §22 locks all
72 design decisions across the 8 slices.

## When to use what

| Goal | Use | Notes |
|---|---|---|
| Make a fresh mutable mesh from indexed data | `HalfEdgeMesh<T>::build_from` | v7a; the substrate everyone else operates on |
| Generate lower-poly LODs (cooker ladder) | `qem_decimate` | v7b; Hoppe-style inversion prevention + Garland 1998 boundary preservation built in |
| Smooth + refine a coarse mesh | `loop_subdivide` | v7c; C² limit surface almost everywhere; cinematic / procgen primitive |
| Uniformise tessellation (FEA / GPU shading-rate) | `isotropic_remesh` | v7d; split + collapse + flip + tangential smoothing with input-BVH projection |
| Close holes in a scanned mesh | `fill_holes` | v7e; §3 DP + §4 Steiner refinement + §5 fairing — full Liepa pipeline |
| Fix non-manifold edges / bowtie vertices | `repair_manifoldness` | v7f; Phase A edges → Phase B bowties (can cascade) |
| Cut self-intersecting triangle pairs | `remove_self_intersections` | v7g; Möller 1997 + per-tri CDT via v6c `constrained_delaunay` |
| Remove noise without volume drift | `taubin_smooth` | v7h; 2-pass λ-shrink + μ-anti-shrink low-pass filter |

## Architecture

8 algorithms + 1 substrate, all building on the same data structure:

```
┌────────────────────────────────────────────────────────────────────┐
│  v7a HalfEdgeMesh<T> — half-edge DCEL substrate                    │
│  ─────────────────────────────────────────────                     │
│  • 16-byte HalfEdgeSlot {origin, twin, next, face} (static_asserted)
│  • Materialised boundary HEs (boundary loop = same .next walk as face)
│  • Lex-tuple twin pairing in build_from (bit-identical across compilers)
│  • Uniform CW fan rotation `cur.twin.next` (interior + boundary)   │
│  • Atomic edits: collapse_edge (link cond), split_edge, flip_edge  │
│  • set_vertex_position added in v7d                                │
└────────────────────────────────────────────────────────────────────┘
              │
              │  (in-place edits or to_indexed → rebuild)
              ▼
┌──────────────┬──────────────┬──────────────┬──────────────┐
│ v7b QEM      │ v7c Loop     │ v7d Remesh   │ v7e FillHoles│
│ Decimation   │ Subdivision  │ (B-K 2004)   │ (Liepa 2003) │
├──────────────┼──────────────┼──────────────┼──────────────┤
│ + Boundary   │ + Boundary   │ + Tangential │ + §4 Steiner │
│   preserv.   │   B-spline   │   smoothing  │   refinement │
│ + Inversion  │   mask       │ + Flip       │ + §5         │
│   prevention │ + Crd-math   │   duplicate- │   Laplacian  │
│ + Locked     │   determ.    │   edge gate  │   fairing    │
│   vertices   │   cos(2π/n)  │ + BVH        │ + Per-loop   │
│ + Lazy-      │              │   projection │   σ from in- │
│   invalid    │              │              │   put mesh   │
│   min-heap   │              │              │              │
├──────────────┴──────────────┴──────────────┴──────────────┤
│ v7f Manifoldness Repair  │ v7g Self-Inter. Removal       │
├──────────────────────────┼───────────────────────────────┤
│ Phase A: non-mfd edges   │ Möller 1997 + orient3d-gated  │
│ Phase B: bowtie vertices │ Per-tri CDT (v6c constrained_ │
│ A → B can cascade        │ delaunay) + cross-tri vertex  │
│                          │ stitching                     │
├──────────────────────────┼───────────────────────────────┤
│ v7h Taubin Smoothing                                     │
├──────────────────────────────────────────────────────────┤
│ 2-pass λ-shrink + μ-anti-shrink                          │
│ Volume-preserving low-pass filter (Taubin 1995)          │
└──────────────────────────────────────────────────────────┘
```

**Module dependencies** (PUBLIC, all transitive):
- `crd-core`, `crd-containers`, `crd-memory`, `crd-math`, `crd-units`
- `crd-geometry-primitives` (Vec3/closest_point/intersection corpus +
  orient3d via predicates.hpp — v7g)
- `crd-geometry-bvh` (BvhTree consumers for input-mesh projection — v7d)
- `crd-geometry-mesh` (TriangleMeshBvh + mesh_closest_point — v7d)
- `crd-geometry-polygon` (constrained_delaunay v6c — v7g)

## API at a glance

All entry points share the same shape: take a `const HalfEdgeMesh<T>&`
+ `<Algorithm>Options<T>`, return a fresh `HalfEdgeMesh<T>` on the
requested allocator; never mutate input.

```cpp
// v7a — substrate.
template <MathScalar T> class HalfEdgeMesh { ... };

// v7b — Quadric Edge Collapse Decimation (Garland-Heckbert).
template <MathScalar T>
HalfEdgeMesh<T> qem_decimate(const HalfEdgeMesh<T>&,
                              const QemDecimateOptions<T>&,
                              QemDecimateReport* = nullptr);

// v7c — Loop subdivision.
template <MathScalar T>
HalfEdgeMesh<T> loop_subdivide(const HalfEdgeMesh<T>&,
                                const LoopSubdivideOptions&,
                                LoopSubdivideReport* = nullptr);

// v7d — Botsch-Kobbelt isotropic remeshing.
template <MathScalar T>
HalfEdgeMesh<T> isotropic_remesh(const HalfEdgeMesh<T>&,
                                  const IsotropicRemeshOptions<T>&,
                                  IsotropicRemeshReport* = nullptr);

// v7e — Liepa hole filling (§3 DP + §4 Steiner + §5 fairing).
template <MathScalar T>
HalfEdgeMesh<T> fill_holes(const HalfEdgeMesh<T>&,
                            const FillHolesOptions<T>&,
                            FillHolesReport* = nullptr);

// v7f — Manifoldness repair.
template <MathScalar T>
HalfEdgeMesh<T> repair_manifoldness(const HalfEdgeMesh<T>&,
                                     const RepairManifoldnessOptions&,
                                     RepairManifoldnessReport* = nullptr);

// v7g — Self-intersection removal.
template <MathScalar T>
HalfEdgeMesh<T> remove_self_intersections(const HalfEdgeMesh<T>&,
                                            const RemoveSelfIntersectionsOptions<T>&,
                                            RemoveSelfIntersectionsReport* = nullptr);

// v7h — Taubin smoothing.
template <MathScalar T>
HalfEdgeMesh<T> taubin_smooth(const HalfEdgeMesh<T>&,
                               const TaubinSmoothOptions<T>&,
                               TaubinSmoothReport* = nullptr);
```

All instantiated for `f32` + `f64`. Every entry point returns a
`<...>Report` struct with telemetry counters + a status enum:
algorithm-specific success states + diagnostic codes (`EmptyMesh`,
`NonManifoldInput`, `InvalidParameters`, etc.) for graceful failure.

## Determinism contract

Per ADR-0063 + ADR-0076 §4 pin #11:

- **Slot-order iteration** for every vertex/face/HE walk that produces
  output. New entries created by edits in the same pass are NOT
  re-processed in that pass (Botsch-Kobbelt iteration discipline,
  v7d D29).
- **Lex-tuple ordering** for any sort or comparator: v7a's twin pairing
  uses `(min(va,vb), max(va,vb), he-id)`; v7b's QEM heap uses
  `(cost, canonical_he_id)`; v7e's DP min uses `(composite_weight, m)`.
- **`crd::math::deterministic::cos`** in v7c for the β(n) weight —
  bit-identical FP across MSVC / clang-cl / GCC / x64 / ARM64 per
  ADR-0063.
- **`crd::math::deterministic::acos`** in v7d (Delaunay flip criterion)
  and v7e (Delaunay flip in Steiner refinement).
- **Shewchuk `orient3d`** in v7g for narrowphase early-exit signs +
  touch-only rejection.
- **Jacobi-style updates** in v7d / v7e fairing / v7h: compute new
  positions against OLD into scratch, apply atomically — order-
  independent.
- **Cross-platform byte-identical output** verified per-slice via the
  determinism test in each test file (`out_a == out_b` on two runs of
  same input).

## Robustness contract

- **2-manifold input required** by v7b/c/d/e/h. v7f IS the substrate's
  manifoldness-repair primitive — accepts non-manifold input, produces
  manifold output. v7g's narrowphase tolerates non-manifold input
  (skips touch-only cases).
- **Non-finite vertex positions** rejected at the underlying
  `HalfEdgeMesh::build_from` (per v7a).
- **Degenerate triangles** (`|cross| < 1e-20`) contribute zero to
  per-vertex quadrics (v7b D9), per-face planes (v7c D20), per-face
  normals (v7d). They don't crash; they just don't participate.
- **Inversion prevention** (Hoppe 1993-class):
  - v7b QEM checks every collapse candidate against face-flip on
    incident faces; rejects if any would flip (D14).
  - v7d's tangential smoothing rejects per-vertex moves that would
    flip incident faces (D38).
- **Link-condition gate** (Edelsbrunner 2001) on every `collapse_edge`
  call from `HalfEdgeMesh` itself — v7b, v7d, v7e all benefit.
- **Duplicate-edge gate** on flip in v7d / v7e (D36, reused in v7e's
  refinement) — prevents `flip_edge` from creating non-manifold.
- **`orient3d`-gated narrowphase** in v7g rejects touch-only cases
  (vertex-on-plane / edge-on-plane) that would produce spurious
  segments from FP roundoff (D61).
- **Graceful CDT-failure degradation** in v7g (D65): on
  `ConstraintsCrossing` the original triangle is kept verbatim — no
  garbage output.

## Performance pins

- **v7b QEM**: O(n log n) via lazy-invalidation min-heap; each
  collapse + neighbour re-evaluation is O(valence). Boundary check
  cached; locked-vertex bit-array indexed lookup.
- **v7c Loop**: O(F + E) per level. Indexed-form pipeline avoids
  per-level HE-mesh moves (D17). Local lookup for σ neighbours via
  CW fan walk.
- **v7d Remesh**: O(E + V) per iteration. Per-pass snapshot of
  canonical-HE list (D29) — no re-iteration of newly-edited topology.
- **v7e Hole-fill**: §3 DP is O(N³) per loop, capped by
  `max_hole_size` default 256 = 8 MB DP table. §4 Steiner refinement
  iterates with `max_refine_iterations` hard cap default 10.
- **v7f Manifoldness**: Phase A is O(E log E) via
  `crd::containers::HashMap` edge keying; Phase B is O(V) detection
  + O(K²) per bowtie for fan BFS (K = incident-triangle count, small
  in practice).
- **v7g Self-intersection**: brute-force O(n²) broadphase (D60 pin
  for v7g initial scope — phase doc references BVH broadphase as
  v7g-followon optimization). Each pair narrowphase is O(1).
- **v7h Taubin**: O((V + E) · n_iterations · 2) — 2 passes per
  iteration, each linear in topology. Boundary flag cached at entry
  (D68).

## Two-layer typed architecture (ADR-0078 §5 D34)

All algorithm bodies are raw `<MathScalar T>` for f32/f64 SIMD-
friendliness and inner-loop performance. Typed `Vec3<Length32>` /
`Vec3<Length64>` consumers will ride strip-compute-retag wrappers in
`*_typed.hpp` headers at first typed-surface consumer pull (deferred
to consumer-driven follow-ons per ADR-0078 §5; same pattern as
crd-geometry-primitives' `queries_typed.hpp` from v0d-2).

## Integration touch-points

| Consumer (current / future) | What it uses | Why |
|---|---|---|
| Cooker LOD pipeline | v7b qem_decimate + locked vertices | Generate 3-5 LODs per mesh with feature corners preserved |
| Asset import (glTF / OBJ) | v7f repair_manifoldness | Imports may have non-manifold edges from authoring tools |
| Photogrammetry / scanner cleanup | v7e fill_holes + v7h taubin_smooth + v7g remove_self_intersections | Scanned data has noise, holes, self-intersections |
| Cinematic / procgen | v7c loop_subdivide | Smooth low-poly assets to high-quality |
| FEA / collision-mesh prep | v7d isotropic_remesh + v7f repair_manifoldness | Uniform triangles + 2-manifold required by solvers |
| Eylem physics (collision-mesh) | v7f repair_manifoldness | Collision input must be 2-manifold |
| SDF bake | v7f repair_manifoldness | SDF needs closed manifold input |
| Procedural booleans | v7g remove_self_intersections + v7e fill_holes | Boolean ops produce self-intersections to be cleaned |

## Open follow-ons (post-close, not silently dropped)

- **v7b-attributes**: QEM extension with vertex-attribute preservation
  (Garland-Zhou 2005 — UV / colour / normal attributes carried
  through collapses via attribute quadrics).
- **v7c-creases**: Loop subdivision with sharp-crease tags (Hoppe
  1994 — per-edge sharpness causes the limit surface to crease
  along that edge).
- **v7d-cotan**: cotangent-weighted Laplacian for v7d tangential
  smoothing (Pinkall-Polthier / Desbrun). Standard quality
  refinement; ships when consumer requires it.
- **v7g-bvh-broadphase**: replace brute-force O(n²) with
  `DynamicBvh::find_overlapping_pairs`. Required for large meshes
  (> ~1k triangles).
- **v7g-bentley-ottmann**: per-triangle pre-pass to insert segment-
  segment intersection Steiner points BEFORE CDT, handling the
  "3+ triangles meet at a common edge" case where CDT currently
  returns `ConstraintsCrossing` and the algorithm degrades
  gracefully to keeping the original.
- **v7g-coplanar**: coplanar triangle-pair resolution. v7g currently
  defers to v7f; a proper handling would compute the polygon
  intersection and retriangulate via v6c.
- **v7h-cotan**: cotangent weighting for the umbrella operator.

## References

- Garland & Heckbert 1997, "Surface Simplification Using Quadric
  Error Metrics" (SIGGRAPH '97). v7b.
- Garland 1998, "Quadric-Based Polygonal Surface Simplification"
  (PhD thesis). v7b boundary preservation.
- Loop 1987, "Smooth Subdivision Surfaces Based on Triangles"
  (M.Sc. thesis). v7c.
- Botsch & Kobbelt 2004, "A Remeshing Approach to Multiresolution
  Modeling" (SGP). v7d.
- Liepa 2003, "Filling Holes in Meshes" (SGP). v7e.
- Edelsbrunner 2001, "Geometry and Topology for Mesh Generation"
  (link condition). v7a, v7b, v7e.
- Möller 1997, "A Fast Triangle-Triangle Intersection Test" (JGT).
  v7g.
- Hoppe et al. 1993, "Mesh Optimization" (SIGGRAPH '93) — inversion
  prevention. v7b, v7d.
- Taubin 1995, "A Signal Processing Approach to Fair Surface Design"
  (SIGGRAPH '95). v7h.
- Shewchuk 1997, "Adaptive Precision Floating-Point Arithmetic and
  Fast Robust Geometric Predicates" — `orient3d`, `incircle`. v7g,
  v7e.

See also: ADR-0076 (`crd-geometry` substrate architecture) §22
amendment for the locked v7 cluster design decisions D1-D72; per-
slice session logs `docs/sessions/2026-05-16-geometry-v7a-*.md` to
`docs/sessions/2026-05-17-geometry-v7h-*.md`.
