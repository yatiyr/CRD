# crd-memory

The engine's allocator interface and a small set of concrete allocators.
Everything that asks "give me bytes" goes through this module.

> Long-form deep-dive: [`docs/memory/MEMORY_FILE.md`](../memory/MEMORY_FILE.md).
> This file is the "I just need to use it" overview.

## What it is

A common interface (`IAllocator`) plus a family of implementations. The core four
are below; see Quick Start, the malloc-free recipe, and the headers for the rest
(`TlsfAllocator`, `GrowableTlsfAllocator`, `GrowablePoolAllocator`,
`VirtualMemoryAllocator`):

- **`MallocAllocator`** — wraps the platform's aligned `malloc` / `free`.
  The default. Used by `default_allocator()`.
- **`LinearAllocator`** — bump pointer. Allocate forever, free everything in
  one O(1) `reset()` (or a `LinearScope` RAII wrapper).
- **`StackAllocator`** — same idea, but with `mark()` / `reset_to(marker)` so
  you can roll back to *any* prior point. Nestable via `StackScope`.
- **`PoolAllocator`** — fixed-size object slots, free-list based. O(1) get
  and return. Use one per type.
- **`StreamingAllocator`** (ADR-0085 S5) — the open-world **streaming policy layer**.
  Composes a VM resident reservation + a `GrowableTlsfAllocator` resident store (real
  O(1) per-payload free, stable addresses) + a `RingAllocator` staging arena. Per-
  `CategoryId` (caller-assigned `u32`) soft byte budgets; an injected
  `IResidencyPolicy` sheds under pressure (`NullResidencyPolicy` default fails
  gracefully). Resident path is mutex-guarded (load path); staging is lock-free.
  Wiring it to `ResourceManager`'s 2Q-LRU lands with the first real streaming
  consumer (Phase 2.7).
- **`RingAllocator`** (ADR-0085 S4) — thread-safe, epoch/fence-gated **FIFO staging**
  for the async load path. `try_claim` is lock-free (single atomic head CAS,
  multi-producer); `begin_epoch(fence)` / `retire(completed_fence)` recycle space
  by epoch (the N-frames-in-flight upload-ring). `u64` fence = a timeline-semaphore
  value (backend-agnostic). Backing buffer from an `IAllocator* parent` (VM-backed
  for malloc-free staging). NOT an `IAllocator` (no per-pointer free — reclamation
  is epoch-collective). Pure `std::atomic`, no OS deps.
- **`OffsetAllocator`** (ADR-0085 S6) — O(1) **offset allocator with external
  metadata**: manages a virtual `[0, capacity)` span (bin float-distribution +
  two-level bitmap + neighbour coalescing, ≤12.5% fragmentation) without ever
  touching the memory it describes. That property lets it manage **device-local
  VRAM** — which TLSF cannot, since TLSF writes free-list headers into the managed
  memory. The kernel under the Vulkan `GpuAllocator`; usable for any CPU offset-
  partitioning too. (Algorithm: Aaltonen's OffsetAllocator, Cerid-idiom port.)
- **`VirtualMemoryAllocator`** (ADR-0085 S2) — stable-address **bump arena** over
  `crd::vm`: reserves a huge address range (64 GiB default, free on 64-bit),
  commits physical pages on demand in commit-block multiples, hands out O(1) bumps
  at addresses that never move. `deallocate` is a no-op; free in bulk via
  `reset()`/`reset_to(mark())`; hand RAM back to the OS with `purge()`. The
  open-world streaming page source — parent a `GrowableTlsfAllocator` on it for
  fine-grained per-object free (S3). Not thread-safe (one arena per subsystem).

All four implement the same interface, so containers and other consumers
hold an `IAllocator*` and don't care which one you pass in.

## When to use what

| Workload | Allocator |
| --- | --- |
| "I just need memory and don't care" | `default_allocator()` |
| "Per-frame scratch — wipe at frame end" | `LinearAllocator` |
| "Recursive parser / build step with nested temporaries" | `StackAllocator` |
| "Many small objects of the same type (ECS components, particles)" | `PoolAllocator` |
| "Long-lived heap allocation" | `MallocAllocator` (or default) |

## Quick start

```cpp
#include <crd/memory/memory.hpp>
using namespace crd::memory;

// 1) The default heap.
auto* heap = default_allocator();
auto* p = construct<MyType>(*heap, ctor_arg1, ctor_arg2);
destroy(*heap, p);

// 2) Frame-scoped scratch.
LinearAllocator scratch(1 * 1024 * 1024);   // 1 MB
{
    LinearScope scope(scratch);
    auto* tmp = allocate_array<float>(scratch, 10000);
    // ... use tmp ...
} // tmp is gone, scratch is back to where the scope started

// 3) Per-type pool.
PoolAllocator particles(sizeof(Particle), 1024, alignof(Particle));
Particle* a = static_cast<Particle*>(particles.allocate(sizeof(Particle), alignof(Particle)));
particles.deallocate(a);
```

## Malloc-free VM-backed heap (open-world, ADR-0085 S3)

Parent a `GrowableTlsfAllocator` on a `VirtualMemoryAllocator` and every chunk —
both the chunk node and its TLSF pool — is carved from the VM reservation instead
of malloc. A general-purpose heap with **stable addresses** and **no malloc at the
root**:

```cpp
// Declare the VM arena FIRST so it outlives the heap (reverse destruction order).
VirtualMemoryAllocator vm;                 // 64 GiB reserve, commit-on-demand
GrowableTlsfAllocator  heap(64u << 20, &vm); // 64 MiB chunks, pulled from `vm`
auto* p = heap.allocate(4096, 16);          // O(1), address never moves
heap.deallocate(p);                          // returns to the TLSF free-list
```

- **Lifetime:** the VM arena MUST outlive the heap (declare arena first).
- **Commit granularity:** each chunk-grow commits `chunk_bytes` of physical memory
  up front (a VM `allocate` commits the whole request) — lower `chunk_bytes` for
  finer-grained commit. (A malloc parent instead lazy-faults via the OS.)
- **Graceful exhaustion:** `heap.try_allocate(...)` returns `nullptr` (never a
  fatal) when the VM reservation is exhausted — `grow()` asks the parent via
  `try_allocate` end-to-end.

## Design rules

- **Allocators are NOT thread-safe.** Either give each thread its own, or
  wrap externally. (`MallocAllocator` is the exception — libc serialises.)
- **OOM is fatal** in heap allocators (`MallocAllocator` / `TlsfAllocator` /
  `VirtualMemoryAllocator` / `GrowableTlsfAllocator` call `CRD_FATAL` from
  `allocate`). Bump/stack/pool allocators return `nullptr` on exhaustion.
- **`try_allocate(size, align)` is the universal non-throwing path** — a virtual
  on `IAllocator` that returns `nullptr` instead of fatal (the default delegates to
  `allocate`, which already-non-fatal allocators inherit; fatal-on-OOM allocators
  override). Composite allocators use it to fall back gracefully.
- **`deallocate(nullptr)` is always safe.**
- **Default alignment is 16 bytes** (`kDefaultAlignment`). Big enough for
  SSE/AVX and every primitive type.
- **No exceptions.** Errors are asserts or fatals.

## Diagnostics

Every allocator carries a `MemoryStats` block:

```cpp
auto snap = my_allocator.stats().snapshot();
snap.alloc_count;     // how many allocate() calls
snap.dealloc_count;   // how many deallocate() calls
snap.bytes_in_use;    // currently held bytes
snap.peak_bytes;      // high-water mark
snap.total_bytes;     // lifetime cumulative
```

Tracking is automatic, atomic, and **debug-only** — release builds get a
zero-overhead struct with all-zero fields, so production code can still
read it without `#ifdef`.

## Streaming / open-world (future)

This module's interface is deliberately ready for the streaming-allocator
work that lands during Phase 2 (graphics). `IAllocator` already exposes
`reallocate` and `allocation_size` with default implementations, so a
future TLSF / streaming allocator can override them without breaking any
container or sink that takes an `IAllocator*`.

See [`docs/ROADMAP.md`](../ROADMAP.md) for the two-phase plan.

## Dependencies

- `crd-core` (types, asserts, platform)
- `crd-log` (memory subsystem logs through `g_log_memory`)

## Tests

`tests/memory/test_memory.cpp` — 25 Catch2 tests covering alignment math,
each allocator's happy path and edge cases, exhaustion, scope rollback,
construct/destroy helpers, and stats counters.
