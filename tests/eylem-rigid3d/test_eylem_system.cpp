// Phase 3.1 v1b-c — EylemSystem (ECS-driven motion integration) tests.
//
// Three scenarios per the phase plan:
//   1. Component registration round-trip — RigidBodyComponent /
//      ColliderComponent register cleanly with SparseSet hint and the
//      world reports them; size + alignment pins are static_asserts in
//      the header.
//   2. Integration — fixed-step a body under gravity for N steps;
//      assert Transform.translation.y descends by ~½·g·t² (closed form,
//      explicit Euler is exact on linear motion under constant gravity
//      to within FP precision).
//   3. Schedule integration — EylemSystem reports
//      SchedulePhase::Physics + fixed_step()=true; under
//      `World::step_fixed`, the integrator runs the expected substep
//      count for a given (dt, fixed_dt) pair.
//
// Static-only design: no jobs::init / shutdown needed because EylemSystem
// is a single-threaded serial system in v1b-c (no jobs::run inside its
// run()). When v1g island parallelism lands, the test fixture will need
// a Catch listener pattern like tests/resources/test_resource_manager.cpp.

#include <catch2/catch_test_macros.hpp>

#include <crd/eylem/components.hpp>
#include <crd/eylem/physics_config.hpp>
#include <crd/eylem/rigid_body.hpp>
#include <crd/eylem_rigid3d/body_pool.hpp>
#include <crd/eylem_rigid3d/eylem_system.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/transform_propagation.hpp>
#include <crd/scene/world.hpp>

#include <cmath>
#include <memory>

using crd::eylem::ColliderComponent;
using crd::eylem::PhysicsConfig;
using crd::eylem::RigidBody;
using crd::eylem::RigidBodyComponent;
using crd::eylem_rigid3d::BodyPool;
using crd::eylem_rigid3d::EylemSystem;

TEST_CASE("eylem v1b-c RigidBodyComponent + ColliderComponent register with SparseSet hint",
          "[eylem][v1b-c][components]")
{
    crd::scene::World world{crd::memory::default_allocator()};

    // SparseSet hint per phase plan (rigid bodies are sparse vs total
    // entity count; lifecycle is dominated by add/remove not iteration).
    const auto rbc_id = world.register_component<RigidBodyComponent>(
        crd::scene::StorageHint::SparseSet);
    const auto cc_id = world.register_component<ColliderComponent>(
        crd::scene::StorageHint::SparseSet);

    REQUIRE_FALSE(rbc_id.is_null());
    REQUIRE_FALSE(cc_id.is_null());
    REQUIRE(world.component_id<RigidBodyComponent>().raw == rbc_id.raw);
    REQUIRE(world.component_id<ColliderComponent>().raw == cc_id.raw);

    // Round-trip: spawn entity with both components carrying explicit
    // ids, read them back via has_component / get_component.
    const auto e = world.spawn();
    RigidBodyComponent rbc{};
    rbc.body_id = crd::eylem::BodyId::make(7U, 1U);
    rbc.sync_to_transform = 1U;
    world.add_component(e, rbc);

    ColliderComponent cc{};
    cc.collider_id = crd::eylem::ColliderId::make(3U, 1U);
    world.add_component(e, cc);

    REQUIRE(world.has_component<RigidBodyComponent>(e));
    REQUIRE(world.has_component<ColliderComponent>(e));
    const auto* rbc_read = world.get_component<RigidBodyComponent>(e);
    REQUIRE(rbc_read != nullptr);
    REQUIRE(rbc_read->body_id.raw == crd::eylem::BodyId::make(7U, 1U).raw);
    REQUIRE(rbc_read->sync_to_transform == 1U);

    const auto* cc_read = world.get_component<ColliderComponent>(e);
    REQUIRE(cc_read != nullptr);
    REQUIRE(cc_read->collider_id.raw == crd::eylem::ColliderId::make(3U, 1U).raw);
}

TEST_CASE("eylem v1b-c EylemSystem reports Physics phase + fixed_step()",
          "[eylem][v1b-c][system]")
{
    BodyPool pool{crd::memory::default_allocator(), 64U};
    PhysicsConfig cfg{};
    EylemSystem system{pool, cfg};

    REQUIRE(system.phase()       == crd::scene::SchedulePhase::Physics);
    REQUIRE(system.fixed_step()  == true);
    REQUIRE(system.name()        == crd::containers::StringView{"EylemSystem"});
}

TEST_CASE("eylem v1b-c EylemSystem integrates motion under gravity",
          "[eylem][v1b-c][system][integration]")
{
    // Earth gravity. fixed_dt = 1/60. 60 substeps = 1.0 s of simulated
    // time. Closed form for explicit-Euler under constant gravity from
    // rest:
    //
    //   v(t) = v0 + g·t          → v(1) = -9.81 m/s
    //   p(t) = p0 + v0·t + ½·g·t²
    //                              → Δp = ½ · -9.81 · 1.0² = -4.905 m
    //                                (modulo damping — disabled below)
    //
    // The integrator is explicit Euler, not the closed form, so the
    // discrete result accumulates a small +½·g·dt² offset per step. With
    // dt = 1/60 and 60 steps that's a known +0.0817 m delta from the
    // continuous formula. We assert against the discrete-Euler closed
    // form: Δp = -½ · g · dt · (N · dt + dt) = -g · dt² · N · (N+1) / 2.
    constexpr crd::f32 dt    = 1.0F / 60.0F;
    constexpr crd::u32 nstep = 60U;
    constexpr crd::f32 g     = -9.81F;

    BodyPool      pool{crd::memory::default_allocator(), 64U};
    PhysicsConfig cfg{};
    cfg.fixed_dt = dt;
    cfg.gravity  = crd::math::Vec3f{0.0F, g, 0.0F};
    EylemSystem  system{pool, cfg};

    crd::scene::World world{crd::memory::default_allocator()};
    world.register_component<crd::scene::Transform>(crd::scene::StorageHint::Archetype);
    world.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
    world.register_component<RigidBodyComponent>(crd::scene::StorageHint::SparseSet);

    // Body: at origin, dynamic (inv_mass = 1), zero damping (so we can
    // assert the closed form).
    RigidBody body{};
    body.position        = {0.0F, 0.0F, 0.0F};
    body.inv_mass        = 1.0F;
    body.linear_damping  = 0.0F;
    body.angular_damping = 0.0F;
    const crd::eylem::BodyId body_id = pool.insert(body);
    REQUIRE_FALSE(body_id.is_null());

    const auto e = world.spawn();
    world.add_component(e, crd::scene::Transform{});
    RigidBodyComponent rbc{};
    rbc.body_id = body_id;
    world.add_component(e, rbc);

    // Run N substeps directly (bypass the schedule for unit-test
    // isolation; the schedule path is exercised by the Schedule test).
    for (crd::u32 i = 0U; i < nstep; ++i)
    {
        system.run(world);
    }

    // Discrete explicit-Euler from rest under constant gravity:
    //   after step k: v_k = g·k·dt
    //                p_k = sum_{i=1..k} v_i · dt = g·dt² · sum_{i=1..k} i
    //                    = g·dt² · k·(k+1)/2
    const crd::f32 expected_y = g * dt * dt * static_cast<crd::f32>(nstep)
                                * static_cast<crd::f32>(nstep + 1U) * 0.5F;

    const auto* tr = world.get_component<crd::scene::Transform>(e);
    REQUIRE(tr != nullptr);
    REQUIRE(std::fabs(tr->translation.y - expected_y) < 1e-3F);
    REQUIRE(tr->translation.x == 0.0F);
    REQUIRE(tr->translation.z == 0.0F);

    // Pool state should mirror what got synced to Transform.
    const RigidBody read = pool.read(body_id);
    REQUIRE(std::fabs(read.position.y - expected_y) < 1e-3F);
    REQUIRE(std::fabs(read.linear_velocity.y - g * dt * static_cast<crd::f32>(nstep)) < 1e-3F);
}

TEST_CASE("eylem v1b-c EylemSystem under World::step_fixed runs expected substep count",
          "[eylem][v1b-c][system][schedule]")
{
    // Use exactly-representable ratios: fixed_dt = 1/64 (exact in IEEE-754
    // binary), frame_dt = 10/64 (exact). Avoids the floor(accum / dt)
    // boundary slop that 1/60 introduces (10 * 1/60 in FP rounds slightly
    // below 10 → floor = 9 → off-by-one substep). v1f scheduler may add
    // a deterministic-mode flag with epsilon-snap; for v1b-c the test
    // simply uses a power-of-two ratio.
    constexpr crd::f32 dt          = 1.0F / 64.0F;
    constexpr crd::f32 g           = -9.81F;
    constexpr crd::u32 nstep_seed  = 10U;
    constexpr crd::f32 frame_dt    = static_cast<crd::f32>(nstep_seed) * dt; // 10/64 exact

    BodyPool      pool{crd::memory::default_allocator(), 64U};
    PhysicsConfig cfg{};
    cfg.fixed_dt = dt;
    cfg.gravity  = crd::math::Vec3f{0.0F, g, 0.0F};

    crd::scene::World world{crd::memory::default_allocator()};
    world.register_component<crd::scene::Transform>(crd::scene::StorageHint::Archetype);
    world.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
    world.register_component<RigidBodyComponent>(crd::scene::StorageHint::SparseSet);
    world.register_system(std::make_unique<EylemSystem>(pool, cfg));

    RigidBody body{};
    body.position        = {0.0F, 0.0F, 0.0F};
    body.inv_mass        = 1.0F;
    body.linear_damping  = 0.0F;
    body.angular_damping = 0.0F;
    const crd::eylem::BodyId body_id = pool.insert(body);

    const auto e = world.spawn();
    world.add_component(e, crd::scene::Transform{});
    RigidBodyComponent rbc{};
    rbc.body_id = body_id;
    world.add_component(e, rbc);

    // step_fixed accumulates frame_dt against fixed_dt and runs the
    // EylemSystem floor(frame_dt / fixed_dt) times. Here that is 10
    // substeps. max_substeps = 32 ensures we don't get clamped.
    world.step_fixed(static_cast<crd::f64>(frame_dt),
                     static_cast<crd::f64>(dt),
                     /*max_substeps=*/32U);

    // Discrete-Euler closed form for 10 substeps under gravity from rest.
    const crd::f32 expected_y = g * dt * dt * static_cast<crd::f32>(nstep_seed)
                                * static_cast<crd::f32>(nstep_seed + 1U) * 0.5F;

    const auto* tr = world.get_component<crd::scene::Transform>(e);
    REQUIRE(tr != nullptr);
    REQUIRE(std::fabs(tr->translation.y - expected_y) < 1e-4F);
}

TEST_CASE("eylem v1b-c static body (inv_mass==0) does not integrate",
          "[eylem][v1b-c][system]")
{
    BodyPool      pool{crd::memory::default_allocator(), 64U};
    PhysicsConfig cfg{};
    cfg.fixed_dt = 1.0F / 60.0F;
    cfg.gravity  = crd::math::Vec3f{0.0F, -9.81F, 0.0F};
    EylemSystem  system{pool, cfg};

    crd::scene::World world{crd::memory::default_allocator()};
    world.register_component<crd::scene::Transform>(crd::scene::StorageHint::Archetype);
    world.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
    world.register_component<RigidBodyComponent>(crd::scene::StorageHint::SparseSet);

    // Static body — inv_mass = 0.
    RigidBody body{};
    body.position = {1.0F, 2.0F, 3.0F};
    body.inv_mass = 0.0F;
    const crd::eylem::BodyId body_id = pool.insert(body);

    const auto e = world.spawn();
    world.add_component(e, crd::scene::Transform{});
    RigidBodyComponent rbc{};
    rbc.body_id = body_id;
    world.add_component(e, rbc);

    for (crd::u32 i = 0U; i < 60U; ++i)
    {
        system.run(world);
    }

    const RigidBody read = pool.read(body_id);
    REQUIRE(read.position.x == 1.0F);
    REQUIRE(read.position.y == 2.0F);
    REQUIRE(read.position.z == 3.0F);
    REQUIRE(read.linear_velocity.y == 0.0F);
}
