#pragma once

#include <crd/scene/component.hpp>
#include <crd/scene/entity.hpp>

namespace crd::scene
{
// IStorageEventSink — the single hook that makes Cerid's L5 IComponentIndex
// framework (ADR-0053) a one-day extension instead of a refactor.
//
// Storage backends call into this interface on every component lifecycle
// event. Phase 3.0 v1c2 ships the call sites with a NullStorageEventSink
// default. Phase 3.0 v1i ships an event-fan-out sink that drives every
// registered IComponentIndex (ChangeDetect, AsyncAware ship there;
// History / SpatialBVH / GpuResident / Replication / Reflection / Scripts
// reserve their slots and fill in during their consumer phases without
// touching this interface or the storage call sites).
//
// Why the indirection: by routing every storage mutation through one
// pluggable sink, the eight-layer architecture's "extensibility from day
// one" property is realised. Any future ECS extension (metrics, editor
// selection, sound occlusion, network priority, AI threat) is implemented
// as an IComponentIndex; the v1i sink fans events to it; no storage
// call-site changes.
class IStorageEventSink
{
public:
    virtual ~IStorageEventSink() = default;

    // A new component value was placed on `entity`. `data` points to the
    // freshly-installed component bytes (same pointer as ChunkLayout offset);
    // valid only for the duration of the call.
    virtual void on_insert(EntityId entity, ComponentId component, const void* data) = 0;

    // A component value was overwritten in place via `get_mut` or upsert.
    // `old_data` and `new_data` may alias (storage typically passes the same
    // pointer twice on chunk-grain updates — this is acceptable; ChangeDetect
    // ignores `old_data` and listens only at chunk granularity).
    virtual void on_update(EntityId entity, ComponentId component, const void* old_data, const void* new_data) = 0;

    // The component was removed from `entity`. Last chance to read its bytes
    // before the storage destructs the slot.
    virtual void on_remove(EntityId entity, ComponentId component, const void* data) = 0;

    // The entity itself was destroyed. Storage has not yet torn down its
    // components — the sink may iterate `entity` once more if needed.
    // Called from `IStorageBackend::on_entity_destroyed`.
    virtual void on_entity_destroyed(EntityId entity) = 0;
};

// NullStorageEventSink — the default sink installed by ArchetypeChunkStorage
// when no IComponentIndex framework is wired. v1i replaces it with a fan-out
// sink. All methods are no-ops.
class NullStorageEventSink final : public IStorageEventSink
{
public:
    void on_insert(EntityId, ComponentId, const void*) override {}
    void on_update(EntityId, ComponentId, const void*, const void*) override {}
    void on_remove(EntityId, ComponentId, const void*) override {}
    void on_entity_destroyed(EntityId) override {}

    // The single shared instance. Returned by ArchetypeChunkStorage when no
    // explicit sink has been set.
    [[nodiscard]] static IStorageEventSink* instance() noexcept;
};

} // namespace crd::scene
