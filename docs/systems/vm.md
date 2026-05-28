# crd-vm

The lowest-level OS **virtual-memory** substrate: reserve / commit / decommit /
protect of address space, backend-neutral. A tiny leaf module (depends only on
`crd-core` + `crd-log`) that both `crd-memory` and `crd-platform` build on.

> Introduced in Phase 2.2 (ADR-0085) as the page source for the streaming
> allocator cluster. Relocated here from `crd-platform` at S2 so the
> `VirtualMemoryAllocator` (in `crd-memory`) could use it without a dependency
> cycle (`crd-platform` PUBLICly links `crd-memory`).

## What it is

`namespace crd::vm` — stateless OS calls, no vendor types in the header:

```cpp
[[nodiscard]] crd::usize page_size();              // commit granularity (4 KiB typical)
[[nodiscard]] crd::usize allocation_granularity(); // reservation base align (64 KiB on Win)
[[nodiscard]] crd::usize large_page_size();        // 0 if unavailable

[[nodiscard]] VmRegion reserve(crd::usize bytes);          // address space only, no backing
[[nodiscard]] VmRegion reserve_at(void* hint, crd::usize); // placement / testing
void                   release(VmRegion&);                  // free the reservation

[[nodiscard]] bool commit(void* ptr, crd::usize bytes);    // back with physical pages (zero-filled)
[[nodiscard]] bool decommit(void* ptr, crd::usize bytes);  // drop pages, keep address reserved
[[nodiscard]] bool protect(void* ptr, crd::usize bytes, Access);
```

Backends: Windows = `VirtualAlloc`/`VirtualFree`/`VirtualProtect`; POSIX =
`mmap(PROT_NONE)` + `mprotect` (commit) + `madvise(MADV_DONTNEED)` (decommit) +
`munmap`. WASM-friendly (no OS-specific types leak).

## Why it exists

The open-world streaming thesis: **reserve a huge contiguous address range up
front** (free on 64-bit — only address space), then **commit physical pages on
demand**. Addresses are STABLE across commit/decommit, so streaming data in and
out never invalidates a pointer; fragmentation collapses to OS page granularity.

## Properties

- **Thread-safe**: every function is a stateless, reentrant OS call. Granularity
  queries cache via a thread-safe one-time init.
- **Determinism (ADR-0063)**: reserved addresses are process-private, never part
  of sim/replay state. `commit()` zero-fills, so committed-then-read bytes are
  deterministic.
- **Stable addresses**: a reservation's base never moves; commit/decommit at page
  granularity within it.

## Consumers

- `crd::memory::VirtualMemoryAllocator` (ADR-0085 S2) — the stable-address bump
  arena that reserves a big range and commits on demand. Page source for the
  malloc-free heap (S3) and the StreamingAllocator (S5).
- `smoke_virtual_memory` — the 16 GiB-world reserve/commit/decommit demo.

## Tests

`tests/vm/test_virtual_memory.cpp` — granularity invariants, reserve/release,
commit+zero, decommit→recommit→zero at a stable base, sparse commit in a 256 MiB
reservation, protect transitions, sub-page rounding, churn (ASan), bad-input
rejection.
