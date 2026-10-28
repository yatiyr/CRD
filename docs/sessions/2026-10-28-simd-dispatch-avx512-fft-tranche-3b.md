# 2026-10-28 — SIMD-DISPATCH: the fft zmm tranche 3b

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `905961a0`.

## CI first

No job needs a fix. Schedule run 40663338177 (`06ff67a5`) and push run 40662109393 (`7e230e22`) are still running.
Each of their 14 finished failures is the nonlinear CG moat (`test_conjugate_gradient.cpp:226`) on a commit before the
Wolfe fix `31f8c3d0`; linux-gcc-asan also shows the registered TP-5 MikkTSpace shift. Every push run since
`31f8c3d0` was cancelled by the next push, so no hosted run has shown the Wolfe fix passing yet. DIAG.11c and DIAG.12
stay Needs CI until an all-green complete-tier run.

## fs6: the six-step transposes on 2 by 2 micro-tiles

`fs6_transpose_inplace` is one body on `WideVec<T>`. A micro-tile is `kCols` complex square: one tile below avx512,
a 2 by 2 block of tiles at avx512, moved by one transpose per plane. The bits stay those of the one-tile pair-swap:

- Each row keeps its twiddle recurrence, one step a tile. At avx512 the state holds two tiles, the second seeded as
  the first advanced one step, and a micro-tile advances it two steps.
- Off the diagonal, the V-half takes its rows' states and the U-half the transposed state block, as before.
- On a diagonal micro-tile the one-tile body twiddles a lower tile with the transposed element's state (row q at
  column p). At avx512 the lower tile's rows take the transposed block on their first tile and their own state on
  the second.

`Vec8d` gains `transpose8x8` and `Vec16f` gains `transpose16x16`: unpacks and two 128-bit lane shuffle stages at
avx512, the quadrant transposes and a swap below it. The level probe compares both bit for bit.

`fs6_transpose_oop` had no caller (the out-of-place rechain measured worse, 2026-07-04) and is deleted.

## Tests

- win-debug: fft `[kernels]` 4,891 assertions in 7 cases; `[simd-levels]` 536,462 in 3.
- win-release under SDE 10.13.1 `-skx`, `CRD_CPU_LEVEL=avx512`: fft `[kernels],[cpu-level-applied]` 6,428 in 8
  (132 s), and `[simd-levels],[cpu-level-applied]` 670,379 in 4. Both bit-exact with portable; the sweep's f64 1M
  forward runs the six-step.

Board candidate for AVX-512 hardware: the 2 by 2 micro-tile transposes against avx2's one-tile pair-swap.

## Left

ip4-AoS's fold on 128-bit complex pairs, and the small-N lane trick.
