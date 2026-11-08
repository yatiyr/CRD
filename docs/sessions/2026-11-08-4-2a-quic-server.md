# 2026-11-08 — 4.2a: the server behind the ingress, and NET.3's hosted close

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [4.2a](../ROADMAP.md#slice-4.2a), [NET.3](../ROADMAP.md#slice-net.3). Design: [S46](../design/network-foundation.md#quic-server).
Sources: RFC 9000 sections 5.2.2, 7.2, 8.1.3, 9, 10.2.3 and 10.3, RFC 9001 section 4.1.4 (RFC Editor text). The
relayed words about the GPL rewrite belong to the other agent (operator note, 2026-11-07); this batch did not touch it.

CI first: dispatch 42473131224 (67168725) fails only crd-repository-tools (fixed in 39b6e1f5) and, on win-asan's
second shard and the ASan lanes, the ingress and packet test defects f3cd695a fixed. NET.3's audit is complete there:
crd-net-tests and every net corpus pass on win-debug (MSVC), and every NET.3 case and net corpus passes on win-asan's
second shard (its one net failure is that 4.2a ingress test). NET.3 closes.

## What landed (capability `network.transport`)

- `QuicServer` (quic_server.hpp): the IoLoop's handler. A fixed connection pool, admission with the client's ID routed
  for the connection's life, CONNECTION_REFUSED when full, NEW_TOKEN minted per connection, a min-heap of deadlines,
  slot reuse under generations, peer-address checks (migration stays NET-PATH's) and a peer's stateless reset read
  through a keyed address index.
- 0-RTT ahead of its Initial held in a bounded ring (share per client, eviction, expiry) and read after admission.
- `IngressAction::Early`, `Ingress::refuse` (one close writer with INVALID_TOKEN), `IngressHandler::on_early` and
  `on_unrouted`, and `SendQueue` (the loop's answer ring, now shared with the server).
- Test fixtures `loop_fixture.hpp` and `quic_connection_fixture.hpp` replace copies the new tests would have made.

## Evidence

- linux-gcc-asan: crd-net-tests 321 cases (21.1 million assertions); linux-clang-tsan: the server, loop, ingress and
  connection cases (85), the server's six times alike; linux-gcc-release and MSVC win-debug (320 cases) pass, the
  loopback case on both hosts.
- 16 source mutations, all caught: held 0-RTT unread, no peer check, no refusal, peer reset unread, no NEW_TOKEN,
  client route kept or never made, no share, no expiry, a busy host ignored, the wrong version, the heap unsifted, no
  eviction, the ingress dropping 0-RTT, the loop answering claimed resets, the refusal's error code.
- `crd-fuzz-net-ingress`: 1,183,483 executions in 174 s, clean; a 0-RTT seed added; the corpus replays under ASan.

## Next

- 4.2a: the client's host (its socket loop and connections), interop with a pinned peer implementation, the TCP and
  TLS path and the browser endpoints.
