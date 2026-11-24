# 2026-11-24: NET-PATH, the connection fuzzer's transfer bounds rounds, not time

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). CI first: fuzz host-check 45342658777 (f3fceaef) failed
`net-quic-connection` seed 8403 (container-overflow-off arm, input 55415b3c): the transfer oracle at its end
(neither all 5000 bytes with the FIN nor a closed end). The input fails on HEAD 5f977e19 too.

## Cause (traced)

- Over paths, the script left the client at address 1 on its way to the server's preferred socket, its RTT taught
  to minutes. Every server datagram goes to address 2, which the client's NAT left, so none arrives.
- The server's tuple (socket 1, address 1) validates with no peer connection ID. It holds one, in use, since it
  retired the client's sequence 1 at op 82. That RETIRE_CONNECTION_ID was lost, still in flight while the client was
  reachable (ops 127 to 139), declared lost at op 143 and resent while the client was elsewhere. The client never
  issued sequence 2. This is the exhaustion the [path design](../design/network-foundation.md#migration) accepts
  ("a move may find none unused").
- The client is flow-blocked at the 1024-byte stream window, not paced: the credit cannot reach it.
- The pair is live: with the round cap lifted, the client closes on its idle timeout at 9023 s, 12 rounds in. The
  transfer's 512 rounds of at most 5 s covered 2560 s and ended at 6077 s.

## What landed

- `transfer()`: a round in which nothing was written or read and neither end's counters moved (no datagram sent
  or taken) leaves only time to change the pair, so it goes to the next timer instead of 5 s at most; the 5 s cap
  stays for every round that moved. The bound is rounds, as `settle()`'s is events.
- Input 55415b3c joins `corpus-quic-connection`; it fails the oracle without the change.

## Evidence

- linux-clang-fuzz: the input passes; the 77-input corpus replays clean; libFuzzer seeds 8403 and 8404, 480 s each,
  34,247 runs, no find. linux-gcc-debug: `crd-fuzz-net-quic-connection` corpus CTest passes.

## Next

Watch host-check 45351689742 (5f977e19) and the public-checks qmux unidirectional-stream case in the next finished
run; then NET-PATH's Remaining.
