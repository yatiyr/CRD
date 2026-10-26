# 2026-10-26 — DIAG.11b clause (8): Tracy Profiler as the peer on comparable features

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Contract: [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b) ("use peer tools only on comparable
> features"); session: [2026-10-26](../sessions/2026-10-26-diag-11b-tracy-peer.md); owner row:
> [DIAG.11b](../ROADMAP.md#slice-diag.11b). Pin: [pinned inputs](../design/pinned-inputs.md#the-tracy-peer-opt-in).

- **Machine:** Intel i9-14900K (8 performance cores, 32 logical CPUs), 63.7 GiB, Windows 11 10.0.26200; WSL2
  (kernel 6.6.87.2) on the same host for Linux. Every board ran after `scripts/build-bench.py host` read the host idle
  (a run whose check read it contended was discarded and run again).
- **Builds:** win-shipping-profile (MSVC 19.51, `/O2 /GL`, profiling compiled in) with `-DCRD_DIAG_PEER_TRACY=ON`;
  win-shipping (profiling compiled out) for the compiled-out arm; linux-gcc-shipping (GCC 13.3, LTO, **profiling
  compiled out**: Cerid's zones and frame marks are not in that build) through `scripts/wsl-lane.sh`, with the option.
- **Peer:** Tracy Profiler 0.14.1 (tag commit `30997d5c`, BSD-3-Clause), the client built from the pinned archive with
  `TRACY_ENABLE TRACY_ON_DEMAND TRACY_MANUAL_LIFETIME TRACY_DELAYED_INIT TRACY_ONLY_LOCALHOST TRACY_ONLY_IPV4` and
  without broadcast, sampling, system tracing, call stacks, frame images, code transfer and crash handler (the
  features no Cerid mode has a counterpart for); its timer is the cycle counter (`rdtsc`, invariant on both hosts).
  The client is `tracy-capture.exe` 0.14.1 from the pinned Windows release archive on `127.0.0.1` (on Linux through
  WSL2's localhost relay), one client and one port per connected process.
- **Harness:** `crd-diag-overhead-tracy` (`tests/support/diag-overhead`, the same sources as `crd-diag-overhead`
  with `CRD_DIAG_OVERHEAD_TRACY=1`): one binary holds Cerid's off, basic and full modes and the `tracy` mode, so every
  Cerid-against-Tracy row compares two arms of one binary. `scripts/diag-overhead.py run` (10 interleaved processes
  an arm, 200 timed frames after 20 warm-up, four jobs threads, affinity `5555` on Windows, bootstrap 95% intervals)
  and `instructions` (cachegrind, the differences of 60 and 20 timed frames, two repeats, one jobs thread).
  `tracy-connected` runs with the client attached from before the first frame to after the last, and a process counts
  only when the client's capture holds every zone, frame mark and message the process emitted (`Zones:`/`Frames:` of
  `tracy-capture` and `tracy-csvexport -m`; every connected process of every board matched).

## Which Cerid feature has which Tracy peer

| Cerid feature (where the harness exercises it) | Tracy counterpart at the same site | Peer? |
|---|---|---|
| Profiler zone, `CRD_PERF_SCOPE` (`zones`; full mode, the profiler started) | `ZoneScopedN` | Peer. |
| Profiler frame mark, `CRD_PERF_FRAME_MARK` / `frame_mark()` (`marks`; once a frame in every full workload) | `FrameMark` | Partial: Cerid's mark also snapshots the counters and allocators into its 240-frame history, drives the GPU frame lifecycle and pins the profiler's lifetime; Tracy's is the boundary event alone (its plots and GPU contexts are separate events). |
| Flight-recorder event site kept, `CRD_FLIGHT_RECORD` at the full detail (`events`; one site per job, read, run or batch elsewhere) | `TracyMessage`, guarded by `TracyIsConnected` so the text is formatted only with a client, as Cerid's only for a kept event | Peer for the event. Cerid keeps it, with two scalars, in an in-process ring of the last N in one total order that triggers and capture bundles read; Tracy streams it to the client. |
| A site the detail leaves out (basic: severity below Warning, counted) | — | No peer: Tracy filters severity only in the viewer. |
| Runtime-disabled site, zone or mark (off: no recorder, no profiler) | On demand with no client: every site checks the connection | Peer ("not connected"). |
| Compiled out (win-shipping, linux-gcc-shipping) | `TRACY_ENABLE` undefined | Equal by construction (no code); not measured. |
| Jobs adapter: a region per job through crd-jobs' `JobObserver` (jobs, io; full) | A Tracy zone per job through the same observer (C API: a callback holds no C++ scope) | Peer. Neither workload's jobs wait, so Tracy's fiber API is not needed. |
| CEIR profiling bridge: a dispatch count and self time per op class, aggregated in process (offline; full) | A Tracy zone per dispatched op named by its class (the same `RunHooks`), zones around compile and run, the plan shape and dispatch total as plots | Closest peer: Tracy derives the same per-class counts and self times in its viewer from a zone per op; no Tracy feature aggregates in process. |
| Profiler counters, `CRD_PERF_COUNTER_*` (offline: six a frame) | `TracyPlot` | Peer; exercised only inside the offline rows, not measured apart. |
| Allocator statistics on request (`--memory`: the heap registered with the profiler, counted, snapshotted at each mark) | `TracyAllocN` / `TracyFreeN` at every operation | Partial: Tracy records every event (a superset from which its viewer builds the same totals); Cerid keeps the totals. |
| Allocator watch, crash capture, flight triggers and capture bundles, replay, device validation | — | No peer (Tracy's crash handler only flushes the trace, and is off here). |
| Diagnostic host frame (watchdog heartbeat, frame-hitch trigger) | — | No peer. |
| GPU scopes, `CRD_PERF_GPU_SCOPE` | `TracyD3D12Zone` / `TracyVkZone` | A peer exists, not measured: no mode installs a GPU backend in the harness, and Tracy's GPU contexts need the renderer's command lists (engine code). The render rows compare the one site per frame and the frame mark. |

## Per event, Windows (win-shipping-profile)

Cost per event over the reference: the paired difference of the frame medians over the events a frame makes (4,096
sites, 2,048 zones, 64 marks), CPU in `QueryProcessCycleTime` cycles (every thread: Tracy's worker included).

| Event | Cerid full (kept) | Tracy, client connected | Cerid − Tracy, connected | Cerid off over compiled-out | Tracy, no client, over Cerid off |
|---|---:|---:|---:|---:|---:|
| Site / message | 75.1 ns, 240.5 cycles | 71.4 ns, 297.1 cycles | **+3.9 ns** (+3.6 .. +4.1); −55.6 cycles | +0.1 ns | +0.1 ns |
| Zone | 20.0 ns, 64.2 cycles | 18.6 ns, 63.3 cycles | **+1.4 ns** (+1.3 .. +1.5); +1.3 cycles | +1.5 ns | −1.1 ns |
| Frame mark | 27.6 ns, 83.3 cycles | 23.2 ns, 75.2 cycles | **+4.3 ns** (+4.2 .. +4.8) | +0.5 ns | +4.0 ns |
| Site at basic (filtered, counted) | +1.1 ns over off | — | no peer | | |

The zone and site rows move by about ±1.5 ns between builds of the same source (code layout of a two-instruction
loop): over four builds the zone read −0.8, −0.5, −0.8 and +1.4 ns against Tracy connected and its off cost 0.5 to
1.5 ns, the site +1.2 to +3.9 ns. In one binary the last change (the zone site's name, below) costs nothing: the old
and the new site form side by side read +0.000 ns per zone off and +0.08 ns (−0.06 .. +0.14) full. So: **zones
within the build-to-build variation of Tracy connected (+1.4 ns on this board); a kept site within a few
nanoseconds of Tracy's message in time and 55 to 66 cycles cheaper in CPU (Tracy's worker compresses and sends each message); Cerid's frame mark 4.3 ns over Tracy's**, which
is the lifetime pin's two read-modify-writes and the history slot Tracy's mark does not keep. Not connected, Cerid's
off site equals Tracy's no-client site, Cerid's off mark is 4 ns cheaper, and Cerid's off zone is 0.1 to 1.1 ns over
Tracy's no-client zone (build to build).

## The repairs, and the clock that decided the zone

Before this session Cerid lost on all three. The first board (the peer binary before any repair, 4 processes an arm):
zone 26.6 ns against Tracy's 19.5 (+7.1 ns), mark 84.4 ns against 14.1 (+70.3 ns), site 76.1 against 71.5 (+4.6 ns).

- **The discriminator** (`crd-diag-overhead-tracy-osclock`: the same harness against a Tracy client timed by
  `std::chrono::high_resolution_clock`, which is `steady_clock` on MSVC, the clock Cerid's zones read): Tracy's zone
  costs 31.9 ns on it against 18.6 on `rdtsc`. With the same clock Cerid's zone was already 4.9 ns cheaper than
  Tracy's (27.8 against 32.7, before the repair); the whole gap was the clock, two `steady_clock` reads of about 11 ns
  each (a four-thread read loop: 11.9 ns `steady_clock`, 6.9 ns TickClock). Now Cerid's zone is 12.4 ns cheaper than
  Tracy's on `steady_clock` and its site 5.4 ns cheaper; their marks are equal (+1.0 ns).
- **TickClock** (crd-time): MonotonicClock's timeline read from the cycle counter, mapped by an anchor and a slope
  measured against MonotonicClock (re-anchored every 8 ms by whichever reader passes the period, the slope over the
  whole time since calibration, never stepping back; MonotonicClock itself during a 2 ms calibration and on a CPU
  without an invariant counter). The profiler's zones, frame marks and `diagnostic_now_ns()` (every flight-recorder
  event) read it. It also gives Windows zones the counter's resolution where `steady_clock` ticks at 10 MHz.
- **The frame mark** built a zeroed 3.6 KiB copy of its history slot (`FrameRecord rec{}`) before copying the used
  part into the slot; it now writes the slot directly, and its two counters (single writer: one frame loop marks
  frames) are stores instead of read-modify-writes; an allocator-free mark takes no snapshot pin.
- **Off**: a stopped profiler's entry points took the lifetime pin (two sequentially consistent RMWs) before seeing
  there was no profiler; they now look first. A zone with no profiler makes no call (an inline relaxed load).

Against the build before them (board `c`, `--before-exe`: the pre-repair binary `19659b84`, so two binaries and a
few percent of layout; the zone rows' off cell is that layout, above):

| Workload | off vs before-off | full vs before-full | per event, full |
|---|---:|---:|---:|
| zones | +0.7 ns a zone (layout, above) | −21.4% frame | −5.9 ns a zone (other builds −7.7, −7.9) |
| marks | −8.6 ns a mark (−91.5% frame) | −69.7% frame | −65.4 ns a mark |
| events | +0.0 ns | −1.6% frame | −1.3 ns a site |
| offline | −0.7% (−2.6 .. +0.4) | −0.3% (−0.6 .. +0.5) | |
| jobs | −4.9% (−6.6 .. −4.0) | +2.7% (+0.6 .. +3.7) | |

## Per frame, the workloads, Windows

Cost over the same binary's off; the last two columns are Cerid full over Tracy connected (positive: Cerid costs more).

| Workload | off µs | full µs | Tracy µs | Tracy connected µs | full | Tracy, no client (frame / CPU) | Tracy connected (frame / CPU) | **Cerid − Tracy connected, frame** | CPU |
|---|---:|---:|---:|---:|---:|---|---|---:|---:|
| alloc | 87.6 | 88.8 | 87.6 | 89.0 | +1.6% | +0.2% / +16.7% | +1.2% / +2.5% | −0.0% (−1.0 .. +1.0) | −1.3% |
| jobs | 207.2 | 339.9 | 213.5 | 293.7 | +63.3% | +2.7% / +2.3% | +40.6% / +45.6% | **+15.9% (+14.6 .. +17.3)** | +10.6% |
| offline | 336.6 | 1215.3 | 533.6 | 2084.3 | +260.7% | +59.0% / +57.0% | +517.7% / +1105.5% | −41.7% (−42.1 .. −41.0) | −70.5% |
| io | 614.0 | 621.9 | 611.0 | 620.0 | +1.5% | −1.0% / −0.6% | +0.8% / +0.9% | +0.2% (−0.7 .. +1.1) | −0.4% |
| render | 368.9 | 364.2 | 363.9 | 370.6 | −0.2% | −1.6% / −0.1% | +0.1% / +1.2% | +0.1% (−6.0 .. +0.5) | −1.6% |

Tracy with no client still costs CPU where its hooks run (offline: the per-op hooks are installed and called with no
client, +59%; Cerid's off installs none) and in its idle worker (alloc +16.7% CPU at an unchanged frame). The offline
row is the bridge's design: aggregated in process it costs 42% less frame time and 70% less CPU than a zone per op.
Two earlier boards of the same workload (before the last source changes, idle host) read jobs +4.8% and +12.6%;
this board's +15.9% is the final one.

**The loss: jobs.** Attributed in one binary by switching each off in turn (a one-off build that gates them by environment, 7 interleaved
processes each):
Cerid's kept site per job costs 71.8 µs a frame against Tracy's message 50.6 µs, while Cerid's jobs adapter costs
26.0 µs against Tracy's zone per job 36.6 µs. Single-threaded the site is at parity (above); with four threads it is
not. Three experiments on the recorder (each a one-off build, not kept): events padded to whole cache lines 76.9 against
76.6 µs (no change); one `fetch_add` claim instead of the compare-exchange loop 80.0 against 73.6 (no change);
thread-private claims and slots 45.5 against 77.3 µs, below Tracy's 50.6. The cost is the recorder's one claim word
and the ring slots every thread writes, the price of its single total order of the last N events (the trigger window
is exact in claim order). Removing it changes that contract: owned as DIAG.11b follow-up (q), a design decision.
Also measured and owned there: in one binary, the four-thread jobs frame with TickClock in both zones and sites is
+2.9% (+1.7 .. +3.4) over the same frame on `steady_clock` (+1.3% from the zones alone, +7.0% for the zones without
the sites), although a four-thread read loop finds TickClock 5 ns cheaper per read; not a re-anchor storm (9 to 19 a
run), not the counter's serialisation (`rdtscp` the same), not the map's cache line (128-byte isolation the same).
Across builds the final full jobs frame is +2.7% over the build before the repairs.

## Memory events (`--memory`), Windows

| Arm | p50 µs | over off | per allocate+free pair |
|---|---:|---:|---:|
| off (statistics gate, not requested) | 86.8 | (+9.4% over compiled-out: clause (1)'s gate) | |
| Cerid full, statistics requested | 125.3 | +44.2% (+43.5 .. +44.9) | 9.4 ns |
| Tracy, no client | 102.4 | +17.6% | 3.8 ns |
| Tracy connected, every operation reported | 355.1 | +310.9% (+299.3 .. +321.8) | 65.5 ns |

Cerid's totals cost 65% less frame time and 81% less CPU than Tracy's event log (which records more).

## Linux (linux-gcc-shipping under WSL2): timing

Cerid's zones and marks are compiled out on this lane (its shipping build has no profiler): those rows are Tracy
against nothing. TickClock is mapped there (the record's `tick_clock_mapped`).

| Event | Cerid full | Tracy connected | Cerid − Tracy | Tracy, no client, over off |
|---|---:|---:|---:|---:|
| Site / message | 57.2 ns | 62.1 ns | **−4.6 ns** (−6.2 .. −3.8); CPU −37.9 ns | +0.1 ns |
| Zone | compiled out | 24.9 ns | −24.9 ns | +0.4 ns |
| Frame mark | compiled out | 31.0 ns | −30.4 ns | +4.9 ns |

| Workload | full | Tracy, no client (frame / CPU) | Tracy connected (frame / CPU) | Cerid − Tracy connected, frame | CPU |
|---|---:|---|---|---:|---:|
| alloc | +2.5% | +0.7% / +0.2% | +1.7% / +4.2% | −1.5% (−3.3 .. +3.8) | −0.2% |
| jobs | +32.7% | +2.3% / −1.0% | +37.3% / +76.0% | −3.3% (−6.7 .. +0.6) | −19.9% |
| offline | +255.0% | +32.3% / +32.0% | +467.3% / +1062.5% | −36.8% (−38.9 .. −35.6) | −69.7% |
| io | +4.1% | −0.4% / +2.3% | +3.3% / +8.4% | −1.7% (−6.5 .. +19.6) | −5.9% |
| render (lavapipe) | −1.0% | +0.0% / +0.1% | −0.1% / +3.7% | −1.3% (−4.4 .. +2.1) | −2.3% |

Without the profiler's zones (compiled out here) Cerid's jobs frame is 3.3% cheaper than Tracy's and its CPU 20%
lower. `--memory`: Cerid's statistics are compiled out here (+2.3%, the sites and watch); Tracy +20.0% with no
client, +1,038% connected (185 ns a pair). Discriminator: Tracy on `clock_gettime` costs 37.4 ns a zone, 34.2 ns a
mark, and its message 67.9 ns (Cerid's site 10.2 ns cheaper).

## Linux: instructions (cachegrind, one jobs thread)

| Workload | Cerid full | Tracy connected | Cerid − Tracy | Tracy, no client |
|---|---:|---:|---:|---:|
| events | 1,286 instructions a site | 1,640 a message | −354 (−21.4% a frame) | 5 a site (Cerid basic: 18, the filtered count) |
| zones | compiled out | 184 .. 236 a zone | | 9 .. 10 a zone |
| marks | compiled out | 322 .. 340 a mark | | 15 .. 28 a mark |
| jobs (frame) | +293.5% over off | +603.8% over off | −44.1% a frame | +16.8% (the observer's calls) |
| alloc (frame) | +1.8% over off | +2.3% over off | −0.5% a frame | ±0.0% |

The connected counts include Tracy's worker thread (compression and sending), which is where its CPU goes.

## Verdict

Comparable features measured on both lanes, connected and not connected. After the repairs Cerid's zone is within the
build-to-build variation of Tracy's (+1.4 ns on the final board, −0.8 to −0.5 ns on three earlier builds), its kept
event within a few nanoseconds in time and cheaper in CPU (cheaper in both on Linux), its aggregated CEIR profile and
allocator totals far cheaper than Tracy's per-event equivalents, and its disabled sites, zones and marks at or below
Tracy's no-client cost; its frame mark costs 4 ns more (the lifetime pin and the history Tracy's mark does not keep).
**Loss:** the four-thread jobs frame on Windows, +15.9% (+14.6 .. +17.3) over Tracy connected on the final board (+4.8%
and +12.6% on two earlier boards of the same workload before the last source changes), from the flight recorder's shared
claim and slots (and an unexplained 2.9% from the cycle-counter clock at four threads), owned as follow-up (q). No peer:
basic's severity filter, the allocator watch, crash capture, triggers and bundles, replay, device validation, the host
frame; GPU scopes not measured.
