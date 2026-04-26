# Session — 2026-04-26 — crd-containers v1c + v1d (final)

## Goal

Close out `crd-containers` v1 in a single session:

1. **v1c**: Ship `HashMap<K, V>` and `HashSet<K>` (open addressing +
   Robin Hood probing + backshift deletion, no tombstones).
2. **v1d**: Migrate log's `RingBufferSink` storage from `std::vector` to
   `crd::containers::Array<StoredLogRecord>` AND break the
   `crd-log ↔ crd-containers` link cycle.
3. Mini quality pass across Debug / Release / ASan.

All three landed.

## What we built / changed

### v1c — HashMap + HashSet

#### `engine/containers/include/crd/containers/hash_map.hpp` (~430 lines)

Open-addressing hash table with three key design choices:

- **Robin Hood probing.** Each slot tracks its probe distance from its
  ideal home in a parallel `u8 m_dist[]` array. On insert, when we hit
  a "richer" slot (smaller distance), we swap and continue placing the
  displaced entry. Probe distances stay tight; lookups can short-circuit
  when they encounter a slot whose distance < current probe count
  ("if our key were here, it would have evicted them").
- **Backshift deletion.** No tombstones. After erasing a slot, walk
  forward shifting any entry with `distance > 0` one step closer to its
  ideal. Lookups stay fast indefinitely; no rehash needed to clean up.
- **Heterogeneous lookup.** `find`/`contains`/`erase` are templates on
  the query type `Q`. With default `KeyEqual = std::equal_to<>` (the
  transparent C++14 form) and the `DefaultHash<String>` overloads we
  added in this session, `HashMap<String, V>::find(StringView{...})`
  works without allocating a temporary `String`.

Layout: three parallel arrays through one `IAllocator*` —
`K* m_keys`, `V* m_vals`, `u8* m_dist`. `0xFF` in `m_dist` means
"empty"; `0..0xFE` is the live entry's probe distance. Power-of-two
capacity for mask-based modulo. `kMaxLoadFactor = 0.875`.

`emplace_no_grow` stages the new entry on stack-local aligned storage,
then runs the Robin Hood probe loop. Three exit paths:
- **Empty slot** → place there, `++m_size`, return true.
- **Same key + same distance** → duplicate, discard, return false.
- **Richer slot** → swap, continue probing with the displaced entry.

Iterator skips empty slots automatically. We expose `it.key()` and
`it.value()` accessors instead of `std::pair` to avoid forcing a copy
through pair-of-references.

#### `engine/containers/include/crd/containers/hash_set.hpp`

Thin wrapper over `HashMap<K, EmptySetValue>` where `EmptySetValue` is
a 1-byte empty struct. Same probing, same backshift, same heterogeneous
lookup. Iterator yields keys directly (not pairs).

#### Heterogeneous hash extension — `hash.hpp`

`DefaultHash<String>` now has three overloads:

```cpp
u64 operator()(const String& s)        const noexcept;   // bytes via to_view
u64 operator()(std::string_view sv)    const noexcept { return hash_string(sv); }
u64 operator()(const char* cstr)       const noexcept;
```

All three produce identical `u64` for identical bytes. v1b had pinned
that contract for `String <-> StringView`; v1c extends it to `Hash{}(qv)`
with `qv : StringView` resolving to the second overload. So
`m.find(StringView{...})` on `HashMap<String, V>` hashes the view
directly without materialising a temporary String.

#### Tests — 19 new cases

`HashMap` (16): default empty, insert+find round-trip, duplicate-insert
no-op, erase round-trip, `operator[]` insert-default + overwrite, 1000-key
rehash stress, 500-key backshift correctness (erase every third key, all
remaining still findable), `clear` keeps capacity, `reserve` avoids
mid-fill rehash, copy ctor + copy assign, move ctor leaves source empty,
iterator visits each live entry once, heterogeneous String/StringView
lookup, heterogeneous erase, 2000-key insert+erase+reinsert stress, dtor
counting under rehash.

`HashSet` (3): insert/contains/erase + duplicate rejection, iteration
yields keys, heterogeneous contains by StringView.

### v1d — log RingBufferSink migration + cycle break

#### The dependency cycle

Naïve v1d (just swap `std::vector` for `crd::containers::Array` in
`RingBufferSink`) would have made `crd-log` depend on `crd-containers`.
But `crd-containers` already depended on `crd-log` for the
`g_log_containers` channel definition. Two-way cycle = bad time.

#### The fix

Move first-party channel definitions to `crd-log`. New file:
`engine/log/src/log_channels_first_party.cpp`. Contains the actual
`CRD_DEFINE_LOG_CHANNEL(g_log_containers, ...)`. The header
`<crd/containers/log_channel.hpp>` only *declares* the channel via
`CRD_DECLARE_LOG_CHANNEL` (extern) — header-only use of `crd-log`'s
macros doesn't introduce a link-time dependency.

CMake graph after:
- `crd-log → crd-containers` (link)  ✅ one-way
- `crd-containers → crd-log` (link)  ❌ removed

The same pattern (move definitions to crd-log) is now available for any
future "first-party" module-specific channel that needs to avoid a
cycle.

#### Force-link anchor evolution

The anchor block in `containers.hpp` previously kept the channel
registrar in `crd-containers/src/log_channel.cpp` alive. After the
move, the registrar lives in `crd-log/src/log_channels_first_party.cpp`
— but consumers of containers headers still need to anchor it. Added a
new force-link helper `crd::log::force_link_first_party_channels()` and
referenced it from `containers.hpp`'s anchor block.

Also: changed the anchor variable type from `inline const int = func()`
to `inline volatile int = func()`. During implementation, MSVC was
folding the `const int` initialisation away — the function returned a
constant, so the optimizer concluded the call had no observable effect
and emitted no `UNDEF` reference, leaving the linker to strip the TU.
We confirmed this with `dumpbin /symbols`. `volatile` forces a real
load, which forces the call, which forces the symbol reference, which
forces the linker to keep the TU.

#### `RingBufferSink` storage swap

`engine/log/include/crd/log/sinks/ring_buffer_sink.hpp` and `.cpp`:

```diff
-#include <vector>
-std::vector<StoredLogRecord> m_buffer;
+#include <crd/containers/array.hpp>
+crd::containers::Array<StoredLogRecord> m_buffer;
```

Pre-fill with `m_buffer.resize(m_capacity)` in the constructor so we can
index into it like a fixed-size array. Manual `% m_capacity` ring math
stays unchanged because the sink's contract is "overwrite oldest when
full" and `crd::containers::RingBuffer<T>` deliberately refuses on full
(separation of policy from container). Public API of `RingBufferSink`
is unchanged — `snapshot()` still returns `std::vector<StoredLogRecord>`.

Also added `IAllocator*` parameter to the sink's constructor (default
to `default_allocator()`) so callers can route the sink's record
storage through a custom arena later. Existing callers don't need to
change.

### Mini quality pass

| Preset | Build | Tests | Result |
| --- | --- | --- | --- |
| `win-debug` | clean | 119/119 | ✅ |
| `win-release` | clean | 118/118 | ✅ (Debug-only memory-stats test correctly skipped) |
| `win-asan` | clean | 119/119 | ✅ no leaks, no UAF, no OOB |

## Plain-English explanation

`HashMap` is the `std::unordered_map` replacement. You give it keys and
values, it stores them in a flat array (no chained linked lists, so
much friendlier to the cache), and it uses a clever probing strategy
called Robin Hood that keeps lookups consistently fast. Erase doesn't
leave "tombstones" — instead we shift later entries back one slot
toward their natural home, so the table stays compact. The interesting
trick: with a `HashMap<String, u32>`, you can search by `StringView`
directly without first making a temporary String — saves the allocation
that `std::unordered_map` would force on you.

`HashSet` is a HashMap whose values are an empty 1-byte struct. Same
performance, same heterogeneous lookups, exposes a key-only API.

The v1d work is mostly invisible from a user perspective. Internally,
log's `RingBufferSink` (which keeps the most recent N records in
memory for a future debug overlay) used to store its records in a
`std::vector`. Now it uses our own `Array<StoredLogRecord>`. The bigger
deal is the *module dependency cleanup*: previously `crd-log` and
`crd-containers` referenced each other, which CMake tolerates but
causes drama whenever you add new symbols. Now the arrow is one-way:
`crd-log` depends on `crd-containers`, and `crd-containers` only
declares (header-only) what it needs from `crd-log`'s macros. Cleaner
build graph, easier to reason about going forward.

## Decisions made

- **HashMap uses Robin Hood probing + backshift deletion.** No
  tombstones means lookups stay fast over the lifetime of the table.
- **`kMaxLoadFactor = 0.875`.** High because Robin Hood is the right
  algorithm for high load factors; probe variance stays low.
- **Power-of-two capacity** for mask-based modulo.
- **Heterogeneous lookup is mandatory** for the workload we'll have:
  resource caches keyed by `String` but searched by `StringView` /
  `const char*`. No temporary String allocation.
- **`HashSet<K>` is a HashMap wrapper**, not a separate impl. ~5 lines
  of glue vs. ~400 lines of duplicated probing logic.
- **Iterator exposes `key()`/`value()`, not `pair<K, V>&`.** Saves a
  reference-pair object per iteration; reads more naturally.
- **`g_log_containers` definition moved to `crd-log`** to break the
  module dependency cycle. Same pattern applies to any future
  first-party channel that would otherwise need a circular link.
- **Force-link anchors use `volatile int`, not `const int`.** Verified
  by `dumpbin /symbols`: `const int = func()` was foldable, the call
  got elided, the linker stripped the TU. `volatile` forces a real
  read.
- **`RingBufferSink` keeps its overwrite-on-full contract.** Did NOT
  migrate to `crd::containers::RingBuffer<T>` (which refuses on full).
  Storage swap to `Array<T>` was sufficient; layering an "overwrite"
  policy on top of `RingBuffer<T>` would have been more code for no
  win.
- **`RingBufferSink` constructor takes an `IAllocator*`** so callers
  can route record storage through a custom arena. Defaults to
  `default_allocator()` for backwards compatibility.

## Files touched

- `engine/containers/include/crd/containers/hash_map.hpp` — new (~430 lines)
- `engine/containers/include/crd/containers/hash_set.hpp` — new (~95 lines)
- `engine/containers/include/crd/containers/hash.hpp` — added `DefaultHash<String>` heterogeneous overloads for `StringView` and `const char*`
- `engine/containers/include/crd/containers/containers.hpp` — added v1c includes, switched anchor block to `volatile int`, added `force_link_first_party_channels` anchor
- `engine/containers/src/log_channel.cpp` — channel definition removed (now in crd-log); `force_link_log_channel` stub remains as the per-TU anchor
- `engine/log/src/log_channels_first_party.cpp` — new; defines `g_log_containers` and `force_link_first_party_channels`
- `engine/log/include/crd/log/log_channel.hpp` — added `force_link_first_party_channels()` declaration
- `engine/log/include/crd/log/sinks/ring_buffer_sink.hpp` — `std::vector` -> `crd::containers::Array`; ctor now takes `IAllocator*`
- `engine/log/src/sinks/ring_buffer_sink.cpp` — corresponding implementation update; pre-fills via `m_buffer.resize(m_capacity)`
- `engine/log/CMakeLists.txt` — added `crd-containers` to public link deps; documented why
- `tests/containers/test_containers.cpp` — added 19 HashMap/HashSet tests
- `runtime/src/main.cpp` — added HashMap + HashSet smoke section
- `docs/containers/CONTAINERS_FILE.md` — full v1c (HashMap deep-dive) and v1d (cycle-break + RingBufferSink migration) sections, updated test counts
- `docs/systems/containers.md` — status table flipped to v1c/v1d ✅, added HashMap/HashSet sections, updated test counts
- `docs/ROADMAP.md` — containers v1 ✅, Phase 1 next = math, Where I Left Off
- `CONTEXT.md` — Module Status row updated
- `docs/systems/README.md` — containers row updated

## Tests / verification

- Debug: ✅ `cmake --build --preset win-debug` clean.
- Debug: ✅ `119/119` Catch2 tests pass.
- Release: ✅ `cmake --build --preset win-release` clean.
- Release: ✅ `118/118` (Debug-only memory-stats test correctly skipped).
- ASan: ✅ `cmake --build --preset win-asan` clean.
- ASan: ✅ `119/119`, no leaks, no UAF, no OOB.
- Manual: ran `crd-runtime.exe` in Debug. New runtime smoke output:
  - `HashMap<String,u32>: size=3 cap=8 load_factor=0.375 mesh.obj v=3` —
    heterogeneous String→StringView lookup found the entry, no temp String.
  - `HashSet<u32>: size=17 (after 100 inserts mod 17)` — duplicates correctly
    rejected by Robin Hood + KeyEqual.

## Next session starts with

`crd-containers` v1 is **complete**. Phase 1's next module is `crd-math`.

1. Open `docs/ROADMAP.md`, re-read "Where I left off".
2. **Discuss `crd-math` v1 implementation plan** — column-major,
   radians, scalar first. Topics to settle before coding:
   - Vec2/3/4 element layout (anonymous union for `.x/.y/.z` + array
     subscript? union-with-struct vs `T m_data[N]` + accessor methods)
   - SIMD strategy (scalar v1, opt-in SSE/NEON later via `Vec4f_simd`?)
   - Quaternion convention (Hamilton vs JPL, identity = `(0,0,0,1)` or
     `(1,0,0,0)`?)
   - Matrix multiplication direction (column vectors → `Mat * Vec`)
   - Test count target (~30 for v1 vec, ~25 for v2 mat+quat, ~15 for v3
     primitives)
3. After plan agreement, begin **`crd-math` v1**: `Vec2/3/4`,
   `Mat2/3/4` (column-major), basic ops, dot/cross/length/normalize.

Phase 1 remaining after math: `crd-platform` (window/input/timer/fs +
GLFW), then a finishing pass (CI, benchmark, doc sweep). 4-6 sessions
to Phase 1 close.
