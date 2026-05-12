# Session: crd-jobs — fiber job system hardened (4 root-caused bugs fixed)

**Date:** 2026-05-12
**Scope:** `engine/jobs/src/worker_pool.{cpp,hpp}`, `engine/jobs/src/counter.{cpp,hpp}`,
`engine/jobs/CMakeLists.txt`, `tests/jobs/test_jobs.cpp`, `tests/jobs/test_counter.cpp`,
`docs/debt.md`, `docs/jobs/WINDOWS_VERIFICATION.md` (new), `CRD/.vscode/*` (dev-env, separate)
**Type:** bug fix — closes the long-standing `linux-gcc-release` ctest flake debt and the
worker-fiber races behind it
**Platform note:** done on a native Linux dev VM (Ubuntu 26.04, 1 vCPU, SSE4.2-only CPU).
All local builds used `-DCRD_SIMD_LEVEL=sse2` (the default `auto`→`avx2` SIGILLs on this CPU).
The Windows half of the DoD sweep (MSVC / clang-cl) and AVX2 codegen were **not** run here —
see `docs/jobs/WINDOWS_VERIFICATION.md`.

## Summary

The `linux-gcc-release` "intermittent ctest flake" in `docs/debt.md` (3+ observed instances)
was masking a real crash in the test `jobs: run_and_wait from inside a worker fiber`. Diagnosed
via gdb on core dumps + a live hang; **four distinct, all pre-existing, bugs in the fiber job
system** were behind it. All four fixed; ~9,000+ aggressive stress runs now pass clean (was
~1-in-100-to-400 crash rate before the `tl_fiber` fix); full Linux ctest sweep green.

## The four bugs (and fixes)

### 1. Optimizer caches the per-thread TLS base across `fiber_switch` — `worker_pool.cpp`

`job_fiber_trampoline` reads thread-locals (the scheduler context to switch back to, the
current-fiber slot, the pool pointer) *after* the job call. A job that `jobs::wait()`s from
inside its fiber suspends and can be resumed — hence return to the trampoline — on a *different*
OS thread than the one that dispatched it. The C++ abstract machine has no notion of that
migration, so GCC at `-O3` hoisted `lea tl_sched_ctx@tpoff(%fs:0)` out of the trampoline loop
into a callee-saved register and reused it across the job call. On a cross-thread resume the
trailing `fiber_switch` then restored the *wrong* thread's saved scheduler RSP → two OS threads
on one stack → corruption (often surfacing in a later, unrelated trampoline invocation, hence the
"random" feel). MSVC was masked by the pre-existing `/Od` on `worker_pool.cpp`. Confirmed by
disassembling the optimized `worker_pool.cpp.o`.

**Fix:** `CRD_JOBS_TLS_OPAQUE` macro = `__attribute__((noipa))` on GCC, `CRD_NOINLINE` elsewhere.
Applied to `tl_scheduler_context` / `tl_current_fiber_ref` / `tl_worker_pool`; `job_fiber_trampoline`
reaches every post-resume thread-local through those — forcing the TLS base to be re-read from
the live CPU register after the job returns. `noipa` (not just `noinline`) is required so
IPA-PURE-CONST can't prove the accessor `const` and CSE two calls back together. This is the
canonical fix in the literature (LLVM #98479 / #47179 / #63022, LDC #666: "define TLS accessors
in a separate TU and call them every time"; alt is "disable TLS-address caching across opaque
calls" = the pre-existing `/Od`). Regenerated disasm confirms `&tl_sched_ctx` is now an opaque
`call` *after* the job call, no longer cached in a callee-saved register across it.

### 2. `run_job_in_fiber` leaves `tl_fiber` stale after a suspend — `worker_pool.cpp` (the one that was actually crashing)

When a fiber completes, the trampoline nulls `tl_fiber`; when a fiber *suspends*, nothing did —
`run_job_in_fiber` returned with `tl_fiber` still pointing at the parked fiber. So after a
`pump()`-driven job suspended inside `jobs::wait()`'s spin loop, the *main thread's* `tl_fiber`
was stale, and the next `jobs::wait()` on the main thread read it (`tl_current_fiber_ref()`),
took the fiber-suspend path with a garbage "current fiber" → `counter_wait` corrupted the
runtime or asserted `fiber must be Active before parking`. Caught via a core dump: the asserting
thread's backtrace was `main → jobs::wait → counter_wait`, which is impossible if `tl_fiber` is
correct (the main thread is never inside a fiber).

**Fix:** `run_job_in_fiber` clears `tl_fiber` before returning on the suspended-fiber path (the
completed path was already fine). This was the highest-impact fix — it took the stress crash rate
from ~1-in-100-to-400 to 0.

### 3. `counter_wait` published the `Waiter` before the fiber parked — `counter.cpp`

`counter_wait` ran on the fiber, published its `Waiter` onto `counter->waiters` (making the fiber
wakeable), and *then* `fiber_switch`ed to the scheduler (which is what saves the fiber's context).
A `counter_decrement` on another thread could grab the just-published `Waiter`, wake the fiber,
and enqueue a resume — which could `fiber_switch` into the fiber's not-yet-saved (stale) context.
Plus the `canceled`-bool cancel/wake handoff had a TOCTOU window: a decrement could observe a
published `Waiter` as "not canceled, target matches" before `counter_wait` had decided whether to
cancel → phantom resume queued for a fiber that then keeps running.

**Fix — switch-then-publish** (the canonical fiber-scheduler pattern; cf. FiberTaskingLib's
`CleanUpOldFiber`, Naughty Dog GDC 2015): `counter_wait` fills the `Waiter`, stashes a park
request via `tl_set_pending_park(counter, w)`, and `fiber_switch`es to the scheduler *first*
(saving its context). The scheduler — in `run_job_in_fiber`'s suspended-fiber branch, same OS
thread — then publishes the `Waiter` (`counter_finish_park`), ABA-re-checks the value, and on a
satisfied re-check resumes the fiber itself. Cancel/wake arbitration is now a single CAS on a
3-state token `WaiterClaim {Pending, Canceled, Wakeup}` (RMWs on one atomic → exactly one party
acts). A `Waiter::park_finalized` flag (set by `counter_finish_park` at its end, spun on by
`counter_wait` after the switch) keeps the resumed fiber from racing ahead and releasing the
counter / unwinding the `Waiter` while `counter_finish_park` is still touching them.

### 4. `counter_decrement`'s "steal list → partition → re-push" races concurrent decrements — `counter.cpp`

Decrement A steals the waiters list, finds a not-yet-satisfied waiter, is about to re-push it;
decrement C (the one that brings the value to 0) runs its `exchange` in that gap, sees an empty
list, does nothing; A re-pushes the waiter onto a list that no future decrement will ever drain →
lost wakeup → deadlock. This is a pre-existing bug in the original `counter_decrement` (same
`put_back` re-push loop); `run(span of N)` does exactly N concurrent decrements with one parked
waiter, so it was a real (if tiny) window.

**Fix:** the job system only ever waits for the counter to reach 0 (and a counter only decreases),
so `counter_decrement` now touches `counter->waiters` at *exactly one* point — the decrement that
hits 0 — and only ever drains it, never partially rebuilds (the `put_back` path is gone).
`counter_wait` / `counter_finish_park` `CRD_ASSERT(target == 0)` to document the contract.

## Other changes

- `engine/jobs/CMakeLists.txt`: rewrote the `/Od`-on-MSVC comment block — the root cause is now
  understood and fixed in source on every compiler; the MSVC `/Od` on `worker_pool.cpp` +
  `fiber_init.cpp` is retained as belt-and-suspenders and can be dropped after a Windows sweep.
- `tests/jobs/test_jobs.cpp`: new `jobs: cross-thread fiber resume stress` (`[jobs][stress]`) —
  4 workers × nested `run_and_wait`, unit-test-sized (~a few thousand fiber switches; the heavy
  fuzzing of this path was done out-of-suite via a standalone repro).
- `tests/jobs/test_counter.cpp`: updated for the `WaiterClaim` token, the `counter_finish_park`
  publish step, and the new "waiters list untouched until zero" behaviour.
- `Waiter` grew `claim` (`WaiterClaim` atomic, replaces the `canceled` bool) and `park_finalized`
  (atomic bool); `counter_wait`'s signature is unchanged.

## Verification

- **Disassembly:** optimized `worker_pool.cpp.o` for `job_fiber_trampoline` no longer
  materialises / caches `&tl_sched_ctx` across the job call — it's an opaque `call` to
  `tl_scheduler_context()` *after* the job returns.
- **Stress (standalone repro — 4/6/8 worker threads, 16 roots × 12 children, nested
  `run_and_wait`, ~200–400 rounds):** **0 failures across ~9,000+ runs** spanning release (`-O3`,
  no asserts), `-O2 -g` (asserts on), and `-O0 -g` (asserts on). Pre-`tl_fiber`-fix: ~1-in-100-to-400.
- `crd-jobs-tests` full `[jobs]` + `[jobs][counter]` suites: pass; `jobs: cross-thread fiber
  resume stress` ran 300× clean.
- **Full Linux ctest sweep (this VM, SSE2, asserts-on configs):** `linux-gcc-debug` 1060/1060;
  `-debug-scalar` 1060/1060; `-asan` (ASan+UBSan) 1060/1060; `-relwithdebinfo` 1060/1060;
  `-release` 1057/1057; `-shipping` 1057/1057 — all green. (1060 vs 1057 = the 3
  `#if CRD_ENABLE_ASSERTS`-gated `FiberState` tests; the +1 over the pre-fix count is the new
  `jobs: cross-thread fiber resume stress` test.)
- **Windows + AVX2 verification (follow-up, 2026-05-12 — done on the Windows dev box):** full
  14-config `scripts/full-sweep.ps1` PASS — all 9 Windows configs (MSVC debug/relwithdebinfo/
  release/asan/debug-scalar/shipping, clang-cl, clang-cl-shipping — each build+ctest+sandbox-smoke;
  tidy build-only) and all 6 Linux configs (now AVX2, since `auto`→`avx2` on the runner CPU),
  with `jobs: cross-thread fiber resume stress` passing on every config. Then a targeted AVX2
  stress on the `linux-gcc-release` (AVX2) build: the old reliable repro `crd-resources-tests
  "Eviction: load_streamed delivers correct payload via AsyncFile"` at 24-way parallel × 220
  rounds (~5,300 invocations; the old code crashed within ~28 under that load) → 0 failures;
  `jobs: cross-thread fiber resume stress` 12-way × 120 (~1,400 invocations) → 0; full `[jobs]`
  ×20 → 0. The race is gone, on AVX2 as well as SSE2. `docs/jobs/WINDOWS_VERIFICATION.md` is
  therefore satisfied; the MSVC `/Od` is dropped (see CMakeLists note + the follow-up below).

## Notes / follow-ups

- ~~The sweep has `scalar` and `avx2` SIMD lanes but no plain-`sse2` lane~~ — **closed
  2026-05-12 follow-up:** added `win-debug-sse2` + `linux-gcc-debug-sse2` config/build/test
  presets (`CMakePresets.json`), the `linux-gcc-debug-sse2` CI lane + `win-debug-sse2` CI lane
  (`.github/workflows/ci.yml`), and both to `scripts/full-sweep.ps1` (Win ×10 / Linux ×7 now).
- ~~The MSVC `/Od` on `worker_pool.cpp` + `fiber_init.cpp` is now redundant with the source
  fixes; drop it after a Windows sweep confirms~~ — **dropped 2026-05-12 follow-up** after the
  full Windows+Linux sweep + the Windows jobs stress run above confirmed the source fix holds
  without it. (`INTERPROCEDURAL_OPTIMIZATION OFF` on `crd-jobs` stays — the hand-rolled asm
  context switch is still LTO-incompatible.)
- The `CounterPool::release` debug assert-walk over leftover `Waiter`s was removed in the
  2026-05-12 follow-up — post the switch-then-publish protocol, no Pending waiter can survive
  to `release()` time (the zero-decrement drains and claims every entry), so the walk could only
  ever inspect already-unwound `counter_wait()` frames (a stack use-after-read) while asserting
  a thing that's now provably true. `acquire()` still clears `counter->waiters` for the next user.
- `scripts/wsl-build.ps1` hardened in the same follow-up (it was surfaced when the cleanup sweep
  hit a fresh `_deps` re-configure): (a) `linux-gcc-debug-sse2` added to its `[ValidateSet]`;
  (b) the `& wsl.exe -- bash …` call no longer runs under `$ErrorActionPreference='Stop'` — the
  inner bash's stderr (e.g. CMake's zstd `cmake_minimum_required` deprecation warning) was being
  surfaced by PowerShell 5.1 as a native-command error record and terminating the whole sweep on
  a benign warning. Now scoped to `Continue` around that call (restored after), trusting
  `$LASTEXITCODE` — which was always the real failure signal. (This is the long-noted
  `wsl-build.ps1` stderr brittleness from the v0e post-mortem; now actually fixed.)

## Proposed commit message

```
fix(jobs): harden the fiber job system — TLS hoist, stale tl_fiber, park protocol

Four pre-existing bugs were behind the intermittent linux-gcc-release ctest
flake (a masked crash in "jobs: run_and_wait from inside a worker fiber"):

1. job_fiber_trampoline read thread-locals after the job call; GCC -O3
   hoisted the per-thread TLS base out of the loop and reused it across the
   call, so a cross-thread fiber resume switched onto the wrong thread's
   scheduler stack. Fixed: CRD_JOBS_TLS_OPAQUE (noipa/noinline) accessors,
   trampoline reads through them.
2. run_job_in_fiber left tl_fiber pointing at the parked fiber after a
   suspend; the main thread's next jobs::wait() then took the fiber path
   with a garbage current fiber. Fixed: clear tl_fiber on the suspend path.
3. counter_wait published its Waiter before fiber_switch saved the fiber's
   context (resume could switch into a stale context; cancel/wake had a
   TOCTOU window). Fixed: switch-then-publish — fiber stashes a park
   request and switches first; the scheduler publishes via
   counter_finish_park; cancel/wake is a single WaiterClaim CAS; a
   park_finalized handshake keeps the resumed fiber from racing ahead.
4. counter_decrement's steal-partition-rebuild raced concurrent decrements
   (lost wakeup → deadlock). Fixed: drain the waiters list only at the
   decrement that reaches 0; no re-push. counter_wait asserts target == 0.

Adds a [jobs][stress] regression test. Verified: disasm; ~9000+ aggressive
cross-thread-resume stress runs across release/-O2/-O0 with 0 failures;
full Linux ctest sweep green. Closes the linux-gcc-release flake debt item.
```
