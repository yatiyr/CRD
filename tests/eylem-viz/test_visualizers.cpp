// Phase 3.1 v1b-d — crd-eylem-viz visualizer tests.
//
// Two scenarios per the phase plan:
//   1. Registration: register_eylem_visualizers attaches the
//      RigidBodyComponent + ColliderComponent visualizers to the
//      registry; dispatch via DebugVizSystem-style invoke_all fires
//      them on entities carrying the components.
//   2. Dispatch produces expected primitives: a moving body with the
//      ShowVelocity flag emits an arrow; a sphere collider emits
//      sphere_wire lines; a box collider emits box_wire lines (12
//      edges); a capsule collider emits capsule_wire primitives.

#include <catch2/catch_test_macros.hpp>

#include <crd/draw/debug_viz_component.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/visualizer_registry.hpp>
#include <crd/eylem/components.hpp>
#include <crd/eylem/physics_config.hpp>
#include <crd/eylem/rigid_body.hpp>
#include <crd/eylem_rigid3d/body_pool.hpp>
#include <crd/eylem_rigid3d/collider_pool.hpp>
#include <crd/eylem_viz/eylem_viz.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>

using crd::draw::Category;
using crd::draw::DebugVizComponent;
using crd::draw::RenderBuffer;
using crd::draw::VisualizerRegistry;
using crd::eylem::BodyId;
using crd::eylem::ColliderComponent;
using crd::eylem::RigidBodyComponent;
using crd::eylem_rigid3d::BodyPool;
using crd::eylem_rigid3d::ColliderPool;
using crd::eylem_viz::register_eylem_visualizers;

namespace
{
// Helper: register the components needed by both visualizers. World is
// non-movable per ADR-0049, so the caller constructs it; we populate.
void register_test_components(crd::scene::World& world)
{
    world.register_component<crd::scene::Transform>(crd::scene::StorageHint::Archetype);
    world.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
    world.register_component<RigidBodyComponent>(crd::scene::StorageHint::SparseSet);
    world.register_component<ColliderComponent>(crd::scene::StorageHint::SparseSet);
    world.register_component<DebugVizComponent>(crd::scene::StorageHint::SparseSet);
}
} // namespace

TEST_CASE("eylem v1b-d register_eylem_visualizers fires on RBC + CC entities",
          "[eylem][v1b-d][viz]")
{
    crd::memory::TlsfAllocator alloc{16U << 20}; // 16 MB per-test heap
    BodyPool     body_pool{&alloc, 64U};
    ColliderPool collider_pool{&alloc, /*capacity_per_kind=*/64U};

    VisualizerRegistry registry{&alloc};
    register_eylem_visualizers(registry, body_pool, collider_pool);

    // Spawn a body + sphere collider entity; mark Wireframe + ShowVelocity.
    crd::scene::World world{&alloc};
    register_test_components(world);
    crd::eylem::RigidBody body{};
    body.position        = {0.0F, 5.0F, 0.0F};
    body.linear_velocity = {1.0F, 0.0F, 0.0F};
    body.inv_mass        = 1.0F;
    const BodyId body_id = body_pool.insert(body);
    REQUIRE_FALSE(body_id.is_null());

    crd::eylem::Collider sphere{};
    sphere.shape         = crd::eylem::ColliderShape::Sphere;
    sphere.sphere.radius = 0.5F;
    const auto cid = collider_pool.insert(body_id, sphere);
    REQUIRE_FALSE(cid.is_null());

    const auto e = world.spawn();
    crd::scene::Transform tr{};
    tr.translation = crd::math::from_raw_vec<crd::units::dim::Length>(crd::math::Vec3f{0.0F, 5.0F, 0.0F});
    tr.world = crd::math::from_trs(tr.translation, tr.rotation, tr.scale);
    world.add_component(e, tr);

    RigidBodyComponent rbc{};
    rbc.body_id = body_id;
    world.add_component(e, rbc);
    ColliderComponent cc{};
    cc.collider_id = cid;
    world.add_component(e, cc);

    DebugVizComponent viz{};
    viz.flags = DebugVizComponent::Wireframe | DebugVizComponent::ShowVelocity;
    viz.scale = 1.0F;
    world.add_component(e, viz);

    RenderBuffer buf{&alloc};
    REQUIRE(buf.empty());

    registry.invoke_all(world, e, buf, viz);

    // Both visualizers should have written: velocity arrow (line stem +
    // 4 cone-head triangles) AND sphere wireframe (lots of line segments).
    // Just assert the buffer is non-empty and contains lines + triangles.
    REQUIRE_FALSE(buf.empty());
    REQUIRE(buf.line_count()     > 0U); // sphere wire + arrow stem
    REQUIRE(buf.triangle_count() > 0U); // arrow head cone
}

TEST_CASE("eylem v1b-d ColliderComponent emits sphere_wire when shape=Sphere",
          "[eylem][v1b-d][viz][collider]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    BodyPool     body_pool{&alloc, 64U};
    ColliderPool collider_pool{&alloc, 64U};
    VisualizerRegistry registry{&alloc};
    register_eylem_visualizers(registry, body_pool, collider_pool);

    crd::scene::World world{&alloc};
    register_test_components(world);
    const BodyId body_id = body_pool.insert(crd::eylem::RigidBody{});

    crd::eylem::Collider sphere{};
    sphere.shape         = crd::eylem::ColliderShape::Sphere;
    sphere.sphere.radius = 1.5F;
    const auto cid = collider_pool.insert(body_id, sphere);

    const auto e = world.spawn();
    crd::scene::Transform tr{};
    tr.world = crd::math::from_trs(tr.translation, tr.rotation, tr.scale);
    world.add_component(e, tr);
    ColliderComponent cc{};
    cc.collider_id = cid;
    world.add_component(e, cc);
    DebugVizComponent viz{};
    viz.flags = DebugVizComponent::Wireframe; // no ShowVelocity
    world.add_component(e, viz);

    RenderBuffer buf{&alloc};
    registry.invoke_all(world, e, buf, viz);

    // Sphere wire: 16 longitude meridians × 8 segments + lat rings; just
    // assert lines were emitted and no triangles (no arrow, no solid).
    REQUIRE(buf.line_count()     > 0U);
    REQUIRE(buf.triangle_count() == 0U);
}

TEST_CASE("eylem v1b-d ColliderComponent emits 12 edges when shape=Box",
          "[eylem][v1b-d][viz][collider]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    BodyPool     body_pool{&alloc, 64U};
    ColliderPool collider_pool{&alloc, 64U};
    VisualizerRegistry registry{&alloc};
    register_eylem_visualizers(registry, body_pool, collider_pool);

    crd::scene::World world{&alloc};
    register_test_components(world);
    const BodyId body_id = body_pool.insert(crd::eylem::RigidBody{});

    crd::eylem::Collider box{};
    box.shape          = crd::eylem::ColliderShape::Box;
    box.box.half_extents = {0.5F, 0.5F, 0.5F};
    const auto cid = collider_pool.insert(body_id, box);

    const auto e = world.spawn();
    crd::scene::Transform tr{};
    tr.world = crd::math::from_trs(tr.translation, tr.rotation, tr.scale);
    world.add_component(e, tr);
    ColliderComponent cc{};
    cc.collider_id = cid;
    world.add_component(e, cc);
    DebugVizComponent viz{};
    viz.flags = DebugVizComponent::Wireframe;
    world.add_component(e, viz);

    RenderBuffer buf{&alloc};
    registry.invoke_all(world, e, buf, viz);

    // box_wire_to emits exactly 12 edges per the d1 contract
    // (8 corners → 12 unique edges of a cube).
    REQUIRE(buf.line_count()     == 12U);
    REQUIRE(buf.triangle_count() == 0U);
}

TEST_CASE("eylem v1b-d RigidBodyComponent visualizer respects ShowVelocity flag",
          "[eylem][v1b-d][viz][body]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    BodyPool     body_pool{&alloc, 64U};
    ColliderPool collider_pool{&alloc, 64U};
    VisualizerRegistry registry{&alloc};
    register_eylem_visualizers(registry, body_pool, collider_pool);

    crd::scene::World world{&alloc};
    register_test_components(world);
    crd::eylem::RigidBody body{};
    body.linear_velocity = {3.0F, 0.0F, 0.0F}; // moving along +X
    const BodyId body_id = body_pool.insert(body);

    const auto e = world.spawn();
    crd::scene::Transform tr{};
    tr.world = crd::math::from_trs(tr.translation, tr.rotation, tr.scale);
    world.add_component(e, tr);
    RigidBodyComponent rbc{};
    rbc.body_id = body_id;
    world.add_component(e, rbc);

    // Without ShowVelocity flag → no arrow, buffer stays empty.
    DebugVizComponent viz_noflag{};
    viz_noflag.flags = 0; // no flags
    world.add_component(e, viz_noflag);

    RenderBuffer buf{&alloc};
    registry.invoke_all(world, e, buf, viz_noflag);
    REQUIRE(buf.empty());

    // With ShowVelocity flag → arrow primitive emitted (line stem +
    // cone head triangles).
    DebugVizComponent viz_show{};
    viz_show.flags = DebugVizComponent::ShowVelocity;
    viz_show.scale = 1.0F;
    registry.invoke_all(world, e, buf, viz_show);
    REQUIRE(buf.line_count()     > 0U);
    REQUIRE(buf.triangle_count() > 0U);
}

TEST_CASE("eylem v1b-d static body (zero velocity) emits no arrow even with ShowVelocity",
          "[eylem][v1b-d][viz][body]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    BodyPool     body_pool{&alloc, 64U};
    ColliderPool collider_pool{&alloc, 64U};
    VisualizerRegistry registry{&alloc};
    register_eylem_visualizers(registry, body_pool, collider_pool);

    crd::scene::World world{&alloc};
    register_test_components(world);
    crd::eylem::RigidBody body{};
    body.linear_velocity = {0.0F, 0.0F, 0.0F}; // at rest
    const BodyId body_id = body_pool.insert(body);

    const auto e = world.spawn();
    crd::scene::Transform tr{};
    tr.world = crd::math::from_trs(tr.translation, tr.rotation, tr.scale);
    world.add_component(e, tr);
    RigidBodyComponent rbc{};
    rbc.body_id = body_id;
    world.add_component(e, rbc);
    DebugVizComponent viz{};
    viz.flags = DebugVizComponent::ShowVelocity;
    world.add_component(e, viz);

    RenderBuffer buf{&alloc};
    registry.invoke_all(world, e, buf, viz);
    // Below-threshold speed → no arrow drawn (would render as a tiny dot).
    REQUIRE(buf.empty());
}
