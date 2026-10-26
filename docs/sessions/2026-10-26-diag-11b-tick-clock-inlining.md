# DIAG.11b follow-up (q): the cycle-counter clock's slowdown on the four-thread jobs frame, explained and fixed

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b), follow-ups (q) and (r). Board:
> [tick clock inlining](../bench/2026-10-26-diag-11b-tick-clock-inlining.md). Previous session:
> [Tracy peer](2026-10-26-diag-11b-tracy-peer.md). Rules: [AGENTS](../../AGENTS.md).

The user's assignment had two parts in order: explain and fix (or disprove) the unexplained +2.9% of the four-thread
jobs frame with crd-time's `TickClock` against `steady_clock`, confirm it on linux-gcc-shipping and give it a board
or test with teeth; then write the per-thread-ring design brief of follow-up (q) without implementing it on main
([brief](2026-10-26-flight-recorder-per-thread-rings-decision.md)). Complete-tier run 40255964357 was in flight on
`82ed7559` and was not touched; no workflow was started. The advisor was consulted before the experiments and before
the fix.

## What was measured first

- The earlier session's "one binary" claim could not be audited (its clock-switch experiment was not kept), so it was
  rebuilt: an environment switch in `counter_invariant()` sends TickClock down the non-invariant path (MonotonicClock
  for every read), leaving the tick arm's code identical to production. It reproduced: the system clock −2.90%
  (−4.35 .. −1.61) on the full frame, −5.0% with the sites off, the off arms equal.
- Thread count: TickClock is cheaper at one thread (+4.2% to +5.1% for the system clock), even at two, slower at four.
- A decomposition in one binary put the read's cost half on `rdtsc` (+16.5 µs a frame) and half on the map's loads
  and arithmetic (+18.4 µs) — seven loads of a read-only line should cost a fraction of that.

## The cause

cdb on the shipping binary: the mapped read made six out-of-line calls to `std::atomic` load stubs (three of them
ICF-folded into `std::basic_string_view<char>::data`) inside a six-register frame. The same stubs are called from
crd-perf's `pop_region`, `StripedCount::add` and the jobs adapter. Why: MSVC LTCG keeps one body per inline function,
the harness's own units (crd-math consumers, `/arch:AVX2`) link first, and LTCG does not inline an AVX2 body into the
baseline-ISA crd-time and crd-perf. A two-file probe reproduced it with nothing but `std::atomic`, by link order.
Building crd-time and crd-perf at `/arch:AVX2` (an experiment) inlined the loads, and TickClock came out cheaper on the
frame. On the four-thread frame each job's path is bounded by locked operations on contended lines, so the read's
latency (a chain of calls) reaches the frame, where a read loop measures its throughput.

Ruled out on the way: core frequency (each job's loop takes 330 TSC ticks in every arm); `lfence` or `rdtscp` around
the counter (not faster). Discarded: two experiment arms whose shared volatile sink word added its own line
contention, and VTune's user-mode attribution of the difference to the job's loop (disproved by the TSC timing).

## The fix

- `engine/foundation/time/src/clocks.cpp`: the mapped read loads its map through functions of its own unit — with
  MSVC on x64 the compiler's `__iso_volatile_load32/64` and a compiler barrier (what `std::atomic`'s acquire and
  relaxed loads are on x64), elsewhere `std::atomic`'s own loads with the acquire fence; `unmapped_now` (the
  calibration) is out of line like the re-anchor. cdb: no call in the mapped read but the re-anchor's.
- `TickClock::prefer_system()` (crd-time): MonotonicClock for good, before the first read only (a mapped clock moved
  later could step back); the non-invariant CPU's path, for an untrusted counter and for the board.
- `crd-diag-overhead --clock <tick|system>`: the record states `build.clock`; `--check` holds the clock to the one
  asked (the counter wherever it may be mapped, MonotonicClock with `--clock system`); new CTest
  `crd-diag-overhead-jobs-full-system-clock` (28 harness CTests on Windows, 29 on Linux).
- `scripts/diag-overhead.py`: arm `full-system-clock`, a record whose clock is not its arm's is an instrument failure,
  and comparison `tick-clock` (full over full-system-clock) judged at 0% on a frame of work, reported per event on the
  site workloads. `test-diag-overhead.py` 29 → 32 cases.

## Evidence

- Windows, `diag-overhead.py run`, 10 processes: fixed, jobs −2.22% (−4.35 .. −1.29) frame and −2.47% CPU, within;
  a kept site 6.2 ns and a zone 12.7 ns cheaper than on the system clock (69.9 and 16.5 ns over off). Teeth: the same
  source with the read back on `std::atomic`'s loads, +4.16% (+2.86 .. +5.89), over.
- linux-gcc-shipping: GCC inlined the loads before and after; 24 processes, −1.22% (−2.14 .. −0.04), within (10
  processes: unresolved, −1.64% (−2.92 .. +0.64)); a site 6.7 ns cheaper.
- Teeth of the unit case `TickClock: prefer_system before the first read keeps every read on MonotonicClock`
  (`[tick]`, assert builds): `prefer_system` set back to leave the state Unstarted fails `test_clocks.cpp:219`, `:225`
  and `:227` on win-debug; restored, 4 of 4 `[tick]` cases pass.
- The engine-wide defect (every baseline library linked after an AVX2 unit under MSVC LTCG; in crd-perf alone it is
  most of the 340 → 311 µs the experiment build showed on the full frame, a cross-binary number) is not fixed here:
  making the ISA uniform changes every library's code generation, GCC's `-mfma` and the determinism flags' scope
  (ADR-0063) and the relocatable package's `Cerid::simd-flags`; owned as DIAG.11b follow-up (r), a decision for the
  user. Lesson: `feedback_msvc_ltcg_does_not_inline_an_avx2_unit_s_inline_body_into_a_baseline_isa_library`.

## Verification

win-debug: `crd-time-tests` (70 cases), `crd-perf-tests`, the 28 harness CTests; win-shipping-profile: the 28 harness
CTests and `crd-time-tests`; win-clang-cl-shipping: crd-time compiles warning-free; linux-gcc-shipping: `[tick]`, the
board above. LLVM-20 tidy clean on the four changed C++ files. Hosted once pushed: every lane builds the harness and
passes its CTests (28 on Windows, 29 on Linux) with `jobs-full-system-clock`, the time suites pass with the new
`[tick]` case on the assert lanes (TSan included), and the repository job passes `test-diag-overhead.py`'s 32 cases.
