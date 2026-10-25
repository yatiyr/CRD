---
id: ADR-0033
title: crd-jobs implementation architecture
date: 2026-05-01
status: Accepted
tags: [jobs, arch, fibers, threading]
---

# ADR-0033 — crd-jobs implementation architecture

<!-- doc-role: decision -->
> Decision record; read status and supersession notes. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

## Context

Phase 2.5 brings `crd-jobs`: a production-grade fiber-based job system required for async pipeline
compilation, async asset upload, and general parallel work across all future engine subsystems.
ADR-0015 established the high-level shape (fibers, work-stealing, counters). This ADR records the
concrete implementation decisions made at the start of Phase 2.5.

## Decisions

### 1. Main thread joins the worker pool (converts to a fiber)

The main OS thread calls `jobs::init()` and is immediately converted to a fiber, then enters the
same worker loop as every other thread. This is the Naughty Dog approach (GDC 2015).

**Why:** If the main thread sits outside the pool it either blocks on a semaphore (wasted core) or
busy-polls (wasted power). Joining the pool means every core is productive during a `wait()`.

**Implication — pinned jobs:** GLFW and Win32 UI APIs require calls from the thread that owns the
window (the main OS thread, thread 0). To preserve this, `crd-jobs` introduces a **pinned job**
concept: a job tagged with a required thread index. The main thread worker loop checks for pinned
work at every iteration before stealing from other queues. GLFW event pumping runs as a pinned job
each frame. Amended 2026-10-11 (DIAG.10c): each thread's pinned work was a single slot, so a second
job pinned to a thread before that thread drained the first asserted where asserts are on and
overwrote (lost) the first elsewhere; authored CEIR reaches this with two `task.main_thread` tasks
in flight. The pinned lane is now a bounded MPMC queue (`Config::pinned_queue_capacity`, default
64), and a full pinned lane, like a full injection queue, is a counted, always-on fatal instead of
a dropped job ([session](../sessions/2026-10-11-diag-10c-queue-pressure.md)). Amended again
2026-10-11 (DIAG.10c workloads): on the default (shared-semaphore) wake path a job pinned to a
worker thread posted one token and woke one arbitrary sleeper; when that was another worker it took
the token, found nothing it could run and slept again, and the target slept on with the job in its
lane, so whoever waited on it hung. A pinned push now wakes every sleeper, and a worker announces
that it is going to sleep before its last look at its own pinned lane; a worker about to sleep that
finds an announced sleeper holding pinned work wakes every sleeper instead of sleeping. The opt-in
targeted path (ADR-0094) is unchanged ([session](../sessions/2026-10-11-diag-10c-workloads.md)).

### 2. Hand-rolled assembly context switch

The fiber context switch is implemented in hand-written assembly (`fiber_switch.asm` for MSVC,
`fiber_switch.S` for GCC/Clang). It saves and restores only the callee-saved registers mandated by
the platform ABI:

- **Windows x64 ABI:** RBX, RBP, RDI, RSI, R12–R15, XMM6–XMM15, RSP (+ MXCSR, FCW for
  floating-point environment)
- **Linux x86-64 SysV ABI:** RBX, RBP, R12–R15, RSP (XMMs are caller-saved), + MXCSR and FCW. Amended
  2026-10-11 (DIAG.10c): the original "no FP env save needed" was wrong. The psABI makes the MXCSR control bits and
  the x87 control word callee-saved, and without the save a job's flush-to-zero or rounding mode reached the next
  job on its thread ([runtime diagnostics](../design/runtime-diagnostics.md#diag-10c)).

**Why:** Platform fiber APIs (CreateFiber / makecontext) save the full CPU state (SSE, AVX,
TLS slots, debug registers) and pay OS-level overhead (~200 ns per switch). The hand-rolled path
saves only what the ABI mandates (~15–30 ns per switch). Boost.Context uses the same technique;
so do DICE Frostbite, IO Interactive Glacier, and Insomniac's engine.

One assembly file per platform is the canonical approach. `CMakeLists.txt` selects the correct
source via generator expressions.

### 3. Chase-Lev work-stealing deques + Vyukov MPMC injection

Each worker thread owns **three Chase-Lev work-stealing deques** — one per priority level
(High / Normal / Low). The owner pushes and pops from its own bottom; thieves steal from the top
using a CAS.

External threads (non-workers, async I/O callbacks) inject work through **three Vyukov MPMC bounded
queues** — one per priority. Workers drain injection queues before checking their local deques (so
external injection is low-latency from the worker's perspective).

**Why Chase-Lev:** Proven in the literature (Lê et al. 2013 "Correct and Efficient Work-Stealing for
Weak Memory Models"). The owner's push/pop path is fully lock-free and has no CAS; the steal path has
exactly one CAS. This minimises contention on the hot path.

**Why Vyukov MPMC:** The injection queue is bounded (known capacity at init time) and the
Vyukov design is the standard single-bounded lock-free MPMC reference with zero dynamic allocation.

### 4. 64-byte SBO (small-buffer optimisation) for job closures

`make_job<F>()` uses a 64-byte inline storage buffer. Closures that fit (≤ 64 bytes including the
function pointer slot) are stored inline in the `JobDecl` struct — zero heap allocation. Closures
larger than 64 bytes must be passed via `JobDecl` with a raw `void (*fn)(void*)` + `void* data`
pair (the caller manages lifetime).

**Why 64 bytes:** A cache line is 64 bytes. Typical game closures (one or two pointers + one or two
integers) fit. The struct layout is:

```
JobDecl {
    fn_ptr   : 8 bytes
    sbo_buf  : 48 bytes     ← 6 pointers' worth
    stack    : 1 byte (StackSize enum)
    priority : 1 byte
    _pad     : 6 bytes
}  total: 64 bytes (one cache line)
```

### 5. Pool-allocated counters with ABA-safe double-check wait

`Counter` objects are pre-allocated at `jobs::init()` time into a fixed-size pool (`Config::max_counters`).
No heap allocation occurs at `run()` time.

The `wait(counter, target)` suspend protocol uses an **ABA-safe double-check**:

1. Load counter value. If already at target → return immediately.
2. Append current fiber to the counter's waiting list (Treiber stack via CAS).
3. Re-load counter value. If now at target → remove self from list, return immediately.
4. Otherwise yield — suspend current fiber, switch to scheduler.

This eliminates the race where the counter reaches the target between step 1 and step 2 without
requiring epoch counters or seqlocks.

### 6. Three priority levels — High / Normal / Low

Priority levels are `enum class Priority : u8 { High, Normal, Low }`.

Workers drain in order: all High injection → local High deque → steal High from peers →
Normal injection → local Normal deque → steal Normal → Low injection → local Low deque →
steal Low. Only if all queues are fully empty does a worker sleep on a semaphore.

**Why three levels:** One level is insufficient for engine work (async upload must not delay input
processing). Five levels add complexity with diminishing returns. High/Normal/Low maps naturally
onto: High = time-critical frame work (input, audio), Normal = parallel game tasks (physics, AI),
Low = background I/O, cook, streaming.

### 7. Stack sizes — best practice

Three fiber stack sizes matching the Naughty Dog reference and industry practice:

| Size | Bytes | Pool capacity | Use case |
|---|---|---|---|
| Small | 64 KB | 128 | Leaf tasks: math, callbacks, short asset work |
| Medium | 512 KB | 64 | Mid-depth: decompression, render command building, animation |
| Large | 2 MB | 16 | Main thread fiber; deep recursion; scripting |

Stacks are VirtualAlloc'd with a guard page below the stack to catch overflows. Fiber pool
acquisition fails loudly (assert) rather than silently returning nullptr.

### 8. A thread keeps its completed job's fiber (2026-10-25, JOBS-HARD.3)

Each tier's free list is a Treiber stack whose 64-bit head packs the fiber index with a pop generation (ABA-safe
without a double-width compare-exchange). Every job used to pop a fiber from it and push the fiber back, so with
eight workers on 16 logical processors the overhead harness's tiny-job workload spent 53% of its CPU in
`FiberPool::acquire`, 37% on the head's `lock cmpxchg`
([sampling board](../bench/2026-10-25-diag-11b-external-sampling.md)).

A worker thread (and the main thread while it waits or pumps) now keeps the fiber of the job it completed. The
fiber stays acquired from the pool; its stack is rebuilt and it runs the thread's next new job of the same tier. The
thread returns it before it dispatches a resume (the parked fiber runs, not a fresh one), before a job of another
tier, just before it blocks waiting for work (the semaphore's pre-sleep step, a `SleepHook`; a worker that finds a
token instead loops and keeps it, because work is coming), when it leaves its dispatch loop, when `wait()`,
`pump_main_thread_once()` or `pump_main_thread_until_idle()` returns, and `WorkerPool::shutdown()` drains every slot
before the pool's own shutdown. Returning it on every empty `try_pop` instead (the first version, 1a12466c) left one
free-list acquisition for every 3.6 jobs at sixteen threads, because a worker that found the queues empty usually
found a token as well. A run of jobs that do not park then reaches the free list once when it starts and once when it ends,
instead of twice per job. The slot is a plain pointer that only its thread touches, so no hand-off between threads
exists. A fiber parked on one thread and completed on another is kept by the thread that completed it.

**Why this and not a cache of idle fibers in front of the free list.** A per-thread cache of idle fibers, filled
and drained in batches, makes acquire and release local too, but then a fiber in a cache is not in use. The pool's
in-use count would be the sum of per-thread counts. Its high water would be the peak of that sum, and that peak
cannot be observed without a global update on every acquire. `crd::BudgetReading` promises a high water that the
resource actually held and never undercounts: `pressure_report` proves a window Relaxed from it. Under that promise
an idle cache keeps one contended atomic update per job, or it gives up the exact high water. A kept fiber is
counted as acquired, so the existing counters stay exact, and no other job can take it.

**Why not let a starving thread take another thread's kept fiber.** Kept fibers are not stolen. With stealing, a
reading could show every fiber in use while a request would still be served, so Full and Saturated would overstate
the pool. Stealing is not needed for safety. A request is refused only when every fiber is running, parked or kept
by another thread, and each other thread holds at most one fiber at any moment (the one it runs or the one it
keeps between jobs). So a refusal needs `parked + threads − 1` fibers held, the same condition as before. The
sizing rule is unchanged: a tier is safe when its count is at least the fibers that can be parked at once plus
the threads. A tier sized below that bound could be refused before too, whenever every thread ran at once. The
high water can now read up to one fiber per thread higher on the same work: those fibers really were held.

The free list itself is unchanged: its head, the in-use count and the peak share one cache line, written by the same
thread in the same call (one line transfer, not two), and each tier has its own line. `FiberPool::acquisitions()`
counts the fibers taken off the list.

### 9. One wake per batch of submitted jobs (2026-10-25, JOBS-HARD.3)

A token on the worker semaphore is a wake hint, and only an idle worker consumes one. `push()` posted one token and
one wake per job, so a `run()` of 2048 tiny jobs left up to 2048 tokens for idle workers to spin through (an empty scan
of every queue and peer deque each), and the producer paid a wake per job (on Linux a `FUTEX_WAKE` syscall even with no
sleeper). The per-job pool operations had paced the consumers; once the kept fiber removed them, that spin made
linux-gcc-shipping's off mode 25% slower at four threads. `jobs::run` of a span now pushes its jobs without a wake
each (`Scheduler::push_unwoken`) and posts at most one token per thread with one wake (`Scheduler::wake_for`), after
its first injected job, so sleeping workers start while the rest is pushed, and after its last, for a worker that ran
dry and slept in between. A woken worker runs jobs until the queues are empty, so a token per thread wakes every
worker a batch can use. Pinned jobs keep their own broadcast (that token is for one thread), single pushes are
unchanged, and the opt-in targeted wake path (ADR-0094) wakes up to the same number of idle workers. The
`Semaphore` protocol itself is unchanged.

## Consequences

- `crd-jobs` depends only on `crd-core` and `crd-containers`. No circular deps.
- GLFW event pumping requires one pinned-High job submitted each frame by the application loop.
- `crd-resources` (Phase 2.6) will use `jobs::run()` for async loads and `frame_alloc()` for
  per-frame scratch.
- The assembly files are the only non-portable code in the module; they are isolated to one file
  per platform and tested by the context-switch unit tests.
- `jobs::init()` must be called before any other `crd-jobs` API. `jobs::shutdown()` joins all
  worker threads before returning.
