# 2026-10-31 — SIMD-PERF: the cnn rows against the pre-row engine

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 2 (the cnn rows' 11% to 16% against the pre-row engine
at avx2, from the [sse2 boards](../bench/2026-10-31-simd-dispatch-sse2-boards.md)). Board:
[2026-10-31-simd-perf-cnn-pool-staging](../bench/2026-10-31-simd-perf-cnn-pool-staging.md). Before:
[the f32 forward's 2K to 16K kernel](2026-10-31-simd-perf-fft-small-kernel-profile.md).

## Why this target

The row's acceptance rule ranks a regression against the pre-row engine above a peer gap (fixed at its cause, where
a peer lead gets one evidence-backed attempt), and the cnn rows were the regression still open in item 1's list. The
fft f32 forward's 4K gap to MKL stays the next fft lever (the in-place forward, a generator project whose three
predecessor levers did not move 4K); it is parked, not dropped. The sse2 forward's 4.9x to 6.0x against its own
inverse was looked at first: its sub-FFTs run the generated codelets, whose single-rounded fmas are the exact
emulation at sse2, while the inverse's radix-8 passes multiply and add separately. A fix is a new `FftAlgorithm`
chosen by a level-table row, and `gen_fft_level_plans.py` takes its rows from the i9's boards only; how a benchmark
machine row enters that table is decided first (Left).

## CI first

No mirror run has results for this track's commits: push run 41224375222 (`ba8bfba3`) was pending with no jobs and
every earlier push run was cancelled by the next push. Nothing to fix from CI.

## Found on the way: the Linux build

The lane tree's first build failed: `durable_file.cpp`'s `Journal::append` cast `ftruncate`'s result to `void`, and
GCC's `warn_unused_result` (glibc's fortified declaration on Ubuntu) ignores the cast, so `-Werror` stopped
linux-gcc-release; every Linux lane of the hosted CI builds the same way. The failed append now checks the cut on
both platforms and closes the journal when the cut fails, since a later record would follow bytes replay stops at
(`fix(platform)`, the header documents it).

## Profile (the bound, with its evidence)

Each engine's `bench_nn` on CPU 3, `perf record` and sample counts by symbol, pre-row's restricted to its cnn part:

- **The convolution's fma loop was level** (pre-row 8,087 samples, HEAD 8,256): the same 24 broadcasts and 24 fmas a
  tap row in both, but HEAD's `ki` step computed the row as `rows + (ci * h + ih) * rs` with two `imul`s, the bounds
  branch and `c.h`, `s.rows` and `s.rs` reloaded through the references, where pre-row advanced a pointer.
- **The staging and pool took 3,400 samples against pre-row's 2,099**, and `memmove` 640 against 419:
  `max_pool2_row` was a call a pooled row (64 a layer an image), took the IEEE maximum's select chain on rows the
  relu had already cleared of NaN and -0, and ran the second layer's 8-column rows (4 pooled) through zero-filled
  stack copies, a `memmove` call and a masked partial store.

## The change

- **The pool:** one call a row block (`max_pool2_rows`), whole registers of each width down to four lanes (a new
  `Vec4f` `load_complex_deinterleaved`, SSE2, NEON and scalar, in the level probe) and single columns for the rest;
  after a relu the plain maximum, whose bits on rows without NaN and -0 are the IEEE maximum's. The stand-alone 2x2
  pool takes the same row body with the IEEE maximum, so no copy or `memmove` either.
- **The staging:** each channel's rows sit between two zero rows (`nn_conv2d_scratch_floats` counts them), so a tap
  is the next padded row and a channel the next plane: no branch or multiply in the loop. The fma chain and its
  order are unchanged, the zero rows giving the same fmas with zero the old shared zero row gave.

The first board (the pool alone) put the cnn rows at 0.930 to 0.975 of pre-row; the second profile then showed the
convolution's tap rows, and with the staging its loop took 7,948 samples against pre-row's 8,087.

## Results (board)

- **avx2 against pre-row (25 repetitions):** HEAD 0.834 to 0.873; the kept change level in five cells (0.990 to
  1.013) and the f32 batch-4096 cell at 0.972 [0.962-0.980]. That cell is the noisiest of both programs (each
  process's best of 50 spread 15% over 25 processes; pre-row's median moved from 3,191 to 3,601 ns across boards),
  and in pre-row's part order (`bench_nn mlp cnn`) it read 0.962 [0.942-1.002]; the profile holds the convolution,
  the pool, the fc GEMM and softmax level with pre-row's, so no f32-only cost is left that it names.
- **avx512:** 1.19x to 1.25x HEAD; 1.04x to 1.14x pre-row's AVX2 program.
- **sse2:** level with HEAD (1.011 to 1.012); a sample costs 40x avx2's there, the f32 fma emulation.
- **portable:** 1.08x to 1.10x HEAD.

## What landed besides

`bench_nn` takes `mlp`, `cnn` and `act` (all of them without one): a cnn-only process takes about 3 s at avx2
against the whole program's 4.6 s, and its profile leaves out the mlp and the activation spans' erf.

## Checks

On the machine (linux-gcc-release lane tree at the candidate): `crd-hesap-tensor-tests`, `crd-hesap-tensor-nn-tests`
and `crd-math-tests`, every `^nn: `, `tensor kernels`, kernel-level-arm and simd case (307, all passing; the conv
case checks the direct kernel against the scalar fma chain with NaNs, infinities, signed zeros and subnormals at
widths 1 to 33, with and without relu and the fused pool, at every level the machine runs; the pool case widths 2 to
257; the level probe the new split load at every level). Every board process passed its tier bound against the f32
reference. Locally: the plan, repository and Allman checks. The machine ran 1.58 h in one start and is stopped.

## Left

- The f32 batch-4096 cell: a board whose two programs share one input and one part order (pre-row's `bench_nn`
  takes no part argument) would separate the engine from the program; nothing in the profile is left to fix.
- torch and onnxruntime have no pin in `cmake/pins.json` and are not on the machine: pin them (pip wheels, SHA-256
  checked) and board the mlp and cnn rows against them there (the i9 had onnxruntime ahead at cnn batch 4096, 0.96).
- avx512's second convolution layer is 8 columns wide, so it runs on `Vec8f` with half of each register idle: two
  output rows a `Vec16f` would halve its steps (same bits, the lanes are independent columns).
- The sse2 floor's f32 fma emulation (40x avx2 on the cnn), with the sse2 fft forward (4.9x and 6.0x its own
  inverse): first decide how the benchmark machine's boards enter `level_plans_gen.hpp`.
- The fft items: the in-place f32 forward for 2K to 8K, the 2K plan, the four-step's P2 at 2M, the 4M six-step; then
  the jobs retune's boards.
