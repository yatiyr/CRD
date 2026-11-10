# 2026-11-10 — 4.2a: RFC 9220 interop

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S64](../design/network-foundation.md#websocket-tunnels).

CI first: run 42700242742 (e51e5d45) failed linux-gcc-release, -relwithdebinfo and -shipping at the LTO link of
`crd-net-tests` on `-Wnull-dereference` in `test_http2.cpp` lines 505, 671, 984, 1143 and 1148: every one a
`last()->` site 9b2b9de2 already replaced. Relinked on linux-gcc-relwithdebinfo here: clean. No later run has finished.

## What landed

- `crd-quic-peer --transport h3`: `connections` WebSocket tunnels at once on one HTTP/3 connection (`Http3Tunnels` over
  interop_qmux.hpp's `TunnelPeer`), then `h3_close`/`h3_close_code`; the client closes with H3_NO_ERROR once the
  server's GOAWAY says its tunnels are done. QMux's options and config moved into interop_qmux.hpp for both peers.
- `crd-qmux-crate-peer --transport h3`: the `h3` crate over Quinn 0.11.12, extended CONNECT once the server's SETTINGS
  allow it, tokio-tungstenite and the `qmux` crate on the request stream; the RFC 8441 policy now serves both versions.
- h3 and h3-quinn come from `packages/h3-crates`, the h3 repository's archive at 43745ad8 (SHA-256 pinned, downloaded
  twice): `Protocol::WEBSOCKET` is on its main branch only. `crd_add_cargo_peer(... PATCH crate dir)` passes
  `--config patch.crates-io`; check-pins accepts a sourceless locked crate only when the tool's `patched` names it at
  that version and `patched_from` names a pinned package (unit test with every refusal).
- `qmux_interop.py --quic`: the WebSocket cases over `h3`, two tunnels at once, and four runs through
  interop_common's UDP relay (5% loss, 5% duplication, 5 ms delay, 10 ms jitter).
- Found by the runs, fixed in crd-net: `WebSocketTunnels` took STOP_SENDING as the end of both directions, and Quinn
  sends the frame ahead of stream data it still owes. So a 400 sent complete after stopping the request (RFC 9114
  section 4.1.1) read as refused with no status, and, intermittently, a tunnel whose peer stopped reading once its
  Close had gone ended Reset with that Close still in flight. STOP_SENDING now ends this side's direction alone and
  the peer's is read to its end (RFC 9000 section 3.5): an opening request takes its answer (a refusal keeps its
  status, a 2xx is refused), an open tunnel reads what follows. Two new `[ws-tunnel]` cases, each failing with its half
  of the fix reverted.
- TP-14 registered: the qmux crate consumes a stream whose first frame is RESET_STREAM without handing it to
  `accept_uni()`; `crd-quic-peer`'s tunnel stream gets 2 MiB buffers, as the HTTP/2 path's crate windows do.
- `websocket.h3` implemented in the standards register.

## Evidence

- linux-gcc-public-checks (Debug, -Werror; Clippy `-D warnings` over the patched h3): CTest
  `crd-net-qmux-interop-crate`, 170 of 170 cases, and the 40 HTTP/3 cases three more times, all passing. Before the
  open-tunnel half of the fix, one full run lost two of them to the Reset above.
- linux-gcc-debug: `crd-net-tests [ws-tunnel]`, 29 cases, 3.16 million assertions. Strict clang-tidy 20 clean on
  websocket_tunnel.cpp and test_websocket_tunnel.cpp. linux-gcc-relwithdebinfo LTO link of `crd-net-tests` clean.

## Next

ALPN `h2` on the stream host (RFC 8441 over TLS), the last open item of 4.2a; then NET-PATH.
