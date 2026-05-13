# Session — 2026-05-13 — Phase 3.1.7 v1j-a: `crd-geometry-viz` companion module substrate

## Goal

First of two v1j sub-slices (split at the user's request after they expanded scope to include a sandbox demo with a scene selector + four showcase modes). v1j-a ships the **substrate** — the new `crd-geometry-viz` module — and tests that exercise every adapter. v1j-b lands the sandbox integration.

The substrate's job is dependency inversion. `crd-geometry-primitives`, `crd-geometry-bvh`, and the future `crd-geometry-{convex,mesh,spatial,...}` modules **never** link `crd-draw` — a headless / cooker / DAW / scientific-computing build consumes the geometry substrate without pulling the GPU debug-draw layer. The bridge that knows about BOTH lives here (ADR-0066 §13 plug-in pattern; mirrors `crd-eylem-viz`).

## What we built / changed

### `engine/geometry-viz/` (new module)

Five files: `CMakeLists.txt`, umbrella `crd/geometry/viz/viz.hpp`, plus three layered headers + impls:

- **`primitives.hpp` + `src/primitives.cpp`** — overloaded `draw(RenderBuffer&, const Shape&, ...)` for every concrete primitive in `crd::geometry::primitives`: `AABB3` / `OBB3` / `Sphere` / `Capsule3` / `Cylinder3` / `Plane` (finite-patch overload) / `Triangle3` / `Tetrahedron` / `Frustum` / `Ray3` / `Segment3` / `Line3`. Most overloads are inline header forwards to existing `crd::draw::*_to` (e.g. `viz::draw(buf, AABB3)` → `aabb_wire_to`); the two `.cpp`-side helpers are the `Plane` finite-patch grid emitter (builds a tangent frame `(u, v) ⊥ normal`, draws a `grid_divisions × grid_divisions` square patch projected onto the plane through `anchor`) and the `Frustum` 8-corner reconstruction (3-plane intersection via the closed-form `(d1·(n2×n3) + d2·(n3×n1) + d3·(n1×n2)) / det(n1, n2, n3)` solver; emits the 12 frustum edges with `intersect_three_planes` returning `false` on near-parallel planes).
- **`queries.hpp` + `src/queries.cpp`** — `draw_ray_hit(buf, ray, t, normal_dir, ...)` (ray segment to hit + 3-axis cross at the hit point + optional normal arrow), `draw_closest_point(buf, query, closest, ...)` (segment + endpoint markers), `draw_normals(buf, points, normals, hair_length, ...)` (one arrow per pair; for mesh-normal visualisation in `-mesh` v4+).
- **`bvh.hpp` + `src/bvh.cpp`** — `depth_color(u32 depth)` 8-entry palette (hue-distinct, avoiding near-greys) indexed by `depth % 8`; `draw_bvh(BvhTree, prims, depth_limit)` recursive walk emitting depth-coloured AABBs per node; `draw_bvh(Bvh4Tree, prims, depth_limit)` analogous for the 4-wide topology; `draw_bvh(DynamicBvh)` (root-bounds-only today — DynamicBvh's public API doesn't expose per-leaf walk with internal bounds; debt for editor slice); `draw_overlap_pairs_with(DynamicBvh, ud→pos lambda)` template emitting one line per overlapping leaf pair via `find_overlapping_pairs`; `draw_frustum_cull(Frustum, BvhTree, prims, kept_color, culled_color)` per-prim two-colour cull visualisation using AABB-positive-vertex plane test.

Dependencies (PUBLIC): `crd-core`, `crd-math`, `crd-containers`, `crd-geometry-primitives`, `crd-geometry-bvh`, `crd-draw`. No `crd-scene` / `crd-eylem` / `crd-renderer` — pure geometry-to-draw adapter.

### `tests/geometry-viz/` (new)

Three test files / 26 cases / 44 assertions on `crd-geometry-viz-tests`:

- **`test_primitives.cpp`** — exact line-count pins for AABB (12 edges), OBB (12), Triangle (3), Tetrahedron (6), Plane patch (`(divs+1) × 2` lines for the grid), Ray / Segment / Line (1 each), Frustum (12 — exactly when all 8 corners reconstruct). Non-zero-line-count pins for Sphere / Capsule / Cylinder (resolution-dependent counts). Plus the degenerate-input pin: `Plane(zero-normal)` emits 0 lines (no UB).
- **`test_queries.cpp`** — `draw_ray_hit` emits 4 lines (ray + 3-axis cross) without normal, more with normal; `draw_closest_point` emits 4 lines + 1 point; `draw_normals` emits ≥ 1 arrow per (point, normal) pair; empty input emits nothing.
- **`test_bvh.cpp`** — `depth_color` cycles every 8 depths; `draw_bvh(BvhTree)` emits `node_count × 12` edges; `depth_limit=1` caps the count; `draw_bvh(Bvh4Tree)` emits a non-zero count; `draw_bvh(empty DynamicBvh)` emits 0; `draw_overlap_pairs_with` over a 3-leaf scene with one overlapping pair emits exactly 1 line; `draw_frustum_cull` over 2 prims (1 inside / 1 outside a unit-cube frustum) emits 2 wireframes.

## Decisions made

- **Companion-module pattern, mirroring `crd-eylem-viz`.** `crd-geometry` substrate stays consumable from headless / cooker / DAW / scientific-computing builds. The bridge knowing about both `crd-geometry-*` and `crd-draw` lives in a separate, opt-in module.
- **Adapters forward, they don't reimplement.** Each primitive overload is a thin inline call into the existing `crd::draw::aabb_wire_to` / `sphere_wire_to` / etc. The value-add is the type adaptation — caller writes `viz::draw(buf, my_aabb)` instead of `aabb_wire_to(buf, my_aabb.min, my_aabb.max, ...)`. Two adapters live in `.cpp` (Plane patch's tangent-frame math + Frustum's 3-plane corner reconstruction) — those don't fit the inline forwarding pattern.
- **Overloaded `draw` for primitives, named functions for queries / BVH.** `draw(buf, Shape)` reads naturally for a primitive (the shape disambiguates). For query results and BVH traversals the function name carries the action (`draw_ray_hit`, `draw_bvh`, `draw_overlap_pairs_with`); overloading them on the geometry type would obscure the intent.
- **`Frustum` 8-corner reconstruction in `.cpp` not `.hpp`.** The 3-plane intersection has a non-trivial branch on plane non-parallelism. Inline'd it would duplicate the helpers everywhere; out-of-line keeps the substrate header clean.
- **`DynamicBvh` per-leaf walk renamed to `draw_bvh_bounds`** — the function the substrate ships emits only the tree's outer-union AABB; calling it `draw_bvh` while it draws one box would be misleading (advisor flagged it during the close — the `draw_bvh(BvhTree)` form draws per-node, so a same-named overload on `DynamicBvh` reasonably looks like it does too). Renamed to `draw_bvh_bounds(DynamicBvh)` so the name matches the behaviour; the previous no-op `tree.query(universe, [](u32){})` dead-code call inside the impl is removed. Per-leaf visualisation goes through `draw_overlap_pairs_with` (callback-supplied positions). A public leaf walker is `crd-geometry-bvh` debt; the editor slice (Phase 7) will need it. Not added today (no current consumer is blocked).
- **`draw_overlap_pairs(DynamicBvh)` convenience overload emits nothing** (advisor flag — the earlier v1j-a draft emitted a "count-indicator" line whose length scaled with the pair count; visually that looked like a misalignment, not an indicator, and a future-you would file a "what's this stray line?" bug). The function exists so callers can write the call symmetrically with the other `viz::*` overloads, get back nothing, and reach for `draw_overlap_pairs_with(buf, tree, ud→pos)` to actually see lines. Test asserts the no-op behaviour.
- **Test scope = "primitive emit counts, not rendered output."** Every test pins `line_count()` / `point_count()` after one call. A bug like "OBB rotation matrix transposed" produces the right edge count but visually wrong output — that's v1j-b's sandbox eye-test territory. Substrate tests catch regressions like "AABB lost a face" or "Tetrahedron emits 12 edges instead of 6."

## Files touched

- New module `engine/geometry-viz/` — `CMakeLists.txt`, `include/crd/geometry/viz/{viz,primitives,queries,bvh}.hpp`, `src/{primitives,queries,bvh}.cpp`.
- New tests `tests/geometry-viz/` — `CMakeLists.txt`, `test_{primitives,queries,bvh}.cpp`.
- `CMakeLists.txt` (root) — `add_subdirectory(engine/geometry-viz)` after `engine/draw` and `engine/eylem-viz`.
- `tests/CMakeLists.txt` — `add_subdirectory(geometry-viz)`.
- New docs `docs/systems/geometry-viz.md`, this session log.

## Tests / verification

Per the in-flight `-bvh` directive (full 17-config sweep deferred to v1 cluster close — **now after v1j**, which is one more sub-slice away):

- **win-debug**: full build ✅; ctest **1301/1301 PASS** (was 1275 after v1i-c debt — +26 new TEST_CASEs in `tests/geometry-viz/`).
- **win-asan**: full build ✅; ctest **1301/1301 PASS** (~66 s).
- **win-shipping**: full build ✅ (full LTO, MSVC); ctest **1296/1296 PASS**.
- **win-tidy**: build ✅; zero new warnings on the v1j-a files. Pre-existing `bugprone-unchecked-optional-access` on `test_validation.cpp:178` unchanged (Catch2 `REQUIRE(opt.has_value())` pattern).
- **CI guards**: all four green (`crd-no-std-math-check` / `crd-no-std-sort-check` / `crd-simd-emission-check` / `crd-no-non-ascii-test-names` — the last one **actually verifies** now after the v1i-c fix; would have caught the em-dash / `²` / `×` regressions had it existed in this state).

`crd-geometry-viz-tests`: **27 cases / 46 assertions** on win-debug (was 26/44 before the advisor-flagged rename + no-op fix — `draw_bvh(DynamicBvh)` → `draw_bvh_bounds`; new test for the now-explicit no-op `draw_overlap_pairs` convenience).

## Next session starts with

- **Phase 3.1.7 v1j-b** — sandbox integration. ImGui dropdown selecting between the existing **eylem physics demo** (3 falling bodies; preserved as-is) and a new **geometry showcase** scene. The showcase has four ImGui-selectable sub-modes:
  1. **Primitive viewer** — dropdown of primitive types (Sphere / AABB / OBB / Capsule / Cylinder / Triangle / Tetrahedron / Frustum) with sliders for params; renders the wireframe + a closest-point segment from a draggable query point. Exercises every `viz::draw` overload.
  2. **Query showcase** — fixed small scene of ~5 primitives; ImGui controls pick a query (raycast / overlap / closest-point / shapecast); renders input + result via the v1j-a adapters. Exercises `draw_ray_hit` / `draw_closest_point` / shapecast paths.
  3. **BVH viewer** — N random AABBs (slider for N); render node AABBs colour-keyed by depth; toggle BvhTree / Bvh4Tree / DynamicBvh; for DynamicBvh insert/remove buttons + `draw_overlap_pairs_with`; frustum-cull toggle (`draw_frustum_cull`).
  4. **SDF heatmap** — sample the v1h `sd_*` analytic SDFs at a grid; render points tinted by distance.
- Estimated ~1500 LOC sandbox/UI work over ~3 days. The substrate (this slice) is feature-complete; v1j-b is integration and demo.
- **After v1j-b**: v1 cluster closes (v1a → v1j-b). **Full 17-config `scripts/full-sweep.ps1` deferred to that point** runs. Then on to **v2** (`-convex`: GJK + EPA + SAT + Quickhull, including GJK-cast).
- **Debt added this slice (carried forward):** `DynamicBvh::for_each_leaf(Fn)` public walker so `draw_bvh(DynamicBvh)` can show per-leaf bounds instead of just the root. Not blocking today (no current consumer needs it); editor Phase 7 will.
