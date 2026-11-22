# 2026-11-23: NET-PATH, following a client whose address has no unused ID

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S75](../design/network-foundation.md#migration)'s "Following
the peer" and "The attacks" bullets.

CI first: nothing to fix. CI 45073010169 (f32832fd) had no job yet; fuzz host-check 45073088098 dispatched on f32832fd.

## What landed

- The follow-up from the last batch, reproduced first: a raw-client case failed on HEAD. The client moves to b with
  a new DCID; b's challenges are lost; the server reverts to a (RFC 9000 section 9.3.2) and forgot b, retiring the
  only ID it sent there. The client's next packet from b then found no unused ID and moved nothing (section 9.5 lets
  only a packet with the same DCID keep the current ID, so reusing a's ID was never an option).
- A server now keeps the address it reverted from, with the ID it sent there (`expire_paths`): the client's next packet
  from there moves it back on that packet, and the ID links nothing new. No longer validating, the path gives the ID
  up when another needs one (`address_paths`). A client still forgets its failed path, so its next move has an ID.
- A non-probing packet from an address without an unused ID records the move (`Tuples::waiting`) and starts the
  address's validation, its challenge waiting for the ID too. The packet that brings the ID completes the move
  (`follow_peer`), unless a later non-probing packet chose another path or the path was forgotten (its slot may hold
  another address by then). Before, the move needed a further non-probing packet after the ID.
- The connection fuzzer's follow oracle: once the client pinged, a packet the server opened that left it neither
  following nor validating fails, unless another validation failed since.

## Evidence

- `test_quic_connection.cpp`: two new cases (the revert kept and moved back on one packet, every datagram to b on
  b's ID; the waiting move completed, cancelled by a later packet, and cancelled by b's expiry with a probe from c
  taking its slot), and the revert case now checks b kept, unvalidated and idle. HEAD failed 3 of them (8 assertions).
- Six source mutations, each rebuilt alone and each caught: the revert dropping the path, no waiting recorded, waiting
  never reset, drop_path leaving waiting set, the waiting path not validated, the waiting move never made.
- linux-gcc-debug (WSL): 49 CTests pass (`crd-net-tests`, every `crd-check|crd-no-|crd-fuzz-net` guard).
  linux-gcc-release (LTO) `[net-path],[multipath],[quic-connection]` and linux-gcc-asan `QUIC paths*,[multipath]` pass.
- `crd-fuzz-net-quic-connection` (linux-clang-fuzz, 2 jobs) ran 300 s: 81334 executions, no artifact; the 44
  committed and 1616 scratch inputs replay clean under the tightened oracle.
- Pinned clang-tidy 22.1.8 clean on the three touched sources; clang-format 20 findings fixed on added lines only.

## Next

NET-PATH's scheduling: streams and datagram flows pinned to a path (`QuicDatagramSend` takes the pin), an idle path's
RTT kept fresh, PATH_ACK placement; then the fuzzers opening path IDs.
