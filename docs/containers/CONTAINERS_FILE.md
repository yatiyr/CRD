# crd-containers — A Walkthrough In Plain Words

> Companion to `docs/log/LOG_FILE.md` and `docs/memory/MEMORY_FILE.md`.
> Same spirit: this is for *you* (and future-you), not a manual for
> outsiders. It explains *why* every piece exists and *how* the insides
> actually work, without pretending things are simpler than they are.
>
> This file documents v1a: `Array<T>`, `FixedArray<T, N>`, `Span` aliases,
> and the `hash.hpp` defaults. v1b will append `String` + `RingBuffer`,
> v1c will append `HashMap` + `HashSet`, and v1d will move log's
> RingBufferSink onto our own RingBuffer.

---

## 1. The 30-second mental model

The engine has its own containers because the standard library's are:

1. **Wrong allocator pattern.** `std::vector<T, Alloc>` makes the
   allocator part of the type. Switching from a heap to a streaming
   allocator changes the type of every container in your program. We
   don't want that.
2. **Throw-happy.** STL throws `bad_alloc` on OOM. We use `CRD_FATAL`
   (heap) or return `nullptr` / `false` (sub-budget).
3. **Cache-hostile by default.** `std::list`, `std::map`,
   `std::unordered_map` (chaining) all chase pointers. We want
   contiguous memory wherever possible.

Our v1a answer: a tiny set of correct, predictable containers, each
holding an `IAllocator*` as a constructor argument so swapping
allocators is a runtime concern, not a type concern.

```cpp
Array<Mesh>     meshes(default_allocator());          // heap
Array<Mesh>     frame_meshes(&frame_linear_alloc);    // bump arena
Array<Mesh>     world_meshes(&streaming_alloc);       // (Phase 2) virtual mem
```

All three are the **same type**. The function signatures that take an
`Array<Mesh>` don't change as we go from boring to fancy.

---

## 2. The pieces (v1a)

### 2.1 `alignment` recap

We don't redefine alignment helpers — `crd-memory` already exposes
`align_up`, `is_pow2`, `kDefaultAlignment`. Containers use those when
they need them.

### 2.2 `hash.hpp` — the default hashers

Three building blocks:

```cpp
constexpr u64 hash_u64(u64 x) noexcept;     // splitmix64 + non-zero seed
constexpr u64 fnv1a_64(const void* data, usize n) noexcept;
inline    u64 hash_string(std::string_view sv) noexcept;
```

**Why splitmix64?** Identity-hashing integers (just returning the integer
as the hash) is a known performance footgun with open-addressing tables —
sequential keys collide on probe paths. splitmix64 is a tiny, fast,
well-distributed bit mixer that's the de-facto standard for "I have a
u64, give me a hash".

**Why the non-zero seed?** The vanilla splitmix64 finalizer has 0 as a
fixed point: `hash(0) == 0`. That's correctness-preserving (slot 0 is a
valid slot) but ugly. We XOR a golden-ratio constant first so 0 doesn't
map to 0. Costs one XOR.

**Why FNV-1a?** Simple, header-only, stable, no dependencies. Not the
absolute fastest hash on Earth — `xxhash`/`wyhash` would beat it — but
"good enough until profiling tells me otherwise" is the right rule. The
function is `constexpr` so compile-time string hashes for static
dispatch are free.

**Default dispatch.** `DefaultHash<T>` is the entry point that
`HashMap<K, V>` (v1c) will use:

| `T` | Implementation |
| --- | --- |
| any integer (`u8`/`u16`/`u32`/`u64`/`i8`...`i64`) | `hash_u64` |
| `const char*`, `std::string_view` | `hash_string` (FNV-1a) |
| `T*` (any pointer) | `hash_u64(reinterpret_cast<u64>(p))` |
| anything else | falls back to `std::hash<T>` |

### 2.3 `span.hpp` — non-owning views

Just an alias around `std::span`:

```cpp
template<typename T, std::size_t Extent = std::dynamic_extent>
using Span      = std::span<T, Extent>;
template<typename T>
using ConstSpan = std::span<const T>;
```

…plus three helpers:

```cpp
make_span(p, n);                     // raw pointer + size
make_span(c_array);                  // T (&)[N]
as_span(container);                  // Array, FixedArray, anything with .data()/.size()
as_const_span(container);            // const-correct version
```

Why alias instead of write our own? Because `std::span` is exactly the
right shape, well-tested, and any future toolchain bug fix lands for
free. The wrapper buys us namespace consistency (`crd::containers::Span`)
and the `as_span` helpers, that's all.

### 2.4 `Array<T>` — the workhorse

The public layout looks like this:

```cpp
template<typename T>
class Array
{
public:
    explicit Array(IAllocator* = default_allocator());
    Array(usize initial_capacity, IAllocator* = default_allocator());
    Array(std::initializer_list<T>, IAllocator* = default_allocator());
    // copy / move ctors + assignments

    T&  operator[](usize);
    T&  front();
    T&  back();
    T*  data();
    usize size();
    usize capacity();

    void  reserve(usize);                  // OOM = fatal
    bool  try_reserve(usize);              // bool result
    void  shrink_to_fit();
    void  resize(usize, const T& fill = ...);

    void  clear();
    void  push_back(const T&);             // OOM = fatal
    bool  try_push_back(const T&);         // bool result
    template<typename... A> T& emplace_back(A&&...);
    void  pop_back();

    void  erase(usize i);                  // O(n), preserves order
    void  swap_remove(usize i);            // O(1), no order
    void  insert(usize i, const T& v);     // O(n)

    T*    begin(); T* end();               // raw pointer iterators

private:
    IAllocator*  m_alloc;
    T*           m_data;
    usize        m_size;
    usize        m_capacity;
};
```

**Memory layout:** four pointer-sized fields = 32 bytes on 64-bit.
That's 8 bytes more than `std::vector` (which doesn't carry an allocator
pointer) but that 8 bytes is exactly what buys us the
"swap allocators without changing the type" property.

**Growth strategy:** 1.5x with a minimum initial capacity of 8 elements.
Folly and EA STL both picked 1.5x over 2x because it lets earlier
allocations be reused (the new buffer is small enough that the old one
might fit the new requirement after a few growths, leading to less
fragmentation in long-running programs). 2x amortises slightly better in
the worst case but at the cost of more peak memory.

**Push APIs split:**

- `push_back(v)`: the default. If we run out of capacity, grow. If the
  allocator can't grow (heap exhausted), the allocator itself triggers
  `CRD_FATAL` and the process dies. Standard.
- `try_push_back(v) -> bool`: returns false instead of fatal-ing if the
  allocator returns null. Useful when the allocator is a sub-budget
  (`LinearAllocator(256)` for example) and we want to gracefully stop
  pushing rather than crash. Sub-budget allocators return `nullptr`
  on exhaustion (by design — see `MEMORY_FILE.md` §2.3).

**`swap_remove` vs `erase`:** game engines very rarely need ordered
deletion mid-array. Entity lists, draw call buckets, free lists — all
unordered. `swap_remove(i)` is `m_data[i] = std::move(m_data[--m_size]);`
plus the dtor — O(1). `erase(i)` is the std::vector behaviour — O(n)
shift. Both are provided; pick the right one.

**`insert(i, v)` is on the slow side.** It exists because sometimes you
need order, but if you find yourself calling it in a hot path, you've
likely chosen the wrong data structure.

**Relocation strategy:** when growing, we either `memcpy` (when
`std::is_trivially_copyable_v<T>`) or move-construct + destroy each
element. The constexpr-if branch keeps trivial types (most of what an
engine pushes around) at memcpy speed.

**Iterators are raw pointers.** `T*` and `const T*`. That makes
`std::sort(a.begin(), a.end())` work, `std::find` work, range-for work.
No custom iterator class to maintain.

**Copy vs move:**

- Copy ctor uses RHS's allocator unless an explicit one is passed.
  Useful for cross-arena copies: `Array<Mesh> backup(my_array, &heap);`
- Move ctor takes the allocator pointer + buffer + size + capacity.
  RHS becomes empty (allocator preserved so the moved-from object can
  still be reused).

### 2.5 `FixedArray<T, N>` — bounded, allocator-free

A counterpart for "I know the upper bound at compile time":

```cpp
template<typename T, usize N>
class FixedArray
{
    static_assert(N > 0);
    // ...same interface as Array, minus capacity-changing operations
private:
    alignas(T) std::byte m_storage[sizeof(T) * N];
    usize m_size;
};
```

**Memory:** `sizeof(T) * N + sizeof(usize)` plus alignment padding. No
heap pointer, no allocator. Lives wherever you put it — on the stack,
inside a struct, inside another container.

**Use cases the engine will hit:**

- 8 vertex stream slots, 16 texture bindings, 64 light sources per
  cluster — bounded by the API or by design.
- Encoding intermediate command lists where the upper count is known.
- Any "scratch container" inside a function scope where the typical
  size is small.

**`try_push_back` returns false** when we're full, because there's no
"grow path" that would succeed. `push_back` asserts.

**No iterators trickery here either.** `T*` and `const T*` again.
`alignas(T) std::byte` storage + `reinterpret_cast<T*>(...)` is the
standard "uninitialised aligned bytes" pattern; placement-new in,
`p->~T()` out.

### 2.6 `g_log_containers` — the channel

A regular log channel registered at startup:

```cpp
CRD_DEFINE_LOG_CHANNEL(g_log_containers, "Containers", LogLevel::Info)
```

Used (sparingly) for capacity warnings. Subsystems calling container
routines should use their own channel for normal messages.

---

## 3. The dead-code-stripping problem

Worth flagging because it bit us during this session:

Our static library `crd-containers.lib` contains exactly *one* `.cpp`
file (`log_channel.cpp`) — every other piece is a header-only template.
When the test executable links against `crd-containers.lib` but doesn't
reference any *symbol* defined in `log_channel.cpp`, MSVC's linker
strips the entire `.obj` from the archive as dead code. That kills the
static initializer that registers the channel.

The fix: in `containers.hpp`, an anonymous-namespace static variable
calls a tiny `force_link_log_channel()` function defined in
`log_channel.cpp`. That single reference forces the linker to keep the
whole `.obj`, which keeps the registrar alive.

```cpp
// log_channel.hpp
int force_link_log_channel() noexcept;

// containers.hpp
namespace crd::containers::detail {
    namespace {
        [[maybe_unused]] inline const int g_log_channel_anchor =
            force_link_log_channel();
    }
}

// log_channel.cpp
int force_link_log_channel() noexcept { return 0; }
```

Same trick will apply to v1b/v1c when they introduce module-private
state (e.g., a default `KeyEqual` policy) that no caller references
directly.

---

## 4. Why these choices

### 4.1 Constructor-arg allocator vs template parameter

This is the single most important decision in `crd-containers`. Long
version is in `MEMORY_FILE.md` §5.1. Short version: with a template
parameter, the allocator is part of the type, which means a function
that takes `Array<Mesh>` will only accept arrays of the *one* allocator
type. With a constructor argument, `Array<Mesh>` is one type forever
and the allocator becomes runtime data — which is what you need for
streaming (different chunks live in different arenas).

### 4.2 1.5x growth, initial 8

- 1.5x makes earlier blocks reusable for later allocations (Folly/EA
  observation).
- 8 as the floor keeps the worst-case "push 5 things" scenario from
  doing 4 reallocations.
- Power-of-two growth would be fine too; 1.5x is just slightly nicer
  for memory pressure.

### 4.3 Two push APIs

The single "always-fatal" API was tempting (one less function), but it
makes sub-budget allocator usage painful. With a tiny `LinearAllocator`
backing an `Array`, you genuinely *want* to know when it's full so you
can flush, reset, or fall back. So we have both.

### 4.4 No exceptions

Engine-wide convention. OOM is `CRD_FATAL`. Errors are bool returns.

### 4.5 Iterators = raw pointers

Cheapest possible. `<algorithm>` works. Range-for works. The downside
is iterator validity is harder to track — but that's true of std too,
and our use cases are mostly "build the container, iterate, throw it
away".

### 4.6 `std::span` instead of our own

There's no upside to writing our own `Span<T>` on top of `T*` + `usize`
when `std::span` does that already. Aliasing is the right answer.

### 4.7 splitmix64 + golden-ratio seed

splitmix64 alone has `hash(0) == 0`. Real hash tables work fine with it
(slot 0 is a valid slot), but it's an obvious weirdness. XORing the
golden-ratio constant first removes the fixed point at zero with a
single instruction. Cheap, principled, done.

### 4.8 No `Vector` alias for `Array`

Would save one keystroke and add nothing. Two names for the same thing
is just future confusion.

### 4.9 No linked list, no RB-tree map, no generic Tree/Graph

- Linked lists are cache-hostile. Game engines that use them (rare) use
  intrusive ones, which we'll add when something actually needs one.
- RB-tree maps lose to hash maps on every game-engine workload I've ever
  seen. If you need ordered iteration, sort an Array.
- "Tree" and "Graph" aren't single data structures — scene graphs,
  render graphs, BVHs, behavior trees, dialogue graphs are *each*
  specific structures with specific invariants. Generic templates would
  be the wrong abstraction. They'll appear as named types in their
  respective subsystems.

---

## 5. The path of one allocation

```cpp
Array<u32> ids;                   // ctor: m_alloc = default_allocator(), rest 0
ids.push_back(42);
```

Step by step:

1. `push_back` checks `m_size == m_capacity` → yes (both 0).
2. Calls `grow_to_at_least(1)`.
3. `next_capacity(1)` returns 8 (initial capacity floor).
4. Calls `m_alloc->allocate(sizeof(u32) * 8, alignof(u32))`.
5. `MallocAllocator::allocate` → `_aligned_malloc(32, 4)` → returns
   pointer.
6. `relocate(new_data, m_data /*nullptr*/, 0)` — nothing to copy.
7. `free_buffer()` is a no-op (m_data was nullptr).
8. `m_data = new_data; m_capacity = 8;`
9. Placement-new constructs `u32(42)` at `m_data[0]`.
10. `m_size = 1`.

Subsequent `push_back`s up to index 7 just placement-new + bump
`m_size`. Push #9 triggers another grow: `next_capacity(9) = 8 + 8/2 = 12`.

Back-of-the-envelope: pushing 1000 trivial `u32`s does about
`log_1.5(1000/8) ≈ 12` reallocations, each a `memcpy`. That's the worst
case; in practice you `reserve()` once and skip all of it.

---

## 6. Common mistakes the engine catches

| Mistake | What happens |
| --- | --- |
| `a[i]` with `i >= a.size()` | `CRD_ASSERT` in `operator[]` |
| `a.front()` on empty array | `CRD_ASSERT(m_size > 0)` |
| `a.pop_back()` on empty | `CRD_ASSERT(m_size > 0)` |
| `a.swap_remove(i)` with `i >= size` | `CRD_ASSERT` |
| `FixedArray::push_back` past `N` | `CRD_ASSERT(m_size < N)` |
| Iterator stored across `push_back` that grew | (no automatic catch — same as std::vector; ASan helps) |
| `try_push_back` ignored | `[[nodiscard]]` warns |

---

## 7. The example output

Running `crd-runtime.exe` after this session, the new container smoke
section prints:

```
[INF] [Engine] ... Array<u32>: size=10 capacity=12 front=0 back=81
[INF] [Engine] ... FixedArray<const char*, 4>: size=3/4 full?=false
[INF] [Engine] ... Linear-backed Array: pushed 18 u32s before exhaustion (scratch used=152/256)
[INF] [Engine] ... hash_u64(42) = 0xBDD732262FEB6E95, fnv1a('cerid') = 0x70D43D5E3AC82D20
```

Things to notice:

- **Array grew from 8 → 12** after the 9th push, exactly as the 1.5x
  policy says.
- **Linear-backed Array stopped at 18 u32s**: 256-byte scratch arena,
  Array doing 1.5x growth = a sequence of `8*4`, `12*4`, `18*4`, `27*4`
  byte allocations. The 27-element grow needs 108 bytes; we already
  have 152 bytes of older buffers held by our (cumulative-tracking)
  linear allocator, so there's not enough room → `try_push_back`
  returns false → we stop. The memory channel even logs the exhaustion:
  `ContainerScratch exhausted (requested 108 + 0 pad, have 104 of 256)`.
- **`hash_u64(42)` is non-zero and well-mixed.** The seed XOR is doing
  its job.

---

## 8. v1b — `String`, `StringView`, `RingBuffer<T>`

### 8.1 `String` — SSO with allocator-aware heap fallback

`String` is our `std::string` replacement. Same shape as the rest of the
module: an `IAllocator*` constructor argument, no exceptions, no template
allocator parameter.

```cpp
String s;                              // empty, default allocator
String hello("hello");                 // small, lives entirely inline
String wide(std::string_view{"a string longer than 23 chars goes to heap"});
hello.append(", world");               // grows; promotes to heap if needed
hello == "hello, world";               // true
hello == StringView{"hello, world"};   // also true
```

#### Layout — exactly 32 bytes

```
┌─────────────────────────────────────────────────────────┬──────────────┐
│   union {                                               │              │
│     struct { char buf[23]; u8 size_or_flag; } m_small;  │  IAllocator* │
│     struct { char* data; usize size; usize cap_and_flag;}│  m_alloc    │
│   };                              (24 bytes)            │  (8 bytes)   │
├─────────────────────────────────────────────────────────┼──────────────┤
│                       sizeof(String) = 32                              │
└────────────────────────────────────────────────────────────────────────┘
```

Two strings per 64-byte cacheline. The allocator pointer is per-string so
every String can live in a different arena (frame scratch, asset arena,
heap…) without code changes upstream.

#### SSO discriminant

The last byte of the union (offset 23) is the discriminant:

- **Small mode**: `m_small.size_or_flag` holds the live size (`0..23`).
- **Heap mode**: `m_heap.cap_and_flag`'s top byte is `0xFF`. Because we
  set the capacity in the low 56 bits and stamp `0xFF` into the top byte
  via `set_heap_capacity()`, the *same byte address* (offset 23) reads
  `0xFF` regardless of whether we're approaching it through the small
  struct view or the heap struct view.

`sso_state()` is a single byte read + compare against `0xFF`. No branchy
size logic, no extra fields.

#### `const char*` and `std::string_view` ctors are `explicit`

This is the critical decision that makes comparison overload resolution
unambiguous. If `String(const char*)` were implicit:

```cpp
String s("hello");
s == "hello";                          // 'const char*' could become String
                                       // OR std::string_view — ambiguous!
```

By forcing direct-initialization (`String s("x")`), the compiler can only
walk the `const char* → string_view` path during comparison. Five
operator overloads then cover everything cleanly:

| Expression | Resolved by |
| --- | --- |
| `s == s2` (String == String) | `friend operator==(String, String)` |
| `s == sv` (String == StringView) | `friend operator==(String, string_view)` |
| `s == "literal"` | same (`const char*` → `string_view`) |
| `sv == s` | rewritten by C++20 |
| `"literal" == s` | rewritten by C++20 |

#### Growth strategy

When `push_back`/`append`/`reserve` outgrows the current capacity:

1. Call `next_capacity(target)` — `current * 1.5`, but never less than
   `kSsoCapacity * 2` (so we don't bounce back to SSO accidentally).
2. Allocate `new_cap_chars + 1` bytes (the `+1` is for the trailing
   `'\0'`, kept so `c_str()` is O(1)).
3. `memcpy` existing contents over.
4. Free old heap buffer (if any).
5. Stamp the heap discriminant via `set_heap_capacity`.

#### `shrink_to_fit` can return to SSO

If you `clear()` a long heap-mode String and then call `shrink_to_fit()`,
the storage demotes back to inline. We allocate a stack-local copy, free
the heap, and write the SSO discriminant. Useful for caches and pools
that want to keep String objects around without paying for their
last-large-content's heap.

#### Heterogeneous hashing

`DefaultHash<String>` and `DefaultHash<StringView>` produce **identical
u64s for identical bytes**:

```cpp
String s("the quick brown fox");
StringView sv{"the quick brown fox"};
DefaultHash<String>{}(s) == DefaultHash<StringView>{}(sv);   // true
```

This pin is mandatory: when v1c's `HashMap<String, V>::find(StringView)`
arrives, it must hash the StringView and compare the resulting bucket
against a stored String, which only works if the hashes agree.
The implementation is one line — `DefaultHash<String>` delegates to
`DefaultHash<StringView>` after building a view through `to_view(s)`.

#### Move semantics

Move-construct copies the 24-byte union payload bytewise (works for
either layout) and zeroes the source's discriminant — i.e. source becomes
an *empty SSO String* with allocator preserved. So a moved-from String is
still usable as an empty string; you can keep pushing to it. Move-assign
frees any owned heap buffer first, then does the same.

### 8.2 `StringView` — alias

```cpp
using StringView = std::string_view;

StringView to_view(const String& s) noexcept;     // out-of-line in string.cpp
```

`to_view` is provided as a non-inline function so callers can include
`<crd/containers/string_view.hpp>` without dragging in `<string.hpp>`'s
implementation. The implicit conversion operator on String is also there
for natural call sites.

### 8.3 `RingBuffer<T>` — fixed-capacity FIFO

```cpp
RingBuffer<u32> events(8);             // power-of-two required
events.try_push(42);
u32 out;
events.try_pop(out);
```

Internal state is six fields:

```
m_data     : pointer to capacity slots (raw uninitialised storage)
m_capacity : power of two
m_mask     : capacity - 1     (for cheap modulo)
m_head     : next write index
m_tail     : next read index
m_size     : current count    (cheap empty/full checks)
```

The mask trick: `(index + 1) & m_mask` instead of `% capacity`. Costs
nothing if capacity is a power of two — and we assert that at
construction.

#### `try_push` / `try_pop` return false instead of overwriting

Deliberate choice. Overwriting is a *policy* — log's `RingBufferSink`
wants overwrite, but a job queue or audio buffer wants explicit refusal.
Rather than build the policy into the container, we return false and let
the caller decide. The eventual `RingBufferSink` migration in v1d will
wrap our RingBuffer with an "overwrite when full" helper.

#### `snapshot(Array<T>& out)`

Walks `m_size` slots in chronological order (starting from `m_tail`) and
appends them to the caller's Array. Used for debug overlays — you don't
want to drain the ring just to display its contents.

#### Single-threaded only in v1

A real SPSC lock-free ring (head/tail as atomics, padded apart) becomes
useful when the job system arrives in Phase 2. v1's mutex-free, single-
threaded version is correct, simple, and good enough for log's
RingBufferSink and for any single-thread debug capture buffer.

### 8.4 The dead-code-stripping anchor, extended

We now have two `.cpp` files in `crd-containers`: `log_channel.cpp` and
`string.cpp`. Both need to survive MSVC's dead-code stripping. The
umbrella header now declares two anchors:

```cpp
namespace crd::containers
{
    int force_link_log_channel() noexcept;
    int force_link_string() noexcept;
}

namespace crd::containers::detail
{
    namespace
    {
        [[maybe_unused]] inline const int g_log_channel_anchor =
            ::crd::containers::force_link_log_channel();
        [[maybe_unused]] inline const int g_string_anchor =
            ::crd::containers::force_link_string();
    }
}
```

Each non-template `.cpp` we add gets its own anchor pair from now on.

---

## 9. v1c — `HashMap<K, V>`, `HashSet<K>`

### 9.1 Why open addressing + Robin Hood + backshift?

Open addressing means every entry lives directly in the slot array — no
chained linked lists, no per-entry heap allocations. Cache-friendly
probes always stay inside the same contiguous buffer.

Robin Hood probing tracks each entry's probe distance from its ideal
slot. When inserting hits a slot whose occupant is "richer" (smaller
distance), we swap and continue placing the displaced entry. Result:
probe distances are bounded and the variance is small. Lookups
short-circuit early when they encounter a slot whose occupant has a
smaller distance than the probe we're at — if our key were here, it
would have evicted them, so we know it's not in the table.

Backshift deletion is the lookup-friendly alternative to tombstones.
On `erase`, after destroying the slot, we walk forward and shift any
entry with `distance > 0` one step closer to its ideal home. The
table stays compact, lookups stay fast indefinitely.

### 9.2 Layout

```cpp
template<typename K, typename V, typename Hash, typename KeyEqual>
class HashMap
{
    IAllocator* m_alloc;
    K*  m_keys;        // capacity-many slots, raw uninit storage
    V*  m_vals;        // capacity-many slots, raw uninit storage
    u8* m_dist;        // capacity-many bytes; 0xFF = empty
    usize m_capacity;  // power of two
    usize m_mask;      // capacity - 1
    usize m_size;
};
```

Three parallel arrays (keys, values, dist bytes) are allocated through
the IAllocator. Power-of-two capacity gives mask-based modulo. The
`m_dist` byte both encodes "is this slot live?" (`0xFF` = empty) and
the probe distance (`0..0xFE`) for live entries.

### 9.3 The five core operations

#### `find(key)` — heterogeneous lookup

```cpp
template<typename Q>
V* find(const Q& key) noexcept;
```

`Q` is whatever the hasher and `KeyEqual` accept. With the default
`KeyEqual = std::equal_to<>` (the C++14 transparent variant), and
heterogeneous-hash specialisations on the hasher side, this lets you
search a `HashMap<String, V>` with a `StringView` or `const char*`
without allocating a temporary `String`.

The probe loop:

1. Hash the key, mask down to a slot.
2. If the slot is empty, key isn't in the table.
3. If the slot's distance is less than our probe count, key isn't in the
   table (Robin Hood early termination).
4. If distance matches and the keys compare equal, found.
5. Otherwise advance one slot, increment probe count.

Average lookup is O(1); worst case is bounded by the maximum probe
distance which Robin Hood keeps tight.

#### `emplace(key, args...)` — Robin Hood insert

The new entry is staged in stack-local aligned storage, then we walk
the table:

1. Empty slot? Place there.
2. Same distance + same key? Duplicate; discard our staged entry.
3. Slot is "richer" (distance < ours)? Swap in our entry, continue
   probing with the *displaced* entry. The richer slot got our entry
   because we paid more probes to find it.

Returns `true` if a NEW entry was placed, `false` if the key already
existed (existing value preserved — use `operator[]` to overwrite).

#### `erase(key)` — backshift

After destroying the entry's slot:

```cpp
cur  = idx;
next = (cur + 1) & m_mask;
while (m_dist[next] != empty && m_dist[next] > 0) {
    move slot[next] to slot[cur];
    m_dist[cur] = m_dist[next] - 1;
    mark slot[next] empty;
    cur = next;
    next = (next + 1) & m_mask;
}
```

We stop when we hit an empty slot OR a slot whose entry is already in
its ideal home (`distance == 0`). No tombstones ever exist in the
table, so subsequent lookups stay fast.

#### `operator[]` — insert-or-find

Returns a reference to the value for `key`. If missing, inserts a
default-constructed `V`. After insertion the slot may have moved due to
Robin Hood swaps, so we re-find by key to get the final reference.

#### `iterator` — skip empty slots

`++` advances `m_idx` and skips slots whose `m_dist` is `0xFF`. End is
`m_capacity`. We expose `key()` and `value()` accessors instead of
`std::pair` — saves a copy and reads more naturally.

### 9.4 Capacity policy

```
kMaxLoadFactor   = 0.875
kInitialCapacity = 8
```

`reserve(n)` and `ensure_capacity_for_insert()` compute the required
power-of-two capacity such that `n / capacity <= 0.875`. When growing,
we double. The 0.875 ceiling keeps probe distances bounded even at high
fill — Robin Hood is the right algorithm for high load factors.

### 9.5 Heterogeneous hash for `HashMap<String, V>`

`DefaultHash<String>` was already pinned in v1b to produce the same
`u64` as `DefaultHash<StringView>` for identical bytes. v1c extends the
specialisation so `DefaultHash<String>` *itself* has overloads for
`StringView` and `const char*`:

```cpp
template<> struct DefaultHash<String> {
    u64 operator()(const String& s)        const noexcept;  // bytes via to_view
    u64 operator()(std::string_view sv)    const noexcept { return hash_string(sv); }
    u64 operator()(const char* cstr)       const noexcept { /* same */ }
};
```

So `m.find(StringView{"foo"})` resolves through `Hash{}(key)` →
`DefaultHash<String>{}(StringView{"foo"})` (the second overload), and
the resulting hash matches the one used to store the entry. No
temporary String is ever materialised.

Equality on the lookup side uses transparent `std::equal_to<>` which
forwards to `String == StringView` (defined in v1b's friend operator).

### 9.6 `HashSet<K>` is a wrapper

```cpp
template<typename K, ...>
class HashSet {
    HashMap<K, EmptySetValue, ...> m_map;  // value is a 1-byte tag
public:
    bool insert(const K&);
    template<typename Q> bool erase(const Q&);
    template<typename Q> bool contains(const Q&) const;
    // iterator yields K (not pair)
};
```

The `EmptySetValue` is an empty struct (`struct {}`); compilers give it
1 byte each. We pay for it but the cost is tiny vs. the full Robin
Hood reuse we get. Done this way to avoid duplicating ~400 lines of
hash-table code.

---

## 10. v1d — Log RingBufferSink migration + cycle break

### 10.1 The dependency cycle problem

Original v1d plan was "move `RingBufferSink`'s storage onto
`crd::containers::RingBuffer<StoredLogRecord>`". But:

- `crd-log` would now need `crd-containers` headers + lib
- `crd-containers` already had a link dependency on `crd-log` (for the
  `g_log_containers` channel definition)

Two-way cycle. CMake doesn't love it. Even when MSVC's static-lib
resolution can sometimes work it out, the build graph stays fragile and
any future addition to either side risks tripping a new cycle.

### 10.2 The fix: move first-party channel definitions to crd-log

`g_log_containers` is *declared* in `<crd/containers/log_channel.hpp>`
(an `extern` reference) but *defined* by
`engine/log/src/log_channels_first_party.cpp`. Now:

- `crd-log` depends on `crd-containers` (containers headers, Array<T>
  storage for RingBufferSink, plus the channel decl).
- `crd-containers` does NOT link `crd-log`. Its `crd-log` use is
  header-only — only the `CRD_DECLARE_LOG_CHANNEL` macro from
  `<crd/log/log_channel.hpp>`.

One-way arrow, no cycle.

For consumers (test executables, runtime), nothing changes: linking
`crd-log` brings in the channel symbol; linking `crd-containers` brings
in the headers. Linking both — which everyone does — gets both.

### 10.3 The dead-code-stripping anchor, evolved

The anchor block in `containers.hpp` now references three force-link
helpers, including one that lives in `crd-log`:

```cpp
namespace crd::containers::detail {
    namespace {
        inline volatile int g_log_channel_anchor =
            ::crd::containers::force_link_log_channel();
        inline volatile int g_first_party_channels_anchor =
            ::crd::log::force_link_first_party_channels();   // NEW
        inline volatile int g_string_anchor =
            ::crd::containers::force_link_string();
    }
}
```

Two important details:

1. **`volatile int`, not `const int`.** During v1d implementation the
   `inline const int = func()` form turned out to be foldable — MSVC
   could see the function returned a constant and elide the call,
   leaving no UNDEF reference and letting the linker strip the TU. We
   verified by `dumpbin /symbols` on the test obj. `volatile` forces
   the compiler to emit an actual call + read.
2. **Anchored across the dependency boundary.** Anyone including
   `<crd/containers/containers.hpp>` now anchors both the
   crd-containers and crd-log first-party-channels TUs.

### 10.4 RingBufferSink: from std::vector to crd::containers::Array

Storage swap, same ring math:

```diff
-#include <vector>
-std::vector<StoredLogRecord> m_buffer;
+#include <crd/containers/array.hpp>
+crd::containers::Array<StoredLogRecord> m_buffer;
```

Constructor pre-fills the buffer to `m_capacity` elements via
`m_buffer.resize(m_capacity)` so we can index into it like a fixed-size
array (the wrap-around math is the same as before — manual `% m_capacity`
on `m_head`).

Why not `crd::containers::RingBuffer<StoredLogRecord>` directly? Two
reasons:

1. **Capacity must be power of two** for `RingBuffer<T>`. The sink
   accepts any user-supplied capacity (default 1024, but anything goes).
   Forcing power-of-two would change the public API.
2. **`RingBuffer::try_push` refuses on full** — but the sink's contract
   is *overwrite the oldest entry*. Either we wrap RingBuffer with a
   policy adapter (extra abstraction layer for a single user) or we
   keep the manual ring math with `Array<T>` storage. We picked the
   latter; it's strictly less code.

Snapshot still returns `std::vector<StoredLogRecord>` — that's the
external API and changing it would ripple through tests and any future
ImGui debug overlay. Internal storage, public API: separate concerns.

---

## 11. Tests reference

`tests/containers/test_containers.cpp` — **76 cases**:

| Group | Count |
| --- | --- |
| `hash` (splitmix, FNV-1a, DefaultHash specs) | 3 |
| `Span` (as_span / make_span) | 3 |
| `Array` (push / emplace / erase / swap_remove / iterators / std::sort / try_push_back / dtor counting) | 17 |
| `FixedArray` (full semantics, copy/move, range-for, Span source) | 7 |
| `log channel` (force-link anchor works) | 1 |
| `String` (SSO boundary, heap promotion, append, comparison, heterogeneous hash) | 16 |
| `RingBuffer` (push/pop FIFO, wrap-around, snapshot, dtors) | 9 |
| **`HashMap` v1c** (insert/find/erase, duplicates, op[], rehash, backshift, clear, reserve, copy/move, iterator, heterogeneous lookup, stress test, dtors) | 16 |
| **`HashSet` v1c** (insert/contains/erase, iteration, heterogeneous contains) | 3 |
| `crd-log` tests (levels, channels, sinks, async, bridge) — separate suite | 16 |
| `crd-memory` tests (alignment, allocators, stats) — separate suite | 25 |
| `crd-core` tests | 2 |

Container suite alone: **76 tests** (was 57 in v1b).
**Engine total: 119 tests** in Debug + ASan, **118** in Release (one
Debug-only memory-stats test correctly skipped).

| Preset | Tests | Notes |
| --- | --- | --- |
| `win-debug` | 119/119 | full suite |
| `win-release` | 118/118 | Debug-only stats test correctly skipped |
| `win-asan` | 119/119 | no leaks, no UAF, no OOB |

---

## 12. What this gives me, in one sentence

A complete container foundation — Array, FixedArray, Span, String,
RingBuffer, HashMap, HashSet — all allocator-aware via `IAllocator*`,
all green under Debug/Release/ASan, with a clean one-way module
dependency graph (crd-log → crd-containers, no cycle), ready to be
the substrate for `crd-math`, `crd-platform`, and the rest of Phase 1.
