# 2026-11-24: NET-PATH, the in-order path touches no bitmap

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Remaining item: crd-net's bulk path brought to msquic's cost on the
board. Follow-up: the `udp-ring` host-check's refusals.

## Ring refusals (host-check 45288424193, alone; 45289266249, loaded)

Both runs landed on AWS (kernel 7.0.0-1014-aws, `ulimit -l` 8192 KiB, overcommit 0, no capabilities). No ring was
refused: 4, 8, 1024 and 32768 buffers opened 40 times alone and 600 times beside whole `crd-net-tests` runs on
every CPU. The failing CI 45238977369 job ran on Azure (westus3) after two hours of the full ASan suite. The AWS
hosts cannot reproduce it. On WSL (kernel 6.6), `ulimit -l 0` refuses nothing either: that kernel charges no ring
memory to `RLIMIT_MEMLOCK`. The cause is still unattributed.

## What landed (bulk path, profiled with perf on WSL)

- `WireWriter::put_bytes` copied byte by byte, with a bounds check per byte. That was 28% of the sending client's
  user time. It now checks once and does one `memcpy`. A range past the span still stops the process, now before
  anything is written (`crd-containers-fatal-wire-writer-overrun`).
- Stream bitmaps: the receive buffer's insert ran two bitmap scans and a counted mark (popcount, a libgcc call on
  the SSE2 floor) per packet. The send buffer's in-order acknowledgement did the same. Data in order now touches no
  bit. Below the contiguous (acknowledged) end everything is held, and bits are kept only past it. A watermark
  (`m_marked`, `m_acked_marked`) says when early bits may continue a run, or must be cleared before a wrapped offset
  reuses them. Marks whose count nobody reads no longer count.

## Evidence

- New cases: early runs joined, a changed byte refused by its bit and below the contiguous end, and wrapped offsets
  finding read or acknowledged bits clear, for both buffers.
- Teeth: each of the four watermark conditions forced false fails `[stream]` (23, 857, 84 and 87 assertions).
- linux-gcc-debug: `crd-net-tests` all 1,172 cases (7 skips), `crd-containers-tests`, `[mls]`, and the 19
  `crd-no-*`/`crd-check*` guards. linux-gcc-asan: `[stream]`, `[quic-connection]`, the containers suite and fatals.
- libFuzzer, 150 s each: quic-stream (725,775 runs), quic-stream-set (726,752), quic-connection (7,179). No find.
- clang-tidy 22.1.8 clean on the changed sources; braces pass.
- Bulk, 1 GiB over one connection, 4 interleaved runs of one tree before and after: user time fell from 0.89 to
  0.58 s on the server and from 1.47 to 0.89 s on the client. System time stayed 1.0-1.2 s; msquic's is 0.24 s.
- Board (`quic_board.py --rounds 3 --window 65536`, `linux-gcc-release`): crd-net 407 MiB per CPU-second (268
  before), msquic 969 (2.38 times, was 3.6). Handshakes: crd-net 3,622 per CPU-second, msquic 1,552.

## Next

- The remaining bulk gap is mostly in the kernel. crd-net spends 1.1 s of system time per GiB, msquic 0.24 s.
  Count datagrams per send and per receive call, and the receiver's ACK rate, before changing the trains.
- Ring: a resource refusal (ENOMEM or EAGAIN at Memory, Setup or Register) retries with half the buffers, down to
  the minimum, instead of falling back to `recvmmsg`. `SocketFeatures` reports the count it got.
