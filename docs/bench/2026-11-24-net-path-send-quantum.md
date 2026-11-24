# 2026-11-24 — NET-PATH: the QUIC board's datagram counts and the window controllers' send quantum

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Session: [send quantum](../sessions/2026-11-24-net-path-send-quantum.md).
Method, machine and arms as the [first board](2026-11-23-net-path-quic-board.md): `quic_board.py --rounds 2`, bytes
at 64 MiB and 1 GiB, `linux-gcc-release`, WSL2 on the i9-14900K. "Before" is 34e72e21. "Pacing off" is that tree with
the board's `CongestionPolicy::pacing` false, a control that was not committed. "Plain quantum" is the send quantum on
every flight, also not committed (it lost 25 times more after idle in the simulator, below). "After" is what landed:
the quantum only on a held flight. Counts are per MiB from the same slope: data datagrams per send message (a
segmented train is one), the ACK datagrams the server sent, and data datagrams per ACK.

## Results

| crd-net (offload) | window | MiB per CPU-second | median | MiB per wall second | datagrams per train | ACKs per MiB | data per ACK |
|---|---|---|---|---|---|---|---|
| before | 64 KiB | 355 | 347 | 596 | 9.5 | 69 | 10.8 |
| pacing off | 64 KiB | 441 | 424 | 452 | 23.3 | 17 | 45.5 |
| plain quantum | 64 KiB | 451 | 450 | 481 | 12.0 | 17 | 44.0 |
| after | 64 KiB | 403 | 403 | 473 | 12.1 | 30 | 25.1 |
| before | 1 MiB | 398 | 381 | 716 | 10.0 | 54 | 13.6 |
| pacing off | 1 MiB | 563 | 543 | 912 | 32.0 | 12 | 63.5 |
| plain quantum | 1 MiB | 590 | 572 | 951 | 22.3 | 16 | 45.2 |
| after | 1 MiB | 534 | 533 | 848 | 21.9 | 16 | 46.2 |

msquic in the same runs: 834 to 955 MiB per CPU-second, 1,581 to 1,775 per wall second, 39.7 to 40.7 datagrams per
train, 8 to 10 ACKs per MiB and 76.9 to 87.8 data datagrams per ACK. Its lead with the landed rule: 1.64 times at
1 MiB windows (2.09 before) and 2.37 at 64 KiB (2.44).

## Reading

- Before, crd-net handed the host 79 data trains and 54-69 ACKs per MiB, each a buffer its stack carries; msquic 18
  and 8-10. The trains were the pacer's burst, the initial window of 10 datagrams: with pacing off they grew to 23-32.
- Both stacks ACK about once per two trains (crd-net 46 / 22, msquic 87 / 40), so the train length accounts for the
  ACK gap. msquic does not pace below a 1 ms RTT (`QUIC_MIN_PACING_RTT`), so its trains are its send batches.
- At 64 KiB the landed rule gains less than plain quantum (ACKs 30 per MiB against 17), as expected where the stream
  window drains between flow-control updates and flights restart from nothing in flight (not traced). crd-net's
  wall rate at 64 KiB fell from 596 to 473 MiB per second while its CPU rate rose; the cause is open.
- Idle restarts, simulated (`bottleneck_fixture.hpp`, 1 Gbit/s, 2 ms RTT, 32 KiB queue, twenty 1 MiB writes a second
  apart): plain quantum 114 to 157 drops and 17 to 20 reductions, the landed rule and the initial window 1 to 5 and 1.
  Steady bulk with even the plain quantum, over 12-256 KiB queues, 2-40 ms RTTs and 0-1% loss, matched the initial
  window within two reductions, but only because the fixture's sender wakes exactly at each release time, so no
  quantum ever left mid-flight.
- Coarse turns (a temporary fixture change, not committed: every event rounded up to 250 us or 1 ms, CUBIC, version 7
  against 8, 10 s of bulk). Version 7 cannot fill a fast link, at one initial window a turn: 38% of 1 Gbit/s at
  250 us turns, 9% at 1 ms. Version 8 fills 73-93% at 250 us over 32-64 KiB queues (2-64 drops), 17-36% at 1 ms.
  It loses where the queue is under a burst: a 12 KB queue at 1 Gbit/s falls to 6-13% (v7 38%), and at 100 Mbit/s
  with 1 ms turns to 65-69% with 216 drops where v7 fills 96% with none. The quantum needs a bound the path's losses
  teach; until then shallow-queue paths under coarse turns are worse than before.
