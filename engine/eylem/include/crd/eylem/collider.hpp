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
enum class ColliderShape : crd::u8
{
    Sphere     = 0,
    Box        = 1,
    Capsule    = 2,
    ConvexHull = 3,
    Plane      = 4
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

// Collider — local-frame variant of one shape attached to a body.
//
// `local_position` and `local_rotation` express the collider's pose in the
// owning body's local frame. A body with multiple colliders forms a
// compound shape (PhysX `PxShape` family).
struct Collider
{
    ColliderShape    shape           = ColliderShape::Sphere;
    crd::math::Vec3f local_position{0.0F, 0.0F, 0.0F};
    crd::math::Quatf local_rotation{0.0F, 0.0F, 0.0F, 1.0F};

    union
    {
        ColliderSphere     sphere;
        ColliderBox        box;
        ColliderCapsule    capsule;
        ColliderConvexHull convex_hull;
        ColliderPlane      plane;
    };

    constexpr Collider() noexcept : sphere{} {}
};

// API surface freeze pins (ADR-0062 §15).
// shape (1B) + 3B pad + Vec3f (12B) + Quatf (16B) + union (max = ColliderPlane = 16B).
static_assert(sizeof(ColliderSphere)     ==  4, "ColliderSphere must pack to 4 bytes");
static_assert(sizeof(ColliderBox)        == 12, "ColliderBox must pack to 12 bytes");
static_assert(sizeof(ColliderCapsule)    ==  8, "ColliderCapsule must pack to 8 bytes");
static_assert(sizeof(ColliderConvexHull) ==  8, "ColliderConvexHull must pack to 8 bytes");
static_assert(sizeof(ColliderPlane)      == 16, "ColliderPlane must pack to 16 bytes");

} // namespace crd::eylem
