# 2026-11-24: NET-PATH, a restarted flight's first round paced at one window per smoothed RTT

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Operator order: 65610837's regression (d), the idle-restart case
passing from version 8's own history. CI first: CI 45327125880 pending, 45322193716 queued; fuzz host-check
45321264762 (c6963151) ended during the batch with one red, net-quic-connection seed 8702, input 3e9670d4 (below).
GitHub's main is at c6963151, behind GitLab; left as it is.

## What landed

- NewReno and CUBIC (interface version 8): a flight that restarts from nothing in flight paces at one window per
  smoothed RTT until its first acknowledgement, then at the policy's gain again (`m_restart_round`, beside
  `m_restarted`, which ends the restart's initial-window burst when the pacer first holds it). BBR's restart from
  idle (draft-ietf-ccwg-bbr section 5.4, `restart_from_idle`) is the precedent. Version 7 and Prague are unchanged.
- The idle-restart case runs version 8 from the first byte, no live swap; a unit case covers the round (from
  nothing in flight, past a pacer hold, ended by the first ACK, not restarted with packets in flight, restarted
  again after a drain; version 7 and Prague untouched); the congestion fuzzer checks every window controller's rate
  is its gain's or, from version 8, one window per smoothed RTT.

## Evidence

- Cause: from version 8's own slow start NewReno keeps 277,781 bytes (one reduction; version 7 two, 184,990). A
  version 7 restart from that window, by a live swap, drops the same 32 with 2 reductions: the window at 1.25 windows
  per RTT overflowed the 32 KiB queue, not the quantum.
- Sixteen restart rows, version 7, before and after ([board note](../bench/2026-11-24-net-path-restart-pacing.md)):
  the operator's row 32 drops and 2 reductions to 1 and 1 (CUBIC 1 and 1 to none), each MiB 14% sooner; no row worse
  beyond 0.2 ms, the 20 ms rows 1 to 23 ms sooner. QUIC board, interleaved with 7889ca65's build: 64 KiB 432 per
  CPU-second and 680 per wall second against 436 and 684; handshakes and 1 MiB windows within spread.
- Teeth: with the gain back, both new cases fail (NewReno 32 drops and 2 reductions, CUBIC a reduction; the rate
  checks of version 8).
- linux-gcc-debug: `crd-net-tests` 1,183 cases (1,175 passed, 8 skipped); the 30 net fuzz corpus CTests and the
  `crd-no-*`/`crd-check*` guards, 49 of 49. libFuzzer (linux-clang-fuzz): net-congestion, seed 8406, 301 s, 697,700
  runs; net-quic-connection, seed 8407, two jobs, 361 s, 26,602 runs; no find.

## Host-check 45321264762, seed 8702 (open)

Input 3e9670d4 fails the follow check (`follows() || (ended() && !proving(true))`) on HEAD too. Cause, traced: the
client's path table dropped its fallback tuple (`QuicPaths::drop`) and kept none, though its first tuple was still
validated; the active tuple's revalidation then expired and the client closed with `path_lost` (RFC 9000 section
9.3.2 lets it revert). Refilling the fallback from the validated tuples left fixes the input but fails two others
in a 360 s run (07785c27, 2400d9c3: the preferred-address check `client.on_preferred_address()`), which pass
without it; not landed.

## Next

Host-check seed 8702 first (above). Then (b) the simulator rows below version 7, with version 7's exact-timer
number on each row to show whether its 95.1% at 1 ms turns is the turn's rate cap; CUBIC over a 12 KB queue at
1 Gbit/s and 2 ms restarts 2.5 ms slower than version 7 (47.7 against 45.2 ms, from a smaller window); (c) the
recovery-period rule on deep 20 ms rows. The coarse-turn (train-cap) table was not re-measured; its committed
cases pass.
