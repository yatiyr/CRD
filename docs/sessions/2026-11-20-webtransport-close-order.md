# 2026-11-20 — WebTransport: a close never outruns its capsule

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path) (the follow-up its last batch named). Design: [S59](../design/network-foundation.md#webtransport).

## CI first

Run 44807121941 (730a2e86) was still running at the end of this batch. Both repository checks passed, and no job had
failed. This push tier does not run win-relwithdebinfo, so the C4723 fix (6951b10a) still waits for a scheduled run.

## The defect

webtransport-go `closer client window 65536` failed 1 run in 9 on GitHub's machines. Cerid's `close()` wrote the
WT_CLOSE_SESSION capsule and FIN, then at once stopped the CONNECT stream's reading part. The STOP_SENDING is a control
frame, so it went out ahead of the capsule, which was waiting for flow-control credit. Its arrival made
webtransport-go 0.13's next capsule write fail. That ended its session with WT_SESSION_GONE before it read the capsule
(`writeToConnectStream`). Draft -17 section 6 makes that STOP_SENDING optional (MAY) and requires the recipient to close
or reset the stream.

## What landed

- A closing session keeps its slot (`Closing`). It discards what the peer still sends, a capsule crossing the close
  included, and frees itself on the peer's FIN, reset or STOP_SENDING. It never stops the reading part itself.
- A peer that never answers costs nothing. Once the capsule and FIN are acknowledged, the session drops out of
  `sessions_alive()`. When a new session needs the slot, the old one is stopped with WT_SESSION_GONE.
- S59's Closing paragraph and the header's contract say so.

## Evidence

- Six new unit sections:
  - nothing overtakes a held-back capsule;
  - bytes after the close are discarded until the peer's FIN;
  - a reset by the peer ends the session at once;
  - a simultaneous close reports nothing at either end;
  - a slot whose peer never answers is reclaimed by the next session;
  - a slot whose capsule has not been acknowledged is not.

  On HEAD's engine, four of the six fail. The first fails because the server sees Stopped while the capsule is held.
- win-debug (MSVC 14.51): all 944 `crd-net-tests` cases pass.
- WSL linux-gcc-public-checks: the Go `window 65536` cases ran 30 rounds, 120 runs in all, with no failure. The full Go
  matrix passed 34 of 34, and the Quinn and Cerid matrix 94 of 94. The failure never showed locally before the fix,
  so the unit test carries the proof of the order.
- linux-clang-fuzz: `crd-fuzz-net-webtransport` replayed its corpus clean, then ran 300 s into a scratch directory:
  35007 executions and 671 new units, with no finding.

## Next

Connection migration, NAT rebinding and LEO handover (NET-PATH).
