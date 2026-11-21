# 2026-11-22 — NET-PATH: multipath negotiated for path ID 0

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath), its
"Negotiation" bullet. Source: RFC 10069 sections 2 to 2.3, 3.2 to 3.4 and 4 to 4.7, read from the RFC Editor's text.

**CI first.** Fuzz host-check 44926199681 (c2d3610f) failed net-quic-recovery leg 8202 with two events. Both are
the receive-timestamp oracle in `op_write`, which 607b544d fixed in the fuzzer's model after c2d3610f. Both
artifacts replay clean on a linux-clang-fuzz build of HEAD. Nothing else of this track's has failed.

## What landed

- A connection offers RFC 10069 when `parameters.has_initial_max_path_id` is set. `config_valid` admits only
  initial_max_path_id 0 until paths open, which section 2.1 defines as multipath with no further path ID.
- Before, a caller could set the parameter, and the connection then offered it while still reading every multipath
  frame as unknown. `QuicConnectionConfig` now refuses any other value.
- A client offering multipath refuses a zero-length connection ID at `connect()`. `quic_check_multipath` had no
  caller; it now runs where the peer's parameters are read.
- Negotiated (`multipath()`): path ID 0's IDs go in the PATH_ forms (section 3.2). The plain frames still count
  for path ID 0. Every frame is read under this side's limit (section 4).
- PATH_ACK of path ID 0 shares the ACK frame's handler (`ack_frame`, moved out of `on_frame`).
- MAX_PATH_ID raises `peer_max_path_id()`. PATH_STATUS keeps the newest sequence (`peer_path_status()`).
- PATHS_BLOCKED and PATH_CIDS_BLOCKED are checked (`QuicCids::local_next`) and inform only.
- PATH_ABANDON of the only path closes the connection with NO_ERROR (section 3.4).

## Evidence

- `[multipath]`: 29 cases on linux-gcc-debug. New in `test_quic_connection.cpp`:
  - every accepted frame and 12 refusals;
  - both ID forms in every offer combination, and FRAME_ENCODING_ERROR when one end alone offers;
  - zero-length IDs, with and without the server's offer;
  - a pair that negotiates (or not), tops up and rotates;
  - the config refusal and the `connect()` refusal.
- `crd-net-tests "[net]"`: 861 cases, 7 skipped as before. The connection corpus replays clean.
- Teeth: with the limit check and the PATH_ forms disabled, two cases and five assertions fail.
- The connection fuzzer's NET-STD bit now offers multipath from both ends and from the raw client. New oracles:
  negotiated exactly when offered, and the peer's limit never below its initial value. New seed: `frames-multipath`
  (seed-73efb2a38134ec77).
- Register: `quic.multipath` stays planned; its note and evidence name the negotiation.
- libFuzzer, linux-clang-fuzz, 2 jobs for 300 s: 68 k runs, no artifact, corpus 41 to 2199.

## Next

Opening paths (S78 "Lifecycle"):
- raise this side's limit to kQuicMaxPaths - 1 and issue IDs per unused path ID;
- the client opens the path; validation, then PATH_ABANDON with three PTOs of retention;
- a path ID's space opens once, as a fuzz oracle (its nonce is never reused);
- a datagram's path ID selects its slot before `open`;
- `QuicHost`'s ID table sized for every path's IDs.
