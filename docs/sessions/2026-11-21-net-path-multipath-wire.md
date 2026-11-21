# 2026-11-21 — NET-PATH: multipath QUIC's wire layer (RFC 10069)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath).

## CI first

No run has finished on 089cbcd3. Host-check 44913047111 (legs 8601 and 8602) is still queued. The three red lanes
in scheduled run 44895376363 (linux-gcc-release, -relwithdebinfo, -shipping) built cf3611f8. That commit predates
7ed26c44, which already fixes their LTO array-bounds error in `SendQueue::queued`.

## What landed

- draft-ietf-quic-multipath-21 became RFC 10069 on 2026-11-07 (datatracker record; `rfc10069.txt` SHA-256
  c5e80af1...dcdc69, read in full). The suggested codepoints match the IANA QUIC registry (`quic.xml` 04ac5756...6443).
  The pinned picoquic sends the same codepoints. The register entry is re-pinned to the RFC (`planned`, 15 codepoints).
  gen-register-ids gains the QUIC Transport Error Codes registry.
- Frames: all nine of section 4, accepted in 1-RTT packets only. A path ID or maximum above 2^32 - 1 is
  PROTOCOL_VIOLATION. PATH_ACK is written through the ACK family's one writer. PATH_NEW_CONNECTION_ID shares
  NEW_CONNECTION_ID's reader and counts as probing.
- Transport parameter: `initial_max_path_id` must be at most 2^32 - 1. It is not in the early-data context.
  `quic_multipath_negotiated` and `quic_check_multipath` refuse a zero-length connection ID once both ends offer it.
- Nonce: crd-crypto's `quic_packet_nonce` puts the path ID into the AEAD nonce. `seal` and `open` take the path;
  path 0 gives RFC 9001's nonce.
- The connection does not offer multipath yet. It reads every multipath frame as FRAME_ENCODING_ERROR, the error for
  an extension that was never negotiated.

## Evidence

- `[quic-frame]`, `[quic-tp]` and `[multipath]` pass on win-debug and linux-gcc-debug (20 cases, 400817 assertions).
  So does crd-crypto `[quic]` (15 cases): section 2.4's example nonce, path separation under all three AEADs, and one
  confidentiality limit shared across paths.
- Full crd-net-tests: win-debug 1042 cases, linux-gcc-debug 1043, 7 skipped as before. linux-gcc-debug built with no
  warnings. On win-debug, all 29 net corpora replay and the 53 `crd-no-*`, `crd-check*` and drift guards pass.
- Codec fuzzer: new oracles cover the path bounds, 1-RTT-only placement and PATH_ACK round trips. The seeds now
  include every multipath frame, the parameter, and a MAX_PATH_ID of 2^32.
- libFuzzer ran 300 s with four jobs, about 5.3 M runs per job. It logged two null-page SEGVs inside
  `take_quic_varint`, reported at a line that holds no such call. Neither replays: not with the libFuzzer binary run
  three times, not on linux-gcc-debug, and not in the 5276-input working corpus. This is the host's known fault
  pattern (`host-misexecutes-local-faults`). Host-check leg 8601 covers the target on hosted runners.

## Next

S78's connection slices, in order: per-path Application recovery with path 0 alone, keeping every existing case
unchanged; path IDs, connection IDs and the lifecycle, proven against picoquic on two paths; then scheduling,
hostile-path tests and the fuzz oracles.
