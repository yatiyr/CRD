# 2026-11-25: NET-PATH, a coarse host's turn of trains, and no restart for a flight the pacer held

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path), Remaining item "more than one train a turn for a host that wakes once a
millisecond". Follows [the one-train quantum](2026-11-25-net-path-one-train-quantum.md), Next 2. User decision on
record (2026-11-01): "I WANT NET DONE PROPERLY AND GOLD STANDARD AND WITHOUT ANY DEFERS!" Ran on this PC (WSL2
linux-gcc-debug, -release, linux-clang-fuzz).

**CI first:** nothing to fix: 45610916017 (1dbff857) 22 passed, none failed; 45648028909 (83d0f2f3) queued.

## Landed

- **`HostTurn`** (congestion.cpp): the shortest of the last 8 gaps between a window controller's send instants while
  the pacer held the flight, at most the shortest gap between its ACK frames' instants (a host that turns finer shows
  it while the window holds the flight). `CongestionState::host_turn_ns` reports it; a live swap keeps it.
- **`turn_most`**: on a host whose turn is a send quantum's millisecond or longer, where that turn at the pacing
  rate carries more than one train, the burst is the turn's whole datagrams and the train cap grows to them by the
  same probes, sent as several trains (`SendQueue::send_through`). A finer host keeps one train: ungated, the loopback
  board's ACK-woken host sent what each frame's room allowed, 26.3 datagrams a message against 43.4, wall -2% (whole-
  train bursts alone moved nothing: room, not credit, ended those instants).
- **Restart rule**: a flight the pacer held until a turn longer than its round trip, whose ACKs emptied it, did not
  idle. A game polling once a 60 Hz frame over a 2 ms path sent the initial window a frame (5.8% of 100 Mbit/s).
- **Resize rebase**: a datagram resize raises the train cap and its proven length to the new initial window. Under
  the 1200-byte initial window a 1472-byte flight made every train a probe that never settled, and a connection served
  once a 1 ms turn sent the initial window a turn for good (found by the connection test below).
- BBR keeps its one-train quantum: no loss-taught cap bounds a turn's burst there (Remaining).

## Evidence

- Grid (NewReno, CUBIC, BBR; 1 Gbit/s and 100 Mbit/s; exact, 250 us, 1 ms, 16.7 ms turns; 12 KB to 512 KiB; 2 and
  20 ms; 10 s; taken before the millisecond gate, which its 1 ms and 16.7 ms turns pass) against the same tree with
  the turn never measured: 176 of 240 rows identical (every exact, 250 us, BBR and 100 Mbit/s 1 ms row). NewReno /
  CUBIC, before -> after (link, drops, reductions):
  - 1 Gbit/s, 1 ms, 256 KiB: 2 ms 51.69% -> 92.71% / 99.36% (613 and 624 drops, 17 reductions, as exact timers'
    99.8% with 443 and 17); 20 ms 50.48% -> 87.95% / 90.88% (exact: 65.0% / 85.3%).
  - 1 Gbit/s, 1 ms, 512 KiB: 51.69% -> 99.52% / 99.58% at 2 ms; 50.48% -> 91.43% / 92.00% at 20 ms.
  - 1 Gbit/s, 1 ms, 64 KiB: 51.69% / 50.48% kept, no reduction; 63 / 59 drops where none (the first probe past a
    train loses 54 once). A one-datagram first step cost 10 / 6 but cut 256 KiB at 20 ms to 77.2% / 84.8%.
  - 100 Mbit/s, 16.7 ms frames, 512 KiB: 2 ms 5.75% -> 96.46%, 20 ms 28.19% -> 93.99%; 256 KiB 2 ms 5.75% -> 82.68% /
    95.84%. At 1 Gbit/s a 512 KiB queue holds a frame's burst to 24% (0.57% and 2.81% before). The 16.7 ms rows at
    12 KB and 32 KiB drop 15 to 19 datagrams where they dropped 0 to 6.
- Tests: the turn's filter (a stall, window-held gaps over two blocks, a finer host, ACK gaps, swaps, version 7 and
  Prague); the Flow rows above, a 250 us host at 10 Gbit/s keeping one train, a 50 ms stall and a 1 ms host turning
  exact (one train after), a 1 kHz host jittering 0.95 to 1.05 ms (measures 0.95, keeps one train). A QuicConnection
  pair served once a turn measures 1 ms and 16.7 ms, and at 1 ms (1 Gbit/s, 512 KiB) moves over 1.5 trains a turn in
  the transfer's second half. Teeth, failed assertions: turn ignored 12, ACK gaps ignored 6, restart rule reverted 4,
  window gaps counted 2, seed dropped 2, millisecond gate removed 2, resize rebase removed 1. Debug crd-net-tests
  1,215 cases pass (8 RIO skipped); guards 19 pass.
- Fuzz: the oracle bounds the burst by a millisecond-or-longer turn's whole datagrams and the cap by one train until a
  turn is measured; corpus replay 14 clean; 691,034 runs (300 s) before the gate, 390,265 (180 s) after it and
  279,907 (150 s) after the resize rebase, clean.
- Board (1 GiB, five interleaved rounds, best (median) MiB per CPU-second, wall): against 1f7cb1dd's build, 1 MiB
  windows 736 (707) against 742 (729), wall 1,212 both, 43.4 datagrams a message against 43.3; 64 KiB 494 (491)
  against 504 (500), wall 784 both. Against the same source with the turn and restart rule off: 735 (722) against
  740 (724), wall 1,216 against 1,239; 64 KiB 485 (480) against 493 (479), wall 761 against 780. Every count
  (messages, ACKs, waits) equals the control's.

## Next

1. CI first: the first finished run with this change (MSVC, clang-cl, ARM64, TSan; `CongestionState` grew a field).
2. A connection whose executor's turn passes its round trip (the 16.7 ms frame over 2 ms) still sends the initial
   window a turn: the flight-burst bound (half the flight that last ran dry, raised only by larger flights) never
   rises, as each turn's ACKs empty its flight.
3. Host waits finer than a millisecond: `wait_milliseconds` rounds every pacing wait of `IoLoop::run` up to whole
   milliseconds (Linux `ppoll`/`epoll_pwait2`, a Windows high-resolution waitable timer).
4. More than one train a turn under a millisecond (instants cut at whole trains where the window's room ends them),
   and BBR's quantum past one train on a coarse host, bounded by a loss-taught cap.
