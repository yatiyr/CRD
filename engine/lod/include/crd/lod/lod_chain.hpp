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
};

// Build the chain IN PLACE: appends each level's vertices and indices to the
// mesh's own streams and fills `mesh.lods` (entry 0 describing the source range).
// `scratch` carries the build's working memory; the appended data lands on the
// mesh's own allocator.
[[nodiscard]] LodBuildReport build_lod_chain(crd::resources::MeshResource& mesh, const LodPolicy& policy,
                                             crd::memory::IAllocator* scratch);

} // namespace crd::lod
