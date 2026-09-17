# DIAG.5b — Linux crash and hang capture (DG09): plan, decomposition and acceptance

<!-- doc-role: historical -->

Owner slice: [DIAG.5b](../ROADMAP.md#slice-diag.5b). Contract: [runtime-diagnostics
design](../design/runtime-diagnostics.md#diag-5b); [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md).
DG09 (fatal-path reliability) — the Linux half of what DIAG.5a delivered on Windows. **Full slice doc: (a)–(g)
complete; row 074 flipped Open→Needs CI (see the "DIAG.5b (g)" acceptance review at the end).** It opens with the
confirmed mechanism, the original defects of the Linux branch against the acceptance, and the local WSL verification
regime, then documents each sub-unit's landed work; the closing (g) section is the clause-by-clause acceptance table.
The engine changes were (b) the hardened handler, (c) the concurrent-fault gate, and (d) the per-worker alt-stack
wiring; (e)/(f)/(g) are all test/specimen code.

## Acceptance (verbatim from the design)

> Use minimal async-signal-safe recording or a qualified external collector. Install and manage alternate signal
> stacks for every relevant OS thread; collect original registers, signal and process/thread identity without
> allocating, formatting or taking general locks in the handler. Handle installation failure, recursive faults, signal
> chaining, core-policy permissions and normal uninstall. Symbolization/backtraces occur outside compromised
> execution.
>
> Acceptance: faults on worker/fiber stacks, exhausted stacks, logger/allocator lock ownership, secondary signals,
> unwritable paths and missing core collector. SIGKILL/OOM-killer cases retain previous records or explicitly report
> no final capture; do not promise a signal handler for uncatchable termination. No global sysctl/security changes.

## Design vs ADR — the scope that governs the eventual flip

Same resolution as 5a: [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md) §112–120 withholds any
external/paid/newly-installed collector (a separate future user decision), so DIAG.5b is the **in-tree
sigaction/backtrace path hardened** — not a new collector. The Linux artifact is the design's "minimal
async-signal-safe recording": a `.log` carrying the signal, `si_code`/`si_addr`, original registers, thread identity
and the executable path. The call stack is **not** walked in the handler (see the (b) correction below) — it is
recovered offline from the OS core; symbolization is explicitly out of the handler ("outside compromised execution")
and is DIAG.5d. There is **no minidump equivalent** on Linux — `capture_dump`/`read_dump_stream` stay
`Unsupported`/`0` here by design (the OS core dump is the heavy artifact; the crd handler adds the async-signal-safe
record and honest termination). The flip note must say this so it does not claim a collector the ADR withholds.

## Current Linux branch — what exists, and its defects vs the acceptance

[`crash.cpp`](../../engine/foundation/core/src/crash.cpp) lines 722–898 install
`sigaction(SIGSEGV/SIGABRT/SIGFPE/SIGILL, SA_SIGINFO|SA_RESETHAND)` and, in the handler, `mkdir` + `snprintf` a path,
`open(O_TRUNC)`, `write` a one-line header, `backtrace`+`backtrace_symbols_fd`, fire the report hook, then restore
`SIG_DFL` and `raise(sig)`. It terminates with the original signal (good — the harness reads the reason from
`WTERMSIG`), but against the acceptance it is unhardened:

1. **No alternate signal stack (no `sigaltstack`, no `SA_ONSTACK`).** A stack-exhaustion fault has no room to run the
   handler → no record. Acceptance: "exhausted stacks", "alternate signal stacks for every relevant OS thread". This
   is the headline gap and the reason 5b is not just a Windows port.
2. **Non-async-signal-safe work in the handler.** `mkdir` and `snprintf` run inside the handler; `snprintf` is not on
   the POSIX async-signal-safe list. Acceptance: "without allocating, formatting or taking general locks."
3. **Stack walked/symbolized in compromised context.** `backtrace`+`backtrace_symbols_fd` take the loader lock and
   can malloc (first call `dlopen`s `libgcc_s`). Design: "Symbolization/**backtraces** occur outside compromised
   execution" → the handler must **not** call `backtrace` at all; the stack comes from the OS core, offline (5d). This
   corrects the (a) plan, which had suggested writing raw frame addresses / pre-warming `backtrace` (see (b) below).
5. **Original registers dropped.** The handler ignores `ucontext_t` (`void* ctx`). Acceptance: "collect original
   registers" (PC/SP at minimum, arch-gated `REG_RIP`/`pc`).
6. **No concurrent-fault gate.** Two faulting threads both enter and interleave writes / double artifacts. Needs a
   lock-free atomic gate; the loser bounded-waits then re-raises.
7. **Filenames collide and clobber.** `crash_pid%d_sig%d.log` opened `O_TRUNC` — two faults with the same pid+signal
   overwrite. Needs `O_CREAT|O_EXCL` + a serial (the async-signal-safe analog of Windows `CREATE_NEW`).
8. **Thin identity.** `report.faulting_tid = 0`; no `si_code`; no executable path/build-id. `gettid()` is
   async-signal-safe; the exe path (`readlink /proc/self/exe`) can be cached at install; GNU build-id note parsing is
   the "correctly identified binary" analog and is deferred to 5d with symbolization.
9. **`mkdir` in the handler, not at install.** The dir is created (again) on every fault. Create once in `install()`.
10. **Core-policy handling absent.** The re-raise writes an OS core; on CI that is slow and noisy. The design forbids
    **global** sysctl/security changes, so the fix belongs in the **specimen** (per-process `setrlimit(RLIMIT_CORE,
    {0,0})` in a Linux `crd_diag_harden()` branch — which today is Windows-only), never a system-wide change.

## Verification regime (decided this tick, with evidence)

Not "CI-qualified by construction" — this machine has a usable Linux toolchain:

- **WSL Ubuntu 24.04, g++ 13.3.0, glibc 2.39** (`wsl -l -v` → Ubuntu Running; `g++/clang++/cmake/gdb` all present).
  Configured Linux build trees already exist under `build/linux-gcc-debug`, `build/linux-gcc-asan`, etc.
- **Confirmed this tick:** the current Linux branch of `crash.cpp` `-fsyntax-only`s clean under g++ 13.3
  (`-Iengine/foundation/core/include -Ibuild/linux-gcc-debug/engine/core/include`). So every 5b engine tick is
  locally syntax-checkable, and (via the existing tree) buildable and **runnable** under WSL — real death-test
  evidence, not just CI.
- **Harness status mapping** (`specimen_runner.cpp` non-Windows branch): `WIFSIGNALED` → `crashed = true`,
  `exit_code = 128 + WTERMSIG`. So the Linux acceptance codes are `SIGSEGV`→**139**, `SIGABRT`→**134**, `SIGFPE`→**136**,
  `SIGILL`→**132** — the analog of the Windows NTSTATUS assertions. `WIFEXITED` → `WEXITSTATUS` (clean/instrument
  codes unchanged).
- **CI lanes:** the cached push lanes are `win-debug linux-gcc-debug` (asserts on), so 5b qualifies on **every push**
  via `linux-gcc-debug`; the complete/nightly tier adds `linux-gcc-asan` and `linux-clang-tsan` for the sanitizer
  interaction below.

## Gotchas that will bite blind Linux code (desk-checked; one confirmed)

- **`SIGSTKSZ` is not constexpr on glibc ≥ 2.34 — CONFIRMED this tick:** `char a[SIGSTKSZ];` fails "size of array is
  not an integral constant-expression" under g++ 13.3/glibc 2.39. The alt stack must use a fixed constant (≥ 64 KiB)
  or `sysconf(_SC_SIGSTKSZ)` + a heap allocation in `install()` — never a `static char[SIGSTKSZ]`.
- **`sigaltstack` is per-thread.** `install()`'s alt stack covers only the installing thread; a worker overflow with
  no alt stack cannot run the handler. So Linux `guard_current_thread_stack()` = register a per-thread alt stack, and
  (unlike 5a, which has a separate handler thread) a stack-exhaustion capture on a worker **requires** the worker to
  have called it — the "guard unnecessary" 5a finding does not transfer.
- **Async-signal-safe only:** `write`, `open(O_WRONLY|O_CREAT|O_EXCL)`, `close`, `fsync`, `getpid`, `gettid`,
  `clock_gettime`, `sigaction`, `raise`, `nanosleep`. No malloc/`snprintf`/`std::string`/locks. Mirror the Windows
  `emit_*` helpers over `write(2)` (integer→decimal/hex by hand).
- **Original context** needs `_GNU_SOURCE` before includes and an `#if defined(__x86_64__)` / `__aarch64__` gate for
  `REG_RIP` / `uc_mcontext.pc`.
- **Terminate with the original signal:** chain to the previous handler then `raise(sig)` — never `_exit(1)` (that
  would destroy "retains original fault reason", which the harness reads from `WTERMSIG`). Do **not** set `SA_NODEFER`,
  so a handler self-fault is masked → kernel kills → bounded. (This corrects the (a) plan's suggestion of
  `SA_RESETHAND`, which (c) proved harmful under concurrent faults — see the (c) section.)
- **ASan on Linux is the OPPOSITE of Windows:** `allow_user_segv_handler` defaults true, so our `sigaction` **replaces**
  ASan's SEGV handler — a null deref on the `linux-gcc-asan` lane writes **our** report and ASan stays silent (on
  Windows ASan's VEH preempted us). Assert per lane; expect one CI adaptation cycle. TSan (`linux-clang-tsan`) likewise
  installs its own handlers — reason per lane.
- **SIGKILL / OOM-killer are uncatchable** — the acceptance explicitly says do not promise a handler for them; the
  test asserts "no final capture" honestly rather than pretending.

## Per-tick decomposition

- **(a) [this tick]** census, acceptance extraction, regime decision, decomposition. No engine code; row 074 Open.
- **(b) [DONE — see the "(b)" section below]** core hardened handler in `crash.cpp`'s `#elif defined(__linux__)` branch
  (no new file, no CMake change): install-time per-thread alt stack (`SA_ONSTACK`, fixed 64 KiB), dir created once + dir
  and exe path cached at install, `O_EXCL`+serial collision-safe filenames, async-signal-safe formatting over `write`,
  **no stack walk** (registers + signal + identity only; the stack is the OS core, offline), original registers
  (arch-gated x86_64/aarch64), `si_code`/`si_addr`/`gettid` identity, honest `WriteResult` (Ok only after
  open+write+`fsync`), chain to the previous handler then re-raise; `uninstall` restores handlers and disables the alt
  stack. Verified under WSL (see the (b) section).
- **(c) [DONE — see the "(c)" section below]** concurrent-fault gate (lock-free atomic; bounded loser wait then chain) +
  recursive/secondary-fault handling. Corrects (b): `SA_RESETHAND` is **removed** (it resets disposition process-wide,
  killing a concurrent faulter mid-record); the four fault signals are masked in `sa_mask` so a recursive fault is
  bounded to `SIG_DFL`.
- **(d) [deferred one tick — blocked, now unblockable next]** per-thread alt-stack registration = Linux
  `guard_current_thread_stack()`; worker-pool wiring so a worker/fiber stack-exhaustion fault is captured (required
  here — no separate handler thread). The wiring itself is one line in `worker_loop`, but verifying it needs the
  crash-capture specimen to *build* under WSL, which surfaced a pre-existing `linux-gcc-debug` `-Werror` chain in
  committed DIAG code (missing-field-initializers from DIAG.4c's `exhaustions` field, fixed this tick — see the
  [4c session doc](2026-09-15-diag-4c-hang-starvation-rt.md); and a `class-memaccess` in `diagnostic_allocator.cpp`
  upstream, reported for a dedicated triage). Landed this tick as (d) prep: the specimen's `fiber_overflow` mode (a job
  that overflows its fiber stack) and the `recurse()` GCC `-Winfinite-recursion` pragma (the Linux equivalent of the
  MSVC C4717 suppression). Preflight confirmed the discriminator is viable: fiber stacks are guard-paged (64K/512K/2M
  tiers via `mmap(PROT_NONE)`+`mprotect`), so a fiber overflow is a clean SIGSEGV. **Update (2026-09-15): the Linux
  build is now unblocked** (the crd-memory `class-memaccess` + `count_dumps` `-Werror` sites were fixed — see the
  [4c doc](2026-09-15-diag-4c-hang-starvation-rt.md)), and the specimen builds and runs under WSL. **Done — see the
  "DIAG.5b (d)" section below.** (An earlier "pre-wiring baseline" note here claimed `fiber`/`fiber_overflow` → 0
  records; a clean re-measurement on a native `/tmp` dir showed that was a **harness artifact** — those modes are
  captured because `jobs::wait()` pumps the job onto the guarded main thread. The true worker-thread discriminator is
  the `fiber_overflow_worker` mode, measured below.)
- **(e)** specimen + harness: Linux `crd_diag_harden()` (own `RLIMIT_CORE` → 0), a Linux `denied` (rmdir), and the
  `test_diag_crash_capture` Linux acceptance block asserting `exit_code == 128 + signo`, verdict `Crashed`, and the
  on-disk record present/absent (+ `tid != pid`, si_code) per mode. **Done — see the "DIAG.5b (e)" section below.**
- **(f)** unwritable-path + missing-core-collector + SIGKILL/OOM honesty + install-failure + uninstall tests; the
  record-content read-back (signal/registers/addresses present) — the 5b analog of 5a's `read_dump_stream` check.
  **Done — see the "DIAG.5b (f)" section below.**
- **(g)** acceptance review + flip row 074 Open→Needs CI with a Session link + the latest actions/runs URL.
  **Done — see the "DIAG.5b (g)" acceptance review at the end; row 074 is now Needs CI.**

Row 074 stays **Open** ((a) and (b) landed; the slice is not fully done until (g)).

## DIAG.5b (b) — the core hardened Linux handler

All changes are inside `crash.cpp`'s `#elif defined(__linux__)` branch: **`crash.hpp` is untouched**, so on Windows this
is only a recompile of `crash.cpp` with the Linux block preprocessed out (regression re-checked below). What landed,
against the (a) defect list:

- **Per-thread alternate signal stack.** `guard_current_thread_stack()` is now the registrar: it `sigaltstack`s a
  fixed 64 KiB `alignas(64) thread_local` buffer (`SIGSTKSZ` is not constexpr on glibc ≥ 2.34 — confirmed in (a) — so
  a static-sized buffer cannot use it; `.tbss`, no malloc/free). `install()` calls it for the installing thread and
  sets `SA_ONSTACK`; `uninstall()` `SS_DISABLE`s it. Every thread that may fault must call it (there is no separate
  handler thread as on Windows, so the 5a "guard unnecessary" finding does not transfer) — worker/fiber wiring is (d).
- **Handler is registers + signal + identity only — no stack walk.** Per the acceptance ("Symbolization/**backtraces**
  occur outside compromised execution"), `backtrace`/`backtrace_symbols_fd` and `<execinfo.h>` are **removed** (they
  take the loader lock and can malloc). This **corrects the (a) plan**, which had said to write raw frame addresses and
  pre-warm `backtrace`; neither is done. The record carries `si_signo`/`si_code`/`si_addr`, `getpid`/`gettid`, the
  cached exe path, and the full original register file from `ucontext_t` (x86_64: all 23 `gregs` labelled;
  aarch64: `regs[0..30]`+`sp`+`pc`, arch-gated). The stack is recovered offline from the OS core.
- **Async-signal-safe throughout.** `mkdir`, `snprintf` and the CRT are gone from the handler; the dir is created once
  in `install()`, the exe path cached there via `readlink("/proc/self/exe")`, and the record is formatted by hand
  (`ap_dec`/`ap_hex`/`ap_str`) into a stack buffer then written with a short-write/EINTR-safe `write_all` + `fsync`.
- **Collision-safe filenames.** `crash_<pid>_<tid>_<CLOCK_MONOTONIC ns hex>_<serial>.log` opened `O_CREAT|O_EXCL` in a
  retry loop — the async-signal-safe analog of Windows `CREATE_NEW`; two faults never clobber.
- **Honest result + chaining.** `WriteResult` is `Ok` only after open+write+`fsync` all succeed (else `OpenFailed`/
  `DumpFailed`, partial `unlink`ed); the stderr line is `crash record: <path>` only on `Ok`, else `crash record FAILED
  (errno N)` — a failed write cannot print success (mirrors Windows). `dump_path` stays `nullptr` (a Windows wide
  minidump path); the record path goes to stderr. After the record the handler **chains** to the previously-installed
  handler (`sigaction(sig, &prev); raise(sig)`) so a default prev terminates with the original signal (the harness
  reads `WTERMSIG`) and a sanitizer's prev also reports — the acceptance's "signal chaining", and it makes the
  `linux-gcc-asan` reasoning clean (ASan's `allow_user_segv_handler` default means our handler runs first, then chains).

**Verification (real, under WSL — Ubuntu 24.04, g++ 13.3.0, glibc 2.39):**
- `crash.cpp` compiles clean under the **exact CI flag set** `-Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow
  -Wdouble-promotion -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference -Wformat=2` (one real fix found: with
  `thread_local`, `alignas(64)` must precede the storage-class specifier).
- A death-test probe linked against the built object (`ulimit -c 0`, the specimen-side `RLIMIT_CORE` the (a) doc
  called for) produced, one process per fault:

  | mode | logs written | evidence |
  | --- | --- | --- |
  | `av` (null write) | **1** | `signal 11 code 1 addr 0x0`, `exe /tmp/probe`, full x86_64 gregs (`rax`/`cr2` = 0x0) |
  | `av` twice → one dir | **2** | distinct filenames — `O_EXCL` collision-safety |
  | `overflow` (main thread) | **1** | main-thread stack exhaustion captured — the `SA_ONSTACK` payoff (would be 0 without it: the handler cannot run on an exhausted stack) |
  | `thread_overflow` (worker) | **1** | record shows `tid ≠ pid` — per-thread alt-stack design validated early |
  | `denied` (dir removed) | **0** | `crash record FAILED (errno 2)` (ENOENT) — honest `OpenFailed`, no partial |

- Windows regression: `crd-core` + `crd-core-tests` rebuilt win-debug (crash.cpp recompiled, Linux block out);
  `[crash]`+`[crash-capture]` still **24 / 121**. win-asan not needed (no header change, no Windows-code change).
- Slice-tag grep on `crash.cpp`: clean. Both validators: PASS. No repo-root scratch (the probe lives in WSL `/tmp`).

Row 074 stays **Open** ((a)–(c) landed; the slice is not fully done until (g)).

## DIAG.5b (c) — concurrent-fault gate + recursive/secondary faults

Still all inside `crash.cpp`'s `#elif defined(__linux__)` branch (`crash.hpp` untouched). Two things landed, and the
first **corrects (b)**:

- **`SA_RESETHAND` removed — the crux.** `SA_RESETHAND` resets the signal's disposition to `SIG_DFL` **process-wide** on
  handler entry. So when thread B faults with the same signal while thread A is mid-record, B takes the *default*
  action and tears the process down — A's record is truncated and its `O_EXCL` file is never cleaned up. A gate alone
  cannot fix this (B's fault never reaches the handler). The handler is now `SA_SIGINFO | SA_ONSTACK` with the four
  fault signals **blocked in `sa_mask`** for the duration: a *recursive* fault on the handling thread is forced to
  `SIG_DFL` (bounded termination, no re-entry), while another thread's fault still reaches the handler and hits the
  gate (the mask is per-thread). The chain step already restores the previous handler before `raise`, so removing
  `RESETHAND` does not change chaining.
- **Single-shot gate.** Two lock-free atomics (`s_gate`, `s_done`; `static_assert`ed lock-free, reset in `install()`
  incl. the re-install path and in `uninstall()`). The winner CASes `s_gate` 0→1, writes the one record, fires the
  hook, then `s_done.store(release)`. A concurrent loser bounded-waits (`≤ 5 s`, 1 ms `nanosleep`) for `s_done` — so
  the winner's record has landed before anyone chains and tears the process down — then reports `Suppressed` and
  chains. `s_done` is set *after* the winner's hook, so a *hung* winner hook (a fault in the hook is already bounded by
  the mask) still yields bounded termination: the loser waits its 5 s bound then chains, and the record is already on
  disk. The record writer was extracted to a helper (`write_crash_record`) so the gated handler stays readable.

**Verification (WSL — Ubuntu 24.04, g++ 13.3.0, glibc 2.39):**
- `crash.cpp` strict compile clean under the full CI flag set (`-Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow
  -Wdouble-promotion -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference -Wformat=2`).
- **A/B on a `concurrent` probe (two threads fault simultaneously), 12 runs each**, counting runs that produce exactly
  one *complete* record (its last register line `cr2` present, i.e. not truncated):

  | build | exactly-one-complete | anomalies |
  | --- | --- | --- |
  | (b) `SA_RESETHAND`, no gate | **6 / 12** | 6 runs wrote a *truncated* record (process torn down mid-write) |
  | (c) masked signals + gate | **12 / 12** | 0 |

  The pre-fix flakiness is the exact failure the correction addresses; the post-fix is deterministic.
- **Single-fault / recursive modes against (c)** (one run each, 20 s timeout guard): `av`, `overflow`,
  `thread_overflow` → exit 139, 1 complete record (the mask change did not regress single-fault or alt-stack capture);
  `denied` → exit 139, 0 records (honest `OpenFailed`); **`hook_fault`** (the crash-report hook itself `null_write`s)
  → exit 139, 1 complete record, **no hang** — the recursive fault is bounded by the mask, and the record (written
  before the hook) is intact.
- Windows regression: `crd-core`+`crd-core-tests` rebuilt win-debug; `[crash]`+`[crash-capture]` still **24 / 121**.
  Slice-tag grep on `crash.cpp`: clean. Both validators: PASS. No repo-root scratch (probes/scripts live in WSL `/tmp`
  and the session scratchpad).

Row 074 stays **Open** ((e)–(g) remain; see the (d) section below).

## DIAG.5b (d) — worker/fiber alternate-stack wiring

A job runs on a **fiber stack** that `worker_loop` switches into from the bare OS (scheduler) thread
(`worker_pool.cpp` `run_job_in_fiber` → `fiber_switch`; the comment at the suspend path confirms the caller is "back
on the bare OS thread"). `install()` gives its alternate signal stack only to the **installing (main) thread**
(`crash.cpp:1108`), so a fiber-stack overflow that faults **on a worker OS thread** finds no alt stack and the handler
cannot run — the fault is lost. The fix is one registration per worker: `crd::crash::guard_current_thread_stack();` at
`worker_loop` entry (after the `tl_idx/tl_pool_ptr/tl_frame_arena` TLS setup, before the loop). `sigaltstack` is
per-OS-thread, so one call covers **every** fiber that worker ever runs. No sigmask is involved anywhere in `jobs`/
`core` (grep-confirmed: no `sigmask`/`sigprocmask`/`SIG_BLOCK`), so signal delivery on the worker was never masked —
the sole gap was the missing alt stack.

**Correcting the earlier baseline.** A prior note claimed `fiber`/`fiber_overflow` produced 0 records pre-wiring. A
clean re-measurement (native `/tmp` dir, script file — not the `bash -c` harness whose `$()`/reaper noise corrupted the
earlier count) shows all of `av`, `fiber`, `fiber_overflow` produce **1** record even pre-wiring, but with **`pid ==
tid`**: `jobs::wait()` **pumps** on the main thread (`jobs.cpp:308 if (is_main && g_pool.pump())`), so the specimen's
main OS thread runs the fiber and the fault lands on the guarded main thread. Those modes therefore never exercised a
worker. The true worker-thread proof needs the fault forced onto a background worker.

**New specimen mode `fiber_overflow_worker`.** `init(num_threads = 2)` (main + one background worker), `run(j)`, then
main **sleeps** (never `wait()`/`pump()`), so only the worker can run the job; its fiber stack overflows on the worker
OS thread. Bounded fallback exit `61` if (never expected) no fault occurs, so the harness cannot hang.

**Before/after (WSL — Ubuntu 24.04, g++ 13.3.0, glibc 2.39; native `/tmp`), same specimen source, only the
`worker_loop` wiring changed:**

| build | `fiber_overflow_worker` | evidence |
| --- | --- | --- |
| **before** (no `worker_loop` wiring) | exit 139, **0 records** | worker faults (SIGSEGV, `core dumped`) but no alt stack → handler cannot run |
| **after** (`guard_current_thread_stack()` in `worker_loop`) | exit 139, **1 record** | `pid 143401 tid 143402` (**tid ≠ pid** — a genuine background worker), `signal 11 code 2` (SEGV_ACCERR = guard-page hit), `addr` high in the mmap'd fiber-stack region (a real stack overflow, not a null deref) |

The 0 → 1 flip on an otherwise-identical build is (d)'s discriminator.

**Verification:**
- Linux `-Werror` (full CI flag set): `worker_pool.cpp` recompiles clean; `crd-jobs.a`, `crd-jobs-tests` and the diag
  specimens relink clean (0 `error:`/`FAILED:`).
- Windows regression (win-debug): `crd-jobs-tests` **172 / 29686**; `crd-core-tests` **26 / 133** (incl. `[crash]`
  14 / 78, `[crash-capture]` 10 / 43) — the `worker_loop` call is `SetThreadStackGuarantee` on Windows and did not
  change the tested Windows `fiber` limitation (still 0 dumps: capturing fiber/CET faults there needs a VEH — 5d).
- Slice-tag grep on `worker_pool.cpp`: clean. Both validators: PASS. No repo-root scratch (measurements in WSL `/tmp`).

Row 074 stays **Open**: (e) specimen/harness Linux modes + wired assertions (this tick added the `fiber_overflow_worker`
mode but no Catch2 test yet — the CI-run assertions are (e)), (f) unwritable/SIGKILL/record-readback, (g) acceptance
review then flip. Next tick begins with the protocol's advisor + CI check.

**Carry-forward for (e)/(g):**
- **(e) — assert `tid != pid` from the record, not just `count == 1`.** `fiber_overflow_worker` relies on the worker
  (not main) running the job. Today `jobs::run()` from main enqueues and a background worker steals it, so the fault
  lands on the worker (`tid != pid`, verified above). But if a future scheduler change ever ran `run()` inline on the
  calling thread, the fault would land on the guarded main thread → `count == 1` with `pid == tid` — a green that
  proves nothing about workers. The (e) Catch2 assertion must read the record's tid and require `tid != pid` (the
  fallback exit `61` already guards the no-fault direction).
- **(g) — note the worker-stack cost of the wiring.** On Windows `guard_current_thread_stack()` is
  `SetThreadStackGuarantee`, which reserves its guarantee (64 KiB) from each worker's committed stack, so every worker
  loses that much usable stack. Nothing regressed (172/172), but deep-recursion-on-a-worker paths (fiber-exhaustion
  specimen, worker snapshot) now have slightly less headroom; call this out in the (g) acceptance review as a known
  cost, since worker stack size is a tunable `Config` field.

## DIAG.5b (e) — Linux specimen hardening + crash-capture acceptance tests

Turns the (a)–(d) Linux capture into CI-enforced acceptance on the `linux-gcc-debug` lane. All changes are in
**test/specimen code** (`tests/…`), no engine source:

- **`crd_diag_harden()` Linux branch** (`specimen_common.hpp`): `setrlimit(RLIMIT_CORE, {0,0})` so a crashing specimen
  writes no kernel core file — the crd crash record is the artifact of record, and a core only costs time and litters
  the tree. Verified: after `av` in an empty cwd, **0** `core*` files.
- **Linux `denied` mode** (`crash_capture_specimen.cpp`): the dir-removal was `#if defined(_WIN32)` only, so on Linux
  `denied` never denied (it measured identical to `av`, 1 record). Added `#elif defined(__linux__) ::rmdir(dir);`
  (mirroring the Windows `RemoveDirectoryA`) so the record open fails with ENOENT → `OpenFailed`. Verified: dir gone,
  **0** records.
- **`test_diag_crash_capture.cpp` restructure**: hoisted the platform-neutral helpers (`built_with_asan`,
  `specimen_path`, `fresh_dir`, `run_mode`) above the `#if defined(_WIN32)`; the Windows-only `.dmp`/NTSTATUS/marker
  helpers stay inside it; added a `#elif defined(__linux__)` acceptance block with `count_crash_records`/
  `first_crash_record` (prefix `crash_`, ext `.log`) and a 4-field record parser (`pid`/`tid`/`signal`/`code`). Same
  file → no test-registration change; `add_dependencies(crd-core-tests crd-diag-crash-capture-specimen)` and the
  `$<TARGET_FILE:…>` macro (no `.exe` on Linux) were already present, so CI builds the specimen before the tests run.

**Every assertion was MEASURED against the built specimen before being written** (native `/tmp`, script file — not the
mangling `bash -c` harness). The Linux table, with the platform differences called out:

| mode | exit | records | asserted evidence | vs Windows |
| --- | --- | --- | --- | --- |
| `av` | 139 | 1 | `signal 11`, `pid == tid` (main-thread fault) | same shape (record vs dump) |
| `overflow` (main) | 139 | 1 | `signal 11` — main alt stack from `install()` | same |
| `overflow_thread` (unguarded) | 139 | **0** | dir **exists**, no record — honest negative | same (0) |
| `overflow_thread_guarded` | 139 | 1 | `tid != pid`, `code 2` (SEGV_ACCERR) | same |
| `fiber_overflow_worker` | 139 | 1 | `tid != pid`, `code 2` — the (d) proof | n/a (Linux-specific) |
| `fiber` (null-deref) | 139 | 1 | captured (`wait()` pumps onto guarded main) | **opposite**: Windows 0 (needs VEH) |
| `concurrent` | 139 | 1 | exactly one (single-shot gate) | same |
| `denied` | 139 | 0 | dir **absent** (rmdir'd) | same |
| `fastfail` (`__builtin_trap`) | **132** | 1 | `signal 4` (SIGILL) — crd installs for SIGILL | **opposite**: Windows 0 (`__fastfail` bypasses SEH) |

The `overflow_thread` (0, dir present) vs `denied` (0, dir absent) pair distinguishes "handler never ran" from "open
failed" by dir existence — the same discriminator the Windows block uses, so no Linux marker is needed (the marker
stays Windows-only). ASan: each case guards with `built_with_asan()` → `CHECK(exit != 0)` only (on Linux crd's
`sigaction` replaces ASan's own SIGSEGV handler, so records under ASan are uncharacterized — there is no linux-asan
tree here); the full record assertions run on `linux-gcc-debug`. Never skip-as-pass.

**Verification:**
- Linux (`linux-gcc-debug`, `-Werror`): specimen + `crd-core-tests` build clean (0 `error:`/`FAILED:`);
  `crd-core-tests "[crash-capture]"` → **9 / 49** (all Linux cases); harden 0 core files; `denied` dir-gone/0-records.
- Windows (win-debug): specimen + `crd-core-tests` rebuilt clean; `[crash-capture]` still **10 / 43** (the hoist is
  behavior-neutral; the Linux cases compile out), full core **26 / 133**.
- Both validators PASS. No engine source touched (slice-tag grep N/A). No repo-root scratch (WSL `/tmp` + scratchpad).

**Deferred to (e2), noted honestly (not skipped):**
- `inject_dump_fail` on Linux: the Linux `write_crash_record` (`crash.cpp` ~849–957) does **not** check the
  `s_inject_step` seam (that seam is on the Windows `MiniDumpWriteDump` path), so the mode currently measures as `av`
  (1 record). The Windows `inject_dump_fail` test case is `#if CRD_ENABLE_ASSERTS` **and** inside the `_WIN32` block, so
  **no Linux test exercises this mode at all** — it is not a silent Linux pass, it is simply not asserted on Linux.
  Wiring the seam into the Linux writer (and adding a Linux assertion) is a small separate piece.
- `abort`→SIGABRT and `fpe`→SIGFPE are not yet specimen modes (crd installs for both signals; adding the modes + their
  assertions would round out the handled-signal set).
- ASan Linux characterization needs a linux-asan build tree to assert measured values instead of only `exit != 0`.

Row 074 stays **Open**: (f) unwritable-path/SIGKILL/OOM honesty + record-content read-back, (g) acceptance review +
flip. Next tick begins with the protocol's advisor + CI check.

**Carry-forward for (f):** `parse_record` currently scans the whole file for `pid`/`tid`/`signal`/`code` tokens. That
is unambiguous against today's record (the register dump uses different tokens), but is fragile if a future header
field ever contains one of those bare words. When (f) adds the record-content read-back, make the parser stop at the
`regs` sentinel so it only scans the header — a ~2-line change.

## DIAG.5b (f) — reliability contract: record read-back, SIGKILL retention, install/uninstall

Closes the acceptance's remaining reliability clauses. **All changes are test/specimen code** (`tests/…`); no engine
source was touched (the Linux `install()`/`uninstall()` already had the behaviour these tests assert — verified before
writing them). The (a) audit's fear that `install()` might mishandle an existing dir or purge records was checked and
is unfounded: `install()` tolerates `EEXIST` (existing dir → Ok), `stat`+`S_ISDIR`-rejects a non-dir path
(→ `OutputDirUnusable`), and neither install nor the handler ever unlinks prior records.

- **Record read-back (`av`)** — the 5b analog of 5a's `read_dump_stream` check, from the on-disk `.log`. `parse_record`
  now reads the header fields (pid/tid/signal/code/**addr**/**exe**) only *before* the `regs` sentinel (so no register
  token can shadow a header field), then notes the register dump is present and that its final `cr2` line was reached.
  Asserted on `av`: `addr == "0x0"` (the null write's `si_addr`), `basename(exe) == basename(specimen)` (the "correctly
  identified binary", the no-symbols identity — build-id/symbolization is 5d), register dump present, and — **arch-gated
  on `regs x86_64`** — `cr2` present (a completeness/not-truncated check; on x86_64 `cr2` is the faulting address). An
  aarch64 runner skips only the named-register assert, never the record-present asserts.
- **SIGKILL / OOM retention (new `sigkill` mode)** — `kill(getpid(), SIGKILL)` is the OOM-killer mechanism made
  deterministic (no attempt to actually exhaust memory). The test pre-seeds a prior `crash_*.log` in the output dir,
  runs the mode, and asserts exit **137** (128 + SIGKILL), verdict `Crashed`, the seed **retained**, and **no new
  record** — the acceptance's "retain previous records or explicitly report no final capture, without promising a
  handler for uncatchable termination", verbatim. Not ASan-guarded (SIGKILL is uncatchable on every build).
- **Install-failure** — already covered cross-platform by the contract test "an output path that names an existing file
  is `OutputDirUnusable`" (runs on Linux; `install()`'s `stat`+`S_ISDIR` path). No new work.
- **Normal uninstall (new Linux contract test)** — observable from *outside* the crash API via raw POSIX queries:
  install a `SIG_IGN` sentinel on SIGSEGV; after crd `install()`, `sigaction(SIGSEGV, nullptr, &cur)` shows
  `SA_ONSTACK` set and the sentinel displaced; after `uninstall()`, the sentinel is restored and
  `sigaltstack(nullptr,&ss)` reports `SS_DISABLE`. Restores `SIG_DFL` at the end so a later crashing test is not left
  ignored.
- **Missing core collector** — already covered: `capture_dump` is `Unsupported` off Windows (contract test) and
  `crd_diag_harden()`'s `RLIMIT_CORE → 0` (e) means no OS core is expected; the crd record is the artifact. No new code.

**Verification:**
- Linux (`linux-gcc-debug`, `-Werror`): specimen + `crd-core-tests` build clean; `[crash-capture]` **10 / 59** (9 + the
  `sigkill` case; `av` gained the read-back asserts); `[crash]` **4 / 16** (+ the uninstall-restoration case). Direct
  probe: `sigkill` with a pre-seeded record → exit 137, 1 record (only the seed).
- Windows (win-debug): specimen + `crd-core-tests` rebuilt clean; `[crash-capture]` **10 / 43**, `[crash]` **14 / 78**,
  full core **26 / 133** — all unchanged (every (f) addition is `#if defined(__linux__)`; the `sigkill` mode compiles to
  a plain fault on Windows and no Windows test uses it).
- No engine source touched (slice-tag grep N/A). Both validators PASS. No repo-root scratch (WSL `/tmp` + scratchpad).

**(f2) / carry-forward (honest, not skipped):** a second write-time-denial variant beyond `denied` (rmdir→ENOENT) —
e.g. replacing the output dir with a regular file after `install()` (ENOTDIR→`OpenFailed`), uid-independent — was left
out for room; `denied` already proves the write-time `OpenFailed` path. The handler's `crash record FAILED (errno N)`
stderr line would be a free "handler ran" discriminator for `denied`/unwritable if the harness captured stderr (it
pipes stdout only) — noted, no plumbing added this tick. `inject_dump_fail` on Linux and `abort`/`fpe` modes remain
(e2).

**Carry-forward for (g)** (the acceptance review is clause-by-clause, not a rubber stamp — produce one
`clause → test(s) → evidence` table):
- **Thinnest-proof clauses to resolve first:** *"logger/allocator lock ownership"* — the `logger_lock` mode is
  currently `_WIN32`-only in the specimen; either add a Linux variant (`flockfile(stderr)` then fault) or record it as
  measured-not-asserted on Linux. *"Signal chaining"* — the handler chains to the previous disposition + `raise(sig)`
  (implemented in (b)/(c)); confirm whether any test *asserts* it (e.g. a prior handler observed running) or only that
  it is implemented, and say which.
- **Record-reader limitation to state:** `parse_record` reads `exe` as a single whitespace-delimited token, so it
  assumes a space-free executable path (true for the specimen and the CI `temp_directory_path()`). The record *writer*
  and format are unaffected; only the test reader would truncate a spaced path — list it as a known reader limitation.
- **Flip text must state the ADR boundary** ([ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md)
  §112–120): no external/paid/newly-installed collector; `capture_dump`/`read_dump_stream` are `Unsupported`/`0` on
  Linux **by design** (the OS core is the heavy artifact; crd adds the async-signal-safe record + honest termination).

Row 074 stays **Open**: only **(g)** — the acceptance review + the Open→Needs CI flip — remains. Next tick begins with
the protocol's advisor + CI check.

## DIAG.5b (g) — acceptance review + flip

Closed the three thin clauses first (all test/specimen code, no engine change): a Linux `logger_lock` (`flockfile`), a
`chain` mode (a prior SIGSEGV handler installed before crd `install()` that drops `chain.marker` and re-raises), and a
`hook_fault` mode (the crash-report hook itself faults). Then reviewed every sentence of the acceptance block quoted at
the top of this doc against the test(s) or code property that proves it.

**Clause → test / evidence.** Tests are `crd-core-tests` on `linux-gcc-debug` unless noted; "grep-confirmed, code
property" = a property of `crash.cpp`'s Linux block, not a runtime test.

| acceptance clause | proof |
| --- | --- |
| minimal async-signal-safe recording | `[crash]` record read-back + code property: handler uses `write(2)`/`open(O_CREAT\|O_EXCL)`/`close` only — no `fprintf`/`fputs`/`printf`/`snprintf`/malloc in the handler region (grep-confirmed) |
| *or* a qualified external collector | withheld by [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md) §112–120; `capture_dump`/`read_dump_stream` are `Unsupported`/`0` on Linux **by design** — contract test "capture_dump is Unsupported off Windows" |
| alternate signal stacks for every relevant OS thread | `overflow` (main, 1), `overflow_thread` (unguarded → **0**, honest negative), `overflow_thread_guarded` (1, `tid≠pid`, `code 2`), `fiber_overflow_worker` (1, `tid≠pid`, `code 2` — the (d) wiring) |
| collect original registers | `av` read-back: register dump present, `cr2` present on `regs x86_64` (completeness/not-truncated) |
| signal + process/thread identity | `av`: `signal 11`, `pid == tid`; guarded/worker: `tid != pid`; read-back: `basename(exe) == specimen` |
| no allocating/formatting/general locks in handler | code property (grep) + `logger_lock`: holds `flockfile(stderr)` then faults → still 1 record (write(2)-only handler) |
| installation failure | contract test "an output path that names an existing file is `OutputDirUnusable`" (Linux-active: `install()` `stat`+`S_ISDIR`) |
| recursive faults | `hook_fault`: 1 record (written before the hook), verdict `Crashed` **not `Timeout`** — the recursive fault is masked → `SIG_DFL`, bounded |
| signal chaining | `chain`: 1 record **and** `chain.marker` present — crd recorded then restored + re-raised into the prior handler |
| core-policy permissions | `crd_diag_harden()` `setrlimit(RLIMIT_CORE,0)` (e) — 0 core files measured; specimen-local, no global change |
| normal uninstall | contract test "uninstall restores the previous handler and disables the alt stack": prior `sigaction` restored + `sigaltstack` `SS_DISABLE` |
| symbolization/backtraces outside compromised execution | code property: the handler does **not** call `backtrace`/`backtrace_symbols` (grep-confirmed); the stack is recovered offline from the OS core — symbolization is DIAG.5d |
| faults on worker/fiber stacks | `fiber_overflow_worker` (worker, `tid≠pid`); `fiber` (null-deref, captured via `wait()` pump onto main) |
| exhausted stacks | `overflow`, `overflow_thread_guarded`, `fiber_overflow_worker` |
| logger/allocator lock ownership | `logger_lock` (stdio lock held; write(2)-only handler still records) |
| secondary signals | `fastfail` → SIGILL captured (exit 132); `concurrent` two-thread gate → exactly 1 record. Install set is SIGSEGV/SIGABRT/SIGFPE/SIGILL; dedicated `abort`/`fpe` fault modes are **(e2)** — measured-not-asserted |
| unwritable paths | `denied` (rmdir → ENOENT → `OpenFailed`): dir absent, 0 records |
| missing core collector | `capture_dump` Unsupported (contract) + `RLIMIT_CORE→0` — no OS core expected; the crd record is the artifact |
| SIGKILL/OOM retain prior or report none | `sigkill`: exit 137, pre-seeded record retained, **no** new record |
| don't promise a handler for uncatchable termination | `sigkill` — no handler runs (SIGKILL); honest, asserted |
| no global sysctl/security changes | code property: only specimen-local `setrlimit`; no `sysctl`/`prctl` security change (grep-confirmed) |

**Known limitations, stated not hidden:** `parse_record` reads `exe` as one whitespace-delimited token, so the
read-back test assumes a space-free executable path (true for the specimen and the CI `temp_directory_path()`); the
record writer/format are unaffected. **(e2):** `abort`/`fpe` specimen modes and `inject_dump_fail` on Linux (the Linux
writer does not honor the injection seam). **(f2):** a second write-time-denial variant beyond `denied`, and a
stderr-captured "handler ran" discriminator (the harness pipes stdout only). Under Linux ASan crd's `sigaction`
replaces ASan's SIGSEGV handler, so records under ASan are uncharacterized (no linux-asan tree here); every case's ASan
branch asserts only `exit != 0`, never skip-as-pass — full record assertions run on `linux-gcc-debug`.

**Verification at flip:** Linux `[crash-capture]` **13 / 72**, `[crash]` **4 / 16** (build clean under `-Werror`);
Windows `[crash-capture]` **10 / 43**, `[crash]` **14 / 78**, full core **26 / 133** — unchanged (every (g) addition is
`#if defined(__linux__)` or a new mode no Windows test uses). No engine source touched this tick. Both validators PASS.

Row 074 → **Needs CI**.
