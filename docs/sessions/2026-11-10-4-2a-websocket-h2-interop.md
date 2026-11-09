# 2026-11-10 — 4.2a: RFC 8441 interop

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S64](../design/network-foundation.md#websocket-tunnels).

CI first: no run containing 9b2b9de2 or 8cfa1ce8 has completed (42700242742 queued, 42713546395 pending), so the
linux-gcc-release LTO partition read and the hosted interop reads owed since the last batch stay owed.

## What landed

- `crd-qmux-peer --transport tls|ws|h2` (was `--websocket 1`): `h2` runs `connections` WebSocket tunnels at once on one
  HTTP/2 connection with prior knowledge, each running interop_echo.hpp's application. `interop_qmux.hpp` holds the
  QMux report both paths print and `TunnelPeer`, which drives `WebSocketTunnels` over either HTTP version, ready for
  the HTTP/3 tunnels in `crd-quic-peer`.
- `crd-qmux-crate-peer --transport h2`: the `h2` crate 0.4.20 (extended CONNECT, SETTINGS_ENABLE_CONNECT_PROTOCOL
  awaited before the first CONNECT), tokio-tungstenite on the stream through a pipe that hands credit back as bytes are
  taken, and the `qmux` crate's `ws::Upgraded` above it; a hand-written RFC 8441 policy on the server (method,
  `:protocol`, path, version 13, a subprotocol it takes, else 400). Cargo.lock adds h2, tokio-util, fnv and
  atomic-waker, every one from crates.io with its SHA-256.
- `qmux_interop.py`: a `transport` and a `tunnels` setting; the WebSocket case list runs over `ws` and `h2`, plus two
  tunnels at once (plain, with 8x1 MiB and 64 datagrams each, and through the cutting relay). Reports are paired by
  their `connection=N`, since tunnels end in any order; a tunnel crd-net's server refused leaves no report, by design.
- `websocket.h2` implemented in the standards register.

## Evidence

- linux-gcc-public-checks (Debug, -Werror), Clippy `-D warnings`: `qmux_interop.py`, 130 of 130 cases (65 each way
  round: QMux over TLS, over WebSocket over HTTP/1.1, and 42 over HTTP/2 tunnels), and the h2 subset twice more.
- Found on the way, both in the crate peer, fixed there: its server waited for the client to hang up, which a silent
  path never does, so it now ends with its tunnels and flushes GOAWAY; and with the `h2` crate's default 65,535-byte
  windows a reset stream's data sat behind 256 KiB of other data until its RESET_STREAM left alone. crd-net may do
  that (RFC 9000 section 19.4, final size 0), but the `qmux` crate opens such a stream without handing it to
  `accept_uni()` (session.rs, "RESET_STREAM can be the first frame"), so its reader waited forever. The peer now runs
  with hyper's windows (2 MiB per stream, 5 MiB per connection), as the TLS and HTTP/1.1 paths' unbounded windows
  already behaved. Third-party behaviour, left be.

## Next

RFC 9220 against a second HTTP/3 implementation: the `h3` crate's `Protocol::WEBSOCKET` is on its main branch only
(merged 2026-01-04, no release since 0.0.8), so it comes in as a SHA-256-pinned source archive patched over the
registry crate, which needs check-pins.py to accept that one shape; then `crd-quic-peer --transport h3` on
`TunnelPeer`, flipping `websocket.h3`. Then ALPN `h2` on the stream host (RFC 8441 inside TLS).
