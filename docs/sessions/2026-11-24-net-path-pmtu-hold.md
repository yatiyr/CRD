# 2026-11-24: NET-PATH, bulk interop cases held open for both path MTU searches

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S50](../design/network-foundation.md#quic-interop). Follows
[the CRYPTO buffers](2026-11-24-net-path-crypto-buffers.md), whose first next item this is. Everything ran on this PC.

**CI first:** nothing to fix. CI 45358207939 (6c5c06fe) has no failed job so far. All four `net-quic-connection`
seeds of host-check 45377043523 (096b091d) passed.

The UBSan report in `QuicPaths::credit` from the last batch is closed. It read "applying non-zero offset to non-null
pointer 0x7fffe37c9088 produced null pointer". That needs an index far outside the path table, and no caller passes
one. The input replayed clean 3 times in one process and 40 times across address layouts, and the hosted seeds
passed. By the host-misexecution lesson the item is closed.

## What landed

- **ngtcp2's 1200 on a 1280-byte path.** In a failing 4x4 MiB run, ngtcp2 sent 1406 at 2 to 56 ms and 1342 at
  137 to 190 ms. It gives a size up after three probes over five PTOs (`ngtcp2_pmtud.c`), so 1232 was due at about
  270 ms, but the faster bulk path ended the echo at 242 ms. The transfer size had stood in for time. Now the closer
  of every bulk case (`searches()`: no datagrams, no reset, 4 MiB or more) holds the connection 1 s
  (`--hold-ms`, new in `ngtcp2_quic_peer.c`; ngtcp2 keeps probing while held), and both checks stay exact.
- **Refused CRYPTO growth while data is owed.** A new case gets there through a HelloRetryRequest that the raw client
  never acknowledges (the server prefers secp384r1, the client shares X25519). Refused once, the Initial level sends
  what fits beside the request, and the next send grows it and sends the rest. The Handshake flight comes again on
  the probe timeout. Fixtures: `CountingAllocator::refuse_call`, per-end `Identity` groups, `RawRun::begin()`.

## Evidence

- Without the hold the four 1280 cases fail, and at 4x16 MiB (1.07 s) they pass. With it: 100 runs pinned to two
  cores (`taskset -c 0,1`), 0 failing.
- `crd-net-http3-interop-nghttp3` passes. `crd-net-quic-interop-ngtcp2`: 179 of 180 pass, every bulk case among
  them. The failure, "cerid client v1 ... 4x524288 preferred address" (no hold), ended on the server's first
  address. It fails 6 of 40 runs at 4x512 KiB and 0 of 25 at 4x4 MiB.
- The new case passes with and without the refusal, and the mutant that closes on every refused growth fails it.
  linux-gcc-debug `crd-net-tests` passes (896,397,832 assertions). linux-gcc-asan passes the four CRYPTO cases with
  no leak. LLVM 22 clang-tidy is clean on the test and fixtures.

## Next

1. The preferred-address flake: does a failing run send a PATH_CHALLENGE to the preferred port at all?
   `follow_preferred` runs once, at confirmation. If none is sent, the cause is there; if one is answered late,
   look at the validation or the peer's write rounds.
2. Grow sent entries and frames with the flight, take held packets only while keys are missing, and rerun the board.
