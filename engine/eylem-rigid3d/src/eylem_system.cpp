// EylemSystem impl. Phase 3.1 v1b-c.

#include <crd/eylem_rigid3d/eylem_system.hpp>

#include <crd/eylem/components.hpp>
#include <crd/eylem/rigid_body.hpp>
#include <crd/eylem_rigid3d/body_pool.hpp>
#include <crd/math/quat.hpp>
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
    const crd::units::Duration32                    dt      = m_config.fixed_dt;
    const crd::math::Vec3<crd::units::Acceleration32> gravity = m_config.gravity;

    // Render-interpolation snapshot. Captures the integrator's CURRENT
    // pose (the one the renderer most recently interpolated TOWARDS)
    // into the prev columns before this substep advances state. The
    // RigidBodyInterpolationSystem (PreRender, variable-rate) lerps
    // prev↔curr by alpha = world.fixed_step_alpha(fixed_dt) and writes
    // the result into Transform — Glenn Fiedler "Fix Your Timestep" §5.
    //
    // O(chunk_count) — full SIMD column copy across the body storage.
    // Touches free lanes too; harmless because live[lane]==0 guards
    // every consumer.
    m_body_pool->snapshot_state_to_prev();

    // Iterate (Transform, RigidBodyComponent) pairs. Iteration order is
    // deterministic by EntityId per the v1f Query DSL contract; per-entity
    // body integration is a pure function of (state, dt, gravity) under
    // the ADR-0063 deterministic-FP contract.
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

        crd::eylem::RigidBody body = m_body_pool->read(rbc.body_id);

        // Static (inv_mass == 0) bodies do not integrate. Honours the
        // RigidBody convention from rigid_body.hpp:60 — `inv_mass = 0`
        // marks the body as effectively static for the solver.
        if (body.inv_mass.value > 0.0F)
        {
            // Linear: v += g·dt, p += v·dt. End-to-end typed under
            // ADR-0078 §3 D20 — `Acceleration * Time = Velocity` and
            // `Velocity * Time = Length` close at compile time. Damping
            // factor is a dimensionless rate (1/s · s = 1); compute it
            // raw and apply via Vec<T> * scalar.
            body.linear_velocity += gravity * dt;
            const crd::f32 lin_damp_factor = 1.0F - body.linear_damping * dt.value;
            body.linear_velocity = body.linear_velocity * lin_damp_factor;

            body.position += body.linear_velocity * dt;

            // Angular: ω damping, then small-angle quaternion update.
            //   q_new = normalize(q + 0.5 · dt · (ω_quat · q))
            // where ω_quat = (ωx, ωy, ωz, 0). Standard explicit Euler on
            // the quaternion derivative q̇ = 0.5 · ω_quat · q. The Quat
            // representation is unit-norm and dimensionless; angular
            // velocity escapes via `.value` at the boundary into the
            // dimensionless quaternion arithmetic.
            const crd::f32 ang_damp_factor = 1.0F - body.angular_damping * dt.value;
            body.angular_velocity = body.angular_velocity * ang_damp_factor;

            const crd::f32 half_dt = 0.5F * dt.value;
            const crd::math::Quatf omega_q{
                body.angular_velocity.x.value,
                body.angular_velocity.y.value,
                body.angular_velocity.z.value,
                0.0F};
            const crd::math::Quatf q_dot = omega_q * body.rotation;
            crd::math::Quatf       q_new{
                body.rotation.x + half_dt * q_dot.x,
                body.rotation.y + half_dt * q_dot.y,
                body.rotation.z + half_dt * q_dot.z,
                body.rotation.w + half_dt * q_dot.w};
            (void)crd::math::try_normalize(q_new);
            body.rotation = q_new;

            // INTEGRATOR write — only curr columns. The prev columns
            // were captured by snapshot_state_to_prev() above and must
            // remain frozen for the InterpolationSystem to lerp from.
            // Calling write() (teleport semantics) here would clobber
            // prev with the just-integrated curr and the renderer would
            // see no motion regardless of alpha.
            m_body_pool->write_curr_only(rbc.body_id, body);
        }

        // Sync integrated pose back to Transform unless the entity opted
        // out (e.g., Transform driven by an animation curve). Use World's
        // setters (not direct field writes) so TransformDirtyFlag is
        // tagged and TransformPropagation refreshes Transform.world in
        // PreRender.
        if (rbc.sync_to_transform != 0U)
        {
            // World::set_translation currently takes raw Vec3f — bridge
            // through to_raw_vec. Once World gains a typed overload (v0d
            // adoption C), this becomes a direct typed assign.
            world.set_translation(entity, crd::math::to_raw_vec(body.position));
            world.set_rotation_quat(entity, body.rotation);
        }
        else
        {
            // Still update the local Transform reference (the iteration
            // captures it by reference from the storage), so the next
            // system in the same phase sees the integrated pose without
            // routing through World setters. The caller has assumed
            // responsibility for whatever downstream propagation needs.
            // Both sides are now typed Vec3<Length32> — direct assign.
            transform.translation = body.position;
            transform.rotation    = body.rotation;
        }
    }
}

} // namespace crd::eylem_rigid3d
