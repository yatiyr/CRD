# crd-geometry-viz

Debug-draw companion module for `crd-geometry`. Phase 3.1.7 v1j-a.

`crd-geometry-primitives`, `crd-geometry-bvh`, and the future
`crd-geometry-{convex,mesh,spatial,...}` substrates **never** link `crd-draw`
— a headless / cooker / DAW / scientific-computing build consumes the
geometry substrate without pulling the GPU debug-draw layer. `crd-geometry-viz`
is the bridge that knows about BOTH: it takes `crd::geometry::primitives::*`
types and `crd::geometry::bvh::*` trees and emits records into a
`crd::draw::RenderBuffer`. Consumers (the sandbox today, the future editor)
link it explicitly when they want geometry visualised. ADR-0066 §13 plug-in
pattern; mirrors `crd-eylem-viz`.

Module: `engine/geometry-viz/`, target `crd-geometry-viz`, namespace
`crd::geometry::viz`. Depends on `crd-core` + `crd-math` + `crd-containers` +
`crd-geometry-primitives` + `crd-geometry-bvh` + `crd-draw`.

## Status

| Slice | Scope | State |
|---|---|---|
| v1j-a | Substrate. Headers `primitives.hpp` / `queries.hpp` / `bvh.hpp` + impls. Overloaded `draw(RenderBuffer&, Shape, ...)` for AABB3 / OBB3 / Sphere / Capsule3 / Cylinder3 / Plane (patch) / Triangle3 / Tetrahedron / Frustum / Ray3 / Segment3 / Line3 — forwards to existing `crd::draw::*_to` helpers. `draw_ray_hit` / `draw_closest_point` / `draw_normals` for query results. `draw_bvh(BvhTree | Bvh4Tree | DynamicBvh)` walks every node + emits depth-coloured AABBs (8-entry palette, cycles `depth % 8`). `draw_overlap_pairs_with(DynamicBvh, ud→pos)` template emits one line per overlapping leaf pair. `draw_frustum_cull(Frustum, BvhTree, prims)` two-colour kept-vs-culled per-prim. 26 cases / 44 assertions on `crd-geometry-viz-tests`. | ✅ 2026-05-13 |
| v1j-b | Sandbox integration. Scene selector (ImGui dropdown) switching between the existing eylem physics demo and a new geometry-showcase scene. Showcase has four sub-modes: primitive viewer / query showcase / BVH viewer / SDF heatmap. | ⏳ next |

## What you get today (v1j-a)

### `primitives.hpp`

Overloaded `draw(RenderBuffer&, const Shape&, color, ...)` for every concrete
primitive. The geometry type names match `crd::geometry::primitives::*`
(`AABB3<f32>`, `Sphere<f32>`, `Capsule3<f32>`, `Triangle3<f32>`,
`Tetrahedron<f32>`, `Frustum<f32>`, etc.). Forwards to the existing
`crd::draw::*_to` family — `aabb_wire_to`, `box_wire_to`, `sphere_wire_to`,
`capsule_wire_to`, etc. — so the call-site reads as

```cpp
crd::geometry::viz::draw(buf, my_aabb);
crd::geometry::viz::draw(buf, my_sphere, crd::draw::kRed);
crd::geometry::viz::draw(buf, my_capsule);
```

Plane and Line are infinite by construction; the adapter emits a finite
patch / segment of caller-controlled extent so something visible reaches
the screen. Frustum's 8 corners are reconstructed from the 6 planes by
3-plane intersection (closed-form `(n2×n3)·(...)` solver) — works for any
finite-aspect frustum the projection-matrix routines emit.

### `queries.hpp`

- `draw_ray_hit(buf, ray, t, normal_dir, normal_length, ...)` — ray segment
  from origin to hit + 3-axis cross at the hit point + optional normal arrow.
- `draw_closest_point(buf, query, closest, ...)` — segment from query to
  closest + 3-axis cross at query + point at closest.
- `draw_normals(buf, points, normals, hair_length, ...)` — one arrow per
  (point, normal) pair. For mesh-normal visualisation in `-mesh` v4 +.

### `bvh.hpp`

- `depth_color(u32 depth)` — 8-entry HSV-around-the-wheel palette indexed by
  `depth % 8`. Hue-distinct values, avoiding near-greys so contrast survives
  the engine's neutral debug clear.
- `draw_bvh(buf, BvhTree, prims, depth_limit=0)` — recursive walk of every
  node; each emits its 12-edge AABB wireframe coloured by depth.
  `depth_limit > 0` caps traversal at that depth.
- `draw_bvh(buf, Bvh4Tree, prims, depth_limit=0)` — same shape for the
  4-wide collapse: per node, emits the node bounds and each child's bounds
  (the child bounds-cache lets us colour by depth without a second walk).
- `draw_bvh_bounds(buf, DynamicBvh)` — emits only the tree's outer-union
  AABB (the DynamicBvh's public API doesn't expose per-leaf iteration with
  internal-node bounds today; a public `for_each_leaf` walker is debt for
  the editor slice). Named `_bounds` rather than overloading `draw_bvh`
  because what it does is materially different from the per-node walks on
  the static trees.
- `draw_overlap_pairs_with(buf, DynamicBvh, ud_to_pos, ...)` — emits one
  line per overlapping leaf pair via `find_overlapping_pairs`, connecting
  the centroids the caller's lambda returns for each `user_data`.
- `draw_overlap_pairs(buf, DynamicBvh, ...)` — deliberately a no-op. The
  tree has no centroid table — a position-less call has nothing meaningful
  to draw. The function exists so callers can write the call symmetrically
  with the other `viz::*` overloads, get back nothing, and reach for
  `draw_overlap_pairs_with` (above) to supply positions.
- `draw_frustum_cull(buf, Frustum, BvhTree, prims, kept_color, culled_color,
  ...)` — per-prim AABB wireframe in kept/culled colour. Conservative: a
  partial-overlap prim counts as kept (matches the "would this be drawn?"
  visual intent).

### Pinned design decisions

- **Adapters forward, they don't reimplement.** Each primitive draw is a
  thin overload that calls `crd::draw::aabb_wire_to` / etc. with the right
  argument-shape conversion. No new geometry generation here.
- **Plane and Line emit finite patches**, sized by caller — infinity isn't
  drawable.
- **DynamicBvh's per-leaf walk is not yet a public API** on the tree, so
  the depth-coloured walk for DynamicBvh shows only root bounds. Per-leaf
  visualisation works via `draw_overlap_pairs_with` (callback-supplied
  positions) or by the caller maintaining its own user_data→fat_aabb
  mapping. A public `DynamicBvh::for_each_leaf(Fn)` is geometry-bvh debt;
  the editor slice will need it.
- **Test scope is "primitive emit count, not rendered output."** Every
  test checks that a known shape adapter emits the expected `line_count()`
  / `point_count()` after one call — a regression like "the OBB rotation
  matrix is wrong" surfaces here as the same edge count but visually wrong
  output, which is what v1j-b's sandbox eye-test catches.
