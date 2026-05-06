# ADR-0056 — Scene/ECS L6–L8: Reserved API slots (Replication, Scripts, Reflection)

**Status:** Accepted
**Date:** 2026-05-06
**Tags:** scene, ecs, arch, layer-6, layer-7, layer-8, networking, scripting, editor

---

## Context

Phase 3.0 ships Layers 1–5 of the eight-layer architecture (`docs/phases/phase-3.0-scene-ecs.md`). Layers 6–8 are **deferred for implementation but not for design** — their API contracts must be locked now so Phase 3.0 doesn't accidentally close the doors they need to walk through later.

This ADR locks the registration grammar and the integration points for:

- **L6: Replication / Networking** (consumed in Phase 4.2, ADR-0035)
- **L7: Scripts & Behaviors** (consumed in Phase 4.0, ADR-0034)
- **L8: Reflection / Editor** (consumed in Phase 7)

Implementations land in their respective consumer phases. What lands in Phase 3.0 is the *slot shape* — the registration parameter, the integration point, the no-op default. Adding any of these later without API damage requires reserving them now.

---

## Decision

### 1. Layer 6 — Replication: per-component policy at registration

Each component registers a replication policy:

```cpp
enum class Replication : crd::u8
{
    Local              = 0,   // never replicated; default
    ServerAuthoritative,      // server writes, clients read; sent on change
    ClientPredicted,          // client writes, server validates; rollback-capable
    Remote,                   // read-only mirror of remote owner's state
};

world.register_component<Transform>(
    StorageHint::Archetype,
    Replication::ServerAuthoritative,    // reserved slot — Phase 4.2 honours
    History{60}                          // for rollback
);
world.register_component<PlayerInput>(
    StorageHint::Archetype,
    Replication::ClientPredicted
);
world.register_component<EditorSelected>(
    StorageHint::SparseSet,
    Replication::Local                   // never sent over network
);
```

Phase 3.0 stores the policy and otherwise no-ops. Phase 4.2 ships a `ReplicationIndex` (Layer 5 observer per ADR-0053) that:

- Watches `ChangeDetect` events on replicated components.
- Builds delta packets per tick.
- Drives the rewind-and-replay path against `History` for rollback.

`Replication` is part of the registration grammar from day one. Adding it later would be a breaking change to every component registration; reserving the parameter now is free (`crd::u8` per registration entry).

### 2. Layer 7 — Scripts as components

Scripting is implemented by a `ScriptComponent` whose payload is a hot-reloadable function pointer + per-instance state blob:

```cpp
// Phase 4.0 fills these in. Phase 3.0 reserves the type and the schedule
// integration point.
struct ScriptComponent
{
    ScriptHandle script;     // handle into hot-reloadable DLL (ADR-0034)
    crd::u32     state_size; // size of state blob in bytes
    // followed by N bytes of state (variable per script)
};

// Phase 3.0 registers it as a regular component (no impl yet — script handle
// is null, system is no-op):
world.register_component<ScriptComponent>(
    StorageHint::SparseSet   // scripts are entity-tagged, not bulk-stored
);
```

The bulk-iteration model:

```cpp
// Phase 4.0 implementation:
class ScriptSystem : public ISystem
{
    SchedulePhase phase() const override { return SchedulePhase::Update; }

    void run(World& world, jobs::Counter& fence) override
    {
        // Group scripts by ScriptHandle so each chunk vectorises one script.
        auto by_script = world.query<ScriptComponent>().group_by<ScriptHandle>();

        for (auto& [handle, group] : by_script)
        {
            const ScriptVTable& vt = ScriptRegistry::lookup(handle);
            group.par_each(jobs::pool(), &fence,
                [&vt](EntityId e, ScriptComponent& sc) {
                    vt.tick(e, sc.state_blob());
                });
        }
    }
};
```

Phase 3.0 reserves:
- `ScriptComponent` type + storage hint.
- `query.group_by<T>()` operator (DSL, ADR-0052) — reserved API; impl when scripts ship.
- `ScriptHandle` opaque type alias.

For million-agent simulation: this combination — `SparseSet` storage (cheap toggle on/off), chunk-grouped iteration, jobified per-group — matches Unreal Mass and Unity DOTS shapes for "Mass AI" workloads.

### 3. Layer 8 — Reflection: opt-in per component

Reflection is opt-in. Components register their field layout *additionally* — separate from `ComponentSerialize` (ADR-0055), which handles save/load:

```cpp
world.register_component<Transform>(
    StorageHint::Archetype,
    History{8},
    AsyncAware{},
    SpatialBVH{},
    GpuResident{},
    Replication::ServerAuthoritative,
    ComponentSerialize{ /* TOML/blob round-trip */ },
    Reflection{ /* field-level inspector access — Phase 7 fills this */ }
);
```

`Reflection` carries:
- Display name.
- Field list with name, type, offset, optional editor metadata (range, units, color-picker, etc.).
- Reset-to-default callback.

Phase 3.0 stores the reflection record and stops. Phase 7 implements the editor inspector that walks reflection records to render UI per component. Components without `Reflection` are valid (they exist, they save, they replicate) but the editor displays them as opaque blobs.

This split keeps the editor's complexity out of the runtime: a release build with `Reflection` registrations still pays only the storage cost of the records (~64 bytes per registered component). The walker code is editor-only.

### 4. Integration: all three layers ride the Layer 5 framework

Replication is implemented as a Layer 5 `IComponentIndex` (ADR-0053). It observes change events on replicated components, builds packets at frame end, applies received packets at frame begin. No new core machinery in Phase 4.2 — same observer interface as `ChangeDetectIndex`.

Scripts are implemented as a regular `ISystem` operating on `ScriptComponent`. Hot-reload is delegated to the DLL machinery (ADR-0034); the ECS layer doesn't know about hot-reload.

Reflection records are stored alongside component registrations. Editor (Phase 7) walks the registry; runtime never touches reflection data.

**Three deferred features, three different integration shapes, all riding existing slots.** This is what "extensible from day one" means in practice: the slots accept the parameters today, the implementations land later, and no caller code changes when they do.

---

## Rationale

### Why reserve all three at once

The cost of reserving an API parameter is one struct field per component registration entry. A no-op runtime that accepts `Replication::ServerAuthoritative` and stores it in a 1-byte field is essentially free.

The cost of *not* reserving and adding later: every component registration in user code must be touched. With ~50 component registrations across game code + UI code + editor code by Phase 4.0, that is 50 breaking changes for a single feature addition. We don't pay that cost.

### Why integrate via Layer 5 where possible

Replication is a natural Layer 5 index — it observes component changes and produces side-effects (packets). Treating it as a special case would require new dispatch machinery; treating it as an Index reuses what already exists. ADR-0053 designed Layer 5 as a generic extension point precisely so features like Replication slot in.

Scripts and Reflection don't fit Layer 5's observer model (they don't react to lifecycle events; they are read by a system or walked by tooling). They get their own integration shape, but reuse the registration grammar for consistency.

### Why scripts are SparseSet

Most agents in a million-agent simulation will *not* have a custom script — they'll be controlled by archetype-uniform behaviour systems. The few that do (boss AI, scripted events, debug agents) are sparse. SparseSet matches: O(1) presence check, dense-array iteration, low memory cost when most entities lack the component.

When ultra-scale scripted-agent simulation arrives (UE Mass equivalent for 1M scripted entities), an Archetype-stored `BehaviorState` component (data-driven state machine, not arbitrary script) becomes appropriate. That's a separate feature, not a scripts replacement.

---

## Consequences

- `world.register_component<T>(...)` accepts variadic trait parameters from Phase 3.0 onward, including:
  - `StorageHint` (ADR-0050)
  - `History{N}`, `SpatialBVH{}`, `GpuResident{}`, `AsyncAware{}` (ADR-0053)
  - `Replication::*` (this ADR)
  - `ComponentSerialize{...}` (ADR-0055)
  - `Reflection{...}` (this ADR)
- Phase 3.0 implementation parses, stores, and acts on `StorageHint`, `AsyncAware`, `ComponentSerialize`. Other traits are stored and otherwise no-op.
- Phase 4.0 (scripts) lands `ScriptComponent` + `ScriptSystem` + `query.group_by<T>()` impl. No registration grammar changes.
- Phase 4.2 (networking) lands `ReplicationIndex` + delta-packet machinery. No registration grammar changes.
- Phase 7 (editor) lands the reflection walker + inspector UI. No registration grammar changes.
- All three deferred-implementation features are testable as no-ops in Phase 3.0: registering `Replication::ServerAuthoritative` validates the parser; runtime ignores it; tests cover that the field round-trips correctly.

---

## References

- ADR-0020 — Scene & ECS hybrid (cornerstone)
- ADR-0034 — C++ hot-reload DLL scripting (Layer 7 backbone)
- ADR-0035 — Networking architecture (Layer 6 consumer)
- ADR-0049 — Entity identity (Layer 1)
- ADR-0050 — Storage backends (Layer 2)
- ADR-0052 — Query · System · Schedule (Layer 4; group_by<T> reserved)
- ADR-0053 — Component index slot framework (Layer 5; Replication rides this)
- ADR-0055 — Scene serialization (ComponentSerialize trait, sibling of Reflection)
