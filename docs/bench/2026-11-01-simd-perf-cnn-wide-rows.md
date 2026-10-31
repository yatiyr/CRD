# 2026-11-01 — SIMD-PERF: avx512's 16-column conv rows pooled in registers, and the fft gaps re-read (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-11-01](../sessions/2026-11-01-simd-perf-cnn-wide-rows.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), Ubuntu 24.04, GCC 13.3, perf with the guest PMU (`mem_load_retired.l3_miss` reads 0 there: the guest PMU,
  not a measurement). **Real AVX-512 hardware.** Every process pinned to CPU 3. One start, 0.66 h.
- **Binaries:** HEAD `36792923` (`~/ab/nn-head8`) and the candidate (`~/ab/nn-c8`, the kept change), both from the
  linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO); the pre-row engine `2f2aed72`'s own `bench_nn`
  (`scripts/build-prerow.sh`, its compiled AVX2). The fft rows: the same tree's `bench_fft_f32`, `bench_fft_vs_refs`
  and `prof_fft_kernel` at HEAD, and the profile tree (`CRD_HESAP_FFT_PROFILE=ON`) for the phase counters. MKL
  2020.0.4, FFTW 3.3.10, pocketfft `4d865cba`.
- **Program:** `bench_nn cnn` (frozen CNN: conv 1 to 8 channels on 16 x 16, relu, 2x2 pool, conv 8 to 16 on 8 x 8,
  relu, pool, fc 256 to 10, softmax; f32, Q8_0 and i8 tiers; batch 8 and 4096; best of 50 whole-batch inferences, ns
  a sample); pre-row runs its whole program.
- **Method:** `scripts/ab-board.py`, one warm-up, rotated order, median paired speed ratio [percentile-bootstrap
  95%], 2% band, the arm faster above 1: avx512 10 repetitions (every cell decided at the first check), avx2 5 to 10
  as the control. `CRD_CPU_LEVEL` selects the hesap-tensor kernels' level. Profiles: `perf stat` (cycles,
  instructions, branch misses) and `perf record` (cycles) of one `bench_nn cnn` process each.

## Where the time went (one `bench_nn cnn` process, avx512)

| | HEAD | kept |
|---|---|---|
| cycles | 5.22 G | 4.17 G |
| instructions | 17.99 G | 14.01 G |
| branch misses | 1.71 M | 1.01 M |
| `conv2d_images` (staging, pool, image loop) | 23.8% | 11.5% |
| the convolution's row blocks | 63.9% (two symbols, 47.7% and 16.2%) | 76.6% |

- **HEAD:** after the [image-loop board](2026-11-01-simd-perf-cnn-pool-in-register.md), the first layer's
  16-column rows were the one avx512 path still outside registers: each conv row a block of eight channels on its
  own (eight accumulators), the two rows of a pooled row computed one after the other with every input row loaded
  twice, then stored to two row buffers and read back by `max_pool2_row`'s general loops.
- **The kept change:** `conv3x3_wide_pair` holds both rows of eight channels in sixteen `zmm` accumulators (16 of
  32), each row load shared by the eight channels, and pools every channel's two registers in place: the rows'
  maximum, one `vpermps` to split the even and odd columns (`Vec16f`'s `deinterleave`), their maximum and one `ymm`
  store of the eight pooled columns. Cycles -20%, instructions -22%, `conv2d_images` halved.

## Boards (ns a sample, speed ratio over the reference)

**avx512, HEAD the reference (10 repetitions, every cell decided):**

| row | HEAD | the kept change | ratio | pre-row | ratio |
|---|---|---|---|---|---|
| f32 batch 8 | 2,319 | 1,846 | **1.253 [1.222-1.286] faster** | 3,616 | 0.640 [0.620-0.645] slower |
| q8 batch 8 | 2,322 | 1,986 | **1.185 [1.160-1.207] faster** | 3,702 | 0.621 [0.617-0.633] slower |
| i8 batch 8 | 2,296 | 1,957 | **1.190 [1.169-1.204] faster** | 3,730 | 0.607 [0.604-0.622] slower |
| f32 batch 4096 | 2,407 | 1,794 | **1.341 [1.336-1.346] faster** | 3,649 | 0.660 [0.658-0.661] slower |
| q8 batch 4096 | 2,304 | 1,891 | **1.221 [1.209-1.225] faster** | 3,787 | 0.608 [0.606-0.611] slower |
| i8 batch 4096 | 2,262 | 1,878 | **1.203 [1.198-1.208] faster** | 3,763 | 0.601 [0.598-0.603] slower |

Against the pre-row engine the kept change is 1.86x to 2.03x (its medians over pre-row's in the same board; HEAD
was 1.52x to 1.66x).

**avx2, HEAD the reference (the control; no row pair is built below avx512, so the code is the same):** f32 batch
4096 1.002 [0.998-1.003], q8 batch 4096 1.002 [0.999-1.005], i8 batch 4096 1.002 [0.997-1.004] and i8 batch 8 1.005
[0.986-1.017] level; f32 batch 8 1.003 [0.965-1.017] and q8 batch 8 0.994 [0.978-1.006] unresolved after the 10
repetitions, both intervals across 1.

## Correctness

Each lane of a wide pair is one output column, accumulated over the same (channel, ki, kj) chain from zero, then bias +
chain and the relu, as `conv3x3_step`; the pool takes `max_pool2_row`'s maximum of the same two values and then of the
same column pair, so the bits are HEAD's at every level. On the machine every `crd-hesap-tensor-kernels-at-<level>` and
`crd-math-kernels-at-<level>` case passed at portable, sse2, sse4.2, avx2 and avx512 (174), the convolution case now
with 12- and 16-column shapes (R 8 and 8 + 8, even heights) beside the 9-, 15- and 16-column ones it had; storing the
even columns in place of the pooled ones in the wide pool fails the avx512 arm only (teeth), and the restored source
passes again.

## The fft gaps re-read on the same start (no change)

The planned forward against its peers, one run each of the programs above, ms a transform, best of reps.

**f32 forward against MKL (`bench_fft_f32`, every arm the planner offers):**

| n | avx512 planned (arm) | MKL | ratio | avx2 planned | MKL | ratio |
|---|---|---|---|---|---|---|
| 1K | 0.00073 (standalone-hier) | 0.00100 | 1.37x | 0.00104 | 0.00105 | 1.01x |
| 2K | 0.00252 (ip4) | 0.00201 | 0.80x | 0.00275 | 0.00198 | 0.72x |
| 4K | 0.00489 (standalone-hier) | 0.00355 | 0.73x | 0.00549 | 0.00381 | 0.69x |
| 8K | 0.01248 (standalone-hier) | 0.01102 | 0.88x | 0.01474 | 0.01126 | 0.76x |
| 16K | 0.03259 (ip4) | 0.02585 | 0.79x | 0.03046 | 0.02624 | 0.86x |
| 32K | 0.06530 (ip4) | 0.06064 | 0.93x | 0.07235 | 0.06078 | 0.84x |
| 64K | 0.14976 (ip4) | 0.13339 | 0.89x | 0.14173 | 0.12764 | 0.90x |

Arm rows ahead of the plan on this machine (single runs, recorded for the machine-dependent plan item, not plans):
2K standalone-hier (0.91x MKL at avx512, 0.84x at avx2); 8K deep-split-4 at avx2 (0.85x); 32K and 64K deep-split-4
at avx512 (0.97x both) and 32K deep-split-3 at avx2 (0.87x). Every arm printed the same bits at both levels.

**f64 forward against FFTW and MKL (`bench_fft_vs_refs`):**

| n | avx512 | FFTW | MKL | x FFTW | avx2 | FFTW | MKL | x FFTW |
|---|---|---|---|---|---|---|---|---|
| 128K | 0.765 | 0.687 | 1.229 | 0.90x | 0.874 | 0.654 | 1.224 | 0.75x |
| 256K | 1.679 | 1.578 | 2.651 | 0.94x | 1.907 | 1.583 | 2.688 | 0.83x |
| 512K | 3.790 | 4.923 | 5.505 | 1.30x | 4.244 | 4.916 | 5.541 | 1.16x |
| 1M | 8.552 | 10.773 | 11.722 | 1.26x | 9.802 | 11.134 | 11.750 | 1.14x |
| 2M | 21.734 | 21.897 | 25.298 | 1.01x | 27.811 | 21.439 | 24.957 | 0.77x |
| 4M | 55.267 | 46.916 | 79.006 | 0.85x | 60.175 | 48.462 | 88.521 | 0.81x |
| 8M | 115.883 | 111.639 | 115.827 | 0.96x | 138.987 | 122.962 | 111.290 | 0.88x |

At 4M the six-step arm (43.8 ms avx512, 52.8 avx2) is ahead of the planned four-step at both levels, and at avx512
of FFTW. The timing is the same for every in-place contender: the input copy before each repetition is untimed for
Cerid, FFTW and MKL alike (`time_best_ms`'s `prepare`), so these ratios are not biased by the restore; the
`prof_fft_kernel` loop (22.2 ms at avx2 2M) runs on its own output without a restore and is not comparable with them.

**The 2M f64 four-step's phases (`prof_fft_kernel 21`, the profile tree, rdtsc Mticks a transform):**

| phase | avx2 | avx512 |
|---|---|---|
| P1 gather (strided rows to scratch, `memcpy`) | 6.9 (13.2%) | 6.9 (15.9%) |
| P1 sub-FFT (the batched 2048-point plan) | 14.4 (27.5%) | 10.7 (24.5%) |
| P1 twiddle and transposed store | 7.4 (14.1%) | 7.4 (17.1%) |
| P2, the fused 32 x 32 path (gather, sub-FFT and scatter; one timer) | 23.6 (45.1%) | 18.5 (42.4%) |

`perf stat` of the counter-free build at avx2 (core cycles, 61 transforms): 79.7 M cycles and 170 M instructions a
transform, backend-bound 64% (TopdownL1), fill buffers full 18.5% of cycles, about 100 K load and 74 K store page
walks a transform (the strided gathers and the scatter touch a new 4 KiB page every row; huge pages were measured not
to move the four-step's time on the [first board](2026-10-31-simd-perf-fft-f32-xeon.md)). The hottest symbols are
the hier codelets shared by P1 and P2 (`codelet32_stage1_fused_32x32_gather` 22%, `codelet32_batched` 22%,
`codelet64_stage1_fused_64x32` 13%), `execute_four_step`'s own body 19% (P1's twiddle and P2's scatter) and the
gather's `memmove` 11%: attribute by the phase counters, not by symbol.

The levers this names (P1's 2048-point gather fused like the 1024-point one, P2's stage 2 writing the output in
place of the scatter) and the 4K in-place forward need new bodies in `hier_codelets.hpp`, whose generator is absent
(`scripts/generated-sources.json`: the bytes are the artifact, REPO.DEV.10); see the session's Left.
