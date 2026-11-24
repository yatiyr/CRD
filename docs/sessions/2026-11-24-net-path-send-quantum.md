# 2026-11-24: NET-PATH, the board counts datagrams and a held flight bursts a send quantum

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Remaining item: crd-net's bulk path brought to msquic's cost on the
board. CI first: nothing finished since 34e72e21 (CI 45299910780 pending, host-checks 45299047960 and 45296064704
queued).

## What landed

- Board: each end reports `datagrams_sent`, `send_messages`, `datagrams_received` and `receive_messages` (crd-net
  from its socket's counters, msquic from `QUIC_PARAM_GLOBAL_PERF_COUNTERS`); `quic_board.py` requires them and
  prints datagrams per train, ACKs per MiB and data per ACK.
- Engine (congestion interface version 8): `CongestionController::burst` takes the RTT. NewReno's and CUBIC's burst
  is the initial window (RFC 9002 section 7.7, read again this session), except while the window or the pacer holds
  an ACK-clocked flight back and it has not restarted from nothing in flight: then a send quantum, a millisecond of
  the pacing rate, at most 64 KiB, at least that window. A version 7 policy keeps the initial window. BBR shares the
  constants; Prague's burst before a sample is the initial window exactly.

## Evidence

- The counts found the cause: 10 datagrams per train (the initial-window burst) and 54-69 ACKs per MiB against
  msquic's 40 and 8-10; pacing off grew trains to 23-32. Both stacks ACK about once per two trains.
- Board ([2026-11-24](../bench/2026-11-24-net-path-send-quantum.md)), 1 MiB windows: 398 to 534 MiB per CPU-second,
  trains 10 to 21.9, ACKs 54 to 16 per MiB; msquic's lead 2.09 to 1.64 times. 64 KiB: 355 to 403. The wall rate at
  64 KiB fell (596 to 473 MiB per second); open.
- A quantum on every flight (the first version) put 114-157 drops and 17-20 reductions into a 32 KiB queue at
  1 Gbit/s and 2 ms after each idle second; the landed rule 1-5 and 1, as the initial window. That case is now a test
  (teeth: the restart rule removed fails it and three burst checks, 7 assertions).
- New cases: the burst at long, short and loopback round trips, unpaced, version 7, a restart, a pacer or window hold,
  an application-limited seed and a jumbo initial window. The congestion fuzzer checks the rule on every input.
- linux-gcc-debug: all 1,175 `crd-net-tests` cases (8 skips), the 19 `crd-no-*`/`crd-check*` guards.
  linux-gcc-public-checks: `crd-net-quic-board-check`. libFuzzer, 150 s each: congestion (359,641 runs),
  quic-connection (6,795); the congestion corpus replays. No find. clang-tidy 22.1.8 clean on congestion.cpp/.hpp,
  quic_path_control.cpp and the three changed tests; `cerid_quic_board.cpp` is UNGATED there (no Windows unit
  compiles it), built with -Werror on Linux. Format on added lines; braces pass.
- Coarse turns (temporary fixture change, every event rounded to 250 us or 1 ms; numbers in the board note): the
  quantum fills 73-93% of 1 Gbit/s where the initial window fills 38% (9% at 1 ms turns), but under a burst-sized
  queue it loses: 12 KB at 1 Gbit/s 6-13% against 38%, and at 100 Mbit/s with 1 ms turns 65-69% and 216 drops
  against 96% and none. Unproven until bounded.

## Next

- Bound the quantum by what the path's losses teach (a loss in a burst lowers the cap toward the initial window; it
  grows back while bursts go clean), with a turn period in `bottleneck_fixture.hpp` and the coarse-turn cases as tests.
- The wall-rate drop at 64 KiB windows (MAX_STREAM_DATA updates per MiB, time blocked on flow control, before and
  after). Then the 22-datagram train limit (`kBatchChunk`, `IoLoopConfig::batch`, the per-turn sends).
