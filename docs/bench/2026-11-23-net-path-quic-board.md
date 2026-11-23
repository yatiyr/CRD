# 2026-11-23 — NET-PATH: the QUIC transport budget board, crd-net against msquic

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Session: [QUIC board](../sessions/2026-11-23-net-path-quic-board.md).

## Machine and arms

Intel Core i9-14900K, WSL2 (kernel 6.6.87.2), Ubuntu 24.04, GCC 13.3.0, the `linux-gcc-release` tree (link-time
optimisation) with `-DCRD_BUILD_QUIC_VS_MSQUIC=ON`; msquic 2.6.4 Release over OpenSSL 4.0.3, as pinned. The loopback,
no impairment. `quic_board.py --rounds 5` (tests/foundation/net/interop): each arm is one implementation as both server
and client (`crd-quic-board`, `crd-msquic-quic-board`), QUIC v1, AES-128-GCM, secp256r1 (msquic over OpenSSL offers
nothing else first), the server's Ed25519 certificate pinned by the client, CUBIC, a 1 MiB stream window and a 16 MiB
connection window. Handshakes: 200 and 2,000 connections, 16 under way at once, each an empty stream with a FIN answered
by the server's FIN, then the client's close. Bytes: one connection, one stream of 64 MiB and of 1 GiB from a prefilled
buffer to a counting sink. Each cost is the slope between the two sizes of user plus system CPU (os.wait4) of both
processes; the table shows the best of five interleaved rounds and the median. "No offload": crd-net's sockets with
neither segmentation nor coalescing; msquic has no switch for them.

## Results (run 1)

| arm | handshakes per CPU-second (both ends) | median | server | client | per wall second | vs crd-net |
|---|---|---|---|---|---|---|
| crd-net | 1,016 | 995 | 2,084 | 1,982 | 1,264 | 1.00 |
| crd-net, no offload | 947 | 916 | 1,906 | 1,882 | 1,303 | 0.93 |
| msquic | 1,540 | 1,499 | 3,939 | 2,529 | 2,627 | 1.52 |

| arm | MiB per CPU-second (both ends) | median | server | client | MiB per wall second | vs crd-net |
|---|---|---|---|---|---|---|
| crd-net | 294 | 293 | 767 | 476 | 485 | 1.00 |
| crd-net, no offload | 186 | 185 | 485 | 303 | 311 | 0.63 |
| msquic | 969 | 941 | 2,152 | 1,763 | 1,833 | 3.30 |

## Reading

- msquic leads: 1.5 times crd-net's handshakes per CPU-second and 3.3 times its bytes per CPU-second, at both ends.
  The bulk gap is the transport's per-packet path (both sides use AES-NI GCM: AWS-LC and OpenSSL); the handshake gap
  is mostly the server (3,939 against 2,084). Neither is explained yet: profiling both and closing them is NET-PATH's
  next work.
- Segmentation and coalescing are worth 1.58 times on bytes and 1.07 times on handshakes to crd-net.
- msquic's wall rates come from its worker threads (one per core); crd-net runs one thread per process.
