# 2026-11-09 — 4.2a: interoperability with ngtcp2

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S50](../design/network-foundation.md#quic-interop). Sources: RFC 9000,
RFC 9001 sections 6 and 6.6, RFC 9368, RFC 9369; ngtcp2 1.26.0's headers.

CI first: every finished failure of 42510394122 (5e685840) is already fixed: the two shipping lanes' version-change key
check and win-arm64's C4324 by 954d098a, win-debug-scalar's ntp checksum transient. The first tidy run over the four
ticket-cache files found one finding in the test (`ticket[0]`), fixed in 5964d66d.

## What landed

- `crd-quic-peer` (tests/foundation/net/interop/cerid_quic_peer.cpp) over `QuicServer` and `QuicClient`, and
  `crd-ngtcp2-quic-peer` over ngtcp2 and the pinned OpenSSL 4.0.3, real UDP on the loopback. ngtcp2 is built by its own
  CMake, libraries only (`CRD_BUILD_QUIC_VS_NGTCP2`, on `linux-gcc-public-checks`; `cmake/pins.json` carries the
  release's own archive). `openssl_self_signed.h` is the certificate helper both OpenSSL programs share.
- `quic_interop.py`: both ways round, every byte checked, each end's report against the other's, a relay of seeded
  impairments on the path, Retry packets counted off the wire.
- **Defect found by the first real peer**: `key_update_interval` closed a connection with AEAD_LIMIT_REACHED, because
  the key's hard limit was the interval and the update can only start once the phase is acknowledged. Real flights
  exceed a quarter of an interval; the virtual-path tests did not. An interval now starts updates only; the hard cap
  is `limit_sealed_packets` beside `limit_failed_opens`.

## Evidence

- `crd-net-quic-interop-ngtcp2`: 58 of 58 under CTest (69 s), and `crd-crypto-tls-interop-openssl` unchanged after the
  certificate helper moved. Before the fix the four "Cerid updates" cases failed with close code 15 after about 140
  packets, which is the fix's proof; the changed packet test covers the interval and the cap apart
  (`[quic-packet],[quic-connection]`: 63 cases, 10.5 million assertions).
- ngtcp2's `NGTCP2_ERR_RETRY` on a reordered first flight is its documented behaviour: the peer program drops the
  attempt and takes the client's next Initial.

## Next

- The remaining interop cases: datagrams (RFC 9221), resumption and 0-RTT, Version Negotiation and stateless reset; then
  the TCP and TLS path and the browser endpoints.
