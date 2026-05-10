// crd-draw -- DebugVizSystem impl (Phase 3.1 v1a-draw d3, ADR-0066 sec 11-12).

#include <crd/draw/debug_viz_system.hpp>

#include <crd/draw/debug_viz_component.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/visualizer_registry.hpp>
#include <crd/scene/world.hpp>

namespace crd::draw
{
DebugVizSystem::DebugVizSystem(VisualizerRegistry& registry, RenderBuffer& buffer) noexcept
    : m_registry(&registry), m_buffer(&buffer)
{
}

crd::scene::SchedulePhase DebugVizSystem::phase() const
{
    return crd::scene::SchedulePhase::PostRender;
}

crd::containers::StringView DebugVizSystem::name() const
{
    return crd::containers::StringView{"DebugVizSystem"};
}

void DebugVizSystem::run(crd::scene::World& world)
{
    // Iterate every entity carrying DebugVizComponent. For each, dispatch
    // every registered visualizer; the registry skips entries whose
    // component type the entity doesn't carry.
    for (auto&& [entity, viz] : world.query<DebugVizComponent>())
    {
        m_registry->invoke_all(world, entity, *m_buffer, viz);
    }
}

} // namespace crd::draw
