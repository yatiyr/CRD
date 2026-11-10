# 2026-11-10 — NET-PATH: Prague (L4S) and classic ECN AQM detection

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S70](../design/network-foundation.md#prague).

## CI first

No run containing this track's newer commits has finished: 42833630111 (484a423c) is pending and 42811793243 queued.
The last finished runs (42774048974, 42762080661, 42744641737, 42736485342) were read in earlier batches.

## What landed

- Congestion interface version 3: `on_ecn(marked, ce, newest_sent_ns, now)` replaces `on_congestion`, and
  `on_ecn_unavailable()` reports a path without ECN once. `QuicEcnValidator::on_ack` returns the CE rise, and
  `set_codepoint()` follows a live swap. `QuicConnectionConfig::ecn` is now a switch: the codepoint is the
  controller's (`congestion_codepoint()`: ECT(1) for Prague, ECT(0) otherwise).
- `CongestionAlgorithm::Prague` (draft-briscoe-iccrg-prague-congestion-control-04, RFC 9331 section 4.3). Alpha runs
  from the first CE with gain 1/16. A CE outside CWR reduces the window by alpha / 2. Only unmarked bytes grow it.
  A loss halves it, Reno-combined with a recent mark. After 500 rounds growth scales by (srtt / 25 ms)^2, with alpha
  and CWR on the virtual round trip. It paces at 1x (2x early in slow start) with a 250 us burst. Without ECN it is
  Reno.
- `crd/hesap/net/classic_ecn.hpp` (header-only, `numerics.network-estimators`): TR-BB-2019-002 v03's passive
  detector, its averages, score, stickiness and idle halving in integer arithmetic. Prague's reduction moves toward
  ABE's 30% as the score rises (alternative 2).
- Fixtures: `bottleneck_fixture.hpp` (BBR's simulated bottleneck, now shared, with ECN feedback per packet). The
  QUIC fixture gains a DualQ-style ECT(1) step (`ce_queue`) and re-marks both ECTs.
- `crd-quic-peer --congestion prague` reports `alpha` and `classic`.
- Register: `path.l4s-ecn` implemented; `cc.prague` watched (draft -04, posted 2024-07-24, expired 185 days later
  with no successor); `cc.classic-ecn-detection` implemented.

## Evidence

- win-debug: `crd-net-tests [net]` ran 700 cases: 699 passed. The failure is `test_stream_socket.cpp:137`
  (`shutdown_send` returned Reset), in code this batch did not touch, during a run beside a second test process;
  `[tcp]` passed on its rerun (27 cases). Unexplained: CI's Windows lanes are watched for it. `[prague]`: 9 cases, 17.2 million assertions. `crd-hesap-net-tests ~[kernels]`: 159 cases. Both fuzz
  targets' seeds and corpora replay clean, with the new `prague-marks` seed.
- WSL linux-gcc-debug: the four targets build with no warning; `[prague],[congestion],[bbr],[quic-ecn],[net-path],
  [delivery-rate]` pass (88 cases, 45.6 million assertions), and `[classic-ecn],[estimators]` (29 cases).
- linux-clang-fuzz, `fuzz.py run`: net-congestion 350 s and 1.65 million executions clean, net-quic-ecn 175 s and
  1.72 million clean (scratch corpora; the committed corpus gains only the generated seed).
- Figures in S70: 93.5% of a 10 MB/s, 20 ms link at a 1 ms step with at most 1.1 ms queued (CUBIC 85.5%, NewReno
  77.6%). Over QUIC, 94.9% of 2 MB/s, 50 ms with at most 1.8 ms queued, where CUBIC's ECT(0) queues 80 ms.

## Next

The bottleneck at 2 and 5 ms steps and at 50 ms: utilization must rise toward the link as the step passes a seventh
of the BDP, or the shortfall is Prague's. Then Prague against picoquic's Prague through the relay with an ECT(1)
step, and a two-flow bottleneck (Prague beside CUBIC in one classic queue) for the fallback's coexistence. Then the
delay-based controllers, migration and the fast paths.
