# 2026-11-24: NET-PATH, the client's IDs ride its challenge to a preferred address

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S75](../design/network-foundation.md#migration). Follows
[the MTU hold](2026-11-24-net-path-pmtu-hold.md), whose first next item this is. Everything ran on this PC (WSL).

**CI first:** nothing to fix. CI 45358207939 (6c5c06fe) has no failed job; its win-asan job is still running.
CI 45389305498 (209b2468) has not started.

## Cause

The ngtcp2 preferred-address flake ("cerid client v1 ... 4x524288 preferred address" ending on the first address)
was an engine fault. Traced runs showed the client's PATH_CHALLENGE leaving 0.1 ms after confirmation, and ngtcp2's
server reading it on its preferred socket and never answering. The client issues its connection IDs once confirmed,
and they left in the same send but in the active path's packet, after the challenge. When the server read its
preferred socket first, it held no unused client ID. RFC 9000 section 9.5 forbids reusing one from a new address,
so ngtcp2 drops that challenge (`conn_write_path_response`). The retry comes one probe period later (more than
25 ms), and by then the 4x512 KiB echo had ended.

## What landed

- A packet carrying a PATH_CHALLENGE on a path that is not active also carries the local IDs owed. NEW_CONNECTION_ID
  and its PATH_ form are probing frames (section 9.1). `write_owed_ids()` is the one writer for both packets; a
  lost one is owed again by its record, as before.
- A `[preferred-address]` case loses every client datagram to the server's first address once the client is
  confirmed. The server holds one client ID then, and more than one when it first sends from its preferred socket;
  both ends move with no failed validation. Without the carry the case fails: the client never reaches the
  preferred address. The dead-preferred-socket case now also checks that the IDs its challenges carried still
  arrive on the first path.

## Evidence

- Before the fix: 6 of 40 failed (last batch), then 3 of 40, 1 of 30 and 2 of 40 here. After it, pinned to two
  cores: 150 of 150 runs pass, both cases each.
- linux-gcc-debug `crd-net-tests`: 896,397,993 assertions, all pass. `crd-net-quic-interop-ngtcp2`,
  `crd-net-http3-interop-nghttp3`, `-picoquic` (multipath included) and `-msquic` pass.
- linux-clang-fuzz: the `quic-connection` and `quic-cids` corpora pass. 300 s of libFuzzer at seed 24112026 ran
  clean (written to a scratch directory).
- linux-gcc-asan: every `[preferred-address]` case passes with no report.
- LLVM 22.1.8 clang-tidy is clean on the three touched files.

## Next

1. Memory per held connection: grow sent entries and frames with the flight, take held packets only while keys are
   missing, then rerun the board.
