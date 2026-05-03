---
id: ADR-0041
title: crd-platform async filesystem I/O
date: 2026-05-03
status: Accepted
tags: [platform, resources, jobs]
---

# ADR-0041 — `crd-platform` async filesystem I/O

## Context

ADR-0022 listed five prerequisites for the open-world streaming pipeline:

1. Allocator architecture (Phase 1, shipped)
2. Job system (Phase 2.5, shipped)
3. **Async filesystem I/O (`crd-platform` + Phase 2.5 jobs)** ← the missing piece
4. Resource manager / streamer (`crd-resources`, Phase 2.6)
5. Streaming allocator implementation (Phase 2.2)

Prerequisite 3 was deferred: at the time `crd-platform` had only synchronous file APIs because
nothing higher in the stack needed async yet. With `crd-jobs` now shipped (Phase 2.5) and
`crd-resources` v1d planning async loads, the gap must close.

The naive options:
- **Synchronous reads on a worker fiber.** A blocking `read()` parks the OS thread; the fiber
  scheduler can't migrate other fibers onto a parked thread, defeating the whole point of the
  fiber pool.
- **Synchronous reads on a dedicated I/O thread pool.** Works, but introduces a second thread
  pool that the job system doesn't manage. Two separate scheduling stories complicate priority
  inheritance and shutdown.
- **OS-native async I/O behind a `crd::jobs::Counter`.** IOCP on Windows, `io_uring` on Linux.
  Native completion ports notify a kernel-managed thread; that thread decrements the counter;
  the fiber that called `wait` resumes on the original scheduler.

The third is what every shipping streaming pipeline uses (Frostbite, Unreal IOStore,
Decima Engine). It's also the only option that interoperates cleanly with the existing job
system — by handing back a `crd::jobs::Counter*` it joins the existing wait/suspend
infrastructure.

The remaining design question was **where the API lives**. Options:
- Inside `crd-resources` as a private utility — leaks file I/O into a higher-level module that
  shouldn't own it.
- A new `crd-io` module — adds a new module for one feature.
- **Extension of `crd-platform` — `crd-platform` already owns the filesystem-abstraction
  responsibility.**

The third option is a one-API extension to a module that's the right architectural home.

## Decision

**Extend `crd-platform` in Phase 2.6 v1d with `AsyncFile::read_async()` returning a
`crd::jobs::Counter*`. IOCP on Windows, `io_uring` (with `aio` fallback) on Linux. No
dependency on `crd-resources` — `crd-platform` stays low in the dependency graph; `crd-resources`
is the first consumer but the API is general.**

### 1. Public API

```cpp
// engine/platform/include/crd/platform/async_file.hpp
namespace crd::platform
{
class AsyncFile
{
public:
    [[nodiscard]] static crd::containers::UniquePtr<AsyncFile>
    open_read(crd::containers::StringView path);

    // Issues an async read. Returns a Counter that hits 0 when the read completes.
    // Caller waits with crd::jobs::wait(counter), which suspends the fiber.
    // After the counter hits 0, dst is valid and read_result() is queryable.
    [[nodiscard]] crd::jobs::Counter*
    read_async(u64 offset, crd::containers::Span<u8> dst);

    // Result of the most recently completed read on this file.
    struct ReadResult { u64 bytes_read; bool ok; i32 error_code; };
    [[nodiscard]] ReadResult last_result() const noexcept;

    [[nodiscard]] u64 size() const noexcept;
    ~AsyncFile();
};
} // namespace crd::platform
```

`open_read` returns `nullptr` on failure; the caller owns the `UniquePtr`. The file is closed
on destruction. `read_async` is callable concurrently from multiple jobs against the same file
(each call gets its own counter and own `dst` region).

### 2. Backend on Windows — IOCP

Each `AsyncFile` opens with `FILE_FLAG_OVERLAPPED`. A process-wide `HANDLE g_iocp` (one IOCP
created at `crd::platform::init()`) is associated with each file. Each `read_async` allocates
an `OVERLAPPED` from a small pool, calls `ReadFile`, and returns a fresh `crd::jobs::Counter*`
initialised to 1. A dedicated `iocp_completion_thread` calls `GetQueuedCompletionStatus` in a
loop; on completion it stores the result, decrements the counter, and (via the existing job
system's waiter-walk) re-queues any fibers waiting on it.

The completion thread is OS-managed; it lives outside the `crd::jobs` worker pool. It does no
work besides counter decrement and is effectively always asleep in the kernel.

### 3. Backend on Linux — `io_uring` (with `aio` fallback)

Preferred backend is `io_uring` (kernel ≥ 5.6). The same pattern: one process-wide ring
created at `crd::platform::init()`, `read_async` submits an SQE with the counter pointer in
`user_data`, a dedicated `uring_completion_thread` runs `io_uring_wait_cqe` in a loop and
decrements counters from completed CQEs.

If the kernel doesn't support `io_uring`, the backend falls back to POSIX `aio`. Same API
surface; the build script picks one at configure time based on `<liburing.h>` availability.

### 4. Counter ownership

The `Counter*` returned by `read_async` is owned by the I/O backend. It is freed back to the
counter pool after the completion thread has decremented it AND every fiber waiter has been
re-queued. Callers MUST NOT call `crd::jobs::counter_release` on it — they only call
`crd::jobs::wait`. This mirrors the convention of `crd::jobs::run` (caller waits, the system
manages the counter lifetime).

### 5. `crd-platform` does NOT depend on `crd-resources`

The dependency direction is `crd-platform → crd-jobs` (it produces `Counter*` which are
defined by `crd-jobs`). `crd-resources` depends on both. There is no upward dependency from
`crd-platform` to anything in Phase 2.6+.

The existing `crd-platform → crd-jobs` edge is new in this phase. It's a small, downward edge
(jobs is itself low in the graph; it depends only on core + containers per ADR-0033). No cycle.

### 6. Synchronous `File` API stays untouched

The existing synchronous `crd::platform::File` (read/write blocking) remains for tests, CLI
tools, the cooker, and any caller that doesn't need the fiber-cooperative path. The new
`AsyncFile` is additive.

## Consequences

**Good:**
- Closes the missing prerequisite of ADR-0022. The streaming pipeline now has all five pieces.
- Fiber-cooperative I/O: a render-prep fiber waiting on a 4 MB read does not park the OS
  thread; other fibers continue to drain.
- One uniform wait primitive (`crd::jobs::Counter`) covers compute jobs AND I/O completion.
  Higher-level code waits the same way regardless of what it's waiting on.
- `crd-resources` is the first consumer but the API is general — a future audio streamer or
  network preloader uses the same `AsyncFile`.
- Backend choice (IOCP vs `io_uring` vs `aio`) is invisible to callers. Adding macOS later
  means adding one backend file; the public header doesn't change.

**Constraints:**
- `crd-platform` now depends on `crd-jobs`. This is a new edge; previously `crd-platform` had no
  upward dependencies in the engine graph. Acceptable because `crd-jobs` is itself at the
  bottom of the graph (depends only on core + containers); no cycles introduced.
- Counter pool sizing matters: each in-flight async read holds one `Counter`. The default
  `crd::jobs::Config::max_counters = 512` covers typical usage; large streaming workloads may
  need to bump it.
- IOCP and `io_uring` both require process-global initialization. `crd::platform::init()` /
  `shutdown()` must run before / after `crd::jobs::init()` / `shutdown()` only because the
  completion thread should be alive before any read is issued. The `crd-app` startup order
  enforces this.
- The completion threads (one IOCP, one `io_uring`) are OS threads outside the job system's
  worker pool. They show up in `tasklist`/`htop` separately. Minimal wake-up cost, but they
  exist.
- POSIX `aio` is the worst-case backend — it's slower and less reliable than `io_uring`. The
  fallback exists for old kernels (RHEL 8 era); production targets should use `io_uring`.

## References

- `docs/phases/phase-2.6-resources.md` (v1d slice)
- ADR-0006 — Platform v1 (the module being extended)
- ADR-0022 — Open-world streaming pipeline (the prerequisite this closes)
- ADR-0033 — `crd-jobs` implementation (the `Counter` substrate this hooks into)
- ADR-0036 — `crd-resources` module placement (the first consumer of this API)
- ADR-0039 — `ResourceHandle<T>` semantics (the `wait_ready()` fiber path uses this)
- IOCP — Microsoft I/O Completion Ports
- `io_uring` — Jens Axboe, kernel ≥ 5.6
