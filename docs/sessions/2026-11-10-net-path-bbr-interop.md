# 2026-11-10 — NET-PATH: BBRv3 against ngtcp2 through a relay bottleneck

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S69](../design/network-foundation.md#bbr).

## CI first

The only failed job in scheduled run 42801965828 is win-arm64-debug, on `fuzz_congestion.cpp` C4324. That run is at
efed53d5, before 1d964d5c's pragma. No finished run contains 1d964d5c or later yet (42824103727 is pending).

## What landed

- `crd-quic-peer --congestion newreno|cubic|bbr`. Its report now names the controller and gives BBR's model: bandwidth
  and min_rtt_ns (both 0 for a window controller).
- `crd-ngtcp2-quic-peer --cc reno|cubic|bbr`: the second controller is ngtcp2's own (`ngtcp2_bbr.c`).
- `Relay(rate, buffer)` in `interop_common.py`: each direction is a bottleneck with a drop-tail queue. It counts tail
  drops, the mean queueing delay, and how much of the link full datagrams use. PMTU oracles treat a bottleneck as
  lossy (`drops()`).
- `quic_interop.py` gains 20 cases (156 in all):
  - Cerid BBR on the loopback, and NewReno on the loopback.
  - Through the bottleneck (2.5 MB/s, 30 ms, a two-BDP buffer), in versions 1 and 2, against ngtcp2's CUBIC, Reno
    and BBR, and with 1% random loss.
  - Over 5% loss with duplication and jitter, over CE marking, and over a 1350-byte MTU.

## Evidence (WSL linux-gcc-public-checks, ~/cerid-track-c-build)

- Full driver: 156 of 156 cases pass in 241 s, after one oracle fix (below). The ten bottleneck cases ran 100 times
  with no failure.
- BBR's minimum RTT is 30.1 to 31.3 ms on a 30 ms path. Cerid's direction never overflowed the buffer.
- A Cerid client's model held 0.70 to 1.17 of the rate. It used 0.92 to 0.94 of the link clean, and 0.84 to 0.88
  under 1% and 2% loss. In the same runs CUBIC used 0.24 and 0.16.
- Teeth: forcing Cerid to CUBIC in the 1%-loss case fails the utilisation oracle (0.24 of the link).
- Two oracle bounds were wrong and are corrected:
  - A server's echo sends only what it reads, so behind a CUBIC client under loss its model held 0.31 of the link.
    The rate floor now applies to a client only.
  - ACK frames bunched by the Python relay let one sample read 1.26 of the link. The ceiling is 1.5, and the queue
    bound is one round trip, which BBR's two-BDP window enforces.
- `check-generated`, `check-repository` and `check-allman-braces` pass.

## Next

Prague (L4S, ECT(1) with an AccECN-style reaction) on the same sampler. After that: the delay-based controllers,
migration, the fast paths, multipath, and the msquic/quiche boards.
