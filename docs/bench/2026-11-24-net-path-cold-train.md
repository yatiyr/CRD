# 2026-11-24 — NET-PATH: the train cap grows only from trains a held host sent

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Session: [cold train](../sessions/2026-11-24-net-path-cold-train.md).

## Method

The [recovery-burst board](2026-11-24-net-path-recovery-burst.md)'s grid (96 rows, 10 s, cells link / drops /
reductions) and the [restart board](2026-11-24-net-path-restart-pacing.md) (16 rows; drops / reductions / done in ms
over twenty restarts, and the window the 8 MiB's slow start left), `linux-gcc-release` on WSL2 (i9-14900K). "before"
is version 8 at 912e3e0d, "after" this batch. The v7 and before columns reproduce the recovery-burst board's cells.

## The grid: the 10 rows that changed (the other 86, all 64 coarse-turn rows among them, are identical)

| controller | link | host turn | queue | RTT | v7 | before | after |
|---|---|---|---|---|---|---|---|
| CUBIC | 1 Gbit/s | exact | 12 KB | 2 ms | 88.3% / 212 / 41 | 87.4% / 46 / 39 | 88.3% / 212 / 41 |
| CUBIC | 1 Gbit/s | exact | 32 KiB | 2 ms | 93.1% / 359 / 38 | 93.0% / 284 / 38 | 93.1% / 359 / 38 |
| CUBIC | 1 Gbit/s | exact | 64 KiB | 2 ms | 98.0% / 402 / 33 | 98.0% / 348 / 32 | 98.0% / 402 / 33 |
| CUBIC | 1 Gbit/s | exact | 256 KiB | 20 ms | 85.3% / 3584 / 3 | 85.3% / 3587 / 3 | 85.3% / 3584 / 3 |
| CUBIC | 100 Mbit/s | exact | 64 KiB | 20 ms | 97.1% / 374 / 5 | 97.1% / 372 / 5 | 97.1% / 374 / 5 |
| NewReno | 1 Gbit/s | exact | 12 KB | 2 ms | 77.7% / 156 / 45 | 77.3% / 51 / 44 | 77.7% / 156 / 45 |
| NewReno | 1 Gbit/s | exact | 32 KiB | 2 ms | 82.8% / 261 / 42 | 83.0% / 212 / 42 | 82.8% / 261 / 42 |
| NewReno | 1 Gbit/s | exact | 64 KiB | 2 ms | 89.1% / 302 / 37 | 89.2% / 269 / 37 | 89.1% / 302 / 37 |
| NewReno | 1 Gbit/s | exact | 256 KiB | 20 ms | 65.0% / 2439 / 2 | 65.1% / 2441 / 2 | 65.0% / 2439 / 2 |
| NewReno | 100 Mbit/s | exact | 64 KiB | 20 ms | 86.9% / 270 / 5 | 86.9% / 269 / 5 | 86.9% / 270 / 5 |

At two decimals: CUBIC 12 KB 87.39% to 88.27%, NewReno 12 KB 77.29% to 77.73%; NewReno 32 KiB 83.03% to 82.84%,
64 KiB 89.20% to 89.07%, 256 KiB 20 ms 65.09% to 65.05% (each now version 7's exactly).

## Restarts (16 rows)

Every row's slow start now leaves version 7's window (NewReno 1 Gbit/s 2 ms 32 KiB 184,990 bytes against 277,781;
CUBIC 1 Gbit/s 2 ms 12 KB 221,311 against 141,876). Drops and reductions are equal on every row; done is equal or
sooner: NewReno 1 Gbit/s 2 ms 32 KiB 12.4 to 11.5 ms, CUBIC 32 KiB 11.5 to 11.1, CUBIC 12 KB 47.7 to 45.2, the rest
within 0.1 ms.

## Reading

- The cause (traced, CUBIC 1 Gbit/s, 12 KB, 2 ms): the first flight's ten datagrams left in one train, which reached
  the proven 12000 bytes, so the cap doubled to 24000 at the first round's end. On exact timers no later train is
  longer than two datagrams, so no probe ever went, and the unproven 24000 bytes stayed the pacer's credit: ACK-clocked
  runs at twice the link's rate in slow start filled the queue at a 61,200-byte window (8 drops by 114,000).
- With both rules version 8 is version 7 on all 32 exact-timer rows. The three NewReno rows that fall (by 0.04 to
  0.19 point) had been above version 7 only through that unproven credit.
- A rule that skipped every flight starting from nothing in flight delayed a coarse host's first growth (its turns
  acknowledge the whole flight before they send) and cost the shallow coarse rows 7 probe drops (12 to 19): rejected
  for the rule that skips only a flight whose start no acknowledgement emptied.
