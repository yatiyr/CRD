# DIAG.5a — Windows crash and hang capture (DG09): plan and decomposition

<!-- doc-role: historical -->

Owner slice: [DIAG.5a](../ROADMAP.md#slice-diag.5a). Contract: [runtime-diagnostics
design](../design/runtime-diagnostics.md#diag-5a); [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md).
DG09 (fatal-path reliability) transferred here from the DIAG.0 census — crash capture is catalogued there as
"implemented; unqualified — no death/child-process test". This is the opening (a) sub-unit: it records the confirmed
mechanism, the exact current defects, the layering decision and the per-tick decomposition. **No engine code lands in
this tick and row 073 stays Open**; each engine increment below is its own later tick, and the Open→Needs CI flip is
the final sub-unit (planned as (g); it was absorbed into (f) once dump-content validation and the acceptance review
landed together).

## Design vs ADR — the scope that governs the eventual flip wording

The design body says "replace the unreliable fatal-path dependency on in-process dumping with a qualified external
handler **or** OS-supported mechanism". [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md)
§112–120 resolves that choice: **no external/paid/newly-installed collector is authorized** (it remains a separate
future capability requiring its own user decision); the qualified baseline is the **in-tree path**, and DIAG.5a is
the existing Windows `MiniDumpWriteDump` route in [crash.cpp](../../engine/foundation/core/src/crash.cpp) **hardened
to check its return and avoid second-resolution filename collisions**. So DIAG.5a's acceptance is met by the hardened
in-process path (a pre-created handler thread is still in-process — it is the safe way to dump off a possibly
exhausted faulting stack, not an external process). The "minimal emergency fallback" the design requires is the
stderr ExceptionCode/Address print plus a crash-report hook fired when the dump cannot be written. The flip note must
state this so it does not claim an external collector that the ADR withholds.

## Current defects in the Windows path (from crash.cpp today)

The `#if defined(_WIN32)` branch installs `SetUnhandledExceptionFilter(&crash_filter)` and dumps in-process. Against
the [design acceptance](../design/runtime-diagnostics.md#diag-5a):

1. **Write result ignored, success announced anyway.** `MiniDumpWriteDump(...)` (crash.cpp:45) discards its `BOOL`;
   "crash dump: %s" (crash.cpp:55) prints whenever the *file opened*, not when the dump *succeeded*. Acceptance:
   "failed MiniDumpWriteDump cannot print success."
2. **Second-resolution filenames collide, and overwrite.** The name is `..._%02d%02d%02d.dmp` to the second
   (crash.cpp:30) opened `CREATE_ALWAYS` (crash.cpp:37) — two crashes in one second collide and the second clobbers
   the first. Acceptance: "collision-safe files" and "concurrent crashes".
3. **ANSI / `MAX_PATH`, not Unicode/long-path-safe.** `CreateFileA`, `MAX_PATH` (crash.cpp:18,36). Acceptance:
   "Unicode/long-path-safe".
4. **No concurrent-crash serialization.** Two faulting threads both enter the filter and both drive DbgHelp, which is
   not thread-safe. Acceptance: "concurrent crashes", "serialize DbgHelp access".
5. **No install-result check.** `install()` (crash.cpp:76) ignores the `CreateDirectory` result and the previous
   filter pointer. Acceptance: "Check every install/write result", "support early startup, native plugin faults".
6. **Filter runs on the faulting (possibly exhausted) stack.** No `SetThreadStackGuarantee`, no handler thread.
   Acceptance: "stack exhaustion".
7. **No preserved fault reason for a parent harness.** The filter returns `EXCEPTION_CONTINUE_SEARCH`; there is no
   qualification that a parent verifies actual termination and retains the original fault reason. Acceptance +
   census: "no death/child-process test".

(The Linux `sigaction`/`backtrace` branch's `SA_ONSTACK` and install-failure gaps are DIAG.5b, not this slice.)

## Layering decision — the crash-report hook

crd-core sits below crd-perf, so `crash.cpp` cannot call DIAG.2b's emergency recorder directly (ADR §107: hosts
compose the report; no module gains a second crash path). The seam is a `set_crash_report_handler(handler, user)`
(the atomic-pair, no-default-action pattern used by the watchdog/RT sentinels), invoked from the fault path with a
**preallocated** `CrashReport { code, address, faulting_tid, write_result, dump_path_or_last_error }`. Hosts wire it
to the emergency record. This keeps the fault path allocation-free and lock-free of the logger while still delivering
the "minimal emergency fallback".

## Decomposition (one sub-unit per later tick)

- **(b) crash API contract + honest write.** `CrashReport` + `set_crash_report_handler`; checked `install()` (dir
  created, `SetUnhandledExceptionFilter` result, previous-filter pointer inspected — re-asserting our filter also
  cheaply detects a plugin that displaced us); the full output path resolved **at install** (UTF-8→wide,
  `GetFullPathNameW`, `\\?\` prefix) into a preallocated wide buffer, never in the filter; filename = pid + 64-bit
  tick + counter, opened `CREATE_NEW`, bump the counter on `ERROR_FILE_EXISTS`; check the `MiniDumpWriteDump` BOOL,
  then `FlushFileBuffers`/`CloseHandle`; on any failure `DeleteFileW` the partial (no plausible garbage) and print
  FAILED; the success line only after all steps succeed. Terminate via `TerminateProcess(self, ExceptionCode)` so the
  exit code *is* the fault reason — but first verify how `crash_specimen` suppresses the error dialog today (census:
  "no Windows error dialog") before touching that.
- **(c) handler thread + stack guarantee + concurrent-crash gate.** A pre-created handler thread with its own stack
  does the dump (MSDN: dump from a thread other than the faulter; also survives a faulter-stack overflow). The
  faulting thread only: `InterlockedCompareExchange` single-shot gate, capture `ep` + **its own tid** (the dumped
  `mei.ThreadId` must be the faulter, not the writer), `SetEvent`, then a **bounded wait on the "dump done" event** —
  a second concurrent faulter must also wait, never return first (returning kills the process mid-dump); the second
  fault is recorded through the hook as "concurrent fault suppressed", one dump only. `SetThreadStackGuarantee` on
  owned threads. One DbgHelp lock in crash.cpp (grep confirms only crash.cpp uses DbgHelp in `engine/`), documented
  for any future in-process symbolizer.
- **(d) hang-triggered live dump.** The row is "crash *and hang* capture", and DIAG.4c deferred running-thread stack
  capture to here. A `HangReport`/`worker_snapshot` consumer can request a **non-fatal** `MiniDumpWriteDump` of self
  from a non-worker thread through the same writer — no terminate.
- **(e) acceptance specimens** (DIAG.0 harness, subprocess): crash while holding a logger lock; concurrent crash;
  injected write failure (an `CRD_ENABLE_ASSERTS`-gated injection seam that forces the write to fail — an injection,
  never a weakened check); denied/full output (target dir is an existing *file*); missing symbols (the dump still
  identifies the exe by CV GUID/age in the module-list stream — no symbolized-frame claim; that is 5d); stack
  overflow; fail-fast (`__fastfail` bypasses SEH — the `0xC0000409` exit is retained and **no** dump is claimed, the
  limitation *is* the assertion). Plus **fault inside a fiber job**: check whether the fiber switch updates TEB
  `StackBase/StackLimit`; if not, the dumped faulting-thread stack range is wrong — a real gap to find during (e),
  not at the flip.
- **(f) harness qualification.** Reuse `Want::Crash` → `Verdict::Crashed` and `Outcome.exit_code`. Add: the announced
  path equals the actual file; `MiniDumpReadDumpStream` validates it (ExceptionStream code matches the injected
  fault; ModuleListStream names the specimen binary); the parent verifies actual termination and retains the original
  fault reason. Under win-asan, model the ASan-takes-the-AV case as the explicit `built_with_asan()` →
  `SanitizerCaught` branch (DIAG.1b pattern), never a skip.
- **(g) doc + flip.** Full-acceptance review section here; row 073 Open→Needs CI with this Session link + the latest
  actions/runs URL.

## Constraints honored throughout

No registry / WER LocalDumps (a system setting — out of scope for this loop). Zero heap and no logger lock on the
fault path. Docs may name `DIAG.5a`; `crash.cpp` comments may not carry slice tags. In-tree only — no external
collector.

## Status

(a) complete: mechanism confirmed (in-tree hardened MiniDump, per ADR-0133 §112–120), defects enumerated, layering
and decomposition recorded. Row 073 stays **Open**. Next tick begins (b) — the crash API contract and honest write
result — starting from a fresh advisor call and CI check per the loop protocol.

## DIAG.5a (b) — checked install + honest, collision-safe minidump write

The crash API is now a checked contract. [crash.hpp](../../engine/foundation/core/include/crd/core/crash.hpp)
gained `InstallResult` (`Ok` / `OkReinstalled` / `OkReplacedForeignFilter` / `OutputDirUnusable` / `PathTooLong` /
`FilterInstallFailed` / `Unsupported`), `WriteResult` (`Ok` / `NotInstalled` / `OpenFailed` / `DumpFailed` /
`FlushFailed` / `Unsupported`), a preallocated `CrashReport {code, address, faulting_tid, write, dump_path,
last_error}`, `set_crash_report_handler` (the atomic-pair, no-default-action pattern), a now-`[[nodiscard]]`
`install()`, and `capture_dump(out_path)` — the live, non-fatal entry to the **shared writer** the fatal filter also
uses (the hang-triggered dump will drive the same writer in a later sub-unit).

[crash.cpp](../../engine/foundation/core/src/crash.cpp) Windows path, hardened against the DG09 defects:

- **Path resolved once, at install** — `MultiByteToWideChar(CP_UTF8)` → `GetFullPathNameW` → `\?\` prefix
  (UNC-aware) into a preallocated 32768-wide buffer; nothing resolves on the fatal path. `CreateDirectoryW`, then a
  `GetFileAttributesW & FILE_ATTRIBUTE_DIRECTORY` check because `ERROR_ALREADY_EXISTS` is *also* returned when the
  path names a file — that check is what makes the denied/existing-file case honest instead of a false success.
- **Collision-safe filenames** — `crash_<pid>_<tick64hex>_<serial>_<attempt>.dmp`, backslash-joined (a forward slash
  is not normalized under `\?\`), formatted by hand-rolled bounded builders (no CRT locale/lock on the fatal path),
  opened `CREATE_NEW`; on `ERROR_FILE_EXISTS` the attempt index bumps (bounded). Same-second and concurrent dumps can
  no longer collide or overwrite.
- **Honest write result** — the `MiniDumpWriteDump` BOOL is checked (its last error is an HRESULT, stored raw), then
  `FlushFileBuffers`, then `CloseHandle`; on any failure the partial file is `DeleteFileW`d and the FAILED line names
  the failing step. The "crash dump: <path>" success line prints only after all three succeed. The emergency
  fallback (ExceptionCode/Address to stderr) always prints; the report hook fires with the filled `CrashReport`.
  `MiniDumpWriteDump` is serialized by an SRWLOCK (DbgHelp is not thread-safe; grep confirms crash.cpp is the only
  `engine/` DbgHelp user).
- **Install honesty** — the *originally* saved previous filter is preserved across re-installs (the old code saved
  its own filter on a double-install, so `uninstall()` restored our filter, not the real previous — fixed);
  re-install returns `OkReinstalled` when our filter is still current and `OkReplacedForeignFilter` when a foreign
  filter had displaced us (a cheap "a plugin took over" signal). `EXCEPTION_CONTINUE_SEARCH` is kept, so the process
  still exits with the exception code (why the crash specimen verdicts `Crashed`) and any prior/ASan filter still
  runs; process termination is decided in (c) where the concurrent-faulter wait forces it.

The Linux branch implements the same signatures (checked `install()` with a dir `stat`, stored report handler fired
from the signal path with `write == Unsupported`, `capture_dump` → `Unsupported`); its alternate-signal-stack and
install-failure hardening remain for the Linux crash-capture slice. A non-Windows/non-Linux stub branch returns
`Unsupported` throughout so the header links on any target. The three callers now honour the checked result:
`Application` warns to stderr on a non-Ok install and continues; the two test mains `(void)`-discard it.

Tested without a crash by [test_diag_crash_contract.cpp](../../tests/foundation/core/test_diag_crash_contract.cpp)
(dumps land in a unique `temp_directory_path()` subtree, never repo-root `./crashes`, and every case removes its
tree): install into a usable dir → `Ok`, the dir is created, re-install → `OkReinstalled`, double `uninstall()`
harmless; an output path naming an existing file → `OutputDirUnusable`; `capture_dump` before install →
`NotInstalled`; a live capture → `Ok` with a `\?\`-prefixed absolute path to an existing non-empty file, and a
second back-to-back capture writes a distinct file without clobbering the first; a capture into a since-removed dir →
`OpenFailed` with nothing left behind.

Verified: `crd-core-tests` win-debug 7 cases / 31 assertions (5 new `[core][diag][crash]`); the same 5 cases under
the real `win-asan` preset 19 assertions (the DbgHelp self-dump is ASan-clean); the perf `[diag][harness]` crash
specimen still verdicts `Crashed` (10 cases, no regression); full win-debug build clean (crd-core, crd-app, the two
diag/stress mains); both validators PASS; no slice tags in the touched engine source (three `DIAG.5b`/`DIAG.11a`
comment tags the grep caught were rephrased); no repo-root scratch or `crashes/`.

Row 073 stays **Open** (partial slice). Next sub-unit **(c)**: the pre-created handler thread with its own stack,
`SetThreadStackGuarantee`, and the single-shot concurrent-crash gate (both faulters wait on "dump done"; one dump;
the second recorded as suppressed) — the stack-exhaustion and concurrent-crash acceptance cases.

## DIAG.5a (c) — dedicated handler thread + single-shot concurrent-fault gate + stack guarantee

The Windows fatal path now hands the dump off to a **dedicated crash handler thread** created (with a 1 MiB stack)
at the first `install()`, never in the filter — a `CreateThread` under a fault inside loader/DllMain code would
deadlock on the loader lock. The faulter does the minimum and the handler does the write on its own fresh stack,
which is the whole point for a stack-overflow fault (the faulting stack has no room left to run DbgHelp).

- **`crash_filter` split into `handle_fatal(ep, tid)` + a chaining tail.** `handle_fatal` is the shared core (gate →
  hand-off → wait → return a `CrashReport`); `crash_filter` calls it then **chains**:
  `return s_prev_filter ? s_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;`. (b) returned a bare `CONTINUE_SEARCH`,
  which silently dropped any host/plugin/sanitizer filter we had replaced — chaining preserves it, the design's
  "preserve exception context / native plugin faults" clause. The process still exits with the exception code when
  there is no previous filter.
- **Single-shot gate.** `InterlockedCompareExchange(&s_gate, 1, 0)`: the first fault owns the one dump per process.
  The winner stores its `EXCEPTION_POINTERS` + **its own tid** (so `mei.ThreadId` is the faulter, not the writer),
  signals the handler, and waits (bounded, 30 s) on a manual-reset "done" event. A concurrent second fault reports
  `WriteResult::Suppressed`, fires the hook, and **also waits on "done"** before returning — returning first would
  let the OS tear the process down mid-dump. The manual-reset event releases the winner and every loser together.
- **Handler thread does the write, print and hook** on the fresh stack, publishes the `CrashReport` before setting
  "done"; the winner reads it back after the wait. The `crash.hpp` handler comment was corrected to say the fatal
  report fires from the handler thread (the suppressed notification fires from the extra faulting thread).
- **Fallbacks.** If the faulting thread *is* the handler thread (a fault inside DbgHelp), it cannot signal-and-wait,
  so it does a direct in-thread dump under a `TryAcquireSRWLockExclusive` (a held lock → `HandlerTimeout`, never a
  deadlock). If the handler thread wedges past the bounded wait, the winner takes the same direct-fallback path.
- **Stack guarantee.** `crd::crash::guard_current_thread_stack(bytes)` wraps `SetThreadStackGuarantee` (checked,
  best-effort); `install()` calls it for the installing thread. Worker/fiber-thread wiring and the overflow proof are
  left to (e), where the stack-overflow specimen shows whether more is needed. `uninstall()` signals quit, joins the
  handler thread promptly, closes the events and resets the gate; a fresh `install()` is a fresh generation.

`WriteResult` gained `Suppressed` and `HandlerTimeout`. `FilterInstallFailed` is now reachable on Windows
(event/thread-creation failure) but is not fault-injected in-process — it is covered by inspection, not a seeded
test, since forcing `CreateThread`/`CreateEventW` to fail from a unit test is not reliably possible here.

Exercised **without a crash** by an asserts-gated test seam,
[`test_fatal_path(code)`](../../engine/foundation/core/src/crash.cpp), which builds a *synthetic* `EXCEPTION_RECORD`
+ `RtlCaptureContext` and calls `handle_fatal` directly — **not** `crash_filter`, so it never chains to Catch2's own
fatal filter (Catch2 installs one around every test). New `[core][diag][crash]` cases: a simulated fatal writes
exactly one dump, returns `Ok`, and the hook fires with `faulting_tid == caller` while
`GetCurrentThreadId()` inside the hook ≠ caller (proving the hop to the handler thread); two threads faulting
concurrently yield exactly one `Ok` + one `Suppressed` and exactly one `.dmp`, both returning (neither blocks
forever); the fatal path before install is `NotInstalled`; `uninstall()` joins the handler thread promptly and a
re-install still writes.

Verified: `crd-core-tests` win-debug 11 cases / 50 assertions (9 `[core][diag][crash]`, 4 new); the same 9 under the
real **win-asan** preset 38 assertions (the handler-thread hand-off and the synthetic-record path are ASan-clean —
no real exception is raised, so ASan's own handler stays out); full win-debug build clean (178 targets, header
changed); perf `[diag][harness]` crash specimen still verdicts `Crashed`; both validators PASS; no slice tags in the
touched engine source; no repo-root scratch or `crashes/`.

Row 073 stays **Open** (partial slice). Next sub-unit **(d)**: the hang-triggered **non-fatal** live dump — a
`HangReport` / `worker_snapshot` consumer requests a dump of self through the same writer, no terminate — closing the
"crash *and hang* capture" title and the running-thread capture DIAG.4c deferred here.

## DIAG.5a (d) — hang-triggered non-fatal live dump through the shared writer

The row's "crash *and hang* capture" and the running-thread stack capture DIAG.4c deferred here are both delivered by
one mechanism: `MiniDumpWriteDump` **is** the safe running-thread capture — it suspends every other thread and walks
their stacks safely, so a hang/watchdog observer calling it from a non-worker thread captures the stuck workers'
stacks. The cooperative worker snapshot (4c) names *what* is stuck; this dump shows *where*. No new stack-walking
code.

- **Shared writer parametrized.** `write_dump` gained a `DumpKind` (filename prefix `crash_` / `hang_` / `live_`;
  fatal stays `crash_`) and an optional `DumpNote` whose evidence blob is embedded as one `MINIDUMP_USER_STREAM`
  (`kEvidenceStreamType = 'CRDD'`, above `LastReservedStream`). The fatal path is unchanged (`Crash`, no note).
- **New `capture_dump(const DumpNote&, out_path)`** is the non-fatal entry (the old no-arg overload delegates as
  `Manual`). It **does not touch the fatal single-shot gate** and takes the same `s_dump_lock`. It is **bounded**: a
  per-process cap of 16 non-fatal dumps (a 2a-style bounded policy, a constant not a knob) — further calls return
  `Suppressed` — so a runaway observer cannot fill the disk. `install()` resets the counter per generation.
- **No default action.** jobs does **not** call `capture_dump` on hang (that would be both a default action and a
  second crash path, ADR §107). A host installs the hang handler that calls it; in the test, the test is the host.
- **`read_dump_stream(path, stream_type, out, cap)`** was added to crd-core because `dbghelp` is linked PRIVATE, so
  tests/tools cannot call `MiniDumpReadDumpStream` themselves. It maps the dump, reads one stream (a system stream
  number or a user type), copies up to `cap`, and returns the true byte count. (f) reuses it for the exception and
  module-list streams.

**Fiber-stack honesty — verified good, not a gap.** The concern was that a worker currently on a fiber stack would
be dumped with the wrong OS stack range. The crd fiber switch (`fiber_switch_win64.asm`) **saves and restores the
TEB `StackBase`/`StackLimit`** across a switch, so a suspended worker's TEB matches whichever stack it is on;
`MiniDumpWithThreadInfo` therefore captures the correct (fiber) stack for a worker executing a fiber. This is
recorded here so (e) need not "fix" a non-existent gap.

Proven by a core round-trip test and an end-to-end jobs test:
- *Core* (`test_diag_crash_contract.cpp`): a `Hang` capture returns `Ok`, the filename carries `\hang_`,
  `read_dump_stream(path, kEvidenceStreamType)` round-trips the evidence bytes exactly, and the `ThreadListStream`
  parses to `NumberOfThreads >= 1` (the dump really captured threads). A `Manual` capture carries `\live_`.
- *Jobs* (`test_diag_hang_dump.cpp`): a host hang handler, fired by the watchdog on an ExecutorStarved stall
  (`num_threads=1`, `hang_watchdog_period_ms=20`, one queued job, no pump), calls `capture_dump({Hang, evidence})`
  from the watchdog thread; the test asserts `Ok`, a `hang_*.dmp` that exists and is non-empty, and that the embedded
  `HangEvidence` round-trips (`kind == ExecutorStarved`, `executing == 0`). It uses the process-wide handler
  `main_diag.cpp` already installs (via the ambient output dir) rather than install/uninstalling its own, which would
  fight Catch2's per-test SEH filter; the one dump it creates is deleted, and `/crashes/` is gitignored.

**Residual risks recorded (ADR-accepted for the in-tree path):** `MiniDumpWriteDump` suspends all other threads, so
if a suspended thread holds the CRT heap lock and DbgHelp allocates, the dump can deadlock — the known in-process
hazard; the test's waiter spins on a plain atomic (no allocation) to stay clear of it. A multi-hundred-ms handler
makes the next watchdog window read `Paused` (overrun) — the correct re-baseline, not a bug. `capture_dump` must
never be called from a fiber (OS-stack capture plus a lock held across a possible park).

Verified: `crd-core-tests` win-debug 11 `[crash]` cases / 61 assertions (2 new) and win-asan 61; `crd-jobs-tests`
win-debug 172 cases / 29686 assertions (1 new `[hang-dump]`) and the `[hang-dump]` case win-asan (suspend-all is
ASan-clean); twin 6 / 45; full win-debug build clean (178 targets, header changed); perf `[diag][harness]` still
`Crashed`; both validators PASS; no slice tags in the touched engine source; no repo-root scratch (the gitignored
`crashes/` main_diag creates was removed).

Row 073 stays **Open** (partial slice). Next sub-unit **(e)**: the acceptance specimens (DIAG.0 subprocess harness) —
crash while holding a logger lock, concurrent crash, injected write failure, denied/full output, missing symbols,
stack overflow, fail-fast (`__fastfail` retains `0xC0000409`, no dump claimed), and a fault inside a fiber job.

## DIAG.5a (e1) — crash-capture acceptance specimen (av / concurrent / fail-fast / denied)

The fatal path is now qualified from OUTSIDE the process, closing the census's "no death/child-process test" gap for
four of the acceptance modes. One specimen binary, `crash_capture_specimen`, takes an output dir + a mode on argv,
installs the real crd crash handler, sets a **record-only** report hook (an `_Exit` from the hook would destroy the
retained fault reason), then faults; the DIAG.0 harness classifies the child's exit and the test inspects the dir it
owns. Termination is the filter chain → OS, so the exit code *is* the fault reason — the "retains original fault
reason" acceptance, verified by the parent.

Wired via `crd_diag_specimen(crd-diag-crash-capture-specimen ... crd-core Threads::Threads)`; the consumer
`tests/foundation/core/test_diag_crash_capture.cpp` links `crd-diag-harness` and gets the specimen path by compile
def. Core tests have no `main_diag` pre-install and no in-process `install()`, so there is no Catch2 SEH-filter fight.
(The `core` module's `crd_module()` gained `support/diag` in TESTS, the same declaration `perf`/`jobs` already carry,
so the module-graph verifier accepts the new link.)

Per-mode acceptance (win-debug):
- **av** — a wild null write: `Crashed`, exit `0xC0000005`, **exactly one** non-empty dump (the handler ran).
- **concurrent** — two threads fault at once: `Crashed`, exit `0xC0000005`, **exactly one** dump (the single-shot
  gate; the second fault is suppressed and still waits for the in-flight dump).
- **fastfail** — `__fastfail(FAST_FAIL_FATAL_APP_EXIT)`: `Crashed`, exit `0xC0000409`, **zero** dumps — the fault
  bypasses SEH entirely, so no dump can be claimed; the limitation *is* the assertion.
- **denied** — `install()` succeeds, the dir is then removed, the fault's dump write fails (`OpenFailed`, error
  `PATH_NOT_FOUND`, no partial): `Crashed`, exit `0xC0000005`, the dir stays absent. (Install-to-an-existing-file,
  the other denied variant, is already the (b) contract test.)

**Sanitizer branch, measured not assumed.** Under win-asan, ASan installs a vectored exception handler that owns
access violations *before* any SEH unhandled-exception filter, so the crd handler is preempted: av / concurrent /
denied are still fatal (ASan reports and exits non-zero) but write **no crd dump**. The test asserts that honest
reality under `built_with_asan()` (`exit_code != 0`, zero crd dumps) rather than skipping — the sanitizer IS the
crash reporter in that build. `fastfail` bypasses even ASan's VEH, so it is identical on both presets. (The existing
bare `crash_specimen` verdicts `Crashed` under win-asan only because it never installs our handler and its AV falls
through ASan's VEH to the OS; once our handler is installed, ASan's VEH is what runs first.)

**Recorded for (e2) — a real defect this exposed:** `do_fatal_dump` still prints via `std::fprintf(stderr, …)`. If
the faulting thread holds the CRT stream lock (crashed mid-`fprintf`), the handler thread blocks on that lock until
the 30 s `HandlerTimeout` — the logger-lock acceptance case would surface as a 30-second test. The fix is
`WriteFile(GetStdHandle(STD_ERROR_HANDLE), …)` with hand-rolled narrow formatting (the wide builders already exist;
add narrow twins). Not fixed this tick; it is the first item of (e2).

Deferred with reasons: the logger-lock, injected-write-failure (an asserts-gated seam) and fiber-fault modes need
engine changes → **(e2)**; "missing symbols" is not a specimen but an **(f)** assertion on the dump's module-list
stream (it identifies the exe by CV GUID/age; no symbolized frames are claimed — that is 5d); the stderr
"cannot print success" text claim also waits for **(f)** because `Outcome` does not retain child stderr.

Verified: `crd-core-tests` win-debug full suite 17 cases / 87 assertions (4 new `[core][diag][crash-capture]`) and
win-asan (the ASan branch exercised, 9 assertions across the 4 cases); full win-debug build clean (new specimen
target + module-graph change); perf `[diag][harness]` still `Crashed`; both validators PASS (the new specimen
registration and module declaration are accepted); no engine source touched this tick; no repo-root scratch.

Row 073 stays **Open** (partial slice). Next sub-unit **(e2)**: the modes needing engine changes — the stderr-lock
fix above (`WriteFile` on the fatal path), stack-overflow (guarantee on non-installing threads + a specimen), an
asserts-gated injected-write-failure seam, and a fault inside a fiber job (links crd-jobs).

## DIAG.5a (e2) — lock-free fatal-path output + stack-overflow / logger-lock specimens

The lock hazard (e1) recorded is fixed and guarded, and stack overflow is covered.

**The defect, proven first.** A new `logger_lock` specimen mode takes the CRT stderr stream lock (`_lock_file(stderr)`)
on the faulting thread, then faults. Run BEFORE the fix, the handler thread's `fprintf(stderr, …)` blocked on that
lock until the 30 s `HandlerTimeout`; the harness killed the child at its bound and returned **`Timeout`** — the
discriminator that confirmed the hazard.

**The fix (crash.cpp only, no header change).** Every fatal-path `std::fprintf`/`std::fflush` — the success/FAILED
lines and the ExceptionCode/Address line in `do_fatal_dump`, and both `handle_fatal` fallback messages — now writes
through `WriteFile(GetStdHandle(STD_ERROR_HANDLE), …)`, which takes no CRT lock. Narrow formatters (`emit_cstr`,
`emit_u32_dec`, fixed-width `emit_hex`) format into tiny stack buffers (stack-overflow-safe); the dump path is
narrowed once with `WideCharToMultiByte(CP_UTF8)` into a static buffer that only the single gate winner ever writes.
No flush needed (unbuffered). `application.cpp`'s startup warning and the Linux `write()` branch are unchanged.
`kHandlerWaitMs` was **not** lowered — the test's speed comes from removing the lock, not from weakening the bound.
(Console-codepage mojibake of non-ASCII path bytes on the emergency line is acceptable for a last-chance message.)

After the fix, `logger_lock` verdicts `Crashed`, exit `0xC0000005`, `timed_out == false`, and **one** dump — the
regression guard.

**Stack overflow.** A `recurse()` (non-tail, `volatile char pad[1024]` per frame, C4717 locally suppressed) drives
`STATUS_STACK_OVERFLOW` (`0xC00000FD`) in three modes:
- `overflow` (installing/main thread) → `Crashed`, `0xC00000FD`, one dump.
- `overflow_thread_guarded` (worker calls `guard_current_thread_stack()` first) → one dump.
- `overflow_thread` (worker, **no** guard) → also one dump. **Measured, and it is the important finding:** because
  (c) moved the dump onto the dedicated handler thread's fresh stack, the overflowed faulting thread only needs room
  for the tiny filter hand-off (CAS + two stores + `SetEvent` + wait), which the OS's default guard-page slack
  already covers. So `SetThreadStackGuarantee` on worker threads is **defensive, not required** for dumpability —
  which downgrades the (e3) worker-loop guard wiring to optional hardening rather than a fix.

**Sanitizer branch (measured).** Under win-asan, ASan's VEH reports the stack overflow itself → same shape as an AV
(exit ≠ 0, zero crd dumps); `logger_lock` under ASan is fatal but **not** a timeout (ASan does not write through the
CRT stream lock). Both asserted explicitly under `built_with_asan()`.

**Repository-structure fix (regression from (b)).** `install()` now creates its output directory, so the jobs/stress
test mains' `install("./crashes")` was eagerly creating a repo-root `crashes/` on every run — which
`check-repository.py` flags as an unexpected root artifact (it is gitignored, so the loop's own scratch grep missed
it). `main_diag.cpp` and `main_stress.cpp` now install under `std::filesystem::temp_directory_path() /
"crd-test-crashes"` instead; running them leaves the repo root clean.

**Residual heap-lock limitation (ADR-accepted, reasoned, not a test).** "Crash while *allocating*" is worse than the
stderr case and cannot be fixed in-process: `MiniDumpWriteDump` blocks on the faulter's heap lock *while holding*
`s_dump_lock`, so the winner's direct-fallback `TryAcquireSRWLockExclusive` fails → `HandlerTimeout`, no dump, exit
after the bound. The dedicated-thread design cannot help here by construction; the qualified external collector (a
separate future user decision per ADR-0133) is the real fix. Documented, deliberately not seeded as a slow test.

Verified: `crd-core-tests` win-debug 8 `[core][diag][crash-capture]` cases / 31 assertions (4 new: logger_lock +
three overflow) and win-asan 18 assertions (ASan branches exercised); full win-debug build clean; `crd-jobs-tests`
full 172 cases / 29686 assertions and `[hang-dump]` green (crash.cpp changed — no regression); twin 6 / 45; perf
`[diag][harness]` still `Crashed`; both validators PASS (repo-root `crashes/` no longer created); no slice tags in
the touched engine source; no repo-root scratch.

Row 073 stays **Open** (partial slice). Next sub-unit **(e3)**: the injected-write-failure seam (asserts-gated, with
a hook→marker-file so "0 dumps" distinguishes an injected failure from a handler that never ran) and a fault inside a
fiber job (links crd-jobs); optional worker-loop guard wiring as recorded above. Then **(f)**: harness dump
validation via `read_dump_stream` (exception + module-list streams) and the Open→Needs CI flip.

## DIAG.5a (e3) — injected MiniDumpWriteDump failure + fiber-fault finding

The last acceptance failure mode and the fiber probe. Worker-loop stack-guard wiring is dropped: (e2) measured that
an unguarded worker overflow already dumps (the dump runs on the handler thread's fresh stack), so it is not needed.

**Injected write failure (the `DumpFailed` step).** `denied` already proves `OpenFailed`; the acceptance also names
`MiniDumpWriteDump` *itself* failing, which only a forced FALSE can reach. A test-only, asserts-gated seam
`test_inject_write_failure(WriteResult step)` (crash.hpp/.cpp; `install()` clears it; Ok = off; Linux/fallback no-op)
makes the next write fail at a chosen step. For `DumpFailed` the real `MiniDumpWriteDump` runs, then the result is
forced FALSE so the *exact* cleanup path executes (close, `DeleteFileW` the partial, honest result). No real oracle is
weakened — every true check still runs; this only injects an additional failure under a default-off flag.

Two proofs:
- *In-process* (`test_diag_crash_contract.cpp`): inject `DumpFailed` → `capture_dump` → `DumpFailed`, zero `.dmp` in
  the dir, no partial; clear → next capture `Ok`. Exact and cheap.
- *Fatal path* (`crash_capture_specimen` mode `inject_dump_fail`): inject, then null-write → `Crashed`, `0xC0000005`,
  **zero** dumps. "Zero dumps" alone cannot distinguish an honest write failure from a handler that never ran — so the
  record-only hook drops a `report.marker` (Win32 `CreateFileA`/`WriteFile` on the handler thread, no CRT lock,
  interlocked-once) holding the decimal `WriteResult`. The test asserts the marker reads `DumpFailed`: the handler
  **ran** and **saw** the failure. This is also the machine-readable form of "failed MiniDumpWriteDump cannot print
  success" — the success line is emitted only under `wr == Ok`, and here the hook saw `DumpFailed`.

**Fiber fault — measured limitation, not a capture.** Mode `fiber` (links crd-jobs) runs a job that null-writes while
on a fiber stack. Measured 3/3: the process terminates (`0xC0000005`) but our handler **never runs** — zero dumps,
no marker. jobs installs no SEH/VEH of its own (grep-confirmed), so this is inherent: a last-chance
`SetUnhandledExceptionFilter` does not reliably fire for a fault on a fiber stack. That is exactly why `main_diag.cpp`
installs a **VEH** for its own runs ("VEH fires before the unhandled-exception filter — catches CET violations that
bypass `SetUnhandledExceptionFilter`"). Capturing fiber/CET faults needs a VEH, which the owned crash API does not
install by default because a general VEH fires on *every* C++ exception. The test asserts this honestly (like
fail-fast: the limitation is the assertion). It is not a 5a acceptance item; a VEH-based fiber/CET path is future
work. (d)'s hang-triggered dump of a fiber pool is unaffected — it dumps from a healthy non-fault thread.)

**Test platform gating.** The crash-capture cases assert Windows NTSTATUS codes, so `test_diag_crash_capture.cpp` is
now `#if defined(_WIN32)` in full (Linux crash capture with its own signal codes is DIAG.5b) — this also fixes the
latent Linux exposure of the (e1)/(e2) cases. The specimen still builds on all platforms.

Verified: `crd-core-tests` win-debug `[crash-capture]`+`[crash]` 22 cases / 105 assertions (2 new: in-process inject,
fatal-path inject; fiber added) and win-asan 22 / 88 (both branches); full win-debug build clean (header + specimen
link changed); `crd-jobs-tests` full 172 / 29686 and `[hang-dump]` stable 4/4 (one unrelated timing-sensitive live
case flaked once, green on re-run — a pre-existing property of the live watchdog tests, not this change); twin 6 / 45;
perf `[diag][harness]` still `Crashed`; both validators PASS; no slice tags in the touched engine source; no
repo-root scratch.

Row 073 stays **Open** (final sub-unit remains). Next: **(f)** — dump-content validation via `read_dump_stream` (the
`ExceptionStream` code matches the injected fault; the `ModuleListStream` names the specimen binary and carries a CV
GUID/age — the "missing symbols" acceptance answered without symbolized frames, which is 5d), the full acceptance
review, and the Open→Needs CI flip.

## DIAG.5a (f) — dump-content validation, acceptance review, Needs-CI flip (absorbing the planned (g))

The closing sub-unit. It proves the written dump is *readable and correctly identified* by reading it back through the
public `read_dump_stream`, states the full acceptance matrix with its honest gaps, and flips row 073 Open→Needs CI. No
engine code changes this tick — only tests.

**Dump-content validation.** A header-only, Windows-only probe
[`minidump_probe.hpp`](../../tests/foundation/core/minidump_probe.hpp) (included by both crash test TUs via a quoted
path — no CMake change; `<DbgHelp.h>` is pulled for MINIDUMP struct *layouts* only, so no `dbghelp.lib` link is added)
reads a dump back with `read_dump_stream`:
- `exception_code` / `exception_thread_id` decode the `ExceptionStream` (type 6) — those fields are inline in the
  stream, no RVA needed.
- `names_module_with_cv` walks the `ModuleListStream` (type 4). A module's `ModuleNameRva` and `CvRecord.Rva` are file
  offsets into the whole `.dmp` (**not** offsets into the stream), so the raw file bytes are indexed directly: the
  `MINIDUMP_STRING` name is matched case-insensitively against the crashing binary's basename, and the CV record is
  required to be RSDS (`'RSDS'` magic + a 16-byte GUID + a 4-byte age, >= 24 bytes).

That CV identity is exactly what a later symbolization step keys on — so this answers the **"missing symbols"**
acceptance (the dump carries enough for symbols to be *matched or refused* later) **without claiming a symbolized
frame**, which is DIAG.5d.

Three proofs:
- *In-process, both presets* (`test_diag_crash_contract.cpp`, `[crash]`, asserts-gated): `test_fatal_path(0xC0000005)`
  → the dump's `ExceptionStream` carries `0xC0000005` and the **faulting** thread's id (proving the id the dedicated
  handler recorded is the faulter's, not the writer's), and its module list names `crd-core-tests.exe` (resolved at
  runtime via `GetModuleFileNameW`) with an RSDS CV record. No real fault is raised, so ASan's VEH stays out and this
  holds identically on win-debug **and** win-asan — the preset-independent proof of a readable, identified dump.
- *Fatal `av` dump* (`test_diag_crash_capture.cpp`, non-ASan branch): the same exception-code and named-module checks
  on the real fatal-filter dump glob'd from the specimen run — the dump the OS-delivered fault actually produced, not
  a simulated one.
- *Write-step completeness* (contract, asserts-gated): injected `OpenFailed` and `FlushFailed` each report their exact
  step and leave zero `.dmp` — closing the mapping alongside the existing `DumpFailed` seam so a reported success is
  never a truncated dump on any failing step.

## DIAG.5a — acceptance review (design clause -> evidence; honest gaps named)

Against the [design acceptance](../design/runtime-diagnostics.md#diag-5a), scoped by
[ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md) §112–120 to the **in-tree hardened path** (no
external/paid/newly-installed collector — that remains a separate future user decision):

| Acceptance clause | Evidence | Sub-unit |
| --- | --- | --- |
| Checked install; usable-dir / existing-file / long-path | install-contract cases (Ok / OkReinstalled / OutputDirUnusable; `\?\` resolved path) | (b) |
| Honest write result — success only on a complete file | live-capture case + injected `Dump`/`Open`/`Flush` seams; the success line is emitted only under `wr == Ok` | (b)/(e3)/(f) |
| Collision-safe filenames | `CREATE_NEW` attempt loop; back-to-back live captures never collide or clobber | (b) |
| Readable, correctly identified dump | `ExceptionStream` code + faulting tid; `ModuleListStream` names the binary + RSDS CV | (f) |
| Missing symbols | CV GUID/age carried for later match-or-refuse (symbolization itself is 5d) | (f) |
| Concurrent crashes -> exactly one dump | in-process 2-thread gate (one Ok + one Suppressed, one dump) + `concurrent` specimen | (c)/(e1) |
| Off-faulting-stack dump | dedicated handler thread on its own fresh stack; hook observed to run off the faulter | (c) |
| Stack exhaustion | main-thread + unguarded-worker + guarded-worker overflow specimens all dump `0xC00000FD` | (e2) |
| Handler failure honest | injected `DumpFailed` on the fatal path — the marker proves the handler ran and saw it | (e3) |
| Denied / full output | `denied` specimen (`OpenFailed`, no partial) + injected `Open`/`Flush` seams | (e1)/(f) |
| Fault while holding the logger lock | `logger_lock` specimen — lock-free `WriteFile` fatal output, no 30 s `HandlerTimeout` | (e2) |
| Parent verifies termination + retains fault reason | every specimen `Crashed` with its NTSTATUS exit via the DIAG.0 harness | (e1)+ |
| No automatic upload | none exists; the fallback is the stderr line + a record-only hook | (a)/(b) |
| Hang-triggered live dump | `capture_dump({Hang, evidence})` + evidence-stream round-trip + jobs `[hang-dump]` test | (d) |

**Honest gaps (named, not hidden):**
- **Crash while *allocating*** — NOT met by the in-process mechanism *by construction*: `MiniDumpWriteDump` (DbgHelp)
  acquires the faulting process's heap/loader locks, so a fault raised while those are held self-deadlocks (bounded by
  the 30 s `HandlerTimeout` -> `direct_fallback_dump`, then honest termination). This is the ADR-accepted limitation
  that transfers to the future external collector — a separate user decision, not a 5a deliverable.
- **Fiber / CET faults** bypass `SetUnhandledExceptionFilter` (measured 3/3, (e3)); capturing them needs a VEH, which
  the owned API does not install by default (a general VEH fires on every C++ exception). Future work.
- **Under ASan**, the sanitizer's VEH owns access violations before our SEH filter, so on win-asan an AV is fatal with
  **no** crd dump (the sanitizer is the reporter there); asserted explicitly per mode, never skipped. Windows crd
  capture is validated on win-debug. `__fastfail` bypasses even ASan's VEH (identical on both presets).
- **Symbolized frames** are DIAG.5d; **Linux** capture is DIAG.5b; **early-startup** capture is by-design (install
  runs in `main`) and not separately seeded; a real **disk-full** volume is approximated by the injected write-step
  failures, not a physically full disk.

Also delivered under this row beyond the raw acceptance list: the hang-triggered live dump with an evidence user
stream (d); the latent double-install path fixed so `install()` is idempotent (b); the lock-free fatal-path stderr fix
without which the logger-lock case is a 30 s `Timeout` (e2).

**Verify (f):** `crd-core-tests` win-debug `[crash]`+`[crash-capture]` 24 cases / 121 assertions and win-asan 24 / 101
(the content-validation case runs on both presets); full core suite 26 / 133 win-debug; both validators PASS; the only
files touched this tick are two test TUs and one new test-only header — no engine source changed, so perf
`[diag][harness]` and every other suite are unaffected; no repo-root scratch.

Row 073 flips **Open -> Needs CI** (this is the final sub-unit). Next Open row: **DIAG.5b** — Linux signal-safe
emergency capture (sigaction/backtrace + `SA_ONSTACK`, original fault context, bounded collector failure).
