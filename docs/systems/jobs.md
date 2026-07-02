# crd-jobs

Fiber-based job system. The async backbone for all engine subsystems from Phase 2.6 onward.
**Phase 2.5 complete** — all 11 slices (v1a–v1k) shipped.

Depends only on `crd-core` + `crd-containers`. `crd-app` links it PUBLIC; `Application::run()`
calls `jobs::init()` / `jobs::shutdown()` automatically. ADR-0033.

## Status

| Slice | Ships | Status |
| --- | --- | --- |
| v1a | Hand-rolled asm context switch (Windows x64 MASM + Linux x64 AT&T) | ✅ |
| v1b | Fiber pool (three tiers, VirtualAlloc guard pages, ABA-safe Treiber free list) | ✅ |
| v1c | Chase-Lev work-stealing deque | ✅ |
| v1d | Vyukov MPMC injection queue | ✅ |
| v1e | Priority scheduler (3-level drain: H/N/L, pinned slot, semaphore sleep) | ✅ |
| v1f | Counter + wait mechanism (ABA-safe 6-step protocol, cooperative fiber suspend) | ✅ |
| v1g | Worker thread pool + main-thread fiber | ✅ |
| v1h | Public API layer (`run` / `wait` / `run_and_wait` / `init` / `shutdown`) | ✅ |
| v1i | SBO lambda helpers (`make_job<F>` 41-byte SBO, `parallel_for`) | ✅ |
| v1j | Per-thread frame allocator (`frame_alloc` / `frame_reset`) | ✅ |
| v1k | Integration smoke + `crd-app` wiring | ✅ |

## Core decisions

- **Fiber-based, not thread-based**: jobs run on fibers, not raw threads. Fibers suspend cooperatively (via `wait()`) without blocking the OS thread. The thread is immediately available for other work.
- **Hand-rolled asm context switch**: portable across Windows (MASM) and Linux (AT&T syntax). Saves all ABI-mandated callee-saved registers including XMM6–XMM15, MXCSR/FCW, and Windows TIB stack bounds (guard-page safe). No boost.context dependency.
- **Three priority levels**: High / Normal / Low. Drain order per worker iteration: pinned → H-inject → H-local → H-steal → N-inject → N-local → N-steal → L-inject → L-local → L-steal.
- **41-byte SBO for closures**: `make_job<F>()` stores callables ≤ 41 bytes inline in `JobDecl::data + _pad`. No heap allocation for typical lambda captures.
- **ABA-safe wait**: `wait(counter, target)` uses a 6-step Treiber protocol with a `canceled` atomic for double-check. No spurious wake-up is possible.
- **Frame arena**: per-thread malloc-backed bump allocator, 1 MB/thread default. `parallel_for` uses it for its `JobDecl` array. Reset at frame boundaries via `frame_reset()` (non-thread-safe; call only after all jobs from the previous frame have completed).
- **Cerid-owned sleep semaphore (2026-07-02)**: worker sleep/wake (`Scheduler::m_semaphore` + the ADR-0094 per-worker `ThreadState::wake`) runs on `crd::jobs::detail::Semaphore` — raw futex (Linux) / `WaitOnAddress` (Windows) with the canonical expected==0-observed protocol and release-always-wakes. Replaced `std::counting_semaphore` after libstdc++ (GCC 13.3, the CI runner) lost a shutdown wake under oversubscription and hung the determinism-moat tests at `join()` (proven by futex forensics: worker asleep with expected==1 while the counter word read 1). Every concurrency primitive in the module is now hand-rolled: fibers, Chase-Lev deque, Vyukov MPMC, counter park/wake, sleep semaphore. Session log: `docs/sessions/2026-07-02-jobs-semaphore-lost-wake.md`.
- **crd-app wiring**: `Application::run()` calls `jobs::init(m_desc.jobs_config)` before the tick loop. `ApplicationDesc::jobs_config` lets callers configure thread count, pool sizes, and frame arena capacity.

## Public API

```cpp
namespace crd::jobs {

// Lifecycle
struct Config {
    u32 num_threads              = 0;       // 0 = hardware_concurrency()
    u32 small_fiber_count        = 128;
    u32 medium_fiber_count       = 64;
    u32 large_fiber_count        = 16;
    u32 max_counters             = 512;
    u32 frame_alloc_bytes        = 1 << 20; // 1 MB per thread
    u32 injection_queue_capacity = 4096;
};
void init(const Config& config = {});
void shutdown();

// Submit + wait
[[nodiscard]] Counter* run(crd::containers::ConstSpan<JobDecl> jobs);
[[nodiscard]] Counter* run(JobDecl job);
void wait(Counter* counter, u32 target = 0);
void run_and_wait(crd::containers::ConstSpan<JobDecl> jobs);
void run_and_wait(JobDecl job);

// Closure helpers
template<typename F>
[[nodiscard]] JobDecl make_job(F&& fn,
                               StackSize stack    = StackSize::Small,
                               Priority  priority = Priority::Normal);

template<typename F>
[[nodiscard]] Counter* parallel_for(u32 count, u32 num_jobs, F&& fn,
                                    StackSize stack    = StackSize::Small,
                                    Priority  priority = Priority::Normal);

// parallel_reduce (D-002 v2): split [0,count) into num_jobs ranges, map each to
// a partial result, fold left-to-right in job order from `init`. Synchronous —
// submits, waits, folds, returns. R must be trivially copyable; MapFn under the
// same 41-byte SBO constraint as parallel_for's F; ReduceFn runs only on the
// caller. (Freezing an Array around a parallel pass is not a jobs function — it
// would force a crd-jobs->crd-containers edge; use crd::containers::FrozenView +
// parallel_for + wait at the call site.)
template<typename R, typename MapFn, typename ReduceFn>
[[nodiscard]] R parallel_reduce(u32 count, u32 num_jobs, R init, MapFn&& map, ReduceFn&& reduce,
                                StackSize stack = StackSize::Small, Priority priority = Priority::Normal);

// Per-thread frame allocator
[[nodiscard]] void* frame_alloc(usize size, usize alignment = alignof(std::max_align_t));
void frame_reset(); // non-thread-safe; call at frame boundary only

// Introspection
[[nodiscard]] bool is_worker_fiber() noexcept;
[[nodiscard]] u32  worker_index()    noexcept;
[[nodiscard]] u32  num_workers()     noexcept;

} // namespace crd::jobs
```

## Fiber pool tiers

| Tier   | Stack size | Count | Use case |
|--------|-----------|-------|---------|
| Small  | 64 KB     | 128   | Leaf tasks, callbacks, math |
| Medium | 512 KB    | 64    | Decompression, render prep, animation |
| Large  | 2 MB      | 16    | Main thread fiber, scripting, deep recursion |

Guard page per stack (VirtualAlloc on Windows, mmap on Linux). Stack overflow → access violation → debuggable crash, not silent corruption.

## How to use it

### Basic run + wait

```cpp
// Single job via SBO lambda
auto job = crd::jobs::make_job([]() {
    // work
});
crd::jobs::Counter* c = crd::jobs::run(job);
crd::jobs::wait(c);

// Or use the convenience form
crd::jobs::run_and_wait(crd::jobs::make_job([]() { /* work */ }));
```

### parallel_for

```cpp
std::atomic<u64> total{0};
crd::jobs::Counter* c = crd::jobs::parallel_for(
    1000U, 4U,
    [&total](crd::u32 begin, crd::u32 end) {
        u64 local = 0;
        for (u32 i = begin; i < end; ++i)
            local += i;
        total.fetch_add(local, std::memory_order_relaxed);
    });
crd::jobs::wait(c);
```

### Frame allocator

```cpp
// At frame start (after all previous-frame jobs have completed):
crd::jobs::frame_reset();

// Inside a job:
auto* buf = static_cast<MyStruct*>(
    crd::jobs::frame_alloc(sizeof(MyStruct), alignof(MyStruct)));
new (buf) MyStruct{...}; // placement new for non-trivial types
```

### Priority and stack size

```cpp
auto high_job = crd::jobs::make_job(
    []() { /* urgent */ },
    crd::jobs::StackSize::Small,
    crd::jobs::Priority::High);
```

### Via `ApplicationDesc` (typical game/sim entry point)

```cpp
crd::app::ApplicationDesc desc;
desc.jobs_config.num_threads = 8;
desc.jobs_config.frame_alloc_bytes = 2u << 20u; // 2 MB per thread
crd::app::Application app(desc);
app.push_layer(std::make_unique<GameLayer>());
app.run(); // jobs::init() and jobs::shutdown() are called automatically
```

## SBO constraints for `make_job<F>`

The closure must satisfy all four conditions:
- `sizeof(F) <= 41` bytes
- `alignof(F) <= 8`
- `std::is_trivially_copyable_v<F>` — true
- `std::is_trivially_destructible_v<F>` — true

Closures that capture by reference (raw pointer-size captures) almost always satisfy this.
Closures exceeding 41 bytes: use the raw `JobDecl::fn` / `JobDecl::data` form and manage lifetime manually.

## Long-term direction

- `crd-resources` (Phase 2.6) uses `run()` / `wait()` for async asset loading.
- `crd-scene` (Phase 3.0) uses `parallel_for()` for component system updates.
- Physics (Phase 3.1) uses pinned jobs for fixed-step simulation on a dedicated thread.
- `frame_alloc()` becomes the standard per-frame scratch allocator for any hot-path that needs temporary heap-sized storage without hitting `malloc`.
- The main thread fiber (thread 0 converted to fiber, joined as worker 0) means application code can call `wait()` from `Application::run()` without blocking the thread.
