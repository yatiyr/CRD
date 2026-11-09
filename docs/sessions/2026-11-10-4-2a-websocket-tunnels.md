# 2026-11-10 — 4.2a: WebSocket tunnels over HTTP/2 and HTTP/3

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S64](../design/network-foundation.md#websocket-tunnels).

CI first: host-check 42682559508 (`net-hpack`, seed 9001) is audited: both rounds exit 0, 811,426 and 886,645 executions, no event. Every failed job of in-progress run 42670207777 (0ef76019) fails in a Build or Test step on the other owner's code (`airy.hpp`, `bessel.hpp`, `scalar_kernels.hpp`, `crd-no-untagged-physical-numeric`) or mikktspace; no test of this track failed in any of its 21 failed jobs. No completed run holds 7345d0a3 yet.

Found on the way: linux-gcc-release's LTO link refused `crd-net-tests` on seven `last()->` dereferences in `test_http2.cpp` (GCC's `-Wnull-dereference`), which no hosted run had reached yet; `last_of()` and `last_frame_of()` now require and return a reference (9b2b9de2).

## What landed

- `QmuxConfig::tunnel` and `QmuxConnection::start_tunnel()`: QMux's WebSocket message stage with no TLS of its own. A staging buffer from the allocator stands where TLS's output was, sized so `produce()`'s back-pressure test is unchanged; `taken()` lets the owner hand on what `outgoing()` showed without a copy. One `is_client()` replaces eight role lookups.
- `WebSocketTunnels` (`websocket_tunnel.hpp`): RFC 8441 and RFC 9220 over an `Http2Connection` or an `Http3Connection`, owner-run as WebTransport is: a fixed pool, Opened, Service, Refused and Closed, rounds of writes from a rotating start, every end mapped as the RFCs say.
- `ws_check_connect()` and `ws_read_connect_response()` in `websocket.hpp`: the HTTP/1.1 handshake's policy split out (`check_policy()`), so every HTTP version answers by the same rules; 400 with `sec-websocket-version: 13` for another version, 425 for a request in 0-RTT.
- `crd-fuzz-net-websocket-tunnel` (`net-websocket-tunnel`): a client honest in HTTP/2 or HTTP/3 and hostile above it, and two honest ends over a cut wire.

## Evidence

- `crd-net-tests [ws-tunnel]`: 26 cases, 3.2 million assertions, on linux-gcc-debug, linux-gcc-asan, linux-gcc-release (LTO) and win-debug (MSVC); the whole of `crd-net-tests` (793 cases) passes on linux-gcc-debug. Strict analysis clean. Both versions run every case: 200 KiB each way and datagrams at the limit with HTTP/2 cut at 1 and 7 bytes and nothing allocated once open, a reader that stops holding the writer by flow control, a `ChannelLink` reading a frame each way, 403, 404, 400 (version, none, no common subprotocol) and 425 never reaching the application, the pool's REFUSED_STREAM and H3_REQUEST_REJECTED, a client refusing 2xx without or with a foreign subprotocol, 403 and 302, the handshake timeout, a close as CONNECTION_CLOSE then Close then the stream's end, a reset and an end without Close, the idle timeout, fragments with a Ping between them (answered with its payload) and an unmasked frame failing with Close 1002 then END_STREAM, and a failed HTTP/2 connection ending its tunnels.
- Fuzz: three findings in the first seconds, all the harness's own and fixed there: an Opened tunnel may already be Closed by what arrived behind its CONNECT (the contract now says so), a stream opened before the peer's parameters, and a one-byte window making an honest transfer outlast the idle timeout (honest mode now stands time still). Then three sessions on linux-clang-fuzz, 20,550 executions, corpus 3 to about 680 units, no finding of the code: each ended in a sanitizer report that does not replay (30 runs and more under libFuzzer, and on linux-gcc-asan) at an unrelated site (a `std::span` built at address 0x1, a SEGV inside `Array::resize`'s ASan annotation, an `IAllocator` with an invalid vptr): the host misexecution MEMORY records, as the HPACK sessions saw. The 1,090 inputs the sessions kept replay clean on linux-gcc-asan and linux-gcc-release.

## Next

Interop of both tunnels against real peers (the `qmux` crate's WebSocket transport over hyper/h2 extended CONNECT and h3 over Quinn, pinned in the crate peer's lock), which flips `websocket.h2` and `websocket.h3`; then ALPN `h2` on the stream host.
