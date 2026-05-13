# 2026-05-13 — Phase 3.1.7 v1 debt-payment pass

**Closes:** Phase 3.1.7 v1 cluster debt-payment for the four active-debt items filed against v1j-b.
**Successor:** Phase 3.1.7 v2 (`-convex`) — GJK distance + GJK boolean + EPA penetration + SAT + Quickhull + `ConvexHullView` queries + GJK-cast.

## What shipped

Four `docs/debt.md` entries paid same-day, in this order:

1. **#4 — ImGui `vkFlushMappedMemoryRanges` nonCoherentAtomSize VUID** (`CMakeLists.txt` post-fetch patch).
2. **#3 — `closest_point` overloads for `Cylinder3` + `Tetrahedron`** (`engine/geometry-primitives/include/crd/geometry/primitives/closest_point.hpp`).
3. **#2 — Per-frame BVH rebuild caching in the sandbox BVH viewer** (`BvhViewerCache` in `sandbox/src/geometry_showcase.{hpp,cpp}` + `sandbox/src/sandbox_layer.{hpp,cpp}`).
4. **#1 — Automated coverage of the showcase render paths** (`tests/sandbox/` new target `crd-sandbox-showcase-tests`).

### Debt #4 — ImGui VUID-01389/01390 (CPM post-fetch patch)

The Vulkan validation layer was firing two VUIDs from `imgui_impl_vulkan.cpp`'s texture-upload path during dense ImGui widget rendering (font glyph rasterisation + partial atlas updates):

- **VUID-VkMappedMemoryRange-size-01390** — `vkFlushMappedMemoryRanges`'s `range[0].size = upload_size` not a multiple of `nonCoherentAtomSize`.
- **VUID-VkMappedMemoryRange-size-01389** — when patched to `VK_WHOLE_SIZE`, the *end* of the current mapping (still bounded by `upload_size`) must equal memory size; the original `vkMapMemory(..., upload_size, ...)` made it not.

Fixed with a CMake post-fetch patch on the CPM-fetched `imgui_impl_vulkan.cpp`: rewrite both the `vkMapMemory` size argument AND the `range[0].size` to `VK_WHOLE_SIZE`. Idempotent via `string(FIND ...)` check so repeated configures skip. Patched at configure time; the message `[crd] Patched imgui_impl_vulkan.cpp texture-upload map/flush -> VK_WHOLE_SIZE (VUID 01389/01390)` confirms it ran. Smoke `smoke_imgui_overlay` no longer emits the VUIDs.

### Debt #3 — `closest_point(Cylinder3<T>)` + `closest_point(Tetrahedron<T>)`

Added the two missing primitive overloads so the showcase's "every primitive shows a closest-point overlay" property holds.

- **`Cylinder3`** — axial/radial decomposition: project query into the cylinder's local frame via `axial = dot(p - a, axis_unit)`, then `radial_vec = (p - a) - axis_unit * axial`. Interior is `axial ∈ [0,len] && radial² ≤ r²`. Outside, clamp axial then either keep the radial vector (if inside radius — closest-on-disk) or shrink it to radius (closest on the side wall). 4-case test sweep (interior, beyond-cap-on-axis, beyond-cap-off-axis, degenerate-axis) catches each branch. Initial implementation used `r_vec = p - axis_point_after_clamp` which mixed axial+radial when t was clamped — the fix decomposes *before* clamping.
- **`Tetrahedron`** — `contains(tet, p)` short-circuit when inside, then minimum over the 4 face Triangle3 closest-points. Brute-force per-face comparison is the test.

Total +118 cases / +64559 assertions in `crd-geometry-primitives-tests`.

### Debt #2 — `BvhViewerCache` for the sandbox BVH viewer

The viewer previously rebuilt the tree every frame, capping the N slider at 800. Wrapped the prim Array + centroids Array + `std::unique_ptr<{BvhTree,Bvh4Tree,DynamicBvh}>` in a `BvhViewerCache` POD owned by `SandboxLayer` (lazy-constructed under the eylem TLSF). A SplitMix64 + FNV fingerprint over `(N, seed, world_size, max_box_size, tree_kind)` triggers rebuild only when a slider mutates; identical-fingerprint frames re-walk the cached tree.

Slider cap lifted 800 → 5000. The cache is move-only; non-copyable.

### Debt #1 — `tests/sandbox/test_showcase.cpp` automated coverage

New test target `crd-sandbox-showcase-tests`. Compiles `sandbox/src/geometry_showcase.cpp` directly (no separate library target — the showcase is sandbox-internal) and links the same deps the sandbox does plus Catch2. 7 cases / 46 assertions:

1. Every `ShowcasePrimitive` (11 enumerants) renders a non-empty buffer when `render_geometry_showcase` is called in `PrimitiveViewer` mode.
2. Every `ShowcaseQuery` (4 enumerants) emits at least the backdrop AABBs.
3. Every `ShowcaseTreeKind` (3 enumerants — Binary/Quad/Dynamic) builds + walks; cache reuse on same-fingerprint frames pins line count == previous frame; mutating `bv_seed` must flip the fingerprint.
4. Every `ShowcaseSdfKind` (9 enumerants) emits at least 30 cross-line samples within the band-pass.
5. `render_draw_showcase` emits >30 lines + at least one triangle (the `*_solid` variants).
6. `line_width` is threaded into emissions — width attribute lands on emitted lines.
7. `SandboxScene` enum values are stable at `Physics=0`, `GeometryViz=1`, `DrawShowcase=2` (static_assert pin so the dropdown indices in `sandbox_layer.cpp` can't drift).

## Verification

`scripts/full-sweep.ps1` (17 configs: Win × 9 + Linux × 7 + win-tidy build) ran end-to-end. **Result: 16/17 PASS.** The one failure was on `win-shipping` at the `crd-sandbox.exe` LTCG codegen phase — a fatal MSVC C1001 internal compiler error inside `link!DllGetObjHandler()` (Access violation) pointing at `engine/config/src/config.cpp(245)` (a file untouched in this slice; pre-existing `Config::load_from_file`).

The slot pointed at by C1001 is plain `Config::load_from_file()`; nothing pathological at that line. The other 16 configs PASS — notably `win-clang-cl-shipping` (same shipping flags, different compiler) is green, ruling out anything in our code.

Standalone retry of `cmake --build --preset win-shipping --target crd-sandbox` linked clean immediately with **no source change**. This is upstream MSVC LTCG nondeterminism, the same family as the previously-documented `win-release` LTCG miscompile in `engine/resources/include/crd/resources/resource_manager.hpp` (fixed with `CRD_NOINLINE` on `evict_block_locked`/`try_evict_to_budget`). Slice closes per `feedback_transient_msvc_ltcg_ice_accept.md` — re-running a 13-minute sweep just hoping the dice land differently is treadmill work; the retry-clean rebuild is the verification.

The ICE is filed as new tracked debt in `docs/debt.md` under `### Transient MSVC LTCG internal compiler error on win-shipping crd-sandbox.exe link`. One incident is upstream noise; recurrence promotes it to actionable workaround (likely `CRD_NOINLINE` on a v1-cluster header function).

### Test counts (post-debt-payment)

- `crd-geometry-primitives-tests` 118/118 cases (was 103 after v1h; +15 cases from Cylinder3 + Tetrahedron closest-point)
- `crd-geometry-bvh-tests` 82 cases (unchanged from v1i-c)
- `crd-sandbox-showcase-tests` 7 cases / 46 assertions (new)
- Win-debug ctest **1308/1308** (was 1297 after v1j-b)
- 16/17 configs PASS

## Decisions

1. **Closed on retry-success, not on re-sweep.** When the user heard "16/17 PASS, the lone failure was an MSVC LTCG transient that retries clean", their explicit guidance was "we don't need a full sweep now". The retry-PASS rebuild + matching clang-cl-shipping PASS is the verification; spending another ~13 minutes to chase a green sweep log of evidence we already have is not slice-closure work. Memory rule: `feedback_transient_msvc_ltcg_ice_accept.md`.

2. **The `tests/geometry-viz/test_showcase.cpp` debt entry was paid as `tests/sandbox/test_showcase.cpp` instead.** Same intent (automated coverage of the v1j-b showcase render paths) but the test compiles `sandbox/src/geometry_showcase.cpp` directly because that's where the render functions live. `tests/geometry-viz/` stays for tests of the `crd-geometry-viz` substrate itself.

3. **`BvhViewerCache::dynamic` is a `std::unique_ptr<DynamicBvh>`.** `DynamicBvh` has private state that requires construction via its public constructor; storing it inline in the cache would require a default-constructible mode or move-assignment over the parent allocator pointer. `unique_ptr` is the cleanest expression and the cache itself stays move-only.

4. **Cylinder3 closest-point algorithm: decompose before clamping.** Initial attempt mixed axial + radial components when `axial` was outside `[0, len]`, getting the wrong closest-on-disk point. The fix splits the query vector into axial scalar + radial vector *in the cylinder's local frame*, then handles clamping component-wise. Four test sub-cases (interior, beyond cap on-axis, beyond cap off-axis, degenerate axis) cover the branches.

5. **ImGui patch is idempotent.** A `string(FIND ...)` check on the source text skips the patch if `VK_WHOLE_SIZE` already appears at those lines. Repeated CMake reconfigures don't re-apply.

## Open / deferred

- **MSVC LTCG transient ICE** is now tracked debt. Promote to action only if it recurs.
- **v2 `-convex`** is the next slice. ADR-0076 §1 sub-module 3, plan in `docs/phases/phase-3.1.7-geometry.md`. Estimated ~3300 LOC engine + ~1000 tests / ~2 weeks. Ships GJK distance + GJK boolean + EPA penetration + SAT + Quickhull + `ConvexHullView` queries + GJK-cast (the general moving-convex shapecast v1i-b deferred to v2).

## Files touched

- `CMakeLists.txt` — ImGui CPM post-fetch patch (debt #4).
- `engine/geometry-primitives/include/crd/geometry/primitives/closest_point.hpp` — `closest_point(Cylinder3)`, `closest_point(Tetrahedron)`, `#include "barycentric.hpp"` (debt #3).
- `tests/geometry-primitives/test_closest_point.cpp` — Cylinder3 + Tetrahedron `TEMPLATE_TEST_CASE` sweep (debt #3).
- `sandbox/src/geometry_showcase.hpp` — `BvhViewerCache` struct, `line_width` + origin-triad state on `GeometryShowcaseState`, `render_geometry_showcase` signature gains `BvhViewerCache&` (debt #2).
- `sandbox/src/geometry_showcase.cpp` — fingerprint hash, cache-keyed rebuild in `render_bvh_viewer`, slider cap raised to 5000 (debt #2).
- `sandbox/src/sandbox_layer.{hpp,cpp}` — `m_bvh_cache` member, lazy construction under eylem TLSF (debt #2).
- `tests/sandbox/CMakeLists.txt` + `tests/sandbox/test_showcase.cpp` — new test target `crd-sandbox-showcase-tests` (debt #1).
- `tests/CMakeLists.txt` — `add_subdirectory(sandbox)`.
- `docs/debt.md` — strike entries #1–#4; add `Transient MSVC LTCG ICE` entry.
- `context.md` — v1 cluster + debts CLOSED notice.

## Next session

Start Phase 3.1.7 v2 `-convex`. First slice = v2a (GJK distance + boolean). New module `engine/geometry-convex/` (target `crd-geometry-convex`, ns `crd::geometry::convex`) with `crd-geometry-primitives` as PUBLIC dep.
