# 2026-11-08 — 4.2a: spurious losses raise the thresholds

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [4.2a](../ROADMAP.md#slice-4.2a), [NET.3](../ROADMAP.md#slice-net.3). Design: [S37](../design/network-foundation.md#quic-recovery).

## CI first

- Dispatch 42435554329 (contains 9c7a6717): all 13 failed jobs lost their tools (ninja, sccache, actionlint archives) to
  GitHub's HTTP 504 before building. crd-net-tests and every net corpus pass on win-clang-cl, win-clang-cl-shipping,
  win-arm64-debug, the four ARM64 Linux lanes and win-asan's first shard. win-debug (MSVC) and win-asan's second shard
  remain for NET.3, so complete-tier dispatch 42473131224 was started on 67168725.
- The same outage failed 42463343148 and 42468367943 at Preflight. `scripts/pins.py` now retries a download after
  5, 15, 45, 120 and 300 s, which is longer than that outage.
- The relayed GPL-rewrite words belong to the other agent (operator note, 2026-11-07); this batch did not touch it.

## What landed (capability `network.transport`)

- `QuicRecovery` keeps a lost packet in its slot for one probe period, or until a send needs the slot. A later
  acknowledgement of it counts as spurious: it raises no event and puts nothing back in flight. It is counted in
  `QuicAckOutcome::spurious_*` so a controller can undo, it retires any ACK the packet carried, and it gives an RTT
  sample when it is the frame's largest.
- Thresholds follow RFC 9002 section 6.1 using RFC 8985 section 6.2's mechanism. The packet threshold rises to the
  reordering measured against the largest acknowledged before the frame, capped at 32. A loss declared by the time
  threshold widens its window by min_rtt/4, once per round trip, capped at smoothed_rtt. After 16 loss detections
  with no spurious loss, both return to RFC 9002's values. `packet_threshold()`, `reorder_steps()` and
  `loss_delay_ns()` show the current values.
- The fuzz model checks each loss against the thresholds that detection used. Spurious counts are bounded by the
  model's candidates, and the thresholds stay within their caps. Standards register: `quic.recovery` now has its
  evidence.

## Evidence

- crd-net-tests: 282 cases pass on win-debug (MSVC) and 283 on linux-gcc-debug, with the `crd-no-*`/`crd-check-*`
  guards and the net corpus replays. The recovery suite has 6 new cases. LLVM 20 strict tidy is clean on the 5
  changed C++ files.
- 19 source mutations of the new code, all caught. Two had survived at first and are now covered by tests: the
  reordering measured from the largest acknowledged before the frame, and the packet threshold's decay. S37: 42.
- Hostile datagram path (10% loss, 20 ms jitter, same seeds): before this change 1,812 datagrams were Acked and 5,388
  Lost; now 5,640 are Acked and 1,560 Lost.
- linux-clang-fuzz `net-quic-recovery`: three runs on the new code made 1.74 M executions. Each stopped on an event
  that did not replay: a jump to address 0, a pc on the stack, and a `this` of 0x6c. Every artifact and corpus replays
  clean in both builds. As a control, the same target built from HEAD (without this change) failed the same way after
  26 k executions ("member call on null pointer" of a reference). That points to the host fault recorded since
  2026-10-11, not to this code. Host-check legs 8201 and 8202 now run this target on hosted CPUs.

## Next

- Read the host-check legs for net-quic-recovery, then NET.3's two lanes in 42473131224, then close NET.3.
- 4.2a: this side's connection IDs and stateless reset, Retry and NEW_TOKEN on the client, compatible version
  negotiation's switch, 0-RTT, the IngressHandler, interop with a pinned peer, TCP/TLS and the browser endpoints.
