# 2026-11-08 — 4.2a: the connection's datagrams

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S41](../design/network-foundation.md#quic-datagrams).

## CI first

Nothing of this track failed: 42463343148 failed only at Preflight (actionlint download, HTTP 504 three times).
The relayed GPL-rewrite words belong to the other agent (operator note, 2026-11-07); this batch did not touch it.

## What landed (capability `network.transport`)

- `quic_datagram.hpp`: `QuicDatagrams`, RFC 9221 for a connection. max_datagram_frame_size is enforced both ways,
  with PROTOCOL_VIOLATION past the size this side advertised or with none advertised. The largest payload is fixed
  for the worst-case short header, so it never strands a queued datagram. Both rings are byte rings read in place.
  Datagrams can expire, and each outcome (Acked, Lost, Expired) is reported once, with `send_count` holding a
  datagram until its report is polled.
- `QuicConnection` carries them (`QuicConnectionConfig::datagrams`, `datagrams()`). A 1-RTT packet holds the
  connection's own frames, then datagrams, then stream data. An owed ACK goes first; an ACK only carried along goes
  after the datagrams, so a datagram of the largest size still fits. Outcomes are ledger records.
- Standards register: `quic.datagram` names its evidence so far.
- `crd-fuzz-net-quic-datagram` (3 seeds); `crd-fuzz-net-quic-connection` now carries datagrams after its stream.

## Evidence

- win-debug (MSVC): crd-net-tests 276 cases pass (14 new); linux-gcc-debug: 277 cases pass, with the
  `crd-no-*`/`crd-check-*` guards and every net corpus replay. LLVM 20 strict tidy clean on all 8 changed C++ files.
- 30 source mutations. The first pass left five survivors. Three became tests: the largest datagram beside an owed
  ACK needs a 20-byte connection ID, a DATAGRAM-only packet must count in flight, and a clean path acknowledges
  every datagram. Two were redundant code, now removed: a ring's empty head, and a size-0 check that the size check
  already covers. Final pass: 29 caught, 1 does not compile.
- linux-clang-fuzz: `net-quic-datagram` ran 543,396 executions in 230 s (corpus 3 to 353), then 285,457 more in
  180 s on the final code (to 534); `net-quic-connection` ran 47,619 executions in 230 s (4 to 871). No artifacts.
- Hostile paths (10% loss, 20 ms jitter): 1,812 Acked and 5,388 Lost reports, so most are reordering past RFC 9002's
  fixed thresholds.

## Next

- 4.2a: S37's reordering thresholds raised on a spurious loss (RFC 9002 section 6.1), then this side's connection
  IDs and stateless reset, Retry and NEW_TOKEN on the client, compatible version
  negotiation's switch, the 0-RTT buffer and 0-RTT (keeping the datagram size, RFC 9221 section 3), the connection
  behind the IngressHandler, interop with a pinned peer; then the TCP and TLS path and the browser endpoints.
