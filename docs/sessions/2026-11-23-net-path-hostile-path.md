# 2026-11-23: NET-PATH, the hostile path and per-slot credit

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath), Proof
bullet. CI first: nothing of this track's failed (CI 45126393085 had no failed job; host-check 45153550994 queued).

## What landed

- Connection fuzzer: the server's amplification credit is checked per slot. Each client datagram records the path ID it
  left on and counts for the slot that reads it (that path ID's; the leading slot for a long header or an ID the server
  no longer routes, as `datagram_slot()` reads it; the IDs are 8 bytes on the keyed raw path). Every unproven 4-tuple of
  every open slot holds `credit() + sent <= 3 * received` at every delivery and send. The per-address sum over every
  slot, and the any-slot-validated excuse, are gone.
- Follow check: a failed validation is required only once the server opened a packet of the kept path ID from the
  client's current address, sent in the client's current epoch (since its last NAT rebinding or move; an older packet
  moves nothing, find 9976136a). libFuzzer find f370db2c: path ID 0 carried nothing after the rebinding (paths 1 and 2
  took its data, its datagrams were lost), its 72-entry storage was full of unacknowledged packets, so after
  `keep_one_path()` its PATH_ABANDONs wait, by design, for the probe timeout that makes room (RFC 9002 section 6.2.4).
  This is not an engine defect. The input is in the corpus.
- `Net` fixture: per-socket loss, copies and jitter (`socket_loss`, `socket_duplicate`, `socket_jitter_ns`), so one path
  ID's link is hostile while the others stay clean. No random draw is added while they are 0.
- New `[multipath]` case, run on 4 seeds. Only path ID 1 loses (8%), duplicates (6%) and jitters (12 ms). Its client
  socket is NAT-rebound mid-stream, to the next port or to a new address. The server follows in path ID 1's slot, sends
  there unvalidated within 3x what the client sent, and validates there. It keeps congestion state only after a port
  change. Path ID 0's 4-tuple, table, floor and controller stay untouched, and 192 KiB cross whole.

## Evidence

- Mutations, each caught, then restored and rebuilt:
  - Cross-slot crediting on receive: the committed corpus catches it. The old summed oracle caught it only through the
    3133-input libFuzzer corpus, never through the committed one.
  - `send()` reading the leading slot's credit: caught.
  - Port-only keep limited to slot 0: the new case fails its floor check.
  - Further slots never following: the new case times out.
  - The waiting-path skip reverted: bb4b5a5c still fails under the new follow condition.
- libFuzzer (linux-clang-fuzz, `-fork=2`), five runs; the three oracle finds are in the corpus:
  - 168 s: f370db2c above.
  - 362 s: a UBSan null `Tuples` access in the harness's own leading-slot loop, which now reads open slots only.
  - 345 s: 9976136a, a stale packet counted as heard (the epoch above).
  - 410 s: 3bbcc526, the keyed raw path's 8-byte IDs read as 16 (every datagram fell to the leading slot).
  - 436 s: 0736d765, which fails HEAD's fuzzer the same way (below).
- Cross-slot crediting is caught by 5 committed inputs after the ID-length fix.
- linux-gcc-debug: crd-net-tests 1155 cases (1148 passed, 7 skipped), every net corpus, and the `crd-no-|crd-check`
  guards. Replays: the committed corpus, 34 seeds and 3384 libFuzzer inputs. LLVM 22 tidy is clean on the three changed
  files; the validators pass.

## Next

- Find 0736d765 (`~/fz-slot5-art` in WSL; it fails on HEAD too, so it is not this batch's). The client NAT-rebinds and
  moves to the server's preferred address. The server validates the new address on socket 0, but its move to socket 1 is
  deferred, so its active path stays on the dead old address. With no ACKs, the client's storage fills, its
  PATH_RESPONSEs wait for its probe timeouts, the server's validation fails four times, and the client idles out. RFC
  9000 section 9.6.2: until the move completes, the server should send from its original socket to the client's
  validated current address.
- Then interop with picoquic (two paths, abandon, status, both roles), S78's last multipath item.
