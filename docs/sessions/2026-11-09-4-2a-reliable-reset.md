# 2026-11-09 — 4.2a: RESET_STREAM_AT

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S40](../design/network-foundation.md#quic-stream-set). Spec: draft-ietf-quic-reliable-stream-reset-11 (read in full, SHA-256 `3222008d...9c1e`), the transport feature WebTransport over HTTP/3 requires.

CI first: run 42589388858 (bea6e470), every failed job read. Nothing of this track is still open: `crd-repository-tools`, `qpack.cpp` narrowing and the link-time null dereference were fixed by e72ea284, f3ba9dc7 and 7cc32918; `airy.hpp` and `bessel.hpp` belong to hesap-special's owner.

## What landed

- Frame 0x24 (`QuicFrameKind::ResetStreamAt`), a Reliable Size past the Final Size refused; parameter 0x1d, empty; both in the register and its generated IDs. `quic.reliable-reset` is `implemented`.
- `QuicStreamSet::reset(id, error, reliable_size)`. The kept bytes are sent and resent as the data before a FIN is, what lies above is gone from the buffer, and the frame waits until the kept bytes have all been sent, so both credits cover its final size without a second check. The part ends when the frame with the current size and those bytes are acknowledged. A second call lowers the size; STOP_SENDING lowers it to zero.
- The receiver offers bytes up to the smallest size and only then shows the reset; `fin` is never set. A later, larger size is ignored; a changed code is STREAM_STATE_ERROR, a changed final size FINAL_SIZE_ERROR, an unadvertised frame FRAME_ENCODING_ERROR.
- 0-RTT: `reset_stream_at` is in the early context (format 2), and a server dropping it under a ticket that remembers it is refused. Refused for QMux (`in_order`), where it is no frame.

## Found and fixed

- A reliable size that stops inside a frame would have held the stream for ever, a hostile peer's cheap way to pin slots: `QuicStreamRead::reset_coming` says a reset follows, `give_up_reliable()` shows it now, and HTTP/3 (which gives the size no meaning) takes it at once.
- A reliable reset's bytes waiting on credit owed no STREAM_DATA_BLOCKED, so a peer that raises credit on that signal could deadlock the frame. They owe it now.
- `QuicStreamSet::on_all_acked()` (QMux) named the whole stream in a 32-bit length, so a stream past 4 GiB stopped being acknowledged and stalled. It now names only the unacknowledged rest; a 4 GiB case holds it.

## Evidence

- `crd-net-tests` on linux-gcc-debug: 592 cases pass. New: receiver refusals and reorderings, lowering before and after the frame went, frames deferred on credit, 32 seeds of loss, reorder and duplication, and real connections in both QUIC versions over 15% loss with jitter.
- `crd-fuzz-net-quic-stream-set`, reliable resets added: 688,526 executions in 151 s on linux-clang-fuzz, no artifact. `crd-fuzz-net-http3`: 16,586 in 121 s, no artifact.
- Teeth: settling the part on the frame's acknowledgement alone fails the hostile-exchange case (2,300 of 4,055 bytes delivered, no reset); restored, it passes.

## Next

WebTransport over HTTP/3 (draft -17) on `Http3Connection`.
