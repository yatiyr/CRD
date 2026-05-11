#pragma once

// Collider shape variant. Closed set of primitive shapes; each carries its
// type-specific data inline. ConvexHull references an external vertex
// buffer (the buffer is owned by the scene and shared across instances).
//
// Tagged-union layout chosen over inheritance for AoSoA-friendliness in
// v1b storage and cache-tight broadphase iteration.

#include <crd/core/types.hpp>
#include <crd/eylem/types.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/vec.hpp>

namespace crd::eylem
{
// ColliderShape -- the five canonical collider categories every modern
// physics engine ships. Each is best at a different problem; locking all
// five into the API surface so v1+ slices can drop in their narrow-phase
// impls without a breaking enum bump.
//
//   Primitives (Sphere/Box/Capsule)  -- analytic contact gen, tiny memory.
//   ConvexHull                        -- GJK + EPA, single-convex shape.
//   Plane                             -- analytic, infinite ground plane.
//   TriangleMesh                      -- BVH over triangles + per-tri SAT.
//                                        Best for STATIC level geometry where
//                                        SDF voxel cost would be wasteful.
//   Heightfield                       -- analytic per-cell contact gen.
//                                        Best for TERRAIN -- 100x cheaper
//                                        than equivalent SDF voxels.
//   Sdf                               -- closest-point + gradient on a baked
//                                        SDF (Phase 3.1.5 substrate). Best
//                                        for SMOOTH dynamic mesh colliders;
//                                        unified with renderer DFAO, font
//                                        MTSDF, audio occlusion via crd-sdf.
//
// Convex decomposition tooling (V-HACD / HACD) is a Phase 7 editor concern
// -- artists decompose offline, runtime sees N ConvexHull colliders.
//
// Ship matrix (locked):
//   v1b-b:        Sphere / Box / Capsule storage
//   v1d:          + ConvexHull + Plane (GJK/EPA narrow phase)
//   v1d-mesh:     + TriangleMesh (BVH narrow phase)
//   v1d-hf:       + Heightfield (per-cell narrow phase)
//   Phase 3.1.5:  + Sdf (consumes crd-sdf substrate)
enum class ColliderShape : crd::u8
{
    Sphere       = 0,
    Box          = 1,
    Capsule      = 2,
    ConvexHull   = 3,
    Plane        = 4,
    TriangleMesh = 5, // v1d-mesh: BVH over triangles
    Heightfield  = 6, // v1d-hf:   per-cell analytic
    Sdf          = 7, // Phase 3.1.5: crd-sdf substrate consumer
};

struct ColliderSphere
{
    crd::f32 radius = 1.0F;
};

struct ColliderBox
{
    crd::math::Vec3f half_extents{1.0F, 1.0F, 1.0F};
};

struct ColliderCapsule
{
    crd::f32 radius      = 0.5F;
    crd::f32 half_height = 0.5F; // half of cylinder body length, excluding hemispheres
};

struct ColliderConvexHull
{
    // Reference into a scene-owned vertex buffer. Implementation associates
    // the slot with the convex hull data + its precomputed support function
    // table. v1d wires GJK against this.
    crd::u32 vertex_buffer_slot = 0;
    crd::u32 vertex_count       = 0;
};

struct ColliderPlane
{
    // Plane equation: dot(normal, p) + d = 0, normal points "outside".
    crd::math::Vec3f normal{0.0F, 1.0F, 0.0F};
    crd::f32         d = 0.0F;
};

// Static triangle mesh -- BVH-backed contact generation against the mesh's
// triangles. Best for level geometry where SDF voxel memory would be
// wasteful. The BVH is built offline by the .mesh-collider cooker (lands
// alongside v1d-mesh) and stored in `persistent_alloc`. The handle here
// references the cooked record.
struct ColliderTriangleMesh
{
    // Slot in the scene-owned triangle-mesh registry. The registry holds
    // {vertex buffer, index buffer, BVH nodes}. Slot 0 = unbound.
    crd::u32 mesh_slot     = 0;
    // Number of triangles for diagnostic / capacity bookkeeping.
    crd::u32 triangle_count = 0;
};

// Heightfield -- 2D regular-grid height map. Per-cell contact gen is
// analytic (compute the bilinear-interpolated height + normal at a query
// point). Vastly cheaper than triangle mesh or SDF for terrain at scale
// -- a 4096x4096 height map is ~64 MB of f32 vs ~640 MB equivalent SDF
// voxels at 0.5 m resolution.
struct ColliderHeightfield
{
    // Slot in the scene-owned heightfield registry.
    crd::u32         heightfield_slot = 0;
    // Cell spacing in metres (uniform on X and Z; height in Y).
    crd::f32         cell_size_m      = 1.0F;
    // Grid extent in cells (cols on X, rows on Z). Cooker enforces.
    crd::u32         cols             = 0;
    crd::u32         rows             = 0;
    // Min / max height for AABB culling without sampling every cell.
    crd::f32         min_height_m     = 0.0F;
    crd::f32         max_height_m     = 0.0F;
};

// Signed-distance-field collider -- closest-point + gradient queries
// against a baked SDF. Implementation lives in Phase 3.1.5 (`crd-sdf`
// substrate); this collider type holds the handle into the SDF resource.
// Best for SMOOTH dynamic mesh colliders where SDF voxel cost is paid for
// by closest-point query speed + smooth contact normals.
struct ColliderSdf
{
    // Slot in the scene-owned SDF registry. Indirects through crd-sdf's
    // SdfResource (Phase 3.1.5 v0+).
    crd::u32         sdf_slot          = 0;
    // World-space scale factor (SDFs are baked in their own coordinate
    // frame; this scales the queries back to scene units).
    crd::f32         world_scale       = 1.0F;
    // Numeric thickness band around the surface for narrow-phase contact
    // detection. Smaller = sharper edges; larger = smoother contact.
    crd::f32         contact_thickness = 0.01F;
};

// Per-collider flags (ADR-0068 §10.1). Sensor is a per-collider flag, NOT
// a body type — a single body can have both solid colliders (a character's
// foot capsule) AND sensor colliders (a proximity aura sphere). Locked
// across PhysX, Box2D v3, Unity, Godot, Unreal, Bullet, ODE, MuJoCo;
// Jolt's per-body sensor model is the documented modern-AAA-games
// exception (research dossier §3).
struct ColliderFlags
{
    crd::u8 is_sensor : 1; // overlap-only; no contact response
    crd::u8 _reserved : 7;
};

static_assert(sizeof(ColliderFlags) == 1, "ColliderFlags must pack to 1 byte");

// Collider — local-frame variant of one shape attached to a body.
//
// `local_position` and `local_rotation` express the collider's pose in the
// owning body's local frame. A body with multiple colliders forms a
// compound shape (PhysX `PxShape` family).
struct Collider
{
    ColliderShape    shape           = ColliderShape::Sphere;
    ColliderFlags    flags{};
    crd::math::Vec3f local_position{0.0F, 0.0F, 0.0F};
    crd::math::Quatf local_rotation{0.0F, 0.0F, 0.0F, 1.0F};

    union
    {
        ColliderSphere       sphere;
        ColliderBox          box;
        ColliderCapsule      capsule;
        ColliderConvexHull   convex_hull;
        ColliderPlane        plane;
        ColliderTriangleMesh triangle_mesh;
        ColliderHeightfield  heightfield;
        ColliderSdf          sdf;
    };

    constexpr Collider() noexcept : sphere{} {}
};

// API surface freeze pins (ADR-0062 §15).
// shape (1B) + 3B pad + Vec3f (12B) + Quatf (16B) + union (largest = ColliderHeightfield = 24B).
static_assert(sizeof(ColliderSphere)       ==  4, "ColliderSphere must pack to 4 bytes");
static_assert(sizeof(ColliderBox)          == 12, "ColliderBox must pack to 12 bytes");
static_assert(sizeof(ColliderCapsule)      ==  8, "ColliderCapsule must pack to 8 bytes");
static_assert(sizeof(ColliderConvexHull)   ==  8, "ColliderConvexHull must pack to 8 bytes");
static_assert(sizeof(ColliderPlane)        == 16, "ColliderPlane must pack to 16 bytes");
static_assert(sizeof(ColliderTriangleMesh) ==  8, "ColliderTriangleMesh must pack to 8 bytes");
static_assert(sizeof(ColliderHeightfield)  == 24, "ColliderHeightfield must pack to 24 bytes");
static_assert(sizeof(ColliderSdf)          == 12, "ColliderSdf must pack to 12 bytes");

} // namespace crd::eylem
