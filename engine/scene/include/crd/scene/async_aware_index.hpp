#pragma once

#include <crd/containers/hash_map.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/scene/component_index.hpp>
#include <crd/scene/entity.hpp>

namespace crd::scene
{
// LoadState — Phase 3.0 v1i (ADR-0053 §4, ADR-0039).
//
// Tracked per (EntityId, ComponentId) by AsyncAwareIndex. Default state
// when a component is inserted on an entity = Loading; the caller flips
// it to Loaded when their async work completes (mesh GPU upload, audio
// decode, scripted-asset bind, etc.).
enum class LoadState : crd::u8
{
    Loading = 0, // default on insert; pending async work
    Loaded  = 1, // ready for use by consumers
    Failed  = 2, // async work errored; consumers should treat as absent
};

// AsyncAwareIndex — Phase 3.0 v1i (ADR-0053 §4).
//
// Tracks LoadState per (entity, component) for components tagged with
// AsyncAware{} at registration. The `query<...>().skip_pending<T>()`
// operator filters entities whose T is in `Loading` state — renderers,
// physics, audio, etc. iterate only "ready" entities without manual
// "is the data ready?" checks.
//
// Default semantic — pinned 2026-05-07 v1i planning:
//   on_insert(e, c) → state[(c, e)] = Loading
//   mark_loaded(e, c) → state[(c, e)] = Loaded
//   mark_failed(e, c) → state[(c, e)] = Failed
//   on_remove / on_entity_destroyed → drop entry
//   .skip_pending<T>() excludes Loading (Failed/Loaded entities pass)
//
// If a caller has synchronously-set components that are immediately
// ready (no async work), they should NOT tag the component with
// AsyncAware{} — use the trait only when the data may legitimately be
// in flight.
class AsyncAwareIndex : public IComponentIndex
{
public:
    explicit AsyncAwareIndex(crd::memory::IAllocator* alloc);

    // ---- IStorageEventSink (via IComponentIndex) -----------------------

    void on_insert(EntityId e, ComponentId c, const void* data) override;
    void on_update(EntityId e, ComponentId c, const void* old_data, const void* new_data) override;
    void on_remove(EntityId e, ComponentId c, const void* data) override;
    void on_entity_destroyed(EntityId e) override;

    // ---- IComponentIndex -----------------------------------------------

    [[nodiscard]] ComponentMask observed() const override { return m_observed; }
    [[nodiscard]] crd::containers::StringView name() const override
    {
        return crd::containers::StringView{"AsyncAwareIndex"};
    }

    // ---- Public API ----------------------------------------------------

    // Add `c` to the observed mask. Auto-called by World::register_component
    // when AsyncAware{} is in the trait list.
    void watch(ComponentId c) noexcept { m_observed.set(c); }

    // Caller-driven state transitions. Call mark_loaded() when async work
    // for (e, c) finishes successfully; mark_failed() on error.
    void mark_loading(EntityId e, ComponentId c) noexcept;
    void mark_loaded(EntityId e, ComponentId c) noexcept;
    void mark_failed(EntityId e, ComponentId c) noexcept;

    // Query API — used by Query::skip_pending<T>() and direct callers.
    [[nodiscard]] LoadState load_state(EntityId e, ComponentId c) const noexcept;
    [[nodiscard]] bool is_pending(EntityId e, ComponentId c) const noexcept;
    [[nodiscard]] bool is_loaded(EntityId e, ComponentId c) const noexcept;

    // Diagnostics.
    [[nodiscard]] crd::usize tracked_entries() const noexcept { return m_state.size(); }

private:
    [[nodiscard]] static constexpr crd::u64 encode_key(ComponentId c, EntityId e) noexcept
    {
        const crd::u64 cid = static_cast<crd::u64>(c.raw);
        const crd::u64 idx = static_cast<crd::u64>(e.index());
        const crd::u64 gen = static_cast<crd::u64>(e.generation()) & 0xFFFFULL;
        return (cid << 48) | (gen << 32) | idx;
    }

    void set_state(EntityId e, ComponentId c, LoadState s) noexcept;

    ComponentMask                                  m_observed{};
    crd::containers::HashMap<crd::u64, LoadState>  m_state;
};

} // namespace crd::scene
