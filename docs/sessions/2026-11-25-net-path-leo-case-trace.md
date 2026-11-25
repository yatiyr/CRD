# 2026-11-25: NET-PATH, why the LEO case's aware CUBIC run undoes a reduction

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path), the bulk-path item's single SipHash draw. Follows
[the sender's gates](2026-11-25-net-path-sender-gates.md), Next item 1. Ran on this PC (WSL2, linux-gcc-debug).
No engine or test change landed; the single draw stays out.

**CI first:** nothing to fix. CI 45496952342 (a3928095) had 12 jobs passed, none failed, 10 still running; 45513434445
(2f7d02b6, the first run with 9f3ec29b) had not started.

## The trace (temporary loss, ACK and state prints; seed 3 with the single draw)

- The handover hit at 19,082.9 ms. The outage took pn 15504-15525, which were covered and kept from CUBIC. The
  server's ACKs sent during the outage were lost too. Its first ACK after the outage arrived at 19,220.8 ms
  (+138 ms) and acknowledged 35 packets at once. That released a train at once (pn 15528-15537), then paced packets.
- That train left in the last 12 ms of the case's 150 ms delay spike. The spike outlasts the schedule's 100 ms
  after-guard, so its packets were not covered (`ho=0`). The fixture stamps each datagram's delay as it is sent, so
  packets sent once the spike ended overtook them by 30 ms. pn 15550 (sent at +150.3 ms) was acknowledged first.
- The 22 overtaken packets were declared lost by packet number: a reduction to 58,196 bytes. They arrived 4-29 ms
  later and were undone by RFC 9002's spurious-loss path. The engine answered a real reordering correctly.
- Blind's first ACK after the outage came at +164 ms, after the spike had ended, so nothing it sent was reordered.
- Aware's other two reductions in the 2 s windows (+438 ms, +1,916 ms) were ordinary drop-tail overflows.
  CUBIC overflows about every 2.7 s here.

## The case is the defect (sweeps over the client's seed; seeds n and n+4 run identically)

- HEAD's engine already fails seeds 1 and 5 the same way: aware has 2 reductions with 1 undone, against blind's 2,
  and is 31 ms slower. The single draw fails 4 of 4 distinct seeds.
- CUBIC's elapsed time minus blind's, over 21 s: -42, +31, -14 and -36 ms at HEAD; +5, +2, -29 and +10 ms with the
  single draw. On a queue of one BDP, CUBIC's 0.7 cut still keeps the pipe full (1.4 BDP is above 1 BDP). So
  without the schedule, a handover costs CUBIC only its outage, and a speed sign on this link is chance.
  BBR's margin is real: 43-345 ms over the same 8 runs.
- Candidate A: a 2 s period, reductions net of undone ones in a 500 ms window, 4 seeds × 6 first-handover shifts ×
  both builds. Blind and off are cut at every measured handover but at most one (CUBIC 9-10 of 9-10). Aware CUBIC
  is cut 0-3 times, and BBR always at least 4 fewer times than blind. BBR's elapsed gap is at least 204 ms.
  - Still red: CUBIC's speed (-150 to +160 ms), BBR's bytes (3 of 48 runs), and `off.handover_losses == 0`.
    A 7 s shift folds to 1 s and covers BBR's own probe losses.
  - Also red: 13 `min_mtu` checks in the 48 case runs, blind's and aware's alike. The path falls to 1200 bytes.
- Candidate B: A plus a 10 ms queue. CUBIC overflows all the time (aware is cut as often as blind), so it fails.

## Next

1. The `min_mtu` reds: outages every 2 s pass for a black hole. The likely route is three suspect detections
   before any large packet is acknowledged, since only the small PTO probes are acknowledged after an outage. Trace
   it. If confirmed, count a detection as suspect only when the lost large packets were sent after an acknowledged
   smaller one. This is the engine fix.
2. Then the hardened case on candidate A. Its claims:
   - CUBIC and BBR: blind and off are cut at every handover, and aware rarely.
   - BBR: faster.
   - CUBIC: no speed claim on one BDP, and the 15 s case keeps its MTU and kept-loss checks.
   Then land the single draw. The patches, sweep script and raw tables are in the WSL home's `qb-exp/patches` and
   `qb-exp/leo-sweeps`.
