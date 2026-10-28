# 2026-10-28 — SIMD-DISPATCH: the fft zmm tranche 3b

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `905961a0`.

## CI first

No job needs a fix. Schedule run 40663338177 (`06ff67a5`) and push run 40662109393 (`7e230e22`) are still running.
Each of their 14 finished failures is the nonlinear CG moat (`test_conjugate_gradient.cpp:226`) on a commit before the
Wolfe fix `31f8c3d0`; linux-gcc-asan also shows the registered TP-5 MikkTSpace shift. Every push run since
`31f8c3d0` was cancelled by the next push, so no hosted run has shown the Wolfe fix passing yet. DIAG.11c and DIAG.12
stay Needs CI until an all-green complete-tier run.

ip4-AoS batch: the schedule run's eight failed jobs (win-shipping, win-shipping-profile, win-release,
win-debug, win-public-checks, linux-gcc-release, linux-gcc-relwithdebinfo, linux-clang-tsan) are all the same moat
on `06ff67a5`, an ancestor of the fix. Every later push run was cancelled by the next push.

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

Only f64 reaches the six-step (f64 1M forward); the planner gives f32 1M the deep-split. The f32 body (Vec16f,
sixteen-row micro-tiles) compiles at every level, and only the probe runs its `transpose16x16`.

`fs6_transpose_oop` had no caller (the out-of-place rechain measured worse, 2026-07-04) and is deleted.

## Small-N: the batched codelets four transforms a step

The batched N=8 and N=16 codelets (`small_n_codelets.hpp`) run per complex lane: each lane carries its own
transform. Their leaves are templates on the step's type, and at avx512 a Vec8d step takes four transforms of the
batch, then a last pair on Vec4d. The leaf twiddles are the same on every lane, so a Vec8d holds the Vec4d entry
twice. Vec8d gains `swap_pairs` and `fmaddsub`, each its halves' helper below avx512, with probe rows.

The single-transform lane trick stays on Vec4d at every level. Its two halves are its even and odd subsequences, so
a wider register would be a different decomposition with a different rounding order, outside the family's one
algorithm per size.

## ip4-AoS: two or four fold units a call

Every operation of the interleaved in-place engine is per complex lane or per fold unit, so at avx512 it runs on
`WideVec` with each lane's operations unchanged:

- A fold unit is a complex pair: 128 bits of f32, 256 of f64. One call folds one unit on Vec4d, two on Vec8f or
  Vec8d (the f32 twin pattern, now f64's at avx512) and four on Vec16f. One `fold16` body serves every count; unit u
  sits at `lbase + 2u`, and in the odd-length layout units (v, 0) and (v, 1) are adjacent, so a call takes
  consecutive units.
- The combine passes and the radix-2 tail keep their block pair times two vectors: four chains at every width, four
  f64 or eight f32 complex a vector at avx512. Every pass's run is a multiple of the wide step (q >= 16).
- Vec8d gains `addsub` (a masked subtract on the sum), `load_dup_pairs`, `load_c_quad`, `unpack_c_lo/hi`,
  `blend_c_odd` and `store_c_lo/hi`, each its Vec4d halves' helper below avx512 (concat, mix_lo_hi, load_pair128).
  Vec16f gains `swap_pairs`, `addsub`, `fmaddsub`, `load_dup_pairs`, `load_c_quad`, `unpack_c_lo/hi` and
  `blend_c_odd`. The probe compares each bit for bit (arithmetic by value).
- `load_dup_pairs` and `load_c_quad` differ by return type only, so on Vec4d, Vec8f, Vec8d and Vec16f they are static
  members (`V::load_dup_pairs`), as `V::load` is; the free forms are gone.

f32 takes ip4-AoS at 2K, 16K, 32K and 64K, both parities, so the kernel sweep runs the Vec16f fold too.

Two strict-analysis findings in `d769accb`'s code are fixed: the diagonal micro-tile's twiddle choice is one select
instead of two equal branches, and the probe's transpose input is one indexed load instead of a nested conditional.

## Tests

The batched shapes gain N=8 with one pair and N=16 with a four-transform step and a last pair.

- fs6 (`d769accb`): win-debug fft `[kernels]` 4,891 assertions in 7 cases and `[simd-levels]` 536,462 in 3;
  win-release under SDE 10.13.1 `-skx`, `CRD_CPU_LEVEL=avx512`, fft `[kernels],[cpu-level-applied]` 6,428 in 8
  (132 s) and `[simd-levels],[cpu-level-applied]` 670,379 in 4. The sweep's f64 1M forward runs the six-step.
- Small-N: win-debug `[simd-levels]` 537,504 in 3; win-release natively fft `[kernels]` 5,035 in 7; under SDE skx
  at avx512 fft `[kernels],[cpu-level-applied]` 6,620 in 8 (123 s) and `[simd-levels],[cpu-level-applied]`
  671,679 in 4.
- ip4-AoS: win-debug fft `[kernels]` 5,035 in 7 and `[simd-levels]` 541,672 in 3; win-release natively fft
  `[kernels]` 5,035 in 7; under SDE skx at avx512 fft `[kernels],[cpu-level-applied]` 6,620 in 8 (127 s) and
  `[simd-levels],[cpu-level-applied]` 676,879 in 4.

Every run is bit-exact with portable.

Board candidates for AVX-512 hardware: the 2 by 2 micro-tile transposes against avx2's one-tile pair-swap, the
four-transform small-N steps against avx2's pairs, and ip4-AoS's two- and four-unit folds and wide passes
against avx2's.

## Left

None: tranche 3b is complete. The row goes on with NEON parity.
