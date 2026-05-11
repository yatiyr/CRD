#pragma once

// EylemSystem — first eylem entity integrating motion in the ECS world.
// Phase 3.1 v1b-c (per docs/phases/phase-3.1-eylem.md §v1b-c).
//
// Architecture decisions locked at v1b-c:
//
//   1. Lives in `crd-eylem-rigid3d` (concrete-pool module), not `crd-eylem`
//      (interface module), because it touches BodyPool directly. Components
//      it iterates (`RigidBodyComponent` from `crd-eylem`) ARE in the
//      interface module so other systems / cookers / tooling can register
//      them without depending on rigid3d.
//
//   2. Phase = `SchedulePhase::Physics`. fixed_step() = true. The schedule
//      drives substep cadence via `World::step_fixed(dt, fixed_dt,
//      max_substeps)`; the System reads the per-step delta from the
//      `PhysicsConfig` it was constructed with (caller's responsibility to
//      pass the same `fixed_dt` to the schedule).
//
//   3. v1b-c integrator is HONESTLY MINIMAL — no collision, no constraints,
//      no broadphase. Bodies fall through the world. The frame body is:
//
//         linvel  += gravity · dt
//         pos     += linvel  · dt
//         rot      = small_angle_quat_step(rot, angvel, dt)
//
//      The full SI solver lands at v1e (per phase plan); the broadphase at
//      v1c. v1b-c proves end-to-end ECS↔BodyPool integration works.
//
//   4. Writes back to `Transform` via `World::set_translation` /
//      `set_rotation_quat` so `TransformPropagation` picks up the change
//      in `PreRender`. Direct write to `Transform.translation` would
//      bypass dirty-marking and the world matrix would not refresh.
//      Per-entity opt-out via `RigidBodyComponent::sync_to_transform = 0`.
//
//   5. Iteration order is deterministic by construction — `Query<Transform,
//      RigidBodyComponent>` visits entities in EntityId order (per the v1f
//      query DSL contract); the per-entity integration is a pure function
//      of (state, dt, gravity); the BodyPool's `write_lane` does scalar
//      stores in a fixed lane order. Per ADR-0063 §4.

#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/eylem/physics_config.hpp>
#include <crd/scene/system.hpp>

namespace crd::eylem_rigid3d
{
class BodyPool;
}

namespace crd::eylem_rigid3d
{
class EylemSystem final : public crd::scene::ISystem
{
public:
    // Constructed against a body pool + a PhysicsConfig (gravity + fixed_dt
    // are read from this snapshot — re-construct the system if the config
    // changes mid-run; v1b-c is not designed for per-frame config swaps).
    EylemSystem(BodyPool& body_pool, const crd::eylem::PhysicsConfig& config) noexcept;

    [[nodiscard]] crd::scene::SchedulePhase phase() const override
    {
        return crd::scene::SchedulePhase::Physics;
    }

    [[nodiscard]] bool fixed_step() const noexcept override { return true; }

    [[nodiscard]] crd::containers::StringView name() const override
    {
        return crd::containers::StringView{"EylemSystem"};
    }

    // Run one fixed substep. The schedule guarantees `fixed_dt` cadence
    // when invoked via `step_fixed`. Iterates entities with both
    // `Transform` and `RigidBodyComponent`, integrates motion, syncs the
    // result back to `Transform` (per the `sync_to_transform` flag).
    void run(crd::scene::World& world) override;

private:
    BodyPool*                 m_body_pool = nullptr;
    crd::eylem::PhysicsConfig m_config{};
};

} // namespace crd::eylem_rigid3d
