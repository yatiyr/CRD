# 2026-11-01 — SIMD-PERF: avx512's 16-column conv rows pooled in registers, and the fft gaps re-read

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf). The item the [image-loop
session](2026-11-01-simd-perf-cnn-pool-in-register.md) left first (the first conv layer's 16-column rows at avx512,
`conv2d_images` 25% of the avx512 samples), chosen after the fft gaps were measured again on the same start and their
levers found to need codelets nothing can regenerate.
Board: [2026-11-01-simd-perf-cnn-wide-rows](../bench/2026-11-01-simd-perf-cnn-wide-rows.md).

## CI first

No mirror run has reported on this track's last commits: every push run since 41227536389 was cancelled by a later
push while queued, and 41246085460 (`f7fe700c`, the image-loop batch) was pending without jobs. The one run still
going, 41231558128 (`d5cc2ec0`), had failed its build and test jobs on causes outside this track's code, read from the
job logs: `durable_file.cpp:848`'s `(void)::ftruncate` on the Linux GCC lanes (replaced on main by this track's
`be0adf3f`, which `d5cc2ec0` predates), `cpu_features.cpp:1500` and `:1513` (`-Wshadow`, linux-arm64 clang; the
serial workflow's `1667ad60`), `test_register_ids.cpp:51` (C4389 on win-debug; the serial workflow's `2782ab9e`,
after `d5cc2ec0`, changes that line) and three clang-cl test failures in the serial workflow's open files (cpu
identity, hardware identity, the comply verb). Nothing of this track's to fix. MSVC, clang-cl and ARM64 have built
neither `Vec8f`'s `deinterleave` (2f51cb33) nor this batch's `Vec16f` one yet; both read members every backend has
(the AVX2 `v`, the others `lo` and `hi`; the scalar arm only `lane`).

## Target choice (measured, then decided)

- **fft, re-read on this start**
  ([board](../bench/2026-11-01-simd-perf-cnn-wide-rows.md#the-fft-gaps-re-read-on-the-same-start-no-change)): the f32 4K
  forward is still the widest peer gap (0.73x MKL at avx512, 0.69x at avx2), then 2K, 16K and 8K; the f64 forward trails
  FFTW at avx2 from 2M (0.77x) and at 128K and 256K (0.75x, 0.83x). The 2M four-step's counters put P2's fused 32 x 32
  path at 45% (avx2) and 42% (avx512), P1's 2048-point sub-FFT at 28% and 25%. `bench_fft_vs_refs` restores every
  in-place contender's input untimed, so its peer ratios are not biased against Cerid; the `prof_fft_kernel` loop is not
  comparable with them (no restore).
- The levers these name (P1's 2048-point gather fused like the 1024-point one, P2's stage 2 storing the output in place
  of the scatter, the 4K in-place forward with bodies that keep their points in registers) all need new codelets in
  `hier_codelets.hpp`, which is generator-absent (REPO.DEV.10: the bytes are the artifact). Hand-copying a 17,000-line
  unit's bodies is not the proper way; a committed generator that reproduces the header and its two units byte for
  byte is the step before any of them (Left).
- The avx512 arm rows ahead of the plan on this machine (2K standalone-hier, 4M six-step, 32K and 64K deep-split-4) stay
  evidence: batch 1 recorded the avx512 fft plan table as QUAL.1-HW's clause, not `level_plans_gen.hpp`'s.
- So this batch took the cnn's first layer: a lever the last profile names, inside this track's files, bit-exact.

## Profile (the bound, with its evidence)

`bench_nn cnn` at avx512 on CPU 3, HEAD `36792923`: 5.22 G cycles, 17.99 G instructions, 1.71 M branch misses;
`conv2d_images` 23.8% of the samples, the convolution's row blocks 63.9%. The second layer's 8-column rows already
ran two rows a `Vec16f` and pooled in it; the first layer's 16-column rows ran each conv row as its own block (eight
`Vec16f` accumulators, every input row loaded again for the second row), stored both rows to the row buffers and
pooled them through `max_pool2_row`'s loops: the stores, reloads, the pair split and the loop counters were the
`conv2d_images` share.

## The change

- **`conv3x3_wide_pair`:** for rows of nine to sixteen columns at avx512 the two conv rows of eight output channels
  run in sixteen `zmm` accumulators (16 of 32), each row's tap loaded once for the eight channels; each channel's
  two registers go to the same sinks as the narrow pair through a second call form: `PairRows` stores the two rows,
  `PairPool` takes their maximum, splits it into even and odd columns and stores the pooled row (one `ymm` store of
  eight columns, a partial store for fewer). `row_pairs` now holds for rows up to `Wide`'s columns, and the row
  block picks the narrow or the wide pair by the row's width.
- **`deinterleave(Vec16f, Vec8f&, Vec8f&)`** in `crd/math/simd/vec16f.hpp`: one `vpermps` and the two halves at
  avx512, each `Vec8f` half's own split below; in the SIMD probe at every level.
- `bench_nn.cpp`'s comment named `scripts/v14m_nn_peers.py`, which left the tree in the July cleanup: it now names
  the 2026-07 board and the missing pins.
- `check_nn_conv`'s comment had a line of 141 columns: rewrapped with the new shapes (12 and 16 columns at R 8 and
  8 + 8, even heights).

## Results (board)

- **avx512:** 1.185x to 1.341x HEAD, every cell decided after 10 repetitions; 1.86x to 2.03x the pre-row engine's
  AVX2 program (HEAD was 1.52x to 1.66x); cycles 5.22 G to 4.17 G, instructions 17.99 G to 14.01 G, `conv2d_images`
  23.8% to 11.5% of the samples.
- **avx2 (control, the same code):** four cells level, two unresolved with intervals across 1 (0.994 and 1.003).

## Checks

On the machine (linux-gcc-release lane tree at the candidate, no warning under the lane's `-Werror`): every
`crd-hesap-tensor-kernels-at-<level>` and `crd-math-kernels-at-<level>` case at portable, sse2, sse4.2, avx2 and
avx512, 174, all passing. Teeth: storing the even columns in place of the pooled ones in the wide pool fails the
avx512 arm only; the restored source rebuilt and passed. Not run this batch: Clang's syntax check of the unit at every
level (the machine was stopped first); the hosted clang, clang-cl and MSVC lanes build it. Locally: the plan,
repository and Allman checks. The machine ran 0.66 h in one start and is stopped.

## Left

- **A generator for the hier codelets** (`hier_codelets.hpp` and `fft_hier_codelets_0.cpp`/`_1.cpp`, generator-absent
  under REPO.DEV.10): reproduce today's bytes from a committed script, flip its `generated-sources.json` status, then
  build on it the 4K in-place f32 forward (rows through the register transpose, column-batched passes, radix-16 bodies
  at avx512 and radix-8 at avx2), P1's 2048-point gather fusion and P2's scatter-fused stage 2 at 2M, each boarded
  against MKL and FFTW at both levels.
- The cnn at avx2: both layers still pool through the row buffers (`conv2d_images` 21% of avx2's samples last
  batch); 16 `ymm` registers do not hold two rows by eight channels, so its lever is another blocking, profiled first.
- torch and onnxruntime have no pin in `cmake/pins.json`: pin them and board the mlp and cnn rows against them.
- Then the jobs retune's boards.
