# crd-containers

Allocator-aware sequence and lookup containers built on top of `crd-memory`.
What you reach for instead of `std::vector` / `std::unordered_map` / `std::string`.

> Long-form deep-dive: [`docs/containers/CONTAINERS_FILE.md`](../containers/CONTAINERS_FILE.md).
> This file is the "I just need to use it" overview.

## Status

| Sub-version | Ships | Status |
| --- | --- | --- |
| v1a | `Array<T>`, `FixedArray<T, N>`, `Span` aliases, hash defaults | ✅ |
| v1b | `String` (SSO 23-byte), `StringView`, `RingBuffer<T>` | ✅ |
| v1c | `HashMap<K, V>` (Robin Hood + backshift), `HashSet<K>` | ✅ |
| v1d | log `RingBufferSink` migration + module dependency cycle break | ✅ |

**`crd-containers` v1 is complete.** Phase 1 next: `crd-math`.

## What you get today

### `Array<T>` — dynamic, contiguous, allocator-aware

Our `std::vector` replacement, but the allocator is a constructor argument
(an `IAllocator*`) instead of a template parameter. Type stays stable when
the allocator changes — important for the open-world streaming work later.

```cpp
Array<u32> ids;                                 // default-allocator-backed
Array<u32> scratch_ids(&my_linear_allocator);   // backed by a linear arena

ids.push_back(42);            // OOM/exhaustion = CRD_FATAL
if (!scratch_ids.try_push_back(99))             // sub-budget exhaustion-tolerant
{
    // gracefully fall back ...
}

ids.swap_remove(3);           // O(1), order not preserved
ids.erase(3);                 // O(n), order preserved
ids.insert(2, 99);
ids.shrink_to_fit();
```

Growth is 1.5x with an initial capacity of 8 elements. Iterators are raw
pointers — `range-for`, `std::sort`, `std::find`, etc. all "just work".

**Debug freeze guard (D-002 v2).** `arr.freeze()` / `arr.unfreeze()` (re-entrant;
`freeze()` is `const`) bracket a scope during which any *structural* mutation
(push/pop/erase/insert/clear/resize/reserve/shrink/assign/move-from) trips a
`CRD_ASSERT` — element access stays allowed. The canonical use is "many fibers
write disjoint elements of a frozen `Array` during a parallel pass". It's
debug-only (zero size / zero cost in Release) and a development net, not a hard
barrier. `FrozenView<T>` is a move-only RAII wrapper that freeze()s for its
lifetime and exposes `operator[]` / `data()` / iterators:

```cpp
{
    crd::containers::FrozenView<f32> fv(my_array);   // frozen for this scope
    crd::jobs::Counter* c = crd::jobs::parallel_for(static_cast<u32>(fv.size()), k,
        [&fv](u32 b, u32 e){ for (u32 i = b; i < e; ++i) fv[i] = compute(i); });
    crd::jobs::wait(c);
}                                                     // unfrozen here
```

### `FixedArray<T, N>` — stack-only, no allocator

A bounded counterpart for places where the upper bound is known at compile
time (vertex slots, texture bindings, fixed UI panes). No heap allocation,
no allocator field, just `alignas(T) std::byte` storage.

```cpp
FixedArray<const char*, 8> tags;
tags.push_back("renderer");
tags.push_back("physics");
if (!tags.try_push_back("audio")) { /* full */ }
```

### `Span<T>` / `ConstSpan<T>` — non-owning views

Aliases for `std::span<T>` and `std::span<const T>`, plus `as_span` /
`as_const_span` / `make_span` helpers that work with `Array<T>`,
`FixedArray<T, N>`, and raw arrays.

```cpp
void process_meshes(ConstSpan<Mesh> meshes);
process_meshes(as_const_span(my_array));
```

### `hash.hpp` — defaults, ready to be plugged into v1c's `HashMap`

- `hash_u64(x)` — splitmix64 with a non-zero seed mix (so `hash(0) != 0`).
- `fnv1a_64(p, n)` — FNV-1a 64-bit for raw bytes.
- `hash_string(...)` — convenience wrappers.
- `DefaultHash<T>` — specialisations for every integer type, `const char*`,
  `std::string_view`, and pointers; falls back to `std::hash<T>` for
  user-defined types.
- `DefaultHash<String>` (v1b) — delegates to `DefaultHash<StringView>` so
  String and StringView with identical bytes hash to identical u64s.

### `String` — allocator-aware, SSO inside

24-byte payload + 8-byte allocator pointer = **`sizeof(String) == 32`**.
Strings up to 23 chars live entirely inline; longer strings spill onto
the heap via the carried `IAllocator*`.

```cpp
String s;                              // empty, default allocator
String hello("hello");                 // 5 chars, stays inline
String big(StringView{"this lives on the heap because it is long"});
hello.append(", world");               // grows; promotes to heap if needed
hello == "hello, world";               // true, no allocation
```

Ctors from `const char*` and `StringView` are **explicit** on purpose —
this keeps `s == "literal"` overload resolution unambiguous. Writing
`String s = "x";` won't compile; use `String s("x");` (or
`String s{"x"};`) instead.

`StringView` is `std::string_view`. Two strings per cacheline; comparison
goes through StringView, so all of `String == String`,
`String == StringView`, and `String == "literal"` resolve cleanly.

### `RingBuffer<T>` — fixed-capacity FIFO

Power-of-two capacity, single-threaded, mask-based indexing.
`try_push` returns false when full (no automatic overwrite — that's a
wrapper's job). `snapshot(Array<T>&)` appends a chronological copy for
debug overlays.

```cpp
RingBuffer<u32> events(8);            // capacity must be power of two
for (u32 i = 0; i < 12; ++i) (void)events.try_push(i);  // 8 accepted, 4 refused

Array<u32> snap;
events.snapshot(snap);                // chronological order
```

### `HashMap<K, V>` — open-addressing hash table

Open addressing + Robin Hood probing + backshift deletion (no
tombstones). Power-of-two capacity, max load factor `0.875`.

```cpp
HashMap<String, u32> asset_versions;
asset_versions.insert(String("mesh.obj"), 3);
asset_versions.insert(String("shader.frag"), 7);

// Heterogeneous lookup: no temporary String allocated.
auto* p = asset_versions.find(StringView{"mesh.obj"});

// Default-init insert via operator[].
asset_versions["new.png"] = 1;

// Iteration: skips empty slots automatically; key()/value() instead of pair.
for (auto it = asset_versions.begin(); it != asset_versions.end(); ++it) {
    process(it.key(), it.value());
}
```

Heterogeneous lookup means `find`/`contains`/`erase` accept any key
type the hasher and equality predicate accept. With `HashMap<String,
V>` and the default hasher, you can search by `StringView` or
`const char*` without allocating a temporary String — the
`DefaultHash<String>` specialisation has overloads producing the
same `u64` for the same bytes.

### `HashSet<K>` — set of unique keys

Thin wrapper over `HashMap<K, EmptySetValue>`. Same probing, same
backshift, same heterogeneous lookup. Iterator yields keys directly
(not pairs).

```cpp
HashSet<String> seen;
seen.insert(String("alpha"));
seen.insert(String("beta"));
if (seen.contains(StringView{"alpha"})) { ... }   // heterogeneous

for (const String& key : seen) { ... }
```

### Concurrent containers — `SpscQueue` / `ConcurrentQueue` / `AtomicArray`

All `IAllocator*`-backed, fixed-capacity, lock-free. Use only when the access
pattern genuinely needs it — a serial reduction or `jobs::parallel_reduce`
beats shared mutable state when you can use it (see `docs/systems/scene-concurrency.md`
for the "match the structure to the access pattern" rationale).

- **`SpscQueue<T>`** — single-producer / single-consumer FIFO. `try_push` /
  `try_pop` (non-blocking; false when full/empty). Cache-line-split head/tail.
  Supports non-trivially-copyable `T`.
- **`ConcurrentQueue<T>`** (D-002 v3) — bounded MPMC FIFO (Vyukov 1024cores
  algorithm; the public, allocator-aware form of what was the `crd-jobs`
  scheduler's internal injection queue). `try_push` / `try_emplace` / `try_pop`,
  power-of-two capacity, zero post-construction allocation. `T` must be trivially
  copyable.
- **`AtomicArray<T>`** (D-002 v4) — bounded lock-free *append-only* vector. A
  producer claims the next slot with one `m_head.fetch_add(1)` and constructs
  there; slots never recycled or moved (addresses stable). `push`/`emplace`
  return the index or `npos` on overflow (a sizing bug). Reads are valid after
  the parallel pass joins; `clear()` reuses it between passes. The canonical use
  is "collect ≤N results from a parallel pass". Supports non-trivial `T`.
- **`CacheLinePadded<T>`** (D-002 v4) — `alignas(64)` one-element-per-cache-line
  wrapper. For an array of atomic counters many fibers `fetch_add`, the pattern
  is `Array<CacheLinePadded<u32>>` + `std::atomic_ref<u32>(slot.value)` (the
  element stays trivially copyable so `Array` works *and* can grow; cache-line
  separated so adjacent RMWs don't false-share). Do **not** wrap `std::atomic<T>`
  and put it in `Array` — `std::atomic` isn't movable and `Array` relocates on
  growth.

## v1d — log RingBufferSink migration + cycle break

Two cleanup tasks landed together:

- **Module dependency cycle broken.** `crd-log` now depends on
  `crd-containers` (one-way). The `g_log_containers` channel is
  *declared* in `<crd/containers/log_channel.hpp>` but *defined* in
  `engine/log/src/log_channels_first_party.cpp`. `crd-containers` has
  no link-time dependency on `crd-log` any more.
- **Log's `RingBufferSink` storage** migrated from `std::vector` to
  `crd::containers::Array<StoredLogRecord>`. Same external API
  (`snapshot()` still returns `std::vector`), same overwrite-on-full
  behaviour. Engine code is now consistently using its own
  containers everywhere it can.

## Naming choice

We use `Array`, `HashMap`, `String`, `RingBuffer`, `FixedArray` rather than
`Vector`, `Map`, etc. Rationale: distinct from STL terminology, no
ambiguity with RB-tree-based `Map`, and consistent with EA STL / Bitsquid
style.

## Allocator pattern

Every container takes an `IAllocator*` as a **constructor argument**:

```cpp
template<typename T>
class Array {
public:
    explicit Array(IAllocator* alloc = default_allocator());
    // ...
private:
    IAllocator* m_alloc;
    // ...
};
```

This is intentional — the `std::vector<T, Alloc>` template-allocator
pattern bakes the allocator into the type, which makes streaming /
arena-swapping painful. Our way: an `Array<Mesh>` is one type forever; you
just hand it a different `IAllocator*` when you build it.

## Two push APIs: when to use which

- **`push_back(v)`** — the boring, default API. Triggers `CRD_FATAL` on
  OOM through the underlying allocator. Use this when you're backed by
  the heap (`MallocAllocator`) and OOM means "the game is dead anyway".
- **`try_push_back(v)`** — returns `bool`. Use this when you're backed by
  a sub-budget allocator (`LinearAllocator`, `StackAllocator`, or some
  caller-bounded arena) and you want to *gracefully* refuse the
  allocation rather than crash.

## What's deliberately NOT here

`Vector` (= `Array`), linked list, `std::stack` adapter, `std::map`-style
RB-tree, `PriorityQueue`, generic `Tree`, generic `Graph`, `SmallVector`,
`IntrusiveList`, `Optional`, `Variant`, `Pair`, `Bitset`, `SortedArray`.
Reasons:

- Aliases or trivial wrappers around `Array` add nothing.
- Cache-hostile structures (linked list, RB-tree) aren't useful in a
  game engine.
- Generic `Tree<T>` / `Graph<T>` are wrong abstractions — scene graphs,
  render graphs, BVHs, behavior trees, dialogue graphs are all
  *specific* structures with their own invariants. Each will live in
  its own subsystem.
- `std::optional`, `std::variant`, `std::pair`, `std::bitset` already exist
  and are fine. We use them.

## Dependencies

- `crd-core` (types, asserts)
- `crd-log` (channel for capacity warnings)
- `crd-memory` (allocators)

## Tests

`tests/containers/test_containers.cpp` — 76 Catch2 tests covering hash
math, Span helpers, Array (push/emplace/erase/swap_remove/insert/resize/
shrink/copy/move/iterators/`<algorithm>` interop/sub-budget exhaustion/
dtor counting), FixedArray (full semantics, copy/move, range-for, Span
interop), String (SSO boundary at 23, heap promotion, append, comparisons,
heterogeneous hash), RingBuffer (push/pop FIFO, wrap-around, snapshot,
move ctor, dtors), HashMap (insert/find/erase round-trip, duplicates,
operator[], rehash + 1000-key stress, backshift correctness, copy/move,
iterator skip-empties, heterogeneous String/StringView lookup, dtors),
HashSet (insert/contains/erase, iteration, heterogeneous contains), and
channel registration.

Engine total — verified across all three build flavours:

| Preset | Tests | Notes |
| --- | --- | --- |
| `win-debug` | 119/119 | full suite |
| `win-release` | 118/118 | one Debug-only memory-stats test correctly skipped |
| `win-asan` | 119/119 | no leaks, no UAF, no out-of-bounds |
