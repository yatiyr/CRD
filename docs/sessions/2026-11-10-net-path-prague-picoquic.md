# 2026-11-10 — NET-PATH: Prague against picoquic's Prague through an L4S relay

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S70](../design/network-foundation.md#prague).

## CI first

Run 42811793243 (69f1ec57): win-arm64-debug passes, confirming the C4324 fix (1d964d5c). Its one failure,
linux-gcc-asan shard 1, is a SHA-256 mismatch on the download in `install-vulkan-validation.py`. Shard 2 passed with
the same pin, so the pin stays.

## What landed

- Pins: picoquic v2026.11.08.77fa502, picotls e446233b (the commit picoquic builds against) and picotest a99858e0
  (the submodule that picotls's commit archive leaves empty). `CRD_BUILD_QUIC_VS_PICOQUIC` is on in
  linux-gcc-public-checks. Each library builds with its own CMake, library targets only, against OpenSSL 4.0.
- `crd-picoquic-quic-peer`: the `cerid-interop` echo over picoquic's API and socket helpers, with picoquic's own
  Prague, CUBIC, NewReno and BBR. Each datagram carries the codepoint its controller asks for. The peer pins the
  Ed25519 key, checks the handshake signature, and reports the ECN counts each way.
- Relay `mark='keep'`: each datagram keeps its codepoint (IP_RECVTOS in, an IP_TOS cmsg out). The queue becomes an
  AQM: an ECT(1) step, a ramp, or a classic threshold for both ECT codepoints. Utilisation is measured after a
  settling time.
- `picoquic_interop.py` (CTest `crd-net-quic-interop-picoquic`, 24 cases, 145 s): both roles, v1 and v2, through
  2.5 MB/s, 30 ms, two-BDP bottlenecks. Each case uses one of: a 1 ms step, a 0.5 to 2.5 ms ramp, the step with 1%
  loss, or a 5 ms classic AQM.

## Evidence (WSL linux-gcc-public-checks)

- The relay change keeps every other interop CTest passing: ngtcp2 (156 cases), nghttp3, quiche and both
  WebTransport peers. `crd-check-source-routes` passes.
- Each end's CE count equals the relay's marks in every case. Cerid's ACK_ECN reports exactly the CE it read, and
  Cerid validates ECN on ECT(1).
- A Cerid client through the step or the ramp keeps 0.90 to 0.995 of the link, with a mean queue of 0.29 to 1.63 ms
  and nothing dropped. Behind the classic AQM, the queue stays near its depth.
- Oracle limits found, now written into the oracles:
  - The detector reads the round trip. picoquic's CUBIC, NewReno or BBR, queued in front of Cerid's
    acknowledgements, reads as a classic bottleneck, so the detector is held to 0 only beside picoquic's Prague.
  - Alone in a classic AQM, Prague's queue is too steady for a sure verdict (1 in 5 of 7 runs of 13 s). The
    detector matters beside a classic flow, and `test_prague.cpp` proves that case.
  - Utilisation measured from 1 s fell to 0.79 once (the slow-start exit). It is now measured from 2 s, with 4 MiB
    per stream.
- Teeth: counting a received CE as ECT(1) in `quic_recovery.cpp` fails the step cases.

## Next

The delay-based controllers shared with NET-LIVE. Then migration, NAT rebinding and LEO handover, the fast paths,
multipath, and the msquic and quiche boards.
