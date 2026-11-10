# 2026-11-10 — NET-PATH: Prague settled, beside a second flow and beside CUBIC

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S70](../design/network-foundation.md#prague).

## CI first

No run containing this track's commits since c5a06d38 has finished: every push run since was replaced while pending,
and 42847701580 (52bc5780) is pending. The last finished runs were read in earlier batches. Nothing to fix.

## What landed

- Fixed: the bottleneck fixture's `crd::net::testing::Path` (ba5b22fd) and the QUIC fixture's `Path` were two types of
  one name, an ODR violation that fails linux-gcc-release's LTO link (`-Werror=odr`). The fixture's class is now
  `Flow`.
- `bottleneck_fixture.hpp`: the link (rate, buffer, CE threshold) is a `Link` that flows share; `run_flows` steps
  several flows in time order; a link can mark CE on a ramp (probability rising across `ce_ramp` bytes) as well as
  a step. One-flow behaviour is unchanged (the BBR and Prague cases pass as before).
- `test_prague.cpp`, three new cases and a settled half of the step case (all measured at 10 MB/s, two-BDP buffers).

## Evidence (win-debug `[net]` 703 cases; WSL linux-gcc-debug and linux-gcc-release `[prague],[bbr],[congestion],
[net-path]` 91 cases each)

- Settled (20 s), one flow: L4S steps of 1 and 2 ms keep 99.9% and 100% at 20 ms, 99.3% and 99.95% at 50 ms, the
  queue at the step and nothing dropped. The 93.5% recorded in S70 was measured from 2 s to 6 s, while Prague was
  still growing back from its slow start's exit (to a quarter of the BDP); settled, CUBIC keeps 88.8% and NewReno
  77.6% at the 1 ms step. Steps of 5 and 10 ms are classic depths: the detector reaches 1 and the link keeps 92.3%
  to 99.7% (ABE's 30% leaves a link short below 3/7 of the BDP).
- Two Prague flows at 20 ms through a 1 ms step: 5.04 and 4.90 MB/s, at most 12.4 KB queued, nothing dropped.
- RTT independence, 5 against 20 ms: through the step Prague splits 8.03 / 1.97 MB/s (alpha 2.5% / 9.4%: the
  shorter flow's marks lag less) where NewReno splits 7.41 / 0.48; through a ramp from 0.5 to 2.5 ms, 6.08 / 3.92.
- Beside CUBIC in one classic ECN queue (both 20 ms): at a 5 ms step Prague 5.79 and CUBIC 3.96 MB/s, at 15 ms
  6.47 and 3.52; the detector at 1 both times, within TR-BB-2019-002 section 8's 0.5 to 2 (a scalable answer
  alone reaches about 16). NewReno beside the same CUBIC: 4.64 / 4.71 and 4.96 / 5.02.

## Next

Prague against picoquic's Prague through the relay with an ECT(1) step. Then the delay-based controllers,
migration and the fast paths.
