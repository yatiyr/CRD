// RigidBodyInterpolationSystem impl. Phase 3.1 v1b-e.

#include <crd/eylem_rigid3d/interpolation_system.hpp>

#include <crd/eylem/components.hpp>
#include <crd/eylem/rigid_body.hpp>
#include <crd/eylem_rigid3d/body_pool.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/vec.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>

namespace crd::eylem_rigid3d
{
// Interpolation primitives live in crd-math:
//   crd::math::lerp(Vec3f, Vec3f, f32)  → vec.hpp:514
//   crd::math::nlerp(Quatf, Quatf, f32) → quat.hpp:270 (includes dot<0
//     short-arc fix-up: at small angles between a and b — the regime
//     fixed-step physics produces — nlerp is visually indistinguishable
//     from slerp at a fraction of the cost, no acos/sin/sincos).
// They're the canonical engine-wide primitives; we consume them here.

RigidBodyInterpolationSystem::RigidBodyInterpolationSystem(BodyPool& body_pool,
                                                           const crd::eylem::PhysicsConfig& config) noexcept
    : m_body_pool(&body_pool)
    , m_config(config)
{
}

void RigidBodyInterpolationSystem::run(crd::scene::World& world)
{
    // alpha ∈ [0, 1) — fraction of the way from the LAST integrated
    // pose toward the NEXT one. World owns the accumulator.
    const crd::f64 alpha_d = world.fixed_step_alpha(static_cast<crd::f64>(m_config.fixed_dt.value));
    const crd::f32 alpha   = static_cast<crd::f32>(alpha_d);

    for (auto&& [entity, transform, rbc] :
         world.query<crd::scene::Transform, crd::eylem::RigidBodyComponent>())
    {
        // User opted out of having physics drive Transform; honour it.
        // Symmetric with EylemSystem's sync_to_transform check.
        if (rbc.sync_to_transform == 0U)
        {
            continue;
        }
        if (rbc.body_id.is_null())
        {
            continue;
        }
        if (!m_body_pool->contains(rbc.body_id))
        {
            continue;
        }

        // Read both states. The full RigidBody read is wasteful for
        // pure pos+rot interpolation (one extra chunk-cache line of
        // velocity / inertia data); the v1c+ broadphase will
        // optimise this with a typed `read_pose` helper that returns
        // only pos+rot from a single chunk lookup. For v1b-e's small
        // body counts this cost is negligible.
        const crd::eylem::RigidBody  curr = m_body_pool->read(rbc.body_id);
        const BodyPool::PrevState    prev = m_body_pool->read_prev(rbc.body_id);

        // crd::math::lerp is MathScalar-only by ADR-0078 §2 D2 (reductions
        // and t-parameter scaling stay raw). Bridge typed Length32 vecs
        // through to_raw_vec → lerp → re-tag for the World setter.
        const crd::math::Vec3f prev_pos_raw = crd::math::to_raw_vec(prev.position);
        const crd::math::Vec3f curr_pos_raw = crd::math::to_raw_vec(curr.position);
        const crd::math::Vec3f interp_pos   = crd::math::lerp(prev_pos_raw, curr_pos_raw, alpha);
        const crd::math::Quatf interp_rot   = crd::math::nlerp(prev.rotation, curr.rotation, alpha);

        // Route through World setters so TransformDirtyFlag is tagged
        // and TransformPropagation (registered AFTER us in PreRender)
        // refreshes Transform.world this frame.
        world.set_translation(entity, interp_pos);
        world.set_rotation_quat(entity, interp_rot);

        // Suppress -Wunused-but-set warnings; transform reference is
        // not read here because the World setters own the canonical
        // path that also tags dirty.
        (void)transform;
    }
}

} // namespace crd::eylem_rigid3d
