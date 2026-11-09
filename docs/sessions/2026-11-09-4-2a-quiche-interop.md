# 2026-11-09 — 4.2a: HTTP/3 interop with quiche

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S58](../design/network-foundation.md#http3-interop).

CI first: run 42589388858 (bea6e470) failed `crd-repository-tools` everywhere: `crd-fuzz-net-http3` had no `scripts/fuzz.py` entry, fixed in e72ea284. Its `qpack.cpp` narrowing was already fixed by f3ba9dc7. `airy.hpp` on win-debug belongs to hesap-special's owner.

## What landed

- `crd-quiche-peer` (`tests/foundation/net/interop/quiche_peer`): http3_exchange.h's exchange over quiche 0.30.0 and BoringSSL (boring 5.4.0), sans-I/O on one UDP socket; the Cargo.lock is the pin (`tools/quiche-crates`), built by `crd_add_cargo_peer`, clean under Clippy with `-D warnings`.
- `crd-net-http3-interop-quiche` (label `http3-interop`, `CRD_BUILD_HTTP3_VS_QUICHE`, on `linux-gcc-public-checks`); `http3_interop.py` takes `--nghttp3` or `--quiche`. No crd-net source changed.
- Register: `http.h3` names the second peer. The `quic.*` entries stay with the row.

## Evidence

- 54 of 54 cases, both ways round over real UDP, first run of the Cerid-client half: bodies from none to 8 MiB, 256 requests through 8 open streams, 12 KB sections, one section over the peer's limit (a Cerid client refuses it, a Cerid server answers 431 to quiche), trailers, 103, Extended CONNECT tunnels, RFC 9297 datagrams, GOAWAY in two steps, rejecting and from a client, cancels from either end, 0-RTT accepted and refused, and a path with 4% loss, 3% duplication and jitter that reorders. X25519MLKEM768 throughout.
- `crd-net-http3-interop-nghttp3` still passes through the shared driver (`linux-gcc-public-checks`).
- Teeth: Cerid's datagram quarter stream ID off by one fails all four datagram cases, quiche reading the wrong tunnel; restored, four pass.

## Not asked of quiche, and why

- QUIC version 2: quiche 0.30.0 implements version 1 alone.
- QPACK's dynamic table: its encoder and decoder have none. Cerid advertises a table, quiche advertises zero, and the driver holds Cerid to inserting nothing. nghttp3 keeps carrying the table cases.

## Found

- BoringSSL leaves Ed25519 out of what a client verifies by default, so a quiche client refused Cerid's Ed25519 identity with handshake_failure until the peer named it. Its default, not a defect.
- A refused 0-RTT, which reading quiche suggested a client could not survive, passes: the case stays in the run.

## Next

WebTransport sessions over `Http3Connection` (both `:protocol` spellings, the draft revision negotiated through NET-STD) and their mapping onto the channel model, with `web-transport-quinn` from the crate family already pinned as the first peer; then WebSocket over HTTP/1.1, HTTP/2 with HPACK and RFC 8441.
