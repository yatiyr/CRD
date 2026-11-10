# 2026-11-10 — NET-PATH: datagram PMTU discovery

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S68](../design/network-foundation.md#pmtu).

CI first: runs 42744641737 (45dbd882) and 42736485342 are still in progress. Every failed job read fails
`crd-repository-tools` and `crd-generated-sources` on `special_refs.inc`'s manifest, which 38b7a6dd records, or
`crd-check-registers` on `cerid.game`, which the congestion slice marked implemented. Nothing of this track is open. Run
42765697707 (001813c0), which holds the owed win-tidy and 114-case ngtcp2 reads, is still pending.

## What landed

- `quic_pmtu.hpp`/`.cpp`: `QuicPathMtu`, RFC 8899's search (Base, Searching, Search Complete, Error, Disabled). It
  probes the ceiling first, then bisects to the byte. A size fails after MAX_PROBES losses. PMTU_RAISE_TIMER restarts
  the search only while data waits. Black hole detection uses suspect loss detections, cleared by later large
  acknowledgements.
- `QuicConnection`: PING and PADDING probes, alone in their datagrams and inside the window and the pacer. An owed ACK
  goes first. `path_probe` keeps probe losses from the controller and from persistent congestion. Datagrams take the
  size found and keep to 1200 bytes while a probe timeout is pending. The controller (`set_max_datagram`) and the
  DATAGRAM limit (`set_path`, which drops oversized payloads) follow the size. New config: `base_datagram_bytes` and
  `path_mtu_discovery`. The default ceiling is 1472 (`kQuicDefaultMaxDatagram`).
- `UdpSocket` forbids fragmentation (`IP_PMTUDISC_PROBE` on both families; Windows falls back to the DF options) and
  reports `dont_fragment`. `QuicHost::build` takes the peer: an IPv6 peer gets base 1232, and a socket without DF gets
  no discovery.
- Tests: `test_quic_pmtu.cpp` (the search alone, then connections over fixed-, falling- and lossy-MTU paths), plus
  cases in `test_quic_client.cpp`, `test_udp.cpp` and `test_quic_datagram.cpp`. Fuzzing: `fuzz_quic_pmtu.cpp` (its
  corpus, `fuzz.py` and the tools test) and a probe oracle in `fuzz_quic_connection.cpp`. Two older packing tests are
  now pinned to 1200 bytes, the size whose room they test.
- Defects the work found and fixed: a probe used to go out ahead of an owed ACK, which stalled a keyed server's address
  validation. A probe size settled by another acknowledgement, or by a lower ceiling, was probed again; the fuzz oracle
  caught it.

## Evidence

- win-debug: every `[net]` test passes (661 cases, 79.4 million assertions), as do the corpus replays of the
  `quic-*`/`congestion` fuzzers. 5,000 random inputs to `crd-fuzz-net-quic-pmtu` replay clean.
- linux-gcc-debug (WSL mirror): the PMTU, UDP, datagram and connection tests pass (101 cases, 26.4 million
  assertions), as do the pmtu and connection corpus replays.

## Next

DPLPMTUD against real peers over an MTU-limited relay (ngtcp2, Quinn, quic-go, quiche), `max_udp_payload_size`
advertised as the loop's receive slot, and ICMP Packet Too Big hints. Then BBRv3.
