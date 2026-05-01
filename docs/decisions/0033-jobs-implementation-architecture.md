---
id: ADR-0033
title: crd-jobs implementation architecture
date: 2026-05-01
status: Accepted
tags: [jobs, arch, fibers, threading]
---

# ADR-0033 — crd-jobs implementation architecture

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
each frame.

### 2. Hand-rolled assembly context switch

The fiber context switch is implemented in hand-written assembly (`fiber_switch.asm` for MSVC,
`fiber_switch.S` for GCC/Clang). It saves and restores only the callee-saved registers mandated by
the platform ABI:

- **Windows x64 ABI:** RBX, RBP, RDI, RSI, R12–R15, XMM6–XMM15, RSP (+ MXCSR, FCW for
  floating-point environment)
- **Linux x86-64 SysV ABI:** RBX, RBP, R12–R15, RSP (XMMs are caller-saved; no FP env save needed)

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

## Consequences

- `crd-jobs` depends only on `crd-core` and `crd-containers`. No circular deps.
- GLFW event pumping requires one pinned-High job submitted each frame by the application loop.
- `crd-resources` (Phase 2.6) will use `jobs::run()` for async loads and `frame_alloc()` for
  per-frame scratch.
- The assembly files are the only non-portable code in the module; they are isolated to one file
  per platform and tested by the context-switch unit tests.
- `jobs::init()` must be called before any other `crd-jobs` API. `jobs::shutdown()` joins all
  worker threads before returning.
