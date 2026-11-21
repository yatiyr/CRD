# 2026-11-21 — NET-PATH: the recovery fuzzer's model honours the replay window

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S37](../design/network-foundation.md#quic-recovery) and
[S78](../design/network-foundation.md#multipath).

**CI first.** Fuzz host-check 44913047111 (089cbcd3) failed both `net-quic-recovery` legs, 8201 and 8202, with 35
events. Every event was one oracle: an ACK frame's receive timestamp did not match its packet's arrival
(fuzz_quic_recovery.cpp:543). All 20 inputs from leg 8201 replay on a linux-clang-fuzz build of 089cbcd3. The
minimised input receives packet 80, has the ACK frame carrying it acknowledged (which retires 80's range,
RFC 9000 section 13.2.4), then feeds packet 80 again. The tracker records 80 again, and the next ACK frame reports
both arrivals.

## Cause and fix

- The engine is not at fault. `QuicPacketProtection::open` checks every packet against its space's replay window
  before the AEAD runs and accepts the number after it (quic_packet.cpp). A copy is Duplicate or Stale and never
  reaches recovery (RFC 9000 section 12.3). Held packets take the same `open`. The hostile datagram flow
  (5 % duplication, "no datagram arrives twice") already proves this end to end. A floor in `QuicAckTracker` would
  give section 12.3 a second owner, and would stop acknowledging late packets that the window still finds fresh,
  which takes away the sender's spurious-loss signal. It was written, then dropped.
- The fuzzer fed recovery a packet that a connection can never deliver. Its model now leaves out a copy of a number
  whose range was forgotten. It still feeds copies the ranges hold, so the refusal oracle stays live, and it adds
  "a number is recorded at most once". `QuicAckTracker::on_received` and `QuicRecovery::on_received` now state that
  the replay window owns duplicate suppression.
- New seed `copy-after-retired-range` (seed-810ee9f2244c606c) is the minimised scenario in the current input format.
  Without the model fix it fails the same oracle on a HEAD-era binary; with the fix it passes.

## Evidence

- linux-gcc-debug: `crd-net-tests "[net]"` 856 cases (7 skipped as before); the recovery corpus replays clean.
- libFuzzer (linux-clang-fuzz, HEAD plus the fix): 4 jobs for 330 s. Two jobs ran the full time clean, with about
  960 k runs. Two stopped on an ASan SEGV at pc 0 and pc 1 in `check_state`. Neither input replays (three replays
  each), and the 5218-input working corpus replays clean three times. This is the host's known pattern
  (`host-misexecutes-local-faults`), as in the last batch.
- Queued host-checks 44926199681 (c2d3610f) and 44932060326 (e3a266b4) predate the fix. Their recovery legs will
  stop on this oracle within seconds, so they say nothing about the per-path control oracles. A new host-check runs
  on this commit.

## Next

Path IDs with their connection IDs and lifecycle (S78). `QuicPacketProtection` keeps one replay window per space,
so each path's Application number space needs its own window when path IDs land, or this hole opens for real.
