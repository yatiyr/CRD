#pragma once

#include <crd/core/types.hpp>

namespace crd::scene
{
// Phase 3.0 v1p — L7 ScriptComponent type freeze (ADR-0056 §2).
//
// `ScriptHandle` and `ScriptComponent` are reserved here so consumers can
// register the component from day one. The runtime payload is stored
// directly in the entity's component slot — no per-instance heap
// allocation. Phase 3.0 ships ONLY the type + storage hint contract;
// Phase 4.0's `ScriptSystem` (ADR-0034 + ADR-0056) is the consumer that
// turns a non-zero `ScriptHandle` into actual hot-reloadable behaviour
// dispatch, and `query.group_by<ScriptHandle>()` (reserved API surface,
// implementation deferred) is the bulk-iteration shape that vectorises
// one script per chunk.
//
// Day-one registration:
//
//   world.register_component<ScriptComponent>(StorageHint::SparseSet);
//
// Most agents in a million-agent simulation will NOT carry a script
// (the bulk are driven by archetype-uniform behaviour systems). Only
// boss AI, scripted events, and debug agents tag in. SparseSet matches:
// O(1) presence check, dense-array iteration, low memory cost when the
// component is sparse across the world. The choice is pinned in
// ADR-0056 §"Why scripts are SparseSet".

// Opaque handle into the hot-reloadable script DLL registry (ADR-0034).
// Phase 3.0 reserves the type — the integer body is uninterpreted at
// this layer. Phase 4.0's `ScriptRegistry` defines the lookup table
// `ScriptHandle → ScriptVTable`. Default (zero) means "no script" and
// is the inert sentinel — `ScriptSystem` skips entities whose handle
// is zero.
struct ScriptHandle
{
    crd::u64 raw = 0U;

    [[nodiscard]] constexpr bool is_null() const noexcept { return raw == 0U; }

    [[nodiscard]] constexpr bool operator==(const ScriptHandle&) const noexcept = default;
};

static_assert(sizeof(ScriptHandle)  == 8U, "ScriptHandle pinned at 8 bytes (ADR-0056 freeze)");
static_assert(alignof(ScriptHandle) == 8U, "ScriptHandle alignment pinned at 8 bytes");

// Per-entity script attachment. The `state_size` field describes the
// per-instance state blob the script reads/writes during `tick()`. In
// v1 the blob is stored externally (Phase 4.0 ScriptSystem owns the
// blob pool keyed by EntityId); v2 may inline N bytes following the
// `ScriptComponent` header for cache locality. The struct layout below
// is FROZEN by v1p — Phase 4.0 may add fields only by bumping a
// schema version on the cooked side, never by editing this header.
struct ScriptComponent
{
    ScriptHandle script;     // 8 B — null = inert (system skips).
    crd::u32     state_size; // 4 B — bytes in the ScriptSystem-owned blob.
    crd::u32     _reserved;  // 4 B — pad to 16 B; Phase 4.0 may repurpose.
};

static_assert(sizeof(ScriptComponent)  == 16U,
              "ScriptComponent pinned at 16 bytes (ADR-0056 freeze; consumer phase 4.0)");
static_assert(alignof(ScriptComponent) == 8U,
              "ScriptComponent alignment pinned at 8 bytes");

} // namespace crd::scene
