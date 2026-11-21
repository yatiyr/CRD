# 2026-11-21 — NET-PATH: QUIC address discovery

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S77](../design/network-foundation.md#address-discovery).

## CI first

No finished run contains e7d7c593..baac052b: every push run since 44895996127 was cancelled by a newer push, and
dispatched run 44895545159 (98617b60) is still queued. Nothing of this track's is failing.

## What landed

- draft-ietf-quic-address-discovery-01 (read in full) behind `QuicTransportParameters::address_discovery` (`Off`
  unless set): the parameter (0, 1, 2; any other TRANSPORT_PARAMETER_ERROR), kept in a server's early data context
  (format 3) and checked for equality by a client whose 0-RTT was accepted; the standards register entry
  implemented with its three codepoints.
- OBSERVED_ADDRESS (0x9f81a6/7): an ack-eliciting probing frame of the application space, PROTOCOL_VIOLATION when
  not asked for or out of place.
- Sending: each path owes one report from the moment it joins, sent in its first 1-RTT packet with a sequence that
  rises over the connection, recorded in the ledger so that a lost one is owed again on its own path alone. An
  IPv4-mapped peer goes as IPv4, and a scope never goes. Off the active path a report only rides with
  PATH_CHALLENGE or PATH_RESPONSE, so a spoofed replay draws nothing new (the draft's section 6.2).
- Receiving: the newest sequence per path is kept (`observed_address()`, `paths().observed()`,
  `address_observations`). The transport does not act on reports; NET-REACH judges them.
- `crd-picoquic-quic-peer` and `crd-quic-peer` take `--address-discovery` and report `observed`;
  `picoquic_interop.py` adds ten cases (both roles; both/both, receive/provide, provide/receive, both/off, off/both).
  picoquic is the one pinned peer that implements the draft (ngtcp2, quiche, Quinn and quic-go trees searched).

## Evidence

- win-debug: all 1039 crd-net-tests cases (7 skipped as before); linux-gcc-debug: all 1040. The five
  `[address-discovery]` cases, the frame and parameter cases and the new 0-RTT refusal pass. Two teeth (probing
  classification removed, a lost report never owed again) each failed the tests, then were restored and rebuilt.
- Fuzzing: codec and connection replays pass on both lanes. The connection target's bit 6 now runs every NET-STD
  draft, and a new oracle checks that each held report names a harness address. New seeds: OBSERVED_ADDRESS in the
  codec payloads and the full server parameters, and a paths script and a raw frame for the connection target.
  libFuzzer ran 270 s with four jobs per target (8.4 M and 116 k executions). It logged four events that no input
  replays: alone, over the 4195-unit working corpus, or in a 150 s single-job codec session. Two were in AWS-LC, as
  in earlier sessions on this host. They follow the host's known faults (`host-misexecutes-local-faults`), so
  host-check legs 8601 and 8602 are added for a hosted run.
- picoquic v2026.11.08: 10 of 10 address discovery cases pass. In each, both ends hold exactly the expected report
  (the peer's loopback view of their own port) or none.

## Next

Dispatch host-check (fuzz) for legs 8601/8602 and audit it. Then multipath QUIC (draft-ietf-quic-multipath-21)
behind its flag. Then the real-NIC cloud run (zero-copy send, Registered I/O, URO), held for one run with the
msquic and quiche budget boards.
