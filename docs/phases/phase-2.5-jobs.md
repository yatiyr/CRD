# Phase 2.5 — `crd-jobs`: fiber-based job system

**Status:** 🚧 active — planning complete; implementation begins v1a  
**ADR:** ADR-0033 — crd-jobs implementation architecture  
**Module:** `engine/jobs/`  
**Depends on:** `crd-core`, `crd-containers` only

---

## Goal

A production-grade fiber-based job system on par with Naughty Dog, DICE Frostbite, and IO Interactive
Glacier. Every engine subsystem from Phase 2.6 onward uses it for async work. The design must be
usable from application code with a minimal, safe API.

**Non-goals for Phase 2.5:**
- Async I/O (disk reads, network) — Phase 2.6 adds the I/O layer on top.
- GPU command recording from jobs — that comes with `crd-resources` (2.6).
- Hot-reloadable job graphs — Phase 3.

---

## Architecture

### Big picture

```
┌─────────────────────────────────────────────────────────┐
│                   Public API (jobs.hpp)                   │
│  run()  wait()  run_and_wait()  make_job()  parallel_for() │
│  frame_alloc()  frame_reset()  init()  shutdown()         │
└────────────────────────┬────────────────────────────────┘
                         │
         ┌───────────────┼───────────────┐
         ▼               ▼               ▼
   [High injection]  [Normal injection] [Low injection]
   Vyukov MPMC       Vyukov MPMC        Vyukov MPMC
         │               │               │
   ┌─────▼───────────────▼───────────────▼─────┐
   │              Scheduler                     │
   │  per-thread: 3 Chase-Lev deques (H/N/L)   │
   │  drain order: H-inject → H-local → H-steal │
   │              N-inject → N-local → N-steal  │
   │              L-inject → L-local → L-steal  │
   │  sleep: semaphore when all queues empty     │
   └───────────────────┬────────────────────────┘
                       │ acquire / release fiber
   ┌───────────────────▼────────────────────────┐
   │              Fiber Pool                     │
   │  Small  64 KB × 128                        │
   │  Medium 512 KB × 64                        │
   │  Large   2 MB × 16                         │
   │  guard page per stack (VirtualAlloc)       │
   └───────────────────┬────────────────────────┘
                       │ context switch
   ┌───────────────────▼────────────────────────┐
   │         Hand-rolled asm context switch      │
   │  fiber_switch(current*, next*)              │
   │  Windows x64: saves RBX RBP RDI RSI        │
   │               R12–R15 XMM6–XMM15 RSP       │
   │               MXCSR FCW                    │
   │  Linux x64:   saves RBX RBP R12–R15 RSP   │
   └────────────────────────────────────────────┘
```

### Counter / wait mechanism

```
Counter {
    atomic<u32>    value          ← decrements to 0
    atomic<Waiter*> waiters        ← Treiber stack
}

Waiter {
    Fiber*         fiber
    u32            target          ← resume when value == target
    atomic<Waiter*> next
}
```

`wait(counter, target=0)` protocol (ABA-safe):
1. Load `counter->value`. If `== target` → return immediately.
2. Append current fiber's `Waiter` to `counter->waiters` (Treiber CAS).
3. Re-load `counter->value`. If `== target` → remove self from waiters, return.
4. Yield: switch to scheduler fiber.

When a job finishes (`counter->value` decrements to `target`), the completer walks the waiters list
and re-queues each waiting fiber as a High-priority job.

### Priority drain order (per worker iteration)

```
for each iteration:
    if pinned job available for my thread_index: run it, continue
    if High injection queue non-empty: pop one, run it, continue
    if my High deque non-empty: pop one, run it, continue
    for each peer in random order: if steal from peer's High deque succeeds: run it, continue
    repeat for Normal
    repeat for Low
    sleep on semaphore until woken
```

Workers are woken by a `sem_post` whenever any job is pushed to any queue.

---

## Public API

```cpp
// engine/jobs/include/crd/jobs/jobs.hpp

namespace crd::jobs {

enum class Priority : u8 { High, Normal, Low };
enum class StackSize : u8 { Small, Medium, Large };

// 64-byte job descriptor (one cache line)
// fn(data) signature for explicit control;
// use make_job<F> for closure convenience.
struct alignas(64) JobDecl
{
    void      (*fn)(void*) = nullptr;
    void*       data       = nullptr;
    StackSize   stack      = StackSize::Small;
    Priority    priority   = Priority::Normal;
    i32         pin_thread = -1;         // -1 = any thread; 0 = main OS thread
    u8          _pad[38]   = {};
};
static_assert(sizeof(JobDecl) == 64);

// make_job — wraps a callable into a JobDecl using 48-byte SBO.
// Callables larger than 48 bytes must use the raw fn/data form.
template<typename F>
[[nodiscard]] JobDecl make_job(F&& fn,
                               StackSize stack    = StackSize::Small,
                               Priority  priority = Priority::Normal);

// Counter — opaque; pool-allocated; returned by run().
class Counter;

// Submit N jobs. Returns a counter initialised to N.
// Counter is returned to the pool when all waiters are done.
[[nodiscard]] Counter* run(crd::containers::ConstSpan<JobDecl> jobs);
[[nodiscard]] Counter* run(JobDecl job);

// Suspend current fiber until counter->value == target.
// Must be called from a fiber context (worker or main thread).
void wait(Counter* counter, u32 target = 0);

// Convenience: submit + wait in one call; counter returned to pool.
void run_and_wait(crd::containers::ConstSpan<JobDecl> jobs);
void run_and_wait(JobDecl job);

// parallel_for — splits [0, count) into num_jobs ranges, one job per range.
// F signature: void(u32 begin, u32 end)
template<typename F>
[[nodiscard]] Counter* parallel_for(u32 count, u32 num_jobs, F&& fn,
                                    StackSize stack    = StackSize::Small,
                                    Priority  priority = Priority::Normal);

// Per-thread bump allocator; reset each frame.
// Thread-safe: each thread has its own arena.
[[nodiscard]] void* frame_alloc(usize size,
                                usize alignment = alignof(std::max_align_t));
void frame_reset();   // resets every thread's arena

// Introspection
[[nodiscard]] bool is_worker_fiber() noexcept;
[[nodiscard]] u32  worker_index()    noexcept;
[[nodiscard]] u32  num_workers()     noexcept;

// Lifecycle
struct Config
{
    u32 num_threads              = 0;     // 0 = hardware_concurrency()
    u32 small_fiber_count        = 128;
    u32 medium_fiber_count       = 64;
    u32 large_fiber_count        = 16;
    u32 max_counters             = 512;
    u32 frame_alloc_bytes        = 1 << 20; // 1 MB per thread
    u32 injection_queue_capacity = 4096;
};

void init(const Config& config = {});
void shutdown();

} // namespace crd::jobs
```

**SBO layout inside `make_job`:**

```cpp
// Internal — not part of the public header.
// The 48-byte buffer occupies the fn + data + _pad fields of JobDecl.
// fn is set to a trampoline that calls operator() on the buffered F.
template<typename F>
requires (sizeof(F) <= 48 && std::is_trivially_destructible_v<F>)
JobDecl make_job_sbo(F&& f, StackSize stack, Priority prio);
```

Large-closure fallback (>48 bytes): caller must ensure lifetime; `fn(data)` form is the escape hatch.

---

## Fiber pool details

| Tier | Stack size | Count | Guard page | Use case |
|---|---|---|---|---|
| Small | 64 KB | 128 | 4 KB below | Leaf tasks, callbacks, math |
| Medium | 512 KB | 64 | 4 KB below | Decompression, render prep, animation |
| Large | 2 MB | 16 | 4 KB below | Main thread fiber, scripting, deep recursion |

Acquisition fails loudly via `CRD_ASSERT` if the pool is exhausted. Debug builds track peak usage.
Stack overflow → guard page triggers access violation → debuggable crash (not silent corruption).

---

## Thread layout

```
Thread 0  main OS thread → converted to fiber → joins worker pool
           + exclusively serves pinned jobs (GLFW event pump etc.)
Thread 1..N-1  worker threads (N = Config::num_threads or hardware_concurrency())
```

Each thread-local state:
```
ThreadLocal {
    WorkStealingDeque<JobDecl> high_deque
    WorkStealingDeque<JobDecl> normal_deque
    WorkStealingDeque<JobDecl> low_deque
    LinearArena                frame_arena
    Fiber*                     current_fiber
    u32                        thread_index
}
```

Shared (global):
```
MpmcQueue<JobDecl>  high_injection
MpmcQueue<JobDecl>  normal_injection
MpmcQueue<JobDecl>  low_injection
CounterPool         counter_pool
FiberPool           fiber_pool     // Small / Medium / Large tiers
Semaphore           worker_sem     // posted on every push
```

---

## Slice table

| Slice | Topic | Status | Notes |
|:---:|---|:---:|---|
| v1a | Hand-rolled asm context switch | ✅ | `fiber_switch_win64.asm` (MASM) + `fiber_switch_lin64.S` (AT&T); saves all ABI-mandated callee-saved regs incl. XMM6–15 + MXCSR/FCW on Windows; 5 unit tests |
| v1b | Fiber pool | ✅ | VirtualAlloc/mmap guard-page stacks; tagged 64-bit Treiber head (ABA-safe via pop-generation counter); `alignas(64)` Tier; explicit `FiberState` state machine (debug); peak watermark; 13 tests incl. concurrent ABA stress |
| v1c | Chase-Lev work-stealing deque | ✅ | `WorkStealingDeque<T>`; Lê et al. 2013; `i64` bottom/top; `alignas(64)` cache-line split; `seq_cst` pop+fence in steal (weak memory model safe); 12 tests incl. 4 000-trial last-element race + two concurrent stress tests |
| v1d | Vyukov MPMC injection queue | ✅ | `MpmcQueue<T>`; Vyukov 1024cores.net; `alignas(64)` producer/consumer positions; `acquire`/`release` sequence handshake; 10 tests incl. SPSC/MPSC/SPMC/MPMC concurrent stress |
| v1e | Priority scheduler (3-level drain) | ⏳ | 3 deques + 3 MPMC per priority per thread; drain order (High → Normal → Low); pinned-job slot |
| v1f | Counter + wait mechanism | ⏳ | `Counter` pool; Treiber waiter list; ABA-safe double-check; fiber suspend/resume |
| v1g | Worker thread pool + main-thread fiber | ⏳ | Real threads; worker loop; `init()` converts main thread to fiber; pinned job dispatch for thread 0 |
| v1h | Public API layer | ⏳ | `run()` / `wait()` / `run_and_wait()`; `Config`; `init()` / `shutdown()`; `is_worker_fiber()` / `worker_index()` |
| v1i | SBO lambda helpers | ⏳ | `make_job<F>()` 48-byte SBO; `parallel_for()`; large-closure fallback documented |
| v1j | Per-frame linear allocator | ⏳ | Per-thread bump arena; `frame_alloc()` / `frame_reset()`; reset is collective (all threads) |
| v1k | Integration smoke + crd-app wiring | ⏳ | `smoke_jobs`; `Application::run()` calls `jobs::init()` / `shutdown()`; all 6 configs green |

---

## Module layout

```
engine/jobs/
  include/crd/jobs/
    jobs.hpp          ← public API
    job_decl.hpp      ← JobDecl, Priority, StackSize (no implementation)
    counter.hpp       ← Counter (opaque forward-decl only in public header)
  src/
    fiber_switch.asm  ← Windows x64 MASM (MSVC assembler)
    fiber_switch.S    ← Linux x86-64 AT&T syntax (GCC/Clang)
    fiber.hpp         ← internal Fiber struct
    fiber_pool.cpp
    work_stealing_deque.hpp  ← header-only template
    mpmc_queue.hpp           ← header-only template
    scheduler.hpp / .cpp
    counter_pool.hpp / .cpp
    frame_arena.hpp / .cpp
    jobs.cpp          ← init / shutdown / run / wait implementation
  CMakeLists.txt

tests/jobs/
  test_fiber_switch.cpp   ← v1a
  test_fiber_pool.cpp     ← v1b
  test_deque.cpp          ← v1c
  test_mpmc.cpp           ← v1d
  test_scheduler.cpp      ← v1e
  test_counter.cpp        ← v1f
  test_jobs.cpp           ← v1g–v1k integration

runtime/examples/
  smoke_jobs.cpp
```

---

## Definition of done (Phase 2.5)

1. All 11 slices (v1a–v1k) shipped with unit tests.
2. `smoke_jobs` runs: main thread fiber conversion, parallel work dispatch, wait-on-counter, priority
   ordering validated at runtime (High jobs complete before Low jobs started at the same time).
3. Six-configuration green: win-debug / win-release / win-asan / win-clang-cl / win-relwithdebinfo / win-tidy.
4. `crd-resources` (Phase 2.6) can call `jobs::run()` without modification.
5. Pinned-job mechanism tested: a job pinned to thread 0 executes on thread 0 only.
6. ADR-0033 filed and cross-referenced.

---

## References

- Naughty Dog GDC 2015 — "Parallelizing the Naughty Dog Engine Using Fibers"
- Lê et al. 2013 — "Correct and Efficient Work-Stealing for Weak Memory Models"
- Vyukov MPMC queue — https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
- Boost.Context (for ABI reference; we do not use the library)
- ADR-0033 — implementation decisions
- ADR-0015 — original job system shape decision
