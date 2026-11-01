# 2026-11-01 — SIMD-PERF: avx2's conv rows as register pairs, pooled in registers

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf). The cnn item the [wide-rows
session](2026-11-01-simd-perf-cnn-wide-rows.md) left (both avx2 layers pooled through the row buffers,
`conv2d_images` 21% of avx2's samples), profiled again on HEAD before the change.
Board: [2026-11-01-simd-perf-cnn-avx2-pairs](../bench/2026-11-01-simd-perf-cnn-avx2-pairs.md).

## CI first

No mirror run has reported on this track's commits since the image-loop batch: every push run from 41246085460
(`f7fe700c`) to 41247018956 was cancelled by a later push while queued, 41247045494 (a dispatch at `67977fda`) was
queued and 41250301420 (`62a88407`) pending without jobs. The one run with job results, 41231558128 (`d5cc2ec0`),
predates every nn commit of this track since `58e6399d` (neither `878e32b0` nor `58e6399d` is its ancestor); the
[wide-rows session](2026-11-01-simd-perf-cnn-wide-rows.md) read and attributed its failures. Its failed job logs were
not readable this time (the run is still in progress). So the clang-tidy question that `b8ebdb2f` raised for the
avx2 and avx512 nn units (the wide pair, now `conv3x3_two_rows`) stays with the hosted win-tidy lane. On the machine,
Clang 18 compiled this batch's unit at every level it builds (portable, sse2, avx2, avx512 and the two VNNI
variants) with the GCC lane's flags and no diagnostic; clang-tidy is not installed there.

## Profile (the bound, with its evidence)

`bench_nn cnn` at avx2 on CPU 3, HEAD `62a88407`: 7.78 G user cycles, 36.70 G instructions (IPC 4.7), 1.59 M branch
misses; TopdownL1 retiring 74.4% (light operations 73.3%), core bound 18.6%, memory bound 2.3%, bad speculation
2.0%, frontend 2.7%. Samples: `conv3x3_row<8>` 72.2%, `conv2d_images` 19.7%. Retiring-bound, so the lever is the
instruction count: below avx512 every conv row ran alone (per tap one row load, eight broadcasts, eight fmas), and
both rows of a pooled row went through the row buffers and `max_pool2_row`'s general loops.

## The change

- **`kGroupPairs`:** at avx2 (a `Vec8f` is one register and no `Wide` pair exists) every row runs its two conv rows
  together, one 8-column group at a time from the left, at most `kPairChannels` (4) channels a block: 8 of the 16
  `ymm` registers, beside the two row loads and the broadcast. Eight channels take two blocks, the row loads repeated
  for the second; any width (the last group's columns stored partially).
- **`conv3x3_two_rows<V>`** is avx512's wide pair generalised over the register and a starting column (`j0`), so
  avx512 and avx2 share one fma body; **`conv3x3_pair<R>`** picks avx512's forms or avx2's groups and blocks.
- **The sinks:** `PairRows` and `PairPool` gain a group form `(r, top, bottom, j0)`: two stores at `j0`, or the
  vertical maximum, `Vec8f`'s `deinterleave`, the pairs' maximum and four pooled columns at `j0 / 2` (no pair
  straddles two groups, `j0` being even; an odd row's last group may hold only its unpooled column). avx512's forms
  keep HEAD's bodies: a first cut that stored through one shared helper changed avx512's code in eight functions and
  ran +1.8% cycles there with the same instructions (the layout floor, below).
- The test's convolution shapes gain 24 columns at R 8 (two blocks, three groups); the unit's header comment says
  where the pairs run.

## Results (board)

- **avx2:** 1.223x to 1.325x HEAD, every cell faster after 25 repetitions; 1.33x to 1.40x the pre-row engine (HEAD
  was 1.03x to 1.10x); user cycles 7.78 G to 6.44 G (-17%), instructions 36.70 G to 26.22 G (-29%), branch misses
  1.59 M to 0.11 M, `conv2d_images` 19.7% to 4.9%. The process is now core-bound (31.1%, retiring 65.8%): the
  samples sit on the fma chain, with `vmovaps` register copies beside it.
- **avx512 (control):** two cells level, four unresolved at medians 0.982 to 1.054, none slower; user cycles 4.12 G
  (HEAD) and 4.06 G.
- **The layout floor:** the first cut's avx512 code, differing from HEAD's only in which compares and branches the
  same stores used, ran +1.8% user cycles (each `perf stat -r 3` within 0.1%); the kept form, also not byte-identical,
  -1.7%. A rebuild that does not change what avx512 computes moves this program about 2% either way.
- **sse2:** its nn functions are byte-identical between HEAD and the kept change (`objdump`, addresses masked); the
  board started there was stopped after about 10 repetitions, unread, since the code is the same (each sse2 process
  runs about 125 us a sample, 40x avx2: its fmas are the exact emulation ADR-0134 requires without hardware fma).

## Checks

On the machine (linux-gcc-release lane tree at the candidate, no warning under the lane's `-Werror`): every
`crd-hesap-tensor-kernels-at-<level>` and `crd-math-kernels-at-<level>` case at portable, sse2, sse4.2, avx2 and
avx512, 174, all passing. Teeth on the kept form, each failing the avx2 arm only and passing restored: the second
channel block's sink without its channel offset; each group pooled at half its pooled column (`j0 / 4`). Clang 18's
syntax check of the unit at every level, clean. Locally: the plan, repository and Allman checks. The machine ran
1.33 h in one start and is stopped.

## Left

- **A generator for the hier codelets** (`hier_codelets.hpp` and `fft_hier_codelets_0.cpp`/`_1.cpp`,
  generator-absent under REPO.DEV.10), then the 4K in-place f32 forward and the 2M four-step's P1 gather and P2
  scatter fusions on it, each boarded against MKL and FFTW at both levels: the fft gaps are now the row's widest.
- The jobs retune's boards per level.
- avx2's conv pair is fma-port-bound with register copies beside the chain: a lever only on a profile that shows
  them costing cycles (annotate first).
- torch and onnxruntime have no pin in `cmake/pins.json`: pin them and board the mlp and cnn rows against them.
