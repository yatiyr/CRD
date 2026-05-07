#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/scene/component_index.hpp>
#include <crd/scene/entity.hpp>

namespace crd::scene
{
// ChangeDetectIndex — Phase 3.0 v1i (ADR-0053 §3).
//
// Per-entity, per-component "last modified frame" tracker. Drives the
// `query<...>().changed<T>()` operator: emit only entities whose T was
// inserted, updated, or removed during the current frame.
//
// Semantics — pinned 2026-05-07 v1i planning:
//   .changed<T>() captures `since_frame = world.current_frame()` at
//   construction. Per-entity filter: pass iff
//   `last_change_frame[(c, e)] >= since_frame`.
//
// Key consequence: writes that happen DURING the current frame, in any
// order vs query construction, are caught. Cross-phase pattern works:
//   PrePhysics writes Transform → PreRender's
//   `query<Transform>().changed<Transform>()` sees those entities.
//
// Cross-frame "what changed since last time my system ran" is the v1h+1
// evolution (requires per-system tracking; v1g's queries are
// constructed-fresh-per-step so they can't carry cross-frame snapshots).
//
// Memory cost: 16 B per (entity, watched-component) pair currently
// modified-or-recently-modified. The HashMap is grown on first event;
// entries are dropped when the entity is destroyed. Chunk-grain tracking
// (cheaper for large worlds) is the v1h+1 optimisation slot per ADR-0053
// §3 "false positives at chunk granularity are accepted."
class ChangeDetectIndex : public IComponentIndex
{
public:
    explicit ChangeDetectIndex(crd::memory::IAllocator* alloc);

    // ---- IStorageEventSink (via IComponentIndex) -----------------------

    void on_insert(EntityId e, ComponentId c, const void* data) override;
    void on_update(EntityId e, ComponentId c, const void* old_data, const void* new_data) override;
    void on_remove(EntityId e, ComponentId c, const void* data) override;
    void on_entity_destroyed(EntityId e) override;

    // ---- IComponentIndex -----------------------------------------------

    [[nodiscard]] ComponentMask observed() const override { return m_observed; }
    void on_frame_begin(crd::u32 frame_index) override { m_current_frame = frame_index; }
    [[nodiscard]] crd::containers::StringView name() const override
    {
        return crd::containers::StringView{"ChangeDetectIndex"};
    }

    // ---- Public API ----------------------------------------------------

    // Add `c` to the observed mask. Auto-called by World on every
    // register_component (ChangeDetect watches every registered type).
    void watch(ComponentId c) noexcept { m_observed.set(c); }

    // True if (entity, component) was modified at frame >= since_frame.
    // since_frame == 0 matches everything ever recorded.
    [[nodiscard]] bool changed_since(EntityId e, ComponentId c, crd::u32 since_frame) const noexcept;

    // Diagnostics.
    [[nodiscard]] crd::u32 current_frame() const noexcept { return m_current_frame; }
    [[nodiscard]] crd::usize tracked_entries() const noexcept { return m_last_change_frame.size(); }

private:
    // Encode (component_id, entity_id) into a single u64 key for the
    // HashMap. Component is in the high 16 bits; entity index is in the
    // low 32; the entity generation in the middle 16 (lossy — collisions
    // are possible across generations, but irrelevant: ChangeDetect is a
    // chunk/frame-grain hint, not a security boundary).
    [[nodiscard]] static constexpr crd::u64 encode_key(ComponentId c, EntityId e) noexcept
    {
        const crd::u64 cid = static_cast<crd::u64>(c.raw);
        const crd::u64 idx = static_cast<crd::u64>(e.index());
        const crd::u64 gen = static_cast<crd::u64>(e.generation()) & 0xFFFFULL;
        return (cid << 48) | (gen << 32) | idx;
    }

    void record(ComponentId c, EntityId e) noexcept;

    ComponentMask                              m_observed{};
    crd::u32                                   m_current_frame = 0;
    crd::containers::HashMap<crd::u64, crd::u32> m_last_change_frame;
};

} // namespace crd::scene
