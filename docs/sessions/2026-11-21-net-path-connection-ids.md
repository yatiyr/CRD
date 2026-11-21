# 2026-11-21 — NET-PATH: connection IDs per path ID

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath), its
"Connection IDs" bullet. Source: RFC 10069 sections 3.2, 4.4 and 4.5, RFC 9000 sections 5.1, 10.3 and 19.15 to 19.16.

## What landed

- `QuicCids` ([quic_connection_ids.hpp](../../engine/foundation/net/include/crd/net/quic_connection_ids.hpp)) owns
  both ends' connection IDs in one `QuicCidSpace` per path ID: path 0's inline, others opened on storage the caller
  owns (at most kQuicMaxPaths) and closed with their IDs unrouted and peer tokens unwatched. Each space has its own
  sequence numbers both ways, Retire Prior To and limit; IDs and reset tokens are unique across spaces.
  `find()` resolves a Destination Connection ID to its path ID and sequence.
- `QuicConnectionIds` and `QuicRoutedIds` moved there. The connection runs path 0 through `QuicCids`, which replaced
  its own arrays and the `PeerCid`/`LocalCid` code. Sent records carry the path ID (`stream_id`). The connection still
  negotiates no multipath.
- Defects fixed on the way:
  - A retirement is counted once per ID ("unless it has already done so", RFC 9000 section 19.15). Before, each
    repeated NEW_CONNECTION_ID below Retire Prior To added one, so frames repeated under loss could reach
    CONNECTION_ID_LIMIT_ERROR.
  - An ID this side retired after forgetting its path was held again when its frame repeated.
  - Learning a token again left the old one watched.
- Smaller fixes:
  - The connection's private `same_bytes` duplicate is gone; it uses `wire::wire_same_bytes`.
  - `test_quic_packet.cpp`'s `Buffer` is now heap-backed. The multipath case (8a886881) held about thirty 20 KB
    datagrams on the stack and overflowed MSVC's 1 MiB in win-debug.

## Evidence

- `[cids]`: 11 cases, 234 assertions. They cover spaces opened, refused and reused; per-space limits and Retire Prior
  To; cross-space conflicts; the retirement bound, loss and acknowledgement; a retired ID's repeat; issuing, rotating
  and retiring per space; and token watching.
- `test_quic_connection.cpp`: the retirement-bound case now uses distinct IDs, and a repeat sends no second
  retirement.
- Full `crd-net-tests` suites:
  - win-debug: 1072 cases.
  - linux-gcc-debug (WSL): 1073 cases.
  - Both pass, with 7 platform skips.
- `crd-fuzz-net-quic-cids` (new): a model of every space checks each verdict, what the host routes and watches, and
  owed frames. It has 4 seeds. On linux-clang-fuzz, 300 s with 2 jobs gave 1.7 M executions with no artifacts, growing
  the corpus from 4 to 2635 inputs, which all replay clean on win-debug.
- Teeth: without the retired-ID guard, both `[cids]` and the corpus replay fail.

## Next

The multipath lifecycle (S78): offer `initial_max_path_id`, read the PATH_ frames into `QuicCids`, and issue IDs per
unused path ID. A client opens a path; validation and PATH_ABANDON follow. The datagram's path ID then selects its
protection, recovery and control slot. Check `ConnectionIdTable` sizing in `QuicHost` for up to four times the IDs.
