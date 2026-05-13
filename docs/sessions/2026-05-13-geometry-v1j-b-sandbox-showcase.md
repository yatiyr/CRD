# Session — 2026-05-13 — Phase 3.1.7 v1j-b: sandbox integration — scene selector + four-mode geometry showcase **(closes v1)**

## Goal

Final v1 slice. Wire the v1j-a `crd-geometry-viz` substrate into the sandbox as a runnable, eye-test-able validation surface for the whole `crd-geometry-*` substrate (v0 / v1a..v1h primitives + SDFs + intersection corpus + BVH structures + closest-point + shapecast + broadphase pairs). Scene-selector dropdown switches between the existing eylem physics demo (preserved) and a new geometry-showcase scene with four ImGui-selectable sub-modes — primitive viewer, query showcase, BVH viewer, SDF heatmap.

After this slice, the v1 cluster (v1a → v1j-b) is closed and the full 17-config `scripts/full-sweep.ps1` (deferred since v1c per user) is due before v2 (`-convex`).

## What we built / changed

### `sandbox/src/geometry_showcase.{hpp,cpp}` — new files

Kept out of the bloated `sandbox_layer.cpp` (1553 LOC and counting) to maintain readable diffs as the showcase grows in later slices. Pure data + pure functions:

- `enum class SandboxScene { Physics, GeometryViz }`
- `struct GeometryShowcaseState` — per-mode parameter blob. `GeometryShowcaseMode { PrimitiveViewer, QueryShowcase, BvhViewer, SdfHeatmap }` plus per-mode state (selected primitive type, ImGui slider values, query points, BVH parameters, SDF kind, grid resolution). All POD-ish; no allocator inside.
- Public surface:
  ```cpp
  void render_geometry_showcase(GeometryShowcaseState&, crd::draw::RenderBuffer&, crd::memory::IAllocator&);
  void draw_geometry_showcase_imgui(GeometryShowcaseState&);
  ```
  Both functions take the state by reference, emit/mutate, retain no pointers. Allocator is the layer-owned TLSF used for the BVH/DynamicBvh scratch.

**Per-mode design:**

1. **Primitive viewer** — dropdown selects one of 11 primitive types (Sphere / AABB / OBB / Capsule3 / Cylinder3 / Plane / Triangle3 / Tetrahedron / Frustum / Ray3 / Segment3). ImGui sliders drive the shape's parameters; `viz::draw(buf, shape)` emits the wireframe. A draggable query point + a `viz::draw_closest_point(buf, query, closest_point(shape, query))` overlay are shown by default for the shapes that have a closest-point overload (Sphere/AABB/OBB/Capsule/Plane/Triangle/Segment); the others (Cylinder/Tetrahedron) skip the overlay because no `closest_point` exists in `closest_point.hpp` v0b. The Frustum is built from FOV/aspect/near/far/position/look-direction sliders via a local `build_view_frustum` (inward-facing planes; near + far + 4 side planes constructed from the camera frame).
2. **Query showcase** — N=`qs_prim_count` random AABBs from a deterministic seed → `bvh_build` each frame. Backdrop emits every prim AABB in grey. ImGui radio picks one of 4 queries:
   - **Raycast**: ImGui ray origin + direction; `crd::geometry::raycast(tree, prims, ray)`; if hit, highlight the hit prim in red + `viz::draw_ray_hit(buf, ray, hit.t)`; else draw the full ray in yellow.
   - **Overlap**: ImGui min/max box; `crd::geometry::overlap(tree, prims, q, hits)`; matched prims drawn green; query box drawn orange.
   - **ClosestPoint**: ImGui query point; `crd::geometry::closest_point(tree, prims, q)`; closest prim cyan; `viz::draw_closest_point` segment.
   - **SphereCast**: ImGui center/dir/radius/tmax; `crd::geometry::cast_sphere(tree, prims, sphere, dir, tmax)`; if hit, sphere at impact position + ray-hit overlay; else swept end-sphere in yellow.
3. **BVH viewer** — N random AABBs (slider 4..800; **capped at 800 per advisor** to respect the 16ms frame budget — per-frame BVH rebuild + walk + emit). Tree-kind radio toggles BvhTree / Bvh4Tree / DynamicBvh: `viz::draw_bvh` emits depth-coloured AABBs for the static trees; `viz::draw_bvh_bounds` emits the outer-union AABB for the dynamic tree plus every leaf's fat AABB via `query(universe, …)`. Depth-limit slider (0 = full walk). For DynamicBvh: optional `viz::draw_overlap_pairs_with(…, ud→pos)` overlay using a centroids side-table the loop builds at insert time. Optional frustum-cull overlay using the same `build_view_frustum` helper + `viz::draw_frustum_cull`.
4. **SDF heatmap** — SDF type selector (Sphere / Box / RoundBox / Torus / Octahedron / Capsule / Cone / BoxFrame / Cylinder). Per-type parameter sliders. Grid resolution slider (4..48) → 3D grid of sample points. Each sample evaluates the v1h analytic `sd_*` function; output coloured via an HSV-style ramp (`sdf_color`): deep blue inside far, bright green at the surface, deep red outside far. Points beyond `sdf_max_distance` are skipped to keep the buffer manageable at high resolutions.

Around 870 LOC of pure showcase code. No tests live here (see the validation-responsibility section below).

### `sandbox/src/sandbox_layer.{hpp,cpp}` — minimal patch

Added two member fields to `SandboxLayer`:
- `SandboxScene m_scene = SandboxScene::Physics;` (default preserves the existing boot behaviour)
- `GeometryShowcaseState m_showcase{};`

Three insertion points in the layer:
- **`on_update`**: when `m_scene == GeometryViz` the eylem `step_fixed` path is skipped; the variable-rate `world->step` still runs so transform propagation + RenderUploadSystem continue working. After `m_draw_buffer.clear()`, when in GeometryViz mode, `render_geometry_showcase(m_showcase, m_draw_buffer, *m_eylem_alloc)` is called — the showcase shares the existing eylem TLSF heap (production-grade allocator already alive for the layer's lifetime).
- **`on_render`**: new "Scene" ImGui panel inserted after the "Sandbox" status panel. Combo box switches `m_scene`; when in GeometryViz mode, `draw_geometry_showcase_imgui(m_showcase)` renders the sub-mode controls.
- The existing physics-demo panels (Profile / Presets, Öbek demo, Asset Browser) remain unconditionally visible — the user can toggle scene + still see them. (Future refinement: hide physics-mode-specific panels when in GeometryViz, debt.)

Total sandbox_layer diff is ~50 LOC.

### `sandbox/CMakeLists.txt` — deps

Added `src/geometry_showcase.cpp` to the source list and linked `crd-geometry-primitives` + `crd-geometry-bvh` + `crd-geometry-viz`.

### Per-advisor adjustments

Advisor flagged three concerns after the initial sweep:

1. **The smoke test only exercises the boot path** — scene defaults to Physics, so the GeometryViz sub-modes never render under the automated 3-second smoke. **Documented explicitly** in the "Validation responsibility" section below.
2. **BVH viewer rebuilds the tree every frame** — at the original 2000-slider cap, 24 000 line emissions per frame (~12 ms) would blow the 16ms budget at N≈1500. **Slider capped at 800** (sub-10000 line emissions / frame, well under budget); rebuild-caching across frames filed as debt for a later slice. ImGui tooltip on the slider explains the cap.
3. **Frustum visualization needs visual check** — the `build_view_frustum` helper constructs side planes through `pos` (so the apex is at `pos` and the visible frustum is the truncated pyramid from `pos + fwd*near` to `pos + fwd*far`). Math is right in principle; flagged for the manual eye-test pass.

## Decisions made

- **Sandbox stays a single layer; scene selection is an enum + branching**, not a layer-stack swap or `SandboxLayer` refactor into a base + scenes. The diff is additive and reversible; if the showcase grows into something needing its own layer, refactor later. Matches the project's pattern of "add incrementally, refactor when the seams hurt."
- **Showcase lives in its own TU** (`geometry_showcase.cpp`) — `sandbox_layer.cpp` is already 1500+ LOC; adding another ~870 would tip it past 2400. Separating the showcase keeps merge conflicts away from the existing physics + profile + asset code, and lets future v1j-c/d/etc. extend the showcase without touching the layer.
- **Reuse the eylem TLSF for the showcase's scratch** — the allocator is already alive and named (`"eylem-tlsf"`); the showcase needs ~MB-scale scratch for the BVH viewer's N=800 prims + tree, well within the eylem heap's 16MB budget. Named-allocator convention preserved.
- **Per-frame BVH rebuild is acceptable for the showcase but documented** — sandbox is the validation harness, not a perf target. Real broadphase consumers (eylem v1c) get the caller-owned-scratch scratch overload from the v1i-c debt-payment pass; the showcase rebuilds because it has to react to slider changes anyway. The N≤800 cap keeps it interactive.
- **Default scene = Physics** so the existing v1b-e eylem demo's boot behaviour is unchanged. A returning user sees the falling bodies first, then discovers the scene dropdown.
- **The 11-primitive ShowcasePrimitive enum matches `crd::geometry::primitives` exactly** — Sphere, AABB, OBB, Capsule3, Cylinder3, Plane, Triangle3, Tetrahedron, Frustum, Ray3, Segment3. Every primitive that has a `viz::draw` overload is exercised; the closest-point overlay activates for the 7 that have a `gprim::closest_point` overload. Cylinder3 and Tetrahedron deliberately skip the overlay (no closest_point in `closest_point.hpp` v0b — would be `-mesh` v4+ territory for the Tetrahedron, and Cylinder3's closest_point is doable but not present yet; debt).
- **Frustum-cull overlay reuses the same `build_view_frustum` helper** so the BVH viewer's cull frustum and the Primitive viewer's standalone Frustum are constructed the same way — visual consistency is testable.

## Validation responsibility (advisor pin)

**Automated coverage of the showcase render paths is zero.** The smoke test (`crd-sandbox.exe --smoke-test 3`) boots the sandbox with `SandboxScene::Physics` and runs 528+ frames at ~176 fps without crashing, confirming:
- The new `crd-geometry-viz` libraries link cleanly into the sandbox.
- The new `m_scene` / `m_showcase` members default-initialize cleanly.
- The new "Scene" ImGui panel renders without crashing in its initial Physics-mode state.
- Existing Physics-mode behaviour is preserved (3 bodies, fixed-step integration).

**It does NOT confirm any of the four GeometryViz sub-modes execute without crashing**, because the user must click into GeometryViz mode for the showcase code path to run. The slice's validation responsibility therefore rests on the user manually clicking through:

1. Scene dropdown → GeometryViz.
2. For each `GeometryShowcaseMode` (PrimitiveViewer / QueryShowcase / BvhViewer / SdfHeatmap):
   - Switch to that mode via the Showcase mode dropdown.
   - For PrimitiveViewer: cycle every primitive type (11 options) and visually confirm the wireframe renders + (for the 7 with overlays) the closest-point segment lands on the primitive surface.
   - For QueryShowcase: cycle every query mode (4 options) and confirm the input + result render sensibly; drag sliders for raycast / overlap / closest_point / sphere-cast and confirm the result tracks.
   - For BvhViewer: cycle every tree kind (3 options); slide N up to 800; toggle depth limit; for DynamicBvh enable overlap-pairs overlay; enable frustum-cull overlay.
   - For SdfHeatmap: cycle every SDF kind (9 options); slide grid resolution + parameters.

**The user's "completely solid" bar is reached only after that walkthrough.** A future test slice (debt: `tests/geometry-viz/test_showcase.cpp`) can smoke-run each `render_geometry_showcase(state, buf, alloc)` call with each mode-state combination against a TLSF buffer and assert `line_count() > 0` / `point_count() > 0` — automated reach into the path the smoke doesn't cover.

## Files touched

- New: `sandbox/src/geometry_showcase.hpp` + `sandbox/src/geometry_showcase.cpp` (~870 LOC), this session log.
- Modified: `sandbox/src/sandbox_layer.hpp` (added `#include "geometry_showcase.hpp"` + the two member fields), `sandbox/src/sandbox_layer.cpp` (conditional eylem step + the new Scene ImGui panel + the showcase render call), `sandbox/CMakeLists.txt` (added source file + 3 link-libs), docs.

## Tests / verification

Per the in-flight `-bvh` directive (full 17-config `scripts/full-sweep.ps1` deferred since v1c — now **finally due** with this slice closing v1):

- **win-debug**: full build ✅; ctest **1302/1302 PASS** (was 1301 after v1j-a — +1 from the test_bvh test_bvh4 SIMD test added during v1i-c debt + no v1j-b unit tests by design); sandbox smoke **529 frames @ 176 fps clean**.
- **win-asan**: full build ✅; ctest **1302/1302 PASS** (~66 s); sandbox smoke **528 frames @ 175.8 fps clean** — no use-after-free / heap-overflow detected by ASan on the new showcase code paths during the boot-then-Physics-default test.
- **win-shipping**: full build ✅ (full LTO, MSVC); ctest **1297/1297 PASS**; sandbox smoke **528 frames @ 175.9 fps clean**.
- **win-tidy**: build ✅; zero new warnings on `geometry_showcase.{hpp,cpp}`. Pre-existing `readability-inconsistent-ifelse-braces` notes in `sandbox_layer.cpp` (lines 1411 + 1569) are unchanged — from old code, not v1j-b's diff.
- **CI guards**: all four green (`crd-no-std-math-check` / `crd-no-std-sort-check` / `crd-no-non-ascii-test-names` / `crd-simd-emission-check`).

**Per-config sandbox-smoke note**: the smoke confirms the boot path. The four showcase sub-modes' code paths are NOT automated — see "Validation responsibility" above. The user-side eye-test walkthrough is the real validation.

## Post-ship bug-fix pass — second round (same-day, user-driven)

After the first bug-fix pass landed (eylem bleed-through, SDF invisibility, Vulkan validation noise) the user ran the sandbox again and found more bleed:

1. **The historic v1a-draw d0d demo block was unconditionally firing in `render_scene`** (`sandbox_layer.cpp:1502-1536`) — `axis_triad`, `box_wire+solid`, `sphere_wire+solid`, `capsule_wire+solid`, `arrow`, `cross_3d`, `arc`. These had no scene gate and bled into both GeometryViz and Physics modes. The user correctly identified these as crd-draw API showcase demos that should live in their own scene.
2. **Line thickness wasn't adjustable via ImGui** — some `viz::draw` overloads accept `width_px` but the showcase never threaded a value through; they all used the default 1.0F.

**Fixes:**

- **Third scene `SandboxScene::DrawShowcase`** — new enum value (value 2 alongside Physics/GeometryViz). The d0d demo block moved out of `render_scene` into a new pure function `render_draw_showcase(state, buf)` in `geometry_showcase.cpp`. The `render_scene` call site is gone — only the overlay-pass plumbing remains. The on_update scene-conditional clear-after-step now triggers for any non-Physics scene (so DrawShowcase also gets a clean canvas free of eylem DebugVizSystem stamps). The ImGui Scene dropdown gains a third option labeled "Draw API showcase" with its own help text.
- **Line-width slider** — new `GeometryShowcaseState::line_width = 2.0F` member, ImGui `DragFloat` slider 0.5..8.0 shown for both GeometryViz and DrawShowcase scenes. The value is threaded through **every** `viz::draw(...)` / `viz::draw_*(...)` call in `render_primitive_viewer` (all 11 primitive cases — Sphere needs `lat`/`lon` args before width, Capsule/Cylinder need `segments`, Plane needs `anchor`/`size`/`grid_div` — each call now passes them explicitly so `width_px` lands), `render_query_showcase` (the 4 query modes + the backdrop sweep), `render_bvh_viewer` (the static-tree walks, dynamic-tree query loop, overlap-pair lambda, frustum-cull both-trees variant), `render_sdf_heatmap`'s `cross_3d_to`, and `render_draw_showcase`'s every `crd::draw::*` wrapper. Added `show_origin_triad` + `origin_triad_size` toggles to control the per-scene origin triad (also routed through `line_width`).
- **First-round fixes retained**: clear-after-step (now for all non-Physics scenes), ForwardRenderPath Renderable skip (still gated on `m_scene == Physics` — both showcases get a mesh-free canvas), `cross_3d_to` for the SDF heatmap.

Re-verified post-second-round-fix on all four configs:
- win-debug build + smoke **529 frames @ 176.2 fps** clean
- win-asan build + smoke **529 frames @ 176.1 fps** clean (upstream ImGui `nonCoherentAtomSize` warning still surfaces during ImGui's initial texture upload — documented as upstream debt, pre-existing in all sandbox runs, not caused by any v1j-b code path)
- win-shipping build + smoke **529 frames @ 176.1 fps** clean

User-side validation walkthrough: 3 scenes now exist (Physics / Geometry / Draw API). Switching between them shows ONLY that scene's primitives — no bleed across scenes. The `line width (px)` slider in both showcase scenes affects every line emission immediately (most visible on the AABB / OBB / Frustum cases where lines dominate; sphere/capsule wires are denser so the effect is subtler).

## Post-ship bug-fix pass — first round (same-day, user-driven)

After the initial v1j-b ship the user manually ran the sandbox and reported three concrete issues. All addressed same-session:

1. **Physics scene bled through into GeometryViz mode** — `world->step` runs the PostRender `DebugVizSystem`, which stamps the eylem demo's RGB triads + body wireframes + velocity arrows into `m_draw_buffer` *before* `render_geometry_showcase` appends its own primitives. **Fix:** in GeometryViz mode, re-clear `m_draw_buffer` after `world->step` but before the showcase emit; the DebugVizSystem's stamps are dropped, the showcase has the canvas to itself. Also added: skip `ForwardRenderPath`'s Renderable submission entirely in GeometryViz mode (procedural assets + öbek demo + asset-browser selection stay hidden) — one-line `&& m_scene == SandboxScene::Physics` guard on the submit loop in `render_scene`.
2. **SDF heatmap was invisible** — the implementation called `add_point_to`, but the `crd-draw` renderer **has no point pipeline** today (line + triangle pipelines only). Every `DebugPoint` record in the buffer was silently dropped at draw time. **Fix:** replace `add_point_to` with `cross_3d_to` (a 3-axis cross emits 3 lines per sample — visible at any camera angle). Defaults retuned for the 4096-line per-frame draw budget: `sdf_grid_res = 12` (was 16; 12³ = 1728 samples × 3 = 5184 lines worst case, the `sdf_max_distance` band-pass typically culls 60-70% so steady state is ~1500-2000 lines), `sdf_grid_extent = 2.0` (was 3.0), `sdf_max_distance = 1.5` (was 2.0). Cross size scales with the cell step (30%) so neighbour crosses don't overlap visually at any resolution.
3. **Vulkan validation warning `vkFlushMappedMemoryRanges()` size not aligned to `nonCoherentAtomSize`** — **upstream ImGui bug** in `imgui_impl_vulkan.cpp:815`. The texture-upload path uses `range[0].size = upload_size` (raw upload size, not rounded to atom boundary). ImGui dense widget trees → more font-glyph rasterisation → triggers the unaligned path. **Not caused by v1j-b** — exists in physics mode too, the showcase's denser ImGui tree just reproduces it more reliably. Documented as upstream debt in `docs/debt.md` with three fix paths (post-fetch CMake patch / upstream PR / VUID suppression). Validation-warning only, no crash / no rendering bug.

Re-verified all four configs post-fix:
- win-debug build + smoke **530 frames @ 176.4 fps** clean
- win-asan build + smoke **528 frames @ 175.8 fps** clean
- win-shipping build + smoke **530 frames @ 176.6 fps** clean

User-side validation (the actual "completely solid" bar — see Validation responsibility above) was the catalyst for this pass and remains the verification path for any further GeometryViz polish.

## Debt added this slice

1. **`tests/geometry-viz/test_showcase.cpp`** — automated smoke of each showcase mode-state combination against a TLSF buffer (assert `line_count() > 0` / `point_count() > 0` per mode). Closes the zero-automation gap on the showcase render paths. ~150 LOC, defer to a v1j-c or after-v1 slice.
2. **Per-frame BVH rebuild caching in the BVH viewer** — today every frame rebuilds the tree. Caching across frames keyed on (N, seed, world_size, max_box_size) would let the slider cap go past 800. ~80 LOC. Defer to a later viz polish slice.
3. **Closest-point overlay for Cylinder3 + Tetrahedron** — neither has a `gprim::closest_point` overload in `closest_point.hpp` v0b. Cylinder3's is straightforward (Ericson §5.1.6-ish); Tetrahedron's is mesh-territory. Filed as `crd-geometry-primitives` debt.

## Next session starts with

- **v1 cluster ✅ CLOSED** — Phase 3.1.7 v1a (BvhTree) → v1b (refit) → v1c (DynamicBvh) → v1d (Bvh4Tree) → v1e (closest-point) → v1f (parallel build) → v1g (Vec4f BVH4 SIMD) → v1h (primitives hardening) → v1i-a/b/c (unified queries + shapecast + broadphase pairs) → v1i-c debt payment (scratch + Vec4f shapecast + AABB-equiv test) → v1j-a (`crd-geometry-viz` substrate) → **v1j-b (this slice — sandbox showcase)**. 14 sub-slices, ~6 development days. Substrate is feature-complete for everything the v1 phase doc promised.
- **Full 17-config `scripts/full-sweep.ps1`** is now due (deferred since v1c per user — CI was surprisingly green so the sweep amortised once at the end). User-triggered: `scripts/full-sweep.ps1` on the user's command. Goes Win × 9 configs (debug / relwithdebinfo / release / asan / clang-cl / debug-scalar / debug-sse2 / shipping / clang-cl-shipping + tidy build-only) + Linux × 7 (gcc-debug / relwithdebinfo / release / asan / debug-scalar / debug-sse2 / shipping). Expected ~20-30 min wall time.
- **Then v2 (`-convex`)** — GJK distance + GJK boolean + EPA penetration + SAT box-pair fast path + `ConvexHullView` queries + GJK-based convex shapecast (the general moving-convex-vs-static-convex TOI that v1i-b deferred to). ~3300 LOC engine + ~1000 tests / ~2 weeks. ADR-0076 §1 sub-module 3.
- **Open debt closing v1** (carried forward): the three above + the existing `DynamicBvh::for_each_leaf(Fn)` walker from v1j-a + `Vec4f` shapecast kernel was paid in v1i-c+. All filed in `docs/debt.md`.
