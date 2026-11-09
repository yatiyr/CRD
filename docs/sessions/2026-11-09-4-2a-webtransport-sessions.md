# 2026-11-09 — 4.2a: WebTransport sessions

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S59](../design/network-foundation.md#webtransport). Spec: draft-ietf-webtrans-http3-17 (SHA-256 `795eee1c...f426713`).

CI first: no completed run holds 3a696d2a or 879fc9a3 yet (42604792496 is still running), so tidy ran here on `http3.cpp` and `test_http3_webtransport.cpp`: clean.

## What landed

- `WebTransport` (`webtransport.hpp`): a fixed pool of sessions over `Http3Connection`, which it polls and passes on. `connect`, `accept`, `reject`, `close`, `drain`, `open_stream`, `write`, `reset`, `stop`, `peek`, `consume`, `send_datagram`, `export_keying_material`.
- Origin is checked before the application hears of a request, and refused with 403; an empty allowed list refuses every browser. The pool, one session without flow control, and the legacy count refuse with H3_REQUEST_REJECTED.
- WT-Available-Protocols and WT-Protocol as RFC 9651 strings, parameters ignored; a client's WT_ALPN_ERROR.
- Capsules on the CONNECT stream, split anywhere, none read before the 2xx. Streams and datagrams wait for their session within a bound, then WT_BUFFERED_STREAM_REJECTED or dropped oldest first.
- Flow control at both ends. Limits count bodies only, credit is granted as the application reads and closes, a reset gives back what it never sent, and a receiver counts a reset at its final size.
- Closing: WT_CLOSE_SESSION and FIN, every stream with WT_SESSION_GONE, a message cut at a character, and the peer's mistakes (too long, not UTF-8, data after the capsule, a FIN inside one).
- `QuicStreamSet::received_end`, `reset_size`, `stream_closed`, `QuicStreamEvent::stop_final`; `Http3Event::slot` and `unread`; `Http3Connection::wt_session_pending`.

## Found and fixed

- A STOP_SENDING overtaking the response or the close capsule ended the session with no status or code. S59 now says when one is ignored.
- A STOP_SENDING that lowered a reliable reset to nothing changed the final size without telling anyone. It now raises the event.
- A pool slot reused by a later session could be charged by an old stream's late events: a stream now forgets its session when the session ends.

## Evidence

- `crd-net-tests` on linux-gcc-debug: 641 cases pass, 22 of them new under `[webtransport]` in `test_webtransport.cpp`. `[webtransport],[http3]` on linux-gcc-asan passes.
- `crd-fuzz-net-webtransport` (new): 29,981 executions in 240 s on linux-clang-fuzz, corpus 3 to 640, no artifact. Three modes: hand-driven hostile peers of a server and of a client, both honest under tight flow control with the window coming back.
- Teeth, each failing its tests and passing once restored: ignoring the data limit, no give-back, Origin ignored, capsules read before the 2xx, headers counted, data after a close allowed, prohibited capsules skipped, no waiting bound.
- win-debug tidy on every new file and `http3.cpp`: clean.

## Next

WebTransport interop against `web-transport-quinn` and a -17 peer, then the session's channel surface. WebSocket and HTTP/2 follow.
