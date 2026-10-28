# 2026-10-28 — SIMD-DISPATCH: the stats zmm bodies

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `1235ab1d`. CI read for this batch:
[sparse zmm bodies](2026-10-28-simd-dispatch-avx512-sparse.md#ci-first).

## The bodies

- **Philox** (`philox_blocks.hpp`, shared with hesap-tensor): `philox4x32_x16` runs groups of sixteen lanes, 48 blocks
  a step in three independent groups, then 16 a group, then AVX2's 8 and SSE2's 4. The sixteen positions split into
  their low and high words with two `vpermt2d`. The 16 by 4 store transposes inside each 128-bit lane as AVX2 does,
  then transposes the four 128-bit lanes with four `vshufi32x4`.
- **Threefry**: `ThreefryAvx512` holds eight blocks a group, 24 blocks a step in three groups, then 8, then AVX2's
  group of four. Every rotation is one `vprolq`. The 4 by 8 store transposes the same way as Philox's.
- **Student-t**: eight terms a step with `crd_log8`. The low half adds to the four lane sums, then the high half, so
  element i still adds to lane i mod 4 in element order. AVX2's four-lane step takes the next four elements.

The generators are integer arithmetic, and the Student-t sums keep their order with the same single-rounded fma and
log core, so every level gives the portable bits.

## Tests

- `test_stats_kernels.cpp`: the block counts run 0 to 79, which meets each AVX-512 step, group and remainder. The
  Threefry counters wrap word 0 at blocks 4 to 27, so a wrap falls in each group of a 24-block step and in the 8-block
  group after it.
- win-debug: `[kernels]` 3,778 assertions in 5 cases (portable, sse2, sse4.2, avx2).
- SDE 10.13.1 `-skx`, `CRD_CPU_LEVEL=avx512`: 3,781 assertions, avx512 included (3.5 s).
- hesap-tensor's kernel units build with the new Philox header.
- Hosted: the SDE arm `crd-hesap-stats-kernels-at-avx512-sde-skx` on win-release and linux-gcc-release.

## Board candidates (row close, on AVX-512 hardware)

- Three against four Philox groups of sixteen (vpmuludq port pressure), and three against two Threefry groups of
  eight.
- The eight-term Student-t step against avx2's, with the two dependent adds per step.
