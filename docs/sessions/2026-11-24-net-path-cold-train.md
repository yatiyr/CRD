# 2026-11-24: NET-PATH, the train cap grows only from trains a held host sent

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Operator order: 65610837's regression (b), its last open row.
CI first: CI 45322193716 (5d441aa3) failed `crd-net-qmux-interop-crate` on linux-gcc-public-checks (its tidy red
was 569c3916's, already fixed). Its two datagram cases reproduce here every run: the crate echoed 128 datagrams and
the Cerid client read 64. Our crate peer's server echo never yielded, so a burst of received datagrams filled the
crate's send lane, which sheds what it cannot take. It now yields after each echo, as its client's sends do; 36 of
36 datagram cases and two whole runs pass. Its third failure (2 of 4 unidirectional streams read, server closes,
over ws) did not reproduce in those two runs. Host-check 45342658777 (f3fceaef) is still queued.

## What landed

- The train cap (NewReno, CUBIC, version 8) grows only after a round whose trains, this round's or the last's,
  reached the proven length. The trains of a cold flight's first round do not count: one that starts from nothing
  in flight with no acknowledgement in the send quantum (1 ms) before it (the first flight, or one after idle). A
  flight that a host turn's acknowledgements emptied still counts: `IoLoop` reads the clock again for a turn's sends,
  so they follow its acknowledgements by the turn's work, not at the same instant (the first push compared the
  instants, which held only in the simulator; a follow-up commit widens it to the quantum, every board row identical).
- Cause (traced): the first flight's 12000-byte train doubled the cap to 24000. On exact timers no later train is
  longer than two datagrams, so no probe ever went, and the unproven 24000 bytes stayed pacer credit. In slow start
  that let ACK-clocked runs go at twice the link's rate, which overflowed a 12 KB queue at a 61,200-byte window.
- Tests: a new [congestion] case. With short trains after a cold start the cap and the burst stay 12000, and a live
  swap carries 12000. A drained turn's train doubles the cap. Version 8 matches or beats version 7 on the
  1 Gbit/s, 12 KB, 2 ms path. The restart case's comment gives its new windows. `Trains` gains drained trains.

## Evidence ([board note](../bench/2026-11-24-net-path-cold-train.md))

- Grid, 96 rows: all 32 exact-timer rows now equal version 7's cells. CUBIC 12 KB goes 87.4% to 88.3%. Three
  NewReno rows fall to version 7's value, which they had beaten only on the unproven credit (32 KiB 83.03 to
  82.84). All 64 coarse-turn rows are identical. Restart board: every slow start leaves version 7's window, drops
  are equal, and done is equal or sooner.
- A rule that skipped every flight starting from nothing in flight was tried and rejected: it delayed the first
  growth on coarse hosts, and shallow coarse rows went from 12 to 19 probe drops.
- Teeth: counting cold trains, removing the reach gate, treating a drained turn as cold, comparing exact instants
  (a turn's sends 100 us after its acknowledgements) or a window that takes in a 2 ms idle each fails the new case.
- Final source: linux-gcc-debug `crd-net-tests` 1,185 cases (1,177 passed, 8 skipped), corpus and guards 49 of 49;
  clang-tidy clean on both files; libFuzzer net-congestion seeds 8415 and 8416, two jobs each, 2.33 M runs, no find.
- QUIC board against 912e3e0d's build, interleaved: 64 KiB windows (11 rounds) 434 against 432 MiB per CPU-second
  and 690 against 663 per wall second; 1 MiB windows (5 rounds) 591 against 591 and 937 against 904.

## Next

Regression (b) is closed, which closes 65610837's four regressions. Audit host-check 45342658777. Watch the
public-checks qmux unidirectional-stream case in the next finished run. Then NET-PATH's Remaining.
