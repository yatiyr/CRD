# 2026-11-25: NET-PATH, a reordered train passes for no black hole; the layout arms read

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path); the arms are [NET.2](../ROADMAP.md#slice-net.2)'s provider. Follows
[the LEO case trace](2026-11-25-net-path-leo-case-trace.md) (Next 1) and [the layout arms](2026-11-25-crypto-threaded-signing-layout-arms.md).
User decisions on record: "approve the downloads we need to do everything properly" and "I WANT NET DONE PROPERLY AND
GOLD STANDARD AND WITHOUT ANY DEFERS!" (2026-11-01).

**CI first:** `crd-no-std-containers-check` failed on two `std::array` members in 94e5783e's control programs. Fixed:
`InlineBytes` (std::array's layout, one empty byte for a slim session) and a plain array for `pad`.

## The false black holes (traced on linux-gcc-debug)

All 13 `min_mtu` reds of the 2 s handover sweep had one signature: when a delay spike ended, a 42-byte packet sent
after it was acknowledged ahead of three paced 1,472-byte packets sent in the spike. The time threshold declared them
lost one at a time, 1 ms apart: three suspect detections, a black hole, then their late arrival 0.1 ms after. The
earlier guess (small PTO probes after the outage) was wrong.

## What landed

- `QuicPathMtu` reads when each packet left (`on_acked`/`on_lost(sent_ns, bytes)`). Three suspect detections also need
  their losses to span a round trip, `max(smoothed_rtt, latest_rtt)`, passed in by `QuicPathControl::end_detection`.
  Overtaken packets left within the delay the path shed, so they never span one. The object is 136 bytes, down from 152.
- Tests: a `[quic-pmtu]` case for a train declared a packet a detection, its spurious arrival, the span's restart at the
  oldest suspect kept, and the black hole found a round trip on. A LEO case at 2 s handovers (CUBIC blind and scheduled,
  BBR blind, with `LeoLink::seed`/`first_ns`). The fuzzer passes send times and a round trip.
- Teeth: without the span, both new cases fail (all three LEO runs at `min_mtu`).

## Evidence

- Sweep, 4 seeds × 6 first handovers × 6 runs: no `min_mtu` red in 144 runs (spike 30 ms), nor in 432 runs with 80 ms
  and 150 ms spikes, including 80 ms for only 50 ms. The candidate-A checks still fail as before.
- Cost: on the suite's 40 ms paths, a real fall is now found at the next probe timeout: 1,469 to 1,592 ms (CUBIC),
  1,469 to 1,595 (NewReno), 1,470 to 1,640 (two-way); the DATAGRAM case is unchanged at 1,629. The flight after the fall
  left within 20 ms and nothing later was acknowledged until that timeout, so no rule could tell it from the reorder
  sooner.
- linux-gcc-debug `crd-net-tests`: 1,204 cases, all passed (8 skipped). Guard CTests all pass.
- libFuzzer (linux-clang-fuzz): pmtu 2.57 M executions, recovery 263 k, both clean.
- ngtcp2 fall interop: 4 of 4. Cerid WebTransport fall: 4 of 4, three rounds.
- win-debug: `[quic-pmtu]`, `[handover]` and path-control cases pass (41 cases); the control programs build.
- LLVM 22 tidy clean (6 files).

## The layout arms (host checks 45562483404, 45562487761)

Four legs drew a Xeon Platinum 8573C. Three ran every arm clean, `cerid` included. The fourth (leg 8 of 45562483404)
failed `cerid` 28 of 40 and the Cerid-free `control-copy-slim` 22 of 40 (AWS-LC's internal error after about 12.6 s).
Clean there, 40 of 40 each: `control-copy`, the 1, 2 and 3 KiB shifts, `cerid-pad` and `cerid-touch-stack`. So AWS-LC
alone refuses, and the stack layout decides when. Recorded in `nonreproduced-failures.json`.

## Next

1. The ML-DSA refusal: a TP entry needs a deterministic failing set, and this one follows the runner machine. Design
   the gate and the engine handling (the layout the clean arms share) before registering it.
2. Acknowledgement evidence sooner after a black hole's first flight (a floor-sized ack-eliciting packet once three
   suspects stand), proven never to hold the size at the floor without a black hole.
3. The LEO case hardened on candidate A, then the single SipHash draw.
