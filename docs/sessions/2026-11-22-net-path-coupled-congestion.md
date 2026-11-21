# 2026-11-22 — NET-PATH: coupled congestion control (RFC 6356) and the Windows CI failures

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath), its
"Coupled congestion control" bullet. Sources: RFC 6356 sections 3 and 4 and RFC 10069 section 5.3, read 2026-11-22.

**CI first.** Run 44941356765 (0799aca4) failed five Windows lanes on the multipath packet case, a stack overflow
that 48bc7127 had already fixed (it passes on win-debug at HEAD; the other Windows lanes wait on a hosted run).
win-tidy found four findings there; two (unused usings) were already fixed at HEAD. A local run of the pinned
clang-tidy over this track's later files found three more. 52060afb and 0f6776f5 fix the five left: local constants
in lower case, a nested conditional, `std::ranges::any_of`, and a self-assignment guard.

## What landed

- `CongestionPolicy::coupled` (congestion interface version 7): NewReno's congestion avoidance coupled across one
  connection's paths by RFC 6356's LIA. Equations 1 and 2 are one exact integer divisor, cwnd_total / alpha =
  (SUM s_j)^2 / MAX (s_j^2 / cwnd_j), at least the member's own window; one member alone is NewReno to the byte.
- `CongestionCoupling` holds up to four members. A controller joins in `make_congestion` and leaves in its
  destructor. `QuicPathControl::start` takes the coupling, and the connection's coupling is declared before its
  controls. Slot 0 joins it now; later slots join when the lifecycle opens them.
- A member counts min(cwnd, flight) once it has an RTT sample. A policy that couples any controller other than
  NewReno is refused (RFC 6356 defines none). Register entry `cc.lia` pins RFC 6356.

## Evidence

- `[coupled]`: 7 cases. One of them follows a control's membership through starts, swaps, a move, a reset and
  destruction; another follows a connection's through a transfer and two live swaps. Group-of-one identity over 30 s
  with loss. The divisor matches a floating-point reference over 2000 random sets, at the RTT bound and at
  kCongestionMaxWindow. Simulated bottleneck results:
  - Two coupled paths beside a NewReno flow take 1.19 of its share; uncoupled, they take 1.96.
  - Two disjoint links each stay 99.9% full.
  - A path with ten times the loss carries 9.7% of the traffic coupled, against 25.4% uncoupled. The total is 1.04
    times the better path alone.
  - Paths with RTTs of 20 and 160 ms are both filled.
- `crd-net-tests "[net]"`: 867 cases on linux-gcc-debug, 7 skipped as before. The `crd-check*`, `crd-no-*` and
  corpus CTests pass. `[coupled],[path_control],[quic-connection]` passes on win-debug. The pinned clang-tidy reports
  every changed file clean.
- Congestion fuzzer: a twin oracle (a coupled NewReno and an uncoupled twin keep one state) and the seed
  `reno-coupled` (seed-3a00952bf7a40d62). The corpus replays clean.
- libFuzzer on linux-clang-fuzz, 2 jobs for 292 s: 1.8 M runs, and the corpus grew from 13 to 4725. It reported
  one artifact that does not replay and whose cause is unconfirmed: an ASan "stack-overflow" in GCC's trendline with
  the program counter on the stack. That path is untouched here, apart from the `each()` event wrapper. The artifact replays clean in the libFuzzer build, in the
  GCC debug build and under valgrind, and does not recur in 3000 mutated runs from it. All 4725 corpus inputs
  replay clean. This host's known misexecution (MEMORY: host-misexecutes) is the likely cause, so the fuzz
  host-check runs on GitHub's CPUs (net-congestion 8501/8502).
- Teeth: a divisor one too large fails three cases and the twin oracle. Coupling switched off fails both
  bottleneck goals. A control or connection that does not pass the coupling fails both membership cases.

## Next

The path ID lifecycle: one batch lands the server-side open, PATH_ABANDON with retention, the fuzzer's oracles and the
raised limit together, driven by the raw peer in `test_quic_connection.cpp` (S78 "Lifecycle").
