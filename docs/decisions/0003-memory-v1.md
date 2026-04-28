# ADR-0003 — Memory v1

**Date:** 2026-04
**Status:** Accepted
**Tags:** [memory]

## Decision

- Two-phase strategy. Phase A (shipped): IAllocator + Malloc / Linear /
  Stack / Pool. Phase B (Phase 2.2): TLSF, BuddyAllocator, RingAllocator,
  StreamingAllocator, GPUAllocator.
- IAllocator exposes `reallocate` and `allocation_size` from day one with
  default implementations, so Phase B can override without breaking
  interface.
- Containers take `IAllocator*` as a constructor argument, not a template
  parameter. Type stays stable when allocator changes.
- Default alignment = 16. Cache line = 64 constant.
- Allocators are not thread-safe by default; `MallocAllocator` is the
  exception (libc serialises).
- OOM in heap allocators is fatal. Sub-budget allocators return nullptr.
- MemoryStats tracking is debug-only. Public API identical between builds.
- `deallocate(nullptr)` is always safe.
- `StackAllocator::Marker` carries an owner pointer in debug builds.

## References

- `docs/phases/phase-1-foundations.md`
- `docs/memory/MEMORY_FILE.md`
