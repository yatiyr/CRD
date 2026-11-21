# 2026-11-22 — NET-PATH: abandoned path IDs' connection IDs, and numbering per path slot

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath), its
"Connection IDs" and "Paths and identity" bullets. Source: RFC 10069 sections 3.1, 3.4 to 3.4.3, 4 and 7.1.

**CI first.** Nothing to fix: every run on this track's commits is still queued (fuzz host-check 44951969164 on
85482a0e, CI 44951971739 on 86c068aa).

## What landed

- `QuicCids::abandon` and `abandoned` (section 3.4). The peer's IDs for the path ID are retired at once: their
  tokens are unwatched, no retirement frame is owed, and the peer's later frames for it are ignored (section 4).
  This side's IDs stay routed and `find()` still names them, so packets in transit open and draw no stateless reset
  (section 3.4.2). Nothing is issued, rotated or announced there again, and a lost frame is not owed again.
  `close()` ends the retention.
- The connection numbers each path slot's Application space apart: next and skipped numbers, owed probes and the
  ACK-only run, in one `Numbering` per number space. Each sent packet records its slot (`QuicSentPacket::path`).
- Each sent-frame record names its slot. Before, the settle walk could run from one path's packet into another
  path's records under the same number.
- The design now decides where per-path state is allocated (S78 "Paths and identity"). It cannot wait for
  validation as section 7.1 suggests: the PATH_CHALLENGE is packet 0 of the new space, and it must be opened,
  acknowledged and answered in that space.

## Evidence

- `[cids]`: 12 cases on linux-gcc-debug. The new case covers each half of abandoning, the ignored frames, the
  refused issue and rotation, the straggler found until `close()`, and the storage reopened.
- `crd-net-tests "[net]"`: 861 cases, 7 skipped as before (limit still 0, so slot 0 alone numbers).
- The cids fuzzer gains an abandon operation (the close op's odd first byte) with the model's rules restated, plus
  an `abandoned()` oracle. New seed `path-abandoned` (seed-877a7e3e128750e6); the corpus replays clean.
- Teeth: with the owed retirements kept through abandon, the new case fails (line 217) and the fuzzer's
  `retirement_owed` oracle fails on the new seed.

## Next

The path ID lifecycle in the connection, with the limit raised only once opening and abandoning both work:
- this side's limit up to kQuicMaxPaths - 1, and ID spaces for path IDs up to the smaller limit;
- a client opens a path; the server opens the number space at the first authenticated packet;
- PATH_ABANDON both ways, with ledger records for it and MAX_PATH_ID, and three PTOs of retention;
- PATH_ACK and PATH_STATUS per slot, and `send_slot()` choosing an open, validated path.
