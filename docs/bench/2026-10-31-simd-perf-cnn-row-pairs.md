# 2026-10-31 — SIMD-PERF: the cnn rows' second convolution at avx512, two conv rows a register (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-10-31](../sessions/2026-10-31-simd-perf-cnn-row-pairs.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), Ubuntu 24.04, GCC 13.3, perf with the guest PMU. **Real AVX-512 hardware.** `build-bench.py host`: 28.7 of
  29.4 GiB available, other processes 0.0 cores, idle. Every process pinned to CPU 3.
- **Binaries:** the pre-row engine `2f2aed72`'s own `bench_nn` (`scripts/build-prerow.sh`, its linux-gcc-release
  preset, AVX2 code); HEAD (`58e6399d`'s engine, kept from the [previous board](2026-10-31-simd-perf-cnn-pool-staging.md)
  as `~/ab/nn-cand2`); the first candidate, the row pair with its channels in loops (`~/ab/nn-pair`); the kept change,
  the channels expanded as packs (`~/ab/nn-pair2`); both from the linux-gcc-release lane tree (GCC 13.3, `-O3`,
  program LTO). `CRD_CPU_LEVEL` selects the hesap-tensor kernels' level; pre-row runs its compiled AVX2.
- **Program:** each engine's `bench_nn` (frozen CNN: conv 1 to 8 channels on 16 x 16, relu, 2x2 pool, conv 8 to 16
  on 8 x 8, relu, pool, fc 256 to 10, softmax; f32, Q8_0 and i8 tiers; batch 8 and 4096; best of 50 whole-batch
  inferences, ns a sample). HEAD's and the candidates' processes run `bench_nn cnn`, pre-row's its whole program.
- **Method:** `scripts/ab-board.py`, one warm-up, 5 to 25 repetitions in rotated order, median paired speed ratio
  [percentile-bootstrap 95%], 2% band; the arm faster above 1. Profiles: `perf stat` (cycles, instructions,
  `-M TopdownL1`/`TopdownL2`) and `perf record` (cycles) of one `bench_nn cnn` process each, symbol sample counts
  and `perf annotate` of the convolution's loop; the unit's code read from a non-LTO compile of its avx512 object.

## Where the time went (avx512, one `bench_nn cnn` process)

| | HEAD | loop pairs | the kept change |
|---|---|---|---|
| cycles | 7.94 G | 6.16 G | 5.99 G |
| instructions | 26.2 G | 20.2 G | 20.1 G |
| retiring / backend / frontend / bad speculation | 62.9 / 20.7 / 13.6 / 2.8% | 60.5 / 22.3 / 14.0 / 3.2% | |
| the convolution's row blocks (samples) | 5,982 | 3,431 + 1,223 | 3,201 + 1,071 |
| `conv2d_images` (staging, pool) | 1,627 | 1,909 | 1,876 |

- **HEAD:** retiring-bound (62.9%; light operations 50.8%, core-bound 17.1%, memory-bound 3.5%), the convolution's
  row block 68% of the samples, and of those the bulk in the 256-bit loop: the second layer's rows are 8 columns,
  so at avx512 they ran on `Vec8f` with each 512-bit register's upper half idle (per tap row 3 loads and 24
  broadcast fmas on ymm registers, `vfmadd231ps mem{1to8}`), 9,216 fmas an image against the first layer's 1,152
  `zmm` fmas.
- **Loop pairs:** conv rows oi and oi + 1 in one `Vec16f` (the low and the high eight lanes, `vinsertf32x8` from
  the next padded row): per tap row 3 loads, 3 inserts and 24 `zmm` fmas for both rows, half HEAD's fmas. The
  function zeroed a 512-byte stack array with `rep stosq` on every call (4.3% of its samples): GCC kept the eight
  accumulators in memory because the output loop over them was not unrolled.
- **The kept change:** the output channels expanded as a pack (`std::integer_sequence`), so every accumulator is a
  register; no `rep stos` and no stack traffic in the function.

## Boards (ns a sample, speed ratio over the reference)

**avx512, pre-row the reference (25 repetitions):**

| row | pre-row | HEAD avx512 | ratio | the kept change | ratio |
|---|---|---|---|---|---|
| f32 batch 8 | 3,034 | 3,040 | 1.007 [0.988-1.031] unresolved | 2,278 | **1.335 [1.318-1.351] faster** |
| q8 batch 8 | 3,106 | 2,845 | 1.101 [1.070-1.117] faster | 2,354 | **1.318 [1.294-1.356] faster** |
| i8 batch 8 | 3,131 | 2,762 | 1.148 [1.101-1.159] faster | 2,334 | **1.348 [1.337-1.360] faster** |
| f32 batch 4096 | 3,024 | 2,744 | 1.103 [1.098-1.109] faster | 2,332 | **1.300 [1.293-1.306] faster** |
| q8 batch 4096 | 3,135 | 2,832 | 1.106 [1.102-1.111] faster | 2,376 | **1.321 [1.311-1.331] faster** |
| i8 batch 4096 | 3,118 | 2,776 | 1.125 [1.119-1.133] faster | 2,349 | **1.325 [1.320-1.335] faster** |

**avx512, HEAD the reference (5 repetitions, every cell decided):**

| row | HEAD | loop pairs | ratio | the kept change | ratio |
|---|---|---|---|---|---|
| f32 batch 8 | 3,062 | 2,324 | 1.321 [1.306-1.348] | 2,255 | **1.356 [1.328-1.398] faster** |
| q8 batch 8 | 2,923 | 2,424 | 1.192 [1.188-1.228] | 2,337 | **1.239 [1.209-1.291] faster** |
| i8 batch 8 | 2,754 | 2,379 | 1.171 [1.149-1.206] | 2,336 | **1.177 [1.149-1.258] faster** |
| f32 batch 4096 | 2,769 | 2,425 | 1.140 [1.123-1.168] | 2,322 | **1.193 [1.174-1.200] faster** |
| q8 batch 4096 | 2,833 | 2,434 | 1.159 [1.149-1.174] | 2,364 | **1.209 [1.183-1.211] faster** |
| i8 batch 4096 | 2,771 | 2,393 | 1.159 [1.134-1.204] | 2,332 | **1.192 [1.166-1.234] faster** |

**The packs against the loops (avx512, 25 repetitions):** 0.991 to 1.031; q8 and i8 batch 4096 faster (1.031
[1.020-1.042], 1.025 [1.021-1.034]), the other four cells unresolved (0.991 [0.978-1.020] to 1.025 [1.011-1.044]),
none slower. Kept for the cleaner code (no stack array, no `rep stos`) at no measured cost.

**avx2, HEAD the reference (25 repetitions):** the row pair is not built there (`Wide` is `Vec8f`); the image loop
now hands two rows a row-block call. 1.007 to 1.031: q8 batch 8 level (1.007 [0.992-1.014]), i8 batch 4096 faster
(1.031 [1.026-1.040]), the other four unresolved at 1.018 to 1.024 (lower bounds 1.001 to 1.015); nothing slower.

## Correctness

Each lane is one output's column and runs the same (c, ki, kj)-ordered chain of single-rounded fmas from 0, then
`fl(bias + chain)` and the relu, so the bits are HEAD's: on the machine every `crd-hesap-tensor*` case matching
nn, kernel, level or simd (402, every forced-level arm: portable, sse2, sse4.2, avx2, avx512 and the VNNI variants)
passes, including the convolution against the scalar fma chain with NaNs, infinities, signed zeros and subnormals,
whose shapes now hold rows of 2, 5, 7 and 8 columns at R 1, 3, 6, 7, 8 and 8 + 8, even and odd heights. Storing the
low half into both rows (a teeth edit on the first candidate) fails the avx512 arm and leaves the avx2 arm passing. Every board process passed its tier bound against the
f32 reference.
