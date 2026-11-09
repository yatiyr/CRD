# 2026-11-09 — 4.2a: HTTP/2 interop

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S63](../design/network-foundation.md#http2).

CI first: every failed job read in run 42670207777 (0ef76019) is the other owner's (`airy.hpp`, `bessel.hpp`, and `expint.hpp` under `crd-no-untagged-physical-numeric`). Nothing of this track is open. Host-check 42682559508 (`net-hpack`) is still queued, so its audit stays owed.

## What landed

- `crd-net-http2-interop` (label `http2-interop`, `CRD_BUILD_HTTP2_VS_PEERS`, on `linux-gcc-public-checks`): `crd-http2-peer` (Http2Connection over TcpPoller, prior knowledge) against `crd-nghttp2-peer` (nghttp2 1.71.0, the release tarball, its SHA-256 equal to the release's `checksums.txt`) and h2spec 2.6.0 (`cmake/pins.json`).
- `http3_exchange.h` is now `http_exchange.h` (`httpx_`): both HTTP versions run one exchange, one field check and one body pattern.
- `Chopper` in `interop_common.py`: a TCP path that passes the bytes on a few at a time at seeded boundaries.
- Register: `http.h2` implemented with this evidence.

## Found and fixed in `Http2Connection`

- h2spec 5.1/12: a frame other than WINDOW_UPDATE, RST_STREAM or PRIORITY on a stream both ends finished was a stream error; section 5.1 asks a connection error STREAM_CLOSED. The closed ring now keeps how each stream closed: this side's reset (dropped), the peer's reset (stream error), both finished (connection error).
- h2spec 6.3/2, a race seen in the third of the first runs: PRIORITY of the wrong size arriving after the response finished was dropped. Sections 6.3 and 6.9 make it, a self-dependency and a zero WINDOW_UPDATE stream errors whatever the state, so a closed stream answers RST_STREAM, charged to `resets`.

## Evidence

- **h2spec**: 147 of 147 with `--strict`, none skipped, eight runs in a row. `ACCEPTED` in the driver is empty: nothing is excused.
- **nghttp2**: 58 of 58, each way round: bodies from none to 8 MiB, 256 requests through 8 open streams, HPACK table 0, 256, 4096, 64 KiB (inserts counted off Cerid's tables), sections needing CONTINUATION and one over the limit (a Cerid client refuses it, a Cerid server answers 431), 1024-byte and 64 KiB stream windows, a 64 KiB connection window that binds, 1 MiB frames, nghttp2's padding, trailers, 103, Extended CONNECT tunnels, GOAWAY in two steps, rejecting and from a client, each end cancelling, and the chopping path at pieces of 48 and 7 bytes.
- **Teeth**: HPACK's dynamic index one off fails the table case both ways; the connection window left uncharged fails the case where it binds both ways (nghttp2 stops at its window: the other end never completes).
- `crd-net-tests [http2]` 41 cases, 1,638 assertions on linux-gcc-public-checks. `crd-net-http3-interop-nghttp3` still passes through the renamed exchange. Validators and strict analysis clean.

## Next

ALPN `h2` on the stream host, RFC 8441 and RFC 9220, which need QMux's message stage without its own TLS.
