# 2026-11-01 — SIMD-PERF: avx2's conv rows as register pairs, pooled in registers (real hardware)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-11-01](../sessions/2026-11-01-simd-perf-cnn-avx2-pairs.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), Ubuntu 24.04, GCC 13.3, perf with the guest PMU. Every process pinned to CPU 3. One start, about 1.3 h.
- **Binaries:** HEAD `62a88407` (`~/ab/nn-head9`) and the kept change (`~/ab/nn-c9b`), both `bench_nn` from the
  linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO); the pre-row engine `2f2aed72`'s own `bench_nn`
  (`scripts/build-prerow.sh`, its compiled AVX2).
- **Program:** `bench_nn cnn` (frozen CNN: conv 1 to 8 channels on 16 x 16, relu, 2x2 pool, conv 8 to 16 on 8 x 8,
  relu, pool, fc 256 to 10, softmax; f32, Q8_0 and i8 tiers; batch 8 and 4096; best of 50 whole-batch inferences, ns
  a sample); pre-row runs its whole program.
- **Method:** `scripts/ab-board.py`, one warm-up, rotated order, median paired speed ratio [percentile-bootstrap
  95%], 2% band, the arm faster above 1: avx2 25 repetitions, avx512 (the control) 30. `CRD_CPU_LEVEL` selects the
  hesap-tensor kernels' level. Profiles: `perf stat -r 3` (user cycles, instructions, branch misses), `perf stat -M
  TopdownL1,TopdownL2` and `perf record` (cycles) of one `bench_nn cnn` process each.

## Where the time went (one `bench_nn cnn` process, avx2)

| | HEAD | kept |
|---|---|---|
| cycles (user) | 7.78 G | 6.44 G |
| instructions (user) | 36.70 G | 26.22 G |
| branch misses | 1.59 M | 0.11 M |
| retiring / core bound / memory bound | 74.4% / 18.6% / 2.3% | 65.8% / 31.1% / 0.8% |
| the convolution (row function) | 72.2% (`conv3x3_row<8>`) | 85.0% (`conv3x3_rowblock<8>`) |
| `conv2d_images` (staging, pool, image loop) | 19.7% | 4.9% |

- **HEAD:** retiring-bound (74% of the slots retired, almost all light operations), so the instruction count was the
  lever. Below avx512 no row pair was built: every conv row ran on its own as eight channels' `Vec8f` chains (per
  tap one row load, eight weight broadcasts and eight fmas), both rows of a pooled row were stored to the row
  buffers, and `max_pool2_row`'s general loops read them back (`conv2d_images` 19.7%).
- **The kept change:** at avx2 every row runs its two conv rows together, one 8-column group at a time and four
  channels a block (8 of the 16 `ymm` registers; two blocks for eight channels): per tap two row loads, four
  broadcasts and eight fmas, each broadcast shared by both rows. The pair pools in its registers (vertical maximum,
  `Vec8f`'s `deinterleave`, the pairs' maximum, one `xmm` store of four pooled columns a group), so no row reaches
  memory before the pool. Instructions -29%, cycles -17%; the process is now bound by the fma ports (core bound
  31%), the hot loop's samples on the `vfmadd` chain.

## Boards (ns a sample, speed ratio over the reference)

**avx2, HEAD the reference (25 repetitions, every cell faster):**

| row | HEAD | the kept change | ratio | pre-row | ratio |
|---|---|---|---|---|---|
| f32 batch 8 | 3,427 | 2,629 | **1.285 [1.213-1.305] faster** | 3,613 | 0.942 [0.933-0.961] slower |
| q8 batch 8 | 3,425 | 2,720 | **1.254 [1.223-1.259] faster** | 3,695 | 0.928 [0.920-0.933] slower |
| i8 batch 8 | 3,446 | 2,746 | **1.241 [1.211-1.266] faster** | 3,733 | 0.921 [0.917-0.942] slower |
| f32 batch 4096 | 3,110 | 2,371 | **1.325 [1.269-1.382] faster** | 3,311 | 0.974 [0.938-1.011] unresolved |
| q8 batch 4096 | 3,048 | 2,506 | **1.223 [1.198-1.269] faster** | 3,341 | 0.909 [0.853-0.944] slower |
| i8 batch 4096 | 3,097 | 2,514 | **1.239 [1.162-1.300] faster** | 3,425 | 0.914 [0.886-0.963] slower |

Against the pre-row engine the kept change is 1.33x to 1.40x (its medians over pre-row's in the same board; HEAD
was 1.03x to 1.10x).

**avx512, HEAD the reference (the control; 30 repetitions):** q8 batch 8 1.002 [0.984-1.009] and i8 batch 8 1.003
[0.988-1.009] level; f32 batch 8 1.012 [1.002-1.023], f32 batch 4096 1.011 [0.981-1.040], q8 batch 4096 0.982
[0.938-1.014] and i8 batch 4096 1.054 [1.012-1.093] unresolved, no cell slower. User cycles 4.12 G (HEAD) and 4.06 G.

**The layout floor, measured:** a first cut of the same change also rewrote the avx512 sinks' stores through one
helper (the same stores, a `>=` in place of an `==` on the column count). Its avx512 code differed from HEAD's in eight
functions with the same instruction count to within 100 a process, yet ran +1.8% user cycles (4.20 G against 4.12 G,
each `perf stat -r 3` within 0.1%) and 0.97x on the batch-8 cells; the kept form leaves those sinks as HEAD wrote
them and its avx512 code, also not byte-identical (GCC outlines the pair lambdas differently), runs -1.7% cycles. So
a code-identical-in-effect rebuild moves this program by about 2% either way: an avx512 cell inside that band is
layout, not the change. sse2's nn functions are byte-identical between HEAD and the kept change (`objdump`, addresses
masked).

## Correctness

Each lane of a pair register is one output column, accumulated over the same (channel, ki, kj) chain from zero, then
bias + chain and the relu, as `conv3x3_step`; the pool takes `max_pool2_row`'s maximum of the same two values and
then of the same column pair (the groups start at even columns, so no pair straddles two), so the bits are HEAD's at
every level. On the machine every `crd-hesap-tensor-kernels-at-<level>` and `crd-math-kernels-at-<level>` case passed
at portable, sse2, sse4.2, avx2 and avx512 (174), the convolution case now also with 24 columns at R 8 (two
four-channel blocks, three groups). Teeth, each failing the avx2 arm only and passing again restored: the second
channel block's sink without its channel offset; each group pooled at half its pooled column (`j0 / 4`).
