# 2026-11-08 — 4.2a: the client's host

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S47](../design/network-foundation.md#quic-client). Sources: RFC 9000
sections 5.2.1, 6.2, 7.2, 8.1.3 and 10.3, RFC 9368 sections 2.1 and 4, RFC 9369 section 5 (RFC Editor text). The
relayed words about the GPL rewrite belong to the other agent (operator note, 2026-11-07); this batch did not touch it.

CI first: no completed run contains dd6139f1 yet. Two guards this track broke, found locally: crd-check-allocation-results
(S45 pushed `recovery`'s check out of its window) and crd-check-registers (NET.3's flip left cerid.session-control,
cerid.agent and cerid.service planned: session control is implemented, agent is AGENT.3's, service NET-SERVER's).

## What landed (capability `network.transport`)

- `QuicHost` (quic_host.hpp): the pool, handles, peer-address routing, deadline heap, send queue, peer reset index
  and close, taken out of `QuicServer`, which now derives from it; one spare connection cell lets a successor be built
  while its predecessor stands.
- A client's `Ingress` (S1 and S2 alone, answers nothing) and `QuicClient` (quic_client.hpp): connect() with a
  per-connection TLS context, IDs routed through the loop's table, a NEW_TOKEN table per server and version used once,
  and Version Negotiation followed once under the same handle (`on_restart`).
- The loop offers a short header too short to protect to `on_unrouted` (a 21-byte stateless reset), on both hosts.
- `quic_host_fixture.hpp`: the echo application and a server stack shared by the server and client tests.

## Evidence

- MSVC win-debug: crd-net-tests 328 cases and the crd-no-/crd-check guards. linux-gcc-asan: 329 cases (28.0 million
  assertions) and the ingress corpus; linux-clang-tsan: the host, loop and ingress cases (44) three times;
  linux-gcc-release (LTO, -Wnull-dereference) 329 cases, after the test's dereferences went through a checked helper.
- 18 source mutations: 17 caught (two as hangs); the peer compare in `on_unrouted` survived, so the peer key now
  holds the IPv6 zone and equals `Endpoint`'s equality, and the compare went. Tiny resets (9 and 20 bytes), a reset
  after a Version Negotiation restart and a client-ingress unit case were added for the others.
- `crd-fuzz-net-ingress` with the client leg: 3 new seeds, the corpus replays. 8.1 million libFuzzer executions gave
  four crashes (TLSF metadata and a stack span corrupted, a wild jump) and a run with the client leg removed three
  more; none replays. That is this host's known misexecution
  ([control](2026-10-11-diag-10a-container-overflow-control.md)), so two hosted legs (net-ingress, seeds 8301 and
  8302) join host-check.yml. test-repository-tools.py met it too (a regex-engine internal error, then a CPython
  segfault); the third run passed.

## Next

- Audit the hosted host-check dispatch and the first CI run containing this batch.
- 4.2a: a reset index by token (every connection of a peer address), a bounded ticket store, interop with a pinned
  peer implementation, the TCP and TLS path and the browser endpoints.
