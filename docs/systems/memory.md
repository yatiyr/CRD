# crd-memory

The engine's allocator interface and a small set of concrete allocators.
Everything that asks "give me bytes" goes through this module.

> Long-form deep-dive: [`docs/memory/MEMORY_FILE.md`](../memory/MEMORY_FILE.md).
> This file is the "I just need to use it" overview.

## What it is

A common interface (`IAllocator`) plus four implementations:

- **`MallocAllocator`** — wraps the platform's aligned `malloc` / `free`.
  The default. Used by `default_allocator()`.
- **`LinearAllocator`** — bump pointer. Allocate forever, free everything in
  one O(1) `reset()` (or a `LinearScope` RAII wrapper).
- **`StackAllocator`** — same idea, but with `mark()` / `reset_to(marker)` so
  you can roll back to *any* prior point. Nestable via `StackScope`.
- **`PoolAllocator`** — fixed-size object slots, free-list based. O(1) get
  and return. Use one per type.

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

## Design rules

- **Allocators are NOT thread-safe.** Either give each thread its own, or
  wrap externally. (`MallocAllocator` is the exception — libc serialises.)
- **OOM is fatal** in heap allocators (`MallocAllocator` calls
  `CRD_FATAL`). Bump/stack/pool allocators return `nullptr` on exhaustion
  so callers can fall back gracefully.
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
