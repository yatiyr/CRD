# 2026-10-28 — SIMD-DISPATCH: SSE2 bodies of the hesap-tensor family

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).
HEAD before: `342ff7da`.

## CI first

- Push run 40581144513 (`59c79f2f`, still running): its failed GCC and Clang jobs are the `GemmBlock` `beta`
  initializers (`syrk_microkernel.hpp:69`, log of linux-gcc-public-checks) that `90a87169` fixed. Every later push run
  was cancelled before a job failed; complete-tier run 40592200636 (`d57b11cd`) is in progress. DIAG.11c and DIAG.12
  stay Needs CI until a complete-tier run finishes.

## Tier R ladder on SSE2

- `binned_sum_f64` (`kernels/tensor_kernels.cpp`) deposits through one register trait per level (`BinnedIsa`: load,
  store, add, sub, or, and, max): three streams of four lanes on AVX2 (unchanged), three of two on SSE2 and SSE4.2.
  The step, the super-block (`kBinEndurance` steps) and the vector gate (eight steps) follow the level. The binned
  representation is partition-independent, so the lane and stream count cannot change a rung.
- Tests: the binned differential adds lengths around the SSE2 gate (47, 48, 49, 53) and its super-block, and a late
  magnitude in the SSE2 body's second super-block (its speculative rollback); the AVX2 cases stay. Local, win-debug:
  `[kernels]` passes forced at sse2 and avx2, `[repro]` at sse2.

## SSE2 and SSE4.2 nn bodies

- hesap-tensor builds an sse4.2 level (`LEVELS portable sse2 sse42 avx2`, table `kTensorKernelsSse42`); its permute,
  binned and Philox bodies are the sse2 ones recompiled.
- Four-lane bodies on both levels: softmax's exp as two `crd_exp_x2` per four values; the Q8_0 and u8 quantizers on
  SSE2 operations (`packs_epi32` then `packus_epi16` is the [0, 255] clamp, as both saturations are monotone); the
  Q8_0 dot keeps lanes 0-3 and 4-7 of each output in two registers and sums the pinned tree from them; the int8 and
  packed int8 dots add exact i32 lanes and finish four outputs at a time through one fma.
- Integer lanes: SSE2 has no u8 x i8 multiply, so it sign- or zero-extends the even and odd bytes to i16 and adds the
  two `pmaddwd` results (lane k = products 4k..4k+3). SSE4.2 runs `pmaddubsw` with the AVX2 bodies' split of u8
  activations (x = xl + 128 xh). Every f32 combine is `fma_exact.hpp`'s `fma_ps`.
- Finding: the AVX2 and AVX-VNNI Q8_0 dots multiplied |x| by w with x's sign, and `psignb` wraps -w for w = -128, so a
  -128 weight times a negative activation gave the product's negation. Raw imports (`add_linear_q8_raw`,
  `nn_q8_blocks_from_raw`) accept any i8. Every vector body now multiplies |w| (`pabsb` reads -128 as 128) by x with
  w's sign; the activations are `quantize_q8_0`'s, |x| <= 127, so nothing wraps and no i16 pair sum saturates.
- Tests: the Q8_0 linear's weights cover every i8, -128 included, and two weight scales are subnormal. Local,
  win-debug: the tensor `[kernels]` cases pass forced at portable, sse2, sse4.2 and avx2 (each run compares every level
  the host runs, the VNNI variant included), and every `crd-hesap-tensor-nn-kernels-at-<level>` arm passes.

## Board candidates (row-close pass)

- The sse2 Tier R ladder against portable and ReproBLAS (portable was 6.5x behind ReproBLAS); the stream count is a
  board choice.
- The sse2 and sse4.2 nn bodies against portable (portable Q8_0 was 13x behind avx2); the Q8_0 tile (eight outputs
  hold sixteen accumulators, more than the sixteen xmm registers with temporaries) and whether the sse4.2 unit beats
  sse2; the avx2 Q8_0 dot's extra `pabsb` per weight against HEAD.
