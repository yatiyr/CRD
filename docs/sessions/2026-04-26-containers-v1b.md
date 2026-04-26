# Session — 2026-04-26 — crd-containers v1b + mini quality pass

## Goal

1. Ship `crd-containers` v1b: `String` (SSO 23-byte), `StringView` alias,
   `RingBuffer<T>` (single-threaded, power-of-two).
2. Mini quality pass: verify Debug + Release + ASan all build clean and
   the test suite is green in each.

Both done.

## What we built / changed

### `String` — `engine/containers/include/crd/containers/string.hpp`

- 24-byte union payload + 8-byte allocator pointer = `sizeof(String) == 32`.
  Two strings per cacheline. ABI sanity asserted with `static_assert`.
- SSO inline buffer holds up to **23 chars** + 1 discriminant byte. The
  discriminant byte is `0..23` in small mode and `0xFF` in heap mode —
  cleverly aligned so the same offset holds the discriminant under either
  union layout, making `sso_state()` a single-byte read + compare.
- Heap mode encodes capacity in the low 56 bits of `cap_and_flag` and
  stamps `0xFF` into the top byte. Capacity stays under `2^56` (way more
  than we'd ever need for a string).
- `const char*` and `std::string_view` ctors are **explicit** to keep
  comparison overload resolution unambiguous: with explicit ctors, only
  the `const char* → string_view` conversion is implicit, so all of
  `String == String`, `String == StringView`, `String == "literal"`
  resolve cleanly via two friend overloads.
- Heterogeneous hashing: `DefaultHash<String>` delegates to
  `DefaultHash<StringView>` so identical bytes hash to identical u64 —
  required for v1c's `HashMap<String, V>::find(StringView)` to work.
- Move-construct copies the 24-byte union payload bytewise (works for
  either active member) and resets the source to an empty SSO so the
  source remains usable.
- Growth: 1.5x with a floor of `kSsoCapacity * 2 = 46` so we never grow
  to a capacity that would shrink back to inline.
- `shrink_to_fit` can demote heap → SSO when contents fit. Useful for
  pools and caches.

### `StringView` — `string_view.hpp`

- Alias to `std::string_view`. We don't write our own.
- `to_view(const String&) noexcept` declared in `string_view.hpp`,
  defined out-of-line in `string.cpp`. Lets headers that only need
  StringView avoid pulling in the full String header.

### `RingBuffer<T>` — `ring_buffer.hpp`

- Header-only template, allocator-aware, fixed power-of-two capacity.
- Mask trick: `(idx + 1) & m_mask` instead of `% capacity`. The
  power-of-two requirement is asserted in the constructor.
- Six fields: `m_alloc`, `m_data`, `m_capacity`, `m_mask`, `m_head`,
  `m_tail`, `m_size`. `m_size` cached so `empty()`/`full()` are O(1)
  branchless byte comparisons.
- `try_push(v)` / `try_emplace(...)` return false when full — no
  automatic overwrite. Overwriting is a *policy* and v1d's RingBufferSink
  wrapper will add it on top.
- `try_pop(out)` move-assigns the slot into `out` and runs `~T()` on the
  vacated slot.
- `snapshot(Array<T>&)` walks `m_size` slots from `m_tail` in
  chronological order and copy-pushes them onto the caller's Array.
- Single-threaded only in v1. SPSC lock-free version comes when the job
  system arrives in Phase 2.
- Move-only (copy explicitly deleted): copying a fixed-size buffer
  silently is rarely what you want.

### Hash header extension — `hash.hpp`

- Added forward-declared `DefaultHash<String>` specialisation. The actual
  `operator()` is defined inline in `string.hpp` (where `String` is
  complete) and delegates to `DefaultHash<StringView>{}(to_view(s))`.

### Umbrella header — `containers.hpp`

- New includes: `string.hpp`, `string_view.hpp`, `ring_buffer.hpp`.
- Force-link anchor block extended: now anchors both `log_channel.cpp`
  and `string.cpp` so MSVC can't strip either out of the static archive.
  The pattern: an extern `int force_link_X() noexcept;` declared in the
  header, defined in the .cpp, referenced from a per-TU
  anonymous-namespace inline const in `containers.hpp`.

### Tests — `tests/containers/test_containers.cpp`

Added 25 new Catch2 tests:

- **String** (16): sizeof == 32, default empty SSO, exactly-23-char
  boundary still SSO, 24-char promotes to heap, `append`-driven promotion,
  `reserve`-driven promotion, c_str NUL-termination in both modes,
  push_back/pop_back, append (StringView/cstr/cstr+n), clear keeps
  capacity, shrink_to_fit returns to SSO, copy ctor, move leaves source
  as empty SSO, comparison operators (incl. heterogeneous), heterogeneous
  hash equality (the v1c prerequisite), `to_view` round-trip.
- **RingBuffer** (8): power-of-two ctor + initial empty state,
  `try_push` fills & refuses, FIFO `try_pop`, wrap-around correctness,
  snapshot order, snapshot after wrap, clear keeps buffer & is reusable,
  move ctor leaves source empty.
- **Lifetime** (1): RingBuffer dtor runs T's dtor on every populated slot.

### Runtime smoke — `runtime/src/main.cpp`

Added a v1b section:

```cpp
String greeting("hello, ");
greeting.append(StringView{"cerid"});
CRD_LOG_INFO(g_log_engine, "String '{}' size={} sso?={}",
             greeting.c_str(), greeting.size(), greeting.is_small());

// Heterogeneous hash sanity
CRD_LOG_INFO(g_log_engine, "hash(String)={:016X} hash(StringView)={:016X}",
             DefaultHash<String>{}(greeting),
             DefaultHash<StringView>{}(StringView{greeting}));

RingBuffer<crd::u32> events(8);
for (crd::u32 i = 0; i < 12; ++i) (void)events.try_push(i);
Array<crd::u32> events_snap;
events.snapshot(events_snap);
```

Output:
```
[INF] [Engine] ... String 'hello, cerid' size=12 sso?=true
[INF] [Engine] ... hash(String)=F321EA3549A8256E hash(StringView)=F321EA3549A8256E
[INF] [Engine] ... RingBuffer<u32>(cap=8): size=8 full?=true accepted=8/12, snapshot.front=0 back=7
```

Confirms: 12-char string fits in SSO, hashes match exactly, ring accepts
first 8 and refuses 4, snapshot is in chronological order.

### Mini quality pass

| Preset | Build | Tests | Result |
| --- | --- | --- | --- |
| `win-debug` | clean | 100/100 | ✅ |
| `win-release` | clean | 99/100 | ✅ (one Debug-only memory-stats test is correctly skipped via `#if defined(CRD_DEBUG)`) |
| `win-asan` | clean | 100/100 | ✅ no leaks, no UAF, no out-of-bounds |

Also visually inspected Release runtime output: `CRD_LOG_TRACE` and
`CRD_LOG_DEBUG` calls are completely absent from the binary (compile-time
stripped via `CRD_LOG_MIN_LEVEL=Info`). MemoryStats correctly reports
all-zero counters in Release because the tracking is `#if defined(CRD_DEBUG)`.

## Plain-English explanation

`String` is what you'll reach for whenever you need owned text — asset
paths, shader uniform names, log messages held in memory, whatever. It's
designed so that the *most common* case (short string, less than 24
chars) doesn't allocate at all — the bytes live inside the String object
itself. Only when you go past 23 chars does it ask its allocator for a
real heap buffer. And like every other container in this module, the
allocator is a constructor argument, not a template parameter, so a
String can live in a frame scratch arena one moment and on the heap the
next without changing any function signatures.

`StringView` is just `std::string_view` — a non-owning (pointer + size)
view. Use it as the parameter type when you don't care whether the caller
gives you a String, a `const char*` literal, or a slice of someone else's
buffer.

`RingBuffer<T>` is a fixed-capacity FIFO queue. You give it a power of
two as the capacity, then you push and pop. When it's full, push refuses;
when it's empty, pop refuses. There's a `snapshot` method that copies the
current contents into an Array in order — that's specifically for things
like the in-game console overlay (later) or log capture, where you want
to *see* the buffer contents without draining them.

The mini quality pass is the first time the engine has been built and
tested in all three flavours (Debug, Release, ASan). Everything is green.
The Release build also visually confirms that compile-time level
stripping is working: TRC and DBG log lines simply do not exist in the
optimized binary.

## Decisions made

- **`String` ctors from `const char*` and `std::string_view` are explicit.**
  Eliminates comparison-overload ambiguity. The cost is `String s = "x";`
  doesn't compile — use `String s("x");` instead.
- **`String` SSO size = 23 chars.** Total struct = 32 bytes. Two per
  cacheline. The discriminant byte is the last byte of the union,
  alignment-arranged to share offset with `cap_and_flag`'s top byte
  (which we always stamp `0xFF` into during heap mode).
- **Capacity grows 1.5x with a `kSsoCapacity * 2` floor.** Prevents
  accidental "grow but still fit in SSO" pathologies.
- **Heterogeneous hash equality is mandatory.** `DefaultHash<String>{}(s)
  == DefaultHash<StringView>{}(StringView{s.data(), s.size()})` for
  every `s` — pinned by a test. Required for v1c HashMap.
- **`RingBuffer::try_push` does not overwrite when full.** Refuses
  instead. Overwrite is a separate policy that wraps RingBuffer; the
  RingBuffer itself stays a pure FIFO.
- **`RingBuffer` is single-threaded in v1.** SPSC lock-free version
  comes with the job system in Phase 2. Premature optimization isn't
  worth chasing yet.
- **Move-only `RingBuffer`.** Copying a fixed-size element buffer
  silently is rarely what callers want. Force them to be explicit.
- **Two `.cpp`s in `crd-containers` now**, both anchored against MSVC's
  dead-code stripping via `force_link_*` helpers and per-TU anchors in
  `containers.hpp`.

## Files touched

- `engine/containers/include/crd/containers/string.hpp` — new (~360 lines)
- `engine/containers/include/crd/containers/string_view.hpp` — new
- `engine/containers/include/crd/containers/ring_buffer.hpp` — new (~225 lines)
- `engine/containers/include/crd/containers/hash.hpp` — added `DefaultHash<String>` forward decl
- `engine/containers/include/crd/containers/containers.hpp` — added v1b includes + extended force-link anchor
- `engine/containers/src/string.cpp` — new (`to_view` defn + force-link helper)
- `tests/containers/test_containers.cpp` — added 25 String/RingBuffer tests
- `runtime/src/main.cpp` — added v1b smoke section
- `docs/containers/CONTAINERS_FILE.md` — full v1b section, updated tests reference, build-flavour matrix
- `docs/systems/containers.md` — String/RingBuffer overview, status v1b ✅
- `CONTEXT.md` — Module Status row updated
- `docs/ROADMAP.md` — v1b ✅, decision-log entry, "Where I left off" → v1c

## Tests / verification

- Debug: ✅ `cmake --build --preset win-debug` clean.
- Debug: ✅ `100/100` Catch2 tests pass.
- Release: ✅ `cmake --build --preset win-release` clean.
- Release: ✅ `99/100` (one Debug-only test correctly skipped).
- Release: visually confirmed compile-time stripping (no TRC/DBG lines).
- ASan: ✅ `cmake --build --preset win-asan` clean.
- ASan: ✅ `100/100`, no leaks, no UAF, no OOB.
- Manual: ran `crd-runtime.exe` in Debug. String SSO check, hash equality
  proof, RingBuffer FIFO + refusal at boundary all visible in output.

## Next session starts with

1. Open `docs/ROADMAP.md`, re-read "Where I left off".
2. Begin **`crd-containers` v1c**: `HashMap<K, V>` (open addressing +
   Robin Hood probing + backshift deletion, no tombstones) and
   `HashSet<K>` (HashMap with empty value).
3. Heterogeneous lookup: `HashMap<String, V>::find(StringView)` /
   `find(const char*)` should work without allocating a temporary
   String. v1b's heterogeneous hash equality test already proves the
   prerequisite.
4. ~25 Catch2 tests: insert/find/erase, collision handling, rehash,
   robin-hood invariant, backshift correctness, iterator skip-empties,
   copy/move, allocator-aware copy, heterogeneous lookup.

End-of-session goal: HashMap + HashSet working, all tests green in
Debug/Release/ASan. Then v1d (log RingBufferSink migration).
