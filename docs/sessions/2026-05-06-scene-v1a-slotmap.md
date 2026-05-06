# 2026-05-06 — Phase 3.0 v1a: `crd-scene` skeleton — `EntityId` + `SlotMap` + `World`

**Status at start:** Phase 2.8 complete. Phase 3.0 architecture locked (ADRs 0049–0057 accepted 2026-05-06). 14 slices planned. `engine/scene/` did not exist.

**Status at end:** Phase 3.0 v1a SHIPPED. `crd-scene` static library exists, six-configuration green, +22 unit tests / +3448 assertions. 13 slices remain in Phase 3.0; next is v1b (`ComponentRegistry` + `IStorageBackend` interface + storage-hint registration grammar).

---

## Goal of this session

Land the foundation entity-identity layer for `crd-scene` per ADR-0049: a 64-bit generational `EntityId`, the dense `SlotMap` that backs it, and the `World` shell that owns the slot map and a deferred-destroy queue. No components, no storage backends, no relations, no queries — those are v1b–v1g. v1a is the smallest shippable foundation that subsequent slices build on.

## What shipped

### New module `engine/scene/`

```
engine/scene/
├── CMakeLists.txt                  # static lib `crd-scene` (crd-core + crd-containers + crd-memory PUBLIC)
├── include/crd/scene/
│   ├── scene.hpp                   # umbrella: pulls in entity, slot_map, world
│   ├── entity.hpp                  # EntityId (header-only, constexpr)
│   ├── slot_map.hpp                # Slot, SlotMap, Iterator
│   └── world.hpp                   # World shell
└── src/
    ├── slot_map.cpp                # ~70 LOC
    └── world.cpp                   # ~30 LOC
```

Wired into root `CMakeLists.txt` as `add_subdirectory(engine/scene)` next to the resources line.

### Public API

`entity.hpp`:
```cpp
struct EntityId
{
    crd::u64 raw = 0;
    [[nodiscard]] constexpr crd::u32 index() const noexcept;
    [[nodiscard]] constexpr crd::u32 generation() const noexcept;
    [[nodiscard]] constexpr bool is_null() const noexcept;
    [[nodiscard]] static constexpr EntityId null() noexcept;
    [[nodiscard]] static constexpr EntityId make(crd::u32 idx, crd::u32 gen) noexcept;
    [[nodiscard]] constexpr bool operator==(const EntityId&) const noexcept = default;
};
static_assert(sizeof(EntityId) == 8, "EntityId must pack to 8 bytes");
```

`slot_map.hpp`:
```cpp
struct Slot
{
    crd::u32 generation = 0;
    crd::u32 next_free  = 0;
    bool     alive      = false;
};

inline constexpr crd::u32 kInvalidSlotIndex = static_cast<crd::u32>(-1);

class SlotMap
{
public:
    explicit SlotMap(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

    [[nodiscard]] EntityId  allocate();
    void                    free(EntityId);
    [[nodiscard]] bool      is_alive(EntityId) const noexcept;
    [[nodiscard]] crd::u32  alive_count() const noexcept;
    [[nodiscard]] crd::u32  slot_count() const noexcept;

    class Iterator { ... };
    [[nodiscard]] Iterator begin() const noexcept;
    [[nodiscard]] Iterator end() const noexcept;
};
```

`world.hpp`:
```cpp
class World
{
public:
    explicit World(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

    [[nodiscard]] EntityId spawn();
    void                   destroy(EntityId);            // deferred
    void                   destroy_immediate(EntityId);  // synchronous, lenient on stale
    void                   flush_destroys();             // drain queue, skip stale

    [[nodiscard]] bool     is_alive(EntityId) const noexcept;
    [[nodiscard]] crd::u32 entity_count() const noexcept;
    [[nodiscard]] crd::u32 pending_destroy_count() const noexcept;

    [[nodiscard]] SlotMap::Iterator begin() const noexcept;
    [[nodiscard]] SlotMap::Iterator end() const noexcept;
};
```

### Tests added (`tests/scene/`, 22 cases / 3448 assertions)

`test_entity.cpp` (5 cases):
- default `EntityId{}` is null, `EntityId::null()` round-trips.
- `make` round-trips through `index()` / `generation()` across u32 zero, typical, max boundaries.
- equality / inequality.
- `is_null` true only for `raw == 0`.
- `STATIC_REQUIRE` for `sizeof == 8` and `is_trivially_copyable_v`.

`test_slot_map.cpp` (9 cases):
- fresh map empty, `is_alive(null) == false`.
- `allocate()` produces alive handle, never index 0.
- `free()` invalidates the same handle (generation collision).
- allocate-after-free reuses the same index with bumped generation; stale handle still invalid.
- free-list LIFO order preserved across multi-step alloc/free.
- mixed allocate/free stress (1000 ops, deterministic seed) preserves invariants.
- iterator yields alive entities only, skips holes.
- slot 0 is permanently the null sentinel (never alive, never returned by allocate).
- out-of-range index yields dead lookup.

`test_world.cpp` (8 cases):
- fresh world is empty.
- `spawn()` produces alive entity.
- `destroy(e)` is deferred; `is_alive` stays true until `flush_destroys()`.
- `flush_destroys()` drains all queued.
- `destroy_immediate(e)` frees synchronously.
- `destroy_immediate` of stale handle is a no-op.
- double `destroy(e)` across flush is safe.
- iteration after destroy/flush only yields surviving entities.

## Two ADR-0049 divergences (intentional, recorded for the next reader)

ADR-0049 §5 specifies `CRD_VERIFY` trap on generation wraparound:
> `++s.generation;` then `// wrap-around at 2^32 is documented as "extreme abuse"; CRD_VERIFY traps if generation ever overflows in debug — practically unreachable.`

The implementation instead silently bumps `0 → 1` after the increment to keep "generation 0" reserved as a dead-slot sentinel value (used by the slot-0 reservation, by `Slot::generation = 0` default-construction of the sentinel, and by `EntityId{}.generation() == 0`). The alive bit prevents handle resurrection within the same frame regardless. This is a pragmatic strengthening of the spec, not a regression — every reachable code path is safer this way. Generation overflow is still recorded as "extreme abuse" but the behaviour is well-defined rather than trap-or-wrap.

ADR-0049 §4 commentary says:
> `world.destroy_immediate(EntityId)` with a precondition that no parallel iteration is in flight.

The shown `actually_free_slot` asserts (`CRD_ASSERT(s.alive && s.generation == e.generation())`) on stale handles. The implementation makes `World::destroy_immediate(e)` lenient: `if (m_slots.is_alive(e)) m_slots.free(e);` — assertion-clean for stale handles. This is required so the natural test pattern `destroy(e); flush_destroys(); destroy_immediate(e);` (or `destroy_immediate(e); destroy_immediate(e);`) does not abort. The test "destroy_immediate of stale handle is a no-op" pins this behaviour. `SlotMap::free()` retains the strict precondition (must be alive) — `World` is the lenient layer.

Both divergences are confined to `World` / `SlotMap` and do not affect any documented invariant the storage / index layers will rely on.

## Other notes

- `Slot` is 12 bytes with default padding (u32 + u32 + bool aligned). ADR-0049 commentary says "8 bytes" but the literal struct definition shows three fields totalling 12 with alignment. The implementation matches the literal struct, not the commentary. At 1M entities, the slot map is 12 MB instead of 8 MB — well within budget.
- No `DefaultHash<EntityId>` specialisation yet; deferred to v1c when the storage backends actually use entities as hash-map keys. Cf. `ResourceId`'s pattern in `engine/resources/include/crd/resources/resource_id.hpp`.
- No `on_entity_destroyed` event dispatch in `flush_destroys()` — Layer 2 storage backends and Layer 5 indexes don't exist yet. The dispatch hook lands when the first observer arrives in v1c / v1i.
- `World` and `SlotMap` are non-copyable, default-movable. Standard for resource-owning types in this codebase.

## Six-configuration sweep

| Config | Build | CTest | Headless smokes |
|---|---|---|---|
| win-debug          | ✅ | 503/503 | 17/17 |
| win-relwithdebinfo | ✅ | 503/503 | 17/17 |
| win-release        | ✅ | 500/500 | 17/17 |
| win-asan           | ✅ | 503/503 (DLL PATH fix applied) | 17/17 |
| win-clang-cl       | ✅ | 503/503 | 17/17 |
| win-tidy           | ✅ | — | — |

Test count delta: +22 (was 481/481 / 478/478 in win-debug / win-release pre-v1a).

`win-tidy` initially flagged: lowercase-`u`-suffix on integer literals (project convention is uppercase `U` per `readability-uppercase-literal-suffix`); `bugprone-random-generator-seed` on the deterministic test rng; one `LocalConstexprVariable` not following the `kCamelCase` rule. All resolved (sed-replaced suffix, `NOLINTNEXTLINE`-suppressed the deterministic seed with a justification comment, renamed `max32` → `kMax32`). Final tidy build clean.

`win-release` linker hit a one-shot internal compiler error (access violation while linking `crd-sandbox.exe`) on first attempt — historically known LTCG flake, resolved on retry with `ninja: no work to do` confirming all binaries built clean.

`clang-format -i` run on all seven new source files; no output (the files were already conformant before the run; the format pass was a hygiene check).

## What's next

**v1b — `ComponentRegistry` + `IStorageBackend` interface + storage-hint registration grammar** (~200 LOC + tests, ADRs 0050, 0053, 0056).

- `register_component<T>(StorageHint, ...traits)` with the variadic trait grammar (`AsyncAware`, `History{N}`, `SpatialBVH`, `GpuResident`, `Replication`, `Reflection`, `ScriptComponent`).
- `ComponentId` (`u16`) assigned at registration, stable across registration order.
- `IStorageBackend` interface declared (insert / remove / has / get_mut / for_each_chunk / on_entity_destroyed) — implementations land in v1c (Archetype) and v1d (SparseSet).
- Tests: registration round-trip, duplicate-registration rejection, `ComponentId` stability, all reserved traits accepted at compile time.

## Files touched

```
A  engine/scene/CMakeLists.txt
A  engine/scene/include/crd/scene/scene.hpp
A  engine/scene/include/crd/scene/entity.hpp
A  engine/scene/include/crd/scene/slot_map.hpp
A  engine/scene/include/crd/scene/world.hpp
A  engine/scene/src/slot_map.cpp
A  engine/scene/src/world.cpp
A  tests/scene/CMakeLists.txt
A  tests/scene/test_entity.cpp
A  tests/scene/test_slot_map.cpp
A  tests/scene/test_world.cpp
M  CMakeLists.txt                                    (add_subdirectory(engine/scene))
M  tests/CMakeLists.txt                              (add_subdirectory(scene))
M  CONTEXT.md                                        (current focus → v1b; last shipped → v1a)
A  docs/sessions/2026-05-06-scene-v1a-slotmap.md     (this file)
```

## Proposed commit message

```
feat(scene): v1a — EntityId + SlotMap + World shell

Bootstrap crd-scene (Phase 3.0 v1a, ADR-0049). 64-bit generational
EntityId, dense SlotMap with free-list reuse and slot-0 sentinel, and
a World shell with deferred destroy queue + synchronous escape hatch.

22 unit tests / 3448 assertions cover allocate/free round-trip, gener-
ation collisions, free-list LIFO, iterator hole-skipping, deferred
destroy semantics, and double-destroy safety.

Two intentional divergences from ADR-0049 are documented in the session
log: gen-0 reserved as dead-slot value (skip on overflow rather than
trap), and lenient World::destroy_immediate (no-op on stale handle).

Six-config green: win-debug 503/503, win-relwithdebinfo 503/503,
win-release 500/500, win-asan 503/503, win-clang-cl 503/503, win-tidy
clean. 17/17 headless smokes pass on every non-tidy config.
```
