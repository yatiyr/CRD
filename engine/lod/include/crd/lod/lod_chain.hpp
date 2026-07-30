#pragma once

// ---------------------------------------------------------------------------
// crd-lod — REN-40-C1: BUILDING A MESH'S LOD CHAIN.
//
// **What this is for, in one number.** At 1M instances the measured frame spends
// ~86 ms of GPU drawing ~256k instances at full detail — about 1.5 BILLION
// triangles across the camera view and four shadow cascades, because the far half
// of a 2000-unit field pays full vertex cost for sub-pixel coverage. A correctly
// levelled frame is 10-20M. Nothing else in the renderer can recover that; it has
// to come from not submitting the triangles.
//
// **The chain's shape.** Level 0 is the source mesh, untouched — byte for byte,
// so a mesh with a chain and a mesh without one render identically at the near
// distance. Each further level is a decimation of the source, appended to the
// SAME vertex and index streams:
//
//     vertices : [ level 0 verts ][ level 1 verts ][ level 2 verts ] ...
//     indices  : [ level 0 idx   ][ level 1 idx   ][ level 2 idx   ] ...
//
// ⛔ A decimated level has NEW vertices — `v_opt` is a solved optimum, not one of
// the collapsed endpoints — so a level cannot be a subset of the one above it.
// Its indices are ABSOLUTE into the combined vertex stream, which is what keeps
// `base_vertex` unrepresentable (`IRasterContext::IndexedDraw` explains why that
// matters: Vulkan folds `firstInstance` into `gl_InstanceIndex` and D3D12's
// `SV_InstanceID` does not, so a non-zero base makes the backends disagree).
//
// **Attributes are in the ERROR, not fixed up after it.** The decimation runs
// through `qem_decimate_attr<f32, 2>` with the UV pair as channels
// (`attribute_quadric.hpp`). A position-only metric happily picks the collapse
// that leaves the SURFACE where it was and drags the TEXTURE across it: the
// silhouette stays right and the texture swims as the level changes. Repairing
// UVs afterwards cannot help — the collapse was already CHOSEN without regard to
// the distortion it causes.
//
// ⛔ NORMALS AND TANGENTS ARE RE-DERIVED, never carried. An interpolated normal
// of a simplified surface is the normal of a surface that no longer exists: it
// would light the coarse mesh as though the fine one were still there, which
// reads as a shading POP at exactly the moment the level changes — the artefact
// the whole chain exists to avoid.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/mesh_resource.hpp>

namespace crd::lod
{

inline constexpr crd::u32 kMaxLodLevels = 8U;

// What the author asked for. ⭐ AUTHORED, never C++ constants: the shipped policy
// is a `.crdlod` asset, and this struct is what parsing one produces.
struct LodPolicy
{
    // Levels BEYOND level 0. `ratio[i]` is the triangle fraction of the SOURCE
    // mesh that level i+1 targets (0.5 = half the triangles of level 0).
    // ⛔ Ratios are of the SOURCE, not of the previous level: a chain expressed
    // relative to its predecessor compounds rounding, and two assets asking for
    // "half then half" end up with different level-2 counts depending on how
    // level 1 happened to round.
    crd::u32 extra_levels = 0U;
    crd::f32 ratio[kMaxLodLevels]{};
    // Projected screen HEIGHT in pixels below which this level is chosen. Read
    // by the GPU selector in C2; carried here so one authored file owns the whole
    // policy rather than splitting it across a cook and a renderer constant.
    crd::f32 screen_height[kMaxLodLevels]{};
    // Garland 1998 boundary preservation. Held high by default: a silhouette that
    // shrinks as the level changes is visible from any distance, while interior
    // detail at LOD 2+ is by definition sub-pixel.
    crd::f32 boundary_weight = 1000.0F;
    // ⛔⛔ THE TRIANGLE FLOOR — a level is only worth building if it is still a
    // SURFACE. Applying a ratio blindly is what produced the defect this knob
    // exists to stop: the sandbox's 12-triangle cubes were decimated to SIX, and
    // a closed box cannot be six triangles, so "level 1 of a cube" was a
    // degenerate sliver — on screen the cubes simply VANISHED at any distance
    // past the first threshold. A 20-triangle icosahedron went the other way and
    // produced 20 -> 20 -> 20: three levels that reduce nothing, each costing a
    // draw call and a command for no triangles saved.
    // ⭐ It is AUTHORED and not an engine constant for the same reason the ratios
    // are: what counts as "still a surface" is a property of the content. A
    // source mesh at or below the floor gets NO chain at all, and generation
    // stops at the first level that would go under it or that fails to reduce.
    crd::u32 min_triangles = 64U;
    // ⛔⛔ THE SHAPE TEST — a triangle count is not a quality bar. Measured on the showcase: the 6,036-triangle
    // source decimated to 104 triangles (58x) still cleared `min_triangles`, and every instance rendered as a
    // flat SLIVER — the surface had stopped being the object. The count said the level was fine; the silhouette
    // said it was gone.
    // ⭐ So a generated level must still OCCUPY THE SOURCE'S SPACE: its bounding box extent on every axis has to
    // stay at least this fraction of the source's, or the level is refused and the chain stops there. That is a
    // measurement of the thing that actually matters (does it still look like the object from any direction)
    // rather than a proxy for it, and it is AUTHORED because how much silhouette loss is acceptable is a
    // property of the content.
    crd::f32 min_extent_ratio = 0.5F;
    // ⛔⛔ AND THE AREA — because the extent test is NOT sufficient, which is a thing this had to learn twice.
    // The 104-triangle level that rendered as slivers PASSED the extent test: its bounding box still spanned the
    // source's, because a few surviving vertices sat at the extremes. What it had lost was SURFACE — the shell
    // between those extremes was gone. Summed triangle area is the quantity that distinguishes "a coarse version
    // of the object" (similar area, fewer triangles) from "a handful of slivers spanning the same box".
    crd::f32 min_area_ratio = 0.5F;

    // ── ⭐⭐ REN-40-C3: THE PER-VIEW BIAS. ────────────────────────────────────────────────────────────────
    // ⛔ THE ERROR IS IN PIXELS OF THE TARGET BEING RENDERED, and the targets are not alike. Cascade 3 sits at
    // roughly 0.18 world-units per texel — it physically CANNOT resolve what the forward pass can — yet it was
    // selecting the same level for the same instance, so the most expensive pass in the frame drew the finest
    // geometry to fill the coarsest buffer. Scaling the metric per view fixes that at its source: a cascade
    // treats an instance as SMALLER on screen than the camera does, and drops it down the chain sooner.
    // ⭐ AUTHORED, and authored HERE, because it is the same judgement the switch heights are — how much
    // silhouette a shadow may lose is a property of the content, not of the renderer. Index 0 is the camera;
    // 1..kMaxLodLevels-1 are the cascades. 1.0 = no bias, so an unstated view behaves exactly as before.
    // ⛔ It multiplies the PROJECTED HEIGHT, not the level index, for the reason the per-entity bias does: a
    // step on the index means something different for a 6-level chain than for a 2-level one, while scaling the
    // metric keeps one meaning at every depth.
    crd::f32 view_bias[kMaxLodLevels]{};

    // ── ⭐⭐ REN-40-C4: TRANSITIONS THAT CANNOT BE SEEN. ──────────────────────────────────────────────────
    // ⛔⛔ HYSTERESIS COMES FIRST, AND IT IS NOT OPTIONAL. An instance sitting exactly on a switch height with a
    // camera that breathes by a pixel flips level every frame — and NO fade can hide a flip that happens every
    // frame, because the fade never completes. `hysteresis` widens the threshold in the direction the instance
    // is already going: a level is entered at `h` and left only at `h * (1 + hysteresis)`, so the boundary
    // becomes a band and a jittering instance stays put.
    // ⭐ It is expressible without ANY per-frame state on the device — which matters, because the selector is a
    // stateless kernel over a million instances and remembering last frame's level per instance would be a
    // million-entry buffer and a read-modify-write. See the kernel for how the band is applied one-sided.
    crd::f32 hysteresis = 0.15F;
    // ⭐⭐ THE DITHERED CROSS-DISSOLVE BAND, as a fraction of the switch height. Inside the band BOTH levels are
    // drawn and a stable per-instance hash decides which pixels each keeps, so the change is spread over a
    // distance rather than happening on one frame.
    // ⛔ DITHER OVER GEOMORPH, on purpose: geomorphing needs a NESTED chain (level n+1 a subset of level n),
    // which constrains the simplifier and — decisively — CANNOT cross the mesh-to-impostor boundary at all,
    // which is the transition most likely to be seen. 0 disables it.
    crd::f32 dither_band = 0.25F;
};

enum class LodBuildStatus : crd::u8
{
    Ok = 0,
    EmptyMesh,        // no vertices / no indices to work from
    NotTriangles,     // index count is not a multiple of 3
    NonManifoldInput, // the decimator refuses a non-manifold surface
    NoLevelsRequested,
    AlreadyBuilt, // the mesh already carries a chain; building twice would append a second one
};

struct LodBuildReport
{
    LodBuildStatus status       = LodBuildStatus::Ok;
    crd::u32       levels_built = 0U; // INCLUDING level 0
    crd::u32       triangles[kMaxLodLevels]{};
    crd::f32       error[kMaxLodLevels]{};
    // ⛔ Levels the decimator could not reach (link condition / inversion
    // prevention stopped it early) are REPORTED, not silently accepted as the
    // requested ratio — a chain that claims 10% and delivers 80% is a chain that
    // buys nothing, and the fps board would be the first place anyone noticed.
    crd::u32 levels_short_of_target = 0U;
    // ⛔ AND WHY A LEVEL WAS REFUSED, in numbers. A chain that silently stops one level early is
    // indistinguishable on an fps board from a policy that asked for one level fewer — and the two have
    // completely different fixes. `refused_area_ratio` is the measured area of the first level the shape test
    // rejected, as a fraction of the source's (0 = none refused).
    crd::u32 levels_refused_shape = 0U;
    crd::f32 refused_area_ratio   = 0.0F;
    crd::f32 refused_extent_ratio = 0.0F;
};

// Build the chain IN PLACE: appends each level's vertices and indices to the
// mesh's own streams and fills `mesh.lods` (entry 0 describing the source range).
// `scratch` carries the build's working memory; the appended data lands on the
// mesh's own allocator.
[[nodiscard]] LodBuildReport build_lod_chain(crd::resources::MeshResource& mesh, const LodPolicy& policy,
                                             crd::memory::IAllocator* scratch);

} // namespace crd::lod
