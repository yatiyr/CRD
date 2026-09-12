#pragma once

// Joint — data-only descriptor of a constraint between two bodies. The
// concrete solver behaviour for each JointType is implemented in v1f.

#include <crd/core/types.hpp>
#include <crd/eylem/types.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/vec.hpp>

namespace crd::eylem
{
enum class JointType : crd::u8
{
    // Welds two bodies together. 0 DoF.
    Fixed     = 0,
    // 1 DoF rotational about a single axis. Hinge.
    Revolute  = 1,
    // 3 DoF rotational about a point. Ball-and-socket.
    Spherical = 2,
    // 1 DoF translational along a single axis.
    Prismatic = 3,
    // Maintains a fixed distance between two anchor points.
    Distance  = 4
};

// Anchor — a body-local frame attached to one side of a joint.
struct JointAnchor
{
    crd::math::Vec3f local_position{0.0F, 0.0F, 0.0F};
    crd::math::Quatf local_rotation{0.0F, 0.0F, 0.0F, 1.0F};
};

// Joint motion limits. Interpretation is type-specific:
//   Revolute / Spherical: angular limits in radians
//   Prismatic / Distance: linear limits in metres
struct JointLimit
{
    crd::f32 min     = 0.0F;
    crd::f32 max     = 0.0F;
    bool     enabled = false;
};

struct Joint
{
    JointType   type = JointType::Fixed;
    BodyId      body_a;
    BodyId      body_b;
    JointAnchor anchor_a;
    JointAnchor anchor_b;
    JointLimit  limit;

    // Force / torque thresholds at which the joint breaks. 0 = unbreakable.
    // Implementation: solver checks |constraint impulse| > break threshold.
    crd::f32 break_force  = 0.0F;
    crd::f32 break_torque = 0.0F;
};

// API surface freeze pins (ADR-0062 §15).
static_assert(sizeof(JointAnchor) == 28, "JointAnchor must pack to 28 bytes");
static_assert(sizeof(JointLimit)  == 12, "JointLimit must pack to 12 bytes");

} // namespace crd::eylem
