# ADR-0049 — Scene/ECS L1: Entity identity & SlotMap

**Status:** Accepted
**Date:** 2026-05-06
**Tags:** scene, ecs, arch, layer-1

---

## Context

`crd-scene` is the foundation Phase 3.0 introduces. ADR-0020 established the high-level shape (hybrid SoA components + tree, UI in tree, TOML authoring → cooked binary). This ADR locks Layer 1 of the eight-layer architecture (`docs/phases/phase-3.0-scene-ecs.md`): the entity identity scheme and the slot map that backs it.

The scale target is **millions of live entities** (agents, particles, UI nodes, props). At that scale, every byte and every indirection in the entity handle compounds.

---

## Decision

### 1. `EntityId` is 64-bit, layout `[generation:32 | index:32]`

```cpp
struct EntityId
{
    crd::u64 raw = 0;

    [[nodiscard]] constexpr crd::u32 index()      const noexcept { return static_cast<crd::u32>(raw); }
    [[nodiscard]] constexpr crd::u32 generation() const noexcept { return static_cast<crd::u32>(raw >> 32); }
    [[nodiscard]] constexpr bool     is_null()    const noexcept { return raw == 0; }

    [[nodiscard]] static constexpr EntityId null() noexcept { return EntityId{0}; }
    [[nodiscard]] static constexpr EntityId make(crd::u32 idx, crd::u32 gen) noexcept
    {
        return EntityId{(static_cast<crd::u64>(gen) << 32) | idx};
    }
};
```

Trivial copy, fits one register, default-initialises to `null()`.

### 2. `[gen:32 | idx:32]` over `[gen:16 | idx:16]` or `[gen:8 | idx:24]`

- 4 G entity slots: comfortably above any realistic concurrent scene size (UE5 World Partition shipped at ~10 M).
- 4 G generations per slot: the slot can be reused effectively forever without wraparound.
- 16/16: caps at 64 K live entities — already inadequate for the agent simulations and UI populations Cerid targets.
- 8/24: 256 reuses per slot is a footgun. A slot recycling at a steady rate hits wraparound in seconds; once it does, a stale handle silently resolves to a newly allocated entity at the same index. We never want this class of bug.

### 3. Backing storage: dense `Array<Slot> + free-list head`

```cpp
struct Slot
{
    crd::u32           generation = 1;     // bumped on destroy
    crd::u32           next_free  = kInvalidIdx; // free-list link when vacant
    bool               alive      = false;
    // Component presence and component index pointers live in adjacent SoA arrays
    // owned by World, not in Slot — keeps Slot 8 bytes for cache friendliness.
};

class SlotMap
{
    crd::containers::Array<Slot> m_slots;          // index 0 reserved (sentinel)
    crd::u32                     m_free_head = kInvalidIdx;
    crd::u32                     m_alive_count = 0;
};
```

- Pointer-stable `ChunkedSlotMap` is rejected: component data lives in the storage layer (Archetype/SparseSet), not in slots. Slot moves are invisible to user code, so the chunked indirection costs more than it saves.
- Free list threaded through vacated slots — `next_free` field doubles as the free-list link when `alive == false`. No separate free-list allocation.
- Index 0 is reserved permanently as the null sentinel. `EntityId{0}` always resolves to a dead slot; no special-case branch needed.

### 4. Deferred (one-frame) destroy

`world.destroy(EntityId)` does not free the slot immediately. Instead:

```cpp
class World
{
    crd::containers::Array<EntityId> m_pending_destroy;

    void destroy(EntityId e) { m_pending_destroy.push_back(e); }

    void flush_destroys()  // called once at end-of-frame
    {
        for (EntityId e : m_pending_destroy)
        {
            actually_free_slot(e);   // bumps generation, links into free list,
                                     // notifies all storage backends and indexes
        }
        m_pending_destroy.clear();
    }
};
```

Reasoning: a system iterating `view<Transform, RenderableComponent>` cannot safely have entity X freed underneath it by another system (or a script) running on a different fiber. Deferring destroy to a single sync point at end-of-frame is the standard Bevy / EnTT / Unity DOTS shape and the only model that composes cleanly with parallel iteration.

For the rare case where immediate destroy is needed (test code, transient scratch entities, asset reload), we expose `world.destroy_immediate(EntityId)` with a precondition that no parallel iteration is in flight. Used sparingly.

### 5. Generation policy on slot reuse

```cpp
void actually_free_slot(EntityId e)
{
    Slot& s = m_slots[e.index()];
    CRD_ASSERT(s.alive && s.generation == e.generation());

    ++s.generation;        // wrap-around at 2^32 is documented as "extreme abuse";
                           // CRD_VERIFY traps if generation ever overflows in
                           // debug — practically unreachable.
    s.alive     = false;
    s.next_free = m_free_head;
    m_free_head = e.index();
    --m_alive_count;
}
```

A handle from the previous generation never matches the new generation; lookup returns `null()`. This is the safety property generational handles exist for.

### 6. Lookup contract

```cpp
[[nodiscard]] bool is_alive(EntityId e) const noexcept;          // O(1)
[[nodiscard]] crd::u32 entity_count() const noexcept;            // m_alive_count
[[nodiscard]] EntityIterator iterate_alive() const noexcept;     // skips holes
```

`is_alive(EntityId{0})` returns `false` by construction (slot 0 reserved, `alive == false` always). All other lookups validate generation match.

---

## Rationale

The 32/32 split is sized by the realistic upper bound of Cerid's targeted use cases (UE5 World Partition, RTS armies, particle simulations, UI populations) plus comfortable headroom. The cost relative to 16/16 is one extra cache line per ~150 entity slots — invisible. The cost relative to 8/24 is the absence of a class of subtle bugs.

Dense `Array<Slot>` is the canonical implementation. ChunkedSlotMap (per-chunk allocation, pointer stability) is appropriate when the slots themselves carry meaningful state that callers reference by pointer; in our design they don't. Storage backends own component data; slots only contain (generation, next_free, alive) — 8 bytes packed.

Deferred destroy is non-negotiable once parallel iteration is on the table, which it is from day one (jobified queries are ADR-0052). Immediate-destroy semantics could not coexist safely with ADR-0052's `par_each`.

---

## Consequences

- `EntityId` is 8 bytes, trivially copyable, default-zero is `null()`.
- A `World` with N alive entities and M holes has memory cost `(N + M) * sizeof(Slot)` = `(N + M) * 8` bytes for the slot map alone — 80 MB at 10 M live entities, well within budget.
- Destroying an entity costs O(1) at submission; the work happens at `flush_destroys()` once per frame.
- Storage backends and indexes (Layer 5) receive a single `on_entity_destroyed` event per entity per frame, dispatched in `flush_destroys()`.
- The reserved index-0 sentinel is documented; tests cover the case `world.is_alive(EntityId{0}) == false`.
- Generation overflow at 2^32 is treated as "the application has slot-thrashed for years" — `CRD_VERIFY` traps in debug; release silently wraps but the alive/dead bit prevents handle resurrection within the same frame.

---

## References

- ADR-0020 — Scene & ECS hybrid (cornerstone)
- ADR-0052 — Query · System · Schedule (Layer 4; depends on deferred destroy)
- ADR-0053 — Component index slot framework (Layer 5; receives entity-destroyed events)
- `docs/phases/phase-3.0-scene-ecs.md` — Phase plan, slices v1a–v1n
