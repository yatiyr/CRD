# DIAG.1a — scheduler free-list `next_free` races (DG05)

<!-- doc-role: historical -->

Owner slice: [DIAG.1a](../ROADMAP.md#slice-diag.1a). Contract: [runtime-diagnostics
design](../design/runtime-diagnostics.md#diag-1a); [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md).
DG05 transferred here from CORE-USE.2 (this is the primary repair owner; CORE-USE.2 later
verifies renderer integration, not a duplicate repair queue).

## The defect

Both scheduler pools are lock-free Treiber stacks whose head (`FiberPool::Tier::free_head`,
`CounterPool::m_free_head`) is a `std::atomic<u64>` packing `(index, generation)`; the
generation tag is bumped on every pop and defeats ABA. The **link** field, however, was a
plain non-atomic `crd::u32 next_free`:

- `engine/foundation/jobs/src/fiber.hpp` — `Fiber::next_free`
- `engine/foundation/jobs/src/counter.hpp` — `Counter::next_free`

`acquire()` reads `fibers[idx].next_free` between the head load and the CAS, while a
concurrent `release()` of that same node writes `next_free`. Even though the algorithm is
ABA-correct (a losing CAS discards the stale read), the plain load and plain store on the
same `next_free` object with no intervening synchronisation **are a C++ data race** — UB by
the standard, and exactly what TSan reports (DG05). "It works on x86" is not a fix.

## The repair

`next_free` is now `std::atomic<crd::u32>` in both structs, accessed with
`memory_order_relaxed` at all seven sites (`fiber_pool.cpp`: init/pop/push;
`counter.cpp`: init/pop/acquire-clear/push).

Why relaxed is sufficient — the linearization/ABA argument (documented in the source):

- **Ordering** is carried by the head, not the link. A pusher does a relaxed
  `next_free.store()` then a **release** CAS on the head that publishes both the new index
  and that store; a popper does an **acquire** load/CAS on the head that synchronises-with
  it, so the popper's subsequent relaxed `next_free.load()` observes the pushed successor.
- **Data race** is removed because atomic-vs-atomic accesses are never a data race
  regardless of order; relaxed is the minimum that keeps the fast path cheap.
- **ABA / uniqueness** are unchanged: the read successor is only installed if the CAS sees
  an unchanged `(index, generation)` head; every pop bumps the generation, so an interleaved
  pop+push restoring the same index cannot restore the same tag. A node is on the free list
  or acquired, never both. Linearization point is the successful CAS.

Atomic links were the design's named candidate — and here they are sufficient because the
reclamation model (fixed `unique_ptr<T[]>` backing store, never freed until shutdown, plus
the generation tag) already solves reuse/ownership. No node is ever deallocated while a
racer could touch it, so no hazard-pointer / epoch scheme is required.

`Fiber` gains an atomic member and so is no longer trivially assignable (correct: a live
pool descriptor must never be copied). Two test fixtures that reset via `= Fiber{}` now
reconstruct in place (`~Fiber(); new(&f) Fiber{};`).

## Proof (local, win-debug)

- `crd-jobs` + `crd-jobs-tests` build clean under `/W4 /WX /permissive-`.
- New adversarial stress tests (`[jobs][...][stress][diag]`), both green:
  - `fiber_pool: DG05 exhaustion/reclamation stress preserves uniqueness and completion`
  - `counter_pool: DG05 exhaustion/reclamation stress preserves uniqueness and completion`
  Each oversubscribes the pool (6 threads × batch 6 > 24 slots) so the free list is drained
  to empty and refilled for 6 000 iterations, with reverse/forward release order to churn
  link ordering. Invariants asserted: no `pool_index` held by two threads at once
  (uniqueness), acquired values survive round-trips, the pool is fully restored and the list
  is still exactly drainable afterward (completion).
- Full `jobs`/`counter`/`fiber` ctest set passes with no regressions.

The instrumented-path proof (TSan reports on the original code disappearing with **no
suppression**) is qualified on the hosted TSan lane by the next slice, [DIAG.1b](../ROADMAP.md#slice-diag.1b),
which owns the TSan preset/CI and the happens-before model with ordered and deliberately
racy negative controls.

## DIAG.1b — TSan preset + fiber happens-before positive controls

Landed alongside 1a: the `linux-clang-tsan` configure/build/test presets (the `CRD_ENABLE_TSAN`
`-fsanitize=thread` machinery and the `__tsan_*_fiber` model in `sanitizer_fibers.hpp` already
existed) and, in CI, `linux-clang-tsan` registered as a **diagnostic-tier** preset in
`.github/ci-tiers.json` (fixing the preflight "no tier owner" regression the preset first
introduced). Positive controls (`tests/foundation/jobs/test_tsan_fiber_model.cpp`,
`[jobs][tsan][diag]`) pass on win-debug: ordered write→wait→read across fibers, raw-thread/fiber
equivalence, and a migrating hand-off chain — all happens-before clean by construction, so under
the TSan lane they certify the model stays silent on correct orderings.

The negative control has since landed as well: `tests/support/diag/specimens/data_race_specimen.cpp`
(two threads racing a plain, unsynchronised int) built through the DIAG.0 specimen harness and driven by
`tests/foundation/jobs/test_tsan_race_specimen.cpp` (`[jobs][tsan][diag][harness]`). It asserts the
harness verdict is **SanitizerCaught** when built with `-fsanitize=thread` and **InstrumentAbsent**
otherwise — never a silent pass, mirroring the DIAG.0 ASan heap-overflow specimen. `specimen_common.hpp`
gained ThreadSanitizer detection (`CRD_DIAG_HAS_TSAN`, distinct from the ASan path). On win-debug the
verdict is InstrumentAbsent (green, 4 `[tsan]` cases). The only remaining piece is the hosted
linux-clang-tsan lane actually building the specimen with TSan and observing the catch.

## DIAG.4a — structured job & observer lifetime (DG07)

Hardened the **current scheduler's** ownership/lifetime contracts — no new schedule-control
features (those are DIAG.4b/4c). What landed:

- **Task identity.** `Counter::task_id` (`engine/foundation/jobs/src/counter.hpp`) — a unique id
  stamped per `CounterPool::acquire` (`counter.cpp`), monotonic from `m_next_task_id`, independent of
  the recycled counter/fiber *address*. Public `crd::jobs::current_task_id()` (`jobs.cpp`) derives it
  from the running fiber's `job_counter`, so it is migration-safe and the parent's id reappears when a
  nested job returns — no thread-local bookkeeping.
- **Causal parent edge.** `Counter::parent_task_id`, stamped in `acquire()` from `current_task_id()`
  in the submitting fiber's context (0 from the main thread); public `crd::jobs::parent_task_id()`.
  Publication/resume/end edges are already emitted by the `JobObserver`
  (`on_job_begin`/`on_fiber_resume`/`on_job_end`); the cancel edge is the internal
  `WaiterClaim::Canceled` transition, kept internal.
- **Observer retirement = checked quiescence.** `crd::jobs::is_quiescent()` (uninitialised, or every
  counter free); `set_observer` now `CRD_FATAL`s on a mid-flight swap (`observer.cpp`) so a job can
  never begin under one observer and end under another. The stale "consistent snapshot" comment at
  `worker_pool.cpp` was corrected to the real invariant.
- **Cancellation & drain contract.** Documented as a contract block above `WaiterClaim`
  (`counter.hpp`): park-cancel is the sole cancellation transition; the `park_finalized` handshake
  (Canceled) and steal-once + `on_job_end`-before-decrement (Wakeup) guarantee no owner is released
  while a finalizer/callback still runs; the only bulk drain is `shutdown()` with pending work.
  Verified against the code, not merely asserted.
- **No-blocking-context contract.** `wait()` (`jobs.cpp`) now `CRD_FATAL`s when an unenrolled thread
  waits on a single-thread pool (a guaranteed deadlock); the soft thread-0-pinned hazard stays
  documented (affinity is not visible inside `wait()`).

### Acceptance -> test

| Acceptance item | Test |
| --- | --- |
| one worker | `test_jobs.cpp` (`num_threads=1` public-api cases) |
| nested jobs | `test_diag_task_identity.cpp` (nested id-restore, parent edge); `test_diag_observer_lifetime.cpp` |
| migration | `test_tsan_fiber_model.cpp`; `test_jobs.cpp` cross-thread resume stress |
| pool exhaustion | `test_fiber_pool.cpp` / `test_counter.cpp` DG05 stress |
| shutdown with pending work | `test_jobs.cpp` |
| cancellation at each transition | `test_counter.cpp` Canceled-discard; the race under the stress test + TSan model |
| observer unload/replacement | `test_diag_observer_lifetime.cpp` + `observer_swap_specimen` (subprocess) |
| completion-before-wait-return | `tests/foundation/perf/test_jobs_adapter.cpp` (`jobs_ended == jobs_begun`) |
| pooled, not sequential | `test_diag_task_identity.cpp` (a batch runs on >1 `worker_index`) |
| no-blocking-context (negative) | `unenrolled_wait_specimen` (subprocess) |

### Scope decisions

- **Cancellation = formalize, not add.** The design says complete the *current scheduler's* contracts;
  the scheduler has one cancellation transition (`WaiterClaim::Canceled`) and no job-cancel API. Adding
  one is schedule control (4b/4c). So 4a documents and checks the existing transition rather than
  inventing an API.
- **Subprocess fatals use a platform handler, not a `Crashed` verdict.** A `CRD_FATAL` cannot be read
  as the harness's `Crashed`: on Windows its default path pops a modal assert dialog (headless -> hang),
  and via `std::abort()` the exit code (3) is below the harness's crash threshold (`>= 0xC0000000`);
  the harness captures stdout only, so the message is not a harness-level oracle. So
  `observer_swap_specimen` and `unenrolled_wait_specimen` install the documented
  `set_assert_platform_handler`, verify the fatal's message, and `_Exit(42)`; the consuming tests
  assert that sentinel exit. A non-firing check exits 0 (or hangs -> timeout), which the tests reject.

Local proof (win-debug): full `crd-jobs-tests` — 120 cases / 29 501 assertions green; both validators PASS.

## Follow-up (DIAG.4b twin): premature-wake / zero-drain-release race in counter_decrement

The DIAG.4b controlled-interleaving twin (`crd-jobs-schedcheck`, gate `CRD_JOBS_SCHED_CHECK`) with its
seeded `dec.zero` perturbation surfaced an intermittent `CRD_ASSERT` at
`counter.cpp` — `counter_decrement: underflow — decremented past zero` (`old_val == 0`). In this
handler-less test binary a firing assert pops a modal `MessageBoxA`, so it presented as a ~0-CPU hang
(hence the earlier "lost-wakeup" framing); a temporary `set_assert_platform_handler` that hard-exits
turned the "hang" into a stack-located assert, which is how it was found.

**Mechanism (a real production bug, all builds).** `counter_decrement` does `value.fetch_sub` (its
transition to zero) and only *later* `waiters.exchange(nullptr)` — its last touch of the `Counter`.
Between those two steps the value is already zero, so a fast-path (`counter_wait`'s `value == target`
early return) or an ABA-cancelled waiter can let `jobs::wait()` reach `CounterPool::release()`. The slot
is recycled, a concurrent `run()` re-acquires it (new generation, `value = 1`, `waiters = nullptr`) and
parks a waiter, and the *stale* decrementer's `exchange` then steals the **new** generation's waiter —
a premature wake that returns `run_and_wait` while its child has not run, releases the slot early, and
cascades into a decrement past zero on a later generation. `park_finalized` gates release against
`counter_finish_park`, but nothing gated release against the zero-decrementer's drain. The window is
exactly `fetch_sub` → `exchange`, which is where the twin's `dec.zero` yield sits.

Confirmed empirically on the twin: a gate-only capture showed the counter's `task_id` changing across
that window (`STALE-DRAIN`) in ~8% of runs, `violations == 0` (outside any `finish_park` window),
balanced acquire/release and body counts (no double-dispatch, no double-free of a slot) — i.e. a single
generation validly acquired and released, just too early.

**Fix (~6 lines, no assert weakened).** A `drained` flag on `Counter` (repurposes the former alignment
pad; 64-byte layout unchanged): reset in `acquire()`, set with a release store in `counter_decrement`
immediately after the `waiters.exchange`, and `jobs::wait()` spins on it (acquire) before `release()` —
the single choke point covering the fast-path, spin, and both resume paths, mirroring `counter_wait`'s
existing `park_finalized` spin. `release()`'s comment now names the precondition and who enforces it.

**Verification.** Twin `[.lostwake]` 250x: 0 asserts, 0 hangs, 0 `STALE-DRAIN` (was ~6% underflow +
~8% stale before). Default twin harness 6 cases / 45 assertions green. Gate-off `crd-jobs-tests`
121 cases / 29 502 assertions green. `ctest --preset win-asan -R jobs` 28/28. Both validators PASS.

Still open (next unit): fold the `dec.zero` perturbation into the committed stress as a permanent
regression guard (or script this exact interleaving in the driver as a FOUND case), rename the
`[.lostwake]` case/comments (the "lost-wakeup" label is now inaccurate), and keep a stress-side assert
platform handler so a future regression is a bounded exit rather than a modal-dialog hang.

## DIAG.4b complete: regression guard landed, row → Needs CI

The `dec.zero` perturbation that surfaced the zero-drain/release race is now folded into the committed
schedcheck stress (`test_sched_check.cpp` case 6, renamed "perturbed multicore park/reclaim stress — the
detector stays silent"): the free-running stress applies the seeded ~1/4 yield jitter at every yield
point, records a per-worker last tag, and installs both a 20 s watchdog and an assert platform handler
that dump the stress state and hard-exit (3 / 42) so a firing assert is a bounded, diagnosable exit
rather than a modal-dialog hang. The hidden `[.lostwake]` case and its `g_lw_*`/`task_e8a97827` scaffold
are deleted; the one `StressState` (module-global so the captureless handler can read it) replaces the
old counting-only `StressOracle`. The GenMC model header now states it scopes to the park/publish/reclaim
handshake and does **not** model the zero-drain/release gate (found by this stress, not the model) — the
model is not extended, since a GenMC change cannot be qualified on the Windows dev box.

With the `Counter::drained` fix in place the guard is silent: perturbed case 6 ran 250× standalone and
30× in full-harness context with zero asserts and zero hangs (before the fix: ~6% underflow, ~8%
stale-drain). Twin harness 6 cases / 45 assertions; gate-off `crd-jobs-tests` 121 / 29502; both
validators PASS.

All DIAG.4b acceptance items — controlled replay, delta-minimized failing schedules, broken/repaired
detection, a bounded weak-memory model, and real multicore stress — are delivered, so the row is flipped
Open → Needs CI citing run 34897728374 (still red on the off-limits preset-contract failure; it goes
Done on the next green run, mirroring DIAG.1a–4a).

## DIAG.4c (c) — distinct hang kinds + seeded-deadlock specimen

Building on the wait-graph snapshot (a) and the progress-sensitive watchdog (b), a hang report now carries a
classified *kind*. `HangKind { ExecutorStarved, ParkedStalled, WaitCycle }` (public, in `jobs.hpp`) is decided
by the pure `detail::classify_hang(parked, outstanding, executing)` in `src/hang_watchdog.hpp` from the
parked-fiber evidence alone. `WaitGraphNode` gained `own_task_id` (the parked fiber's own job-counter id, the
edge *source*); an edge i→j exists when `own_task_id[j] == waiting_on_task_id[i]` (owner id non-zero). The
classifier builds successor masks, takes a Warshall transitive closure over ≥1-edge reachability (n ≤ 64, u64
masks), and reports **WaitCycle** if any node is reachable from itself (checked first — a cycle is a deadlock
regardless of what else is queued; handles shared owner ids, self-edges, and 0-id chain ends). With no cycle:
nothing parked → **ExecutorStarved** (work queued, no worker draining it); parked fibers → **ParkedStalled**
(blocked on work never dispatched). The watchdog sets `report.kind` before calling the handler.

Livelock, priority starvation and pool exhaustion are the acceptance's other three kinds and are **deliberately
not faked** from the wait graph: livelock needs job-declared progress (a spinner reads Progressing), starvation
needs a per-lane oldest-waiting age (a scheduler signal), and exhaustion needs an acquire-failure event counter
in the pools. Each is a distinct future unit; recorded, not guessed at.

**WaitCycle proven end-to-end by a subprocess specimen.** A live deadlock cannot be exercised in-process —
`CounterPool::shutdown` asserts on still-acquired counters — so `crd-diag-deadlock-specimen` runs as a bounded
child through the DIAG.0 harness. Two sibling jobs each park on the other's completion counter (each spins
until the other counter is published, so neither parks early); the watchdog fires on its own thread and
`_Exit(42)` on `kind == WaitCycle` (96 = other kind, 97 = stray assert, 0 = never fired). It cannot misclassify:
a still-spinning job keeps its worker *executing*, so that window reads Progressing and resets the debouncer —
the K-th stale window only lands once both fibers are parked and the snapshot holds the full two-node cycle.
Direct run: `KIND=2` (WaitCycle), `PARKED_TOTAL=2`, exit 42. The two in-process live-fire tests now also assert
kind (queued-no-executor → ExecutorStarved; parked-over-un-run-child → ParkedStalled).

Verified: win-debug full jobs 29563 / 140 cases; win-asan full jobs 29563 / 140 (ASan-clean, including the
instrumented deadlock child and the concurrent snapshot); DIAG harness 4 / 7 (deadlock specimen added); twin
6 / 45 (no engine-src change in the specimen sub-piece); both validators PASS; no slice tags in engine source;
no repo-root scratch. DIAG.4c stays **Open** — the acceptance matrix (five distinct kinds; no false positive on
offline/pause/clock) is not yet complete: the three signal-dependent kinds and (d) RT sentinels / (e) safe
snapshot / (f) doc+flip remain.

## DIAG.4c (c cont.) — pool exhaustion as a distinct, always-on, counted fatal

The fourth acceptance kind. Both pools now tally exhaustion events — `CounterPool` and `FiberPool` each gained
an `std::atomic<u32> m_exhaustions` bumped on the acquire fail path, with an `exhaustions()` getter — surfaced
as `ProgressSample::exhaustions` / `HangReport::exhaustions` (appended trailing; the existing 4-field positional
literals value-init the new member) = `counter_pool.exhaustions() + fiber_pool.exhaustions()`.

Crucially, `acquire()` KEEPS its documented contract: assert-with-Ignore then **return nullptr**. The DG05
exhaustion/reclamation stresses and the "exhaustion returns nullptr" unit tests call the pools directly and
depend on that graceful return (they install an Ignore handler); making `acquire()` itself a hard fatal broke
them (three `CRD_DEBUGBREAK`s in the full suite — caught and reverted). The real bug was at the **callers**,
where `CRD_ASSERT_MSG` compiles out in Release: `submit_jobs` packed a null counter into jobs (a Release
null-deref crash) and `run_job_in_fiber` silently **dropped** the popped job on fiber exhaustion (its counter
never decremented → permanent `outstanding`, which the watchdog would mislabel `ExecutorStarved`). Both are now
always-on `CRD_FATAL`s naming the Config knob — in an assert build the pool's assert routes to any installed
handler first, in Release the caller fatal is the distinct, visible failure. There is deliberately **no**
`HangKind::PoolExhausted`: exhaustion now fatals, so it is never a survivable state the watchdog can observe,
and an enum value no code path produces would be a fake report.

Proven by two subprocess specimens (observer_swap pattern, since a live exhaustion aborts): `crd-diag-counter-
exhaustion-specimen` (`max_counters=1`, the second `run()` exhausts) and `crd-diag-fiber-exhaustion-specimen`
(`small_fiber_count=1`, a concurrent dispatch exhausts). Each handler reads `progress_snapshot().exhaustions`
and `_Exit(42)` only if the tally surfaced (60 if the message fired but the count did not, 97 for a different
assert). Direct run: `CRD_DIAG_EXHAUSTIONS=1`, exit 42 for both. The healthy-pool half is proven in-process:
a pool that never exhausts reports `exhaustions == 0`.

Verified: win-debug full jobs 29569 / 143; win-asan full jobs 29569 / 143 (ASan-clean, including both
instrumented exhaustion children); twin 6 / 45; DIAG harness 6; both validators PASS; no slice tags in engine
source; no repo-root scratch. DIAG.4c stays **Open** — of the five acceptance kinds, deadlock (WaitCycle),
parked-stall, executor-starve and pool-exhaustion are delivered; **livelock** (needs job-declared progress) and
**priority starvation** (needs a per-lane oldest-waiting age — a scheduler signal) remain, as do (d) RT
sentinels, (e) safe snapshot / honest-incomplete, and (f) doc + flip.

## DIAG.4c (c cont.) — priority starvation as a distinct per-lane report

The fifth acceptance kind but the fourth delivered (livelock still to come). The scheduler drains strictly
High → Normal → Low with no aging (verified in `execute_one`/`try_pop`), so a sustained higher-lane flood can
indefinitely stall a lower lane's waiting work while the system keeps completing other jobs. That is DISTINCT
from a hang — completions advance, so `hang_verdict` reads Progressing — so the watchdog runs a separate
per-lane starvation check each window.

The signal costs nothing new: `ConcurrentQueue::dequeued()` exposes the Vyukov queue's existing monotonic
dequeue position next to `size()`, and `Scheduler::injection_diagnostics(backlog[3], pops[3])` reads both off
the three injection queues. Public `LaneSample {backlog[3], pops[3]}` + `lane_snapshot()` carry it out (indexed
by Priority; local per-thread deques and stolen work are not counted, since public `run()` jobs land in the
injection queues — exactly where a starved job waits). Pure `starvation_verdict(before, after, completions)` in
`hang_watchdog.hpp` returns a per-lane bitmask: lane L is starved iff it has backlog, made no pops this window,
and the system as a whole progressed (some lane's pops or completions advanced). Flat-everything is a hang, not
starvation, and returns 0 — a boundary the tests pin. A per-lane `StarvationDetector` (K=3, fires once per
episode per lane) debounces it, and the watchdog calls `StarvationHandler(const StarvationReport&, void*)` —
`{lane, backlog, stale_windows, completions}` — installed via `set_starvation_handler` (same atomic-pair
pattern as the hang handler). No `ProgressSample`/`HangReport` changes; starvation is its own signal and report.

Proven by a pure decision table (starved; flat-everything → none; a draining backlog → none; no backlog →
none; two lanes at once; detector fire-once-per-episode + independent lanes) plus a live end-to-end case: a deep
batch of 300 Normal jobs queued **upfront**, each busy-spinning ~1ms, keeps the single background worker
committed to the Normal lane (its pops advancing) for ~300ms — far past the ~60ms K=3 detection window and with
no refill-race gap — while a held Low job starves; the report names lane == Low with backlog ≥ 1, and High
(never backlogged) is never flagged; the batch is then drained so the Low job finally runs. A healthy
three-lane stream never fires. (An initial feeder-refill flood proved flaky — a startup race plus transient
queue gaps let the fast single worker reach Low; the deterministic upfront time-consuming batch removed both.)

Verified: win-debug full jobs 29594 / 152; win-asan full jobs 29594 / 152 (the starvation timing holds under
ASan, stable across repeated runs); twin 6 / 45; both validators PASS; no slice tags in engine source; no
repo-root scratch. DIAG.4c stays **Open** — of the five acceptance kinds, deadlock, parked-stall,
executor-starve, pool-exhaustion and priority-starvation are delivered; **livelock** (needs a job-declared
progress/deadline contract — a spinner reads Progressing, so it cannot be inferred) remains, as do (d) RT
sentinels, (e) safe snapshot / honest-incomplete, and (f) doc + flip.

## DIAG.4c (c cont.) — livelock via opt-in task progress epochs

The last of (c)'s scheduler-observable kinds. A livelock — tasks running but not progressing — is invisible to
every aggregate: a spin loop keeps a worker executing, identical to a legitimate long job (both read
Progressing). So detection is OPT-IN, exactly the design's "task progress records": a long task calls
`jobs::note_progress()` as it advances, and the watchdog reports a monitored task whose progress epoch stays
flat across several windows while it keeps a worker busy. (Deadlines are not in the acceptance list — deferred.)

`Fiber::progress_epoch` (an appended atomic `u32`) is the signal: `note_progress()` is one relaxed `fetch_add`
on the running fiber (a no-op off-fiber), the first call opting the task in (epoch > 0); `FiberPool::release_to`
resets it to 0 so a recycled or free-list fiber reads as unmonitored. Public `ProgressNode` +
`monitored_snapshot()` walk the fibers emitting the monitored, non-parked tasks (a parked monitored task is the
hang detector's domain, so `waiting_on != nullptr` is excluded), truncation-honest like the wait graph. The pure
`LivelockTracker` keeps a fixed 64-entry table keyed by (tier, fiber_index, task_id): a flat epoch for K windows
fires once per episode; an advancing epoch resets; a task absent for a window is evicted (completed or parked);
a reused fiber slot running a new task id is a fresh entry that never inherits the old task's staleness. The
watchdog runs the check each window independently, and **skips it when the window read as Paused** — a frozen
process (debugger/suspend) flatlines every epoch, which is the one false-positive shape. `LivelockReport` +
`set_livelock_handler` deliver it; `HangReport`/`ProgressSample` are untouched.

Proven by a pure decision table (fires on the K-th flat window once per episode; an advancing epoch never; two
tasks fire independently; a task absent for a window restarts its episode; a reused slot with a new task id is a
fresh entry) and four live cases: a true MUTUAL livelock — A and B each opt in once then spin on a flag the
other sets only after its own wait — is reported for **both** task ids while the **hang handler stays silent**
(the distinctness proof in one scenario); and a steadily ticking task, an unmonitored long job (the opt-in
proof), and a parked monitored task are each never flagged.

Verified: win-debug full jobs 29631 / 161; win-asan full jobs 29631 / 161 (the live spins are ASan-clean, stable
across repeated runs); twin 6 / 45; both validators PASS; no slice tags in engine source; no repo-root scratch.
DIAG.4c stays **Open**: (c)'s four scheduler-observable distinct reports — deadlock, parked-stall/executor-
starve, pool-exhaustion, priority-starvation, livelock — are complete, but the acceptance's fifth distinct
report, **forbidden blocking/allocation** (d), plus (e) safe stop/snapshot + honest-incomplete and (f) doc +
flip, remain.

## DIAG.4c (d) — portable real-time sentinels (forbidden allocation / blocking)

The acceptance's fifth distinct report. A declared real-time region (an audio callback, a deadline-bound task)
must not allocate or block; wrapping it in an `RtScope` makes the crd allocators and `jobs::wait()` REPORT a
forbidden operation instead of silently permitting it — a diagnostic, never a behaviour change.

Layering settles the shape: the scope flag and reporting must be readable by both `crd::memory` (allocation
check) and `crd::jobs` (blocking check), and memory cannot depend on jobs, so they live below both in
`crd::core` (`rt_sentinel.hpp`/`.cpp`). `RtScope` is an RAII marker over a thread-local depth (it nests);
`RtViolationKind {Allocation, Blocking}`, `RtViolation {kind, bytes}`, and `set_rt_violation_handler` (the same
atomic-pair, no-default-action pattern as the watchdog) complete the API; `report_rt_violation` is re-entrancy
guarded so a handler that itself logs (and thus allocates) cannot recurse. Everything is `CRD_ENABLE_ASSERTS`-
only — `RtScope` compiles to an empty struct and the checks vanish in a shipping build, where comprehensive
coverage is RTSan's job after qualification.

The allocation check goes at the single shared seam — `MemoryStats::on_allocate`, which every concrete
allocator already calls — rather than editing all ~10 `allocate()` overrides; the blocking check sits at the top
of `jobs::wait()` (before the fast path, so `run_and_wait` and even an already-satisfied wait are covered). Both
check then proceed. A documented invariant keeps it honest: an `RtScope` must not span a fiber park — that park
is itself the Blocking violation, and the thread-local depth would not follow a fiber that resumes on another
thread — so RT scopes stay leaf-level.

Proven by six tests: allocation inside a scope reports `Allocation` with the requested byte count (on the main
thread, off any fiber); nested scopes stay active until the outermost exits; allocation outside every scope is
silent; a **raw `std::thread`** with no job system involved fires (the real audio-thread case, proving the
thread-local works off-fiber); `jobs::wait()` inside a scope reports `Blocking` (on the main thread against a
pre-completed counter, so the wait hits its fast path — no park, no migration); and, with the watchdog live and
the hang/starvation/livelock handlers installed alongside, an RT violation fires **only** the RT handler.

Verified: win-debug jobs 29646 / 167, core 12/2, memory 820203 / 140 (`memory_stats.hpp` is widely included —
no regression); win-asan jobs 29646 / 167 and rt 15/6; twin 6 / 45; both validators PASS; no slice tags in
engine source (a `#diag-4c` doc anchor in a comment was rephrased); no repo-root scratch. DIAG.4c stays
**Open**: with (c)'s five kinds and (d) delivered, the acceptance's distinct-reports matrix is complete;
remaining are **(e)** the safe stop/snapshot + honest-incomplete result for an unresponsive process (whose
parked half is already the wait graph; the running-worker-stack half is DIAG.5a/5b territory — to be scoped next
tick) and **(f)** the session doc + flip.

## DIAG.4c (e) — cooperative worker snapshot with an honest-incomplete result

The running-half of the design's safe stop/snapshot protocol (the parked half is `wait_graph_snapshot`). The
scope split, recorded: "never walking changing stacks unsafely" means the only safe way to read a *running*
worker's state is to have the worker report it at a safe point — a cooperative acknowledgement protocol,
in-process. External running-thread stack capture (SuspendThread / StackWalk / minidump) is the OS-specific,
qualified crash-capture layer's job, not this. So (e) is the failable *attempt* plus task state, not stack
walking; capturing a worker's own stack at its loop top would only yield scheduler frames for responsive
workers and nothing for stuck ones (the meaningful stack is the running fiber's, which needs an external
suspend), so it was deliberately left to the crash layer.

`WorkerProgress` gained `current_task_id` (set/cleared around `run_job_in_fiber` in both `worker_loop` and
`pump`, mirroring `executing`) and `ack_gen`. `WorkerPool::request_snapshot()` bumps a generation and wakes
every sleeper (both wake modes reach all workers); each worker echoes the generation into `ack_gen` at its
loop-top safe point. A worker stuck inside one job never returns to that safe point, so its ack falls behind —
the honest "unresponsive" signal. `worker_snapshot(span, timeout_ms)` bumps the generation, wakes, and polls a
steady clock until every background worker (1..N-1, minus the caller if a worker calls it) acknowledges or the
timeout elapses; `complete = responded == expected`. Thread 0 (the pump thread, not a loop worker) and the
caller are reported from the racy dump and counted responsive. `WorkerNode {thread_index, current_task_id,
executing, responsive}` and `WorkerSnapshotResult {total, expected, responded, complete}` are the public output,
truncation-honest like the other snapshots.

Proven by four tests: an idle pool is fully responsive and complete with no running tasks; a seeded stuck worker
(one gated spin job on a three-thread pool) is reported `responsive == false`, `executing == true`, and
`current_task_id == the spin job's id` — the dump names *what* is stuck even though the worker cannot ack —
while the other background worker is responsive and the result is honestly `complete == false`
(`responded == expected - 1`); the attempt is time-bounded (returns in well under a second, roughly at the
~100 ms deadline, never hanging); and truncation reports the true worker count past a smaller buffer.

Verified: win-debug jobs 29676 / 171; win-asan jobs 29676 / 171 (the poller/worker ack races are ASan-clean,
stable across repeated runs); twin 6 / 45; both validators PASS; no slice tags in engine source (a `DIAG.5a/5b`
cross-reference comment the grep caught was rephrased); no repo-root scratch. DIAG.4c stays **Open** for one
more tick: with (c)'s five distinct-report kinds, (d) real-time sentinels, and now (e)'s bounded-attempt +
honest-incomplete result all delivered, the full acceptance is met — the remaining work is **(f)**: a dedicated
DIAG.4c session doc with the Session link and a full-acceptance review, then the Open -> Needs CI flip.

## DIAG.4c flipped; DIAG.5a begins

DIAG.4c's full-acceptance review and Open→Needs CI flip are recorded in a dedicated doc:
[2026-09-15-diag-4c-hang-starvation-rt.md](2026-09-15-diag-4c-hang-starvation-rt.md). The next Open slice, DIAG.5a
(Windows crash and hang capture, DG09), continues in its own doc:
[2026-09-15-diag-5a-windows-crash-capture.md](2026-09-15-diag-5a-windows-crash-capture.md).
