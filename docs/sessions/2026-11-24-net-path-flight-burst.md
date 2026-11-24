# 2026-11-24: NET-PATH, a burst at most half the flight that last ran out of data

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Operator order: 65610837's regressions, (a) first: crd-net's 64 KiB
wall rate. CI first: nothing finished since c6963151 (CI 45321234937 pending, host-check 45321264762 queued).
GitHub's main is one commit behind GitLab's (c6963151 against 5d441aa3), left as it is.

## What landed

- `congestion_flight_burst` (congestion.hpp): a burst past the initial window is at most half a flight that ran out
  of data with room in the window, never below the initial window (RFC 9002 section 7.7), never raised.
- `QuicPathControl::burst` applies it to every controller under a policy of version 8 or later: the flight is the
  last `on_app_limited` one, raised by any larger flight sent since, forgotten by a rebuild, restart or reset. The
  pacer's release and its credit both use it. `bottleneck_fixture.hpp` drives controllers directly, so no simulator
  row moves.
- The board: crd-net's ends report their loop's waits (count, timeouts, time asked and taken), a third table per MiB;
  `--baseline` runs another build's crd-quic-board as its own interleaved arm; `--workload` runs one workload.

## Evidence

- Cause ([board note](../bench/2026-11-24-net-path-flight-burst.md)): equal CPU per MiB with a lower wall rate, so
  both ends idled. The sender decides (mixed roles). A send buffer or a stream window near 64 KiB does it alike. The
  credit one ACK freed left in two trains, the receiver read both in one turn and ACKed once (25 datagrams per ACK,
  30 client waits per MiB against base's 8).
- After, 5 interleaved rounds: 64 KiB 688 MiB per wall second and 434 per CPU-second against 34e72e21's engine's 683
  and 409 (before: 497 and 422); 1 MiB 594 and 951 against before's 594 and 962. Divisors 4 and 6 gave 655 and 659.
- Tests: "a burst past the initial window is at most half a flight that ran out of data" (the function's edges and
  wrap) and "A path's burst ... is at most half the flight that last ran out of data" (Prague at 50 us: the bound, a
  full pacer's turn held to it, the floor, a larger flight raising it, a live swap keeping it and a version 7 swap
  lifting it, reset, restart and version 7). Teeth: with
  `burst()` returning the controller's, the path case fails (a 49,200-byte turn against 48,000).
- linux-gcc-debug: `crd-net-tests` 1182 cases (1174 passed, 8 skipped); the 30 net fuzz corpus CTests and the
  `crd-no-*`/`crd-check*` guards, 49 of 49. `quic_board.py --check` passes with the msquic arm. LLVM 22.1.8 tidy is
  clean on the path control and both test sources, after renaming the coarse-turn case's two local constants to
  the local style (`kGigabit`, `kHundred` were flagged).
- libFuzzer (linux-clang-fuzz): net-quic-connection, seed 8404, two jobs, 361 s, 26,714 runs; net-congestion, seed
  8405, 151 s, 375,931 runs; no find.

## Next

The rest of 65610837's regressions: (b) the simulator rows below version 7 and the deep 1 Gbit/s rows that now drop
where version 7 dropped none, (c) the recovery-period rule on deep 20 ms rows, (d) the idle-restart case from version
8's own history. Then the 33 PMTU probes at one instant (validation-room note).
