#pragma once

// Material properties — friction, restitution, density. Per-shape, not
// per-body, so a single body with multiple colliders can have heterogeneous
// surface materials (PhysX / Jolt convention).

#include <crd/core/types.hpp>
#include <crd/eylem/types.hpp>

namespace crd::eylem
{
struct Material
{
    // Coulomb friction model. Static friction is used while contact is at
    // rest; dynamic friction once it begins to slide. PhysX convention is
    // dynamic <= static; this is enforced by the impl, not the type.
    crd::f32 static_friction  = 0.5F;
    crd::f32 dynamic_friction = 0.5F;

    // Bounciness in [0, 1]. 0 = inelastic, 1 = perfectly elastic.
    crd::f32 restitution      = 0.0F;

    // kg / m^3. Used to derive mass from collider volume when no explicit
    // mass is set on the rigid body. Default = water (1000 kg/m^3).
    crd::f32 density          = 1000.0F;

    // How material properties from two contacting shapes combine. Defaults
    // match Bullet / PhysX conventions: average for friction (typical of
    // material physics), max for restitution (the bouncier surface wins,
    // matching player intuition for "this rubber ball is bouncy").
    CombineMode friction_combine    = CombineMode::Average;
    CombineMode restitution_combine = CombineMode::Max;
};

// API surface freeze pin (ADR-0062 §15). Layout = 4 floats + 2 bytes + 2 pad.
static_assert(sizeof(Material)  == 20, "Material must pack to 20 bytes");
static_assert(alignof(Material) ==  4, "Material alignment is 4 (largest member f32)");

} // namespace crd::eylem
