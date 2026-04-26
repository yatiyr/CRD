# crd-memory — A Walkthrough In Plain Words

> Companion to `docs/log/LOG_FILE.md`. Same spirit: this is for *you* (and
> future-you), not a manual for outsiders. It explains *why* every piece
> exists and *how* the insides actually work, without pretending things
> are simpler than they are.

---

## 1. The 30-second mental model

Memory in this engine flows through one type:

```cpp
class IAllocator
{
public:
    virtual void* allocate(usize size, usize alignment = 16)        = 0;
    virtual void  deallocate(void* p) noexcept                      = 0;
    virtual bool  owns(const void* p) const noexcept                = 0;

    virtual void* reallocate(void* p, usize old_n, usize new_n,
                             usize alignment = 16);                  // default
    virtual usize allocation_size(const void* p) const noexcept;     // default
};
```

Every container, every subsystem, every data structure that wants memory
holds an `IAllocator*` and calls these methods. We ship four
implementations of `IAllocator` today, and we'll ship more (TLSF,
streaming, GPU) during Phase 2 — same interface, no caller changes.

That's the whole architecture. The rest is just: pick the right concrete
allocator for the job.

---

## 2. The four allocators we have today

### 2.1 `MallocAllocator` — `engine/memory/include/crd/memory/allocators/malloc_allocator.hpp`

The boring one. Wraps the OS:

- Windows: `_aligned_malloc` / `_aligned_free`.
- POSIX:   C11 `aligned_alloc` (which requires the size be a multiple
  of the alignment, so we round up internally) + `free`.

It's the default fallback. `default_allocator()` returns a global
instance of this. Containers that don't specify an allocator end up
here.

Notable choices:

- **`owns()` always returns true** for non-null pointers. We can't
  cheaply tell whether *we* allocated something, so we trust the caller.
  If you want real ownership testing, use a `LinearAllocator` or
  `PoolAllocator` (they own a known buffer range).
- **`allocation_size()` queries the platform** when possible (`_msize`
  variants on Windows have alignment caveats so we return 0; glibc has
  `malloc_usable_size`, others don't). 0 means "unknown".
- **OOM is fatal.** A failed `_aligned_malloc` triggers `CRD_LOG_CRITICAL`
  through the memory channel and then `CRD_FATAL`. There is no
  recovery story for a heap that ran out — the engine is toast.

### 2.2 `LinearAllocator` — bump pointer

Simplest possible "real" allocator. Holds a single contiguous buffer and
an offset:

```text
+----------------------------------------------------------+
| used  | used  | used   | <- m_offset points here          |
+----------------------------------------------------------+
                                                           ^
                                                       m_capacity
```

Each `allocate(size, align)` does:

1. Compute `aligned = align_up(buffer + offset, align)`.
2. Check that `aligned + size` fits in the buffer.
3. Store `offset = aligned + size - buffer`.
4. Return `aligned`.

That's *it*. There's no per-allocation header, no free list, no
fragmentation — because we never free anything individually.
`deallocate()` is intentionally a no-op.

To free, you call `reset()` (offset back to zero) or `reset_to(saved)` to
roll the offset back. The `LinearScope` RAII helper grabs the current
offset on construction and restores it on destruction:

```cpp
LinearAllocator scratch(1 << 20);
size_t before = scratch.used();    // implicit "save"
{
    LinearScope scope(scratch);
    auto* big = allocate_array<float>(scratch, 10000);
    // ... use big ...
}                                  // scope dtor restores the offset
assert(scratch.used() == before);
```

**Why this is good for game engines:** per-frame scratch memory has the
exact "all live, all die together" lifetime. You burn through 200 KB of
intermediate data while building a draw list, then `reset()` at the top
of the next frame and start over. No bookkeeping, no fragmentation,
maximum cache locality (everything is contiguous in one block).

**Why it's not enough on its own:** if you need to release some
allocations but keep others, you need either a `StackAllocator` (LIFO)
or a real general-purpose allocator (TLSF, later).

**Two construction modes:**

```cpp
LinearAllocator a(1024 * 1024);                  // owns 1 MB; freed on dtor
LinearAllocator b(my_buffer, sizeof(my_buffer)); // wraps user-provided memory
```

The second form is how you'd build a stack-backed scratch allocator or
nest one inside another arena.

### 2.3 `StackAllocator` — bump pointer with markers

A `LinearAllocator` with the addition of typed markers:

```cpp
StackAllocator s(64 * 1024);
auto m1 = s.mark();
void* a = s.allocate(64, 16);
auto m2 = s.mark();
void* b = s.allocate(128, 16);

s.reset_to(m2);   // 'b' is gone, 'a' still live
s.reset_to(m1);   // 'a' is also gone
```

In debug builds the `Marker` carries a pointer back to the allocator
that issued it, so accidentally rolling back allocator X with marker
from allocator Y trips an assert. In release the marker is just an
offset.

`StackScope` is the RAII wrapper:

```cpp
{
    StackScope scope(s);
    // allocate, allocate, allocate
}    // dtor calls s.reset_to(scope.marker)
```

Use this when scratch memory has *nested* lifetimes — recursive
parsers, build-step trees, scene-graph traversals where each subtree
needs its own scratch.

### 2.4 `PoolAllocator` — fixed-size object slots

The pool is the right choice when you have *many objects of the same
size*. Examples: 10 000 particles, 4 000 ECS components of one type,
256 sound voices.

Internals:

```text
buffer:  [slot 0][slot 1][slot 2][slot 3] ... [slot N-1]
                                                         each slot = slot_size bytes,
                                                         slot_alignment-aligned

free list: m_free_head -> slot[N-1] -> slot[N-2] -> ... -> slot[0] -> nullptr
```

When the pool is constructed, every slot is added to a singly-linked
free list. Each free slot stores a single pointer (the `next` link)
*inside itself* — that's why `slot_size >= sizeof(void*)` is asserted.

`allocate()` is two reads and a write:

```cpp
FreeNode* node = m_free_head;
m_free_head    = node->next;
return node;
```

`deallocate()` is the inverse:

```cpp
node->next  = m_free_head;
m_free_head = node;
```

Both O(1), zero loops, perfect cache behaviour because consecutive
allocations come from the most recently freed slots (LIFO reuse → hot
in cache).

**`owns()` is exact:** check that the pointer lies inside our buffer
*and* on a slot boundary. Interior pointers fail. This is unlike
`MallocAllocator::owns` which trusts the caller.

**Exhaustion returns nullptr** — the pool may be a sub-budget that the
caller wants to handle gracefully (fall back to default allocator,
overwrite the oldest entry, refuse the spawn, etc.).

**`slot_size` is rounded up to `slot_alignment`** at construction.
Asking for `slot_size=33, slot_alignment=16` gives you 48-byte slots.

---

## 3. The interface up close

### 3.1 `allocate` — required

```cpp
virtual void* allocate(usize size, usize alignment = kDefaultAlignment) = 0;
```

- Returns a non-null aligned pointer **or** triggers `CRD_FATAL` (heap)
  **or** returns `nullptr` (bump/stack/pool exhaustion). Each allocator's
  doc comment says which.
- `size > 0` and `is_pow2(alignment)` are asserted.

### 3.2 `deallocate` — required

```cpp
virtual void deallocate(void* p) noexcept = 0;
```

- `nullptr` is always safe.
- Bump/stack allocators ignore non-null pointers (they don't free
  individually). The pointer is still expected to have come from this
  allocator; passing somebody else's pointer to a pool allocator would
  trip `CRD_ASSERT(owns(p))`.

### 3.3 `owns` — required

```cpp
virtual bool owns(const void* p) const noexcept = 0;
```

Used by composite allocators (later) and by debug ownership checks. The
contract:

| Allocator | `owns(p)` semantics |
| --- | --- |
| `MallocAllocator` | `p != nullptr` (we trust you) |
| `LinearAllocator` | `p` lies inside our buffer range |
| `StackAllocator`  | same |
| `PoolAllocator`   | `p` lies inside buffer **and** on a slot boundary |

### 3.4 `reallocate` — optional, default provided

```cpp
virtual void* reallocate(void* p, usize old_size, usize new_size,
                         usize alignment = kDefaultAlignment);
```

Default: `allocate + memcpy(min(old, new)) + deallocate`. Boring but
always correct. A real-OS realloc *might* grow in place — we ignore that
opportunity for the v1 allocators because none of them benefit from it
(bump/stack don't free old, pool slots are fixed size). When TLSF / a
buddy allocator lands in Phase 2, they'll override this for in-place
growth.

Special cases handled in the default:

- `p == nullptr` → behaves like `allocate(new_size, alignment)`.
- `new_size == 0` → behaves like `deallocate(p)`, returns `nullptr`.

### 3.5 `allocation_size` — optional, default returns 0

```cpp
virtual usize allocation_size(const void* p) const noexcept;
```

Returns "the actual usable bytes for this pointer" or `0` for "I don't
know". Useful for tools and for `PoolAllocator`, which knows exactly
(it's `slot_size`). `MallocAllocator` returns 0 on Windows because
`_msize` would lie about aligned blocks.

---

## 4. The supporting cast

### 4.1 `alignment.hpp`

Pure header, all `constexpr`. `is_pow2`, `align_up`, `align_down`,
`is_aligned`. The constants:

- `kDefaultAlignment = 16` — SSE-friendly, fits every primitive.
- `kCachelineSize = 64` — for padding hot/cold fields apart later.
- `kMinAlignment = alignof(void*)` — smallest alignment any allocator
  will accept; smaller requests get rounded up.

### 4.2 `memory_stats.hpp`

Five atomic counters per allocator. Updated by `on_allocate(bytes)` and
`on_deallocate(bytes)` in the allocator implementations.

The trick: **everything is wrapped in `#if defined(CRD_DEBUG)`**. In
release the mutators do `(void)bytes` — zero overhead. The struct
fields still exist (so the public `stats()` API never changes), they're
just always zero. Tools and log dumps can call `snapshot()` in any
build without `#ifdef` noise.

`peak_bytes` uses a CAS loop because it's a max-update under
contention. We tolerate losing a tiny update under heavy parallelism;
this is a stat, not a correctness signal.

### 4.3 `construct.hpp`

Type-aware allocation helpers built on top of `IAllocator`:

```cpp
T*    construct<T>(allocator, args...);     // alloc + ctor
void  destroy<T>(allocator, p);             // dtor + dealloc
T*    allocate_array<T>(allocator, count);  // alloc only, no ctors
T*    construct_array<T>(allocator, n, args...);   // alloc + each ctor
void  destroy_array<T>(allocator, p, count);       // each dtor + dealloc
void  deallocate_array<T>(allocator, p);    // dealloc only
```

`construct` uses placement new. `destroy` checks
`std::is_trivially_destructible_v<T>` and skips the dtor call when
possible — a microscopic perf win, but correct.

### 4.4 `log_channel.hpp`

The memory subsystem has its own log channel: `g_log_memory`, defined
in `allocator.cpp`. OOM, exhaustion warnings, and (later) leak
summaries fan out through here. Each subsystem that *uses* memory keeps
its own channel; this one is for the allocator implementations
themselves.

### 4.5 `default_allocator()`

```cpp
IAllocator* default_allocator() noexcept;
```

Function-local `static MallocAllocator s_instance{"DefaultAllocator"}`.
First caller constructs it; every later caller gets the same pointer.
Never destroyed — its lifetime is "until process exit", which avoids
any "subsystem X allocated during shutdown after the default allocator
was destroyed" disaster.

Containers and other consumers should use this when they don't have an
allocator passed to them. Use it sparingly in hot paths — frame
scratch should be a `LinearAllocator`, not `default_allocator()`.

---

## 5. Why we chose this shape

### 5.1 `IAllocator*` constructor argument, not template parameter

`std::vector<T, Alloc>` makes the allocator part of the type. That
means switching from `MallocAllocator` to `StreamingAllocator`
*changes the type* of every container, breaking signatures and forcing
a project-wide refactor.

EA STL and Bitsquid both rejected this for game engines and so do we.
Our containers will look like:

```cpp
template<typename T>
class Array
{
public:
    explicit Array(IAllocator* alloc = default_allocator());
    ...
private:
    IAllocator* m_alloc;
    ...
};
```

The type stays stable across allocator changes. Open-world streaming
becomes a drop-in: `Array<Mesh> chunk_meshes(streaming_alloc);`. No
template surgery.

The cost is one virtual call per allocate. In practice this is
*nothing* — most allocations happen during loading or once-per-frame,
and the inner-loop allocations should go through a `LinearAllocator`
which the compiler can devirtualise when the type is known statically.

### 5.2 No exceptions

Engine convention. OOM in the heap is fatal (you can't keep playing the
game with no memory). OOM in a sub-budget (linear/stack/pool) returns
`nullptr` and the caller decides what to do.

### 5.3 Allocators are not thread-safe

The hot path stays branch-free. Per-thread allocators or external
wrappers are the answer. `MallocAllocator` is the lone exception
because libc serialises internally — but you still shouldn't share a
`MallocAllocator`'s `MemoryStats` between threads in performance-
critical code (the atomics serialise anyway).

### 5.4 Default-initialised `IAllocator*` field

Containers should accept `IAllocator* = default_allocator()`. This way
`Array<int> a;` "just works" without ceremony, but power users can pass
their own arena.

### 5.5 Streaming-ready interface from day one

`reallocate` and `allocation_size` are virtual functions with default
implementations. When TLSF / streaming allocators ship in Phase 2 they
override these — *no interface change*. Existing containers and sinks
keep working with the new allocators with zero source changes.

---

## 6. The lifetime of a typical allocation

```cpp
auto* a = default_allocator();                 // -> MallocAllocator instance
auto* p = construct<Mesh>(*a, "wall.obj");     // 1 v-call to allocate, then ctor
...
destroy(*a, p);                                // dtor, then 1 v-call to deallocate
```

Step by step:

1. `construct<Mesh>` calls `a->allocate(sizeof(Mesh), alignof(Mesh))`.
2. `MallocAllocator::allocate` asserts size > 0, alignment is power of two.
3. Calls `_aligned_malloc(sizeof(Mesh), alignof(Mesh))`.
4. Checks for null — if null, log Critical and `CRD_FATAL`.
5. Updates `MemoryStats` (debug only).
6. Returns the pointer.
7. `construct` runs `::new (p) Mesh("wall.obj")`.
8. Caller uses the mesh.
9. `destroy<Mesh>` calls `p->~Mesh()` (skipped if trivially destructible).
10. Calls `a->deallocate(p)`.
11. `MallocAllocator::deallocate` calls `_aligned_free(p)`, updates stats.

For frame scratch:

```cpp
LinearAllocator frame(1 << 20);                 // 1 MB, owned
{
    LinearScope scope(frame);                   // saves offset = 0
    auto* tmp = allocate_array<float>(frame, 1024);  // bump
    auto* tmp2 = allocate_array<int>(frame, 256);    // bump
    use(tmp, tmp2);
}                                               // dtor: frame.reset_to(0)
// Allocator is now empty again. tmp and tmp2 are dangling (don't keep them!).
```

---

## 7. Common mistakes and how the system catches them

| Mistake | What happens |
| --- | --- |
| `allocate(0, 16)` | `CRD_ASSERT(size > 0)` fires |
| `allocate(64, 7)` (not pow2) | `CRD_ASSERT(is_pow2(alignment))` fires |
| `pool.allocate(72, 16)` when slot_size=64 | `CRD_ASSERT(size <= m_slot_size)` fires |
| `pool.deallocate(stack_local_ptr)` | `CRD_ASSERT(owns(p))` fires |
| `stackB.reset_to(markerFromStackA)` | (debug) marker.owner check fires |
| `linear.allocate(too_big, 16)` | returns `nullptr`, logs Error, caller handles |
| OS heap exhausted | `CRD_LOG_CRITICAL` + `CRD_FATAL`, process dies |
| Forgot to `destroy()` (leak) | (debug) `MemoryStats::bytes_in_use` shows it |

---

## 8. The big skip list — what we deliberately did NOT build

These are tracked in `docs/ROADMAP.md` for Phase 2 / 2.5:

- **TLSF / general-purpose O(1) allocator** — replaces the current
  default allocator for hot paths once we have benchmark data.
- **Buddy allocator** — power-of-two block manager, used inside
  `crd-graphics` for GPU memory pages.
- **Ring allocator** — for streaming audio / telemetry / log buffers.
- **Streaming allocator** — virtual address reservation +
  page commit/decommit, the heart of open-world chunk loading.
- **GPU allocator** — Vulkan-side, will live in `crd-graphics` but
  obey the same `IAllocator`-style contract.
- **Resource manager / streamer** (`crd-resources` module) — sits on
  top of the streaming allocator + async filesystem + job system.
- **Leak-detection-on-shutdown report** — walk every allocator at
  `crd::log::shutdown()` and dump non-zero `bytes_in_use` to
  `g_log_memory`. Easy add when we want it.
- **TLSF/streaming `reallocate` overrides** — currently every allocator
  uses the default copy-based realloc. Real ones can grow in place.

The interface (`IAllocator`) is shaped so all of this can land without
changing any caller. That's the whole point of the v1 design.

---

## 9. The example output

Running `crd-runtime.exe` after the memory module landed produces lines
like:

```
2026-04-26 02:53:27.683 [INF] [Engine] tid=...  main.cpp:55 - default_allocator handed out 0x18034a361d0 (= 0xDEADBEEF)
2026-04-26 02:53:27.683 [INF] [Engine] tid=...  main.cpp:63 - frame scratch: 256 floats at 0x18034a42970, used=1024 bytes
2026-04-26 02:53:27.683 [INF] [Engine] tid=...  main.cpp:66 - frame scratch after scope: used=0 bytes
2026-04-26 02:53:27.683 [INF] [Engine] tid=...  main.cpp:76 - particle pool: 2/64 slots in use
2026-04-26 02:53:27.683 [INF] [Engine] tid=...  main.cpp:82 - heap stats: alloc=3 dealloc=1 bytes_in_use=66564 peak=66564
```

Things to notice:

- **`used=1024 bytes`** then **`used=0 bytes`** demonstrates the
  `LinearScope` rolling back the bump pointer on dtor.
- **`bytes_in_use=66564`** is `64 KB (frame scratch buffer) + 64*64
  (pool buffer) + 4 (the u32) ≈ 66 KB`. Exactly the live storage held
  by the heap allocator at that moment, because the inner allocators
  were both in flight and hadn't been destroyed yet.
- **`alloc=3 dealloc=1`** are the heap allocator's lifetime counters:
  three big blocks taken out (u32 + scratch buffer + pool buffer), one
  given back (the u32). The remaining two will be returned when the
  scratch and pool destructors fire as the scope ends.

---

## 10. Tests reference

`tests/memory/test_memory.cpp` covers:

| Group | Tests |
| --- | --- |
| `alignment` | `is_pow2`, `align_up`, `is_aligned` (3) |
| `MallocAllocator` | aligned alloc, batch round-trip, null safety, reallocate grow / null / zero (6) |
| `default_allocator` | stable pointer (1) |
| `LinearAllocator` | aligned chunks, exhaustion, reset, no-op dealloc, scope rollback, reset_to (6) |
| `StackAllocator` | marker rollback, nested scope, exhaustion (3) |
| `PoolAllocator`  | distinct slots / LIFO reuse, slot_size rounding, owns rejection, null safety (4) |
| `construct/destroy` | dtor counter, construct_array dtor count (2) |
| `MemoryStats` | tracking in debug, snapshot in any build (2) |

**25 cases**, all passing as part of `ctest --preset win-debug` (40
total across the engine).

---

## 11. What this gives me, in one sentence

A common allocator interface that today routes everything through a
small set of correct, predictable allocators, and tomorrow lets us
slot in TLSF, GPU, and streaming allocators without changing a single
caller — which is exactly what the open-world game we're aiming at
will need.
