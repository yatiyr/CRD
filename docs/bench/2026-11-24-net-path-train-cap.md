# 2026-11-24 — NET-PATH: the train cap under coarse host turns, before and after

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Session: [train cap](../sessions/2026-11-24-net-path-train-cap.md).

## Method

`bottleneck_fixture.hpp` with its new host turn (`Flow::set_turn`: every ACK and send waits for the next multiple
of the turn, ACKs first), one flow of 1200-byte datagrams, HyStart++ on, bulk for 10 s, `linux-gcc-release` on WSL2
(i9-14900K). "Link" is acknowledged bytes over the link's capacity. v7 is a version 7 policy (the initial window's
burst, 34e72e21's rule); "65610837" is the send quantum without a cap (that commit's `burst`, rebuilt for the run
and not committed); "this batch" is 6184d737's rule (3c859371 changes only Prague, version 7 seeds and the undo, which no row reaches). Exact timers (no turn) for two rows as the reference. 100 Mbit/s at
250 us turns is the same in all three (the quantum never exceeds the initial window there) and is left out.

## Results

| controller | link | host turn | queue | RTT | v7 (34e72e21's burst): link / drops / reductions | 65610837 | this batch |
|---|---|---|---|---|---|---|---|
| CUBIC | 1 Gbit/s | exact | 12 KB | 2 ms | 88.3% / 212 / 41 | 87.4% / 51 / 39 | 87.4% / 46 / 39 |
| CUBIC | 1 Gbit/s | 250 us | 12 KB | 2 ms | 38.4% / 0 / 0 | 6.6% / 574 / 373 | 38.4% / 17 / 0 |
| CUBIC | 1 Gbit/s | 250 us | 12 KB | 20 ms | 37.9% / 0 / 0 | 15.2% / 3 / 3 | 37.9% / 6 / 0 |
| CUBIC | 1 Gbit/s | 250 us | 32 KiB | 2 ms | 38.4% / 0 / 0 | 84.5% / 93 / 33 | 99.7% / 123 / 1 |
| CUBIC | 1 Gbit/s | 250 us | 32 KiB | 20 ms | 37.9% / 0 / 0 | 62.2% / 1625 / 3 | 97.6% / 487 / 1 |
| CUBIC | 1 Gbit/s | 250 us | 64 KiB | 2 ms | 38.4% / 0 / 0 | 93.1% / 207 / 30 | 92.9% / 455 / 38 |
| CUBIC | 1 Gbit/s | 250 us | 64 KiB | 20 ms | 37.9% / 0 / 0 | 68.8% / 2233 / 3 | 96.7% / 1553 / 0 |
| CUBIC | 1 Gbit/s | 250 us | 256 KiB | 2 ms | 38.4% / 0 / 0 | 99.9% / 640 / 14 | 99.7% / 532 / 16 |
| CUBIC | 1 Gbit/s | 250 us | 256 KiB | 20 ms | 37.9% / 0 / 0 | 83.8% / 3393 / 3 | 93.0% / 2167 / 11 |
| CUBIC | 1 Gbit/s | 1 ms | 12 KB | 2 ms | 9.6% / 0 / 0 | 7.0% / 247 / 218 | 9.6% / 12 / 0 |
| CUBIC | 1 Gbit/s | 1 ms | 12 KB | 20 ms | 9.5% / 0 / 0 | 6.5% / 169 / 7 | 9.5% / 6 / 0 |
| CUBIC | 1 Gbit/s | 1 ms | 32 KiB | 2 ms | 9.6% / 0 / 0 | 18.9% / 206 / 88 | 25.9% / 22 / 0 |
| CUBIC | 1 Gbit/s | 1 ms | 32 KiB | 20 ms | 9.5% / 0 / 0 | 17.9% / 445 / 4 | 25.4% / 18 / 0 |
| CUBIC | 1 Gbit/s | 1 ms | 64 KiB | 2 ms | 9.6% / 0 / 0 | 36.5% / 69 / 46 | 51.7% / 10 / 0 |
| CUBIC | 1 Gbit/s | 1 ms | 64 KiB | 20 ms | 9.5% / 0 / 0 | 34.9% / 47 / 3 | 50.5% / 6 / 0 |
| CUBIC | 100 Mbit/s | exact | 12 KB | 20 ms | 88.6% / 177 / 6 | 83.4% / 180 / 6 | 88.6% / 177 / 6 |
| CUBIC | 100 Mbit/s | 1 ms | 12 KB | 2 ms | 95.9% / 0 / 0 | 69.5% / 247 / 218 | 95.9% / 12 / 0 |
| CUBIC | 100 Mbit/s | 1 ms | 12 KB | 20 ms | 95.1% / 0 / 0 | 64.7% / 169 / 7 | 95.1% / 6 / 0 |
| CUBIC | 100 Mbit/s | 1 ms | 32 KiB | 2 ms | 95.9% / 0 / 0 | 99.9% / 149 / 87 | 99.9% / 162 / 89 |
| CUBIC | 100 Mbit/s | 1 ms | 32 KiB | 20 ms | 95.1% / 0 / 0 | 88.4% / 331 / 6 | 92.3% / 142 / 11 |
| CUBIC | 100 Mbit/s | 1 ms | 64 KiB | 2 ms | 95.9% / 0 / 0 | 99.9% / 150 / 38 | 99.9% / 214 / 42 |
| CUBIC | 100 Mbit/s | 1 ms | 64 KiB | 20 ms | 95.1% / 0 / 0 | 96.3% / 404 / 5 | 96.5% / 611 / 11 |
| NewReno | 1 Gbit/s | exact | 12 KB | 2 ms | 77.7% / 156 / 45 | 77.3% / 56 / 44 | 77.3% / 51 / 44 |
| NewReno | 1 Gbit/s | 250 us | 12 KB | 2 ms | 38.4% / 0 / 0 | 6.5% / 654 / 348 | 38.4% / 17 / 0 |
| NewReno | 1 Gbit/s | 250 us | 12 KB | 20 ms | 37.9% / 0 / 0 | 13.2% / 5 / 4 | 37.9% / 6 / 0 |
| NewReno | 1 Gbit/s | 250 us | 32 KiB | 2 ms | 38.4% / 0 / 0 | 74.6% / 66 / 37 | 99.7% / 123 / 1 |
| NewReno | 1 Gbit/s | 250 us | 32 KiB | 20 ms | 37.9% / 0 / 0 | 50.1% / 978 / 2 | 97.6% / 487 / 1 |
| NewReno | 1 Gbit/s | 250 us | 64 KiB | 2 ms | 38.4% / 0 / 0 | 83.0% / 164 / 34 | 83.0% / 374 / 37 |
| NewReno | 1 Gbit/s | 250 us | 64 KiB | 20 ms | 37.9% / 0 / 0 | 54.4% / 1440 / 2 | 96.7% / 1553 / 0 |
| NewReno | 1 Gbit/s | 250 us | 256 KiB | 2 ms | 38.4% / 0 / 0 | 99.5% / 479 / 16 | 99.3% / 456 / 17 |
| NewReno | 1 Gbit/s | 250 us | 256 KiB | 20 ms | 37.9% / 0 / 0 | 63.9% / 2272 / 2 | 75.3% / 1742 / 6 |
| NewReno | 1 Gbit/s | 1 ms | 12 KB | 2 ms | 9.6% / 0 / 0 | 6.3% / 338 / 212 | 9.6% / 12 / 0 |
| NewReno | 1 Gbit/s | 1 ms | 12 KB | 20 ms | 9.5% / 0 / 0 | 5.7% / 112 / 7 | 9.5% / 6 / 0 |
| NewReno | 1 Gbit/s | 1 ms | 32 KiB | 2 ms | 9.6% / 0 / 0 | 15.7% / 235 / 95 | 25.9% / 22 / 0 |
| NewReno | 1 Gbit/s | 1 ms | 32 KiB | 20 ms | 9.5% / 0 / 0 | 15.4% / 279 / 4 | 25.4% / 18 / 0 |
| NewReno | 1 Gbit/s | 1 ms | 64 KiB | 2 ms | 9.6% / 0 / 0 | 31.1% / 104 / 51 | 51.7% / 10 / 0 |
| NewReno | 1 Gbit/s | 1 ms | 64 KiB | 20 ms | 9.5% / 0 / 0 | 30.9% / 25 / 2 | 50.5% / 6 / 0 |
| NewReno | 100 Mbit/s | 1 ms | 12 KB | 2 ms | 95.9% / 0 / 0 | 63.4% / 338 / 212 | 95.9% / 12 / 0 |
| NewReno | 100 Mbit/s | 1 ms | 12 KB | 20 ms | 95.1% / 0 / 0 | 56.5% / 112 / 7 | 95.1% / 6 / 0 |
| NewReno | 100 Mbit/s | 1 ms | 32 KiB | 2 ms | 95.9% / 0 / 0 | 96.6% / 199 / 104 | 96.4% / 203 / 104 |
| NewReno | 100 Mbit/s | 1 ms | 32 KiB | 20 ms | 95.1% / 0 / 0 | 78.1% / 233 / 5 | 81.1% / 88 / 8 |
| NewReno | 100 Mbit/s | 1 ms | 64 KiB | 2 ms | 95.9% / 0 / 0 | 99.9% / 149 / 47 | 99.4% / 222 / 48 |
| NewReno | 100 Mbit/s | 1 ms | 64 KiB | 20 ms | 95.1% / 0 / 0 | 84.7% / 294 / 5 | 89.9% / 449 / 7 |

## Reading

- The operator's rows: 12 KB at 1 Gbit/s back to v7's 38.4% and 37.9% (and 9.6% at 1 ms turns), 100 Mbit/s with
  1 ms turns over 12 KB back to 95.9% and 95.1%, each with 6 to 17 probe drops and no reduction.
- Deeper queues keep the quantum's gain and more: 75-99.7% of 1 Gbit/s at 250 us where v7 fills 38%.
- Not met: 100 Mbit/s, 1 ms turns, 32 KiB at 20 ms (92.3% and 81.1% against 95.1%) and NewReno over 64 KiB at 20 ms
  (89.9%). v7's 12000 bytes a millisecond is 96 Mbit/s, under that link, so it never fills the queue; with exact
  timers the same controllers get 91.0% and 81.6% there, and on an 88 Mbit/s link v7 gets 80.3% (NewReno) too.
- Exact timers: CUBIC at 100 Mbit/s over 12 KB and 20 ms was 83.4% at 65610837 (an extra reduction leaving slow
  start) and is v7's 88.6% again, by the recovery-period rule; 1 Gbit/s over 12 KB at 2 ms stays at 87.4% against
  v7's 88.3% (65610837 the same).

## The QUIC board (crd-net arm only, `quic_board.py --rounds 3`, not interleaved with a baseline)

| window | MiB per CPU-second | median | MiB per wall second | datagrams per train | ACKs per MiB |
|---|---|---|---|---|---|
| 1 MiB | 587 | 585 | 940 | 22.1 | 16 |
| 64 KiB | 416 | 415 | 493 | 12.1 | 31 |

The per-CPU gain of 65610837 holds (534 then, trains 21.9, ACKs 16). The wall rate at 64 KiB (596 at 34e72e21, 473
at 65610837) is still below 34e72e21's: open.
