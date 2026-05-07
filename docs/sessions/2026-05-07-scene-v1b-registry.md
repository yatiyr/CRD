# 2026-05-07 — Phase 3.0 v1b: `ComponentRegistry` + `IStorageBackend` interface + storage-hint registration grammar

**Status at start:** Phase 3.0 v1a shipped 2026-05-06. `crd-scene` had `EntityId` / `SlotMap` / `World` (entity identity layer only). No components, no storage, no traits.

**Status at end:** Phase 3.0 v1b SHIPPED. `World::register_component<T>(traits...)` accepts the full ADR-0056 trait grammar. `ComponentRegistry` + `IStorageBackend` interface + `ComponentMask` (256-bit) all live. 22 new unit tests / 131 new assertions. 12 of 14 slices remaining; v1c (`ArchetypeChunkStorage`) is next.

---

## Goal of this session

Land the **registration grammar** that all 14 Phase 3.0 slices ride on. Components register with a `StorageHint` and a variadic list of traits; the registry stores type metadata + lifecycle ops + trait flags; the storage backends (v1c Archetype, v1d SparseSet) and indexes (v1i) consume what was registered.

No backends, no entity-component bindings, no queries, no index dispatch — just the grammar and the metadata table. ~250 LOC production + ~250 LOC tests, in line with the phase-doc estimate.

## What shipped

### New headers (`engine/scene/include/crd/scene/`)

```
component.hpp           ComponentId (u16 wrapper), ComponentMask (256-bit /
                        std::array<u64, 4>), StorageHint, Replication, all trait
                        markers (AsyncAware, SpatialBVH, GpuResident, History,
                        ComponentSerialize stub, Reflection stub),
                        type-erased lifecycle-op function-pointer types,
                        ComponentInfo
component_registry.hpp  ComponentTypeTag<T> (per-T static tag for identity),
                        component_type_key<T>(), trait dispatch overload set
                        (apply_trait), capture_lifecycle_ops<T>(),
                        ComponentRegistry class
storage_backend.hpp     ChunkView, ChunkVisitor, IStorageBackend interface
                        (declared only — backends ship in v1c / v1d)
```

### New source (`engine/scene/src/`)

```
component_registry.cpp  ~10 LOC (constructor only — registry impl is header-only)
```

### Modified

- `world.hpp` — `World` gained `register_component<T>(traits...)`, `component_info`, `component_id<T>`, `registered_component_count`, `components()` registry accessor. Owns one `ComponentRegistry`.
- `world.cpp` — ctor passes the allocator through to `m_components`.
- `scene.hpp` — umbrella picks up the three new headers.
- `tests/scene/CMakeLists.txt` — added `test_component_registry.cpp`.

### Public API (high level)

```cpp
namespace crd::scene {

struct ComponentId       { crd::u16 raw = 0xFFFF; /* is_null, == */ };
struct ComponentMask     { std::array<crd::u64, 4> bits; /* set/test/clear/&|/popcount */ };

enum class StorageHint   : crd::u8 { Archetype = 0, SparseSet };
enum class Replication   : crd::u8 { Local, ServerAuthoritative, ClientPredicted, Remote };

struct AsyncAware  {};
struct SpatialBVH  {};
struct GpuResident {};
struct History     { crd::u8 window = 0; };
struct ComponentSerialize { /* fourcc, version, 4 callbacks */ };
struct Reflection         { /* display_name, fields */ };

struct ComponentInfo { /* id, name, size, alignment, storage_hint, traits, lifecycle ops */ };

class ComponentRegistry {
public:
    template <typename T, typename... Traits>
    ComponentId register_type(Traits&&... traits);   // idempotent, first-call wins

    [[nodiscard]] const ComponentInfo* info(ComponentId) const noexcept;
    template <typename T> [[nodiscard]] ComponentId id_of() const noexcept;
};

class IStorageBackend {
public:
    virtual ~IStorageBackend() = default;
    virtual void  insert(EntityId, ComponentId, const void*) = 0;
    virtual void  remove(EntityId, ComponentId)              = 0;
    virtual bool  has(EntityId, ComponentId) const           = 0;
    virtual void* get_mut(EntityId, ComponentId)             = 0;
    virtual void  for_each_chunk(ComponentMask, ChunkVisitor, void*) = 0;
    virtual void  on_entity_destroyed(EntityId)              = 0;
};

class World {
    /* v1a: spawn / destroy / flush_destroys / is_alive / iterate */
    /* v1b: */
    template <typename T, typename... Traits>
    ComponentId register_component(Traits&&... traits);

    [[nodiscard]] const ComponentInfo* component_info(ComponentId) const noexcept;
    template <typename T> [[nodiscard]] ComponentId component_id() const noexcept;
    [[nodiscard]] crd::u16 registered_component_count() const noexcept;
};

}
```

## Design choices made and why

### Type-identity via per-T static tag, not RTTI

Each `ComponentTypeTag<T>` instantiation has a `static constexpr char value` whose **address** is unique and ODR-deduplicated across translation units. We use `&ComponentTypeTag<T>::value` as the hash-map key (`HashMap<const void*, ComponentId>`).

Considered alternative: `&typeid(T)` — equivalent in current build configs (RTTI is on), but ties registration to RTTI being enabled and adds an `_RTTI` flag dependency to whoever registers. Per-T static tag is the standard EnTT/flecs technique and is friendlier to RTTI-disabled builds we may want for some shipping configurations.

`typeid(T).name()` is still used for `ComponentInfo::name` — debug-only field, RTTI cost there is acceptable. Stored as `StringView` (typeid name has static storage; no allocation).

### `ComponentMask = 256-bit (std::array<u64, 4>)`, not 64-bit

ADR-0053 §8 says "the observed-mask check is a 64-bit bitwise AND." Reading literally would imply `ComponentMask = u64`. But `ComponentId` is u16 (65k), and once v1c's `ArchetypeChunkStorage` keys archetypes by mask, registering the 65th component with a u64 mask **silently truncates** and merges two distinct archetypes — a corruption-class bug.

256-bit mask sized via `kMaxComponents = 256` constant. `kMaxComponents` is enforced by `CRD_ASSERT` inside `register_type`. Four-word AND is one cache line and a few cycles on x64 — indistinguishable from one-word AND at the dispatch granularity (per-chunk-batch, not per-entity). The "64-bit AND" reading was aspirational, not load-bearing.

### Trait grammar via overload set, not enum dispatch

`detail::apply_trait(ComponentInfo&, Trait)` has one overload per trait type. The variadic registration template fold-expands `(apply_trait(info, traits), ...)`. Unknown trait types fail at compile time — the grammar is closed by the type system, no parse step, no enum table.

This shape lets users introduce their own trait struct in their own code and add an `apply_trait` overload (ADL or explicit specialization). For Phase 3.0, the grammar is exactly the eight types listed above; future extensions land transparently.

### Idempotent re-registration, first-call wins

Phase doc said "duplicate-registration rejected." Two readings:
1. Strict: second call asserts / errors.
2. Idempotent: second call returns existing `ComponentId` and ignores trait args.

Picked (2). Reasoning: libraries can register their components defensively on init without coordinating with other libraries. With strict rejection, two libs that both register `Transform` would race and one would die. With idempotency the order of init doesn't matter.

A side effect: the first registration's traits become canonical for the World. The test "Idempotent re-registration returns the same ComponentId" pins this — second-call traits are observably ignored.

### `ChunkView` is reserved-only in v1b

The query layer (v1g) and index dispatcher (v1i) are written against `ChunkVisitor`. v1b declares the visitor signature and the abstract `ChunkView` (entities pointer + count + present mask) but the per-component pointer table is owned by the backends. v1c populates it for archetype chunks; v1d for sparse-set pools.

### `for_each_chunk` carries an opaque `user_data` pointer (ADR-0050 amendment)

ADR-0050 §1 originally declared the storage interface with:

```cpp
virtual void for_each_chunk(ComponentMask required, ChunkVisitor fn) = 0;
```

The shipped interface adds an opaque `void* user_data`:

```cpp
virtual void for_each_chunk(ComponentMask required, ChunkVisitor fn, void* user_data) = 0;
```

Reasoning: `ChunkVisitor` is a C-callback (`void (*)(const ChunkView&, void*)`), not a `std::function`. Without the user-data pointer the visitor cannot capture state without heap-allocating a closure or stuffing context into a static. The user-data pointer keeps the dispatch path heap-free, matching the project's `crd-jobs` SBO and `Counter` patterns.

ADR-0050 §1 is amended in place to reflect the shipped signature.

### `kMaxComponents` overflow uses `CRD_FATAL`, not `CRD_ASSERT`

`ComponentMask` is sized `std::array<u64, 4>` = 256 bits. Registering a 257th component would write past the array on the next `mask.set()` — corruption-class invariant.

`CRD_ASSERT` is a no-op in release builds. `CRD_VERIFY` evaluates the expression for side-effects in release but does not check it. Only `CRD_FATAL` is always-on. So the overflow check is:

```cpp
if (raw_id >= static_cast<crd::u16>(kMaxComponents))
{
    CRD_FATAL("ComponentRegistry: kMaxComponents (256) exceeded");
}
```

A release build registering the 257th component halts at registration with a debugger break + log entry, not silent corruption later.

### Lifecycle ops captured via `if constexpr`

`detail::capture_lifecycle_ops<T>(info)` writes function-pointer slots only when the trait is satisfied:

```cpp
if constexpr (std::is_default_constructible_v<T>) info.default_construct = ...;
if constexpr (std::is_destructible_v<T>)          info.destruct          = ...;
if constexpr (std::is_move_constructible_v<T>)    info.move_construct    = ...;
```

A component that lacks one of these gets a `nullptr` op slot. Storage backends (v1c) check + assert at use time. No `static_assert` requiring all three — tag-only components or move-only components are valid.

## Tests added (`tests/scene/test_component_registry.cpp`, 22 cases / 131 assertions)

- ComponentId default null + equality (1)
- Fresh registry empty; info(null/zero) returns nullptr (1)
- First registration produces non-null id; size/alignment/name round-trip (2)
- Multiple registrations get distinct, monotonic ids 0/1/2 (1)
- Idempotent re-registration: same id, second-call traits ignored (1)
- id_of<T>() null for unregistered T (1)
- Default StorageHint Archetype; explicit SparseSet stored (2)
- All four index trait markers (AsyncAware, SpatialBVH, GpuResident, History{N}) round-trip (2)
- Replication enum stored; default Local (1)
- Reflection stub round-trips display_name (1)
- Lifecycle ops captured for default-constructible types; default_construct + destruct exercised on raw byte buffer (1)
- Tag-only (empty struct) component registers cleanly (1)
- Non-default-constructible type leaves default_construct null while destruct/move are populated (1)
- ComponentMask: default empty + iteration over 32 bits all clear (1)
- ComponentMask: set/test/clear across word boundaries (0/63/64/255) (1)
- ComponentMask: AND/OR over 200+ bits (1)
- IStorageBackend stub: polymorphic dispatch verified through base pointer; `static_assert(std::has_virtual_destructor_v<IStorageBackend>)` (1)
- World::register_component proxy round-trip (1)
- World::register_component idempotent (1)

## Six-configuration sweep

| Config | Build | CTest | Headless smokes |
|---|---|---|---|
| win-debug          | ✅ | 525/525 | 17/17 |
| win-relwithdebinfo | ✅ | 525/525 | 17/17 |
| win-release        | ✅ | 522/522 | 17/17 |
| win-asan           | ✅ | 525/525 (DLL PATH fix applied) | 17/17 |
| win-clang-cl       | ✅ | 525/525 | 17/17 |
| win-tidy           | ✅ | — | — |

Test count delta: +22 (was 503/503 / 500/500 in win-debug / win-release pre-v1b).

`clang-format -i` run on all eight new + modified source files; reformat applied; rebuild + retest verified clean.

## Open items / what next

**v1c — `ArchetypeChunkStorage`** (~600 LOC + tests, ADR-0050). The first storage backend implementation. 16 KB chunks, 64-byte-aligned SoA, archetype graph (memoised lazy build), per-chunk per-component version counters (free `ChangeDetect` foundation for v1i). Entity insert/move/remove crosses archetypes when component sets change.

May further split into v1c1 (chunk allocator + SoA layout) and v1c2 (archetype graph + entity move) if scope grows.

After v1c lands, `World::add_component<T>(entity, value)` and `has_component<T>(entity)` and `get_component<T>(entity)` are wired through the registry → archetype graph → storage chunk. v1d adds the SparseSet escape hatch for high-churn / sparse / lookup-dominated components.

## Files touched

```
A  engine/scene/include/crd/scene/component.hpp
A  engine/scene/include/crd/scene/component_registry.hpp
A  engine/scene/include/crd/scene/storage_backend.hpp
A  engine/scene/src/component_registry.cpp
A  tests/scene/test_component_registry.cpp
M  engine/scene/include/crd/scene/scene.hpp           (umbrella picks up new headers)
M  engine/scene/include/crd/scene/world.hpp           (register_component / component_info / ...)
M  engine/scene/src/world.cpp                         (ctor inits m_components)
M  tests/scene/CMakeLists.txt                         (add test_component_registry.cpp)
M  CONTEXT.md                                         (current focus → v1c; new last-shipped entry)
M  docs/phases/phase-3.0-scene-ecs.md                 (v1b ✅; matrix row added; slice description filled)
M  docs/systems/scene.md                              (status, slice table, public API + usage)
M  docs/systems/README.md                             (crd-scene row → v1b)
A  docs/sessions/2026-05-07-scene-v1b-registry.md     (this file)
```

## Proposed commit message

```
feat(scene): v1b — ComponentRegistry + IStorageBackend + trait grammar

Land the Phase 3.0 v1b registration grammar (ADRs 0050, 0053, 0056).
World::register_component<T>(traits...) variadic template accepts the
full trait set: StorageHint, Replication, AsyncAware, SpatialBVH,
GpuResident, History{N}, ComponentSerialize, Reflection.

Type identity uses a per-T static tag (no RTTI on hot path).
ComponentMask is 256-bit (std::array<u64, 4>) — ADR-0053's "64-bit AND"
reading is amended in this implementation to avoid silent archetype
truncation when registration count exceeds 64. Lifecycle ops captured
via if-constexpr: tag-only and non-default-constructible types are
valid registrations.

Re-registration is idempotent — first-call traits win, second-call
traits ignored. IStorageBackend interface declared only; Archetype
impl lands v1c, SparseSet v1d.

22 unit tests / 131 assertions added. Six-config green: win-debug
525/525, win-relwithdebinfo 525/525, win-release 522/522, win-asan
525/525, win-clang-cl 525/525, win-tidy clean. 17/17 headless smokes
on every non-tidy config.
```
