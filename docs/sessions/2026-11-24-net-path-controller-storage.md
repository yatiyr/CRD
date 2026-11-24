# 2026-11-24: NET-PATH, a path's controller in storage of its size

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: the Memory bullet of
[the QUIC connection](../design/network-foundation.md#quic-connection). Follows
[the idle bursts](2026-11-24-net-path-idle-bursts.md), whose second next item this starts. Everything ran on this PC
(WSL2).

**CI first:** nothing of this track's to fix. CI 45399560511 (4f83e628) failed only linux-gcc-asan's `ceir 28b-2b`
tune-cache case, which the operator's db00a7e8 fixed. Fuzz host-check 45446014204 (b2ee3435) passed, seed 8702 too.

## What landed

- `ptype /o` of the 20,064-byte `QuicConnection`: each path control held a 2,240-byte slot sized for GCC and a
  1,488-byte handover learner slot, in slot 0's control and in each further slot's.
- `congestion_bytes(policy)` names the storage a policy's controller takes (NewReno 496, CUBIC 576, BBR 680,
  Prague 616, SCReAM 936, GCC 2,216 bytes), and `make_congestion` builds into any aligned storage that holds it.
- `QuicPathControl::reserve` takes that storage, and a learner only for a path that learns. `start()` allocates
  nothing and refuses a control without them. `set_congestion()` to a larger controller builds in new storage after
  the old controller leaves its coupling. `set_handover_learning()` takes the learner the first time, and it stays.
- `QuicConnection` reserves every slot's control at construction, inside its all-or-nothing check. A live
  `set_congestion()` reserves every slot first, open or not, and on any refusal gives back what waited and changes no
  path. `buffer_allocations()` counts what live changes take.

## Evidence

- Tests: the new `[path_control][memory]` case covers start without storage, every refusal, a swap to GCC and back,
  storage waiting for a swap or given back, and the learner taken once. The connection `[memory]` case refuses each
  of the four blocks of a swap to GCC on a multipath client, leaves CUBIC and the live blocks as they were, then opens
  path ID 1 on GCC with no allocation; a refused learner learns nothing. Tooth: no `unreserve()` on a refusal fails it.
- linux-gcc-debug: all 1,201 `crd-net-tests` cases pass (8 skipped, Windows only), and so do the 19 guard CTests
  (`crd-check-allocation-results` moved the recovery's allocation next to its check). The recovery, connection and
  congestion corpora replay clean. LLVM 22.1.8 clang-tidy is clean on the three engine sources and two tests touched. linux-gcc-asan: 266 path control, memory, multipath, coupled, connection,
  congestion and handover cases and the recovery corpus pass.
- Layout: `QuicConnection` is now 16,384 bytes, `QuicPathControl` 816 (it was 4,496).
- Memory board against 8bdfea79's engine, 3 rounds, KiB per held connection: server 29.0 against 32.1, client 29.4
  against 32.4, msquic 49.8 and 67.7. Handshakes per CPU-second, 5 rounds: median 4,534 against 4,539. Bulk at
  1 MiB windows, MiB per CPU-second over two runs (3 and 7 rounds): best 593 and 580 against 602 and 584, medians
  591 and 572 against 598 and 582 (1.2 and 1.7% lower, inside the 2% two-run rule; the per-ACK controller calls
  were already through a pointer).

## Next

1. CI: the first finished run containing this commit, on the MSVC, clang-cl, ARM64 and TSan lanes.
2. A held server end that moved data, below msquic's best rounds. What is left in the object: five 1-RTT packet keys
   (3.4 KiB), the TLS state (1.9), three transport parameter copies (1.1) and the stored configuration (0.8).
