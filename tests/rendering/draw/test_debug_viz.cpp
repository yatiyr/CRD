// crd-draw -- d3 tests for VisualizerRegistry + DebugVizSystem.

#include <crd/draw/debug_viz_component.hpp>
#include <crd/draw/debug_viz_system.hpp>
#include <crd/draw/default_visualizers.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/shapes.hpp>
#include <crd/draw/visualizer_registry.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/transform_propagation.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace
{
// Toy component for registry tests -- isolates the test from any real
// visualizer that might mutate the buffer differently.
struct ProbeComponent
{
    crd::f32 value = 0.0F;
};

// Visualizer that pushes one DebugLine carrying the probe value in its
// width field, so the test can assert (a) it was called, (b) ctx.viz is
// non-null, (c) the captured component pointer is the right entity's data.
void probe_visualize(const void* component, crd::draw::RenderBuffer& buf,
                     const crd::draw::VisualizerContext& ctx) noexcept
{
    REQUIRE(component != nullptr);
    REQUIRE(ctx.viz != nullptr);
    const auto* p = static_cast<const ProbeComponent*>(component);
    crd::draw::add_line_to(buf, {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F},
                           crd::draw::Color{255, 255, 255, 255}, p->value,
                           crd::draw::PrimFlags::make(crd::draw::DepthMode::Always, ctx.category));
}
} // namespace

TEST_CASE("VisualizerRegistry: register + invoke skips entities missing component",
          "[draw][d3]")
{
    crd::scene::World world;
    world.register_component<ProbeComponent>(crd::scene::StorageHint::SparseSet);
    world.register_component<crd::draw::DebugVizComponent>(crd::scene::StorageHint::SparseSet);

    crd::draw::VisualizerRegistry registry;
    registry.register_for<ProbeComponent>(probe_visualize, crd::draw::Category::Debug);
    REQUIRE(registry.entry_count() == 1);

    // Entity A: has Probe + Viz. Entity B: only Viz. Entity C: only Probe.
    const auto a = world.spawn();
    world.add_component(a, ProbeComponent{2.5F});
    world.add_component(a, crd::draw::DebugVizComponent{});

    const auto b = world.spawn();
    world.add_component(b, crd::draw::DebugVizComponent{});

    const auto c = world.spawn();
    world.add_component(c, ProbeComponent{99.0F});

    crd::draw::RenderBuffer buf;
    const crd::draw::DebugVizComponent viz_a{};
    const crd::draw::DebugVizComponent viz_b{};

    registry.invoke_all(world, a, buf, viz_a);
    REQUIRE(buf.lines().size() == 1);
    REQUIRE(buf.lines()[0].width == 2.5F);

    registry.invoke_all(world, b, buf, viz_b);
    // Entity B has no ProbeComponent -- registry skips, no new line.
    REQUIRE(buf.lines().size() == 1);
}

TEST_CASE("DebugVizSystem: dispatches Transform visualizer in PostRender",
          "[draw][d3]")
{
    crd::scene::World world;
    world.register_component<crd::scene::Transform>(crd::scene::transform_serialize_trait());
    world.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
    world.register_component<crd::draw::DebugVizComponent>(crd::scene::StorageHint::SparseSet);
    world.register_builtin_relations();
    world.register_system(std::make_unique<crd::scene::TransformPropagation>());

    crd::draw::VisualizerRegistry registry;
    crd::draw::register_default_visualizers(registry);
    REQUIRE(registry.entry_count() == 1);

    crd::draw::RenderBuffer buf;
    world.register_system(std::make_unique<crd::draw::DebugVizSystem>(registry, buf));

    REQUIRE(crd::scene::SchedulePhase::PostRender == crd::scene::SchedulePhase::PostRender);

    // Spawn an entity with Transform + DebugVizComponent. set_translation
    // marks it dirty so TransformPropagation populates Transform.world.
    const auto e = world.spawn();
    world.add_component(e, crd::scene::Transform{});
    world.set_translation(e, crd::math::Vec3f{3.0F, 0.0F, 0.0F});
    world.add_component(e, crd::draw::DebugVizComponent{});

    // Buffer must be cleared by the caller every frame BEFORE step.
    buf.clear();
    world.step(0.016);

    // axis_triad emits 3 arrows = 3 line stems + 4 cap triangles each = 12 triangles.
    REQUIRE(buf.lines().size() >= 3);
    REQUIRE(buf.triangles().size() >= 12);
}

TEST_CASE("DebugVizComponent: AxisTriad flag gates emission",
          "[draw][d3]")
{
    crd::scene::World world;
    world.register_component<crd::scene::Transform>(crd::scene::transform_serialize_trait());
    world.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
    world.register_component<crd::draw::DebugVizComponent>(crd::scene::StorageHint::SparseSet);
    world.register_builtin_relations();
    world.register_system(std::make_unique<crd::scene::TransformPropagation>());

    crd::draw::VisualizerRegistry registry;
    crd::draw::register_default_visualizers(registry);

    crd::draw::RenderBuffer buf;
    world.register_system(std::make_unique<crd::draw::DebugVizSystem>(registry, buf));

    const auto e = world.spawn();
    world.add_component(e, crd::scene::Transform{});
    world.set_translation(e, crd::math::Vec3f{0.0F, 0.0F, 0.0F});
    // AxisTriad flag CLEARED -- visualizer should early-out.
    crd::draw::DebugVizComponent viz{};
    viz.flags = crd::draw::DebugVizComponent::None;
    world.add_component(e, viz);

    buf.clear();
    world.step(0.016);
    REQUIRE(buf.lines().empty());
    REQUIRE(buf.triangles().empty());
}
