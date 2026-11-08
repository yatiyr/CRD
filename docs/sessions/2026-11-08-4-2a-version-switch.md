# 2026-11-08 — 4.2a: compatible version negotiation

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S44](../design/network-foundation.md#quic-version-switch). Sources:
RFC 9368 sections 2 to 4 and RFC 9369 sections 4 to 6, read from the RFC Editor's text. The relayed words about the
GPL rewrite belong to the other agent (operator note, 2026-11-07); this batch did not touch that work.

CI first: nothing from this track. Dispatch 42472298588 failed only on the fuzz.py entry 39b6e1f5 fixed, CPM's curl
error 56 and GitHub 502/503/504 downloads; schedule run 42468997382 lost actionlint to HTTP 504.

## What landed (capability `network.transport`)

- crd-crypto: `QuicHelloAnswer` (asked once from the select-certificate callback, before any secret) replaces a
  server's parameters and sets its version; `QuicHandshake::set_version()` binds the version into the session ID
  context, so a ticket resumes only in its version (RFC 9369 section 5).
- `QuicPacketProtection`: `expect_version()`, `change_version()`, the original Initial receive key kept until the
  Initial keys go, Handshake and 1-RTT keys in the negotiated version, 0-RTT in the original.
- `QuicConnection`: the server switches while the ClientHello is read; the client learns from the first Initial in
  the new version, is settled by CRYPTO data in the original one and closes on it after a switch; `version()`, and
  `give_new_token()` in place of `QuicAccept::new_token` (a token for the final version).
- Fixed: the client's check after Version Negotiation compares with the attempted version (RFC 9368's figure 1); a
  packet test sealed version 2 keys under version 1 headers. `quic.version-negotiation` and `quic.grease-bit` are
  implemented in the register.

## Evidence

- linux-gcc-asan: crd-net-tests (302 cases), crd-crypto-tests (308) and every net and crypto corpus pass: switches
  both ways on clean and 12 seeded lossy paths, three no-switch offers, two servers answering one flight, bad client
  parameters, a switch after a Retry, tokens and tickets of the negotiated version, and a 1-RTT key held to the key
  log's secret keyed apart.
- `crd-fuzz-net-quic-connection` gains a switch bit (byte 0, bit 3) and two seeds; libFuzzer ran 47274 executions in
  230 s (corpus 9 to 1051, peak RSS 454 MiB, no artifacts).
- 13 source mutations, all caught: the other Initial key dropped, Handshake or 0-RTT keys in the wrong version,
  either check fed the wrong version, tickets unbound (three ways), the chosen version not re-encoded, original
  CRYPTO accepted, a settled client switching, no switch, the attempted version ignored.
- Not yet run: the MSVC, clang-cl and ARM64 lanes (CI), and interop with a peer that switches versions.

## Next

- 4.2a: a bounded 0-RTT reorder buffer and 0-RTT; the connection behind the IngressHandler; interop with a pinned
  peer; the TCP and TLS path; the browser endpoints.
