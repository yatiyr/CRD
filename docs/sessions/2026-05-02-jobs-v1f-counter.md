# Session: `crd-jobs` v1f — Counter + wait mechanism

**Date:** 2026-05-02  
**Phase:** 2.5 — `crd-jobs` fiber-based job system  
**Slice:** v1f — Counter pool, Treiber waiter list, ABA-safe double-check wait  

---

## What was built

### `engine/jobs/src/counter.hpp` (new)

**`Waiter` struct** — stack-allocated by each caller of `counter_wait()`. Represents one fiber
suspended on a `Counter`. Fields:
- `Fiber* fiber` — the suspended fiber (written once before push, read-only thereafter)
- `u32 target` — wake when `counter->value == target`
- `atomic<bool> canceled` — set true by the ABA double-check path; prevents `counter_decrement` from
  scheduling a resume for a waiter that never actually suspended
- `atomic<Waiter*> next` — Treiber stack link; nullptr = end of list

**`Counter` struct** — `alignas(64)`, exactly 64 bytes (one cache line). Static-asserted.
Layout: `atomic<u32> value` (4) + `u32 _pad0` (4, explicit pad for 8-byte alignment of next field) +
`atomic<Waiter*> waiters` (8) + `u32 pool_index` (4) + `u32 next_free` (4) + `u8 _pad1[40]` (40).

**`CounterPool` class** — fixed-capacity lock-free pool using a generation-tagged 64-bit Treiber
free list `[gen:32 | idx:32]`, identical pattern to `FiberPool`. Generation incremented on every pop
makes ABA structurally impossible without CMPXCHG16B. Public interface: `init(capacity)`,
`shutdown()`, `acquire(initial_value)`, `release(counter*)`, `available()`.

**`counter_decrement(counter*, amount)` free function** — atomically decrements the counter value
(`fetch_sub(acq_rel)`), then atomically steals the entire waiter list (`exchange(nullptr, acq_rel)`),
partitions it into: canceled waiters (discarded), satisfied waiters (`target == new_val` → set fiber
state to `Ready` in debug builds, prepend to returned list), and unsatisfied waiters (re-pushed
individually via Treiber CAS). Returns null-terminated woken list via `Waiter::next`. Caller
re-queues each woken fiber as a High-priority job.

**`counter_wait(counter*, w*, current_fiber*, scheduler_ctx, target)` free function** — ABA-safe
six-step protocol:
1. Load `counter->value` (acquire). If `== target` → return (fast path, no suspension).
2. Set `w->fiber = current_fiber`, `w->target = target`, `w->canceled = false`.
3. Treiber CAS push onto `counter->waiters` (release — makes w visible to `counter_decrement`).
4. Reload `counter->value` (acquire). If `== target` → `w->canceled.store(true, release)`, return
   (ABA path — counter was decremented between steps 1 and 3; `counter_decrement` will discard `w`).
5. Suspend: `fiber_switch(&current_fiber->context, &scheduler_ctx)`.
6. Resume here when re-queued by `counter_decrement`. Assert `fiber->state == Ready` in debug.

`FiberState` transitions (debug builds only):
- `Active → Waiting` before `fiber_switch` in step 5
- `Waiting → Ready` by `counter_decrement` when the waiter is moved to the woken list
- `Ready → Active` after `fiber_switch` returns in step 6

### `engine/jobs/src/counter.cpp` (new)

Implements `CounterPool` lifecycle (init wires free list 0→1→…→nil, shutdown asserts no acquired
counters remain), Treiber pop/push for acquire/release, and both free functions.

### `tests/jobs/test_counter.cpp` (new)

14 test cases, 98 assertions:

| # | Test | What it checks |
|---|---|---|
| 1 | counter_pool: init and shutdown | lifecycle, double-shutdown safe, re-init |
| 2 | acquire sets value / release restores available | basic acquire/release round-trip |
| 3 | all pool_indices distinct | 8-capacity pool, each counter gets unique index |
| 4 | no waiters, value reaches zero | basic decrement with no waiters returns null |
| 5 | bulk decrement | decrement by amount > 1 |
| 6 | canceled waiter discarded | `canceled=true` waiter is not added to woken list |
| 7 | non-matching target left in waiter list | waiter with target=1 is re-pushed then returned when value reaches 1 |
| 8 | fast path when value already at target | no fiber_switch called |
| 9 | two waiters same target, both woken | both returned in woken list |
| 10 | full suspension and resumption | real `fiber_switch`; `counter_wait` suspends, `counter_decrement` wakes |
| 11 | fast path when already zero before call | decrement first, then wait — immediate return |
| 12 | two sequential waits on renewed counter | fiber waits twice on different counter acquires |
| 13 | concurrent decrements, exactly one reaches zero | 16-thread stress |
| 14 | concurrent acquire/release stress | 4-thread × 500 iterations |

Tests 10 and 12 use real fiber context switches (same pattern as `test_fiber_switch.cpp`): global
state structs with `CounterPool`, `Counter*`, `Fiber`, `Waiter`, `FiberContext`, manually wiring
`scheduler_ctx` to the test's `FiberContext` to receive the yield.

---

## Fixes also included in this session

### `CMAKE_CXX_FLAGS_RELEASE` missing NDEBUG

CMake 4.x with MSVC leaves `CMAKE_CXX_FLAGS_RELEASE` empty (unlike `CMAKE_C_FLAGS_RELEASE` which
correctly includes `/O2 /Ob2 /DNDEBUG`). This caused the Release build to compile without NDEBUG,
meaning the `#if defined(NDEBUG)` skip guard in the Vulkan triangle test was never taken. The test
then ran the full GPU path in Release LTO builds, where MSVC's LTCG optimization triggered a crash
after 15 assertions.

Fix: added to `CMakeLists.txt`:
```cmake
add_compile_definitions(
    $<$<CONFIG:Release>:NDEBUG>
    $<$<CONFIG:MinSizeRel>:NDEBUG>
)
```

### `test_rhi_vulkan.cpp` unreachable-code warning (C4702) under LTCG

With NDEBUG now defined in Release, the original `#if NDEBUG ... return; #endif` pattern caused
MSVC LTCG to emit C4702 (unreachable code) for all lines after the `return`. With `/WX` this fails
the link step.

Fix: restructured the guard to `#if NDEBUG ... SUCCEED ... #else ... full test ... #endif`, so the
Release binary never sees the GPU-path code at all.

---

## Six-configuration results

- win-debug:          331/331
- win-relwithdebinfo: 331/331
- win-release:        328/328 (3 fewer: FiberState tests excluded by `CRD_ENABLE_ASSERTS=OFF`)
- win-asan:           331/331
- win-clang-cl:       331/331
- win-tidy:           331/331
