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
- Observed on the way: a crd-net client closing as soon as its handshake completed left its server unestablished
  (ended after about 3 s); whether the server read the close in the Handshake space or only drained is still to be
  checked. Every board connection now ends with one empty exchange. A server pool of 256 refused a few connections
  without offloads while closing ones held their slots: the board's pool is 4,096 and its refusals fail the run.
  msquic connections started from msquic's own callbacks were left hanging (about 1 in 50); the board starts them on
  its main thread.

## Evidence

- Release board: crd-net 2.07 times msquic's handshakes per CPU-second with 64 KiB windows, msquic 1.71 times
  crd-net's with 1 MiB windows (crd-net's buffers made whole at connect); msquic 3.3 to 3.6 times crd-net's bytes per
  CPU-second; crd-net without offloads about 0.65 of itself on bytes. `crd-check-frame-resets` segfaulted once in
  the guard run and passed on its rerun (the host's known misexecution).
- `crd-net-quic-board-check`, `crd-net-quic-interop-msquic`, `crd-pins` and `crd-check-source-routes` pass on
  linux-gcc-public-checks.

## Next

crd-net's stream buffers grown on demand; the bulk gap measured with ksoftirqd's share, then profiled (perf is on
WSL) and closed; quiche's end of the board; added p99 latency under 1% loss and reordering (netem in `unshare -Urn`)
and memory per connection.
