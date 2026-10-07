# DIAG.8b pause with pending jobs on the crd-jobs host provider, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8b](../ROADMAP.md#slice-diag.8b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.8b safe points](2026-10-07-diag-8b-safe-point-inspection.md).

## Goal

Second DIAG.8b batch: the acceptance clause "pause with pending jobs, resume/cancel" on the crd-jobs
`HostProvider`, with its cancel flag composed with the session's and a typed answer for bodies that run on the
provider's own sub-interpreters.

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) is the same result as the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision
recorded there stands, and the case was not run again.

## What was there (checked before coding)

- `HostProvider::execute` builds its prototype interpreter internally, so a session could not attach to it at all.
- The prototype constructor does not copy step hooks or the cancel flag into sub-interpreters. Every body the provider
  runs on a sub-interpreter (a parallel range on a pool worker, a map_reduce fold step, a pooled launch body) observed
  only the provider's cancel flag. `Session::invoke` would replace the top interpreter's flag with the session's, so a
  session cancel at a pause would have left a pooled body running and `drain_pooled` waiting for it.
- The submitting thread waits on the pool inside a parallel range or a fold (`jobs::wait`), so it has no safe point
  there; the work that can be pending at a stop is a pooled launch not yet awaited.
- `jobs::parallel_for` takes its job array from the calling thread's frame arena, which only pool threads have. The
  executing thread in the test is therefore the pool's thread 0 (as a host's main loop would be), and the controller
  is a second thread.

## What changed

- **`inspect::HostLink`** (`crd/ceir/inspect.hpp`): a host's cancel flag plus a `pending` probe, passed to a new
  `Session::invoke(in, m, entry, args, host)` overload. With a link, the interpreter observes the host's flag,
  `Session::cancel` raises it as well as the session's own, and a pause re-reads it every 2 ms (the host raises it
  without the session's lock), so a host cancel ends a paused execution. Each stop fills
  `StopRecord::pending_jobs` from the probe on the executing thread.
- **Detached bodies**: `Session::attach_detached(sub)` installs a hook that never blocks and only counts: a bound
  breakpoint hit there increments `detached_hits()` and `refused_pauses()` and records the new refusal
  `DetachedBody`. It never touches the stepping or pause-request state, so it is safe on many pool workers at once
  (the breakpoint table is read-only while an execution is attached).
- **`HostProvider::execute(ctx, m, entry, args, session)`**: the existing `execute` now forwards to a private `run`
  with no session. With one, the submitting interpreter runs through `Session::invoke` with a link to this provider's
  flag and `pooled_unjoined()` (pooled launches not yet joined by an await or join; zero after the drain). The
  session is threaded into `ParallelCtx`, `RangeJob` and `PooledToken`, so every range, fold and pooled sub is
  detached. A launch that captures an outer value still runs in-frame on the submitting interpreter and pauses there.

## Tests

`tests/execution/ceir-host/test_host_inspect.cpp` (3 cases, `[ceir][host][diag]`). The program is authored with the
builder, printed, parsed under a registered file, run through CSE, serialized and loaded into a fresh Context;
breakpoint lines are the lines of marker constants found by scanning the text. The controller thread records what it
saw and the test thread checks it after both have ended. When its script is done the controller waits for the
provider to return; one that does not is cancelled through both flags (the backstop), and the test fails on
`backstop()`, so a broken cancel path fails instead of hanging.

- **Pending and resume** (`@main`): seven line breakpoints bind, including those in a pooled launch body, a
  parallel_for body and a map_reduce combine. The run stops in the in-frame launch body with `pending_jobs == 1` (the
  pooled launch), where the pooled body's marker reads `OutOfScope`; then at the line after the map_reduce with
  `pending_jobs == 1` and the map_reduce result available (6); then after the awaits with `pending_jobs == 0` and the
  awaited value available (7). The result is 23, the launch pooled once, and the detached bodies were counted, not
  paused: 1 launch body, 8 ranges and 4 fold steps give 13 detached hits, 13 refused pauses, last refusal
  `DetachedBody`.
- **Session cancel** (`@spin`): a pooled body loops 2^31 - 1 times with fuel for 2^40 steps, so only a cancel can
  end it. The run stops at the next line with `pending_jobs == 1`; `Session::cancel` ends it with `Cancelled` on the
  held op, and the provider returns without the backstop.
- **Provider cancel**: the same stop, but the controller calls `HostProvider::request_cancel()` instead; the paused
  execution ends with `Cancelled` on the held op, without the backstop.

## Teeth (win-debug)

Each break was applied, `crd-ceir-host-tests` rebuilt and the three new cases run, then the source restored (written
back, so its mtime moved), rebuilt and rerun green (104 assertions in 3 cases).

| Break | Result |
|---|---|
| `Session::cancel` does not raise the host's flag | session cancel case fails: the backstop had to end the run, 3 assertions |
| The pause does not read the host's flag | provider cancel case fails: the backstop had to end the run, 2 assertions |
| Range, fold and pooled subs not detached | pending case fails: no detached hits, 3 assertions |
| No pending probe in the link | pending and session cancel cases fail: `pending_jobs` reads 0, 3 assertions |
| Submitting interpreter keeps the session's own flag | provider cancel case fails: the held op ran, 1 assertion |
| Provider ignores the session | all 3 cases fail: nothing stops |

## Evidence

All on the final sources (after the teeth, only a comment in `inspect.hpp` was rewrapped, the test's controller
lambdas became named locals and one line was wrapped; every lane below was built after that).

- **win-debug:** `crd-ceir-host-tests` 32 cases (1,026 assertions), `crd-ceir-tests` 548 (13,094) and
  `crd-ceir-cook-tests` 42 (1,964) pass. The 8 ctest guards pass. `inspect.hpp` and `host_provider.hpp` are included
  only by crd-ceir, crd-ceir-host and these three test executables, so no whole-tree build was needed.
- **win-shipping, win-clang-cl-shipping (clean thin-LTO links), win-asan (inside vcvars, no ASan report):** the three
  suites build; `crd-ceir-host-tests` (32 cases) and the `[diag]` cases of `crd-ceir-tests` (14) and
  `crd-ceir-cook-tests` (5) pass. The new cases passed 15 times in a row on win-debug, 8 on win-shipping and 5 on
  win-asan.
- **WSL:** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan each build and pass all three suites with the same
  counts and no sanitizer report; the `[diag]` host cases passed 5 more times under TSan with no report, which covers
  the detached hook on pool workers and the pause re-reading the provider's flag.
- **Checks:** strict tidy is clean on the 5 changed C++ files; clang-format `--dry-run` shows only the repository's
  hand alignment on new code; the Allman check, check-master-plan and check-repository pass.

Hosted CI must show `crd-ceir-host-tests` (32 cases, 1,026 assertions) green on all six lanes with its three
`diag 8b` cases passing, and `crd-ceir-tests` (548) and `crd-ceir-cook-tests` (42) still green.

## Remaining on DIAG.8b (local)

- **GPU work is non-pausable at the real seam**: a dispatch already submitted by `execute_lowered` cannot stop, so the
  GPU seam must classify its dispatches as non-pausable instead of leaving it to the session's declared scope.
- **Consumers**: no sandbox or headless consumer executes a plan or interpreter program today, so "inspect/step an
  authored program in headless and sandbox consumers" needs a consumer that runs one.
