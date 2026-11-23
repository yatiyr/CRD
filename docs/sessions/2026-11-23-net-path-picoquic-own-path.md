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

- `crd-picoquic-quic-peer`: `--add-path-at N` opens path ID 1 from a second socket (`picoquic_probe_new_path_ex`,
  asked again after a transient error; no-multipath when it is not negotiated); `--pin-added` holds the last stream
  until picoquic reports the path available and gives it that path's affinity (path ID 0 when the probe answered
  otherwise); `--backup-added` marks it backup; `--abandon-added` abandons it once every echo has ended and closes
  once picoquic deleted it. Each datagram leaves from the socket of the local port picoquic names. New report
  fields: `other_path_delivered`, `backup_seen`, `abandon_received`, `paths_deleted`, `add_path`, `added_path_id`,
  `pinned`, `abandon`.
- `crd-quic-peer`: `--backup-added` (`set_path_status()` once the added path validated) and `--abandon-added`
  (`abandon_path()` once the pinned stream is acknowledged; the close waits for the path ID's space to close, at most
  2 s, through `EchoTurn::close_held`); reports `backup`, `abandon`, `peer_backup`, `abandon_answered`,
  `paths_abandoned`, `paths_closed`.
- `picoquic_interop.py`: the multipath cases run both roles; new "both backup" and "both abandon" cases per role.

## Evidence

- `--only multipath`: 16 of 16 cases pass in three runs on linux-gcc-public-checks; a picoquic client's pinned stream
  is acknowledged whole on path ID 1 by Cerid's server (`other_path_delivered` at least 262,144).
- Engine mutants each fail exactly their case: the peer's status not stored (picoquic client backup), BACKUP sent as
  AVAILABLE (Cerid client backup), a PATH_ABANDON not answered (picoquic client abandon: `abandon_received`).
- picoquic drops a status for a path ID with no path yet (RFC 10069 section 4.3 allows it): registered as
  [TP-15](../third-party-defects.md#tp-15); Cerid's client sets it after validation. A first run also showed that
  picoquic's path deletion may outlast a Cerid client's three-PTO close, so a Cerid client's case checks each end's
  PATH_ABANDON arrived and its own space closed, and a picoquic client's case its deletion.
- `ctest -R "crd-net-.*interop"` on linux-gcc-public-checks: all ten pass (4f812dcc). Pinned clang-tidy 22.1.8 is
  clean on `cerid_quic_peer.cpp`; clang-format is clean on its added lines and `interop_echo.hpp`'s.

## Next

- Audit host-checks 45238990084, 45229585649, 45211969030, 45194420211 and CI 45198716949 when they end.
- NET-PATH Remaining: the budget boards against msquic and quiche, and the real-NIC cloud runs.
