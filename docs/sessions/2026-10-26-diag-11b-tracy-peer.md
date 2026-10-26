# DIAG.11b clause (8): Tracy Profiler as the peer on comparable features

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b). Board:
> [Tracy peer](../bench/2026-10-26-diag-11b-tracy-peer.md). Pin: [pinned inputs](../design/pinned-inputs.md).
> Rules: [AGENTS](../../AGENTS.md).

The design rule is "use peer tools only on comparable features". The user chose Tracy Profiler 0.14.1 and approved
its download; the operator saved the commit-addressed source archive and the Windows release tools under
`build/pin-tracy` (SHA-256 `e9d52734…` and `f7499d74…`, both checked), and nothing else was fetched. The hosted
host-check run 40241945293 was in flight and was not touched; no workflow was started.

## The pin and the peer harness

- `cmake/pins.json`: `packages/tracy` (commit-addressed, BSD-3-Clause read from the archive's LICENSE, gated by the
  new `CRD_DIAG_PEER_TRACY`, off everywhere), `tools/tracy-capture-windows` and `tools/tracy-csvexport-windows` (the
  release archive, each with its member digest). The license manifest and its generated-sources digest were
  regenerated; `check-pins.py` checks ten packages and seventeen tools.
- The root build adds the archive only with the option and builds the client's one unit from a build-owned copy of
  its `public/` tree, as `tracy-client` and `tracy-client-osclock` (the same client on the operating system's clock),
  without link-time optimization and outside the engine's warning set. The pristine unit drew three GCC 13 warnings on
  linux-gcc-shipping, each a real 0.14.1 defect: two registered patches (`tracy-profiler-checked-calls`: a checked
  `pipe()` and bounded host-description writes; `tracy-syspower-checked-scan`: a checked `fscanf`) leave both lanes'
  builds warning-free. 0.14.1 still reads `TRACY_DELAYED_INIT`: with manual lifetime alone the constructor and
  `StartupProfiler` would both start the worker threads.
- `crd-diag-overhead-tracy` is `crd-diag-overhead`'s sources with `CRD_DIAG_OVERHEAD_TRACY=1` and
  `tracy_peer.cpp` (a source route under the option): the Cerid modes and `--mode tracy` in one binary. Every
  application site is `DOV_SITE` (exactly `CRD_FLIGHT_RECORD` in the Cerid modes; a guarded `TracyMessage` in the
  tracy mode); jobs are zoned through crd-jobs' observer, CEIR ops through the plan's `RunHooks`, frames marked with
  `FrameMark`, allocations reported with `--memory`. Its nine `crd-diag-overhead-tracy-*` CTests (only with the
  option) run every workload with no client.
- The common harness gained `zones` (2,048 `CRD_PERF_SCOPE` a frame), `marks` (64 frame marks a frame) and
  `--memory` (the heap registered with the profiler in full), with structural checks (every zone recorded and none
  dropped, every mark counted, the statistics complete); its CTests are 27 on Windows and 28 on Linux. The record
  states `tick_clock_mapped` and a `tracy` object.
- `scripts/diag-overhead.py`: the arms `tracy` and `tracy-connected` (a verified `tracy-capture` per process on its
  own port, the capture held to every zone, frame and message), `--tracy-tools`, `--memory`, the peer comparisons
  (reported only), per-event costs for the site workloads, and `instructions --arms`. `test-diag-overhead.py` 22 → 29
  cases. `scripts/sample-overhead-wpr.py` attributes crd-time's clock frames to their callers (TickClock is the
  per-event clock now, as `steady_clock` was), with a mutation that fails `test-sample-overhead-wpr.py` when dropped.

## What the boards found, and what was repaired

The board and its tables are in the [bench note](../bench/2026-10-26-diag-11b-tracy-peer.md). Before any repair
Cerid lost on all three comparable events on Windows: a zone +7.1 ns, a frame mark +70.3 ns, a kept site +4.6 ns.

1. **The zone's clock.** The discriminator build (Tracy on `steady_clock`) put Tracy's zone at 32.7 ns against
   Cerid's 27.8: the gap was entirely the two `steady_clock` reads. crd-time's new `TickClock` maps the cycle counter
   onto MonotonicClock's timeline (calibrated against it for 2 ms, re-anchored every 8 ms by whichever reader passes
   the period, the slope over the whole baseline, never stepping back, MonotonicClock itself where the counter is not
   invariant; a reader spins while a re-anchor holds the map, a trade-off taken over a lock). The profiler's zones and
   marks and `diagnostic_now_ns()` read it; `cp::init`, `DiagnosticHost::start` and the harness warm it. Zones −7.7 to
   −5.9 ns, sites −1.3 to −1.7 ns.
2. **The frame mark** zeroed a 3.6 KiB copy of its history slot before copying the used part: it writes the slot
   directly, its two single-writer counters are stores, and an allocator-free mark takes no snapshot pin: −65.4 ns.
3. **Off**: a stopped profiler's entry points took the lifetime pin's two RMWs before looking (a disabled mark −8.6
   ns); a zone with no profiler made two calls (now an inline relaxed load and no call).
4. **A latent bug found on the way**: `CRD_PERF_SCOPE`'s static was initialised by `intern_name` on the site's first
   pass, so a site first reached before `cp::init` kept `kInvalidNameId` for good. `detail::SiteName` (constant
   initialised, interned on the first pass while a profiler runs) names it; in one binary it costs the same as before.
5. TickClock's first pairing took one pair per anchor and missed MonotonicClock's bracket by 20 µs under
   ThreadSanitizer (a slow, intercepted clock): the tightest of eight pairs now (worst 304 ns on win-debug; 20 TSan
   repeats clean).

Teeth (each set back by binary replace on win-debug, `crd-perf-tests "[diag11b]"` rebuilt and run, then restored and
rebuilt): zones back on MonotonicClock fail `test_scope_push_pop.cpp:61,62`; the frame mark's clock fails `:65`;
`diagnostic_now_ns` on `steady_clock` fails `:66`; the old guarded static fails the late-start name case (`:86`).
`test-sample-overhead-wpr.py` fails with the clock frames dropped. TickClock's own cases (`[tick]`): reads within
20 µs of MonotonicClock through the calibration and four re-anchors, no thread's reads step back while four threads
re-anchor, a forced map is what a read returns.

## What is left

- **The jobs loss**, owned as DIAG.11b follow-up (q): at four threads Cerid's kept site per job costs 71.8 µs a frame
  against Tracy's 50.6 µs (the jobs adapter is cheaper than Tracy's zone per job: 26.0 against 36.6). Padding the
  events (no change), a `fetch_add` claim (no change) and thread-private claims and slots (45.5 µs, below Tracy) put
  it on the recorder's one claim word and shared slots, the price of its single total order; removing it changes the
  trigger window's contract. With it, the four-thread jobs frame on the cycle-counter clock is +2.9% over
  `steady_clock` in one binary, mechanism not found (not re-anchoring, serialisation or the map's cache line).
- (8)'s remaining part is unchanged: the user's elevated sampling re-record of the repaired jobs pool.

## Verification

win-debug: `crd-perf-tests` (306 cases), `crd-time-tests` (69 cases). win-shipping-profile: the 27 harness CTests and
the 9 peer CTests; win-shipping: 27. linux-gcc-shipping: 28 + 9, the TickClock cases; linux-clang-tsan:
`crd-time-tests`' clock cases 20 times. The repository guards, `check-master-plan.py`, `check-repository.py`,
`check-pins.py` and `check-ci-tiers.py` at the end of the session. Hosted once pushed: every lane builds
`crd-diag-overhead` and passes its CTests (27 on Windows, 28 on Linux, nonzero execution), the time and perf suites
pass with TickClock on every lane (TSan included), the cold lanes fetch and verify the Tracy archive among the
unselected pins, and no lane builds Tracy.
