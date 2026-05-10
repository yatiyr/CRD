#pragma once

// IPhysicsScene — the public eylem world contract.
//
// Implementations:
//   v1a: NullPhysicsScene (this slice; sufficient to link against)
//   v1b+: ScenePool family in crd-eylem-rigid3d (real impls)
//
// Per ADR-0062 §6, this is the interface; no third-party SDK wraps it.

#include <crd/core/types.hpp>
#include <crd/eylem/collider.hpp>
#include <crd/eylem/joint.hpp>
#include <crd/eylem/material.hpp>
#include <crd/eylem/physics_config.hpp>
#include <crd/eylem/rigid_body.hpp>
#include <crd/eylem/types.hpp>
#include <crd/math/vec.hpp>

#include <memory>
#include <optional>

namespace crd::eylem
{
// Result of a successful raycast. Returned in std::optional from raycast().
struct RaycastHit
{
    BodyId           body;
    ColliderId       collider;
    crd::math::Vec3f point{0.0F, 0.0F, 0.0F};   // world-space hit point
    crd::math::Vec3f normal{0.0F, 1.0F, 0.0F};  // world-space surface normal
    crd::f32         distance = 0.0F;            // along the ray, in metres
};

class IPhysicsScene
{
public:
    virtual ~IPhysicsScene() = default;

    // ---- Configuration --------------------------------------------------
    [[nodiscard]] virtual const PhysicsConfig& config() const noexcept = 0;
    virtual void set_gravity(crd::math::Vec3f g) noexcept            = 0;
    [[nodiscard]] virtual crd::math::Vec3f gravity() const noexcept  = 0;

    // ---- Body management ------------------------------------------------
    [[nodiscard]] virtual BodyId add_body(const RigidBody& body)            = 0;
    virtual void                 remove_body(BodyId id)                     = 0;
    [[nodiscard]] virtual bool   has_body(BodyId id) const noexcept         = 0;
    [[nodiscard]] virtual crd::usize body_count() const noexcept            = 0;

    // ---- Collider attachment -------------------------------------------
    // A collider is owned by exactly one body. Material is per-collider.
    [[nodiscard]] virtual ColliderId add_collider(BodyId          body,
                                                  const Collider& collider,
                                                  const Material& material)             = 0;
    virtual void                     remove_collider(ColliderId id)                     = 0;
    [[nodiscard]] virtual bool       has_collider(ColliderId id) const noexcept         = 0;

    // ---- Joint management ----------------------------------------------
    [[nodiscard]] virtual JointId add_joint(const Joint& joint) = 0;
    virtual void                  remove_joint(JointId id)      = 0;
    [[nodiscard]] virtual bool    has_joint(JointId id) const noexcept = 0;

    // ---- State accessors -----------------------------------------------
    // Read body state. Returns a default-constructed RigidBody if id is not
    // valid (callers should check has_body first; the no-throw signature
    // matches the determinism contract — no exceptions in the hot path).
    [[nodiscard]] virtual RigidBody body_state(BodyId id) const = 0;
    virtual void                    set_body_state(BodyId id, const RigidBody& state) = 0;

    // Force / torque application. Accumulated until next step(); cleared at
    // step end.
    virtual void apply_force(BodyId id, crd::math::Vec3f force)                          = 0;
    virtual void apply_torque(BodyId id, crd::math::Vec3f torque)                        = 0;
    virtual void apply_impulse(BodyId id, crd::math::Vec3f impulse, crd::math::Vec3f world_pos) = 0;

    // ---- Stepping -------------------------------------------------------
    // Advance the simulation by dt seconds. Implementations are expected to
    // step in fixed-dt sub-frames per config().fixed_dt; passing arbitrary
    // dt is supported but loses determinism.
    virtual void step(crd::f32 dt) = 0;

    // ---- Scene queries (v1a stubs the surface; v1h delivers full impl) -
    [[nodiscard]] virtual std::optional<RaycastHit> raycast(crd::math::Vec3f origin,
                                                            crd::math::Vec3f direction,
                                                            crd::f32         max_distance) const = 0;
};

// Factory for the v1a null implementation. v1b+ adds real scene factories
// alongside this one (NullPhysicsScene stays available for tests + tools).
[[nodiscard]] std::unique_ptr<IPhysicsScene> make_null_physics_scene(const PhysicsConfig& config);

} // namespace crd::eylem
