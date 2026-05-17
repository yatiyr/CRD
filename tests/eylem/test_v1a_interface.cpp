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

    // ADR-0067 — force-field surface enum values + size sanity check
    // (formula impls land per slice in v1f-fields-a..i; the surface
    // freezes here at v1l alongside everything else in crd-eylem).
    STATIC_REQUIRE(static_cast<crd::u8>(FieldFormula::Directional) == 0);
    STATIC_REQUIRE(static_cast<crd::u8>(FieldFormula::Script)      == 8);
    STATIC_REQUIRE(static_cast<crd::u8>(FieldFalloff::Constant)    == 0);
    STATIC_REQUIRE(static_cast<crd::u8>(FieldFalloff::Polynomial)  == 5);
    STATIC_REQUIRE(static_cast<crd::u8>(FieldMassCoupling::Force)  == 0);
    STATIC_REQUIRE(static_cast<crd::u8>(FieldComposition::Add)     == 0);
    STATIC_REQUIRE(static_cast<crd::u8>(FieldTrigger::Continuous)  == 0);
    REQUIRE(ForceFieldComponent{}.formula     == FieldFormula::Directional);
    REQUIRE(ForceFieldComponent{}.composition == FieldComposition::Add);
    REQUIRE(ForceFieldComponent{}.field_id    == 0ULL);

    // ADR-0078 §3 D21 — geometric params are SI Length32. Defaults:
    //   origin     = (0, 0, 0)   m
    //   radius_min = 0.01        m
    //   radius_max = 1.0         m
    //   noise_scale= 1.0         m  (wavelength)
    const ForceFieldComponent default_field{};
    REQUIRE(default_field.origin.x.value     == 0.0F);
    REQUIRE(default_field.radius_min.value   == 0.01F);
    REQUIRE(default_field.radius_max.value   == 1.0F);
    REQUIRE(default_field.noise_scale.value  == 1.0F);
    // Polymorphic-per-formula raw f32 fields stay raw.
    REQUIRE(default_field.magnitude == 1.0F);
    REQUIRE(default_field.polarity  == 1.0F);

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

TEST_CASE("eylem v1a-material Material defaults match ADR-0069 contract",
          "[eylem][v1a][material][freeze]")
{
    // ADR-0069 §1: 64-byte cache-line struct, alignment 4, locked at v1a freeze.
    STATIC_REQUIRE(sizeof(Material)  == 64);
    STATIC_REQUIRE(alignof(Material) ==  4);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Material>);

    constexpr Material kMat{};
    // Friction defaults
    REQUIRE(kMat.friction_model       == FrictionModel::Coulomb);
    REQUIRE(kMat.friction_combine     == CombineMode::GeometricMean); // ADR-0069 §2 default
    REQUIRE(kMat.friction_static      == 0.5F);
    REQUIRE(kMat.friction_dynamic     == 0.5F);
    REQUIRE(kMat.friction_anisotropy.x == 1.0F);
    REQUIRE(kMat.friction_anisotropy.y == 1.0F);
    REQUIRE(kMat.friction_anisotropy.z == 1.0F);
    REQUIRE(kMat.stribeck_velocity    == 0.01F);
    REQUIRE(kMat.viscous_coefficient  == 0.0F);
    // Restitution defaults
    REQUIRE(kMat.restitution_model    == RestitutionModel::Constant);
    REQUIRE(kMat.restitution_combine  == CombineMode::Max); // ADR-0069 §2 default (PhysX convention)
    REQUIRE(kMat.restitution          == 0.0F);
    REQUIRE(kMat.restitution_decay    == 0.0F);
    // Surface defaults
    REQUIRE(kMat.surface_velocity.x == 0.0F);
    REQUIRE(kMat.surface_velocity.y == 0.0F);
    REQUIRE(kMat.surface_velocity.z == 0.0F);
    // Mass derivation default — water; designer-friendly (1m³ box → 1000 kg)
    REQUIRE(kMat.density              == 1000.0F);
    // Damage / fracture reservation (post-v1; v1 reads but ignores)
    REQUIRE(kMat.yield_stress         == 0.0F);
}

TEST_CASE("eylem v1a-material CombineMode enum values are pinned",
          "[eylem][v1a][material][freeze]")
{
    STATIC_REQUIRE(static_cast<int>(CombineMode::Average)       == 0);
    STATIC_REQUIRE(static_cast<int>(CombineMode::Min)           == 1);
    STATIC_REQUIRE(static_cast<int>(CombineMode::Max)           == 2);
    STATIC_REQUIRE(static_cast<int>(CombineMode::Multiply)      == 3);
    STATIC_REQUIRE(static_cast<int>(CombineMode::GeometricMean) == 4); // ADR-0069 additive slot
}

TEST_CASE("eylem v1a-material FrictionModel enum values are pinned",
          "[eylem][v1a][material][freeze]")
{
    // ADR-0069 §2 — closed enum; new formulas require major-version bump.
    STATIC_REQUIRE(static_cast<int>(FrictionModel::Coulomb)        == 0);
    STATIC_REQUIRE(static_cast<int>(FrictionModel::Stribeck)       == 1);
    STATIC_REQUIRE(static_cast<int>(FrictionModel::LuGre)          == 2);
    STATIC_REQUIRE(static_cast<int>(FrictionModel::Karnopp)        == 3);
    STATIC_REQUIRE(static_cast<int>(FrictionModel::Anisotropic)    == 4);
    STATIC_REQUIRE(static_cast<int>(FrictionModel::FrictionTriple) == 5);
}

TEST_CASE("eylem v1a-material RestitutionModel enum values are pinned",
          "[eylem][v1a][material][freeze]")
{
    STATIC_REQUIRE(static_cast<int>(RestitutionModel::Constant)     == 0);
    STATIC_REQUIRE(static_cast<int>(RestitutionModel::Newton)       == 1);
    STATIC_REQUIRE(static_cast<int>(RestitutionModel::HuntCrossley) == 2);
}

TEST_CASE("eylem v1a-material MaterialId layout matches Body/Collider/Joint pattern",
          "[eylem][v1a][material][freeze]")
{
    // ADR-0069 §3: [generation:8 | index:24]; same as BodyId/ColliderId/JointId.
    STATIC_REQUIRE(sizeof(MaterialId) == 4);
    STATIC_REQUIRE(std::is_trivially_copyable_v<MaterialId>);
    STATIC_REQUIRE(std::is_standard_layout_v<MaterialId>);

    REQUIRE(MaterialId{}.is_null());
    REQUIRE(MaterialId::null().is_null());

    // Slot 1 / generation 1 — the shipped `Default` material.
    REQUIRE_FALSE(MaterialId::default_material().is_null());
    REQUIRE(MaterialId::default_material().index()      == 1U);
    REQUIRE(MaterialId::default_material().generation() == 1U);

    const auto id = MaterialId::make(/*index=*/12345U, /*generation=*/7U);
    REQUIRE(id.index()      == 12345U);
    REQUIRE(id.generation() == 7U);

    // Max addressable: index = 2^24 - 1, generation = 255.
    const auto idmax = MaterialId::make(0x00FF'FFFFU, 255U);
    REQUIRE(idmax.index()      == 0x00FF'FFFFU);
    REQUIRE(idmax.generation() == 255U);

    // Strong typing: MaterialId distinct from BodyId / ColliderId / JointId.
    STATIC_REQUIRE_FALSE(std::is_same_v<MaterialId, BodyId>);
    STATIC_REQUIRE_FALSE(std::is_same_v<MaterialId, ColliderId>);
    STATIC_REQUIRE_FALSE(std::is_same_v<MaterialId, JointId>);
}

TEST_CASE("eylem v1a-material default_material_value matches Material{}",
          "[eylem][v1a][material]")
{
    constexpr Material kA = default_material_value();
    constexpr Material kB{};
    REQUIRE(kA.friction_model    == kB.friction_model);
    REQUIRE(kA.friction_static   == kB.friction_static);
    REQUIRE(kA.friction_dynamic  == kB.friction_dynamic);
    REQUIRE(kA.restitution_model == kB.restitution_model);
    REQUIRE(kA.restitution       == kB.restitution);
    REQUIRE(kA.density           == kB.density);
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

    constexpr Collider kCol{};
    REQUIRE(kCol.shape == ColliderShape::Sphere);
    REQUIRE(kCol.sphere.radius == 1.0F);
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

    constexpr RigidBody kBody{};
    REQUIRE(kBody.inv_mass.value == 0.0F);  // default = static
    REQUIRE(kBody.linear_damping  == 0.05F);
    REQUIRE(kBody.angular_damping == 0.05F);
    REQUIRE(kBody.flags.type      == 0U);    // RigidBodyType::Static
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

    constexpr Joint kJoint{};
    REQUIRE(kJoint.type == JointType::Fixed);
    REQUIRE(kJoint.body_a.is_null());
    REQUIRE(kJoint.body_b.is_null());
    REQUIRE_FALSE(kJoint.limit.enabled);
    REQUIRE(kJoint.break_force  == 0.0F);
    REQUIRE(kJoint.break_torque == 0.0F);
}

TEST_CASE("eylem v1a PhysicsConfig defaults match documented contract", "[eylem][v1a][freeze]")
{
    constexpr PhysicsConfig kCfg{};
    REQUIRE(kCfg.gravity.y.value == -9.81F);
    REQUIRE(kCfg.fixed_dt.value                   == 1.0F / 60.0F);
    REQUIRE(kCfg.velocity_iterations        == 8U);
    REQUIRE(kCfg.position_iterations        == 3U);
    REQUIRE(kCfg.max_bodies                 == 65536U);
    REQUIRE(kCfg.max_contacts_per_pair      == 4U);
    REQUIRE(kCfg.contact_offset.value             == 0.02F);
    REQUIRE(kCfg.contact_breaking_threshold.value == 0.02F);
    REQUIRE(kCfg.sleep_linear_threshold.value     == 0.01F);
    REQUIRE(kCfg.sleep_angular_threshold.value    == 0.01F);
    REQUIRE(kCfg.sleep_time_threshold.value       == 0.5F);
    REQUIRE(kCfg.determinism                == DeterminismMode::CrossPlatform);
    REQUIRE(kCfg.warm_starting_enabled);
    REQUIRE_FALSE(kCfg.ccd_enabled);
}

// ===========================================================================
// Behaviour smokes — NullPhysicsScene round-trips
// ===========================================================================

TEST_CASE("eylem v1a NullPhysicsScene constructs and reports config", "[eylem][v1a][null]")
{
    PhysicsConfig cfg{};
    cfg.gravity = crd::math::from_raw_vec<crd::units::dim::Acceleration>(crd::math::Vec3f{0.0F, -3.71F, 0.0F}); // Mars
    auto scene = make_null_physics_scene(cfg);
    REQUIRE(scene != nullptr);
    REQUIRE(scene->gravity().y.value == -3.71F);
    REQUIRE(scene->config().velocity_iterations == 8U);
}

TEST_CASE("eylem v1a NullPhysicsScene set_gravity round-trips", "[eylem][v1a][null]")
{
    auto scene = make_null_physics_scene(PhysicsConfig{});
    scene->set_gravity(crd::math::from_raw_vec<crd::units::dim::Acceleration>(crd::math::Vec3f{1.0F, 2.0F, 3.0F}));
    REQUIRE(scene->gravity().x.value == 1.0F);
    REQUIRE(scene->gravity().y.value == 2.0F);
    REQUIRE(scene->gravity().z.value == 3.0F);
}

TEST_CASE("eylem v1a NullPhysicsScene add_body / has_body / body_state round-trip", "[eylem][v1a][null]")
{
    auto scene = make_null_physics_scene(PhysicsConfig{});
    REQUIRE(scene->body_count() == 0);

    RigidBody b{};
    b.position = crd::math::from_raw_vec<crd::units::dim::Length>(crd::math::Vec3f{7.0F, 8.0F, 9.0F});
    b.inv_mass = crd::units::InverseMass32{0.5F}; // 2 kg dynamic body

    const BodyId id = scene->add_body(b);
    REQUIRE_FALSE(id.is_null());
    REQUIRE(scene->has_body(id));
    REQUIRE(scene->body_count() == 1);

    const RigidBody read = scene->body_state(id);
    REQUIRE(read.position.x.value == 7.0F);
    REQUIRE(read.position.y.value == 8.0F);
    REQUIRE(read.position.z.value == 9.0F);
    REQUIRE(read.inv_mass.value   == 0.5F);

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

// ===========================================================================
// v1a-material-b — MaterialPool + IPhysicsScene material API behaviour
// ===========================================================================

TEST_CASE("eylem v1a-material-b MaterialPool default-allocates slot 1",
          "[eylem][v1a][material]")
{
    crd::eylem::MaterialPool pool;
    // Slot 0 is the null sentinel; slot 1 is default_material.
    // size() reports user-visible count = 1 (the default).
    REQUIRE(pool.size() == 1U);

    // The default MaterialId resolves to the shipped default.
    REQUIRE(pool.contains(MaterialId::default_material()));
    const Material& def = pool.get(MaterialId::default_material());
    REQUIRE(def.friction_static  == default_material_value().friction_static);
    REQUIRE(def.friction_dynamic == default_material_value().friction_dynamic);
    REQUIRE(def.density          == default_material_value().density);

    // Null id is not contained; get() falls back to the default.
    REQUIRE_FALSE(pool.contains(MaterialId::null()));
    const Material& fallback = pool.get(MaterialId::null());
    REQUIRE(fallback.friction_static == def.friction_static);
}

TEST_CASE("eylem v1a-material-b MaterialPool insert / update round-trips",
          "[eylem][v1a][material]")
{
    crd::eylem::MaterialPool pool;

    Material rubber{};
    rubber.friction_static  = 1.20F;
    rubber.friction_dynamic = 1.10F;
    rubber.restitution      = 0.83F;
    rubber.density          = 1100.0F;

    const MaterialId id = pool.insert(rubber);
    REQUIRE_FALSE(id.is_null());
    REQUIRE(id.index() == 2U);     // slot 0=null, slot 1=default, first user slot=2
    REQUIRE(id.generation() == 1U);
    REQUIRE(pool.contains(id));
    REQUIRE(pool.size() == 2U);    // default + rubber

    const Material& read = pool.get(id);
    REQUIRE(read.friction_static  == 1.20F);
    REQUIRE(read.friction_dynamic == 1.10F);
    REQUIRE(read.restitution      == 0.83F);
    REQUIRE(read.density          == 1100.0F);

    // Update mutates in place; the handle stays valid.
    Material rubber_v2 = rubber;
    rubber_v2.friction_static = 1.50F;
    pool.update(id, rubber_v2);
    REQUIRE(pool.get(id).friction_static == 1.50F);

    // Out-of-range / null updates are silent no-ops.
    pool.update(MaterialId::null(), rubber);
    pool.update(MaterialId::make(99999U, 1U), rubber);
    REQUIRE(pool.get(id).friction_static == 1.50F); // unchanged
}

TEST_CASE("eylem v1a-material-b MaterialPool determinism index sequence is insert order",
          "[eylem][v1a][material][determinism]")
{
    // Two pools constructed in the same way + given the same insert sequence
    // produce identical index assignments. This is the core determinism
    // contract for content-addressed cooker materials (ADR-0067 §3 pattern).
    crd::eylem::MaterialPool a;
    crd::eylem::MaterialPool b;
    Material m1{}; m1.density = 7850.0F;  // steel-ish
    Material m2{}; m2.density = 920.0F;   // ice-ish
    Material m3{}; m3.density = 700.0F;   // oak-ish
    REQUIRE(a.insert(m1).index() == b.insert(m1).index());
    REQUIRE(a.insert(m2).index() == b.insert(m2).index());
    REQUIRE(a.insert(m3).index() == b.insert(m3).index());
    REQUIRE(a.size() == b.size());
}

TEST_CASE("eylem v1a-material-b NullPhysicsScene exposes MaterialPool API",
          "[eylem][v1a][material][null]")
{
    auto scene = make_null_physics_scene(PhysicsConfig{});

    // Default material is always available, even without any create_material calls.
    REQUIRE(scene->has_material(MaterialId::default_material()));
    const Material& def = scene->material(MaterialId::default_material());
    REQUIRE(def.density == default_material_value().density);

    Material steel{};
    steel.friction_static  = 0.74F;
    steel.friction_dynamic = 0.57F;
    steel.restitution      = 0.50F;
    steel.density          = 7850.0F;

    const MaterialId steel_id = scene->create_material(steel);
    REQUIRE_FALSE(steel_id.is_null());
    REQUIRE(scene->has_material(steel_id));

    const Material& read = scene->material(steel_id);
    REQUIRE(read.friction_static == 0.74F);
    REQUIRE(read.density         == 7850.0F);

    // update_material mutates in place; the handle stays valid.
    Material steel_v2 = steel;
    steel_v2.restitution = 0.30F;
    scene->update_material(steel_id, steel_v2);
    REQUIRE(scene->material(steel_id).restitution == 0.30F);

    // Null and out-of-range queries return false; material(null) falls back to default.
    REQUIRE_FALSE(scene->has_material(MaterialId::null()));
    REQUIRE_FALSE(scene->has_material(MaterialId::make(99999U, 1U)));
    REQUIRE(scene->material(MaterialId::null()).density == default_material_value().density);
}

// ===========================================================================
// v1a-material-c — per-collider Collider::material (MaterialId handle)
// ===========================================================================

TEST_CASE("eylem v1a-material-c Collider defaults material to MaterialId::default_material",
          "[eylem][v1a][material]")
{
    constexpr Collider kCol{};
    REQUIRE(kCol.material.index()      == MaterialId::default_material().index());
    REQUIRE(kCol.material.generation() == MaterialId::default_material().generation());
    REQUIRE_FALSE(kCol.material.is_null());
}

TEST_CASE("eylem v1a-material-c add_collider 2-arg overload reads pre-set Collider::material",
          "[eylem][v1a][material][null]")
{
    auto scene = make_null_physics_scene(PhysicsConfig{});
    const BodyId body = scene->add_body(RigidBody{});

    Material rubber{};
    rubber.friction_static  = 1.20F;
    rubber.friction_dynamic = 1.10F;
    rubber.restitution      = 0.83F;
    rubber.density          = 1100.0F;
    const MaterialId rubber_id = scene->create_material(rubber);
    REQUIRE_FALSE(rubber_id.is_null());

    Collider col{};
    col.shape           = ColliderShape::Box;
    col.box.half_extents = {0.5F, 0.5F, 0.5F};
    col.material        = rubber_id;

    const ColliderId cid = scene->add_collider(body, col);
    REQUIRE_FALSE(cid.is_null());
    REQUIRE(scene->has_collider(cid));

    // Material handle on the collider survives round-trip and resolves to
    // the same pool entry.
    REQUIRE(scene->material(rubber_id).friction_static == 1.20F);
    REQUIRE(scene->material(rubber_id).density         == 1100.0F);
}

TEST_CASE("eylem v1a-material-c add_collider 3-arg convenience allocates pool slot",
          "[eylem][v1a][material][null]")
{
    auto scene = make_null_physics_scene(PhysicsConfig{});
    const BodyId body = scene->add_body(RigidBody{});

    Material steel{};
    steel.friction_static  = 0.74F;
    steel.friction_dynamic = 0.57F;
    steel.density          = 7850.0F;

    Collider col{};
    col.shape           = ColliderShape::Sphere;
    col.sphere.radius   = 0.25F;
    // Note: leaving col.material at its default — the 3-arg overload
    // overwrites it with the freshly created material's id.

    // 3-arg convenience overload: creates the material, attaches the
    // returned MaterialId on a copy of `col`, then forwards to add_collider(body, c).
    const ColliderId cid = scene->add_collider(body, col, steel);
    REQUIRE_FALSE(cid.is_null());
    REQUIRE(scene->has_collider(cid));

    // The pool now contains the steel material — there's exactly one user
    // material (steel) on top of the auto-allocated default.
    // (The 3-arg path inserts via create_material; index = 2 since slot 0
    // = null sentinel and slot 1 = default.)
    const MaterialId expected = MaterialId::make(2U, 1U);
    REQUIRE(scene->has_material(expected));
    REQUIRE(scene->material(expected).density         == 7850.0F);
    REQUIRE(scene->material(expected).friction_static == 0.74F);
}

// ===========================================================================
// v1a-material-d — mass derivation (volume × density, ColliderId-stable Σ)
// ===========================================================================

#include <cmath>

namespace
{
constexpr crd::f32 kTestPi = 3.14159265358979323846F;

// Tiny accessor that resolves through a one-element table for unit tests
// that exercise the free function without a scene.
const crd::eylem::Material& test_accessor(void* user_data, crd::eylem::MaterialId id)
{
    const auto* mat = static_cast<const crd::eylem::Material*>(user_data);
    (void)id;
    return *mat;
}
} // namespace

TEST_CASE("eylem v1a-material-d derive_mass_properties sphere matches analytic formula",
          "[eylem][v1a][material]")
{
    Collider sphere{};
    sphere.shape         = ColliderShape::Sphere;
    sphere.sphere.radius = 2.0F;

    Material steel{};
    steel.density = 7850.0F;

    Collider arr[] = {sphere};
    const auto props = derive_mass_properties(
        crd::containers::ConstSpan<Collider>(arr, 1),
        &test_accessor,
        &steel);

    const crd::f32 expected_v = (4.0F / 3.0F) * kTestPi * 2.0F * 2.0F * 2.0F;
    const crd::f32 expected_m = expected_v * 7850.0F;
    REQUIRE(std::fabs(props.mass - expected_m)            < 1e-2F);
    REQUIRE(props.com_local.x                             == 0.0F);
    REQUIRE(props.com_local.y                             == 0.0F);
    REQUIRE(props.com_local.z                             == 0.0F);
    // Sphere inertia diagonal: I = 2/5 m r²  for all axes.
    const crd::f32 expected_i = (2.0F / 5.0F) * expected_m * 4.0F;
    REQUIRE(std::fabs(props.inertia_diagonal.x - expected_i) < 1e-1F);
    REQUIRE(std::fabs(props.inertia_diagonal.y - expected_i) < 1e-1F);
    REQUIRE(std::fabs(props.inertia_diagonal.z - expected_i) < 1e-1F);
}

TEST_CASE("eylem v1a-material-d derive_mass_properties unit-density box at origin",
          "[eylem][v1a][material]")
{
    Collider box{};
    box.shape          = ColliderShape::Box;
    box.box.half_extents = {1.0F, 1.0F, 1.0F}; // 2×2×2 = 8 m³

    Material water{}; // default density 1000 kg/m³
    Collider arr[] = {box};
    const auto props = derive_mass_properties(
        crd::containers::ConstSpan<Collider>(arr, 1),
        &test_accessor,
        &water);

    REQUIRE(std::fabs(props.mass - 8000.0F) < 1e-3F);
    REQUIRE(props.com_local.x == 0.0F);
    REQUIRE(props.com_local.y == 0.0F);
    REQUIRE(props.com_local.z == 0.0F);
    // Box inertia: I_xx = 1/3 * m * (hy² + hz²) = 1/3 * 8000 * 2 = 16000/3
    const crd::f32 expected_i = (8000.0F / 3.0F) * 2.0F;
    REQUIRE(std::fabs(props.inertia_diagonal.x - expected_i) < 1e-1F);
    REQUIRE(std::fabs(props.inertia_diagonal.y - expected_i) < 1e-1F);
    REQUIRE(std::fabs(props.inertia_diagonal.z - expected_i) < 1e-1F);
}

TEST_CASE("eylem v1a-material-d derive_mass_properties two-box compound shifts COM",
          "[eylem][v1a][material]")
{
    // Two unit-density 1m³ cubes side-by-side along +X. Body COM should
    // sit halfway between their centroids.
    Collider a{};
    a.shape          = ColliderShape::Box;
    a.box.half_extents = {0.5F, 0.5F, 0.5F};
    a.local_position = {0.0F, 0.0F, 0.0F};

    Collider b{};
    b.shape          = ColliderShape::Box;
    b.box.half_extents = {0.5F, 0.5F, 0.5F};
    b.local_position = {2.0F, 0.0F, 0.0F};

    Material water{};
    Collider arr[] = {a, b}; // ascending ColliderId order = storage order
    const auto props = derive_mass_properties(
        crd::containers::ConstSpan<Collider>(arr, 2),
        &test_accessor,
        &water);

    REQUIRE(std::fabs(props.mass         - 2000.0F) < 1e-3F);
    REQUIRE(std::fabs(props.com_local.x  - 1.0F)    < 1e-4F);
    REQUIRE(props.com_local.y == 0.0F);
    REQUIRE(props.com_local.z == 0.0F);
    // Inertia about COM, X axis: each box contributes
    //   I_box_about_own_centroid_x = 1/3 m (hy² + hz²) = 1/3 * 1000 * (0.25 + 0.25) = 500/3
    // Plus parallel axis for each at d_x = 1m (dist from box centroid to COM):
    //   PAT for X axis: rotation axis is X; d perpendicular to X is in YZ plane.
    //   Each box d = (±1, 0, 0); ||d||² = 1, d·d^T diagonal = (1, 0, 0).
    //   I_xx_PAT = m * (1 - 1) = 0   (rotation axis aligned with displacement)
    //   I_yy_PAT = m * (1 - 0) = m
    //   I_zz_PAT = m * (1 - 0) = m
    // So I_xx = 2 * 500/3 = 1000/3 ≈ 333.33
    REQUIRE(std::fabs(props.inertia_diagonal.x - (1000.0F / 3.0F)) < 1e-1F);
    // I_yy = 2*(1/3*1000*0.5) + 2*1000 = 1000/3 + 2000 ≈ 2333.33
    REQUIRE(std::fabs(props.inertia_diagonal.y - (1000.0F / 3.0F + 2000.0F)) < 1e-1F);
    REQUIRE(std::fabs(props.inertia_diagonal.z - (1000.0F / 3.0F + 2000.0F)) < 1e-1F);
}

TEST_CASE("eylem v1a-material-d derive_mass_properties returns zeros for plane-only / empty",
          "[eylem][v1a][material]")
{
    Material water{};

    // Empty collider list → zeroed result.
    const auto props_empty = derive_mass_properties(
        crd::containers::ConstSpan<Collider>{},
        &test_accessor,
        &water);
    REQUIRE(props_empty.mass == 0.0F);

    // Plane-only compound: V = 0 → mass stays 0.
    Collider plane{};
    plane.shape       = ColliderShape::Plane;
    plane.plane.normal = {0.0F, 1.0F, 0.0F};
    plane.plane.d      = 0.0F;
    Collider arr[] = {plane};
    const auto props_plane = derive_mass_properties(
        crd::containers::ConstSpan<Collider>(arr, 1),
        &test_accessor,
        &water);
    REQUIRE(props_plane.mass == 0.0F);
    REQUIRE(props_plane.com_local.x == 0.0F);
}

TEST_CASE("eylem v1a-material-d NullPhysicsScene::derive_body_mass walks per-body colliders",
          "[eylem][v1a][material][null]")
{
    auto scene = make_null_physics_scene(PhysicsConfig{});
    const BodyId b1 = scene->add_body(RigidBody{});
    const BodyId b2 = scene->add_body(RigidBody{});

    // Body 1 = single 1m³ cube at origin (uses default material, density 1000).
    Collider cube{};
    cube.shape          = ColliderShape::Box;
    cube.box.half_extents = {0.5F, 0.5F, 0.5F};
    REQUIRE_FALSE(scene->add_collider(b1, cube).is_null());

    // Body 2 = single sphere, radius 1 m, custom denser material.
    Material denser{};
    denser.density = 2000.0F;
    const MaterialId denser_id = scene->create_material(denser);

    Collider sphere{};
    sphere.shape         = ColliderShape::Sphere;
    sphere.sphere.radius = 1.0F;
    sphere.material      = denser_id;
    REQUIRE_FALSE(scene->add_collider(b2, sphere).is_null());

    const auto props_b1 = scene->derive_body_mass(b1);
    REQUIRE(std::fabs(props_b1.mass - 1000.0F) < 1e-3F); // 1 m³ × 1000 kg/m³
    REQUIRE(props_b1.com_local.x == 0.0F);

    const auto props_b2 = scene->derive_body_mass(b2);
    const crd::f32 sphere_v = (4.0F / 3.0F) * kTestPi;
    const crd::f32 expected_b2 = sphere_v * 2000.0F;
    REQUIRE(std::fabs(props_b2.mass - expected_b2) < 1e-2F);

    // Invalid body returns zero-init.
    const auto props_null = scene->derive_body_mass(BodyId::null());
    REQUIRE(props_null.mass == 0.0F);
}

TEST_CASE("eylem v1a NullPhysicsScene step + raycast + force/torque/impulse no-op cleanly", "[eylem][v1a][null]")
{
    auto scene = make_null_physics_scene(PhysicsConfig{});
    const BodyId b = scene->add_body(RigidBody{});

    // No exceptions, no asserts — these are stub no-ops in v1a.
    scene->apply_force(b, crd::math::from_raw_vec<crd::units::dim::Force>(crd::math::Vec3f{1.0F, 0.0F, 0.0F}));
    scene->apply_torque(b, crd::math::from_raw_vec<crd::units::dim::Torque>(crd::math::Vec3f{0.0F, 1.0F, 0.0F}));
    scene->apply_impulse(b, crd::math::from_raw_vec<crd::units::dim::Momentum>(crd::math::Vec3f{0.0F, 0.0F, 1.0F}), crd::math::from_raw_vec<crd::units::dim::Length>(crd::math::Vec3f{0.0F, 0.0F, 0.0F}));
    scene->step(crd::units::Duration32{1.0F / 60.0F});

    // Raycast in the null impl always misses.
    const auto hit = scene->raycast(
        crd::math::from_raw_vec<crd::units::dim::Length>(crd::math::Vec3f{0.0F, 0.0F, 0.0F}),
        crd::math::Vec3f{0.0F, -1.0F, 0.0F},
        crd::units::Length32{100.0F});
    REQUIRE_FALSE(hit.has_value());
}
