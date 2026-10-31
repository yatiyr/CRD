# 2026-10-31 — SIMD-PERF: the cnn rows against the pre-row engine, the fused pool and the staging (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-10-31](../sessions/2026-10-31-simd-perf-cnn-pool-staging.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), Ubuntu 24.04, kernel 7.0.0-1014-gcp, GCC 13.3, perf with the guest PMU. **Real AVX-512 hardware.**
  `build-bench.py host`: 28.7 of 29.4 GiB available, other processes 0.0 cores, idle. Every process pinned to CPU 3.
- **Binaries:** the pre-row engine `2f2aed72`'s own `bench_nn` (`scripts/build-prerow.sh`, its linux-gcc-release
  preset, AVX2 code); HEAD, the engine of `7a2eb462` with the platform build fix of this session, as
  `~/ab/nn-head/bench_nn`; the pool candidate (`~/ab/nn-cand`) and the kept change, pool and staging
  (`~/ab/nn-cand2`), both from the linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO).
  `CRD_CPU_LEVEL` selects the hesap-tensor kernels' level; pre-row predates dispatch and runs its compiled AVX2.
- **Programs:** each engine's own `bench_nn` (frozen CNN: conv 1 to 8 channels on 16 x 16, relu, 2x2 pool, conv 8 to
  16 on 8 x 8, relu, pool, fc 256 to 10, softmax; f32, Q8_0 and i8 tiers; batch 8 and 4096; best of 50 whole-batch
  inferences, ns a sample). HEAD's input is the corpus rows repeated, pre-row's Philox values in [-1, 1]; no value
  is subnormal, and the convolution's work does not depend on the values. HEAD's and the candidates' processes run
  `bench_nn cnn` (the new part argument), pre-row's its whole program (mlp then cnn).
- **Method:** `scripts/ab-board.py`, one warm-up, 5 to 25 repetitions in rotated order, median paired speed ratio
  [percentile-bootstrap 95%], 2% band; the arm faster above 1. Profiles: `perf record` (cycles) of one process
  each, symbol sample counts (`perf report -n`), pre-row's restricted to its cnn part (`--time` from its first
  convolution sample), and `perf annotate` of the convolution's loop.

## Where the time went (HEAD at avx2 against pre-row, samples in the cnn part)

| part | pre-row | HEAD | the pool candidate | the kept change |
|---|---|---|---|---|
| the convolution's row block (fma loop) | 8,087 | 8,256 | 8,624 | 7,948 |
| staging and pool (`conv_run`; HEAD `conv2d_images` + `max_pool2_row`) | 2,099 | 3,400 | 1,992 | 2,091 |
| `memmove` | 419 | 640 (whole program) | 402 | 454 |
| the linears, quantizers, softmax, fc GEMM | 730 | (with the mlp's) | 692 | 695 |

HEAD's profile ran its whole program (mlp, cnn and the act rows; the convolution and the pool run in the cnn part
only); the other columns are the cnn part alone.

- **The pool:** `max_pool2_row` was a call a pooled row (64 a layer an image), took the IEEE maximum's compare and
  select chain on rows the relu had already made NaN-free and without -0, and ran a short row (the second layer's
  8 columns, 4 pooled) through zero-filled stack copies, a `memmove` call and a masked partial store.
- **The convolution:** its fma loop matched pre-row's instruction for instruction (24 broadcasts and 24 fmas a tap
  row), but each `ki` step computed its source row as `rows + (ci * h + ih) * rs` (two `imul`s, a bounds branch,
  `c.h`, `s.rows` and `s.rs` reloaded from memory) where pre-row's advanced a pointer.

## The change

The pool runs one call a row block, whole registers of each width down to four lanes (a new `Vec4f`
`load_complex_deinterleaved`) and single columns for the rest, and after a relu the plain maximum, which on rows
without NaN and -0 gives the IEEE maximum's bits. The staging puts each channel's rows between two zero rows, so a
tap is the next padded row and a channel the next plane: no branch, no multiply in the loop. The scratch slot holds
the two zero rows a channel (`nn_conv2d_scratch_floats`). The bits are unchanged: the nn and tensor kernel cases
(conv against the scalar fma chain with NaNs, infinities, signed zeros and subnormals at widths 1 to 33, with and
without relu and the fused pool, at every level the machine runs) pass.

## Boards (ns a sample, speed ratio over the reference)

**avx2, 25 repetitions (pre-row the reference):**

| row | pre-row | HEAD | ratio | the kept change | ratio |
|---|---|---|---|---|---|
| f32 batch 8 | 3,613 | 4,329 | **0.834 [0.823-0.839] slower** | 3,664 | 0.990 [0.982-1.008] level |
| q8 batch 8 | 3,694 | 4,286 | **0.862 [0.857-0.864] slower** | 3,670 | 1.007 [0.992-1.013] level |
| i8 batch 8 | 3,728 | 4,343 | **0.858 [0.856-0.861] slower** | 3,682 | 1.013 [0.986-1.019] level |
| f32 batch 4096 | 3,601 | 4,248 | **0.843 [0.831-0.864] slower** | 3,729 | 0.972 [0.962-0.980] unresolved |
| q8 batch 4096 | 3,738 | 4,281 | **0.873 [0.861-0.879] slower** | 3,703 | 1.008 [0.981-1.018] level |
| i8 batch 4096 | 3,734 | 4,363 | **0.855 [0.842-0.862] slower** | 3,662 | 1.013 [0.998-1.021] unresolved |

The pool alone (15 repetitions, an earlier board): 0.930 to 0.975 of pre-row, four cells unresolved.

**The f32 batch-4096 cell** is the noisiest row of both programs: each process's best of 50 spread 15% over 25
processes (pre-row 3,161 to 3,688, the kept change 3,258 to 3,748, its q8 row 3,285 to 3,723), and its pre-row
median moved from 3,191 to 3,601 across boards. A board of the kept change running pre-row's order (`mlp cnn`)
put it at 0.962 [0.942-1.002], the other five cells 0.990 to 1.022. The profile above holds the convolution and the
pool level with pre-row's, and the fc GEMM and softmax take the same samples in both; no f32-only cost remains
that the profile names.

**avx512 (the candidate's kernels at the machine's level):**

| row | HEAD | the kept change | ratio over HEAD | pre-row | ratio over pre-row |
|---|---|---|---|---|---|
| f32 batch 8 | 4,118 | 3,479 | 1.186 [1.166-1.300] faster | 3,609 | 1.048 [1.034-1.063] faster |
| q8 batch 8 | 4,205 | 3,498 | 1.198 [1.167-1.217] faster | 3,692 | 1.041 [1.028-1.053] faster |
| i8 batch 8 | 4,176 | 3,408 | 1.225 [1.159-1.233] faster | 3,725 | 1.094 [1.087-1.116] faster |
| f32 batch 4096 | 4,265 | 3,449 | 1.236 [1.231-1.257] faster | 3,546 | 1.059 [1.023-1.071] faster |
| q8 batch 4096 | 4,172 | 3,363 | 1.240 [1.210-1.243] faster | 3,665 | 1.099 [1.069-1.129] faster |
| i8 batch 4096 | 4,143 | 3,319 | 1.249 [1.246-1.290] faster | 3,595 | 1.139 [1.073-1.158] faster |

(HEAD over the kept change: 5 repetitions; against pre-row's AVX2 program: 10.)

**sse2 and portable (HEAD the reference, 5 repetitions):**

| row | HEAD sse2 | kept | ratio | HEAD portable | kept | ratio |
|---|---|---|---|---|---|---|
| f32 batch 8 | 146,877 | 145,268 | 1.011 [1.010-1.012] level | 243,507 | 221,592 | 1.099 [1.098-1.100] faster |
| q8 batch 8 | 142,085 | 140,398 | 1.012 [1.012-1.012] level | 235,275 | 213,957 | 1.100 [1.099-1.263] faster |
| i8 batch 8 | 141,070 | 139,394 | 1.012 [1.012-1.013] level | 234,831 | 213,318 | 1.101 [1.051-1.104] faster |
| f32 batch 4096 | 147,398 | 145,999 | 1.011 [1.008-1.017] level | 239,768 | 222,954 | 1.075 [1.070-1.078] faster |
| q8 batch 4096 | 142,960 | 141,161 | 1.012 [1.010-1.015] level | 231,802 | 214,500 | 1.081 [1.076-1.088] faster |
| i8 batch 4096 | 141,787 | 139,933 | 1.012 [1.012-1.015] level | 231,153 | 214,270 | 1.079 [1.075-1.087] faster |

At sse2 a sample costs 40x avx2's: the convolution's single-rounded f32 fmas run the exact emulation
(`fma_exact.hpp`, the SSE2 floor's price, as the [sse2 boards](2026-10-31-simd-dispatch-sse2-boards.md) recorded);
portable calls the C library's fma a lane.

## Verdict

The regression is fixed at its two causes with the same bits: at avx2 five cells level with pre-row (0.990 to
1.013) and the f32 batch-4096 cell at 0.962 to 0.972 in a row whose processes spread 15%, with no remaining cost
the profile names; at avx512 1.19x to 1.25x HEAD and 1.04x to 1.14x the pre-row engine; sse2 level, portable 1.08x
to 1.10x. Not measured here: torch and onnxruntime, which the machine does not have and `cmake/pins.json` does not
pin yet. Logs and values on the machine: `~/boards/nn` (`avx2-final`, `avx2-order`, `avx2-c2`, `avx512`,
`avx512-pre`, `sse2`, `portable`; `*.json` in `cerid-ab-board/1`).
