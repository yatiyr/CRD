# 2026-11-21 — NET-PATH: multipath QUIC's per-path recovery (RFC 10069 section 5)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath) (Recovery),
[S37](../design/network-foundation.md#quic-recovery) (Paths).

**CI first.** Nothing to fix: no run containing f3fd650e has finished. Fuzz host-checks 44913047111 and 44918653833
are still queued, and both are on 089cbcd3, so neither covers the multipath codec oracles.

## What landed

- `QuicRecovery` keeps one Application space per path slot (kQuicMaxPaths). Each slot has its own RTT, probe backoff,
  loss detection, reordering thresholds and bytes in flight. Slot 0 is the handshake's path and shares its estimate
  with the Initial and Handshake spaces. Every call defaults to slot 0, so a connection without multipath behaves as
  before.
- `open_path` gives a slot its own storage. `close_path` closes the slot first, then reports each outstanding packet
  lost with `on_path` false, so its frames go back to the queues and no controller counts the loss. Slot 0 closes only
  after confirmation and after both handshake spaces are discarded, which also ends receive timestamps. The last open
  slot never closes.
- One loss timer is the earliest of every path's, and a probe backs off only its own path. `on_ack` takes the slot the
  frame arrived through. A change of that slot, or its closing, restarts the minimum RTT (section 5.4).
  `largest_probe_period_ns` now sets the connection's three PTOs (sections 2.5 and 2.6). `on_ack_acknowledged`
  retires a path's ranges when its PATH_ACK went on another path. `QuicSentPacket::path` fills existing padding; a
  static_assert checks the size.

## Evidence

- Every existing case passes unchanged: win-debug crd-net-tests 1051 cases, 7 skipped as before; linux-gcc-debug 1052;
  linux-gcc-release `[recovery]` 42 cases (the LTO build gives no array-bounds warning). 8 new `[multipath]` cases.
  Breaking the minimum's restart or closing the slot after its events fails them.
- Recovery fuzzer: one model space per slot, operations aimed at a slot, slots opened and closed, and an isolation
  oracle (an operation on one slot leaves every other slot exactly as it was). Two new seeds; the 7-seed corpus decodes
  as before. Its first finding was the oracle's own: after a restart, min_rtt can exceed smoothed_rtt. It now checks
  min <= latest and, when a restart was due, min == latest. That input is adopted.
- libFuzzer, 300 s, 4 jobs then 2: 1.58 M runs, coverage 1169 to 1202 edges. Two jobs ran the full time clean. Four
  stopped on faults no input reproduces: ASan stack-use-after-return, UBSan pointer overflow and null `span`, and a
  SEGV in no module. None replays three times on the libFuzzer binary, nor on gcc-debug or win-debug, and the
  4209-input working corpus replays clean. This matches the host's known pattern (`host-misexecutes-local-faults`);
  the hosted fuzz host-check dispatched on this head decides it.
- LLVM-22 tidy is clean on the four changed files and quic_connection.cpp. All 68 corpora replay on win-debug, and the
  20 `crd-no-*` and `crd-check*` guards pass.

## Next

Per-path congestion with its pacer, ECN and PMTU in the connection, then path IDs, their connection IDs and the
lifecycle, against picoquic.
