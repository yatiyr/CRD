# 2026-11-24 — NET-PATH: a burst at most half a flight that ran out of data, the 64 KiB wall rate back

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Session: [flight burst](../sessions/2026-11-24-net-path-flight-burst.md).
`quic_board.py --workload bytes --rounds 5` (64 MiB and 1 GiB), `linux-gcc-release`, WSL2 on the i9-14900K. Every arm
of a table ran interleaved in the same rounds: "base" is 4fb61ff3 (34e72e21's engine), "before" is 5d441aa3, "after"
is this batch. All builds carry this batch's board program (its wait counts).

## Results

| window | arm | MiB per CPU-second | median | MiB per wall second | datagrams per train | ACKs per MiB | client waits per MiB |
|---|---|---|---|---|---|---|---|
| 64 KiB | base | 404 | 398 | 684 | 9.7 | 71 | 8 |
| 64 KiB | before | 422 | 418 | 497 | 12.1 | 30 | 30 |
| 64 KiB | after (vs base) | 434 | 431 | 688 | 14.7 | 48 | 14 |
| 64 KiB | base (same run) | 409 | 403 | 683 | 9.7 | 71 | 8 |
| 1 MiB | after (vs before) | 594 | 589 | 951 | 22.2 | 16 | 1 |
| 1 MiB | before (same run) | 594 | 592 | 962 | 22.3 | 17 | 1 |
| 1 MiB | after (vs base) | 597 | 587 | 967 | 22.3 | 17 | 1 |
| 1 MiB | base (same run) | 431 | 427 | 758 | 10.0 | 62 | 1 |

msquic in the 64 KiB run: 990 MiB per CPU-second, 1,889 per wall second, 41.1 datagrams per train, 10 ACKs per MiB.

## Diagnosis (64 KiB, the rounds before the fix)

- CPU per MiB was equal (before 404, base 394) while the wall rate fell, so both ends were idle: before kept 1.17 cores
  busy, base 1.67. No wait ended on its timeout: every wait was for a datagram.
- Mixed roles: a before client against a base server 500 per wall second, a base client against a before server 686.
  The sender decides.
- A 1 MiB send buffer under a 64 KiB stream window, and a 64 KiB send buffer under a 1 MiB window, each give the same
  fall (497 and 588 against base's 684 and 717): any non-window bound of a flight near 64 KiB does it.
- Before's sender sent the credit an acknowledgement freed in two trains at once; the receiver read both in one turn
  and acknowledged once (25 datagrams per ACK, 30 client waits per MiB), so each end waited for the other.
- Bound = the dry flight over 2, 4 and 6: 687, 655 and 659 per wall second and 433, 398 and 394 per CPU-second; at
  1 MiB all three equal before (928-933, 584-589). Half, Linux's two segmentation bursts in flight, is kept.
