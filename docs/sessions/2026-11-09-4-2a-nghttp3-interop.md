# 2026-11-09 — 4.2a: HTTP/3 interop with nghttp3

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S58](../design/network-foundation.md#http3-interop).

CI first: no completed run contains 413d62e8 or bea6e470 (42589388858 was still pending). Tidy and the sanitizers were owed on the HTTP/3 slice, so they ran here.

## What landed

- `crd-net-http3-interop-nghttp3` (label `http3-interop`): `crd-quic-peer --http3 1` against `crd-ngtcp2-quic-peer --http3 1`, nghttp3 1.19.0 (`cmake/pins.json`, the release tarball, because a commit archive leaves `lib/sfparse` empty), both ways round over real UDP. No new crd-net source: only `qpack_decoder()` and `qpack_encoder()` read accessors on `Http3Connection`.
- `http3_exchange.h`: the exchange's options and field checks, once, for both programs. `ngtcp2_http3.c` is nghttp3's side; `interop_http3.hpp` Cerid's. `Relay` moved to `interop_common.py`.
- Register: `http.h3` and `http.datagrams` implemented with this evidence.

## Evidence

- 66 of 66 cases: QUIC v1 and v2; bodies from none to 8 MiB; 256 requests through 8 open streams; QPACK off, on, 256 bytes (eviction), 64 KiB, blocked streams 0 and up, inserts counted off Cerid's table each way and acknowledged; 12 KB sections; one section over the peer's limit (a Cerid client refuses it, a Cerid server answers 431 to nghttp3); trailers; 103; Extended CONNECT tunnels; RFC 9297 datagrams on their tunnels; GOAWAY in two steps, rejecting requests (completed x 4 equals the GOAWAY's ID, completed plus rejected equals the requests) and from a client; cancels from either end; 0-RTT accepted and refused; a path with 4% loss, 3% duplication and jitter that reorders.
- Teeth: Base one off in `QpackEncoder::encode` fails the table cases both ways.
- `crd-net-quic-interop-ngtcp2` unchanged (the peer gained an application mode).
- Owed on the HTTP/3 slice: `[http3]` and `[qpack]` on linux-gcc-asan (120 cases) and `[http3]` on linux-clang-tsan (84), clean. Strict analysis on `http_message.hpp`, `http_capsule.hpp`, `http3.hpp` and `http3_fixture.hpp`: two naming findings in the fixture, fixed.
- linux-gcc-asan refused a narrowing `usize` to `u32` in `QpackTable::init`, invisible on the other lanes: fixed.

## Found

- The exchange's own: a tunnel's client FIN before its datagrams closed the send part they need; trailers on a tunnel (only DATA may follow). Both are correct refusals by Cerid and nghttp3.
- A QUIC-level close by ngtcp2 deletes nghttp3's stream with its blocked section; the C peer now defers the close to the message's end. Third-party behaviour, handled in the test program only and registered as [TP-13](../third-party-defects.md#tp-13).

## Next

A second HTTP/3 implementation (`quic.*` entries flip with the row, since that changes the crypto inventory's reviewed state), then WebTransport sessions and their mapping onto channels, WebSocket over HTTP/1.1, HTTP/2 with HPACK and RFC 8441.
