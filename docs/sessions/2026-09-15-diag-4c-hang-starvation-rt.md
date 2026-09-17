# DIAG.4c — hangs, starvation and real-time violations (DG07): full-acceptance review

<!-- doc-role: historical -->

Owner slice: [DIAG.4c](../ROADMAP.md#slice-diag.4c). Contract: [runtime-diagnostics
design](../design/runtime-diagnostics.md#diag-4c); [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md).
Implementation narrative (sub-pieces a–e, tick by tick): the DIAG.4c sections of
[the running diagnostics session doc](2026-09-14-diag-1a-scheduler-free-list-races.md) — "structured job &
observer lifetime (4a)", the wait-graph/watchdog build (a/b), "distinct hang kinds + seeded-deadlock specimen (c)",
its three continuations (pool exhaustion, priority starvation, livelock), "portable real-time sentinels (d)" and
"cooperative worker snapshot (e)". This doc is the closing (f): it maps the design's acceptance clause-by-clause to
the landed tests and records the honest scope boundaries, then flips the row to Needs CI.

## What the acceptance requires

From the [design](../design/runtime-diagnostics.md#diag-4c):

> Acceptance: seeded deadlock, livelock, priority starvation, pool exhaustion and forbidden blocking/allocation yield
> distinct reports. Long progressing offline work, debugger pauses, suspend/resume and clock changes do not trigger a
> false deadlock claim. Unresponsive processes have a bounded snapshot attempt and an honest incomplete result.

## Distinct reports — each acceptance kind, its signal, and the landed test

| # | Acceptance kind | Distinct report / signal | Landed test (file · case) |
|---|---|---|---|
| 1 | seeded deadlock | `HangKind::WaitCycle` from a wait-graph cycle (Warshall closure over own→waiting-on edges, checked first) | `test_diag_deadlock_specimen.cpp` · "a seeded wait-cycle deadlock is classified as WaitCycle" (subprocess, `_Exit(42)`); `test_diag_hang_watchdog.cpp` · "classify_hang: a wait cycle among parked fibers is a deadlock" |
| 2 | livelock | opt-in per-task progress epochs; `LivelockReport` fires on a flat epoch across K windows while a worker stays busy | `test_diag_livelock.cpp` · "a mutual livelock is reported for both tasks and no hang fires" (+ the `LivelockTracker` decision table, 5 cases) |
| 3 | priority starvation | per-lane `StarvationReport` (backlog + no pops while the system progresses); strict High→Normal→Low drain, no aging | `test_diag_starvation.cpp` · "a Low lane flooded by Normal work is reported as starved" (+ the `starvation_verdict` decision table, 5 cases) |
| 4 | pool exhaustion | counted, always-on `CRD_FATAL` at the callers (`submit_jobs`, `run_job_in_fiber`); `exhaustions()` tally surfaced on `ProgressSample`/`HangReport` | `test_diag_pool_exhaustion.cpp` · "counter-pool exhaustion is a counted, always-on fatal" / "fiber-pool exhaustion is a counted, always-on fatal" (subprocess) + "a healthy pool reports zero exhaustions" |
| 5 | forbidden blocking/allocation | `RtScope` + `RtViolation{Allocation,Blocking}`; alloc check at the `MemoryStats::on_allocate` seam, block check at the top of `jobs::wait()` | `test_diag_rt_sentinel.cpp` · 6 cases (alloc in-scope, nested scopes, out-of-scope silent, raw-thread off-fiber, `wait` reported Blocking, distinctness) |

The three parked-evidence classifications underneath (1) are separately pinned:
`test_diag_hang_watchdog.cpp` · "classify_hang: nothing parked but work outstanding is executor-starved"
(`ExecutorStarved`) and "parked fibers with no closing cycle are parked-stalled" (`ParkedStalled`).

## Distinctness — the reports do not cross-fire

- **RT violation fires only the RT handler** while the watchdog is live with the hang/starvation/livelock handlers
  all installed: `test_diag_rt_sentinel.cpp` · "a violation is distinct -- hang/starvation/livelock stay silent".
- **Livelock is silent to the hang detector**: a spinning livelocked task keeps a worker executing (reads
  Progressing), so `test_diag_livelock.cpp` · "a mutual livelock is reported for both tasks and no hang fires"
  proves the livelock handler fires while the hang handler does not.
- **Exhaustion is not a survivable watchdog state**: it fatals at the caller, so there is deliberately no
  `HangKind::PoolExhausted` enum value the watchdog could emit as a fake report.

## No false deadlock claim — the four non-fault shapes

| Shape | How it is not a false positive | Evidence |
|---|---|---|
| Long progressing offline work | a worker executing with flat completions reads Progressing, not a hang | `test_diag_hang_watchdog.cpp` · "long executing work over many windows never fires" + "a long job executing with flat completions is progress, not a hang"; `test_diag_livelock.cpp` · "an unmonitored long job is never flagged (opt-in)" |
| Debugger pause | a badly overrun watchdog window is `Paused` (checked first, re-baselined, never counted) | `test_diag_hang_watchdog.cpp` · "a badly overrun window is Paused (debugger/suspend), not a hang" (500 ms expected vs 5000 ms actual) |
| Suspend / resume | same freeze shape as a debugger pause → `Paused`; the livelock check is also skipped for a Paused window (a frozen process flatlines every epoch) | same `Paused` case above; `jobs.cpp` livelock feed guarded on `v != Paused` |
| Clock changes | **by construction** — every watchdog/snapshot/timeout interval is a `steady_clock` delta, immune to wall-clock jumps; there is no seeded test because a monotonic clock has no wall-clock input to perturb | `hang_watchdog.hpp` and `jobs.cpp` use `std::chrono::steady_clock` throughout; stated here rather than asserted |

## Unresponsive process — bounded attempt, honest incomplete

The parked half of the safe stop/snapshot protocol is the wait graph (`test_diag_wait_graph.cpp`, 2 cases,
truncation-honest). The running half is the cooperative worker snapshot: `worker_snapshot(span, timeout_ms)` bumps a
generation, wakes every worker, and polls a steady clock until each background worker acks at its loop-top safe
point or the deadline elapses; a worker stuck inside one job never acks, so the result is honestly incomplete while
still naming the task it is stuck on. `test_diag_worker_snapshot.cpp` · 4 cases: an idle pool is complete; a seeded
stuck worker is `responsive == false`, `executing == true`, `current_task_id == the spin id`, and the result is
`complete == false`; the attempt is time-bounded (returns near the ~100 ms deadline, never hangs); truncation
reports the true worker count past a smaller buffer.

## Honest scope boundaries (design-body items beyond the acceptance gate)

These are named so the flip does not overclaim. They are design-body language, not acceptance clauses; each is
either deferred to a named later slice or an explicitly optional path.

- **Task deadline records** — progress *epochs* landed (the livelock signal); wall-time deadline records are not
  implemented (deadlines are not in the acceptance list). Deferred.
- **Lock/owner diagnostics** — only the wait-graph *counter* owner edges landed (a parked fiber → the `Counter*`
  it waits on). General mutex/lock ownership attribution is not implemented here; it belongs with the crash-capture
  lock-ownership work in DIAG.5a/5b.
- **Optional external process observer** — not landed; the watchdog runs in-process on its own thread outside the
  affected pool, which satisfies "a watchdog outside the affected pool". The external observer is the design's
  optional variant.
- **Actual running-thread stack capture** — deferred to the OS-specific, qualified crash-capture layer
  (DIAG.5a Windows / DIAG.5b Linux). Cooperative own-stack capture at a worker's loop top would yield only
  scheduler frames for responsive workers and nothing for a stuck one, so it was deliberately not attempted here.
- **RTSan** — the portable no-allocation/no-blocking sentinels landed (`CRD_ENABLE_ASSERTS`-gated; compiled out of
  a shipping build). RTSan is the design's "optional after tool qualification" path, not required by the acceptance.
- **TSan qualification of the new plain snapshot reads** — the wait-graph / lane / monitored / worker snapshots use
  relaxed/racy dumps by design; their instrumented-path qualification rides the hosted `linux-clang-tsan` lane set
  up in DIAG.1b, not the Windows build.
- **RT allocation seam coverage** — the `Allocation` check sits at `MemoryStats::on_allocate`, the single shared
  seam every concrete allocator already calls, so it covers both the `allocate()` and `try_allocate()` paths
  (spot-checked against `MallocAllocator`) rather than editing each allocator override.

## Verification (local, at the (e) close — the last engine-changing tick)

- `crd-jobs-tests` win-debug: 29676 assertions / 171 cases green.
- `crd-jobs-tests` under the real `win-asan` preset: 29676 / 171 (poller/worker ack races and the live spins are
  ASan-clean, stable across repeated runs); the instrumented deadlock/exhaustion subprocess children are ASan-clean.
- Twin harness `crd-jobs-schedcheck-tests`: 6 cases / 45 assertions (gate-off engine behaviour unchanged).
- `crd-core-tests` 12/2, `crd-memory-tests` 820203 / 140 (`memory_stats.hpp` is widely included — no regression).
- `check-master-plan.py` and `check-repository.py`: both PASS.
- No `DIAG.N` / `DGnn` / `#diag-N` slice tags in engine source (test files and CMake may carry them); no repo-root
  scratch.

(f) is a documentation-only tick: this review plus the row flip. No engine source changed, so the (e) verification
above is the standing state.

## Row flip

DIAG.4c row 072 → **Needs CI**, with this Session link and the latest published CI run
(`actions/runs/34897728374`, the same run DIAG.4a/4b cite; it is red only on the maintainer-owned repo-hardening
preset, off-limits to this loop, and turns Done on the next green). The next Open row is
[DIAG.5a](../ROADMAP.md#slice-diag.5a) — reliable Windows external/OS crash and hang capture (DG09), which is also
the home of the running-thread stack capture and lock-owner diagnostics deferred above.

## CI hardening (2026-09-15): linux-gcc-debug `-Werror` debt from the `exhaustions` field

Discovered while building the DIAG slices under WSL (Ubuntu 24.04, g++ 13.3, glibc 2.39) for DIAG.5b: the
`linux-gcc-debug` lane is red for reasons **beyond** the maintainer-owned repo-hardening preset noted above. The DIAG
loop had been building only `win-debug` (MSVC), which does **not** warn on several things GCC `-Werror` rejects, so
committed DIAG code accumulated Linux-only violations. Concretely, DIAG.4c added `ProgressSample::exhaustions` (a 5th
field) but left aggregate-init sites at four initializers → `-Werror=missing-field-initializers` under `-Wextra`.

Fixed this tick (Windows-verified: `crd-jobs-tests` 172 cases / 29686 assertions green; `jobs.cpp` also
`-fsyntax-only`-clean under the full CI flag set on g++ 13.3):
- `engine/foundation/jobs/src/jobs.cpp:571` — `ProgressSample s{0U, 0U, 0U, true}` → `{…, true, 0U}` (the early-return
  path needs `quiescent = true`, so a plain `{}` value-init would be wrong; the explicit 5th initializer preserves it).
- `tests/foundation/jobs/test_diag_hang_watchdog.cpp` — 10 `ProgressSample{…}` comparison fixtures gained the neutral
  `exhaustions = 0U`.
- `tests/foundation/jobs/test_diag_hang_dump.cpp` — gated to `#if defined(_WIN32)`: it asserts `capture_dump({Hang,…})
  == Ok`, which is `Unsupported` on Linux (the Linux crash record is signal-handler-driven — DIAG.5b), so it was a
  runtime failure waiting on the Linux lane. Platform scoping, not weakening (the cross-platform hang watchdog stays
  covered by `test_diag_hang_watchdog.cpp`).

**Fixed the next tick (2026-09-15) — the Linux lane now compiles the whole DIAG tree:**
`engine/foundation/memory/src/allocators/diagnostic_allocator.cpp:115` — `std::memset(m_records, …)` over the
non-trivial `Record` (its `AllocationTag` has default member initializers) → `-Werror=class-memaccess`. Replaced with a
placement-new value-init loop (`::new (&m_records[i]) Record{}`); verified byte-equivalent to the old zero-fill
(`AllocationTag` = three `u32`s defaulting to 0, `StackId` = `u32` default 0, `state = kStateEmpty = 0`, `user =
nullptr`). `StackEntry`/`QuarantineEntry` memsets left as-is (trivially-copyable, not flagged). Windows regression:
`crd-memory-tests` win-debug + **win-asan 820203 assertions / 140 cases** (the allocator is the ASan lane's subject),
core 26/133, jobs 172/29686.

One further same-class site surfaced once crd-memory unblocked: `tests/foundation/core/test_diag_crash_contract.cpp`
`count_dumps()` was `-Werror=unused-function` on non-Windows (used only in the `_WIN32` cases) → marked
`[[maybe_unused]]`. With that, **all three DIAG test targets (`crd-core-tests`, `crd-jobs-tests`,
`crd-diag-crash-capture-specimen`) build clean on `linux-gcc-debug` under the full `-Werror` set** — the first time the
DIAG tree has compiled on Linux. First-ever Linux runs under WSL: `crd-core-tests [crash]` 3/3 (the non-Windows
`capture_dump == Unsupported` contract holds with the new handler); `crd-jobs-tests` 170/171 — the one failure is
`test_diag_worker_snapshot` (`responded < expected`), most likely a **timing effect** (the cooperative-snapshot poll
timeout is tight for DrvFs-slow WSL scheduling; the failing count varies run to run) in a test this loop did not touch —
reported, not "fixed" by loosening an oracle. It did **not** reproduce on Windows (172/172); whether it also flakes on
the CI runner is what run `34939903567`'s `linux-gcc-debug` job will show. If it does, that is a tolerance question for
this row (DIAG.4c), to be flagged and handled there — not patched inside a 5b tick. Row 072 stays Needs CI (its flip
waits on a green published run).
