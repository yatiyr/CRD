# 2026-10-27 — SIMD-DISPATCH: the hesap-sparse SELL kernels

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).

## CI read

- No completed run since the last read has a failed job: 40462897802 (`12fbcb1a`), 40458470799 (`c65799a6`),
  40451010209 and 40445742611 were cancelled with no failed job. Push run 40455017769 (`09ba6a3a`) is in progress
  with none failed; 40466574508 (`bed4f492`) is pending. Nothing was dispatched.
- Needs CI audit: no complete run finished since 40434280766, so [DIAG.11c](../ROADMAP.md#slice-diag.11c) and
  [DIAG.12](../ROADMAP.md#slice-diag.12) stay as recorded; they follow [DIAG.11b](../ROADMAP.md#slice-diag.11b)'s user
  re-record (8).

## Mechanism

hesap-sparse is the fifth family: `crd_add_cpu_kernels(crd-hesap-sparse NAME sparse LEVELS portable sse2 avx2)`.

- `sparse_kernels.hpp` holds only data: the slice heights (`kSellSliceF64`, `kSellSliceF32`, which `sell.hpp`'s
  `kSellC` now reads), the `SellSlices<T>` view of a `SellMatrix` (perm `nullptr` for the identity order) and the
  table (`sell_spmv_f64`, `sell_spmv_f32`). `sparse_kernels()` (`src/sparse_kernel_dispatch.cpp`) resolves it once.
- `sell.hpp`'s `sell_spmv_range` calls the table for f64 and f32 (serial and per job range of
  `spmv_sell_parallel`); complex keeps its C scalar sums. The header includes no SIMD header and tests no
  `CRD_SIMD_HAS_AVX2`; a new include-budget entry keeps it free of `<immintrin.h>` on GCC and Clang.
- `kernels/sparse_kernels.cpp`: one driver over a body per level. The portable body keeps C sums in an array (GCC
  vectorizes it with SSE2 at x86-64), the sse2 bodies two 128-bit registers a slice, the avx2 bodies one 256-bit
  register. Every lane adds `vals * x[col]` with two roundings in column order and every level writes y with the
  same scalar expression, so every level gives the CSR spmv's bits.
- The objects define only their level namespace and the table (`nm -C` on the three GCC objects, plus
  `DW.ref.__gxx_personality_v0`; `llvm-nm` on the MSVC objects, plus `__real@` constants); the kernel units compile
  with `-ffp-contract=off` (GCC) and `/fp:precise` (cl).

### Variant board

The body forms were chosen on the board below (linux-gcc-release, `taskset -c 4`, 6 interleaved rounds, medians,
one-thread rows). Each variant is the kernel source with one knob, linked into the same bench.

| Knob | Measured | Kept |
|---|---|---|
| Two slices at a time always | +12% to +21% on wide slices in L2 (uniform-16), but -4% to -15% on narrow slices from DRAM (uniform-4, banded-5 at 1M and 2M rows) | pairs only when both slices are at least 8 wide (`kPairWidth`) |
| sse2 f32 gather: scalar loads to an array, two vector loads, against four-lane `_mm_setr_ps` | array 2% ahead in L2, level from DRAM | array |
| avx2 f64 gather: scalar loads against `vgatherdpd` | scalar loads 6% to 20% ahead in L2, level from DRAM | scalar loads |
| avx2 f32 gather: scalar loads against `vgatherdps` | `vgatherdps` 24% to 31% ahead in L2, 4% to 12% from DRAM | `vgatherdps` |
| Prefetch of `col_idx` 16 elements ahead (HEAD's avx2 f64 form) | no row faster | none |
| Prefetch of the `x[col]` targets 8 or 24 column steps ahead (avx2 f64) | 10% to 31% slower in L2, -10% to +7% from DRAM | none |

## Verification

The differential case runs every level the host executes on f64 and f32 matrices of 0 to 300 rows (row counts that
end mid-slice, empty rows, runs of 8 to 12 entries so that neighbouring slices pair with unequal widths, every
seventeenth row 25 long) in σ orders 0, 1 and 6, over six slice ranges each, at three alpha/beta pairs (beta 0 with
NaN in y). Values include a signed zero, a subnormal, both infinities and NaN. Each range's rows must carry the CSR
spmv's bits, every other row and a guard past y must stay untouched. A public-path case checks `spmv_sell`.

| Tooth | win-debug | linux-gcc-debug |
|---|---|---|
| a slice run alone is not written | f64, f32 and `spmv_sell` | the same |
| a pair's second slice skips its last column | f64, f32 and `spmv_sell` | the same |
| avx2 f32 sums by `vfmadd` | f32 differential | the same |
| avx2 f64 sums by `vfmadd` | f64 differential and `spmv_sell` | the same |
| avx2 f64 gathers lanes 2 and 3 swapped | f64 differential and `spmv_sell` | the same |
| sse2 f64 gathers lanes 2 and 3 swapped | f64 differential | the same |
| sse2 f32 gathers neighbouring lanes swapped | f32 differential | the same |
| portable lanes all read lane 0's column | f64 and f32 differential | the same |
| the σ order is ignored | f64, f32 and `spmv_sell` | the same |
| beta == 0 reads y | f64 and f32 differential | the same |
| the last slice writes a full slice | f64 differential crashes (perm read out of range) | the same (SIGSEGV) |
| a pair is formed past the range's end | f64 differential crashes (width read out of range) | the same (SIGSEGV) |

Each restore was rebuilt and passed. Both lanes pass crd-hesap-sparse-tests (98 cases) and the five
`crd-hesap-sparse-kernels-at-<level>` arms. The include budget reports `OVER` when `sell.hpp` includes
`crd/math/simd/vec4d.hpp`. Strict tidy is clean on the changed sources; the bench is `UNGATED` (no tree resolves its
Eigen include). clang-format reports nothing on the new files and fewer findings on `sell.hpp` than HEAD; Allman and
the 15 guards pass.

## Board

`bench_hesap_spmv_vs_reference` gained f32 rows, an L2-resident size (8,000 rows), the level line and the argument 1
for one-thread rows. linux-gcc-release, `taskset -c 4`, 5 interleaved rounds, medians; ms (L2 rows in µs). New at
forced `CRD_CPU_LEVEL`; HEAD's `sell.hpp` through a scratch include directory at the lane's flags (`head-avx2`) and
at the sse2 lanes' flags (no ISA flags, `CRD_SIMD_TARGET=1`, `head-sse2`); both HEAD binaries print the new
dispatcher's level, which they do not call. Eigen 3 RowMajor, one thread, AVX2 flags.

| Pattern | N | portable | sse2 | avx2 | HEAD sse2 | HEAD avx2 | Eigen | avx2 / HEAD | sse2 / HEAD | avx2 / Eigen |
|---|---|---|---|---|---|---|---|---|---|---|
| uniform-16 f64 | 8K | 39.0 | 32.1 | 30.7 | 40.1 | 33.0 | 45.1 | 1.07x | 1.25x | 1.47x |
| uniform-4 f64 | 8K | 13.6 | 10.6 | 10.5 | 13.4 | 11.0 | 12.2 | 1.05x | 1.26x | 1.16x |
| banded-5 f64 | 8K | 13.9 | 10.7 | 11.5 | 13.1 | 12.5 | 13.2 | 1.09x | 1.22x | 1.15x |
| power-law f64 | 8K | 21.1 | 17.2 | 17.6 | 21.4 | 18.1 | 30.9 | 1.03x | 1.24x | 1.76x |
| uniform-16 f32 | 8K | 29.6 | 22.8 | 17.7 | 24.1 | 23.6 | 42.1 | 1.33x | 1.06x | 2.38x |
| uniform-4 f32 | 8K | 10.1 | 7.3 | 6.2 | 8.0 | 8.1 | 13.1 | 1.31x | 1.10x | 2.11x |
| banded-5 f32 | 8K | 11.2 | 8.5 | 7.2 | 9.1 | 9.4 | 14.5 | 1.31x | 1.07x | 2.01x |
| power-law f32 | 8K | 16.4 | 12.3 | 10.3 | 13.5 | 13.3 | 25.9 | 1.29x | 1.10x | 2.51x |
| uniform-16 f64 | 1M | 23.00 | 22.55 | 23.06 | 24.39 | 22.13 | 26.67 | 0.96x | 1.08x | 1.16x |
| uniform-4 f64 | 1M | 7.68 | 7.17 | 6.98 | 7.36 | 7.23 | 7.53 | 1.04x | 1.03x | 1.08x |
| banded-5 f64 | 1M | 2.93 | 2.81 | 2.81 | 2.97 | 2.85 | 2.90 | 1.02x | 1.06x | 1.03x |
| power-law f64 | 1M | 14.21 | 12.97 | 13.52 | 13.83 | 12.63 | 13.61 | 0.93x | 1.07x | 1.01x |
| uniform-16 f32 | 1M | 18.02 | 17.05 | 16.86 | 17.12 | 18.07 | 18.20 | 1.07x | 1.00x | 1.08x |
| uniform-4 f32 | 1M | 4.84 | 4.19 | 4.69 | 4.14 | 4.92 | 5.49 | 1.05x | 0.99x | 1.17x |
| banded-5 f32 | 1M | 1.92 | 1.65 | 1.70 | 1.67 | 1.71 | 2.33 | 1.00x | 1.01x | 1.37x |
| power-law f32 | 1M | 9.53 | 9.61 | 8.64 | 8.79 | 8.47 | 9.07 | 0.98x | 0.91x | 1.05x |
| uniform-16 f64 | 2M | 66.87 | 70.77 | 70.18 | 71.97 | 72.68 | 73.15 | 1.04x | 1.02x | 1.04x |
| uniform-4 f64 | 2M | 19.20 | 18.95 | 18.94 | 19.05 | 18.27 | 20.94 | 0.96x | 1.01x | 1.11x |
| banded-5 f64 | 2M | 5.73 | 5.56 | 5.62 | 5.73 | 5.79 | 6.20 | 1.03x | 1.03x | 1.10x |
| power-law f64 | 2M | 36.81 | 39.10 | 38.06 | 38.86 | 38.97 | 38.97 | 1.02x | 0.99x | 1.02x |
| uniform-16 f32 | 2M | 41.97 | 40.48 | 39.74 | 42.50 | 43.03 | 45.47 | 1.08x | 1.05x | 1.14x |
| uniform-4 f32 | 2M | 12.06 | 11.05 | 10.88 | 11.78 | 11.64 | 13.72 | 1.07x | 1.07x | 1.26x |
| banded-5 f32 | 2M | 4.06 | 3.77 | 3.75 | 3.81 | 3.81 | 4.76 | 1.02x | 1.01x | 1.27x |
| power-law f32 | 2M | 24.42 | 22.81 | 22.34 | 23.42 | 23.04 | 23.58 | 1.03x | 1.03x | 1.06x |

- In L2 avx2 leads HEAD 1.03x to 1.09x (f64) and 1.29x to 1.33x (f32), sse2 leads HEAD's sse2 lanes 1.22x to 1.26x
  (f64) and 1.06x to 1.10x (f32); avx2 leads Eigen 1.15x to 2.51x.
- From DRAM the rows below 0.97x lie inside their round-to-round spread, which is 8% to 12% there: avx2 power-law
  f64 1M 12.26 to 13.70 against HEAD's 12.07 to 13.24, uniform-16 f64 1M 22.10 to 23.55 against 21.66 to 23.26,
  uniform-4 f64 2M 18.55 to 19.78 against 17.99 to 19.17, sse2 power-law f32 1M 8.64 to 9.75 against 8.32 to 9.58.
  The same binaries two runs earlier gave sse2 power-law f32 1M 0.97x of HEAD's sse2.
- avx2 leads Eigen in every row; from DRAM by 1.01x to 1.04x on the f64 power-law, banded and uniform-16 rows, where
  the product streams 12 bytes per entry from memory as Eigen's CSR does. The lever left there is fewer bytes per
  entry: a SELL variant with 16-bit column deltas per slice.

## Left in the row

The fft, dense and geometry kernels as per-level units first; the complex SELL bodies and the compressed-index SELL
variant join the rest the row lists.
