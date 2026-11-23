# 2026-11-23: NET-PATH, the QUIC transport budget board against msquic

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S50](../design/network-foundation.md#quic-interop)'s msquic
paragraph. Board: [2026-11-23](../bench/2026-11-23-net-path-quic-board.md).

## CI first

No run containing 6da44263 has finished: CI 45252768037 (2cf72586) is still pending with no job started.

## What landed

- `quic_board.py`: each arm one implementation against itself in two processes over the loopback, equal settings
  checked from both ends' reports (connections, established, bytes, cipher, version, crd-net's group); handshakes and
  bytes per CPU-second as the slope between two sizes of both processes' CPU (os.wait4), best and median of interleaved
  rounds. `crd-net-quic-board-check` runs one short round of every arm.
- `cerid_quic_board.cpp` (`crd-quic-board`) and `msquic_quic_board.c` (`crd-msquic-quic-board`): many connections,
  `parallel` under way (closing ones not counted), a prefilled sender and a counting sink; crd-net with and without
  segmentation and coalescing.
- `msquic_common.h`: the credential, pin, version and address both msquic programs set up (out of the peer);
  `interop_echo.hpp` names groups and AEADs from one table each way (`secp256r1` added).
- The msquic block finds Threads and ZLIB itself: a tree with `CRD_BUILD_QUIC_VS_MSQUIC` alone failed to generate.
- Found on the way: a client that closes as soon as its handshake completes drops its unsent Finished (RFC 9000
  section 10.2), so the server never establishes; every board connection therefore ends with one empty exchange.
  msquic connections started from msquic's own callbacks were left hanging (about 1 in 50); the board starts them on
  its main thread.

## Evidence

- Release board, five rounds: msquic 1.52 times crd-net's handshakes per CPU-second and 3.30 times its bytes per
  CPU-second; crd-net without offloads 0.93 and 0.63 of itself.
- `crd-net-quic-board-check`, `crd-net-quic-interop-msquic`, `crd-pins` and `crd-check-source-routes` pass on
  linux-gcc-public-checks.

## Next

Profile crd-net's bulk and handshake paths against msquic's (perf is on WSL) and close both gaps; quiche's end of the
board; added p99 latency under 1% loss and reordering (netem in `unshare -Urn`) and memory per connection.
