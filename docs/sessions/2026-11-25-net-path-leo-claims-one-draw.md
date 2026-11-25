# 2026-11-25: NET-PATH, LEO handover claims over many handovers, and one entropy draw a packet

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Follows [the canaries](2026-11-25-net-path-black-hole-canaries.md),
Next 2. User decision on record (2026-11-01): "I WANT NET DONE PROPERLY AND GOLD STANDARD AND WITHOUT ANY DEFERS!"
Ran on this PC (WSL2 linux-gcc-debug, -release, linux-clang-fuzz; MSVC win-debug).

**CI first:** nothing to fix. 45559790834 still runs win-asan shard 2; 45601823342 (1dbff857) is pending.

## What landed

- `test(net)` 02ea3dff: the 15 s LEO case keeps its kept-loss and MTU checks (off's too) and drops its window, byte
  and speed claims (two handovers, one seed). A new case over handovers every 2 s (500 ms windows, reductions net
  of undone ones) asserts per run what held in every sweep run below: without the schedule or 1 s off it, a cut
  after all handovers but one (BBR all); CUBIC with it at most one in two and 4 fewer than blind; at least ten
  kept losses a handover; MTU kept; over the case's two runs, aware at most half blind's cuts and BBR sooner than
  blind and off. No CUBIC speed claim: on a one-BDP queue it is chance (-248 to +286 ms).
- The learned schedule's stopping handovers now run to 20 s: with them ending at 12 s the lock came before the end
  in about half of 40 runs, and seed 3's lock by 8 s was luck (the single draw locked it at 12.97 s, keeping
  nothing). It is now asserted by 15 s.
  `LeoRun::release_ns` records the release, held to the 16-period history's floor of eleven quiet periods.
- A raw client offering grease_quic_bit, and a case that the packet in a skipped number's place clears its QUIC bit
  as often as any other (41 skips, 19 cleared; 3,031 packets, 1,516 cleared).
- A path control's spurious loss must undo its controller's reduction: removing that call failed none of 85
  congestion, recovery and handover cases.
- `perf(net)` 2cff089a: one SipHash draw a packet, bit 0 for the QUIC bit, bits 1-6 for the skip.

## Evidence

- Sweeps, release (debug gives identical lines): seeds 0-3, first handovers 3.0-4.9 s by 100 ms, blind, aware and
  off, CUBIC and BBR, HEAD and the single draw: 960 transfers. Minimum margins: CUBIC blind less aware cuts 6,
  aware at most 4 of 9-10; BBR aware cuts below blind's in all 160 (by 1 at least), sooner than blind in 159.
  Learner: 80 runs locked by 14 s, released 31.98 to 45.99 s.
- Teeth: without handover coverage the 2 s case fails 16 checks; with the bits correlated (`kSkipShift` 0) the
  draw case finds 0 of 45 skips cleared; without the undo call the path control case fails 2.
- `crd-net-tests`: 1,208 cases, 1,200 passed, 8 skipped (debug, release); changed cases pass on HEAD's engine and
  MSVC (same skip counts). 52 net CTests pass. libFuzzer connection: 18,096 runs clean.
- Board, interleaved with HEAD's build, five rounds: 660 against 661 MiB per CPU-second at 1 MiB windows, 494
  against 490 at 64 KiB: level within noise.

## Next

1. CI first: the first finished run containing 02ea3dff and 2cff089a (MSVC, clang-cl, ARM64, TSan), and win-asan's
   TP-17 gate lines.
2. Trace why a learned lock lingers 12 to 26 quiet periods after handovers stop (the history alone gives eleven),
   then fix it (likely a background-rate test for the keep, as the lock has).
3. Then `has_data` per datagram and the ACK rule on the bulk path.
