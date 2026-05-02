# Session: crd-jobs v1j — per-thread frame allocator

**Date:** 2026-05-02  
**Slice:** v1j  
**Branch:** main

---

## What was built

Per-thread linear bump allocator backing `frame_alloc()` / `frame_reset()`.

### FrameArena design

`engine/jobs/src/frame_arena.hpp` — header-only class:

```cpp
class FrameArena {
    u8*    m_data;
    usize  m_capacity;
    usize  m_cursor;
public:
    bool  init(usize capacity);     // malloc-backed
    void  shutdown() noexcept;       // free
    void* alloc(usize size, usize alignment);  // bump with power-of-two alignment
    void  reset() noexcept;          // cursor = 0
};
```

Alignment math: `aligned = (cursor + align - 1) & ~(align - 1)` — standard bump pattern.

### Thread-local wiring

`WorkerPool` owns all arenas via `std::unique_ptr<FrameArena[]>` + `m_frame_arena_count`. The `unique_ptr<T[]>` choice is deliberate: `std::vector<FrameArena>` with deleted move constructors would fail to compile, and even with move constructors it could reallocate and invalidate the per-thread `tl_frame_arena*` raw pointers already stored by each worker thread.

Thread setup:
- `init()` allocates `m_num_threads` arenas, sets `tl_frame_arena = &m_frame_arenas[0]` for thread 0.
- `worker_loop()` sets `tl_frame_arena = &self->m_frame_arenas[thread_index]` for each worker.
- `shutdown()` calls `m_frame_arenas.reset()` — `~FrameArena()` calls `free()` for each.

### frame_reset() race contract

`frame_reset()` calls `reset()` (non-atomic `cursor = 0`) on every arena. This is NOT thread-safe relative to concurrent `frame_alloc()` calls. The documented contract: call `frame_reset()` only at frame boundaries after all jobs of the previous frame have completed via `wait()` / `run_and_wait()`. This constraint is written on the `frame_reset()` declaration in `jobs.hpp`.

### parallel_for update

`parallel_for` previously allocated `std::vector<JobDecl>` for the job array. Now:
```cpp
auto* const jobs = static_cast<JobDecl*>(
    frame_alloc(num_jobs * sizeof(JobDecl), alignof(JobDecl)));
// ... fill via memcpy from stack-constructed JobDecl
return run(std::span<const JobDecl>(jobs, num_jobs));
```
`run()` copies each element into the scheduler queue, so the frame-allocated array only needs to outlive the `run()` call.

---

## Files changed

- **NEW**: `engine/jobs/src/frame_arena.hpp` — FrameArena class
- `engine/jobs/src/worker_pool.hpp` — added `FrameArena` include, `frame_arena_bytes` in `WorkerConfig`, `unique_ptr<FrameArena[]>` + count in `WorkerPool`, `reset_all_frame_arenas()`, `tl_frame_arena_ref()` declaration
- `engine/jobs/src/worker_pool.cpp` — `tl_frame_arena` thread-local, set in `init()` and `worker_loop()`, freed in `shutdown()`, `reset_all_frame_arenas()` implementation
- `engine/jobs/include/crd/jobs/jobs.hpp` — `frame_alloc_bytes` in `Config`, `frame_alloc()`/`frame_reset()` declarations, `<cstddef>` include, `parallel_for` swapped `vector` for `frame_alloc`
- `engine/jobs/src/jobs.cpp` — `frame_alloc()`/`frame_reset()` implementations, wire `frame_arena_bytes` in `init()`
- `tests/jobs/test_jobs.cpp` — 4 new tests (21–24)
- `docs/phases/phase-2.5-jobs.md` — v1j row marked ✅
- `context.md` — updated

---

## Tests added (tests 21–24)

| # | Name | What it proves |
|---|---|---|
| 21 | frame_alloc returns aligned pointer from main thread | Non-null, aligned-to-8 from thread 0 after init() |
| 22 | frame_alloc respects alignment padding | Alloc(3,1) → cursor=3; next alloc(8,8) lands at offset 8 (alignment bump) |
| 23 | frame_alloc works from a worker fiber | alloc inside a job fiber returns non-null, correct alignment |
| 24 | frame_reset allows full capacity to be reused | Alloc 512B, reset, alloc 1024B (full arena) — no assert |

---

## Decisions

- `unique_ptr<FrameArena[]>` over `vector<FrameArena>`: FrameArena has deleted copy/move — vector can't hold it without move, and even if it could, reallocation would invalidate thread-local raw pointers set by each worker thread. `unique_ptr<T[]>` fixes both.
- `std::malloc` backing (not VirtualAlloc): 1 MB frame arenas are small and numerous; malloc avoids VirtualAlloc/page-size granularity overhead. NOLINT comments suppress cppcoreguidelines-no-malloc.
- `frame_reset()` non-thread-safe by design: making it atomic would add per-alloc overhead. The frame-boundary usage pattern guarantees safety without atomics.
- `memcpy` for parallel_for JobDecl init (not placement new): JobDecl is trivially copyable; memcpy from a stack-constructed object is well-defined and avoids `<new>` dependency.

---

## Six-configuration results

| Config | Tests |
|---|---|
| win-debug | 355/355 ✅ |
| win-release | 352/352 ✅ |
| win-relwithdebinfo | 355/355 ✅ |
| win-asan | 355/355 ✅ |
| win-clang-cl | 355/355 ✅ |
| win-tidy | 355/355, exit 0 ✅ |

---

## Next

**v1k** — integration smoke + crd-app wiring: `smoke_jobs`, `Application::run()` calling `jobs::init()` / `shutdown()`. Defined in `docs/phases/phase-2.5-jobs.md`.
