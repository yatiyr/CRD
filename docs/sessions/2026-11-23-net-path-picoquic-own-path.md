# 2026-11-23: NET-PATH, picoquic past the first path: its own path, status and abandon

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath)'s Proof bullet.

## CI first

Fuzz host-check 45187161054 (5871744e), leg net-quic-connection seed 8402, found `f3d9d7a4` (the follow check's
`heard` clause) beside the known `805dc37f`. It replays clean on HEAD; with e4dfd752's engine change reverted in the
mirror it fails again, so that fix (a challenge on path ID 0 answered by a packet that is not probing) covers it. It
joins `corpus-quic-connection` (97ce5654); `crd-fuzz-net-quic-connection-corpus` passes on linux-gcc-debug.

## What landed (4f812dcc: test peers and driver; no engine change)

- `crd-picoquic-quic-peer`: `--add-path-at N` opens path ID 1 from a second socket (`picoquic_probe_new_path_ex`;
  no-multipath when not negotiated), `--pin-added` gives the held last stream that path's affinity once available,
  `--backup-added` marks it backup, `--abandon-added` abandons it after the echo and closes once picoquic deleted it.
  Each datagram leaves from the socket picoquic names.
- `crd-quic-peer`: `--backup-added` (`set_path_status()` once the added path validated) and `--abandon-added`
  (`abandon_path()` once the pinned stream is acknowledged; the close waits for the path ID's space, at most 2 s,
  through `EchoTurn::close_held`, served on the timer: a loop showed 3 of 25 runs idling out without it).
- `picoquic_interop.py`: the multipath cases run both roles; "both backup" and "both abandon" cases per role.

## Evidence

- `--only multipath`: 16 of 16 in four runs on linux-gcc-public-checks; then 30 of 30 (Cerid client abandon), 25 of
  25 (picoquic client abandon), 25 of 25 (both backup cases) and 15 of 15 (the six v2 cases). Cerid's server
  acknowledges a picoquic client's pinned stream whole on path ID 1 (`other_path_delivered` at least 262,144).
- Engine mutants each fail exactly their case: the peer's status not stored (picoquic client backup), BACKUP sent as
  AVAILABLE (Cerid client backup), a PATH_ABANDON not answered (picoquic client abandon: `abandon_received`).
- picoquic drops a status for a path ID with no path yet (RFC 10069 section 4.3 allows it): registered as
  [TP-15](../third-party-defects.md#tp-15); Cerid's client sets it after validation. picoquic's deletion may outlast
  a Cerid client's three-PTO close, so that case checks each end's PATH_ABANDON arrived and Cerid's space closed.
- `ctest -R "crd-net-.*interop"` on linux-gcc-public-checks: all ten pass. Pinned clang-tidy 22.1.8 is clean on
  `cerid_quic_peer.cpp`; clang-format is clean on the added C++ lines.

## Next

- Audit host-checks 45238990084, 45229585649, 45211969030, 45194420211 and CI 45198716949 when they end.
- NET-PATH Remaining: the budget boards against msquic and quiche, and the real-NIC cloud runs.
