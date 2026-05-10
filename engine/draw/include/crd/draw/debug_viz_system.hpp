#pragma once

// crd-draw -- DebugVizSystem (Phase 3.1 v1a-draw d3, ADR-0066 sec 11-12).
//
// ECS system that runs in `SchedulePhase::PostRender`, iterates every
// entity carrying a `DebugVizComponent`, and dispatches every registered
// visualizer for that entity's components into the supplied
// `RenderBuffer`.
//
// Wiring:
//
//   1. world.register_component<DebugVizComponent>(StorageHint::SparseSet);
//   2. VisualizerRegistry registry;
//      register_default_visualizers(registry);   // Transform → axis triad
//      // Plus any module-specific registrations (eylem-viz, ...).
//   3. world.register_system(std::make_unique<DebugVizSystem>(registry, &buffer));
//   4. Per frame, BEFORE world.step():  buffer.clear();
//   5. After world.step(), submit `buffer` via `add_draw_overlay_pass`.
//
// Single-path contract per quality bar: the buffer pointer is mandatory,
// no thread-local-active fallback. If you want active-buffer routing,
// register a thin wrapper system that uses `crd::draw::active_buffer()`.

#include <crd/containers/string_view.hpp>
#include <crd/scene/system.hpp>

namespace crd::draw
{
class RenderBuffer;
class VisualizerRegistry;

class DebugVizSystem : public crd::scene::ISystem
{
public:
    // `registry` and `buffer` are non-owning; both must outlive the system.
    DebugVizSystem(VisualizerRegistry& registry, RenderBuffer& buffer) noexcept;

    [[nodiscard]] crd::scene::SchedulePhase phase() const override;
    void                                     run(crd::scene::World& world) override;
    [[nodiscard]] crd::containers::StringView name() const override;

private:
    VisualizerRegistry* m_registry;
    RenderBuffer*       m_buffer;
};

} // namespace crd::draw
