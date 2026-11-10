# 2026-11-10 — NET-PATH: GCC, the second delay-based real-time controller

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S72](../design/network-foundation.md#gcc) (sources there).

## What landed

- CI first: win-tidy in run 42856002748 refused `kInf` in `test_bessel.cpp` (a local constant); 7c8076b4 makes it a
  static constant. Its other two refusals were already fixed at HEAD. Host check 42892963665 ran both `net-congestion`
  legs (seeds 8501, 8502) clean on GitHub's machines, so the last batch's unreplayable libFuzzer events stay this
  PC's fault. Open: win-asan in 42856002748 failed `Provider: one verifier and one key serve several threads`, now
  naming the call: the first ML-DSA-65 signing of a thread returned `ProviderFailure`; registered 2026-11-11
  ([register note](2026-11-11-crypto-threaded-signing-register.md)).
- Congestion interface version 5: `CongestionAlgorithm::Gcc`, `CongestionPacket::received_ns` (the peer's receive
  time), GCC's delay-based estimate in `CongestionState::bandwidth`. The slot grows to 1536 bytes (GCC is 1520).
- GCC in `congestion.cpp`: HESAP-NET's packet groups and trendline into libwebrtc's AIMD rate control and link
  capacity estimator, the classic loss controller under it, libwebrtc's congestion window and pacing factors, in
  integer rates. Without receive timestamps each ACK frame's RTT less the ACK delay feeds the trend.
- crd-net now links crd-hesap-net (its compiled estimators), and the package exports hesap-net's archive link-only
  (`crd_package_link_only`, [package contract](../design/public-consumption.md)).
- The bottleneck fixture reports receive times on an optional receiver clock offset.

## Evidence

- win-debug (MSVC): `[gcc]` (13 cases), `[congestion]`, `[scream]`, `[prague]`, `[bbr]`, `[quic-connection]` pass;
  the congestion corpus replays clean with the new `gcc-delay` seed. WSL linux-gcc-debug: `crd-net-tests` (926
  cases), `crd-hesap-net-tests` (184) and the corpus replay pass. clang-tidy 20 is clean on the six changed files.
- Simulated, 10 Mbit/s, 40 ms, 500 ms buffer, a media source: 94% of the link with 27 ms queued on average (CUBIC
  424 ms), a 353 ms peak in its slow sawtooth near the capacity, a halved link followed within 5 s, 1% loss
  ignored, 3.5% beside CUBIC. Through QUIC from ACK frames alone: 93% at 2 Mbit/s with at most 35 ms queued, where
  CUBIC queues 291 ms and overflows. The same rates to the byte with and without receive timestamps, and on a peer
  clock 10^18 ns ahead. Hosted fuzzing of GCC: host check 42906546427 (dispatched; owed read).

## User decisions recorded

2026-11-01: "approve the downloads we need to do everything properly, I will contect a lawyer later! DO NOT BLOCK
ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD STANDARD AND
ELITE".

## Next

QUIC receive timestamps (draft-ietf-quic-receive-ts) behind a NET-STD flag, feeding `received_ns`; then libwebrtc's
probing for GCC's climb; then migration, NAT rebinding and LEO handover.
