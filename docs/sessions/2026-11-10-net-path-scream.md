# 2026-11-10 — NET-PATH: SCReAM v2, the delay-based real-time controller

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S71](../design/network-foundation.md#scream). Source read:
draft-ietf-ccwg-rfc8298bis-screamv2-03 (CCWG, 2026-11-02), with RFC 6817's base delay.

## What landed

- Congestion interface version 4: `CongestionAlgorithm::Scream`, the policy's target rate bounds, delay target and
  four switches (L4S, jitter reduction, competing flows, link loss tolerance), and `CongestionState::target_rate`
  for the media source. `congestion_codepoint()` takes the policy (ECT(1) for a scalable SCReAM).
  `congestion_initial_window()` replaces three copies of RFC 9002's initial window. The slot grows to 1024 bytes.
- SCReAM v2 in `congestion.cpp`, all of sections 4.1 to 4.5, in integer arithmetic. Its queue delay is the RTT
  sample less the peer's ACK delay (new `QuicRtt::adjusted_ns`) less the base delay.
- crd-hesap-net: `BaseDelayHistory` (RFC 6817) and `QueueDelayHistory` (section 4.5.4) in `queue_delay.hpp`.
  `square_root` and `log2_q16` moved out of `ClassicEcnDetector` into `fixed_point.hpp`, so the two share them.
- The bottleneck fixture gains a media source: a frame every period, at the controller's target rate.
- Fixed on the way: nested conditionals in `test_prague.cpp` that win-tidy refuses.

## Evidence

- WSL linux-gcc-debug: `crd-net-tests` (all 913 cases) and `crd-hesap-net-tests` (184) pass. win-debug (MSVC):
  `[scream]`, `[prague]`, `[congestion]` and the queue delay cases pass. clang-tidy 20 is clean on every changed
  source.
- Simulated: SCReAM keeps 99.7% of a 10 Mbit/s, 40 ms link with a 500 ms buffer and at most 59.8 ms queued, where
  CUBIC fills 499 ms. A media source settles at the link's rate. When the link halves, it follows within 5 s with
  at most 65.6 ms queued. An L4S step of 1 ms holds at 2.2 ms with 99.9% of the link. Beside CUBIC it keeps 16.4%,
  against 4.2% with a fixed delay target. QUIC at 16 Mbit/s and 50 ms: 96.3% with 31 ms queued, where CUBIC queues
  124 ms.
- Fuzzing: a new `scream-delay` seed. The corpus replays clean under GCC ASan. Seven of sixteen clang libFuzzer
  sessions (150 to 300 s each) ended in events no input replays: a pc on the stack or in the target's storage, a
  small-integer frame, a null this, a zero-page write. Each artifact and the working corpora replay clean. One seed
  stopped once and ran clean when repeated. That is this host's known
  fault ([host separation](../design/test-instruments.md#host-separation-diag10a)). Two `net-congestion` legs join
  `.github/workflows/host-check.yml` for a hosted answer. The same evening `test-repository-tools.py` died twice on
  this host (a regular expression engine internal error, then a segfault) and passed on the third run.

## User decisions recorded

2026-11-01: "approve the downloads we need to do everything properly, I will contect a lawyer later! DO NOT BLOCK
ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD STANDARD AND
ELITE".

## Next

Dispatch the host check for `net-congestion` and read it. Then GCC: QUIC receive timestamps behind a NET-STD flag,
and GCC's rate control and loss controller over crd-hesap-net's trendline and acknowledged rate. Then migration,
NAT rebinding and LEO handover.
