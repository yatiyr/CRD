# 2026-07-02 — The CI moat-test hang: a lost wake inside std::counting_semaphore, fixed by owning the primitive

> Session goal was "verify the v13 commit + CI, then open v14-a". The 18-config CI verdict was RED — every run
> since the v13 close timed out (>1500 s, the ctest default) on a **different** jobs-parallel determinism-moat test
> (v6-c Arnoldi / v6-h sparse SVD / v7-b FD-gradient / v7-f CG), only on Linux configs (gcc-release / shipping /
> asan). Per "never defer DoD failures", v14 was blocked until this was root-caused and fixed.

## The evidence chain

1. **CI triage:** the timed-out tests normally run in **~0.02 s** (verified from the same runs' logs where the
   previously-failing test passed). A millisecond test hanging >1500 s is a genuine hang, not slowness (the
   `feedback_timeout_is_not_a_hang_proof` scar applied — but this time with the CPU-climb check done properly).
2. **Local repro (SANITY #5 — measure, don't theorize):** a WSL harness (`scripts/repro_moat_hang.sh`) running both
   moat binaries pinned to 4 CPUs (`taskset -c 0-3`, mimicking the 4-vCPU CI runner) reproduced the hang at
   iterations 46, 16, 76 and 6 of the `[moat]` tag sets. Crucially, the hung process's **CPU ticks over 5 s = 0** —
   fully parked, not spinning. That single number eliminated the `jobs::wait` spin-pump, all solver code, and any
   livelock: only two blocking points exist in the whole system (worker `m_semaphore.acquire()` and shutdown's
   `join()`).
3. **Stacks (gdb attach after `yama/ptrace_scope=0`):** main thread in `WorkerPool::shutdown() → std::thread::join()`;
   the sole remaining worker in `worker_loop → std::__atomic_semaphore::_M_acquire → __atomic_wait_address_bare →
   futex`. Only 2 threads alive ⇒ the other workers had exited and joined; one worker missed its shutdown wake.
4. **The kill shot (futex forensics):** `/proc/<tid>/syscall` gave the futex word address — it resolved to
   `g_pool+48` = the scheduler semaphore's `_M_counter` — and the worker's futex `expected` argument was **1**.
   `gdb x/dw` on the word read **1**. The worker was asleep *expecting the exact positive value that should have
   woken it*, with the token sitting there and no future release ever coming. `wait_for_work` protocol above the
   semaphore is provably sound with a conforming counting semaphore; the primitive itself lost the wake.

## The mechanism (named in one sentence, SANITY #1)

GCC 13.3's libstdc++ (`std::counting_semaphore`, the exact toolchain of the ubuntu-24.04 CI runner) can put a
waiter to sleep with a stale positive futex-expected while a token is present: `_S_do_spin` loads the futex
`expected` value **before** running the 16-iteration acquire-predicate spin
(`/usr/include/c++/13/bits/atomic_wait.h:356`), and `__atomic_semaphore::_M_release` **skips the futex wake
entirely** when the counter was already > 0 (`bits/semaphore_base.h`, which carries a
`FIXME - Figure out why this does not wake a waiting thread`). Under oversubscription + a push storm the two
combine into a sleep that no wake can ever end — the GCC PR104928 class ("std::counting_semaphore on Linux can
sleep forever"; same family as PR100806). Windows/MSVC has a different implementation ⇒ Linux-only; any config;
whichever moat test happened to be cycling `jobs::init/shutdown` when the window hit ⇒ a different test each run.

## The fix (root, engine-side): own the primitive

`crd::jobs::detail::Semaphore` (`engine/jobs/src/semaphore.{hpp,cpp}`) — a worker-sleep semaphore built directly
on `futex` (Linux) / `WaitOnAddress` (Windows) with the canonical **expected == decision-value** protocol:

- `acquire()` sleeps only with `expected == 0`, and only after the CAS-drain loop *observed* the count at 0
  (a failed CAS reloads and retries — it can never fall through to sleep on a positive observation). The kernel
  re-checks `word == 0` atomically against concurrent releases, closing the pre-sleep window by construction.
- `release()` **always** wakes (single for n==1, broadcast otherwise) — the skip-when-positive optimization is
  exactly what loses wakes, so it is banned by design.
- `try_acquire_for_ms()` (same protocol, timed) serves the ADR-0094 targeted-wake backstop.

Both scheduler semaphores swapped (`Scheduler::m_semaphore` + `ThreadState::wake`); `<semaphore>`/`<chrono>`
includes dropped; `synchronization.lib` linked on Windows for `WaitOnAddress`. crd-jobs now hand-rolls **all** of
its concurrency primitives (fibers, Chase-Lev deque, Vyukov MPMC, counter park/wake, and now the sleep semaphore) —
the last std concurrency dependency in the hot path is gone, on every toolchain identically.

## Verification

- **New boundary-adversary tests** (`tests/jobs/test_semaphore.cpp`, 4 cases / 60 assertions): token accounting,
  bounded timed waits, a 4-consumer × 50-round lost-wake race hammer, and the end-to-end moat-pattern regression
  (40 × jobs init/shutdown cycles across `{1,2,4,8,16}` workers with a parallel_for landing just before shutdown —
  the exact CI-hang shape). Green on linux-gcc-release and win-debug (MSVC/WaitOnAddress path).
- **The repro loop that hung at iterations 6/16/46/76 pre-fix ran 300 clean iterations post-fix** (both binaries
  concurrently, 4 pinned CPUs — ~40× the pre-fix mean-hang distance).
- Full `crd-hesap-opt-tests` + `crd-hesap-eigen-tests` suites green on linux-gcc-release; full jobs suite green on
  linux-gcc-release + win-debug.
- The user's next commit + 18-config CI sweep is the final gate (the flake was ~1 hit per CI run pre-fix, so a
  green sweep is meaningful signal).

## Files

- `engine/jobs/src/semaphore.hpp` / `semaphore.cpp` — NEW: the futex/WaitOnAddress semaphore.
- `engine/jobs/src/scheduler.hpp` / `scheduler.cpp` — both semaphores swapped; includes trimmed.
- `engine/jobs/CMakeLists.txt` — `synchronization` link on Windows.
- `tests/jobs/test_semaphore.cpp` — NEW: the 4 regression/adversary cases.
- `scripts/repro_moat_hang.sh` — the repro + forensics harness (tracked; rerun after any crd-jobs sleep/wake change).

## Proposed commit

```
fix(jobs): replace std::counting_semaphore with a futex/WaitOnAddress semaphore — libstdc++ lost wakes hung CI

GCC 13.3's counting_semaphore can sleep a waiter with a stale positive futex
expected while a token is present (_S_do_spin preloads expected before the
predicate spin; _M_release skips the wake when the counter was already >0 —
the PR104928 class). Intermittently hung the jobs-parallel determinism-moat
tests at shutdown()'s join() on Linux CI (>1500s ctest timeouts, a different
test each run). Proven by live futex forensics: worker asleep on the scheduler
semaphore with expected==1 while the counter word read 1.

crd::jobs::detail::Semaphore sleeps only with expected==0 observed by the CAS
drain loop and always wakes on release. Both scheduler semaphores swapped;
synchronization.lib linked on Windows; 4 boundary-adversary regression tests;
the 4-CPU-pinned repro loop that hung by iteration <=76 ran 300 clean.
```
