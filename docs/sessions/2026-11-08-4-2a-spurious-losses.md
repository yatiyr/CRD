# 2026-11-08 — 4.2a: spurious losses raise the thresholds

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [4.2a](../ROADMAP.md#slice-4.2a), [NET.3](../ROADMAP.md#slice-net.3).
Design: [S37](../design/network-foundation.md#quic-recovery).

## CI first

- Dispatch 42435554329 (contains 9c7a6717): 13 jobs lost their tool archives to GitHub's HTTP 504 before building.
  crd-net-tests and every net corpus pass on win-clang-cl, win-clang-cl-shipping, win-arm64-debug, the four ARM64
  Linux lanes and win-asan's first shard. NET.3 still needs win-debug (MSVC) and win-asan's second shard, which
  complete-tier dispatch 42473131224 (on 67168725) will cover.
- `scripts/pins.py` now waits 120 s and 300 s after the first three retries, longer than that outage, which also failed
  42463343148 and 42468367943.
- This batch did not touch the GPL rewrite: the relayed words belong to the other agent (operator note, 2026-11-07).

## What landed (capability `network.transport`)

- A lost packet stays held for one probe period, or until a send needs its slot. A later acknowledgement of it counts
  as spurious: there is no event, nothing goes back in flight, and `QuicAckOutcome::spurious_*` records it. Any ACK the
  packet carried is retired, and it gives an RTT sample when it is the frame's largest.
- RFC 9002 section 6.1, using RFC 8985 section 6.2's mechanism:
  - the packet threshold rises to the reordering seen, measured from the largest acknowledged before the frame and
    capped at 32;
  - after a loss declared by time, the time window grows by min_rtt/4 once per round trip, capped at smoothed_rtt;
  - both go back to RFC 9002's values after 16 loss detections with no spurious loss.
- The fuzz model checks each loss against the thresholds that detection used. `quic.recovery` names its evidence.

## Evidence

- crd-net-tests passes on win-debug (MSVC, 282 cases) and linux-gcc-debug (283 cases), with the guards and the net
  corpus replays. LLVM 20 strict tidy is clean.
- 20 source mutations of the new code, all caught; S37 now has 43. Three of them first survived and are now covered
  by tests: the reordering's base, the packet threshold's decay, and a send freeing only the slot it needs.
- Hostile datagram path (10% loss, 20 ms jitter, same seeds): Acked went from 1,812 to 5,640 and Lost from 5,388 to
  1,560.
- linux-clang-fuzz `net-quic-recovery`: 1.74 M executions in three runs. Each run stopped on an event that does not
  replay (a jump to address 0, a pc on the stack, a `this` of 0x6c). The same target built from HEAD without this
  change failed the same way after 26 k executions. That points to the host fault on record since 2026-10-11. Hosted
  host-check legs 8201 and 8202 run this target (42475887688).

## Next

- Read 42475887688's two net-quic-recovery legs. Then read 42473131224's win-debug and win-asan shard 2, and close NET.3.
- 4.2a: this side's connection IDs and stateless reset, Retry and NEW_TOKEN on the client, compatible version
  negotiation's switch, 0-RTT, the IngressHandler, interop with a pinned peer, TCP/TLS and the browser endpoints.
