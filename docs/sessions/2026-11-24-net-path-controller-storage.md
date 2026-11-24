# 2026-11-24: NET-PATH, a path's controller in storage of its size

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: the Memory bullet of
[the QUIC connection](../design/network-foundation.md#quic-connection). Follows
[the idle bursts](2026-11-24-net-path-idle-bursts.md), whose second next item this starts. Ran on this PC (WSL2, MSVC).

**CI first:** nothing of this track's to fix. CI 45399560511 (4f83e628) failed only linux-gcc-asan's `ceir 28b-2b`
tune-cache case, which the operator's db00a7e8 fixed. Fuzz host-check 45446014204 (b2ee3435) passed, seed 8702 too.

## What landed

- Each path control of the 20,064-byte `QuicConnection` held a 2,240-byte slot sized for GCC and a 1,488-byte
  handover learner. `congestion_bytes(policy)` now names a controller's size (NewReno 496, CUBIC 576, BBR 680, Prague
  616, SCReAM 936, GCC 2,216 bytes), and `make_congestion` builds into any aligned storage that holds it.
- `QuicPathControl::reserve` takes that storage, and a learner only for a path that learns. `start()` allocates
  nothing. A swap to a larger controller builds in new storage once the old one has left its coupling; the first
  `set_handover_learning()` takes the learner, which stays.
- `QuicConnection` reserves every slot at construction inside its all-or-nothing check. A live `set_congestion()`
  reserves every slot first and, on a refusal, gives back what waited and changes no path. `buffer_allocations()`
  counts what live changes take.
- `crd-quic-peer`'s HTTP/3 turn takes the echo `serve()` found: Linux GCC release LTO flagged its second lookup as a
  null dereference (`-Werror=null-dereference`).

## Evidence

- New `[path_control][memory]` and connection `[memory]` cases: every refusal (each of a multipath client's four
  blocks for a swap to GCC leaves CUBIC and the live blocks as they were), path ID 1 opened on GCC with no
  allocation, and a refused learner. Tooth: no `unreserve()` on a refusal fails the connection case.
- linux-gcc-debug: 1,201 `crd-net-tests` cases and 19 guard CTests pass. linux-gcc-asan: 266 path control, memory,
  multipath, coupled, connection, congestion and handover cases. win-debug (MSVC): 40 path control, memory, coupled
  and congestion cases. libFuzzer: recovery 211,466, connection 9,671, congestion 292,413 runs, no artifact; corpora
  replay clean. LLVM 22.1.8 clang-tidy clean on the five sources touched. Interop with ngtcp2, nghttp3, picoquic and
  msquic and `quic_board.py --check` pass.
- `QuicConnection` is 16,384 bytes, `QuicPathControl` 816 (was 4,496). Memory board (no data moved), 3 rounds
  against 8bdfea79's engine: server 29.0 against 32.1 KiB per held connection, client 29.4 against 32.4; msquic
  49.8 and 67.7. Handshakes per CPU-second (5 rounds): median 4,534 against 4,539. Bulk, MiB per CPU-second: at
  64 KiB windows (5 rounds) median 432 against 435; at 1 MiB (3 and 7 rounds) medians 591 and 572 against 598 and
  582, 1.2 and 1.7% lower in both runs, inside the 2% two-run rule.

## Next

1. CI: the first finished run containing 01f2610f (MSVC, clang-cl, ARM64, TSan, linux-gcc-release's interop peers).
2. The 1 MiB bulk medians, low in both runs: one more interleaved board against 8bdfea79.
3. A held server that moved data, below msquic's best rounds. Left in the object: five 1-RTT packet keys (3.4 KiB), the
   TLS state (1.9), three transport parameter copies (1.1) and the stored configuration (0.8).
