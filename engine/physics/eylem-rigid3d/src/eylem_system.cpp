// EylemSystem impl. Phase 3.1 v1b-c.

#include <crd/eylem_rigid3d/eylem_system.hpp>

#include <crd/eylem/components.hpp>
#include <crd/eylem_rigid3d/body_pool.hpp>
#include <crd/eylem_rigid3d/rigid3d_dispatch.hpp>
#include <crd/math/vec.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>

namespace crd::eylem_rigid3d
{
EylemSystem::EylemSystem(BodyPool& body_pool, const crd::eylem::PhysicsConfig& config) noexcept
    : m_body_pool(&body_pool)
    , m_config(config)
{
}

void EylemSystem::run(crd::scene::World& world)
{
    // One substep over every chunk of the pool in one kernel call (ADR-0062 §6: the integrate step of the solve,
    // ADR-0134 §5): each lane's pose goes to its prev columns first (the render-interpolation snapshot that
    // RigidBodyInterpolationSystem lerps from, Glenn Fiedler "Fix Your Timestep" §5), then each live body with
    // inv_mass > 0 integrates under the ADR-0063 contract (rigid3d_kernels.hpp). A body integrates once per substep
    // whether no entity, one or several reference it.
    const crd::f32 gravity[3] = {m_config.gravity.x.value, m_config.gravity.y.value, m_config.gravity.z.value};
    BodyPool::Storage& storage = m_body_pool->storage();
    rigid3d_kernels().table->step_bodies(storage.chunks().data(), storage.chunk_count(), gravity,
                                         m_config.fixed_dt.value);

    // Sync each referenced body's pose to its Transform. Iteration order is deterministic by EntityId per the v1f
    // Query DSL contract.
    for (auto&& [entity, transform, rbc] :
         world.query<crd::scene::Transform, crd::eylem::RigidBodyComponent>())
    {
        if (rbc.body_id.is_null())
        {
            continue;
        }
        if (!m_body_pool->contains(rbc.body_id))
        {
            continue;
        }

        const BodyPool::Pose pose = m_body_pool->read_pose(rbc.body_id);

        // Sync the pose back to Transform unless the entity opted out (e.g., Transform driven by an animation
        // curve). Use World's setters (not direct field writes) so TransformDirtyFlag is tagged and
        // TransformPropagation refreshes Transform.world in PreRender.
        if (rbc.sync_to_transform != 0U)
        {
            // World::set_translation currently takes raw Vec3f — bridge
            // through to_raw_vec. Once World gains a typed overload (v0d
            // adoption C), this becomes a direct typed assign.
            world.set_translation(entity, crd::math::to_raw_vec(pose.position));
            world.set_rotation_quat(entity, pose.rotation);
        }
        else
        {
            // Still update the local Transform reference (the iteration
            // captures it by reference from the storage), so the next
            // system in the same phase sees the integrated pose without
            // routing through World setters. The caller has assumed
            // responsibility for whatever downstream propagation needs.
            // Both sides are now typed Vec3<Length32> — direct assign.
            transform.translation = pose.position;
            transform.rotation    = pose.rotation;
        }
    }
}

} // namespace crd::eylem_rigid3d
