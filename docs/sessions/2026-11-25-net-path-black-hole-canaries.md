# 2026-11-25: NET-PATH, black hole canaries and size-aware clearing

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Follows [the reordered train](2026-11-25-net-path-reordered-train-black-hole.md)
(Next 2) and [TP-17](2026-11-25-crypto-threaded-signing-tp17.md). User decision on record (2026-11-01): "I WANT NET
DONE PROPERLY AND GOLD STANDARD AND WITHOUT ANY DEFERS!"

**CI first:** nothing to fix. CI 45559790834 (65e76ee7) has no failed job (five still running); every push run since
was superseded before it started, and 45590080105 (e5d0a736, the first with 21794a44 and 07827fb6) is pending.

## What landed

- Trace of the 40 ms falls at HEAD: the span rule (21794a44) left a real fall's first suspicions within 20 ms of each
  other, and nothing after them was acknowledged until the next probe timeout. In the two-way fall a 1233-byte packet,
  which the 1300-byte path still carries, was acknowledged and cleared every 1500-byte suspicion.
- `QuicPathMtu` compares sizes: a suspicion keeps its smallest lost packet, and only an acknowledged packet at least as
  large, sent after it, clears it (also for a new detection's losses against the newest large acknowledgement).
- Canaries: while a suspicion is held, the connection caps the next ack-eliciting datagram to 1200 bytes after each
  three larger ones (`kQuicPmtuCanaryAfter`, RFC 9002's packet threshold), so their loss is declared a round trip later.
  The size in use and the controller's datagram unit stay. A lower ceiling drops a suspicion. 136 to 144 bytes.
- `test_quic_pmtu.cpp`: fall-to-found bounds in the four connection falls, two unit cases (size-aware clearing,
  canaries and what ends them). The pmtu fuzzer sends packets of any size and holds canaries to a raised size above
  the floor, after three larger packets; a `canary` seed.

## Evidence (linux-gcc-debug unless named)

- Fall to black hole, HEAD then now: CUBIC 192 to 104 ms, NewReno 195 to 104, the two-way fall 240 to 106, a fall
  before a flight (DATAGRAM) 464 to 240. Before the span rule CUBIC took 69 ms: the span's floor is the first
  suspicion plus a round trip, which is what the canary reaches.
- LEO handover sweeps (2 s period, seeds 0-3, six first handovers, 144 runs each): spikes of 30 ms for 150 ms and of
  80 ms for 100 ms, no `min_mtu` red in 288 runs. At 500 ms windows the case's other checks failed 15/12/14/23 times
  against HEAD's 16/18/12/25 (the known LEO weakness, NET-PATH's Remaining).
- Teeth: without the canary the falls take 192, 195, 119 and 464 ms and four bounds fail; with size-blind clearing
  the new unit case fails three checks.
- `crd-net-tests`: 1,206 cases, 1,198 passed and 8 skipped. libFuzzer: pmtu 2.33 M and
  connection 7 k executions, both clean; the corpus gains the `canary` seed.
- Interop: ngtcp2 fall 4 of 4, Cerid WebTransport fall 3 x 4 of 4. linux-gcc-release (LTO link clean), win-debug
  (MSVC): 41 PMTU and handover cases pass. 19 guard CTests and the corpus replay pass; LLVM 22.1.8 tidy is clean.

## Next

1. CI first: the first finished run containing 21794a44, 07827fb6 and this commit (win-asan's TP-17 gate lines).
2. The LEO case hardened on candidate A, then the single SipHash draw.
