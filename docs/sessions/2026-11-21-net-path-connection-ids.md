# 2026-11-21 — NET-PATH: connection IDs per path ID

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath), its
"Connection IDs" bullet. Source: RFC 10069 sections 3.2, 4.4 and 4.5, RFC 9000 sections 5.1, 10.3 and 19.15 to 19.16.

## What landed

- `QuicCids` ([quic_connection_ids.hpp](../../engine/foundation/net/include/crd/net/quic_connection_ids.hpp)) owns
  both ends' connection IDs in one `QuicCidSpace` per path ID: path 0's inline, others opened on caller storage (at
  most kQuicMaxPaths) and closed with their IDs unrouted and tokens unwatched. Each space has its own sequences both
  ways, Retire Prior To and limit; IDs and tokens are unique across spaces; `find()` names a Destination Connection
  ID's path ID and sequence. `QuicConnectionIds` and `QuicRoutedIds` moved there. The connection runs path 0 through
  it (its own arrays are gone); sent records carry the path ID. Multipath is still not negotiated.
- Defects fixed on the way:
  - A retirement is counted once per ID (RFC 9000 section 19.15, "unless it has already done so"). Before, repeated
    frames below Retire Prior To under loss could reach CONNECTION_ID_LIMIT_ERROR.
  - An ID retired here after its path was forgotten was held again when its frame repeated.
  - A token learnt again left the old one watched.
  - A refused frame now changes nothing. Before, a sweep or limit refusal could first retire the ID sent to, so a
    server's CONNECTION_CLOSE went to its own original ID.
  - The connection's duplicate `same_bytes` is gone (`wire::wire_same_bytes`).
  - `test_quic_packet.cpp`'s `Buffer` is heap-backed: the multipath case (8a886881) overflowed win-debug's 1 MiB stack.

## Evidence

- `[cids]`: 11 cases. The bound case in `test_quic_connection.cpp` uses distinct IDs, sends no second retirement for
  a repeat, and checks that the close reaches sequence 4.
- `crd-net-tests`: win-debug 1072 cases, linux-gcc-debug 1073, all passing with 7 platform skips. `tests/net/all` was
  rebuilt; all 33 net corpora pass on both lanes.
- `crd-fuzz-net-quic-cids` (new, 4 seeds) checks a model of every space: each verdict, what the host routes and
  watches, and owed frames. On linux-clang-fuzz with 2 jobs: 300 s, 1.7 M executions, no artifacts, corpus 4 to
  2635, all of it replaying clean on win-debug. After the verdict fix: 130 s, 853 k executions, corpus 4 to 2062.
- net-quic-connection with 2 jobs: 105 s, 24.7 k executions, corpus 41 to 1474.
- Teeth: without the retired-ID guard, both `[cids]` and the corpus replay fail.
- Four local events, none replaying:
  - cids: a span offset overflowing to 0x8, and a model-oracle stop.
  - connection: an offset producing null (`quic_packet.cpp:610`), and a span bound.
  - Each artifact ran 50 to 200 times under the sanitized binary and once under the replay binary, and the working
    corpus replays clean. This is the host's known fault pattern (`host-misexecutes-local-faults`).
  - New host-check legs 8701 (net-quic-cids) and 8702 (net-quic-connection) carry the hosted proof.

## Next

The multipath lifecycle (S78): offer `initial_max_path_id`, read the PATH_ frames into `QuicCids`, and issue IDs per
unused path ID. A client opens a path; validation and PATH_ABANDON follow. The datagram's path ID then selects its
protection, recovery and control slot. Check `ConnectionIdTable` sizing in `QuicHost` for four times the IDs.
