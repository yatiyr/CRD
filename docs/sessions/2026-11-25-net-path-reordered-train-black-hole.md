# 2026-11-25: NET-PATH, a reordered train passes for no black hole; the layout arms read

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path); the arms are [NET.2](../ROADMAP.md#slice-net.2)'s provider. Follows
[the LEO trace](2026-11-25-net-path-leo-case-trace.md) and [the layout arms](2026-11-25-crypto-threaded-signing-layout-arms.md).
User decision on record (2026-11-01): "I WANT NET DONE PROPERLY AND GOLD STANDARD AND WITHOUT ANY DEFERS!"

**CI first:** `crd-no-std-containers-check` failed on 94e5783e's two `std::array` members. Fixed in ed7057ef with
`InlineBytes` and a plain array; MSVC layouts unchanged (plain 3,352 bytes, slim 48, every offset the same).

**The layout arms reversed batch 10's finding.** Four legs drew a Xeon 8573C: three ran all nine arms clean, the case
too. On the fourth (leg 8 of 45562483404) the Cerid-free `control-copy-slim` refused 22 of 40 (AWS-LC's internal
error after about 12.6 s) beside the case's 28. `control-copy`, the 1-3 KiB shifts, `cerid-pad` and
`cerid-touch-stack` were clean 40 of 40. So AWS-LC alone refuses and the stack layout decides when; one runner, one
dispatch. The register entry now says so. win-asan stays ungated. Host check 45579952691 reruns the arms at ead0f6c1.

## The false black holes and the fix (21794a44)

- All 13 `min_mtu` reds of the 2 s handover sweep had one cause. When a delay spike ended, a 42-byte packet overtook
  three paced 1,472-byte ones. The time threshold declared them lost a detection each, 1 ms apart, and they arrived
  0.1 ms after the black hole.
- `QuicPathMtu` now reads send times. Three suspect detections must also span a round trip,
  `max(smoothed_rtt, latest_rtt)`. It is 136 bytes, down from 152.
- New tests: a `[quic-pmtu]` train case, and a LEO case at 2 s handovers (CUBIC blind and scheduled, BBR blind). Both
  fail without the span. The fuzzer now gets send times and a round trip.

## Evidence (HEAD unless named)

- Sweeps, 144 runs each: no `min_mtu` red with spikes of 30 ms, or of 80 ms lasting 100 ms. 432 more runs (80 ms
  lasting 50 and 150 ms, 150 ms lasting 400 ms) on the rule's first build were clean too.
- Cost: a real fall on 40 ms paths is found at the next probe timeout. CUBIC goes from 1,469 to 1,592 ms, NewReno from
  1,469 to 1,595, the two-way case from 1,470 to 1,640; DATAGRAM stays at 1,629. Nothing after the first flight was
  acknowledged before that timeout.
- linux-gcc-debug `crd-net-tests`: 1,204 cases passed. Guard CTests pass.
- libFuzzer: pmtu 2.57 M and recovery 263 k executions, both clean.
- Interop: ngtcp2 fall 4 of 4, Cerid WebTransport fall 3 × 4 of 4.
- win-debug: 41 PMTU, handover and path-control cases pass. LLVM 22 tidy is clean.

## Next

1. The ML-DSA entry: whether TP-1's shape fits (engine handling, no registered-failure line), with the clean arms'
   layout as the handling.
2. Acknowledgement evidence sooner after a fall's first flight, never holding the size at the floor without one.
3. The LEO case hardened on candidate A, then the single SipHash draw.
