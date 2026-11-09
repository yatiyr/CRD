# 2026-11-09 — 4.2a: WebTransport classification in HTTP/3

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S59](../design/network-foundation.md#webtransport). Spec: draft-ietf-webtrans-http3-17 (read in full, SHA-256 `795eee1c...7426713`).

CI first: no completed run holds 3a882670 or 48b6021c yet, so tidy ran here on `quic_frame.cpp`, `quic_stream.cpp`, `quic_stream_set.cpp` and `http3.cpp`, and found one naming issue in `test_quic_stream_set.cpp`, fixed.

## What landed

- `Http3WebTransportSettings` in SETTINGS and in `Http3Config`: SETTINGS_WT_ENABLED, the three initial limits, the legacy session count and 0xFFD277 read as 0x33. Never remembered and outside `early_context`. Without the offer every one is an unknown identifier.
- `Kind::First` and `Kind::WtHead` classify a stream by its first bytes, split anywhere: uni type 0x54, 0x41 first on a bidirectional stream. A session ID that is not a client's bidirectional stream is H3_ID_ERROR. 0x41 anywhere else is H3_FRAME_UNEXPECTED, read before the length it does not have.
- `WtStream`, `WtReadable`, `WtWritable`, `WtAborted` (with the final size), `WtStopped`; `open_wt_stream`, `reset_wt`, `stop_wt`, `abort_wt`, `wt_ready`; figure 4 as `wt_http_code` and `wt_application_code`.
- A server reads no bidirectional stream or WebTransport header before the client's SETTINGS (section 7.1) and takes them up when they arrive. Requests wait too, since QPACK can decode a section only once and the token is inside it.
- A client's session needs section 3.1's settings and parameters from this connection. A server treats a session without them as malformed, and refuses a CONNECT or a header from 0-RTT.
- GOAWAY's refusal now waits for the first bytes, so a session's streams above it survive.

## Found and fixed

- A datagram arriving before its request's HEADERS aborted that request with H3_DATAGRAM_ERROR. The refusal now waits until the request was read.
- A reset of the direction a peer opened asked for a reliable size naming a header that direction never carried, and QUIC refused it silently. Only the opener's direction keeps a header now.
- `copy_readable` said `ends` only at a final size; it now says so at a reliable size too, which is how a header cut by RESET_STREAM_AT is told from one still arriving.

## Evidence

- `crd-net-tests` on linux-gcc-debug: 618 cases pass, 26 of them new under `[webtransport]`. `[http3]` on linux-gcc-asan passes.
- `crd-fuzz-net-http3` gained modes 4 to 6 (hostile peers of both sides with WebTransport, and both honest with reliable resets): 29,937 executions in 220 s on linux-clang-fuzz, no artifact.
- Teeth: letting `advance()` give up reliable bytes on WebTransport streams and dropping the wait for SETTINGS each fail their tests; restored, they pass.

## Next

The session layer of S59 above this: the pool, capsules, flow control, buffering before a session is known, closing, the exporter, then interop.
