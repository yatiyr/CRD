# 2026-11-24: NET-PATH, a recovery period bursts the proven train; version 7's turn-capped rows

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Operator order: 65610837's regressions (b) and (c).
CI first: CI 45322193716 (5d441aa3) failed clang-tidy (win-tidy) on `kGigabit`/`kHundred` in
`test_congestion.cpp` (`readability-identifier-naming`); 569c3916 already renamed them, and no line added since
declares such a local constant. Its other jobs were still running; no other red. Dispatched fuzz host-check
45337144505 on 0a1b61b9. GitHub's main is at 0a1b61b9, level with GitLab.

## What landed

- NewReno and CUBIC (interface version 8): a recovery period bursts the proven train, as a probe out does, instead
  of the initial window (`WindowController::burst`). The congestion fuzzer's oracle allows a burst past the initial
  window in recovery (within the cap); its find against the old oracle, minimised to 16 bytes, joins the corpus.
- Tests: a train-cap case (in recovery 24000, the proven length, where the cap is 48000 and the old rule gave 12000;
  the cap back after the period); the coarse-turn case gains the 1 Gbit/s, 256 KiB, 20 ms row (2.4 times version 7)
  and the turn-cap rows (80 and 90 Mbit/s within 0.5% of version 7; 100 Mbit/s at 1 ms turns at 99% of version 7
  with exact timers, which version 8 meets or beats).

## Evidence ([board note](../bench/2026-11-24-net-path-recovery-burst.md): all 96 rows before and after)

- (c) traced: NewReno, 1 Gbit/s, 250 us turns, 256 KiB, 20 ms spent 0.14 s in 6 periods sending at 357 Mbit/s (the
  initial window a turn); with the proven train at 880 Mbit/s: 75.3% to 93.1% (CUBIC 93.0% to 96.8%). The cap
  itself in recovery cut CUBIC's 100 Mbit/s 12 KB 20 ms exact-timer row to 83.4%; the proven train keeps 88.6%.
- (b): v7's 95.1% at 100 Mbit/s and 1 ms turns is its 96 Mbit/s turn cap under the link (99.0% at 96 Mbit/s; under
  the cap v8 is at most 0.1 below and up to 0.4 above; above it v7 leaves 13-37% unused). Its zero drops at 1 Gbit/s
  and 250 us are its 12000 bytes a turn, 38.4%/37.9% at every depth; with exact timers it drops too, more than v8 at
  250 us except at 64 KiB 2 ms (v8 below exact-timer v7 on link and drops) and NewReno 256 KiB. Drops rose on 9 of
  14 changed rows; one row is 0.1 lower (27 neighbouring paths: means within 0.05 either way).
- Open: the exact-timer row (CUBIC 1 Gbit/s 12 KB 2 ms 87.4% against 88.3%) is the first 250 ms: version 8 leaves
  slow start at 114,000 bytes after 24000-byte trains overflow the 12 KB queue (8 drops), version 7 at 406,800 (174).
  Steady state 88.4% against 88.2-88.7%. Making slow-start train losses teach without a reduction was tried and
  rejected (slow start never ended: 0 reductions, 2,113 drops on 64 KiB rows).
- Teeth: with the old rule the new unit checks (12000 against 13200 and 24000) and the 256 KiB row (NewReno 4.77e9
  against 5.61e9, CUBIC 5.57e9) fail. Restart board, 32 rows: identical before and after.
- linux-gcc-debug: `crd-net-tests` 1,184 cases (1,176 passed, 8 skipped); corpus and guards 49 of 49. libFuzzer:
  net-congestion seed 8414, two jobs, 301 s, 1.4 M runs; net-quic-connection seed 8413, two jobs, 361 s, 23,201
  runs; no find.

## Next

Regression (b)'s exact-timer row stays open: bursting only the initial window restores 88.3/212/41, so the
quantum in slow start is the cause; the slow-start teach arm left the row unchanged, so the loss ending slow start
is not one `teach_train` handles. Then NET-PATH's Remaining.
