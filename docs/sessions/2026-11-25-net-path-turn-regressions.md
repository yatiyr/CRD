# 2026-11-25: NET-PATH, the host turn's regressions measured, and a fine host's emptied flight restarts again

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Operator note on 1f7cb1dd and 4b858fd9 (never regress), items 1-4.
User decision on record (2026-11-01): "I WANT NET DONE PROPERLY AND GOLD STANDARD AND WITHOUT ANY DEFERS!" Ran on this
PC (WSL2 linux-gcc-release grid and board, -debug, linux-clang-fuzz). **CI first:** nothing to fix; no run containing
1f7cb1dd has finished (45648028909 has 3 jobs passed, none failed).

## Landed

- **Item 4, the board's wall rate.** The restart rule (a flight the pacer held, emptied by its acknowledgements, is no
  restart) applied on any host. On the loopback board's acknowledgement-woken sender it made the client wait 162 us a
  MiB against the control's 126 at 64 KiB windows. `HostTurn::fine()`: a host measured finer than a millisecond
  restarts as before the rule; an unmeasured or coarse one keeps it.

## Evidence

- Board (1 GiB, nine interleaved rounds, best (median) MiB per CPU-second, wall; control = the same source with
  `held()` a no-op). Before: 1 MiB 731 (724) against 733 (727), wall 1,187 against 1,204; 64 KiB 498 (493) against 501
  (494), wall 793 against 812. After: 1 MiB 741 (718) against 750 (721), wall 1,233 against 1,241, and a second run
  745 (725) against 734 (722), wall 1,239 against 1,215; 64 KiB 502 (494) against 499 (496), wall 808 against 796,
  client waits 142 against 151 us a MiB.
- Simulator grid (240 rows as before, and 240 more at 80, 96, 128, 192 and 384 KiB queues): identical to HEAD.
- Test: a seeded controller at turns 0, 20 us, 250 us, 1 ms and 16.7 ms restarts its emptied, pacer-held flight (one
  window per RTT, the initial window's burst) only at the two fine turns. Teeth: gate removed, 8 assertions fail.
  Debug crd-net-tests 1,215 cases pass (8 RIO skipped); guards 19 pass. Congestion fuzzer: replay 14 clean, 582,885
  runs (240 s) clean. LLVM-22 tidy clean; 1f7cb1dd's local `kTrain` renamed `train` (identifier naming).

## Measured, not yet met

- **Item 1.** BBR, exact timers, 1 Gbit/s, 32 KiB, 2 ms over seeds 0-15: 93.70-93.72% / 1,377 drops / 168 reductions
  on every seed; with BBR's quantum back at 64 KiB, 92.75% / 812 / 91 on every seed. The spread is zero (no random
  loss, so the seed moves nothing), the rise is past it: BBR's burst bound is owed.
- **Item 2.** NewReno and CUBIC met (92.71/99.37% at 2 ms, 87.95/90.89% at 20 ms against 52.7/51.4%). BBR's two rows
  51.28/50.55% against 52.2/51.5%: open, the same BBR cap.
- **Item 3** (pre-4b858fd9 -> HEAD drops): 1 ms, 64 KiB 0 -> 63 and 59; 16.7 ms, 12 KB 0/6 -> 16/15, 32 KiB 0/6 ->
  19/18, 64 KiB 0 -> 60/59. Floor: a growth's probe loses at least one datagram on a queue it overflows, and the hold
  (8 to 1,024 rounds) re-probes about 9 times in 10 s, so 0 only without learning. Four step rules reached 10/6 and
  7/6 on those rows (geometric from one datagram everywhere, past a train only, one datagram then doubling) and gave
  back 2-21 points on the 20 ms deep rows: none is Pareto, none landed. HEAD's 20 ms, 1 ms-turn rows are themselves
  chaotic in the queue (128 KiB to 512 KiB: 85.2, 77.0, 88.0, 74.3, 91.4%): the cap passes the link's bytes a turn
  (129,600 to 140,400 against 125,000) and no round judges the standing queue. In slow start the window inflates past
  the flight (17.8 MB against 3.0 MB at 500 ms; 603 MB in the 64 KiB row); counting the pacer's hold as limited only
  at half the window collapsed the 16.7 ms rows to the initial window.

## Next

1. A ceiling on a turn's burst at what the path drains a turn (a judged round's standing queue), then a step rule
   for item 3 judged on the queue sweep; the slow-start window held to the flight.
2. BBR's burst bounded by a loss-taught train cap, the window controllers' train logic its single owner (items 1, 2).
