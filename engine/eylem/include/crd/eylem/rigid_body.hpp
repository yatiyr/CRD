#pragma once

// RigidBody — POD describing one simulated body's state. Public AoS layout
// for ergonomic authoring; the v1b storage layer fans these into AoSoA-8
// columns for SIMD broadphase + solver.

#include <crd/core/types.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/vec.hpp>

namespace crd::eylem
{
enum class RigidBodyType : crd::u8
{
    // Never moves. Infinite mass; collides with dynamic + kinematic. Cheap.
    Static    = 0,
    // Moves only via direct set_body_state. Infinite mass for the solver
    // but still pushes dynamic bodies. Used for moving platforms, doors.
    Kinematic = 1,
    // Fully simulated. Finite mass; participates in constraints, collisions,
    // gravity, sleeping.
    Dynamic   = 2
};

// Bit-packed body flags. 32-bit u32 storage for cache density. The two-bit
// `type` field uses RigidBodyType values; `_reserved` is free for v1+
// expansions without changing the wire layout.
struct RigidBodyFlags
{
    crd::u32 type             : 2; // RigidBodyType
    crd::u32 sleeping         : 1; // currently asleep (no force application, no integration)
    crd::u32 ccd_enabled      : 1; // continuous collision detection (v6+)
    crd::u32 gravity_enabled  : 1; // honour world gravity (Static / Kinematic ignore anyway)
    crd::u32 lock_position_x  : 1;
    crd::u32 lock_position_y  : 1;
    crd::u32 lock_position_z  : 1;
    crd::u32 lock_rotation_x  : 1;
    crd::u32 lock_rotation_y  : 1;
    crd::u32 lock_rotation_z  : 1;
    crd::u32 _reserved        : 21;
};

static_assert(sizeof(RigidBodyFlags) == 4, "RigidBodyFlags must pack to 4 bytes");

// Public POD. The `inv_mass = 0` and `inv_inertia = {0,0,0}` invariant marks
// a body as effectively static for the solver (matches Bullet / Box2D /
// PhysX convention; div-by-zero-free constraint math).
//
// Inertia is stored as the diagonal of the body-space inertia tensor.
// Off-diagonal entries are reserved for v1f (asymmetric collider compounds);
// v1c-v1e diagonalise at body construction.
struct RigidBody
{
    crd::math::Vec3f position{0.0F, 0.0F, 0.0F};
    crd::math::Quatf rotation{0.0F, 0.0F, 0.0F, 1.0F};

    crd::math::Vec3f linear_velocity{0.0F, 0.0F, 0.0F};
    crd::math::Vec3f angular_velocity{0.0F, 0.0F, 0.0F};

    // 1 / mass. 0 == infinite mass (Static / Kinematic).
    crd::f32         inv_mass = 0.0F;
    // diag(1/I_xx, 1/I_yy, 1/I_zz) in body space. 0 == infinite inertia.
    crd::math::Vec3f inv_inertia{0.0F, 0.0F, 0.0F};

    crd::f32 linear_damping  = 0.05F;
    crd::f32 angular_damping = 0.05F;

    RigidBodyFlags flags{}; // type defaults to Static (RigidBodyType::Static == 0)
};

// API surface freeze pin (ADR-0062 §15).
//   pos(12) + rot(16) + lin_vel(12) + ang_vel(12) + inv_mass(4)
// + inv_inertia(12) + lin_damp(4) + ang_damp(4) + flags(4) = 80
static_assert(sizeof(RigidBody) == 80, "RigidBody must pack to 80 bytes");

} // namespace crd::eylem
