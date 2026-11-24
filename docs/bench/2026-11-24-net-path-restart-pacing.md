# 2026-11-24 — NET-PATH: a restarted flight's first round paced at one window per smoothed RTT

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Session: [restart pacing](../sessions/2026-11-24-net-path-restart-pacing.md).

## Method

`bottleneck_fixture.hpp`, one flow, HyStart++ on, the policy's version from the first byte (no live swap): 8 MiB,
then twenty 1 MiB writes a second apart. Drops and reductions count the twenty restarts; "done" is the mean time
from a write to its last byte acknowledged or lost. "v7" is a version 7 policy, "before" version 8 at 7889ca65,
"after" this batch (one window per smoothed RTT until a restarted flight's first acknowledgement). `linux-gcc-release`
on WSL2 (i9-14900K). The window is the one the 8 MiB left (each version's own slow start).

## Results (drops / reductions / done in ms)

| controller | link | RTT | queue | window | v7 | before | after |
|---|---|---|---|---|---|---|---|
| NewReno | 1 Gbit/s | 2 ms | 32 KiB | 277,781 (v7 184,990) | 5 / 1 / 13.8 | 32 / 2 / 14.4 | 1 / 1 / 12.4 |
| NewReno | 1 Gbit/s | 2 ms | 12 KB | 152,117 | 20 / 20 / 50.1 | 20 / 20 / 50.2 | 20 / 20 / 50.3 |
| NewReno | 1 Gbit/s | 20 ms | 32 KiB | 2,176,200 | 44 / 1 / 34.8 | 44 / 1 / 34.8 | 0 / 0 / 29.5 |
| NewReno | 1 Gbit/s | 20 ms | 256 KiB | 2,716,800 | 0 / 0 / 28.4 | 0 / 0 / 28.4 | 0 / 0 / 28.4 |
| NewReno | 1 Gbit/s | 2 ms | 256 KiB | 279,497 | 0 / 0 / 10.4 | 0 / 0 / 10.4 | 0 / 0 / 10.4 |
| NewReno | 100 Mbit/s | 2 ms | 12 KB | 30,500 | 104 / 50 / 96.3 | 104 / 50 / 96.3 | 71 / 50 / 96.4 |
| NewReno | 100 Mbit/s | 20 ms | 32 KiB | 184,990 | 5 / 1 / 137.8 | 5 / 1 / 137.8 | 1 / 1 / 114.8 |
| NewReno | 100 Mbit/s | 20 ms | 64 KiB | 201,057 | 1 / 1 / 122.6 | 1 / 1 / 122.7 | 0 / 0 / 108.0 |
| CUBIC | 1 Gbit/s | 2 ms | 32 KiB | 190,770 (v7 203,201) | 1 / 1 / 12.9 | 1 / 1 / 12.9 | 0 / 0 / 11.5 |
| CUBIC | 1 Gbit/s | 2 ms | 12 KB | 141,876 (v7 221,311) | 36 / 20 / 45.2 | 20 / 20 / 47.7 | 20 / 20 / 47.7 |
| CUBIC | 1 Gbit/s | 20 ms | 32 KiB | 3,046,680 | 300 / 2 / 30.6 | 300 / 2 / 30.6 | 130 / 1 / 29.6 |
| CUBIC | 1 Gbit/s | 20 ms | 256 KiB | 3,803,520 | 155 / 1 / 28.3 | 155 / 1 / 28.3 | 29 / 1 / 28.4 |
| CUBIC | 1 Gbit/s | 2 ms | 256 KiB | 508,361 | 1 / 1 / 10.4 | 1 / 1 / 10.4 | 1 / 1 / 10.4 |
| CUBIC | 100 Mbit/s | 2 ms | 12 KB | 27,330 | 119 / 40 / 86.3 | 119 / 40 / 86.3 | 60 / 40 / 86.4 |
| CUBIC | 100 Mbit/s | 20 ms | 32 KiB | 203,355 | 1 / 1 / 129.0 | 1 / 1 / 129.0 | 0 / 0 / 110.8 |
| CUBIC | 100 Mbit/s | 20 ms | 64 KiB | 232,668 | 1 / 1 / 110.6 | 1 / 1 / 109.7 | 0 / 0 / 104.9 |

## Reading

- The operator's row (d), NewReno at 1 Gbit/s, 2 ms, 32 KiB: version 8's own slow start leaves 277,781 bytes (one
  reduction; version 7's two leave 184,990). A version 7 restart from that window, by a live swap, drops the same 32
  with 2 reductions: the window, not the quantum, overflowed the queue, at the pacing gain's 1.25 windows per RTT.
- After: no row drops more or completes later than before beyond 0.2 ms; the 20 ms rows deliver each MiB 1 to 23 ms
  sooner. The 12 KB rows lose one datagram of each restart's initial-window burst (ten datagrams into a ten-datagram
  queue), as version 7 does. CUBIC at 20 ms and 32 KiB loses all 130 in the first restart's one recovery period
  (traced per cycle), from a window the 8 MiB's slow start left above the path; the later nineteen lose none.
- QUIC board (`quic_board.py --rounds 11`, interleaved with 7889ca65's build as `--baseline`): 64 KiB windows 432
  MiB per CPU-second (median 427) and 680 per wall second against 436 (431) and 684; handshakes 3,848 per
  CPU-second (median 3,684) against 3,915 (3,661). At 1 MiB windows (5 rounds) 597 (582) and 944 against 577 (576)
  and 930. Within the board's round-to-round spread on every row.
