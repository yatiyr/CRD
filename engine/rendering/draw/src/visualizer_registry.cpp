// crd-draw -- VisualizerRegistry impl (Phase 3.1 v1a-draw d3, ADR-0066 sec 19.2).

#include <crd/draw/visualizer_registry.hpp>

#include <crd/draw/render_buffer.hpp>
#include <crd/scene/world.hpp>

namespace crd::draw
{
VisualizerRegistry::VisualizerRegistry(crd::memory::IAllocator* alloc) noexcept
    : m_entries(alloc)
{
}

void VisualizerRegistry::register_raw(ComponentFetchFn fetch, VisualizerFn visualize,
                                      Category category) noexcept
{
    m_entries.push_back(Entry{fetch, visualize, category});
}

void VisualizerRegistry::invoke_all(const crd::scene::World& world,
                                    crd::scene::EntityId    entity,
                                    RenderBuffer&           buf,
                                    const DebugVizComponent& viz) const noexcept
{
    for (const Entry& e : m_entries)
    {
        const void* comp = e.fetch(world, entity);
        if (comp == nullptr)
        {
            continue;
        }
        const VisualizerContext ctx{entity, &viz, e.category, &world};
        e.visualize(comp, buf, ctx);
    }
}

} // namespace crd::draw
