#pragma once

// ---------------------------------------------------------------------------
// Sandbox geometry showcase — Phase 3.1.7 v1j-b.
//
// Companion to `SandboxLayer` that exercises the entire `crd-geometry-*` +
// `crd-geometry-viz` substrate interactively. The showcase has four sub-
// modes; each is independent and emits primitives into the layer's shared
// `crd::draw::RenderBuffer`:
//
//   1. Primitive viewer — every concrete primitive type with sliders; renders
//      its wireframe + a draggable query point + the closest-point segment
//      (visual correctness pin: the segment always lands on the surface).
//   2. Query showcase   — fixed BVH of N AABBs; ImGui picks
//      raycast / overlap / closest-point / shapecast and visualises the
//      result via the v1i unified facade.
//   3. BVH viewer       — N random AABBs (deterministic seed), depth-coloured
//      tree walk via viz::draw_bvh, BvhTree / Bvh4Tree / DynamicBvh toggle,
//      overlap pairs and frustum-cull overlays.
//   4. SDF heatmap      — sample any v1h `sd_*` analytic SDF at a 3D grid,
//      tint each sample by its signed distance.
//
// Design rules:
//   * The showcase is *pure data + pure functions*. `GeometryShowcaseState`
//     is a POD-ish struct on the SandboxLayer; `render_geometry_showcase`
//     emits into the caller's `RenderBuffer` and reads/writes the state.
//   * No allocator is stored on the state — anything that needs scratch
//     gets it from a function argument or a TLSF the layer owns.
//   * Switching modes/sub-modes is non-destructive: per-mode state is kept
//     in the showcase struct so you can toggle freely without losing
//     parameter tuning.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/geometry/bvh/bvh4.hpp>
#include <crd/geometry/bvh/bvh_tree.hpp>
#include <crd/geometry/bvh/dynamic_bvh.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <memory>

namespace crd::sandbox
{
// Which top-level scene the SandboxLayer is currently rendering. Three
// scenes coexist — each owns its own draw-buffer contents per frame; no
// bleed-through. User picks via the ImGui Scene dropdown.
enum class SandboxScene : crd::u8
{
    Physics      = 0, // existing v1b-e demo — 3 falling rigid bodies
    GeometryViz  = 1, // v1j-b showcase — interactive crd-geometry validation
    DrawShowcase = 2, // crd-draw API showcase — axis triad / wire+solid shapes / arrow / arc / cross
};

// Sub-mode of the geometry-viz showcase. Each renders a distinct exercise
// of the substrate.
enum class GeometryShowcaseMode : crd::u8
{
    PrimitiveViewer = 0,
    QueryShowcase   = 1,
    BvhViewer       = 2,
    SdfHeatmap      = 3,
};

// Primitive viewer's currently-selected shape.
enum class ShowcasePrimitive : crd::u8
{
    Sphere      = 0,
    Aabb        = 1,
    Obb         = 2,
    Capsule     = 3,
    Cylinder    = 4,
    Plane       = 5,
    Triangle    = 6,
    Tetrahedron = 7,
    Frustum     = 8,
    Ray         = 9,
    Segment     = 10,
};

// Query showcase mode — which crd::geometry facade call to demonstrate.
enum class ShowcaseQuery : crd::u8
{
    Raycast       = 0,
    Overlap       = 1,
    ClosestPoint  = 2,
    SphereCast    = 3,
};

// BVH viewer's tree-type toggle.
enum class ShowcaseTreeKind : crd::u8
{
    Binary  = 0, // BvhTree
    Quad    = 1, // Bvh4Tree (collapsed)
    Dynamic = 2, // DynamicBvh
};

// SDF heatmap: which v1h analytic SDF to sample.
enum class ShowcaseSdfKind : crd::u8
{
    Sphere      = 0,
    Box         = 1,
    RoundBox    = 2,
    Torus       = 3,
    Octahedron  = 4,
    Capsule     = 5,
    Cone        = 6,
    BoxFrame    = 7,
    Cylinder    = 8,
};

// Per-mode parameter state. Lives inline on the SandboxLayer so freely
// switching modes preserves tunings.
struct GeometryShowcaseState
{
    GeometryShowcaseMode mode = GeometryShowcaseMode::PrimitiveViewer;

    // ---- Shared (every mode) ----
    // Pixel width passed to every line emission in the showcase. The
    // crd-draw line pipeline does vertex-shader quad expansion, so the
    // pixel value is what the user sees. Higher = thicker / more
    // visible against the engine's neutral debug clear. Threaded through
    // every `viz::draw(...)` / `viz::draw_*(...)` call below + the
    // SDF heatmap's `cross_3d_to` emissions.
    crd::f32 line_width = 2.0F;
    bool     show_origin_triad = true;
    crd::f32 origin_triad_size = 0.5F;

    // ---- Primitive viewer ----
    ShowcasePrimitive primitive = ShowcasePrimitive::Sphere;
    crd::math::Vec3f  prim_center{0.0F, 0.0F, 0.0F};
    crd::math::Vec3f  prim_half_extents{1.0F, 1.0F, 1.0F};
    crd::math::Vec3f  prim_axis_a{-1.0F, 0.0F, 0.0F};
    crd::math::Vec3f  prim_axis_b{1.0F, 0.0F, 0.0F};
    crd::math::Vec3f  prim_axis_c{0.0F, 1.0F, 0.0F};
    crd::math::Vec3f  prim_axis_d{0.0F, 0.0F, 1.0F};
    crd::f32          prim_radius           = 1.0F;
    crd::math::Vec3f  prim_obb_euler{0.0F, 0.0F, 0.0F}; // yaw/pitch/roll for OBB orientation
    crd::math::Vec3f  prim_plane_normal{0.0F, 1.0F, 0.0F};
    crd::f32          prim_plane_d          = 0.0F;
    crd::f32          prim_plane_patch_size = 4.0F;
    // Frustum (built from view-proj-like parameters)
    crd::f32 prim_frustum_fov_deg = 60.0F;
    crd::f32 prim_frustum_aspect  = 1.6F;
    crd::f32 prim_frustum_near    = 1.0F;
    crd::f32 prim_frustum_far     = 6.0F;
    crd::math::Vec3f prim_frustum_pos{0.0F, 0.0F, -8.0F};
    crd::math::Vec3f prim_frustum_look{0.0F, 0.0F, 1.0F};
    // Draggable query point — closest-point segment endpoint
    crd::math::Vec3f prim_query{3.0F, 0.5F, 0.0F};
    bool             prim_show_query = true;

    // ---- Query showcase ----
    crd::u32         qs_seed      = 0x5EEDED;
    crd::u32         qs_prim_count = 8;
    crd::f32         qs_world_size = 6.0F;
    ShowcaseQuery    qs_mode      = ShowcaseQuery::Raycast;
    crd::math::Vec3f qs_ray_origin{-8.0F, 0.0F, 0.0F};
    crd::math::Vec3f qs_ray_dir{1.0F, 0.0F, 0.0F};
    crd::f32         qs_ray_tmax = 50.0F;
    crd::math::Vec3f qs_overlap_min{-1.0F, -1.0F, -1.0F};
    crd::math::Vec3f qs_overlap_max{1.0F, 1.0F, 1.0F};
    crd::math::Vec3f qs_closest_query{0.0F, 0.0F, 0.0F};
    crd::math::Vec3f qs_sweep_center{-8.0F, 0.0F, 0.0F};
    crd::math::Vec3f qs_sweep_dir{1.0F, 0.0F, 0.0F};
    crd::f32         qs_sweep_radius = 0.5F;
    crd::f32         qs_sweep_tmax   = 50.0F;

    // ---- BVH viewer ----
    crd::u32         bv_n               = 200;
    crd::u32         bv_seed            = 0xCAFE;
    crd::f32         bv_world_size      = 12.0F;
    crd::f32         bv_max_box_size    = 1.5F;
    ShowcaseTreeKind bv_tree_kind       = ShowcaseTreeKind::Binary;
    crd::u32         bv_depth_limit     = 0; // 0 = no limit
    bool             bv_show_overlap_pairs = false;
    bool             bv_show_frustum_cull = false;
    crd::math::Vec3f bv_frustum_pos{0.0F, 0.0F, -16.0F};
    crd::f32         bv_frustum_fov_deg = 50.0F;
    crd::f32         bv_frustum_near    = 1.0F;
    crd::f32         bv_frustum_far     = 20.0F;
    crd::f32         bv_frustum_aspect  = 1.6F;

    // ---- SDF heatmap ----
    // Defaults chosen so the typical (sd_sphere r=1, sd_box, sd_torus) case
    // renders cleanly inside the 4096-line per-frame draw budget. Each grid
    // sample emits one 3-axis cross (3 lines). grid_res=12 → 1728 max
    // samples; the `sdf_max_distance` band-pass cuts most of those, leaving
    // a few hundred lines per frame for a typical shape.
    ShowcaseSdfKind sdf_kind          = ShowcaseSdfKind::Sphere;
    crd::u32        sdf_grid_res      = 12;
    crd::f32        sdf_grid_extent   = 2.0F;
    crd::f32        sdf_max_distance  = 1.5F;
    crd::f32        sdf_sphere_r      = 1.0F;
    crd::math::Vec3f sdf_box_b{1.0F, 0.5F, 0.7F};
    crd::f32        sdf_round_r       = 0.2F;
    crd::math::Vec3f sdf_torus_t{1.0F, 0.3F, 0.0F}; // (major, minor)
    crd::f32        sdf_octa_s        = 1.0F;
    crd::math::Vec3f sdf_capsule_a{-0.5F, 0.0F, 0.0F};
    crd::math::Vec3f sdf_capsule_b{0.5F, 0.0F, 0.0F};
    crd::f32        sdf_capsule_r     = 0.3F;
    crd::math::Vec3f sdf_cone_dir{0.0F, 1.0F, 0.0F};
    crd::f32        sdf_cone_height   = 1.0F;
    crd::f32        sdf_cone_radius   = 0.7F;
    crd::f32        sdf_frame_e       = 0.1F;
    crd::f32        sdf_cylinder_h    = 1.0F;
};

// BVH viewer rebuild cache (v1-close debt payment). Owned by SandboxLayer
// (single instance per process), passed by non-owning pointer into the
// showcase render path. Holds the most recently built prims + tree(s) keyed
// on a fingerprint of the slider parameters; render_bvh_viewer rebuilds
// only when the fingerprint changes, lifting the per-frame O(N) cost.
//
// Allocator is captured at construction (the eylem TLSF — same one used for
// the showcase's per-call scratch); the cache lives for the layer's
// lifetime. Move-only.
struct BvhViewerCache
{
    explicit BvhViewerCache(crd::memory::IAllocator* allocator) noexcept
        : alloc(allocator), prims(allocator), centroids(allocator)
    {
    }
    BvhViewerCache(const BvhViewerCache&) = delete;
    BvhViewerCache& operator=(const BvhViewerCache&) = delete;
    BvhViewerCache(BvhViewerCache&&) noexcept = default;
    BvhViewerCache& operator=(BvhViewerCache&&) noexcept = default;
    ~BvhViewerCache() = default;

    crd::memory::IAllocator* alloc = nullptr;
    crd::u64 fingerprint = 0; // (n, seed, world_size, max_box_size, tree_kind) hash; 0 = no cache
    crd::containers::Array<crd::geometry::primitives::AABB3<crd::f32>> prims;
    crd::containers::Array<crd::math::Vec3f> centroids; // populated for DynamicBvh tree-kind only
    std::unique_ptr<crd::geometry::bvh::BvhTree>   binary;
    std::unique_ptr<crd::geometry::bvh::Bvh4Tree>  quad;
    std::unique_ptr<crd::geometry::bvh::DynamicBvh> dynamic;
};

// Emit the current showcase mode's primitives into `buf`. `alloc` is used for
// per-call scratch (Query showcase BVH build; SDF heatmap is allocation-
// free); `bvh_cache` provides the BVH viewer's persistent cache so it
// doesn't rebuild every frame. Both references are non-owning and the
// function does not retain them beyond the call.
void render_geometry_showcase(GeometryShowcaseState& state, crd::draw::RenderBuffer& buf,
                              crd::memory::IAllocator& alloc, BvhViewerCache& bvh_cache);

// Render the ImGui control panel for the current mode. Mutates `state`.
void draw_geometry_showcase_imgui(GeometryShowcaseState& state);

// Emit the crd-draw API showcase into `buf` — the historic v1a-draw d0d demo
// (axis triad + box wire/solid + sphere wire/solid + capsule wire/solid +
// arrow + cross + arc). Previously hard-coded in `render_scene` to fire
// every frame; now gated on `SandboxScene::DrawShowcase`. Uses the same
// `line_width` slider as the geometry showcase.
void render_draw_showcase(const GeometryShowcaseState& state, crd::draw::RenderBuffer& buf);

} // namespace crd::sandbox
