// Phase 3.1 v1a — eylem interface module API freeze + behaviour smoke tests.
//
// API freeze contract (ADR-0062 §15): every sizeof / alignof / enum value
// the cooker writes and the loader reads is pinned via static_assert. This
// makes any inadvertent layout change a compile failure rather than a
// silent wire-format break.
//
// Behaviour smokes verify NullPhysicsScene round-trips add/has/remove on
// bodies / colliders / joints — sufficient to prove the interface compiles
// + links + can be instantiated.

#include <catch2/catch_test_macros.hpp>

#include <crd/eylem/eylem.hpp>

#include <type_traits>

using namespace crd::eylem;

// ===========================================================================
// API surface freeze — sizeof / alignof / enum / POD pins
// ===========================================================================

TEST_CASE("eylem v1a strong-type IDs are 4 bytes and trivially copyable", "[eylem][v1a][freeze]")
{
    STATIC_REQUIRE(sizeof(BodyId)     == 4);
    STATIC_REQUIRE(sizeof(ColliderId) == 4);
    STATIC_REQUIRE(sizeof(JointId)    == 4);

    STATIC_REQUIRE(std::is_trivially_copyable_v<BodyId>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<ColliderId>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<JointId>);

    STATIC_REQUIRE(std::is_standard_layout_v<BodyId>);
    STATIC_REQUIRE(std::is_standard_layout_v<ColliderId>);
    STATIC_REQUIRE(std::is_standard_layout_v<JointId>);

    // Default-constructed = null sentinel.
    REQUIRE(BodyId{}.is_null());
    REQUIRE(ColliderId{}.is_null());
    REQUIRE(JointId{}.is_null());
}

TEST_CASE("eylem v1a ID generation/index round-trips correctly", "[eylem][v1a][freeze]")
{
    // 24-bit index field, 8-bit generation field.
    const auto id = BodyId::make(/*index=*/12345U, /*generation=*/7U);
    REQUIRE(id.index()      == 12345U);
    REQUIRE(id.generation() == 7U);
    REQUIRE_FALSE(id.is_null());

    // Max addressable: index=2^24-1, generation=255.
    const auto idmax = BodyId::make(0x00FF'FFFFU, 255U);
    REQUIRE(idmax.index()      == 0x00FF'FFFFU);
    REQUIRE(idmax.generation() == 255U);

    // Strong typing: BodyId and ColliderId do NOT compare. (Compile-only check.)
    STATIC_REQUIRE_FALSE(std::is_same_v<BodyId, ColliderId>);
}

TEST_CASE("eylem v1a Material defaults match documented contract", "[eylem][v1a][freeze]")
{
    STATIC_REQUIRE(sizeof(Material) == 20);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Material>);

    constexpr Material m{};
    REQUIRE(m.static_friction  == 0.5F);
    REQUIRE(m.dynamic_friction == 0.5F);
    REQUIRE(m.restitution      == 0.0F);
    REQUIRE(m.density          == 1000.0F);
    REQUIRE(m.friction_combine    == CombineMode::Average);
    REQUIRE(m.restitution_combine == CombineMode::Max);
}

TEST_CASE("eylem v1a CombineMode enum values are pinned", "[eylem][v1a][freeze]")
{
    STATIC_REQUIRE(static_cast<int>(CombineMode::Average)  == 0);
    STATIC_REQUIRE(static_cast<int>(CombineMode::Min)      == 1);
    STATIC_REQUIRE(static_cast<int>(CombineMode::Max)      == 2);
    STATIC_REQUIRE(static_cast<int>(CombineMode::Multiply) == 3);
}

TEST_CASE("eylem v1a DeterminismMode enum values are pinned", "[eylem][v1a][freeze]")
{
    STATIC_REQUIRE(static_cast<int>(DeterminismMode::Default)        == 0);
    STATIC_REQUIRE(static_cast<int>(DeterminismMode::CrossPlatform)  == 1);
    STATIC_REQUIRE(static_cast<int>(DeterminismMode::BackwardCompat) == 2);
}

TEST_CASE("eylem v1a CCDMode enum values are pinned", "[eylem][v1a][freeze]")
{
    STATIC_REQUIRE(static_cast<int>(CCDMode::Discrete) == 0);
    STATIC_REQUIRE(static_cast<int>(CCDMode::Linear)   == 1);
    STATIC_REQUIRE(static_cast<int>(CCDMode::Full)     == 2);
}

TEST_CASE("eylem v1a Collider variant layouts are pinned", "[eylem][v1a][freeze]")
{
    STATIC_REQUIRE(static_cast<int>(ColliderShape::Sphere)     == 0);
    STATIC_REQUIRE(static_cast<int>(ColliderShape::Box)        == 1);
    STATIC_REQUIRE(static_cast<int>(ColliderShape::Capsule)    == 2);
    STATIC_REQUIRE(static_cast<int>(ColliderShape::ConvexHull) == 3);
    STATIC_REQUIRE(static_cast<int>(ColliderShape::Plane)      == 4);

    STATIC_REQUIRE(sizeof(ColliderSphere)     ==  4);
    STATIC_REQUIRE(sizeof(ColliderBox)        == 12);
    STATIC_REQUIRE(sizeof(ColliderCapsule)    ==  8);
    STATIC_REQUIRE(sizeof(ColliderConvexHull) ==  8);
    STATIC_REQUIRE(sizeof(ColliderPlane)      == 16);

    constexpr Collider c{};
    REQUIRE(c.shape == ColliderShape::Sphere);
    REQUIRE(c.sphere.radius == 1.0F);
}

TEST_CASE("eylem v1a RigidBody POD layout + flag packing pinned", "[eylem][v1a][freeze]")
{
    STATIC_REQUIRE(sizeof(RigidBody)      == 80);
    STATIC_REQUIRE(sizeof(RigidBodyFlags) ==  4);
    STATIC_REQUIRE(std::is_trivially_copyable_v<RigidBody>);
    STATIC_REQUIRE(std::is_standard_layout_v<RigidBody>);

    STATIC_REQUIRE(static_cast<int>(RigidBodyType::Static)    == 0);
    STATIC_REQUIRE(static_cast<int>(RigidBodyType::Kinematic) == 1);
    STATIC_REQUIRE(static_cast<int>(RigidBodyType::Dynamic)   == 2);

    constexpr RigidBody b{};
    REQUIRE(b.inv_mass        == 0.0F);  // default = static
    REQUIRE(b.linear_damping  == 0.05F);
    REQUIRE(b.angular_damping == 0.05F);
    REQUIRE(b.flags.type      == 0U);    // RigidBodyType::Static
}

TEST_CASE("eylem v1a Joint layout + JointType pinned", "[eylem][v1a][freeze]")
{
    STATIC_REQUIRE(sizeof(JointAnchor) == 28);
    STATIC_REQUIRE(sizeof(JointLimit)  == 12);

    STATIC_REQUIRE(static_cast<int>(JointType::Fixed)     == 0);
    STATIC_REQUIRE(static_cast<int>(JointType::Revolute)  == 1);
    STATIC_REQUIRE(static_cast<int>(JointType::Spherical) == 2);
    STATIC_REQUIRE(static_cast<int>(JointType::Prismatic) == 3);
    STATIC_REQUIRE(static_cast<int>(JointType::Distance)  == 4);

    constexpr Joint j{};
    REQUIRE(j.type == JointType::Fixed);
    REQUIRE(j.body_a.is_null());
    REQUIRE(j.body_b.is_null());
    REQUIRE_FALSE(j.limit.enabled);
    REQUIRE(j.break_force  == 0.0F);
    REQUIRE(j.break_torque == 0.0F);
}

TEST_CASE("eylem v1a PhysicsConfig defaults match documented contract", "[eylem][v1a][freeze]")
{
    constexpr PhysicsConfig c{};
    REQUIRE(c.gravity.y                  == -9.81F);
    REQUIRE(c.fixed_dt                   == 1.0F / 60.0F);
    REQUIRE(c.velocity_iterations        == 8U);
    REQUIRE(c.position_iterations        == 3U);
    REQUIRE(c.max_bodies                 == 65536U);
    REQUIRE(c.max_contacts_per_pair      == 4U);
    REQUIRE(c.contact_offset             == 0.02F);
    REQUIRE(c.contact_breaking_threshold == 0.02F);
    REQUIRE(c.sleep_linear_threshold     == 0.01F);
    REQUIRE(c.sleep_angular_threshold    == 0.01F);
    REQUIRE(c.sleep_time_threshold       == 0.5F);
    REQUIRE(c.determinism                == DeterminismMode::CrossPlatform);
    REQUIRE(c.warm_starting_enabled);
    REQUIRE_FALSE(c.ccd_enabled);
}

// ===========================================================================
// Behaviour smokes — NullPhysicsScene round-trips
// ===========================================================================

TEST_CASE("eylem v1a NullPhysicsScene constructs and reports config", "[eylem][v1a][null]")
{
    PhysicsConfig cfg{};
    cfg.gravity = {0.0F, -3.71F, 0.0F}; // Mars
    auto scene = make_null_physics_scene(cfg);
    REQUIRE(scene != nullptr);
    REQUIRE(scene->gravity().y == -3.71F);
    REQUIRE(scene->config().velocity_iterations == 8U);
}

TEST_CASE("eylem v1a NullPhysicsScene set_gravity round-trips", "[eylem][v1a][null]")
{
    auto scene = make_null_physics_scene(PhysicsConfig{});
    scene->set_gravity({1.0F, 2.0F, 3.0F});
    REQUIRE(scene->gravity().x == 1.0F);
    REQUIRE(scene->gravity().y == 2.0F);
    REQUIRE(scene->gravity().z == 3.0F);
}

TEST_CASE("eylem v1a NullPhysicsScene add_body / has_body / body_state round-trip", "[eylem][v1a][null]")
{
    auto scene = make_null_physics_scene(PhysicsConfig{});
    REQUIRE(scene->body_count() == 0);

    RigidBody b{};
    b.position = {7.0F, 8.0F, 9.0F};
    b.inv_mass = 0.5F; // 2 kg dynamic body

    const BodyId id = scene->add_body(b);
    REQUIRE_FALSE(id.is_null());
    REQUIRE(scene->has_body(id));
    REQUIRE(scene->body_count() == 1);

    const RigidBody read = scene->body_state(id);
    REQUIRE(read.position.x == 7.0F);
    REQUIRE(read.position.y == 8.0F);
    REQUIRE(read.position.z == 9.0F);
    REQUIRE(read.inv_mass   == 0.5F);

    // Null IDs report as not-present.
    REQUIRE_FALSE(scene->has_body(BodyId::null()));
    // Out-of-range IDs report as not-present.
    REQUIRE_FALSE(scene->has_body(BodyId::make(99999U, 1U)));
}

TEST_CASE("eylem v1a NullPhysicsScene add_collider + add_joint round-trip", "[eylem][v1a][null]")
{
    auto scene = make_null_physics_scene(PhysicsConfig{});

    const BodyId b1 = scene->add_body(RigidBody{});
    const BodyId b2 = scene->add_body(RigidBody{});

    Collider col{};
    col.shape = ColliderShape::Box;
    col.box.half_extents = {0.5F, 0.5F, 0.5F};

    const ColliderId cid = scene->add_collider(b1, col, Material{});
    REQUIRE_FALSE(cid.is_null());
    REQUIRE(scene->has_collider(cid));

    Joint j{};
    j.type   = JointType::Revolute;
    j.body_a = b1;
    j.body_b = b2;

    const JointId jid = scene->add_joint(j);
    REQUIRE_FALSE(jid.is_null());
    REQUIRE(scene->has_joint(jid));
}

TEST_CASE("eylem v1a NullPhysicsScene step + raycast + force/torque/impulse no-op cleanly", "[eylem][v1a][null]")
{
    auto scene = make_null_physics_scene(PhysicsConfig{});
    const BodyId b = scene->add_body(RigidBody{});

    // No exceptions, no asserts — these are stub no-ops in v1a.
    scene->apply_force(b,   {1.0F, 0.0F, 0.0F});
    scene->apply_torque(b,  {0.0F, 1.0F, 0.0F});
    scene->apply_impulse(b, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 0.0F});
    scene->step(1.0F / 60.0F);

    // Raycast in the null impl always misses.
    const auto hit = scene->raycast({0.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, /*max_distance=*/100.0F);
    REQUIRE_FALSE(hit.has_value());
}
