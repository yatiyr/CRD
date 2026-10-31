# 2026-10-31 — SIMD-PERF: the cnn rows' second convolution at avx512, two conv rows a register

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), the avx512 item the [cnn session](2026-10-31-simd-perf-cnn-pool-staging.md)
left (the second convolution layer's 8-column rows on `Vec8f`). Board:
[2026-10-31-simd-perf-cnn-row-pairs](../bench/2026-10-31-simd-perf-cnn-row-pairs.md).

## CI first

Push run 41235146793 (`58e6399d`, this track's last commit) was pending with no jobs. Run 41231558128 (`d5cc2ec0`,
the serial workflow's) failed seven build lanes on two causes outside this track: `cpu_features.cpp:1500`
(`implementer` shadows a local, `-Werror=shadow`) on the four linux-arm64 lanes, from the OWN.1 move, and
`tests/foundation/net/test_register_ids.cpp(51)` (C4389, a signed/unsigned `==`) on win-debug, win-debug-scalar and
win-arm64-debug. Both files belong to the serial workflow's live rows, so they are reported, not edited here.

## Profile (the bound, with its evidence)

`bench_nn cnn` at avx512 on CPU 3: retiring-bound (62.9% of slots; light operations 50.8%, core-bound 17.1%,
memory-bound 3.5%), 26.2 G instructions in 7.94 G cycles; `conv3x3_rowblock<8>` 68% of the samples, most of them in
its 256-bit loop. The second layer's rows are 8 columns, below `Wide`'s 16, so at avx512 they ran on `Vec8f`: per
tap row 3 loads and 24 `vfmadd231ps mem{1to8}` on ymm registers, 9,216 fmas an image against the first layer's
1,152 zmm fmas, each 512-bit FMA slot doing half its work.

## The change

- **Two conv rows a register:** where `Wide` is twice `Group` (avx512) and the row has at most eight columns,
  `conv3x3_row_pair` keeps conv row oi in a `Vec16f`'s low lanes and oi + 1 in its high lanes (the next padded row,
  joined by `vinsertf32x8`), so a tap row costs 3 loads, 3 inserts and 24 zmm fmas for both rows. The lanes are
  independent columns running the same chain, so the bits are unchanged. The image loop hands each row-block call
  two rows (the fused pool's conv rows 2 oi2 and 2 oi2 + 1, or the next two output rows); other levels and wider
  rows run the two rows one after the other as before.
- **Accumulators as registers:** the first form looped over the channels; GCC left the eight accumulators in a stack
  array zeroed by `rep stosq` on every call (4.3% of the function's samples), because the output loop was not
  unrolled. The channels now expand as a pack (`std::integer_sequence`), every accumulator a register, no stack
  traffic (read from the unit's disassembly).

## Results (board)

- **avx512:** 1.18x to 1.36x HEAD, 1.30x to 1.35x the pre-row engine's AVX2 program (HEAD had been 1.01x to 1.15x);
  cycles 7.94 G to 5.99 G and instructions 26.2 G to 20.1 G a process. The packs against the loops: 0.991 to 1.031,
  two cells faster, none slower.
- **avx2:** 1.007 to 1.031 of HEAD, nothing slower (the row pair is not built there).

## Checks

On the machine (linux-gcc-release lane tree at the candidate): every `crd-hesap-tensor*` case matching nn, kernel,
level or simd, 402, all passing at every forced level; the convolution case's shapes gain rows of 2, 5 and 8
columns at R 6, 7 and 8 + 8 with even and odd heights; a teeth edit (the low half stored into both rows) failed the
avx512 arm. Every board process passed its tier bound. Clang 18 (`-fsyntax-only`, the lane's flags) on the unit at
every level: the pushed form's output step, a plain inline function, was unused below avx512 and failed
`-Werror,-Wunused-function` on the five other levels (the clang lanes); it is now a template like the pair, every level
clean, and the avx512 object's disassembly identical to the boarded build's. Locally: the plan, repository and
Allman checks. The machine ran 0.52 h in two starts and is stopped.

## Left

- `conv2d_images` (the staging's per-row `memcpy` calls and the fused pool's trip through the row buffers) is now
  25% of the samples and `memmove` 5.6%: with both conv rows in one register, the pool's vertical maximum is
  `max(low, high)` before any store, and the 8- and 16-column rows can be staged without a call a row.
- torch and onnxruntime have no pin in `cmake/pins.json`: pin them and board the mlp and cnn rows against them.
- How the benchmark machine's boards enter `level_plans_gen.hpp` (the sse2 fft forward's fma emulation), then the
  fft items and the jobs retune's boards.
