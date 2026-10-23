# DIAG.11b clause (6): the flight recorder's record path without a lock, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b). Rules: [AGENTS](../../AGENTS.md). Previous batch:
> [jobs adapter token table](2026-10-24-diag-11b-jobs-adapter-token-table.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failure without a fix on main. 40029933433,
40029693899, 40022015924, 40020783027, 40018724265, 40014687187 and 40014471031 were cancelled with no job.
40006451399 (head 6a964937) was still running; its finished win-asan job failed with four heap-buffer-overflow READs of
8 bytes in `d3d10warp.dll` under `Dx12RasterContext::dxr_pipeline` and `UNEXPECTED PASSED: registered TP-1` for the
pipeline-identity case: the WARP read that a832934d repaired after that head. Its hosted proof waits on the pending
40032224637 (head 5066e492). Not waited on, nothing cancelled, no dispatch.

## The change

`FlightRecorder::record` took the recorder-wide ring mutex for every kept event. Now (`flight_recorder.cpp`):

- One atomic recording word holds the total (above three bits) and the state. `claim()` takes the next sequence by
  compare-exchange only while Armed or Triggered; a frozen ring (Complete, FatalEntered) drops and counts the event.
  The claim that reaches the post window's end moves the state to Complete in the same exchange, so no event is
  claimed after the window closed.
- `trigger()` moves the word from Armed to Triggered in one exchange (under its own mutex, as before); the total it
  exchanged is the trigger's sequence, so the boundary is exact. The `anchored`/`post_start` patch and the
  "provisional total" caveat are deleted.
- `publish()` fills the slot `(seq - base) % capacity` and stores the slot's `sequence` with release through
  `std::atomic_ref`: the field is the publication stamp, so `FlightEvent` keeps its layout. A claim waits for the
  slot's previous occupant (a full ring earlier) to publish; that wait is reached only when the ring laps a claim
  stalled between its claim and its publication.
- Collectors (`poll`, `take_capture`, `finish`) and `reload_policy` hold `collect_mutex`, which `record()` never
  takes. A capture waits for every claimed slot of its window, the fatal path within the same bounded yield budget as
  its try-lock (2000); when either runs out the capture is the trigger alone with `ring_unavailable`, as before. A
  window filled by a claim ends `PostEventsFull` at that event's time.
- `reload_policy` sets the word's reloading flag (claims wait), waits for every claimed slot, swaps the ring and sets
  `base`, then clears the flag. A flag beside the state, not a fifth state: the fatal path's `fetch_or` to
  FatalEntered (3) on a state 4 would have made an undefined 7 that claims carry forward, and an unconditional re-arm
  store at a reload's end could overwrite a FatalEntered set meanwhile; each now keeps the other's bits (found in
  review before the push).
- A drop is counted once, by the first capture collected after it: a thread that saw the ring frozen but counted
  only after the collector's re-arm adds to the next capture. Collected counts stay exact single-threaded (every
  existing case).

Nothing is added beyond what the lock held: the head, live count, anchor and their mutex are gone. One test seam,
`test_with_claim_unpublished` (asserts builds only, beside the existing two), reuses `claim`/`publish`, so a case can
hold a claim unpublished; no other seam can.

## Evidence

Changed and new cases (`tests/foundation/perf/`):

- `test_diag_flight_sites.cpp` "neither a left-out nor a kept event waits on a collector holding the ring": its
  positive control (a kept warning blocked while the ring was held) inverts by design; the kept warning must now
  return while a collector holds the lock, the left-out arm unchanged.
- `test_diag_flight_recorder.cpp` "a collector waits for a claim still publishing; the fatal path only within its
  bound": `take_capture` keeps the held claim's event (sequence, value, message, end time); `finish(Fatal)` returns
  before the held claim publishes, delivering the trigger alone and terminal.

Teeth on win-debug, each restored, rebuilt and rerun green:

- A lock on the record path: the sites case fails (`warning_returned` false).
- The claim that fills the window closes it in a second step: the concurrency case fails 3 of 3 (`bad > 0`). A
  check-then-store claim instead makes it hang to its timeout (twice), a failure but no diagnosis, so the
  two-step close is the recorded tooth.
- The collector reads without waiting: the new case's first section fails (stale slot: sequence, value, message).
- The fatal path waits without a bound: the second section fails (`still_held`, `ring_unavailable`, events).
- On linux-clang-tsan, a relaxed publication: three cases report a data race (`build_capture`'s copy against
  `publish`). Restored: 41 flight, sites and jobs-adapter CTests, 20 repeats, no report.

crd-perf-tests passes on win-debug (294 cases) and linux-gcc-debug (292); on win-debug the 12 `crd-diag-overhead-*`
CTests, `crd-fuzz-perf-flight-corpus` and the fault-injection, device-loss and install-fault cases pass (71), and on
linux-gcc-debug the 12 harness CTests.

Cost, `diag-overhead.py run --workloads jobs,events --arms off,full` on win-shipping-profile (16 processes, 200 frames
after 20, 8 threads, affinity FFFF, host idle by `build-bench.py host`). One binary per source (HEAD's
`flight_recorder.cpp` 21fbdd1f9284, this change's first build 8a9640df6147), measured alternately:

| Run | jobs off p50 µs | jobs full p50 µs | full over off (95% interval) | events full ns/site |
| --- | ---: | ---: | --- | ---: |
| mutex 1 | 399.8 | 538.7 | +35.29% (+29.18% .. +39.05%) | 78.8 |
| no lock 1 | 398.5 | 416.7 | +4.85% (+0.05% .. +8.88%) | 74.8 |
| no lock 2 | 398.2 | 409.7 | +3.71% (-1.88% .. +5.87%) | 74.9 |
| mutex 2 | 417.8 | 535.9 | +27.72% (+21.90% .. +30.12%) | 78.5 |
| no lock 3 | 393.8 | 409.3 | +3.94% (+2.60% .. +5.40%) | 74.5 |
| mutex 3 | 426.6 | 536.3 | +29.45% (+19.52% .. +32.61%) | 79.4 |
| no lock 4 | 391.8 | 405.2 | +4.54% (+0.11% .. +5.84%) | 75.0 |

The pushed source (the reloading flag, 2fdeb82309fc) measured alternately with HEAD's twice more: jobs full over off
+1.33% (-1.35% .. +3.54%) and +3.59% (-0.48% .. +6.42%), HEAD +31.64% and +33.21%; events 75.8 and 75.5 ns per
site, HEAD 79.7 and 79.4. The jobs full frame drops by about 120 µs (about 60 ns per job); full is a reported budget.
The single-threaded events site gains about 4 ns (the uncontended lock). After the flag change: crd-perf-tests on
win-debug (294) and linux-gcc-debug (292), and the 41 TSan CTests with 20 repeats and no report, again.

Found on the way: `check-headers.py` crashed (`UnicodeEncodeError`) printing a localized MSVC diagnostic to a cp1252
console; it now writes UTF-8 like the other guards, and reports the failure. One win-debug build stopped in sccache
("failed to persist temporary file", os error 5) on `test_dx12_validation.cpp`; the rerun compiled it unchanged
(host fault, recorded, not chased).

## Not done in this batch

- Clause (6) next: the CEIR bridge's per-instruction timing.
- (l) zstd 1.5.7 and (o) clang-tidy 20 are downloads; the user's own messages in this run approve neither, so each
  still waits.
