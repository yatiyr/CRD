# 2026-11-25: NET-PATH, the host turn's regressions measured, and a sub-millisecond host's turn held by test

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Operator note on 1f7cb1dd and 4b858fd9 (never regress), items 1-4.
User decision on record (2026-11-01): "I WANT NET DONE PROPERLY AND GOLD STANDARD AND WITHOUT ANY DEFERS!" Ran on this
PC (WSL2 linux-gcc-release grid and board, -debug, linux-clang-fuzz). **CI first:** nothing to fix; no run containing
1f7cb1dd has finished (45648028909: 11 jobs passed, none failed). Its win-tidy will flag 1f7cb1dd's local `kTrain`
(identifier naming), renamed `train` here.

## Landed, then reverted

- 7aa3817a restarted a flight the pacer held and its acknowledgements emptied on a host measured finer than a
  millisecond (the loopback board's sender waited 162 against 126 us a MiB at 64 KiB windows without it). Board after,
  nine interleaved rounds against the same source with `held()` a no-op: 64 KiB wall 808 against 796 MiB/s (one run),
  1 MiB 1,233 against 1,241 and 1,239 against 1,215 (two runs); before: 793 against 812 and 1,187 against 1,204.
- The grid's 2 and 20 ms paths never empty a flight inside a sub-millisecond turn, so its 480 rows did not see it. At a
  500 us turn over 50 us one way it held NewReno and CUBIC to the initial window a turn: 256 KiB 99.97% -> 19.21%,
  64 KiB 88.66 / 91.96% -> 19.21%. A turn-against-smoothed-RTT gate lost 6-12 points on the 64 KiB rows and up to
  2.3 on the 16.7 ms ones. The engine is back at 73b04146's rule, and the test now holds it: the emptied, pacer-held
  flight is no restart at turns 0, 20 us, 250 us, 1 ms and 16.7 ms, and a 500 us host over a 100 us path fills 95% of
  1 Gbit/s in 5 s. Teeth: the millisecond gate fails 10 assertions. Its first draft's threshold `125000000U * 5U * 95U`
  wrapped in 32 bits and passed the gate; it is u64 now. Debug `[congestion]` passes (28 cases); tidy clean.
- So item 4 stays traded, not met.

## Measured, not yet met

- **Item 1.** BBR, exact timers, 1 Gbit/s, 32 KiB, 2 ms, seeds 0-15: 93.70-93.72% / 1,377 drops / 168 reductions on
  every seed; with BBR's quantum back at 64 KiB, 92.75% / 812 / 91 on every seed. No spread (no random loss), so the
  rise is past it and BBR's burst bound is owed.
- **Item 2.** NewReno and CUBIC met (92.71 / 99.37% at 2 ms, 87.95 / 90.89% at 20 ms, against 52.7 / 51.4%). BBR's
  rows: 51.28 / 50.55% against 52.2 / 51.5%, open on the same BBR cap.
- **Item 3** (pre-4b858fd9 -> HEAD drops): 1 ms, 64 KiB 0 -> 63 and 59; 16.7 ms, 12 KB 0/6 -> 16/15, 32 KiB 0/6 ->
  19/18, 64 KiB 0 -> 60/59. Floor: a probe loses at least one datagram on a queue it overflows, and the hold (8 to
  1,024 rounds) re-probes about 9 times in 10 s, so 0 means no learning. Three step rules got those rows to 10/6 and
  7/6 (geometric everywhere with a reset at the train, geometric past a train, one datagram then doubling) and each gave
  back 2-21 points on the 20 ms deep rows; none landed. HEAD's 20 ms, 1 ms-turn rows scatter with the queue (128 to
  512 KiB: 85.2, 77.0, 88.0, 74.3, 91.4%). HEAD's trace of the 512 KiB one: a 129,600-byte burst a turn against the
  125,000 the link drains, 161 of its 164 drops between 500 and 1,000 ms while slow start held a 17.8 MB window over a
  3.0 MB flight; after that the window and the flight agree. Growth gated on half the window collapsed the 16.7 ms rows.

## Next

1. One slow-start item: the window held to the flight a turn's burst leaves, and that burst held to what the path
   drains a turn. Then the item 3 step rule, judged on the queue sweep, and item 4's cost found without a restart.
2. BBR's burst bounded by a loss-taught train cap, with the window controllers' train logic its single owner.
