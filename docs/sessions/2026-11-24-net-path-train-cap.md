# 2026-11-24: NET-PATH, the send quantum bounded by a train cap the path's losses teach

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Operator order: both regressions of 65610837 gone before other work.
CI first: nothing finished since 34e72e21 (CI 45307833572 pending; host-checks 45307860762 and 45299047960 queued).
GitHub's main is one commit behind GitLab's (1b6e5bc8 against b986769c), left as it is.

## What landed (6184d737, 3c859371)

- `Pacer::train`: the bytes that left paced at the same instant (one host turn). `QuicConnection` records it in
  `QuicSentPacket::train` (the struct's spare four bytes) and `CongestionPacket::train` carries it.
- NewReno and CUBIC (interface version 8 still) burst at most a train cap: it starts at the initial window and grows
  only from a proven cap, doubling to 64 KiB until a loss teaches it, then a datagram after 8 clean rounds at it.
  A growth is a probe in two steps, one train past the proven length, then one round sending every train at it. A
  probe's loss teaches the cap and starts no congestion event (RFC 9000 section 14.4's reasoning for PMTU probes),
  and so does every loss of a packet the probe's period sent before it; the next growth waits twice as long, to
  1,024 rounds. A loss past the initial window's bytes otherwise lowers the cap with a reduction, and the cap doubles
  back to what was proven once a round proves it. A recovery period bursts the initial window. An undone period gives
  the cap back; a live swap carries it (`CongestionState::train_cap`); a reset forgets it. Prague, whose burst is
  its own, and a version 7 policy keep none (the fuzzer's oracle found Prague inheriting the exemption).
- `bottleneck_fixture.hpp`: `set_turn` (every ACK and send waits for the next turn, ACKs settled first).

## Evidence

- Simulator, CUBIC and NewReno, 10 s ([board note](../bench/2026-11-24-net-path-train-cap.md)): the operator's rows
  are met. 12 KB queue at 1 Gbit/s: 38.4% and 37.9% (v7 38.4/37.9, 65610837 6.6-15.2), 6-17 drops, no reduction.
  100 Mbit/s, 1 ms turns, 12 KB: 95.9/95.1% (v7 the same with no drops; 65610837 56.5-69.5% with 112-338 drops),
  6-12 drops, no reduction. Deep queues at 1 Gbit/s and 250 us: 75-99.7% where v7 fills 38%.
- Not met: 100 Mbit/s, 1 ms turns, 32 KiB at 20 ms (CUBIC 92.3%, NewReno 81.1%, v7 95.1%) and NewReno at 64 KiB
  (89.9%). v7's 12000 bytes a millisecond is 96 Mbit/s, under that link: it never fills the queue. The same
  controllers with an exact timer get 91.0% and 81.6% there, and on an 88 Mbit/s link v7 gets no more (80.3%).
- Exact timers: a third 65610837 regression found and fixed (CUBIC, 100 Mbit/s, 12 KB, 20 ms: 83.4% back to v7's
  88.6%, by the recovery-period rule). Residual: 1 Gbit/s, 12 KB, 2 ms, 87.4% against v7's 88.3% (65610837 too).
- New tests: the cap's whole cycle (probe, drain, a probe's and a fuller queue's loss, the wait doubling, the way
  back, version 7, seed, reset, undo); the coarse-turn cases (shallow rows no worse than v7 less the probes'
  datagrams and with no reduction, deep rows 2.5 to 5.4 times v7, 0.1% random loss, beside a v7 flow that keeps all
  it had); a path control's trains and a probe's loss reducing nothing. The idle-restart case now swaps from a v7
  history: with v8's own slow start (one reduction, not two) its window is larger and restarts overflow at the
  pacing rate (32 drops), whatever the burst.
- linux-gcc-debug: all 1,179 `crd-net-tests` cases (8 skips) and the 19 `crd-no-*`/`crd-check*` guards pass; the
  congestion corpus replays. libFuzzer congestion, 150 s: 345,873 runs, no find.
- Board, crd-net arm: 587 MiB per CPU-second at 1 MiB windows (534 at 65610837; trains 22.1, ACKs 16 per MiB: kept).
  At 64 KiB 416 per CPU-second and 493 per wall second: the wall rate is still under 34e72e21's 596.

## Next

- Regression (2), the wall rate at 64 KiB windows: interleaved rounds against 4fb61ff3 (34e72e21's engine, today's
  board harness; `git diff 34e72e21 4fb61ff3 -- engine/` is empty), with MAX_STREAM_DATA updates per MiB and time
  blocked on flow control at each end.
