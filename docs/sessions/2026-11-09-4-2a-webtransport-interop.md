# 2026-11-09 — 4.2a: WebTransport interop with web-transport-quinn

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S59](../design/network-foundation.md#webtransport).

CI first: run 42604792496 (7cc32918) is still in progress. Its failed jobs fail in hesap-special (`airy.hpp`, `bessel.hpp`) or at MSVC link time in math (`scalar_kernels.hpp` C2220 under LTCG); neither is this track's.

## What landed

- `crd-webtransport-quinn-peer` (`tests/foundation/net/interop/webtransport_peer`): web-transport-quinn 0.13.6 over Quinn 0.11.12 and rustls; the Cargo.lock is the pin (`tools/web-transport-quinn-crates`), built by `crd_add_cargo_peer`, clean under Clippy.
- `crd-quic-peer --webtransport legacy|h3` over `WebTransport` (`interop_webtransport.hpp`), the exchange defined there: every stream labelled, echoes, unidirectional streams, resets and STOP_SENDING with application codes, datagrams, a control stream so nothing closes under the other end, WT_CLOSE_SESSION.
- `crd-net-webtransport-interop-quinn` (label `webtransport-interop`, `CRD_BUILD_WEBTRANSPORT_VS_QUINN`, on `linux-gcc-public-checks`); `webtransport_interop.py` also runs Cerid against itself in both revisions, with flow control on `h3`.

## Found and fixed

A QUIC defect, found by Cerid against Cerid at 8 streams of 32 KiB: two ends that fill their sent packet storage (`sent_packets`) with data before any ACK arrives stall until idle timeout. A full space refused ACK-only packets and probes as well, so neither end could free the other's entries. `QuicRecovery::can_send_data` and `QuicRecoveryConfig::reserved_entries` now keep eight entries past `sent_packets` for ACK-only packets and probes; data stops at `sent_packets` as before. Quinn's pacing hid it; Cerid to Cerid over the loopback does not.

## Evidence

- `crd-net-webtransport-interop-quinn`: 82 of 82 cases on linux-gcc-public-checks in 86 s, each both ways round over real UDP where the shape allows: streams with no body, 16x64 KiB, 4x3 MiB, 16 unidirectional, 64 datagrams, 8 resets, 8 stops, a mix closed by the server, protocol negotiation, Origin read and refused with 403, 403 and 404 from a server, 4% loss with 3% duplication and jitter, against Quinn (legacy) and Cerid (legacy and `h3`, the latter with flow control under a 64 KiB and a 128 KiB window).
- `crd-net-tests` on that lane: 645 cases pass; four are new (`test_quic_connection.cpp` full storage, `test_quic_recovery.cpp` the reserve). `crd-net-quic-interop-ngtcp2`, `crd-net-http3-interop-nghttp3`, `crd-net-http3-interop-quiche` and `crd-net-qmux-interop-crate` pass after the transport change.
- Teeth, each failing and passing once restored: no reserve (the raw-peer and the probe cases fail); `kWtApplicationErrorFirst` off by one (every Quinn case with a reset or a stop fails).

## Not asked of web-transport-quinn, and why

`webtransport-h3` and flow control (it speaks only the legacy spelling), RESET_STREAM_AT (Quinn 0.11 has none), the exporter (none offered), 0-RTT (no session opens in it). Its `Request::reject` drops the HTTP/3 control stream with the request, which Cerid correctly closes for (H3_CLOSED_CRITICAL_STREAM); the peer holds the session instead.

## Next

A real `webtransport-h3` peer: webtransport-go v0.13.0 (quic-go 0.62, draft -16). Its SETTINGS_WT_ENABLED is 0x2c7cf000, and -16 to -17 changed only that codepoint on the wire, so Cerid sends both as section 7.1 says and a server reads a client's. That needs a pinned Go toolchain (`tools/go-linux-x64`, `scripts/install-go.py`). Then the session's channel surface, WebSocket over HTTP/1.1, HTTP/2 with HPACK and RFC 8441.
