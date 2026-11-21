# 2026-11-21 — NET-PATH: multipath QUIC's wire layer (RFC 10069)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath).

**CI first.** No run has finished on 089cbcd3, and host-check 44913047111 is still queued. Scheduled run 44895376363
(cf3611f8) fails linux-gcc-release, -relwithdebinfo and -shipping with the LTO array-bounds error that 7ed26c44 fixes.

## What landed

- draft-ietf-quic-multipath-21 became RFC 10069 on 2026-11-07. Read in full: `rfc10069.txt`, SHA-256 c5e80af1...dcdc69.
  Its codepoints match IANA's `quic.xml` (04ac5756...6443) and the pinned picoquic. The register entry is re-pinned
  to the RFC (`planned`, 15 codepoints), and gen-register-ids gains the QUIC Transport Error Codes registry.
- All nine frames of section 4, in 1-RTT packets only. A path ID or maximum above 2^32 - 1 is PROTOCOL_VIOLATION.
  PATH_ACK goes through the ACK family's one writer. PATH_NEW_CONNECTION_ID shares NEW_CONNECTION_ID's reader and
  counts as probing.
- `initial_max_path_id` is at most 2^32 - 1 and is never remembered for 0-RTT. Once both ends offer it,
  `quic_check_multipath` refuses zero-length connection IDs.
- crd-crypto's `quic_packet_nonce` puts the path ID into the nonce. `seal` and `open` take the path, and path 0 is
  RFC 9001's nonce.
- A connection offers no multipath yet. It refuses each multipath frame with FRAME_ENCODING_ERROR, or with
  PROTOCOL_VIOLATION outside 1-RTT packets.

## Evidence

- On win-debug and linux-gcc-debug: `[quic-frame]`, `[quic-tp]` and `[multipath]` (21 cases) and crd-crypto `[quic]`
  (15 cases) pass. The crypto cases cover section 2.4's example nonce, path separation under all three AEADs and one
  limit shared across paths.
- The connection case uses a raw client that offers the parameter. Its handshake completes, and each of the eight
  frames then closes the connection with the expected error. With the refusal removed, the case fails.
- Full crd-net-tests pass: 1042 cases on win-debug and 1043 on linux-gcc-debug, with 7 skipped as before. On
  win-debug, all 29 net corpora replay and the 53 `crd-no-*`, `crd-check*` and drift guards pass. LLVM-22 tidy finds
  nothing in the changed sources or tests.
- Codec fuzzer: new oracles check path bounds, 1-RTT placement and PATH_ACK round trips; new seeds cover every
  frame, the parameter and a MAX_PATH_ID of 2^32. libFuzzer ran the current binary with four jobs for 300 s, and
  coverage rose from 946 to 1036 edges. One job ran the full time (6.0 M runs). The other three stopped on
  wild-jump SEGVs: a PC in non-executable memory or a zero frame pointer, with no frame in our code. A stale-binary
  run of the old codec stopped the same way. No artifact replays, either on the libFuzzer binary three times or on
  linux-gcc-debug, and the 5422-input working corpus replays clean on both. This is the host's known fault pattern
  (`host-misexecutes-local-faults`); a hosted fuzz host-check on this head will confirm it.

## Next

S78's connection slices, in order:
- Per-path Application recovery with path 0 only, keeping every existing case unchanged.
- Path IDs, connection IDs and the lifecycle, proven against picoquic on two paths.
- Scheduling, hostile-path tests and the fuzz oracles.
