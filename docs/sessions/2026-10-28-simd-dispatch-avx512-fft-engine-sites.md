# 2026-10-28 — SIMD-DISPATCH: the fft zmm engine sites, tranche 3a

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `48172f8d`.

## CI first

No job needs a fix. Every failure on a completed or running run is on a commit before `31f8c3d0`:

- Schedule run 40663338177 (`06ff67a5`): win-shipping, win-shipping-profile, win-release, win-debug,
  win-public-checks, linux-gcc-release and linux-gcc-relwithdebinfo each fail only the nonlinear CG moat, at
  `test_conjugate_gradient.cpp:226` (`LineSearchFailed`). That is the Wolfe search `31f8c3d0` fixed. The Linux
  lanes hit it too, on hosted runners with AVX-512.
- Push run 40662109393 (`7e230e22`): linux-gcc-asan fails the same moat, plus the registered TP-5 MikkTSpace shift
  ([third-party defects](../third-party-defects.md#tp-5)).

No hosted run has shown the Wolfe fix passing yet. Every push run since `31f8c3d0` was cancelled, and the 0a5f288b
run (40680724687) started no job. The complete run 40649898318 (`3e849d96`) finished as a failure, and the earlier
batches fixed every one of its failures. DIAG.11c and DIAG.12 stay Needs CI until an all-green complete-tier run.

## One body per site, the tile as the bound

`fft_kernels.cpp` names its lanes once:

- `TileVec<T>`: one tile, Vec4d or Vec8f, the four f64 or eight f32 columns of `codelet_lanes.hpp`'s `kTile`.
- `WideVec<T>`: Vec8d or Vec16f at avx512, otherwise the TileVec.

The engine keeps its vectors on every level, debug builds included. The codelets' scalar edition does not apply to
these sites.

`for_tiles<T>(n, run)` runs `run<V>(first, last)` in two parts: two tiles a step over the even tiles, then an odd last
tile on TileVec. It returns `n` rounded down to a tile, where the caller's unchanged scalar tail starts on every
level. The bound matters for bits:

- radix4_row's and the last pass's vector steps round the twiddle products once (fma); the tail rounds them twice.
- Several tails write `-isign * t` where the steps write `0 - sg * t`.

So a column must land in the same part on every level.

Sites moved onto it, each now one body:

- the batched radix-2, -4 and -8 butterflies;
- radix4_combine_inplace;
- radix4_row (its f64 and f32 bodies were the same code twice);
- the four-step twiddle (its arrays are now `alignas(64)`, sized by the step);
- radix4_first_interleaved and radix4_last_interleaved.

The strided twiddle gathers build a Wide from two Vec4d (`gather_f64`). The last pass's group transposes run one
4x4 per tile (`load_points4`). Both are data movement. fs6, ip4-AoS and the small-N codelets now take TileVec by
name. The bodies are unchanged until tranche 3b.

## Tests

`test_fft_kernels.cpp` gains two cases:

- The plain sweep gains a negative-zero pass at every size, both directions, every level, against portable's bits.
  Until now only the batched shapes had one.
- Batched shapes at 16 and 32 points with a batch of three tiles plus one column. In one call the radix-2/4/8
  butterflies run a two-tile step, an odd tile and the scalar tail.

Results:

- win-debug: `[kernels]` 4,891 assertions in 7 cases.
- win-release natively: 4,891 assertions.
- win-release under SDE 10.13.1 `-skx`, `CRD_CPU_LEVEL=avx512`: `[kernels],[cpu-level-applied]` 6,428 assertions in
  8 cases, 138 s, bit-exact with portable.
- Budget: the third sweep pass adds about half to the plain sweep. The slowest hosted arms were linux-gcc-asan's
  `crd-hesap-fft-kernels-at-<level>` at 315 s and win-debug's at 213 s (runs 40662109393 and 40663338177), so about
  470 s against CTest's 1500 s default. win-release's SDE arm took 24 s there.

## Left

Tranche 3b:

- **fs6 micro-tile transposes:** the twiddle recurrence steps `W^{(r+i)·lanes}` once a tile. A two-tile step must hold
  `[w, w·step]` and advance by two sequential steps. The pair-swap partner of a horizontal tile pair is a vertical
  one, so the diagonal logic has to be rewritten for 2 by 2 micro-tiles.
- **ip4-AoS:** its fold works on 128-bit complex pairs (`mix_lo_hi`, `concat_*`, a constant builder per 128-bit lane).
  Every operation per complex value has to stay as it is.
- **The small-N lane trick.**

Board candidate for AVX-512 hardware: the two-tile engine steps against avx2's one.
