#pragma once

// crd-draw -- VisualizerRegistry (Phase 3.1 v1a-draw d3, ADR-0066 sec 19.2).
//
// Typed function-pointer plug-in registry. Module owners (eylem-viz,
// renderer-viz, audio-viz, ...) call `register_for<T>(...)` once at
// startup to teach the registry how to draw a given component type.
// `DebugVizSystem` iterates all registrations every frame for every
// entity carrying a `DebugVizComponent`.
//
// Hook-based contract per CLAUDE.md/quality bar: no enum-of-known-types,
// no central switch on component kind. `crd-draw` knows nothing about
// rigid bodies / joints / colliders -- the consumer modules teach it via
// this registry.
//
// Performance: O(entities_with_DebugViz × registered_visualizers) per
// frame. Adequate for the d3 substrate (low entity counts during
// development); the ADR-0066 §19.2 "category-ordered iteration"
// optimisation lands when a real consumer has measurable perf pressure.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/draw/debug_viz_component.hpp>
#include <crd/draw/types.hpp>
#include <crd/scene/entity.hpp>
#include <crd/scene/world.hpp> // template register_for<T> needs World::get_component<T>

namespace crd::draw
{
class RenderBuffer;

// Per-call context passed to every visualizer invocation. Contains the
// hosting entity, its DebugVizComponent (never null inside the callback),
// and the Category bits the visualizer should stamp on emitted primitives
// (so the overlay-pass `category_mask` can disable categories of viz at
// zero cost).
struct VisualizerContext
{
    crd::scene::EntityId     entity;
    const DebugVizComponent* viz;
    Category                 category;
    // Reserved: lifetime / time_s / pick_id range. Wired in d4 alongside
    // the ImGui control panel.
};

// Type-erased visualizer callback.
//   `component` -- pointer to the entity's component data of the type
//                  this fn was registered for. Caller guarantees non-null.
//   `buf`       -- the per-frame RenderBuffer to emit primitives into.
//   `ctx`       -- per-call context.
using VisualizerFn = void (*)(const void* component, RenderBuffer& buf,
                              const VisualizerContext& ctx);

// Type-erased component fetch. Returns nullptr when `entity` does not
// carry the component this fn was registered for. Captured at template
// instantiation time so the registry can iterate without seeing T.
using ComponentFetchFn = const void* (*)(const crd::scene::World& world,
                                         crd::scene::EntityId    entity);

class VisualizerRegistry
{
public:
    explicit VisualizerRegistry(crd::memory::IAllocator* alloc =
                                    crd::memory::default_allocator()) noexcept;

    // Register a visualizer for component type T. The category is stamped
    // into `VisualizerContext::category` on every invocation; visualizers
    // forward it into the `PrimFlags` of any primitives they emit so the
    // overlay-pass category mask can disable them collectively.
    //
    // Multiple visualizers may register for the same T -- each is called
    // in registration order. Registering the same fn twice is allowed
    // (intentional duplicate emission); the registry does not dedupe.
    template <typename T>
    void register_for(VisualizerFn fn, Category category = Category::Debug);

    // Iterate all registered (fetch, visualize) pairs for `entity`. Calls
    // each visualizer whose fetch returns non-null. Used by DebugVizSystem.
    void invoke_all(const crd::scene::World& world, crd::scene::EntityId entity,
                    RenderBuffer& buf, const DebugVizComponent& viz) const noexcept;

    // Number of registrations. Tests + diagnostics.
    [[nodiscard]] crd::usize entry_count() const noexcept { return m_entries.size(); }

    // Forget all registrations. Tests use this to keep registry state
    // hermetic across cases.
    void clear() noexcept { m_entries.clear(); }

private:
    struct Entry
    {
        ComponentFetchFn fetch;
        VisualizerFn     visualize;
        Category         category;
    };
    crd::containers::Array<Entry> m_entries;

    void register_raw(ComponentFetchFn fetch, VisualizerFn visualize,
                      Category category) noexcept;
};

// Template impl. Captures T's identity in a non-template fetch lambda
// converted to a function pointer so the registry stays type-erased.
template <typename T>
void VisualizerRegistry::register_for(VisualizerFn fn, Category category)
{
    // Captureless lambda decays to function pointer when assigned to a
    // typed variable. The unary `+` is the standard idiom; MSVC dislikes
    // `static_cast<ComponentFetchFn>(lambda)` in this position.
    // `template` disambiguator on `get_component<T>` is required by MSVC
    // because T is dependent on the enclosing function template.
    // Captureless lambda decays to function pointer when assigned to a
    // typed variable.
    const ComponentFetchFn fetch =
        +[](const crd::scene::World& w, crd::scene::EntityId e) -> const void*
    {
        return static_cast<const void*>(w.template get_component<T>(e));
    };
    register_raw(fetch, fn, category);
}

} // namespace crd::draw
