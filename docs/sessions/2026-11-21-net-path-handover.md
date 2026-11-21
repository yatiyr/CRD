# 2026-11-21: NET-PATH, scheduled handovers on LEO links

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path) (Partial). Design: [S76](../design/network-foundation.md#handover)
(StarQUIC, LEONET 2024, read in full); the GCC fix in [S74](../design/network-foundation.md#gcc-probing).

## CI first

Runs 44861565940 (15bea98a) and 44875047802 (cf247ec8) had no failed job when this batch started. Later
44861565940's win-shipping failed with its log cut off mid-way through the numerics tests and its Test step never
completed: the hosted runner was lost, not a test failure. 44875047802 had not started.

## What landed

- `handover.hpp`: `HandoverSchedule` (period, phase, guards on the connection's clock), its validity (guards at
  most an eighth of the period), coverage, and Starlink's 12-27-42-57 preset from a clock and UTC reading with a
  sender's lead.
- `QuicConnection`: `handover` in the configuration and `set_handover()`. A packet sent while covered is marked;
  its loss never reaches the controller or the sampler's loss count, nor its spurious acknowledgement (so it
  undoes no genuine reduction), nor DPLPMTUD's black hole detection (`handover_losses`); its RTT sample and peer
  receive time are no delay signal; no PMTU probe leaves while covered; a move that restarts the path's state
  clears the schedule. `QuicRecovery`: covered packets start or end no persistent congestion run, and
  `rtt_handover` names a covered sample.
- GCC defect found by the LEO runs: before an acknowledged rate an overuse halved the rate once per ACK frame
  (12 halvings in 200 ms to the 5 kbit/s floor after a probe found a 10 Mbit/s link with a one-BDP queue; 16 MiB
  took 344 s). It now halves at most once 200 ms, libwebrtc's initial backoff interval.
- Test fixture: `Path::down_at`, a hop that goes down on a timetable.

## Evidence

- WSL linux-gcc-debug: crd-net-tests 1008 cases pass; `[handover]` 10 cases; the fuzz seeds and every
  `crd-check-*`/`crd-no-*` guard pass (the ISA guard after rebuilding a stale crd-core-tests).
- Teeth, one build: restoring the spurious undo, the RTT and receive-time signal, the PMTU probe, the clear on a
  move and the persistent congestion run each failed its test.
- Measured (design S76): BBRv3 4 reductions instead of 6 in the 2 s after each handover and 7% more delivered;
  SCReAM's least target 23% higher, GCC's 53% with 36% more delivered; a 1.2 s outage inside the guards keeps
  CUBIC out of persistent congestion (57% more delivered after it).
- GCC teeth: halving once a frame again made 6 halvings in the 200 ms the test watches (`test_gcc.cpp`).
- libFuzzer (linux-clang-fuzz): `fuzz_quic_connection` with the new 0xF6 op and seed, 481 s, 62401 runs clean;
  `fuzz_congestion` 241 s, 739290 runs clean.

## Next

Learning the handover schedule without a synchronised clock (HESAP-NET estimator from the latency pattern and the
known period), then the GSO/GRO, io_uring, USO/URO and RIO fast paths.
