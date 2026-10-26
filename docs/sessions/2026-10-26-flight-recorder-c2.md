# DIAG.11b (q1): the flight recorder keeps per-thread rings under one global sequence (C2)

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b), follow-up (q1). Decision: the user's
> option C2 of 2026-10-26 ([brief](2026-10-26-flight-recorder-per-thread-rings-decision.md)). Contract:
> [ADR-0133 ID-7](../decisions/0133-runtime-diagnostics-and-instrumentation.md),
> [flight recorder](../design/runtime-diagnostics.md) (DIAG.9b, DIAG.11b),
> [`flight_recorder.hpp`](../../engine/foundation/perf/include/crd/perf/flight_recorder.hpp). Board:
> [C2](../bench/2026-10-26-diag-11b-flight-recorder-c2.md). Rules: [AGENTS](../../AGENTS.md).

## The task

Implement C2 in full: one global sequence and per-thread rings, keeping ADR-0133's order (one total event order that
agrees with happens-before) and the exact trigger window in claim order; the 152-byte event and the redaction rules
unchanged. The brief's prototype covered only the Armed record path; this is the whole recorder: record, trigger,
the post window and its closing claim, capture and merge, reload, the fatal path, thread lifetime, storage, the
test seams and the sanitizer behaviour.

## The design as built

- **The claim.** A kept event still takes the next sequence of the one recording word, now with a single
  `fetch_add` (before: a compare-exchange loop). The state the `fetch_add` returns decides the claim: Armed is a pre
  window event, Triggered below `post_end` a post window event, and the claim of `post_end - 1` closes the window
  (it stores the window's end, then moves the state to Complete). Every other claim is **void**: one that finds
  Complete or FatalEntered, one past `post_end`, or one that meets a policy reload (that one waits and claims again).
  A void claim writes nothing.
- **Dense sequences.** Void claims add to the word's total, so the recorder gives those sequences back: re-arming
  after a capture is `exchange(make_word(end, Armed))`, and a reload's end `exchange(make_word(base, Armed))`. So
  every sequence below the total is one kept event, as with the global ring: `recorded_before_trigger ==
  trigger.sequence`, the capture's sequences are contiguous across the trigger, and the CFR1 record, its decoder, the
  fuzz corpus, `bundle.inspect`, `flight.status`'s capture fields and the JSON summary are unchanged.
  `dropped_while_complete` is now the given-back count (the total at re-arm less the window's end), exact, with no
  counter written per dropped event. `total_recorded()` counts kept events (the window's end while frozen).
- **Storage.** 64 thread rings (one cache-line header each: owner token, a claim mark, head, next, storage). Each
  ring with storage holds the policy's `capacity`; together they hold at most 65,536 events
  (`flight_thread_rings(capacity)`: 64 up to 1024 events, one at 65,536). Beside them, one shared ring of `capacity`
  slots indexed by sequence with the global ring's stamp protocol, for a thread whose ring has no storage or that
  finds all 64 taken. One block from the default allocator: 9.65 MiB at the default policy (was 152 KiB), at most
  about 19 MiB; untouched rings stay unresident (the harness's resident growth is unchanged).
- **Why no event of a window is lost.** Every ring holds at least `pre_events + post_events` events and nothing is
  written after the window freezes, so a thread's window events are among its ring's newest; in the shared ring the
  window's at most `capacity` consecutive sequences have distinct slots. No new loss metadata was needed.
- **Capture and merge.** A collector (state Complete, collection lock) waits for every ring's claim mark that is odd
  (a claim in flight), then places each ring's newest events back to the window's first sequence at their
  sequence's position, then the shared ring's for positions left. The result is in the one global order for every
  reader of a capture: the bundle, `flight.status`, `bundle.inspect`, the summary and the fatal path's capture. The
  fatal path waits within its existing budget and otherwise delivers the trigger alone, as before.
- **The claim mark.** A ring's owner writes its mark odd before the `fetch_add` and even after the event (or after a
  void claim). A collector or reload that read the word after a claim's `fetch_add` therefore sees its mark; marked
  after, a descheduled thread would have been invisible.
- **Threads.** A thread takes a ring on its first kept event: its own if it holds one, else a free one with storage,
  else a free one without storage (its events go to the shared ring under its own mark), else the shared ring (with
  a shared in-flight count only a reload waits for). No allocation, no lock. The binding is a trivially constructible
  thread-local (recorder id, ring) that a new recorder's id invalidates. A thread gives its rings back when it exits
  (Windows: a thread-local destructor; elsewhere a pthread key destructor, because glibc's thread_local destructor
  list allocates and aborts when that fails), through a list of live recorders under a never-destroyed mutex, so a
  shut-down recorder is never touched; its events stay in the ring. `record()` is `CRD_NOINLINE`: the binding read must
  not be cached across a job fiber's switch to another thread (crd-jobs' TLS caveat).
- **Left out, unchanged:** the left-out counters stay striped (`StripedCount`), outside the order; the jobs
  adapter's `register_thread` is the profiler's and the recorder needs no call from consumers; the recorder has no
  schedcheck yield points (they are crd-jobs'); the diagnostic host's `frame()` is unchanged (it notes the frame and
  polls).
- **Sanitizers.** No annotation: every cross-thread read is ordered by the claim mark's release/acquire, the word's
  read-modify-writes or the owner token's. Under ASan a thread ring's slots stay poisoned until the ring first
  writes them, and the block is unpoisoned before it is freed.
- **Out of memory.** init and reload refuse with `RejectedOutOfMemory` when the block cannot be allocated (a reload
  keeps the old rings). A test-only seam, `FlightRecorder::test_refuse_ring_storage`, makes that reachable, so the
  coverage registry's two `resource-failure` exclusions (`FlightPolicyStatus::RejectedOutOfMemory`,
  `DiagHostStart::RecorderRefused`) are removed; the diagnostic host test asserts the second.
- **flight.status** gains `thread_rings`, `threads_bound`, `ring_bytes` and `shared_ring_events`
  (`FlightRecorder::ring_stats()`). No runbook names those fields.

## Proof of order and of the window

`tests/foundation/perf/test_diag_flight_rings.cpp` (crd-perf-tests, `[diag11b][flight]`):

- *One global order that agrees with happens-before:* four threads pass a token, each recording hop h after
  acquiring it and releasing it at h + 1, while two more threads record; the capture is contiguous and the hops in
  it are consecutive and ascending, from every hopping thread (message passing appears in causal order).
- *The trigger boundary in the trigger's own thread while others record:* every event is kept (the window closes by
  its time bound), the event recorded just before the trigger is in the pre window and the one after in the post
  window; every worker's events are all present in program order; one of five threads records into the shared ring.
- *Exactly `post_events`, and void claims given back:* four workers record 20,000 events each through a 64 + 32
  window; the post window holds exactly 32, every worker's window events are consecutive, all events = kept +
  dropped, and the next events take the window's end, so none recorded after closing appears in a later capture.
- *Threads:* an exiting thread gives its ring back and its events stay; a late thread takes the ring; two rings with
  storage serve four threads (two record into the shared ring); 70 live threads, 6 past the rings.
- *A reload while three threads record*, between storage shapes (64 rings, 2 rings); *a refused allocation* at init
  and at reload.

Teeth (a scratch script applied one break at a time to `flight_recorder.cpp`, rebuilt crd-perf-tests on win-debug and
ran the ring and `[flight]` cases; then the source was restored and rebuilt, and both passed):

| Break | Result |
|---|---|
| merge by timestamp instead of claim (option B's order) | 3 of 6 ring cases and 5 of 31 `[flight]` cases fail (contiguity, causal order) |
| rings concatenated, not placed at their sequence | ring and `[flight]` cases fail (contiguity, order), then the capture waits for positions it never filled; timed out |
| trigger boundary one claim late | 3 of 6 ring cases, 22 of 31 `[flight]` cases fail |
| a claim one past the window's end kept | the post-window case fails (an event recorded after the window closed takes a later capture's position) |

## Two defects found while testing

- **A spinlock under many exiting threads.** The live-recorder list first used a spinlock; 70 threads exiting at
  once spun on it, and under ThreadSanitizer the holder's release starved behind the spinners' exchanges until the
  case hung (247 s and counting). It is now a never-destroyed `std::mutex`.
- **The tests' own waits under ThreadSanitizer.** Threads spinning on an atomic that others must write starved those
  writers under TSan (a Cerid-free reproduction: 70 threads, a token and a `done` counter, 30 s and stuck at 30 to 44
  of 70; with a separate flag only main writes, 20 to 45 ms). The ring cases wait on such flags, and keep every
  thread alive until all have bound, so which thread gets which ring is fixed.

## Verification

| Lane | Ran | Result |
|---|---|---|
| win-debug | crd-perf-tests (312 cases), ring cases 8 more times, crd-fuzz-perf-flight-corpus, the 28 `crd-diag-overhead-*` CTests; ceridc, crd-sandbox, crd-ceridc-tests built | pass |
| win-asan (inside vcvars) | crd-perf-tests (312), corpus replay, crd-diag-coverage | pass |
| linux-gcc-debug | crd-perf-tests (310), corpus replay, crd-diag-coverage | pass |
| linux-gcc-asan | the same | pass, once a fault (below) |
| linux-clang-tsan | the same, `[flight]` 5 more times, and 5 times pinned to two CPUs beside two pinned busy loops | pass |

One linux-gcc-asan full run of crd-perf-tests stopped with a SEGV in `test_diag_flight_identity.cpp` (reading
`/proc/self/maps` into an `Array`), at `0x03e800000a89`: the `{uid, pid}` shape of the host faults of 2026-10-25,
in a case the recorder change does not reach. It did not come back in three more full runs and five runs of the
`[identity]` cases on the same binary. Recorded, not chased: if it is seen again on this host after the microcode
update, the host check needs repeating. Settled the same day ([session](2026-10-26-catch2-fault-address.md)):
the word is Catch2's re-raise of the fault (a sent signal's `si_pid` and `si_uid`); the fault was real and its
address was lost.

tidy (LLVM 20.1.8) is clean on every changed C++ file; the guards, the documentation validator and the repository
check ran on win-debug before the push.

## The numbers

[Board](../bench/2026-10-26-diag-11b-flight-recorder-c2.md). The four-thread jobs frame in full mode against Tracy
connected: **−4.20% (−5.12 .. −3.28), CPU −7.82%** on 30 processes (+1.25% and −1.38% on two 10-process boards);
against the global ring **−5.79%** (30 processes) and −5.33% on the modes board. Basic within 2% on jobs (96
processes), alloc and offline; the disabled budget within 1% on instructions (offline +0.07%, jobs four threads
+0.25% at most). Memory: 9.65 MiB of ring storage at the default policy, resident growth unchanged.

## What only the hosted run can prove

The recorder's cases on every hosted lane (MSVC, clang-cl, GCC, the shipping and release builds that compile the
seams out, and the hosted TSan and ASan lanes); the coverage measurement (`diag-coverage.py`) that the new
transition sites ran and that the two removed exclusions are produced; and the package consumer's bundles. Needs CI.
