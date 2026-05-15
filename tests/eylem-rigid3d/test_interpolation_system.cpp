// Phase 3.1 v1b-e — RigidBodyInterpolationSystem tests.
//
// The interpolation bridge is the canonical Glenn Fiedler "Fix Your
// Timestep" §5 pattern: the integrator snapshots curr→prev at the
// start of each substep, then writes new curr; the interpolator (this
// system) reads both and lerps by alpha = accumulator / fixed_dt.
//
// Test plan (per docs/phases/phase-3.1-eylem.md §v1b-e):
//   1. Schedule wiring — phase = PreRender, fixed_step() = false.
//   2. alpha = 0  → Transform = prev pose (post-integration sees curr,
//                   accumulator drained to 0 → renderer shows the
//                   PRIOR snapshot as Fiedler intends; one frame of
//                   latency is the standard tradeoff).
//   3. alpha = 0.5 → Transform = midpoint of prev↔curr (linear pos,
//                    nlerp rot).
//   4. Quaternion short-arc fix-up — prev=q, curr=-q (same orientation,
//                                    opposite hemisphere) → result
//                                    stays at q (no flip-around).
//   5. sync_to_transform = 0 → Transform untouched (user owns it).
//   6. Null body_id / stale handle → no crash, no write.

#include <crd/eylem/components.hpp>
#include <crd/eylem/physics_config.hpp>
#include <crd/eylem/rigid_body.hpp>
#include <crd/eylem_rigid3d/body_pool.hpp>
#include <crd/eylem_rigid3d/eylem_system.hpp>
#include <crd/eylem_rigid3d/interpolation_system.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>

using crd::eylem::PhysicsConfig;
using crd::eylem::RigidBody;
using crd::eylem::RigidBodyComponent;
using crd::eylem_rigid3d::BodyPool;
using crd::eylem_rigid3d::EylemSystem;
using crd::eylem_rigid3d::RigidBodyInterpolationSystem;

namespace
{
// Per-test fixture: a 4 MB TLSF heap labelled "interp-test" so leaks
// or invalid frees show up named in any debug-allocator dump. Named
// allocators are project standard (memory feedback 2026-05-11).
class InterpFixture
{
public:
    InterpFixture()
        : m_alloc(crd::usize{4U} << 20U, /*parent=*/nullptr, "interp-test"), m_pool(&m_alloc, /*max_bodies=*/64U),
          m_world(&m_alloc)
    {
        m_world.register_component<crd::scene::Transform>(crd::scene::StorageHint::Archetype);
        m_world.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
        m_world.register_component<RigidBodyComponent>(crd::scene::StorageHint::SparseSet);
    }

    crd::memory::TlsfAllocator m_alloc;
    BodyPool m_pool;
    crd::scene::World m_world;
};
} // namespace

TEST_CASE("eylem v1b-e RigidBodyInterpolationSystem reports PreRender + variable", "[eylem][v1b-e][interp][system]")
{
    InterpFixture fix;
    PhysicsConfig cfg{};
    cfg.fixed_dt = 1.0F / 60.0F;
    RigidBodyInterpolationSystem sys{fix.m_pool, cfg};

    REQUIRE(sys.phase() == crd::scene::SchedulePhase::PreRender);
    REQUIRE(sys.fixed_step() == false);
    REQUIRE(sys.name() == crd::containers::StringView{"RigidBodyInterpolationSystem"});
}

TEST_CASE("eylem v1b-e interpolator at alpha=0 emits prev pose", "[eylem][v1b-e][interp][lerp]")
{
    InterpFixture fix;
    PhysicsConfig cfg{};
    cfg.fixed_dt = 1.0F / 64.0F;
    cfg.gravity = crd::math::Vec3f{0.0F, -9.81F, 0.0F};

    EylemSystem integ{fix.m_pool, cfg};
    RigidBodyInterpolationSystem interp{fix.m_pool, cfg};

    // Body at origin, dynamic.
    RigidBody body{};
    body.position = {0.0F, 0.0F, 0.0F};
    body.inv_mass = 1.0F;
    const auto body_id = fix.m_pool.insert(body);
    REQUIRE_FALSE(body_id.is_null());

    const auto e = fix.m_world.spawn();
    fix.m_world.add_component(e, crd::scene::Transform{});
    RigidBodyComponent rbc{};
    rbc.body_id = body_id;
    rbc.sync_to_transform = 1U;
    fix.m_world.add_component(e, rbc);

    // Integrate one substep: prev = origin, curr = origin + ½·g·dt²
    // (one explicit-Euler step under gravity from rest:
    //  v += g·dt → v = (0, g·dt, 0)
    //  p += v·dt → p = (0, g·dt², 0))
    integ.run(fix.m_world);

    // Check pool-level invariants first: prev=origin, curr is one step.
    const RigidBody curr_body = fix.m_pool.read(body_id);
    const auto prev = fix.m_pool.read_prev(body_id);
    REQUIRE(prev.position.y == 0.0F);
    const crd::f32 expected_curr_y = cfg.gravity.y * cfg.fixed_dt * cfg.fixed_dt;
    // Tolerance accounts for one float multiply-and-add per axis under
    // the explicit-Euler integrator (~ULP-of-result rounding); curr_y
    // is on the order of -2.4e-3 m so 1e-5 absolute is conservative.
    REQUIRE(std::fabs(curr_body.position.y - expected_curr_y) < 1e-5F);

    // Step the world by exactly N·fixed_dt so accumulator → 0 → alpha = 0.
    // We bypass step_fixed (which would re-run the integrator) and drive
    // the interpolation system directly; the world.fixed_step_alpha()
    // accessor reads the accumulator that step_fixed maintains. With a
    // brand-new world the accumulator is already 0 → alpha = 0 → the
    // interpolator emits prev.
    REQUIRE(fix.m_world.fixed_step_accumulator() == 0.0);

    interp.run(fix.m_world);

    const auto* tr = fix.m_world.get_component<crd::scene::Transform>(e);
    REQUIRE(tr != nullptr);
    // Transform should hold the PREV pose (origin), not the curr one.
    REQUIRE(std::fabs(tr->translation.y.value - 0.0F) < 1e-6F);
}

TEST_CASE("eylem v1b-e interpolator at alpha=0.5 emits midpoint", "[eylem][v1b-e][interp][lerp]")
{
    InterpFixture fix;
    PhysicsConfig cfg{};
    cfg.fixed_dt = 1.0F / 64.0F;
    cfg.gravity = crd::math::Vec3f{0.0F, 0.0F, 0.0F}; // no gravity for this test

    RigidBodyInterpolationSystem interp{fix.m_pool, cfg};

    // Manually construct prev/curr via two writes:
    //   write(curr=A) → prev=A, curr=A      (teleport semantics)
    //   write_curr_only(curr=B) → prev=A, curr=B (integrator semantics)
    // Then alpha=0.5 should yield (A+B)/2.
    RigidBody a{};
    a.position = {1.0F, 2.0F, 3.0F};
    a.inv_mass = 1.0F;
    const auto body_id = fix.m_pool.insert(a);

    RigidBody b{};
    b.position = {5.0F, 6.0F, 7.0F};
    b.inv_mass = 1.0F;
    fix.m_pool.write_curr_only(body_id, b);

    // Sanity: pool now has prev=A, curr=B.
    const auto prev = fix.m_pool.read_prev(body_id);
    REQUIRE(prev.position.x == 1.0F);
    REQUIRE(prev.position.y == 2.0F);
    REQUIRE(prev.position.z == 3.0F);
    REQUIRE(fix.m_pool.read(body_id).position.x == 5.0F);

    const auto e = fix.m_world.spawn();
    fix.m_world.add_component(e, crd::scene::Transform{});
    RigidBodyComponent rbc{};
    rbc.body_id = body_id;
    rbc.sync_to_transform = 1U;
    fix.m_world.add_component(e, rbc);

    // Force alpha = 0.5 by stuffing the accumulator. World owns the
    // accumulator privately but exposes it via fixed_step_alpha; the
    // only public way to set it is through step_fixed. step_fixed(dt,
    // fixed_dt, max=0) advances the accumulator without running any
    // substep — exactly what we need.
    fix.m_world.step_fixed(0.5 * static_cast<crd::f64>(cfg.fixed_dt), static_cast<crd::f64>(cfg.fixed_dt),
                           /*max_substeps=*/0U);
    REQUIRE(std::fabs(fix.m_world.fixed_step_alpha(cfg.fixed_dt) - 0.5) < 1e-6);

    interp.run(fix.m_world);

    const auto* tr = fix.m_world.get_component<crd::scene::Transform>(e);
    REQUIRE(tr != nullptr);
    REQUIRE(std::fabs(tr->translation.x.value - 3.0F) < 1e-5F); // (1+5)/2
    REQUIRE(std::fabs(tr->translation.y.value - 4.0F) < 1e-5F); // (2+6)/2
    REQUIRE(std::fabs(tr->translation.z.value - 5.0F) < 1e-5F); // (3+7)/2
}

TEST_CASE("eylem v1b-e nlerp takes short arc when dot(prev,curr) < 0", "[eylem][v1b-e][interp][quat]")
{
    InterpFixture fix;
    PhysicsConfig cfg{};
    cfg.fixed_dt = 1.0F / 64.0F;

    RigidBodyInterpolationSystem interp{fix.m_pool, cfg};

    // Non-degenerate antipodal pair. prev = identity, curr = NEGATED
    // small rotation around Y. The negated form represents the SAME
    // orientation as the positive form, but lerping the raw components
    // would walk through (0,0,0,0)-ish — a near-perpendicular orientation
    // — before renormalising. With the dot<0 short-arc fix, the lerp
    // takes the geometrically short path and the result stays a small
    // perturbation from identity.
    //
    // theta = 0.2 rad → small but well above FP noise.
    const crd::f32 half = 0.1F; // theta/2
    const crd::f32 s = std::sin(half);
    const crd::f32 c = std::cos(half);

    RigidBody body{};
    body.position = {0.0F, 0.0F, 0.0F};
    body.rotation = crd::math::Quatf{0.0F, 0.0F, 0.0F, 1.0F}; // identity
    body.inv_mass = 1.0F;
    const auto body_id = fix.m_pool.insert(body);

    // curr = negated Y rotation: (0, -s, 0, -c). Same orientation as
    // (0, s, 0, c) but opposite hemisphere → dot(identity, curr) = -c < 0.
    RigidBody curr_body = body;
    curr_body.rotation = crd::math::Quatf{0.0F, -s, 0.0F, -c};
    fix.m_pool.write_curr_only(body_id, curr_body);

    const auto e = fix.m_world.spawn();
    fix.m_world.add_component(e, crd::scene::Transform{});
    RigidBodyComponent rbc{};
    rbc.body_id = body_id;
    rbc.sync_to_transform = 1U;
    fix.m_world.add_component(e, rbc);

    fix.m_world.step_fixed(0.5 * static_cast<crd::f64>(cfg.fixed_dt), static_cast<crd::f64>(cfg.fixed_dt),
                           /*max_substeps=*/0U);
    interp.run(fix.m_world);

    const auto* tr = fix.m_world.get_component<crd::scene::Transform>(e);
    REQUIRE(tr != nullptr);

    // With the short-arc fix the effective curr is +(0, s, 0, c) and
    // the midpoint lerp is (0, s/2, 0, (1+c)/2), then renormalised.
    // w stays POSITIVE and large (~1). Without the fix, lerp would
    // produce (0, -s/2, 0, (1-c)/2) ≈ (0, -0.05, 0, 0.005); after
    // normalising that lies almost entirely along -Y — a ~180° rotation
    // around Y, completely wrong.
    REQUIRE(tr->rotation.w > 0.9F);
    REQUIRE(tr->rotation.y > 0.0F);
    REQUIRE(std::fabs(tr->rotation.y) < 0.2F);
    // Magnitude must be ~1 (renormalisation succeeded).
    const crd::f32 mag = std::sqrt(tr->rotation.x * tr->rotation.x + tr->rotation.y * tr->rotation.y +
                                   tr->rotation.z * tr->rotation.z + tr->rotation.w * tr->rotation.w);
    REQUIRE(std::fabs(mag - 1.0F) < 1e-5F);
}

TEST_CASE("eylem v1b-e multi-substep flow: prev = pose_after_substep_1, curr = pose_after_substep_2",
          "[eylem][v1b-e][interp][multistep]")
{
    // Realistic frame: accumulator drives 2 fixed substeps with 0.5*dt
    // remainder. After step_fixed:
    //   - Substep 1: snapshot prev=initial → integrate → curr=pose_1
    //   - Substep 2: snapshot prev=pose_1  → integrate → curr=pose_2
    // accumulator left = 0.5*dt → alpha = 0.5 → interp output =
    // midpoint(pose_1, pose_2).
    //
    // Closed-form explicit-Euler from rest under gravity:
    //   pose_1.y = g·dt²       (one step)
    //   pose_2.y = 3·g·dt²     (two steps: 1·g·dt² + 2·g·dt²)
    //   midpoint = 2·g·dt²
    InterpFixture fix;
    PhysicsConfig cfg{};
    cfg.fixed_dt = 1.0F / 64.0F;
    cfg.gravity = crd::math::Vec3f{0.0F, -9.81F, 0.0F};

    RigidBody body{};
    body.position = {0.0F, 0.0F, 0.0F};
    body.inv_mass = 1.0F;
    body.linear_damping = 0.0F;
    body.angular_damping = 0.0F;
    const auto body_id = fix.m_pool.insert(body);

    const auto e = fix.m_world.spawn();
    fix.m_world.add_component(e, crd::scene::Transform{});
    RigidBodyComponent rbc{};
    rbc.body_id = body_id;
    rbc.sync_to_transform = 1U;
    fix.m_world.add_component(e, rbc);

    // Register the integrator + interpolator into the schedule so
    // step_fixed runs both in the right order: EylemSystem fixed × 2,
    // then InterpolationSystem variable × 1.
    fix.m_world.register_system(std::make_unique<EylemSystem>(fix.m_pool, cfg));
    fix.m_world.register_system(std::make_unique<RigidBodyInterpolationSystem>(fix.m_pool, cfg));

    const crd::f64 frame_dt = 2.5 * static_cast<crd::f64>(cfg.fixed_dt);
    fix.m_world.step_fixed(frame_dt, static_cast<crd::f64>(cfg.fixed_dt), /*max_substeps=*/4U);

    // Accumulator should be 0.5 · fixed_dt → alpha = 0.5.
    REQUIRE(std::fabs(fix.m_world.fixed_step_alpha(cfg.fixed_dt) - 0.5) < 1e-6);

    // Pool: prev = pose_1, curr = pose_2.
    const RigidBody curr = fix.m_pool.read(body_id);
    const auto prev = fix.m_pool.read_prev(body_id);
    const crd::f32 pose_1_y = cfg.gravity.y * cfg.fixed_dt * cfg.fixed_dt;        //  g·dt²
    const crd::f32 pose_2_y = 3.0F * cfg.gravity.y * cfg.fixed_dt * cfg.fixed_dt; // 3g·dt²
    REQUIRE(std::fabs(prev.position.y - pose_1_y) < 1e-5F);
    REQUIRE(std::fabs(curr.position.y - pose_2_y) < 1e-5F);

    // Transform.translation = lerp(prev, curr, 0.5) = 2·g·dt².
    const auto* tr = fix.m_world.get_component<crd::scene::Transform>(e);
    REQUIRE(tr != nullptr);
    const crd::f32 expected_y = 2.0F * cfg.gravity.y * cfg.fixed_dt * cfg.fixed_dt;
    REQUIRE(std::fabs(tr->translation.y.value - expected_y) < 1e-5F);
}

TEST_CASE("eylem v1b-e interpolator skips entities with sync_to_transform=0", "[eylem][v1b-e][interp][opt-out]")
{
    InterpFixture fix;
    PhysicsConfig cfg{};
    cfg.fixed_dt = 1.0F / 64.0F;
    RigidBodyInterpolationSystem interp{fix.m_pool, cfg};

    RigidBody body{};
    body.position = {1.0F, 2.0F, 3.0F};
    body.inv_mass = 1.0F;
    const auto body_id = fix.m_pool.insert(body);

    RigidBody curr_body = body;
    curr_body.position = {99.0F, 99.0F, 99.0F};
    fix.m_pool.write_curr_only(body_id, curr_body);

    const auto e = fix.m_world.spawn();
    crd::scene::Transform tr_seed{};
    tr_seed.translation = crd::math::from_raw_vec<crd::units::dim::Length>(crd::math::Vec3f{7.0F, 8.0F, 9.0F});
    fix.m_world.add_component(e, tr_seed);
    RigidBodyComponent rbc{};
    rbc.body_id = body_id;
    rbc.sync_to_transform = 0U; // user owns transform
    fix.m_world.add_component(e, rbc);

    fix.m_world.step_fixed(0.5 * static_cast<crd::f64>(cfg.fixed_dt), static_cast<crd::f64>(cfg.fixed_dt),
                           /*max_substeps=*/0U);
    interp.run(fix.m_world);

    const auto* tr = fix.m_world.get_component<crd::scene::Transform>(e);
    REQUIRE(tr != nullptr);
    REQUIRE(tr->translation.x.value == 7.0F);
    REQUIRE(tr->translation.y.value == 8.0F);
    REQUIRE(tr->translation.z.value == 9.0F);
}

TEST_CASE("eylem v1b-e interpolator no-ops on null/stale body handles", "[eylem][v1b-e][interp][safety]")
{
    InterpFixture fix;
    PhysicsConfig cfg{};
    cfg.fixed_dt = 1.0F / 64.0F;
    RigidBodyInterpolationSystem interp{fix.m_pool, cfg};

    // Entity 1: null body_id.
    const auto e1 = fix.m_world.spawn();
    crd::scene::Transform t1{};
    t1.translation = crd::math::from_raw_vec<crd::units::dim::Length>(crd::math::Vec3f{10.0F, 20.0F, 30.0F});
    fix.m_world.add_component(e1, t1);
    RigidBodyComponent rbc1{};
    rbc1.body_id = crd::eylem::BodyId::null();
    rbc1.sync_to_transform = 1U;
    fix.m_world.add_component(e1, rbc1);

    // Entity 2: stale body_id (insert + remove).
    RigidBody dead{};
    dead.position = {0.0F, 0.0F, 0.0F};
    dead.inv_mass = 1.0F;
    const auto dead_id = fix.m_pool.insert(dead);
    fix.m_pool.remove(dead_id);
    const auto e2 = fix.m_world.spawn();
    crd::scene::Transform t2{};
    t2.translation = crd::math::from_raw_vec<crd::units::dim::Length>(crd::math::Vec3f{40.0F, 50.0F, 60.0F});
    fix.m_world.add_component(e2, t2);
    RigidBodyComponent rbc2{};
    rbc2.body_id = dead_id;
    rbc2.sync_to_transform = 1U;
    fix.m_world.add_component(e2, rbc2);

    fix.m_world.step_fixed(0.5 * static_cast<crd::f64>(cfg.fixed_dt), static_cast<crd::f64>(cfg.fixed_dt),
                           /*max_substeps=*/0U);
    // REQUIRE_NOTHROW expands to `try { ... }` which is illegal under
    // win-tidy's exceptions-disabled mode. The system's run() is noexcept-
    // adjacent (no `throw` in any reachable path; failure modes are
    // CRD_ASSERT). A bare call is sufficient — if anything fires, the
    // test process aborts; if the function returns, the assertions below
    // catch any state corruption.
    interp.run(fix.m_world);

    // Both entities' Transforms must remain at their seeded values.
    const auto* tr1 = fix.m_world.get_component<crd::scene::Transform>(e1);
    const auto* tr2 = fix.m_world.get_component<crd::scene::Transform>(e2);
    REQUIRE(tr1 != nullptr);
    REQUIRE(tr2 != nullptr);
    REQUIRE(tr1->translation.x.value == 10.0F);
    REQUIRE(tr2->translation.x.value == 40.0F);
}

TEST_CASE("eylem v1b-e World::fixed_step_alpha clamps to [0,1] across irregular accumulator",
          "[eylem][v1b-e][interp][world]")
{
    crd::memory::TlsfAllocator alloc{crd::usize{1U} << 20U, nullptr, "interp-alpha-test"};
    crd::scene::World world{&alloc};

    // Brand-new world: accumulator = 0 → alpha = 0.
    REQUIRE(world.fixed_step_alpha(1.0 / 60.0) == 0.0);

    // Half a tick → alpha = 0.5.
    world.step_fixed(0.5 / 64.0, 1.0 / 64.0, /*max_substeps=*/0U);
    REQUIRE(std::fabs(world.fixed_step_alpha(1.0 / 64.0) - 0.5) < 1e-9);

    // fixed_dt <= 0 sentinel returns 0 (defensive).
    REQUIRE(world.fixed_step_alpha(0.0) == 0.0);
    REQUIRE(world.fixed_step_alpha(-1.0) == 0.0);
}
