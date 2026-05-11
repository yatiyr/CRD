#pragma once

// IPhysicsScene — the public eylem world contract.
//
// Implementations:
//   v1a: NullPhysicsScene (this slice; sufficient to link against)
//   v1b+: ScenePool family in crd-eylem-rigid3d (real impls)
//
// Per ADR-0062 §6, this is the interface; no third-party SDK wraps it.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/eylem/collider.hpp>
#include <crd/eylem/collision_filter.hpp>
#include <crd/eylem/joint.hpp>
#include <crd/eylem/mass_properties.hpp>
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
    // A collider is owned by exactly one body. Material is per-collider —
    // referenced by `Collider::material` (a MaterialId handle into the
    // scene's MaterialPool). v1a-material-c canonical contract.
    //
    // Typical scene-load path (cooker → loader → scene):
    //   1. Cooker preallocates materials via create_material()
    //   2. Cooker writes MaterialIds onto each Collider record
    //   3. Loader streams Colliders → add_collider(body, collider) reads
    //      the pre-set MaterialId, no further allocation needed.
    [[nodiscard]] virtual ColliderId add_collider(BodyId          body,
                                                  const Collider& collider)             = 0;

    // Convenience overload for runtime authoring code that wants to inline
    // both the collider AND the material in one call. Internally allocates
    // a pool slot via create_material(material), then forwards to the
    // canonical 2-arg overload with a copy of `collider` whose `material`
    // field has been replaced. NOT virtual — every impl gets the same
    // semantics for free.
    [[nodiscard]] ColliderId add_collider(BodyId          body,
                                          const Collider& collider,
                                          const Material& material)
    {
        Collider c    = collider;
        c.material    = create_material(material);
        return add_collider(body, c);
    }

    virtual void                     remove_collider(ColliderId id)                     = 0;
    [[nodiscard]] virtual bool       has_collider(ColliderId id) const noexcept         = 0;

    // ---- Material pool (ADR-0069 §3 + §11) ------------------------------
    // Scene-owned MaterialPool storage. `MaterialId::default_material()`
    // is always valid (slot 1, allocated at scene construction). v1a-
    // material-c lands per-collider `Collider::material` (MaterialId);
    // v1a-material-d lands mass derivation from collider volume × density.
    //
    // create_material — append-only allocation; returns the assigned
    //   MaterialId (or MaterialId::null() at pool capacity).
    // update_material — in-place mutation; the handle stays stable. No-op
    //   if `id` is invalid (null / out-of-range / wrong generation).
    // material — read access; returns the default material for invalid
    //   ids so callers do not need to null-check on the read path.
    // has_material — strict validity check (false for null + invalid).
    [[nodiscard]] virtual MaterialId      create_material(const Material& material)               = 0;
    virtual void                          update_material(MaterialId id, const Material& material) noexcept = 0;
    [[nodiscard]] virtual const Material& material(MaterialId id) const noexcept                  = 0;
    [[nodiscard]] virtual bool            has_material(MaterialId id) const noexcept              = 0;

    // ---- Mass derivation (ADR-0069 §3 + §8 + §11) ----------------------
    // For the body identified by `id`, walk its colliders in ascending
    // ColliderId order and compute mass / COM / inertia diagonal from
    // each collider's analytic volume × material.density.
    //
    // Returns a zero-initialised result for an invalid `id` or for bodies
    // whose colliders are all zero-volume (planes, statics, deferred
    // mesh/hull/sdf cases). Callers may apply the result to the body
    // (typically: `inv_mass = 1/mass; inv_inertia = 1/diag`) or ignore it
    // when the user has authored explicit `inv_mass != 0` (which signals
    // "manual override; do not derive").
    [[nodiscard]] virtual DerivedMassProperties derive_body_mass(BodyId id) const = 0;

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

    // ---- Collision filtering — Tier 3 (excluded pairs) -----------------
    // Per ADR-0068 §10.4 Tier 3. Round-trips with URDF / SDF / MJCF
    // importers (v4 articulation slice). Internal storage = hash set of
    // (min(a.raw), max(b.raw)); O(1) test per pair. Unordered: exclude(a,b)
    // == exclude(b,a). Idempotent.
    virtual void                 exclude_pair(BodyId a, BodyId b) noexcept                = 0;
    virtual void                 include_pair(BodyId a, BodyId b) noexcept                = 0;
    [[nodiscard]] virtual bool   is_pair_excluded(BodyId a, BodyId b) const noexcept     = 0;

    // ---- Collision filtering — Tier 4 (ECS predicate) ------------------
    // One predicate per scene; pure function of substep-start state with
    // closed read set declared at registration. nullptr = remove the
    // predicate (universal pair allowance). Per ADR-0068 §10.4 Tier 4.
    virtual void set_collision_predicate(ICollisionPredicate* predicate) noexcept = 0;
    [[nodiscard]] virtual ICollisionPredicate* collision_predicate() const noexcept = 0;

    // ---- ContactModify hook (v1g+) -------------------------------------
    // One callback per scene; pure function with API-enforced no-external-
    // state. nullptr = no contact modification. Per ADR-0068 §10.6.
    virtual void set_contact_modify_callback(IContactModifyCallback* callback) noexcept = 0;
    [[nodiscard]] virtual IContactModifyCallback* contact_modify_callback() const noexcept = 0;

    // ---- Contact / trigger event drains --------------------------------
    // Deferred ECS event-stream dispatch per ADR-0068 §10.5. Events
    // written during step() in PostPhysics phase, drained here in the
    // user phase that follows. Sort key:
    // (min(body_a, body_b), max(body_a, body_b), kind) — deterministic
    // delivery regardless of which fibre generated which contact.
    //
    // Drain semantics: the returned span is valid until the next step()
    // call (events live in scene-managed scratch from PhysicsConfig::
    // solver_scratch). Caller may iterate but must not retain pointers
    // past the next step.
    [[nodiscard]] virtual crd::containers::ConstSpan<ContactEvent> drain_contact_events() noexcept = 0;
    [[nodiscard]] virtual crd::containers::ConstSpan<TriggerEvent> drain_trigger_events() noexcept = 0;

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
