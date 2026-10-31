# 2026-11-01 — SIMD-PERF: the cnn image loop, the row pair pooled in its register and rows staged without a call

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), the item the [row-pair session](2026-10-31-simd-perf-cnn-row-pairs.md)
left (`conv2d_images` a quarter of the cnn samples: the staging's per-row calls and the fused pool's trip through
the row buffers). Board: [2026-11-01-simd-perf-cnn-pool-in-register](../bench/2026-11-01-simd-perf-cnn-pool-in-register.md).

## CI first

The mirror's push runs for this track's last commits (`878e32b0`, `30118416`, `a019687a`) were cancelled by the
next push or still pending: since 41227536389 every push run on main has been cancelled by a later push before it
reported, so no recent commit of either workflow has a completed verdict. The one run still going, 41231558128
(`d5cc2ec0`, the serial workflow's), had failed 12 build jobs on two causes, both read from the job logs:
`durable_file.cpp:848`, the `(void)::ftruncate` that `-Werror=unused-result` refuses on the Linux GCC lanes, already
replaced on main by this track's `be0adf3f` (the journal closes when it cannot cut a failed append), and
`cpu_features.cpp:1500` and `:1513` (`-Wshadow` on the four linux-arm64 clang lanes), in the serial workflow's OWN.1
file, fixed on main by its `1667ad60`. Nothing of this track's to fix.

## Profile (the bound, with its evidence)

`bench_nn cnn` on CPU 3, HEAD `a019687a`, `perf record --call-graph dwarf`: at avx512 5.75 G cycles and 20.1 G
instructions, retiring 64.5% and frontend-bound 16.4% (5.9% at avx2); the convolution's row blocks 55.8% of the
samples, `conv2d_images` 26.9%, and the C library's copies and fills 8.7% (`__memmove_avx512_unaligned_erms` 6.2%,
`memset` 1.1%, their PLT stubs 1.4%), every one called from `conv2d_images`: a `memcpy` of 8 or 16 floats a staged
row and the padding zeroed again for every image. Inside `conv2d_images` the hot lines were the fused pool's: the
second layer's row pair stored from its `Vec16f` into two row buffers, then read back by `max_pool2_row`'s general
loops (the `vshufps`/`vpermps` pair split, the vertical `vmaxps`, the loop counters). At avx2 the same calls took
5.6% and `conv2d_images` 18.2%.

## The change

- **The row pair pooled in its register:** `conv3x3_row_pair` hands each output channel's register (bias + chain and
  the relu applied) to a sink: `PairRows` stores the two conv rows as before, `PairPool` takes the two rows'
  maximum (`low_half`, `high_half`), splits it into its even and odd columns and takes their maximum, then stores
  the pooled row (one `Vec4f` store for four columns, lane by lane for fewer). The same operations on the same
  operands as `max_pool2_row`, so the same bits; the row buffers and their reloads are gone for rows of at most
  eight columns at avx512.
- **`deinterleave(Vec8f, Vec4f&, Vec4f&)`:** the register form of `Vec4f`'s split load in `crd/math/simd/vec8f.hpp`
  (AVX2 `vextractf128` and two `vshufps`, SSE the two halves' `shufps`, NEON `vuzpq_f32`, scalar lanes), in the
  SIMD probe at every level.
- **Staging without a call:** the staged image's zero rows and columns are written once a call
  (`stage_zero`; the scratch is the call's, and each image writes only its input columns), and each row is copied
  in whole registers (`copy_row`: `Wide`, `Group`, `Vec4f`, then one partial group), no `memcpy` a row.

## Results (board)

- **avx512:** 1.093x to 1.112x HEAD (the staging alone 1.018x to 1.041x), 1.50x to 1.66x the pre-row engine's AVX2
  program; cycles 5.75 G to 5.23 G and instructions 20.1 G to 18.0 G a process.
- **avx2:** 1.047x to 1.068x HEAD, every cell faster; against pre-row 1.030x to 1.097x, the f32 batch-8 cell 1.027
  unresolved (lower bound 1.013): the f32 batch-4096 cell that was 0.962 to 0.972 is now 1.030x faster, and no avx2
  cell trails pre-row.
- **sse2 and portable:** medians 0.984 to 1.003, every cell unresolved on a start whose processes spread 4% to 6%;
  the change's part there is under 1% of the time.

## Checks

On the machine (linux-gcc-release lane tree at the candidate): every `crd-hesap-tensor*` case matching nn, kernel,
level or simd and every `crd-math-kernels-at-<level>` case, 464, all passing at every forced level. The convolution
case adds a call over both images at once with the scratch filled with guard words first, and pools rows of 2, 5, 7
and 8 columns with and without the relu. Teeth: skipping the once-a-call zeroing fails every level's arm (a first
form that removed the call failed to compile on `-Werror=unused-function` and so tested nothing; redone by emptying
the body); storing the even columns in place of the pooled ones fails the avx512 arm only. Clang 18
(`-fsyntax-only`, the lane's flags) on the unit at all six levels: clean. The avx512 object's disassembly: no
`rep stos`, no copy call, the pooled pair's instructions on the accumulator registers. Locally: the plan,
repository and Allman checks. The machine ran 1.46 h in one start and is stopped.

## Left

- The first conv layer's 16-column rows at avx512 and both layers at avx2 still pool through the row buffers:
  `conv2d_images` is 25% of the avx512 samples and 21% of avx2's. At avx512 the two conv rows of a 16-column row
  block fit 16 `zmm` accumulators (two rows by eight channels) and could pool in registers as the pair now does;
  avx2's 16 registers do not hold that, so its lever is a different blocking, to be profiled first.
- torch and onnxruntime have no pin in `cmake/pins.json`: pin them and board the mlp and cnn rows against them.
- How the benchmark machine's boards enter `level_plans_gen.hpp` (the sse2 fft forward's fma emulation), then the
  fft items and the jobs retune's boards.
