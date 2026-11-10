# 2026-11-10 — 4.2a: RFC 8441 over TLS, and the row's close

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S65](../design/network-foundation.md#h2-bearer).

CI first: no run containing this track's last commits has finished (42700242742 in progress, 42735839355 pending).

## What landed

- `qmux_interop.py` transport `h2`: WebSocket tunnels on HTTP/2 inside TLS with ALPN `h2`, against the `h2` crate over
  rustls (`h2c` is the prior-knowledge run, renamed). On the Cerid end the tunnels run on `QmuxHost`; a ws or h2
  server's one context lists QMux, http/1.1 and h2. The Cerid end reports how its HTTP/2 connection ended
  (`h2_alpn`, `h2_close`, `h2_close_code`, `h2_tls_error`), and the script checks it.
- Five crd-net defects this found, each fixed with a test that fails without the fix:
  - A GOAWAY and close_notify arriving together ended the connection as TlsFailed. The cause: HTTP/2 output was still
    flushed after this side had stopped sending. Test: `[qmux-h2]`, the case where GOAWAY comes with close_notify.
  - A tunnel was not served when its bytes left and its streams had room again. Test: `[ws-tunnel]`.
  - A tunnel event waiting for `poll()` did not make `next_deadline_ns()` 0, so the host waited for a timer.
    Test: `[ws-tunnel]`.
  - `QmuxHost::wake()` did not reach a tunnel (`WebSocketTunnels::wake()`, `QmuxConnection::wake_tunnel()`).
    Test: `[qmux-h2]`.
  - An HTTP/2 writer that blocked again inside its Writable event was told Writable again, forever (100% CPU). It now
    waits for the next room or credit. Test: `[http2]`.
- Test application (interop_echo.hpp): a stream is reset only after its bytes have left (QMux) or been acknowledged
  (QUIC). Waiting one turn was not enough: the reset could still go first.

## Evidence

- linux-gcc-public-checks: `qmux_interop.py`, 214 of 214 cases, the 44 h2-over-TLS cases among them. The QUIC, HTTP/3
  and WebTransport interop CTests: 5 of 5 pass. Clippy `-D warnings` is clean.
- linux-gcc-debug: `crd-net-tests [net]`, 612 cases (67.2 million assertions); fuzz corpus replays and `crd-check`
  guards, 45 of 45. Each of the five fixes, mutated, fails its test.
- linux-clang-fuzz, seeds 1 and 2, 80 s each, no artifacts: net-http2 (249k executions), net-qmux-host (10k),
  net-websocket-tunnel (11k).
- Strict clang-tidy 20 is clean on the 7 changed sources it can analyse. `cerid_qmux_peer.cpp` builds only on the Linux
  interop lane, where GCC `-Werror` compiled it clean.

## Close

4.2a is Done. WebTransport over HTTP/2 applies only once its draft settles. draft-ietf-webtrans-http2 is at -15:
active, not in last call. The `webtransport.h2` watch entry in the register (review 2026-12-15) carries that trigger.

## Next

NET-PATH. Then the audit of the dispatched complete-tier run.
