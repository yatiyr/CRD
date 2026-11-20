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

- Six new unit sections: a held-back capsule is not overtaken; the peer's bytes are discarded until its FIN; its reset
  ends the session; a simultaneous close reports nothing; an unanswered slot goes to the next session, but not before
  its capsule is acknowledged. Four of the six fail on HEAD's engine (the first: Stopped while the capsule is held).
- win-debug (MSVC 14.51): all 944 `crd-net-tests` cases; WSL linux-gcc-debug: the 50 `[webtransport]` cases; clang-tidy
  20 clean on the three changed sources.
- linux-gcc-public-checks: the failing case (Cerid client closing against Go, window 65536) passed 30 times in a row.
  The full Go matrix passed 34 of 34, and Quinn and Cerid 94 of 94. The pre-fix binary was not looped here, so the unit
  test carries the proof of the order.
- linux-clang-fuzz: `crd-fuzz-net-webtransport` replayed its corpus clean, then 300 s into a scratch directory: 35007
  executions, 671 new units, no finding.

## Next

Connection migration, NAT rebinding and LEO handover (NET-PATH).
