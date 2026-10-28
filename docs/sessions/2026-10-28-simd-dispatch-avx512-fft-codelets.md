# 2026-10-28 — SIMD-DISPATCH: the fft zmm codelets, tranche 1

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `31f8c3d0`.

## CI first

No completed run has a failed job left to fix. Push run 40662109393 (`7e230e22`) has four finished Windows failures,
the Wolfe search fixed in `31f8c3d0`; every other completed run in the last 15 was cancelled with no failed job. In
that run's win-release job, `crd-hesap-fft-kernels-at-avx512-sde-skx` passes in 82.7 s, so the fft SDE arm runs on
the optimised lane, where the SIMD edition of the codelets is compiled. DIAG.11c and DIAG.12 stay Needs CI: no
complete-tier run has finished since.

The eig fixture follow-up of the [debug-frames note](2026-10-28-simd-dispatch-debug-frames-eig-fixture.md) was
already done: `bedfc2bb` made `fill_general` full rank for every nonsymmetric case.

## The tile is the unit, not the register

Every generated body computes tiles of four f64 or eight f32 columns (`kTile`, 32 bytes). The tile decides which
columns the vector steps and the twiddle codelets' scalar tail compute (the tail's products round twice, the tiles'
once), so it must not change with the level. `codelet_lanes.hpp` now gives:

- `TileLane<T>`: one tile a step (Vec4d, Vec8f; a scalar on portable and unoptimised builds);
- `Lane<T>`: the most whole tiles a register holds, two at avx512 (Vec8d, Vec16f);
- `kPairedTiles<T>`: whether a body's odd last tile needs a TileLane step.

`fft_codegen_split.py` emits every lanes codelet (the twiddle combine codelets and the hierarchical ones) as parts
that are templates of the lane, at least one part each. The driver steps the Lane over the whole tiles it holds and,
when Lane holds two, one TileLane step over an odd last tile; the scalar tail is unchanged. Below avx512 the two
lanes are one type and the code is today's loop. The splitter refuses a tile width other than `kTile` and a body that
indexes `t` by a block (`t & m`, `t >> k`) a Lane step could straddle (the smallest is 16 columns). `simd::load<V>`
names the lane, since `load` is overloaded by pointer type. `fma`/`fnmadd`/load/store counts per unit are unchanged
(twiddle 356/356/448/448, hier 1044/1044/656/656): only loops, drivers and part signatures moved.

Vec8d and Vec16f gain `load_complex_deinterleaved` and `store_complex_interleaved` (`vpermt2pd`/`vpermt2ps` at avx512,
the halves' helpers below), pure data movement. Each level's operations are the avx2 ones per column, so the family
stays bit-exact with portable.

Regenerated: `gen_fft_codelets.py` on WSL (twiddle units, `--check` passes; `gen_fft_batched.py --check` too) and the
hier units by `fft_codegen_split.py --lanes` on the `20c9f2a1^` header bytes (the committed header is unchanged).

## Tests

- The level probe checks the new helpers bit for bit (`vec16f`/`vec8d complex (de)interleave`, one template for all
  four widths).
- `test_fft_kernels.cpp`: batched leaves at one and three tiles (f64 4 and 12, f32 8 and 24), so avx512 runs a
  two-tile step and an odd tile; every batched shape also takes an input of negative zeros, whose signs every level
  must keep as portable does.
- win-release, SDE 10.13.1 `-skx`, `CRD_CPU_LEVEL=avx512`: `[kernels],[cpu-level-applied]` 4,740 assertions in 8
  cases, `[simd-levels]` 669,079 in 4; natively 3,603 and 535,420. win-debug compiles the scalar edition.

## Forks

- Fork 1, `gen_fft_batched.py`: it moves onto the lanes with the same driver shape (one shape for every width),
  tranche 2. It still emits its own Vec4d/Vec8f `#ifdef` body today.
- Fork 2, its AoSoA block: keep L = 4/8 and let a zmm step cover two adjacent blocks through the halves. Bits do not
  depend on the layout; L = 8/16 is a board candidate for AVX-512 hardware, which this host does not have.

## Left

Tranche 2, the batched generator on the lanes; tranche 3, the hand-written vector sites of `fft_kernels.cpp`
(batched butterflies, radix-4 rows, fs6 transposes, ip4-AoS, four-step twiddle, first/last interleaved passes,
small-N lane trick). Board candidate: the two-tile step against avx2's one, on AVX-512 hardware.

Follow-up for an extra batch: a test whose REQUIRE fails between `crd::jobs::init` and `shutdown` leaves the pool up,
so the next case asserts `WorkerPool::init called twice` (seen under SDE in the Wolfe batch); 325 init/shutdown pairs
in 138 test files have no scoped owner.
