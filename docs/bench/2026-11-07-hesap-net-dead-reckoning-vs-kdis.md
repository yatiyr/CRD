# 2026-11-07 — HESAP-NET: IEEE 1278.1 dead reckoning against KDIS

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net). Session: [Dead reckoning](../sessions/2026-11-07-hesap-net-dead-reckoning.md).

## Machine and arms

Intel Core i9-14900K (AVX2, no AVX-512), WSL2 Ubuntu 24.04, one thread, GCC 13.3.0, the `linux-gcc-release` tree with
`-DCRD_BUILD_MOTION_VS_PEERS=ON`. The peer, without link-time optimisation: KDIS at 97f8e86a, `DeadReckoningCalculator`,
`Reset` once per entity and `RunAlgorithm` per frame. `crd-hesap-dead-reckoning-board --rounds 25`, run twice: 4,096 entities
with their own dt of 16 to 516 ms and rates up to 2 rad/s per axis, the best of 25 interleaved rounds. The board fails if an
arm's position is more than 0.02 from the peer's, on positions up to 1000.

## Cost, nanoseconds per entity

The ratio is against KDIS in the same run.

| Algorithm | KDIS | hesap portable | hesap sse2 | hesap avx2 |
|---|---|---|---|---|
| FPW | 8.11 to 8.88 | 0.91 (8.9x to 9.8x) | 0.86 (9.4x to 10.3x) | 1.10 to 1.12 (7.4x to 7.9x) |
| RPW | 18.6 | 8.94 to 9.07 (2.1x) | 6.74 to 6.87 (2.7x) | 4.60 to 4.63 (4.0x) |
| RVW | 23.5 to 23.8 | 9.71 to 9.73 (2.4x) | 7.81 to 7.85 (3.0x) | 4.87 to 4.95 (4.8x) |
| FVW | 15.5 to 16.0 | 1.90 (8.1x to 8.4x) | 1.83 to 1.90 (8.4x) | 1.68 to 1.73 (8.9x to 9.5x) |
| RPB | 75.9 | 15.1 to 15.2 (5.0x) | 12.4 to 12.5 (6.1x) | 6.99 to 7.04 (10.8x) |
| RVB | 143.5 to 144.2 | 22.2 (6.5x) | 17.6 to 17.7 (8.2x) | 9.43 to 9.45 (15.2x) |

## Precision

`crd-hesap-dead-reckoning-peer-tests`. Worst gap in R1 v + R2 a against the standard's closed forms in long double, relative
to the displacement:

| Turn per span | hesap | The closed forms in double (Open-DIS's expressions) |
|---|---|---|
| 100 | 3.1e-14 | 3.0e-14 |
| 10 | 1.6e-15 | 1.4e-15 |
| 3 | 6.1e-16 | 5.9e-16 |
| 1.001 | 4.2e-16 | 3.4e-16 |
| 0.999 | 4.1e-16 | 2.5e-16 |
| 0.5 | 5.1e-16 | 3.9e-16 |
| 0.2 | 4.2e-16 | 2.2e-15 |
| 0.1 | 5.4e-16 | 4.7e-15 |

Below 0.1 long double's closed forms lose too much to be a reference; there `crd-hesap-net-tests` integrates the true rotation
instead, and holds every turn from 1e-300 to 400 within 1e-12.

Against KDIS, mean and worst gap per distance moved: 2.2e-8 and 5.1e-8 to 6.2e-8 on the world algorithms, 9.0e-8 and 8.3e-7
on RPB, 1.2e-7 and 6.5e-7 on RVB, which is its single-precision fields and matrices.

## Reading

- Every arm leads. KDIS computes matrices per frame and keeps its intermediates in `float`; its `Reset` work, not timed here,
  saves it the Euler conversion. hesap's cost is the turn, so RPW and RVW lead least, 2.1x to 4.8x: KDIS adds Euler rates
  there, which is not the standard's rotation and is not a reference for ours.
- avx2 is behind sse2 on FPW only, 1.1 ns against 0.86: the 4,096 x 3 loads and stores of a loop this small.
- The avx512 level's bytes are proven under SDE; this machine cannot time it.

## KDIS differences, none followed

Its fixed algorithms 6 and 9 still apply R1 and R2 for a nonzero rate. Its body acceleration is the given one plus the rate
crossed with the velocity, which the board and the oracle add to our column to compare positions. At a turn below 1e-5 its
algorithm 9 adds a * dt instead of a * dt^2 / 2. Its rotating orientation integrates Euler rates. FPB is compared at no rate;
FVB is not compared.
