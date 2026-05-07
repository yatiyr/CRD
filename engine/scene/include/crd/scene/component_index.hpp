#pragma once

#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/entity.hpp>
#include <crd/scene/storage_event_sink.hpp>

namespace crd::scene
{
// IComponentIndex — Phase 3.0 v1i (ADR-0053 §1).
//
// Layer 5's plug-point. Every "novel ECS extension" that needs to react to
// component lifecycle events implements this interface and registers with
// the World; the storage backends fan events out automatically.
//
// Inheritance from IStorageEventSink ensures one event shape across the
// whole stack — storage backends emit IStorageEventSink events, World's
// fan-out sink dispatches them to every registered IComponentIndex whose
// observed() mask includes the relevant component.
//
// The Cerid signature: adding a new ECS extension (metrics, editor
// selection, network priority, sound occlusion, AI threat tracking) is a
// one-day job — implement IComponentIndex, call world.register_index, add
// query operators that read the index's state. No patches to crd-scene
// core.
//
// Frame lifecycle hooks (on_frame_begin / on_frame_end) fire once per
// World::step / step_fixed call (NOT per fixed substep). Indexes that
// need to advance per-frame state (ChangeDetect's frame counter,
// AsyncAware's pending-load reaper, History's frame snapshotting) hook
// these.
class IComponentIndex : public IStorageEventSink
{
public:
    // Bitmask of components this index observes. Storage events for
    // components NOT in this mask are skipped at the fan-out — no
    // virtual call cost. Indexes that observe everything (e.g.
    // ChangeDetect) return an all-set mask.
    [[nodiscard]] virtual ComponentMask observed() const = 0;

    // Frame lifecycle. Called by World::step / step_fixed AROUND the
    // 7-phase dispatch (begin before phase 0, end after phase 6). Called
    // ONCE per step call, not per fixed substep.
    virtual void on_frame_begin(crd::u32 frame_index) { (void)frame_index; }
    virtual void on_frame_end(crd::u32 frame_index) { (void)frame_index; }

    // Diagnostics / profiler labels.
    [[nodiscard]] virtual crd::containers::StringView name() const = 0;
};

} // namespace crd::scene
