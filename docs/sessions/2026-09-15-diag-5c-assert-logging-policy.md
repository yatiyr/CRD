# DIAG.5c — assertion and logging failure policy (DG10)

<!-- doc-role: historical -->

Owner slice: [DIAG.5c](../ROADMAP.md#slice-diag.5c). Contract: [runtime-diagnostics
design](../design/runtime-diagnostics.md#diag-5c); [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md).
DG10 (assertion/logging failure policy). Running slice doc: the sections below are appended per sub-unit. The opening
**(a)** part (census) quotes the acceptance verbatim, records the current assert + logger paths and their exact defects
against that acceptance (with measured evidence), the verification regime, the gotchas, and the per-tick decomposition;
**(a) landed no engine code and left row 075 Open** — each engine increment is its own later tick (see the "(b)…"
sections) and the Open→Needs CI flip is the final sub-unit, the same shape that made DIAG.5b land cleanly.

## Acceptance (verbatim from the design)

> Separate recoverable developer assertions from fatal contract violations. Headless/CI defaults cannot show modal
> dialogs or silently ignore required validation. Bound recursion and flushing; route catastrophic failure directly to
> the emergency channel. Preserve ordinary async logging but define queue pressure, severity retention, source/name
> lifetime, sink failure, deregistration and shutdown. Do not label the existing allocating ring sink crash-safe.
>
> Acceptance: assert from logger sink, OOM during formatting, stuck flush, ignored-site churn, missing console and
> assert before initialization/after shutdown terminate or return the declared outcome promptly with evidence.
> Interactive break behaviour stays explicit; shipping malformed-input rejection cannot rely on compiled-out assertions.

## Design vs ADR — the scope that governs the eventual flip

[ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md) §ID-4 draws the line 5c must respect: *"The
emergency recorder is a bounded in-tree recorder, distinct from the allocating queued
[logger](../../engine/foundation/log/src/logger.cpp) (DG10)."* So 5c is **not** about making the logger crash-safe —
the crash/assert *catastrophic* path is the DIAG.5a/5b emergency recorder ([crash.cpp](../../engine/foundation/core/src/crash.cpp)),
and 5c must **route catastrophic failure there**, not through the allocating logger. 5c's own remit is two policies:
1. **Assertion policy** — separate recoverable developer asserts (`CRD_ASSERT`, compiled out in Release) from fatal
   contract violations (`CRD_FATAL`, always on); make the *default* (no-handler) outcome headless-safe, bounded, and
   evidenced; keep interactive break explicit.
2. **Logging failure policy** — define, for the ordinary async logger, queue pressure, severity retention, source/name
   lifetime, sink failure, deregistration and shutdown; and **do not label the allocating `ring_buffer_sink`
   crash-safe** (it allocates `crd::String` messages — it is a normal sink, never the crash artifact).

## Current state and defects vs the acceptance

Paths: [`assert.cpp`](../../engine/foundation/core/src/assert.cpp) (the assert report path) and
[`logger.cpp`](../../engine/foundation/log/src/logger.cpp) (the async logger + the assert→log bridge).

1. **The default (no-handler) assert path is a MODAL DIALOG — headless-unsafe.** `report_assert_failure` with no
   platform handler falls through to `MessageBoxA(..., MB_ABORTRETRYIGNORE)` on Windows (`assert.cpp:134`), which blocks
   a headless/CI process indefinitely. **Measured baseline (win-debug):** every DIAG specimen avoids this by installing
   a platform handler — `crd-diag-counter-exhaustion-specimen` (which fires `CRD_FATAL` on `CounterPool exhausted`)
   exits **42** promptly, no dialog, because it calls `set_assert_platform_handler`. So headless-safety today is
   achieved **per-specimen**, not by a default policy. `crd_diag_harden()` handles `abort()`
   (`_set_abort_behavior(0, _CALL_REPORTFAULT)`) but not the MessageBox path. Acceptance: *"Headless/CI defaults cannot
   show modal dialogs"* + *"missing console … return the declared outcome promptly with evidence."* **(b)**
2. **The assert→log bridge's `flush()` is UNBOUNDED — the "stuck flush" case.** `crd_log_default_assert_handler`
   (`logger.cpp:167`) logs Critical then calls `flush()`; `flush()` (`logger.cpp:278`) is
   `drain_cv.wait(lock, [] { return queue.empty(); })` with **no timeout**. A stuck worker or sink hangs the assert
   path. Acceptance: *"Bound … flushing"*, *"stuck flush."* **(c)**
3. **OOM during formatting is unhandled in the bridge.** The assert report itself formats into a fixed 1024-byte stack
   buffer (`assert.cpp:101` — bounded, no heap: good). But the bridge's `CRD_LOG_CRITICAL` formats via `std::format`
   into an allocating `crd::String`; an allocation failure there, inside the assert path, is undefined. Acceptance:
   *"OOM during formatting … return the declared outcome."* **(c)**
4. **Reentrancy is only partially bounded.** `fire_assert_handler` has a `thread_local s_in_handler` guard
   (`assert.cpp:72`) so an assert *inside the handler* will not re-fire the handler — but `report_assert_failure` (the
   snprintf + platform/MessageBox path) still runs on a secondary assert. The *"assert from logger sink"* case depends
   on sync-vs-async: async enqueues (no inline sink call on the faulting thread), a synchronous sink config would call
   the sink inline and a sink-side assert would re-enter. Must be characterized and bounded. **(d)**
5. **assert before init / after shutdown.** Before `crd::log::init()` no bridge is installed → the default (MessageBox)
   path. After `shutdown()` the bridge is pulled **first**, before sinks are torn down (`logger.cpp:210` — good), so a
   racing assert during shutdown hits the default path, not a dead logger. The *declared outcome* for both is currently
   "the default path" — which is defect 1. Acceptance: *"assert before initialization/after shutdown … declared outcome
   promptly with evidence."* **(d)**
6. **Logger failure policy is under-specified.** Queue pressure exists as a `dropped` atomic counter on a full
   `RingBuffer{8192}` (`logger.cpp:63,69`) but with no defined **severity retention** (is Critical ever dropped?). Sink
   failure (`sink->write` return ignored in `deliver_to_sinks_locked`), sink **deregistration** during delivery, and
   source/name lifetime are not characterized. Acceptance: *"define queue pressure, severity retention, source/name
   lifetime, sink failure, deregistration and shutdown."* **(e)**
7. **The allocating ring sink must not be labeled crash-safe.** `ring_buffer_sink` stores `crd::String` messages (it
   allocates); the acceptance explicitly forbids calling it crash-safe. A labeling/doc pass + a test that the crash
   path does **not** route through it. **(f)**
8. **Compiled-out asserts vs shipping validation.** `CRD_ASSERT`/`CRD_ASSERT_MSG` are `#if CRD_ENABLE_ASSERTS`
   (compiled out in Release, `assert.hpp:69`); `CRD_FATAL` is always on (`assert.hpp:81`). Acceptance: *"shipping
   malformed-input rejection cannot rely on compiled-out assertions"* — audit that input-rejection paths use `CRD_FATAL`
   (or an explicit runtime reject), not `CRD_ASSERT`. Measured: the counter-exhaustion contract uses `CRD_FATAL`
   (always on) → exit 42, confirming the always-on path works. **(f)**
9. **ignored-site churn.** The per-site ignore table is a fixed 256-slot array under a mutex (`assert.cpp:30-33`); once
   full, further IGNOREs are silently not recorded (`assert.cpp:142`). The acceptance's *"ignored-site churn"* wants a
   defined, bounded behaviour under many distinct ignored sites. **(e)/(f)**

## Verification regime

- **win-debug is primary:** the assertion policy and the modal-dialog defect are Windows-centric (`MessageBoxA`), and
  `CRD_ENABLE_ASSERTS` is on in Debug/RelWithDebInfo. Specimen-based (the DIAG.0 bounded harness) for the headless
  exit-code/no-hang assertions; in-process (`crd-core-tests`/`crd-log-tests`) for the logger queue/flush/sink policy.
- **WSL `linux-gcc-debug`** for the catastrophic-route path: a Linux `abort()`/`CRD_FATAL` → SIGABRT → crd's 5b
  `sigaction` handler (installed for SIGABRT) → an emergency record. This ties 5c's "route catastrophic failure to the
  emergency channel" to 5b's recorder cross-platform.
- **No win-asan:** 5c is not a sanitizer slice (no allocator/UB instrumentation is under test); state this explicitly
  rather than running an unrelated preset. A Release-build check (asserts compiled out) is needed for defect 8.
- A Release/`CRD_ENABLE_ASSERTS=OFF` configuration is required to prove the compiled-out-vs-always-on split (defect 8);
  confirm whether a Release preset exists or whether a targeted translation-unit check suffices.

## Gotchas (desk-checked)

- **The MessageBox defect cannot be measured by running it** — a modal dialog would hang the harness until timeout. Its
  evidence is code (`assert.cpp:134`) + the contrasting measured baseline (platform-handler specimen → prompt exit 42).
- **A bounded assert-path flush must not deadlock its own drain.** `flush()` waits on `drain_cv`, notified by the
  worker; a timeout variant must still let the worker notify (don't hold `queue_mutex` across the whole wait wrong).
- **The bridge self-installs only if no handler is set** (`logger.cpp:194`) and pulls itself out at shutdown; tests and
  specimens install platform/assert handlers — the interaction (which handler wins, and whether both fire) must be kept
  explicit in any (b)/(d) change.
- **`CRD_ENABLE_ASSERTS` off in Release** flips which macros are live; any 5c test asserting compiled-out behaviour must
  be built in that configuration, not Debug.
- **"Interactive break behaviour stays explicit":** the debugger-break path (`CRD_FATAL` breaks into the debugger) must
  remain reachable when interactive — the headless default must not remove the interactive break, only replace the
  modal dialog in headless/CI.

## Decomposition (a)–(g)

- **(a) [this doc]** census + defects-vs-acceptance + verification regime + decomposition. No engine code; row 075 Open.
- **(b)** headless assert default: a no-handler default that, in declared-headless mode, terminates promptly with
  evidence and routes the catastrophic case to the emergency channel — never a modal dialog; interactive break stays
  explicit. Add a default-path specimen proving no hang + a declared exit code. **Done — see the "(b)" section below.**
- **(c)** bound the assert-path flush (a timeout on the bridge's `flush()`, honest "flush did not drain" evidence) and
  define the OOM-during-formatting outcome (bounded/no-alloc fallback in the assert path).
- **(d)** reentrancy + lifecycle: "assert from logger sink" (sync and async), and the declared outcomes for assert
  before-init / after-shutdown.
- **(e)** logger failure policy: queue pressure + **severity retention** (Critical retained), sink-failure handling,
  deregistration during delivery, source/name lifetime, and ignored-site churn bound.
- **(f)** labels + audit: do not label `ring_buffer_sink` crash-safe (+ a test the crash path does not route through
  it); audit that shipping input-rejection uses `CRD_FATAL`/explicit reject, not compiled-out `CRD_ASSERT`
  (Release-config check).
- **(g)** clause-by-clause acceptance review (the `clause → test → evidence` table) + flip row 075 Open→Needs CI with a
  Session link and the latest actions/runs URL.

Row 075 stays **Open** (this is (a); the slice is not done until (g)). Next tick: **(b)** — headless assert default;
begins with the protocol's advisor + CI check.

## DIAG.5c (b) — headless assert default

Closes defect 1 (the modal-dialog hang). A new declared-headless flag makes the DEFAULT (no platform handler) assert
path terminate promptly with evidence instead of blocking on `MessageBox`.

- **Engine (`crd-core`):** `assert.hpp`/`assert.cpp` gain `set_assert_headless(bool)` / `get_assert_headless()` (an
  atomic, default **false**). In `report_assert_failure`'s default path — reached only when no platform handler is
  installed (a platform handler still takes precedence, checked earlier) — after the evidence is written to stderr (and
  `OutputDebugString` on Windows), if headless is set the path does `fflush(stderr); std::abort();`. `abort()` raises
  SIGABRT, which crd's DIAG.5b handler records on Linux (routing the fatal assert to the emergency channel); on Windows
  it terminates promptly with the evidence already emitted. **Off by default, so interactive behaviour is unchanged**
  (the Windows `MessageBox`, the non-Windows `return 2` debug-trap): the only new behaviour is when a process opts in.
- **Design choice — declared, not auto-detected.** Auto-detecting headless from the window station is unreliable: a
  spawned test child in an interactive session inherits a *visible* window station yet is effectively headless (no user
  to click). So headless is a **declared mode**: the test harness's `crd_diag_harden()` now calls
  `set_assert_headless(true)` (every spawned specimen is headless by definition); a shipping headless app sets it at
  startup. This is deterministic and testable, and cannot mis-fire on an interactive desktop.
- **Specimen + tests:** `crash_capture_specimen` gains an `assert_default` mode that fires `CRD_FATAL` with **no** assert
  platform handler. Measured, then asserted:
  - **Linux** (`crd-core-tests [crash-capture]`): exit **134** (128 + SIGABRT), verdict `Crashed`, **1** crash record,
    `signal 6` — the fatal assert is captured by the emergency recorder, no hang.
  - **Windows (win-debug)**: verdict **≠ Timeout** (a blocked `MessageBox` would time out at 15 s), exit **3** (the CRT
    `abort` code), **0** minidumps — `abort()` does not reach the SEH last-chance filter, so the emergency-channel
    routing of a fatal assert is a Linux property here (a Windows assert→minidump route would need an SEH raise — noted
    for a later refinement, not required by (b)).
  - **Windows (win-asan)**: `[crash-capture]` runs on the win-asan lane too, so the test needs an ASan branch like its
    neighbors. Measured: ASan **intercepts `abort()`** (prints `==ABORTING==` and owns the exit), so the exact code is
    not crd's — the test's `built_with_asan()` branch asserts only `verdict ≠ Timeout` (no dialog hang) and
    `exit_code != 0` (terminated abnormally), never a hardcoded code. This was a real gap in the first (b) landing (the
    Windows case asserted `exit == 3` unconditionally); fixed this tick.
- **Precedence unaffected:** every existing specimen installs an assert *platform handler*, which short-circuits before
  the headless branch, so none changed behaviour; only a no-platform-handler default assert now aborts instead of
  dialoging.

**Verification:** Linux `[crash-capture]` **14 / 77** (+ `assert_default`), `[crash]` 4/16, build clean under `-Werror`.
Windows win-debug `[crash-capture]` **11 / 46** (+ `assert_default`), `[crash]` 14/78, full core **27 / 136**; win-asan
(run inside the vcvars env so the ASan runtime DLL is on PATH) `[crash-capture]` **11 / 25**, `[crash]` 14/78 — the
assert.cpp change regressed nothing. Slice-tag grep on `assert.cpp`/`assert.hpp`: clean. Both validators PASS. No
repo-root scratch. (Local gotcha for future ticks: win-asan test exes need the vcvars environment on PATH to load
`clang_rt.asan_dynamic-x86_64.dll` — launched bare they exit `0xC0000135` DLL_NOT_FOUND, which is not a test failure.)

**Note for later sub-units:** the Windows fatal-assert→emergency-channel route (an SEH raise the last-chance filter
catches, so a headless Windows assert also yields a minidump) is a candidate refinement — (b) delivers the no-dialog +
prompt-terminate + Linux emergency-record; it does not claim a Windows minidump from a plain `abort()`.

## DIAG.5c (c) — bounded assert-path flush + declared OOM-during-formatting outcome

**The stuck-flush mechanism (root-caused, not assumed).** The worker loop is **pop-then-write**
(`logger.cpp` `worker_main`: `try_pop` under `queue_mutex`, release, then `deliver_queued` locks `sinks_mutex`
and calls `sink->write()`). So a sink parked in `write()` leaves the worker holding `sinks_mutex` with the queue
already drained — the classic "stuck flush" is therefore a **`sinks_mutex` acquisition** hang at `flush()`'s
`lock_guard` (line 281), not a `drain_cv` hang; and a `flush_for` running on the worker itself would `try_lock` a
`std::mutex` it already owns (**undefined behaviour**). Both shape the fix.

**Bounded (additive; `flush()` unchanged).** New `bool flush_for(u32 timeout_ms) noexcept`:
1. **Worker self-detection.** A stored `worker_id` (captured right after the thread spawns) lets `flush_for` detect
   a reentrant call from inside a sink on the worker (an assert fired in `write()`/`flush()`); it bails at once —
   the worker is the only draining thread and already owns `sinks_mutex`, so waiting or `try_lock` would deadlock /
   be UB. Counts a timeout, one stderr line, returns false. **Not decoration:** the worker provably holds
   `sinks_mutex` during `write()`.
2. **Bounded drain.** `drain_cv.wait_for(timeout)` instead of the unbounded `wait`.
3. **Bounded sink-lock.** `sinks_mutex.try_lock()` once (adopt-lock guard on success); a stuck sink holding it means
   skip the per-sink flush rather than block.
Any give-up bumps a new `flush_timeouts` atomic (exposed as `flush_timeout_count()`, sibling to `dropped_count()`)
and writes one `[crd-log] …` line to stderr — evidence, never a silent hang. `assert_flush_timeout_ms` (default
2000) is a new `LoggerConfig` field; the **assert bridge** now calls `flush_for(cfg.assert_flush_timeout_ms)` in
place of the unbounded `flush()`. `shutdown()`'s `flush()` stays unbounded by design (it runs after the worker has
joined, so nothing is stuck).

**Declared OOM-during-formatting outcome (fact-found, not implemented).** Formatting is `std::format_to` into a
`crd::containers::String` back-inserter (`log_formatter.cpp`), reached through the `noexcept` `dispatch` wrapper's
try/catch. `String::grow_to` (the throwing path back-inserter uses) does **no null-check** on
`m_alloc->allocate(...)`. So OOM during formatting resolves two honest ways, never a silent hang or corruption:
a throwing allocator's `std::bad_alloc` (and `std::format`'s own internal allocation) propagates to `dispatch`'s
catch → the record is **dropped and counted** (`dropped_count()`); a nullptr-returning allocator makes `grow_to`
write through null → **SIGSEGV → crd crash capture → prompt fatal with a record**. Neither recurses through the
assert bridge: `grow_to` **faults**, it does not `CRD_FATAL`, so there is no allocate→assert→log→allocate loop from
this path. `/EHsc` is on, so the try/catch is real.

**Left to (d) (named, not silently deferred).** The assert bridge's *Critical delivery* (`CRD_LOG_CRITICAL`, before
`flush_for`) still takes `sinks_mutex` synchronously, so an assert fired on a non-worker thread while the worker is
wedged in a sink can still block *there* — bounding that reentrant delivery, plus the general assert-from-sink
reentrancy and the after-shutdown/before-init lifecycle, is (d). Note too that `flush_for`'s `worker_id`
self-detection covers **async only**: in sync mode the *calling* thread holds `sinks_mutex` during `write()`, so a
reentrant `flush_for` from inside a sink there would `try_lock` a `std::mutex` it owns (UB by the letter; both
toolchains return false in practice). It is masked today because the synchronous `CRD_LOG_CRITICAL` deadlocks on
that mutex first — (d) owns it (a `thread_local` in-sink flag is the natural shape).

**Verification.** win-debug: new `[log][diag][flush]` 3 cases / 9 assertions PASS — healthy drain returns true with
`flush_timeout_count()==0`; a worker wedged in a `BlockingSink` (record #1 in `write()`, #2 stuck behind) bounds
`flush_for(150)` to **152 ms** (`< 5×`) returning false; a `ReentrantFlushSink` calling `flush_for` *from* the
worker returns false in **6 ms** (self-detection, no deadlock/UB). Full `crd-log` suite 58/18, no regression.
Linux `linux-gcc-debug` `-Werror` build clean (`std::atomic<std::thread::id>`, `snprintf`/`fputs`) and the same
3 flush cases pass. Slice-tag grep on `logger.cpp`/`logger.hpp`: clean. Both validators PASS; no repo-root scratch.
New file `tests/foundation/log/test_diag_assert_flush.cpp` is auto-collected by the glob-based
`tests/foundation/log/CMakeLists.txt` (no CMake edit).

## DIAG.5c (d) — assert-from-sink reentrancy + lifecycle

**The defect (confirmed by reading the code, not assumed).** The worker holds `sinks_mutex` across `sink->write()`
(`deliver_queued`). An assert firing inside `write()` reaches the bridge (`report_assert_failure` → `fire_assert_handler`
runs the handler **before** the platform handler; the `s_in_handler` thread_local only blocks handler-within-handler,
and a first-level assert from a sink is not nested). The bridge then calls `CRD_LOG_CRITICAL` → `dispatch_impl`'s
synchronous Critical branch → `lock_guard(sinks_mutex)` **on the mutex this thread already owns** → hard
self-deadlock / UB (the acceptance's "assert from logger sink"). Sync mode is identical — the calling thread is the
owner. It never reached `flush_for`.

**Fix — a `sinks_mutex`-holding invariant, not a per-case patch.** A `thread_local int tl_in_sink_delivery`, bumped
by a `SinkDeliveryScope` RAII at **every** `sinks_mutex` critical section (`deliver_queued`, `dispatch_impl` sync
branch, `flush()`, `flush_for()`'s adopt-lock, `add_sink`, `clear_sinks`). It is the precise "this thread holds
`sinks_mutex`" predicate, so it covers the async worker, a sync caller, and an assert inside `add_sink`'s `push_back`
uniformly.
- **Bridge:** if `tl_in_sink_delivery > 0`, skip `dispatch` and `flush_for` entirely — one stderr line
  (`assert fired inside a sink: record suppressed (would self-lock)`), bump `sink_reentrant_assert_count()` (a new
  counter, distinct from `flush_timeouts` — different event, different evidence), return. crd-core's own step-1
  stderr evidence + platform/default handler still carries the termination. Not in-delivery → unchanged.
- **`flush_for`:** now bails on the same flag (replacing last tick's `worker_id` check, which it subsumes — the flag
  covers the sync-mode UB `worker_id` could not). `worker_id` removed.
- **Lifecycle:** the async push now bails on `!running` → counted `dropped++`, closing a shutdown-race hang (a log
  racing `shutdown()` after `running=false` but before `initialized=false` would otherwise enqueue a record no
  worker will drain, hanging a later `flush()`). The race window is not deterministically injectable from a test —
  verified by reading `shutdown()`'s ordering, not exercised. A **fully** post-shutdown log (`initialized=false`)
  takes the sync path to zero sinks and is silently discarded, not counted — an acceptable declared outcome (the
  logger is down), distinct from the race-window drop. Asserts **before init** (bridge not installed) and **after
  shutdown** (bridge uninstalled first, `logger.cpp:210`) already bypass the logger — verified, not changed.

**Not guarded, by design (belongs to (e)).** The unbounded `flush()` has no in-delivery check: a sink whose
`write()`/`flush()` itself calls `crd::log::flush()` still self-locks. `shutdown()` requires `flush()` to always
drain fully, so guarding it is not a (d) call — (e)'s sink-failure policy decides bail-with-evidence vs. a declared
sink-contract violation.

**Verification.** win-debug new `[log][diag][reentrancy]` 4 cases / 11 assertions PASS (async + the previously-UB
sync case both suppress in ~0 ms, no deadlock; assert-before-init and after-shutdown bypass the logger and a late
log + flush return `< 2 s`). Full `crd-log` 70/22, no regression. **win-shipping `/W4 /WX` clean** (the
`#if CRD_ENABLE_ASSERTS` gate on the asserting cases compiles to empty, no unused-function warnings). Linux
`linux-gcc-debug` `-Werror` build clean + the same 4 cases pass. Slice-tag grep on `logger.cpp`/`logger.hpp`:
clean. Both validators PASS; no repo-root scratch. New file `tests/foundation/log/test_diag_assert_reentrancy.cpp`
auto-collected by the glob CMake. **Watchdog hygiene:** added `crd-log-tests` to the reaper's `$byName` (a broken
fix here would deadlock a test exe the watchdog could not otherwise reap).

## DIAG.5c (e) — logger failure policy

Scoped by the (a) census defect 6: queue pressure + severity retention, sink failure, deregistration during
delivery, source/name lifetime. (Ring-sink "not crash-safe" labeling is defect 7 → (f); ignored-site churn is
assert.cpp — grouped with (f)'s assert.cpp audit, consistent with defect 9's own "(e)/(f)" tag.)

**The one real code defect: a throwing sink was handled asymmetrically.** `ISink::write` "must not throw", but a
third-party sink can. The **sync** path was caught by `dispatch()`'s outer try/catch (→ `dropped++`, but it lost the
record for *every* sink); the **async** path runs `deliver_to_sinks_locked` on the **`noexcept` worker with no
catch** → an escaping throw is `std::terminate`. Same for `sink->flush()` in `flush()`/`flush_for()`.
**Fix:** per-sink try/catch around `sink->write()` in `deliver_to_sinks_locked` and every `sink->flush()`
(via a `safe_sink_flush` helper). A throw is counted (`sink_failure_count()`, new) + one stderr line, and delivery
**continues to the next sink** — one bad sink neither terminates the worker nor loses the record for the healthy
sinks. The catch never logs (it runs inside `SinkDeliveryScope`; a `CRD_LOG_*` would self-lock on `sinks_mutex`).
`catch (...)` under `/EHsc` contains a thrown C++ exception only — a hardware fault (e.g. an access violation)
inside a sink is **not** swallowed and still reaches the crd crash handler, which is correct.
**Declared:** the logger does **not** auto-disable a failing sink — removal stays the caller's decision.

**Declared (verified by reading; no code change):**
- **Queue pressure / severity retention.** On overflow the *incoming* record is dropped (`dispatch_impl`
  `dropped++`, `logger.cpp` async branch), i.e. newest-dropped, `dropped_count()` is the evidence. A **Critical**
  record with the default `flush_on_critical=true` bypasses the queue and is delivered synchronously, so queue
  pressure can never drop it (severity retention). With `drop_on_overflow=false` the producer blocks on an
  **unbounded** `drain_cv.wait` — that is the configured blocking contract (a wedged sink then blocks producers);
  the headless default is `drop_on_overflow=true`. Not bounded here: bounding it would change configured semantics.
- **Deregistration during delivery.** Sink lifetime = logger lifetime; there is no per-sink removal, only
  `clear_sinks()` / `shutdown()`, both of which take `sinks_mutex` — so they cannot run concurrently with an
  in-flight delivery (delivery holds `sinks_mutex`). No `remove_sink` added (the census asks to *characterize*, not
  extend the API).
- **Source/name lifetime.** `QueuedRecord`/`LogRecord` hold `const Channel*`; channels are static-duration
  (`CRD_DEFINE_LOG_CHANNEL` defines a namespace-scope object), which must outlive any queued record — declared
  requirement. The message text is copied into an owning `crd::String` at enqueue, so it is independent of the
  caller's buffer. `ConsoleSink::write` uses unchecked `fwrite`/`fflush` → on a missing/closed console it fails
  silently (no throw, no assert) — the declared "missing console" outcome.
- **Logger API re-entered from a sink.** A sink that calls `crd::log::flush()`, `add_sink()`, `clear_sinks()` or
  `shutdown()` from inside its own `write()`/`flush()` is a sink-contract violation: each takes `sinks_mutex`, which
  the delivering thread already holds → self-lock. (`flush_for()` is the exception — it bails with evidence on the
  `tl_in_sink_delivery` flag, see (d).) Declared, **not guarded**: `flush()` must drain unconditionally for
  `shutdown()`, and guarding the rest would only defend a sink that is already broken. Not exercised (it would
  deadlock the suite); the `tl_in_sink_delivery` bail is a one-line option if a real sink is ever found doing this.

**Verification.** win-debug new `[log][diag][sinkfail]` 4 cases / 8 assertions PASS: a throwing sink async is a
counted failure (not `std::terminate`) with the healthy ring sink still receiving the record; the sync case keeps
the record for the other sink (not a whole-record drop); queue pressure (capacity 4, wedged sink, 32 Infos) drops
≥4 with the producer returning `< 2 s`; a Critical is delivered synchronously without `flush()`. Full `crd-log`
78/26, no regression. **win-shipping `/W4 /WX` clean.** Linux `linux-gcc-debug` `-Werror` build + full suite 78/26.
Both validators PASS; no repo-root scratch. New file `tests/foundation/log/test_diag_sink_failure.cpp` auto-collected.

## DIAG.5c (f) — labels + audit (census defects 7, 9, 8)

Three `defect → change → evidence` rows (the shape (g)'s clause table transcribes):

- **Defect 7 — the allocating ring sink must not be labeled crash-safe.**
  *Change:* an explicit block comment on `RingBufferSink` (`ring_buffer_sink.hpp`) — `write()` copies three heap
  `crd::String` members and `snapshot()` allocates again; NOT crash-safe / NOT async-signal-safe; never from a
  crash/signal handler; the DIAG.5a/5b emergency recorder (async-signal-safe `write(2)`, no allocation) is that
  channel. No code change (it was already not *labeled* safe; the risk was silence).
  *Evidence (structural, not a runtime test — the process dies before an in-memory ring could be read):* the
  crash-capture specimens link **no** `crd-log` (`tests/support/diag/CMakeLists.txt` — `crd-diag-crash-capture-specimen`
  links `crd-core crd-jobs Threads`; and neither `crd-core` nor `crd-jobs`'s `CMakeLists.txt` links `crd-log`, so it
  is not pulled in transitively either); `crd-core` does not depend upward on `crd-log`; and `crash.cpp` — the **only**
  engine file that installs crash/signal handlers (`sigaction` / `SetUnhandledExceptionFilter` / VEH, confirmed by an
  engine-wide grep; the only other hits are `dx12_signal`, a GPU fence-signal function, not an OS handler) — contains
  no `CRD_ASSERT|CRD_FATAL|CRD_LOG`, so no crd crash/signal handler routes through any sink.

- **Defect 9 — ignored-site churn.** The per-site ignore table was silent once its 256 slots filled (a 257th
  distinct IGNORE was simply not recorded).
  *Change:* the table is now a 256-entry **FIFO ring** — a 257th distinct site evicts the oldest (which fires again),
  counted by the new `assert_ignore_eviction_count()` with **one** stderr line on the first eviction only. A new
  public `bool ignore_assert_site(const char*, int)` is the programmatic record path (the Windows `IDIGNORE` branch
  now calls it); `assert.hpp` gains both. Interactive/opt-in by nature — a **headless build never reaches this
  table** (it aborts before the dialog); stated in the API doc.
  *Evidence:* `tests/foundation/core/test_diag_assert_ignore.cpp` — 300 distinct sites into the 256 ring: eviction
  count delta ≥ 44 (counted, not silent), the newest site (299) is still ignored (`report_assert_failure` returns 0
  **without firing** a counting handler), the oldest (0) was evicted and fires again. 304 assertions, win-debug +
  **win-shipping** (asserts off — the path is `report_assert_failure`/`ignore_assert_site`, always-on) + Linux.

- **Defect 8 — compiled-out asserts vs shipping input rejection.** Audit result: **clean, no conversions.** The one
  external-input parser, the native **toml** parser (`engine/foundation/toml/`), has **zero**
  `CRD_ASSERT/CRD_FATAL/CRD_UNREACHABLE` and rejects malformed input at runtime via `parse_error` / `parse_result`
  (`value<T>()` returns `std::optional`). The asserts in the consumers are all preconditions or writer-side
  invariants, **not** checks on parsed bytes: `config.cpp:71` (`current != nullptr` right after inserting the table
  — an invariant on just-built state), `config.cpp:79` (`parts.size() > 0` on a developer-supplied key —
  precondition), `profile_artifact_builder.cpp:98,130` (`cursor == total` — the *builder's* own size math, a
  cannot-happen invariant), and the profile loaders' `alloc != nullptr` / `schema_version >= 1` (constructor
  preconditions); the perf module's two (`profiler.cpp:219` power-of-two ring-slot config, `:391` a thread-count
  limit that returns `kInvalidThread`) are a config precondition and a resource limit with a runtime fallback, and
  `math/` has none. **Audited: toml, config, profile, perf, math** — none is an input-rejection path that relies on
  a compiled-out `CRD_ASSERT`.
  *Evidence:* the toml suite's "malformed input reports a failure, not a crash" (and 5 more) pass on **win-shipping**
  (`CRD_ENABLE_ASSERTS` off) — proving rejection is runtime, not assert-dependent — plus win-debug + Linux (36/6).

**Verification.** win-debug `crd-core-tests` 440/28 (+ `[ignore]` 304), `crd-log-tests` 78/26, no regression;
**win-shipping** `/W4 /WX` clean, `[ignore]` 304 + toml `[toml]` 36/6 both pass with asserts off; Linux
`linux-gcc-debug` `-Werror` build + `[ignore]` 304 + toml 36/6 + core 409/21. Slice-tag grep on `assert.cpp`/
`assert.hpp`/`ring_buffer_sink.hpp`: clean. Both validators PASS; no repo-root scratch. New file
`tests/foundation/core/test_diag_assert_ignore.cpp` auto-collected.

## DIAG.5c (g) — clause-by-clause acceptance review

Every clause of the design section (`runtime-diagnostics.md#diag-5c`) mapped to its landing sub-unit and evidence.
All numbers are from the (b)–(f) sections above; no code changed this tick.

**Design policy clauses:**

| # | Clause | Where | Evidence |
|---|--------|-------|----------|
| 1 | Separate recoverable dev asserts from fatal contract violations | pre-existing + (f) | `CRD_ASSERT` (`#if CRD_ENABLE_ASSERTS`) vs always-on `CRD_FATAL` (`assert.hpp`); (f) defect-8 audit; (a)'s counter-exhaustion specimen exits 42 on shipping |
| 2 | Headless/CI default: no modal dialog | (b) | `set_assert_headless`→`abort()`; `assert_default` specimen, win-debug + Linux |
| 3 | …no silent ignore of required validation | (b),(f) | headless aborts (never ignores); (f) defect-9 eviction is **counted**, not silent |
| 4 | Bound recursion | pre-existing + (d) | `s_in_handler` (handler-within-handler) **and** `tl_in_sink_delivery` (assert-from-sink) |
| 5 | Bound flushing | (c) | `flush_for(timeout)`; stuck-flush bounded to 152 ms |
| 6 | Route catastrophic failure to the emergency channel | (b) | Linux: `abort`→SIGABRT→5b recorder (`assert_default` Linux test). **Windows: named limitation** (below) |
| 7 | Preserve ordinary async logging | (c)–(e) | `flush()` + worker path unchanged bar per-sink catch / delivery-scope / running-gate; crd-log 78/26 |
| 8 | Queue pressure | (e) | newest-dropped, `dropped_count()`; `[sinkfail]` capacity-4 test |
| 9 | Severity retention | (e) | Critical bypasses the queue (sync); `[sinkfail]` Critical test |
| 10 | Source/name lifetime | (e) | `Channel*` static-duration declared; message copied into owning `String` |
| 11 | Sink failure | (e) | per-sink try/catch, `sink_failure_count()`; `[sinkfail]` async+sync tests |
| 12 | Deregistration during delivery | (e) | declared: lifetime = logger lifetime; `clear_sinks`/`shutdown` serialize on `sinks_mutex` |
| 13 | Shutdown | (d),(e) | bridge uninstalled first; running-gate; `flush()` at shutdown unchanged; after-shutdown test |
| 14 | Don't label the ring sink crash-safe | (f) | defect-7 comment + structural proof (no handler logs; specimens link no crd-log) |

**Acceptance items** ("terminate or return the declared outcome promptly with evidence"):

| Item | Where | Evidence |
|------|-------|----------|
| A. assert from logger sink | (d) | `[reentrancy]` async + sync suppress in ~0 ms, no deadlock. Under headless: **verified by parts** (bridge suppression + (b) headless abort), not one composed specimen |
| B. OOM during formatting | (c) | **declared, untested**: `String::grow_to` OOM → `bad_alloc` caught by `dispatch` (counted drop) or null-deref → crash capture; no allocation-failure seam exists in `memory/` to force it |
| C. stuck flush | (c) | `[flush]` bounded to 152 ms with evidence |
| D. ignored-site churn | (f) | `[ignore]` 304 assertions (win-debug + win-shipping + Linux): FIFO eviction counted |
| E. missing console | (e) | declared: `ConsoleSink` unchecked `fwrite`/`fflush` → silent fail, no throw/assert |
| F. assert before init | (d) | `[reentrancy]` — no bridge installed, returns |
| G. assert after shutdown | (d) | `[reentrancy]` — bridge uninstalled, late log + flush bounded |
| H. interactive break stays explicit | (b) | code property: headless is opt-in; default path still `MessageBoxA` / returns 2 / `CRD_FATAL` breaks (`assert.cpp`). Not runtime-testable (modal) |
| I. shipping malformed-input not via compiled-out asserts | (f) | toml `[toml]` 36/6 on **win-shipping** (asserts off) — runtime `parse_error` rejection |

Every clause has at least declared-with-evidence → the slice is complete; row 075 flips Open→Needs CI.

**Named limitations (carried on the row, per 5a/5b precedent):** (1) a Windows headless assert terminates promptly
with stderr evidence but does **not** emit a minidump from `abort()` — that needs an SEH raise the last-chance
filter catches (future work); Linux routes to the 5b recorder. (2) The unbounded `flush()` and the logger API
re-entered from a sink are declared sink-contract violations, not guarded. (3) `drop_on_overflow=false` blocks the
producer unbounded (configured contract; headless default is drop). (4) The shutdown-race running-gate is reasoned
from `shutdown()` ordering, not deterministically exercised. (5) OOM-during-formatting is declared, untested (no
seam). (6) The ignore table is interactive/opt-in (headless never reaches it). (7) No per-sink `remove_sink`. (8) A
fully post-shutdown log is silently discarded, not counted.

CI: `34946718738` is red on the user's preset-matrix tests (`test-native-build-profiles.py` / `test-repository-tools.py`,
see the CI triage section), **not** on the 5c work — the flip to Needs CI records completion of the slice; the row
goes Done when the user reconciles their preset expectations and a run is green (per protocol step 2).

## CI triage (schedule run 34946718738, HEAD `b726b204`)

This tick advanced no new roadmap row: the CI check found the pushed tree RED with diagnostics-code failures on the
stricter lanes, and protocol step 2 (fix RED before advancing) took precedence. CI builds the committed tree, which
lacks this session's uncommitted diag batch, so the failures split three ways:

- **Already fixed by the uncommitted batch (await the user's push):** GCC `-Werror=class-memaccess` in
  `diagnostic_allocator.cpp:115` (value-init instead of `memset`); `-Wmissing-field-initializers` on
  `jobs.cpp` `ProgressSample` (the `exhaustions` field — local diff adds the 5th initializer); the
  `test_diag_hang_dump.cpp` / `test_diag_hang_watchdog.cpp` unused-symbol warnings. No further action.
- **Fixed this tick (folded into the batch):**
  - `test_diag_rt_sentinel.cpp` C4101 `'rt'/'outer'/'inner'` unreferenced-local (win-shipping / win-release, `/W4 /WX`).
    Root cause was deeper than the warning: with `CRD_ENABLE_ASSERTS` off, `RtScope` is a defaulted no-op, so the
    guards are unreferenced **and** the behavioural `CHECK(g_rt_alloc_fires == 1)` would fail at runtime. Fixed by
    gating the whole suite in `#if CRD_ENABLE_ASSERTS` (the sentinels' own contract; matches `test_counter.cpp`).
    Verified: win-debug tests active + pass (15 assertions / 6 cases); win-shipping builds clean (file gated to empty,
    no C4101, no latent runtime fail).
  - `crash.cpp:256` clang-tidy `cppcoreguidelines-pro-type-const-cast` (win-tidy). The cast is unavoidable —
    `MINIDUMP_USER_STREAM::Buffer` is non-const `PVOID` and `MiniDumpWriteDump` only reads it — so a justified
    `// NOLINT(cppcoreguidelines-pro-type-const-cast)` per `.clang-tidy`'s own call-site convention (line 81). MSVC
    compiles clean on win-debug + win-shipping; not locally re-run through clang-tidy.
  - `specimen_common.hpp`: last tick's unconditional `#include <crd/core/assert.hpp>` broke the minimal
    `crd-diag-data-race-specimen` (links only `Threads`, no crd-core → C1083). Guarded the include + the
    `set_assert_headless` call behind `__has_include(<crd/core/assert.hpp>)` — a specimen without crd-core has no crd
    asserts to harden. Verified: win-debug builds all three test targets + their specimens, EXIT=0.
- **User-domain — untouched (constraint: do not touch project-sync / CI repo-hardening):**
  `scripts/test-native-build-profiles.py:48` (`assertEqual(len(visible presets), 23)` → `24 != 23`) and
  `scripts/test-repository-tools.py:460` (visible-minus-owned preset set changed) both track the CMake **preset
  matrix**, which the user's committed repo-hardening grew to 24 presets without updating these expectations; and
  `[check_no_std_containers] FAIL: 70 owning STL container uses` (engine-wide count; this session's engine diff adds
  **zero** owning STL). `CMakePresets.json` is unmodified locally. **These keep the full CI run RED regardless of the
  diag fixes**, so rows 072–074 (Needs CI) cannot flip to Done until the user reconciles their own preset-matrix
  expectations and pushes this batch.

No ROADMAP row changed. Files this tick: `crash.cpp`, `test_diag_rt_sentinel.cpp`, `specimen_common.hpp`. Both
validators PASS; no repo-root scratch.
