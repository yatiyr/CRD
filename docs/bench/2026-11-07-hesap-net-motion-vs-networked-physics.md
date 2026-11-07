# 2026-11-07 — HESAP-NET: snapshot interpolation against networked-physics

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net). Session: [Interpolation and rewind](../sessions/2026-11-07-hesap-net-motion.md).

## Machine and arms

Intel Core i9-14900K (AVX2, no AVX-512), WSL2 Ubuntu 24.04, one thread, GCC 13.3.0, the `linux-gcc-release` tree with
`-DCRD_BUILD_MOTION_VS_PEERS=ON`. The peer, without link-time optimisation: networked-physics-gdc-2015's
`InterpolateSnapshot_Linear` and `_Hermite` and vectorial's `nlerp` and `slerp`, cut verbatim, on stand-ins with the
arithmetic of vectorial's scalar backend (its SSE backend's normalise is a reciprocal square root estimate that differs by
CPU). `crd-hesap-motion-board --rounds 25`, run twice: 4,096 bodies between two ticks, the best of 25 interleaved rounds.
The board fails if a position or nlerp arm's bits differ from the peer's.

## Cost, nanoseconds per body

A body is its position and its orientation by slerp, as the peer's functions compute them.

| Arm | Linear + slerp | Hermite + slerp | nlerp | slerp |
|---|---|---|---|---|
| networked-physics (vectorial scalar) | 12.43 | 13.44 | 1.94 | 10.44 |
| hesap portable | 8.98 (1.38x) | 9.25 (1.45x) | 4.25 (0.46x) | 8.77 (1.19x) |
| hesap sse2 | 4.58 (2.71x) | 4.82 (2.79x) | 1.66 (1.17x) | 4.39 (2.38x) |
| hesap avx2 | 2.49 (4.98x) | 2.57 (5.23x) | 1.05 (1.85x) | 2.32 (4.50x) |

## Reading

- The only arm behind is the portable level's nlerp, a scalar build's, 2.2x behind because the lane form computes a
  square root and a division for all four lanes through arrays; no x64 or ARM64 build dispatches to it.
- Moving quaternions through `crd/math/simd/interleave4.hpp` instead of scalar planes took the AVX2 nlerp from 2.84 to
  1.05 ns: the planes stalled store forwarding. `hesap.quantize`'s encoder takes the same path.
- slerp costs less and is 880x more accurate: worst component error against the true arc in long double over 4,000
  rounds of 67 random pairs is 1.77e-7 for hesap and 1.56e-4 for the peer, whose nearly-parallel branch is a plain chord
  with a threshold of 0.001.
- The avx512 bodies pass their bit-for-bit arms under SDE; this machine cannot time them.
