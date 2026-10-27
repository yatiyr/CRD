# SIMD-DISPATCH: the hesap-tensor kernel family, part 2 (nn)

<!-- doc-role: historical -->
> Dated evidence. Live owner: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Contract:
> [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md). Previous:
> [part 1](2026-10-27-simd-dispatch-hesap-tensor-kernels.md). Rules: [AGENTS](../../AGENTS.md).

## CI read

- Completed runs 40415857302 to 40438603872 were cancelled with no failed job. Run 40409214033 (head `cf4b8a13`)
  completed with three failed jobs (linux-gcc-release, -relwithdebinfo, -shipping), each only
  `simd convert_truncate gives INT32_MIN`, the GCC fold `e9828cc2` removed (an ancestor of HEAD). Push run
  40434280766 (`d6528387`) is in progress (repository checks and win-vs passed so far), 40438797604 is pending; nothing
  was dispatched while they are open.
- Needs CI audit: DIAG.11c and DIAG.12 wait for a complete-tier run after C2; the newest, 40307186916 at `ae7bdc23`,
  predates it, so both stay Needs CI. This batch changes `cmake/`, so its push run resolves to the complete tier.

## Mechanism

`TensorKernels` (`tensor_kernels.hpp`) gains six entries, each a whole routine called once per row range:
`quantize_q8_0`, `quantize_u8_row`, `nn_softmax_rows`, `nn_linear_q8_rows`, `nn_linear_i8_rows` (the row-major
`nn_linear_i8` surface) and `nn_linear_i8_packed_rows` (the NnSequential runner), over the plain contexts
`NnSoftmaxF32`, `NnLinearQ8` and `NnLinearI8` that replace nn.hpp's `SoftmaxCtx`, `LinQ8Ctx` and `LinI8Ctx`.
`nn_parallel` calls the entries directly. The bodies (the pinned Q8_0 lane dot, the int8 dots on the xl/xh split, the
packed MLAS-style rows with the masked tail, both quantizers, softmax with `crd_exp4`) moved verbatim into
`kernels/tensor_kernels.cpp`, `std::fma` replaced by `crd::math::libm::fma`. nn.hpp keeps no intrinsic, no
`CRD_SIMD_HAS_*` test and no `transcendental.hpp` include. The new `quant_core.hpp` holds `BlockQ8Zero`/`BlockQ4Zero`
and the scalar quantizers in `CRD_MATH_LEVEL_NS`: they are both the reference (`quantize_q8_0` and the two per-tensor
quantizers, moved to dtypes.hpp as their HOME notes asked) and the portable bodies. `libm.hpp` gains `rint(f32)` for
the zero point. No include-budget entry holds nn.hpp off `<immintrin.h>`: it still reaches it through batched.hpp (the
hesap-dense family, open in the row).

## Changes beyond the move

| Finding | Now |
|---|---|
| A NaN element: the scalar quantizers cast NaN to int (undefined behaviour), the AVX2 Q8_0 body stored -128 (outside the symmetric range) and the u8 body 0 | a NaN quantizes as 0 (Q8_0) or the zero point (u8) on every level; the AVX2 Q8_0 body clears the -128 bytes after the pack (only a NaN reaches it) |
| The AVX2 amax and u8 range let a NaN lane win (`maxps`/`minps` return the second operand) | operand order keeps the scalar's `a > amax ? a : amax`; the amax tree keeps depth 2 |
| An infinite range (`-inf` and `+inf` in one row) made the zero point `inf/inf`, cast to int | the zero point is 0 for an empty or undefined range |
| A subnormal scale (Q8_0 amax below about 3.7e-37, a u8 range below about 3e-36) overflows the inverse, so every scaled value is ±inf, cast to int; the AVX2 u8 body put those lanes at 0, not the zero point | a non-finite scaled value quantizes as 0 or the zero point on every level (the u8 body masks to finite lanes) |
| The i8 quantizer note named a `quantize_row_i8` that never existed | rewritten in dtypes.hpp |

The AVX-VNNI bodies stay under `__AVXVNNI__`: no preset defines it, and the 2026-07-05 nn board reached them only
through an ad hoc `-mavxvnni` build. The kernel unit now has its own level flags, so an ad hoc build no longer reaches
them either; ADR-0134 §1 makes AVX-VNNI a feature the avx2 kernels select on, the next item of the row.

## Verification

`test_tensor_kernels.cpp` (`[tensor][kernels]`), every level the host runs: both quantizers against the scalar
reference on 23 blocks and 27 rows (zero and signed-zero blocks, huge and tiny scales, exact .5 ties, NaN, all-NaN,
one and both infinities, subnormal scales, post-ReLU, all-negative and constant rows), with the defined values for
NaN and non-finite scaled inputs checked; the Q8_0
linear against the pinned lane order for K 32/96/256 and N 1 to 19 (every tile width, ±127 saturation, ReLU) on a
row range with sentinel rows and eight sentinel words past the output; both int8 linears against the integer formula
for K 32/64/160 and N 1 to 21 (u8 values to 255, zero points 0 and 255, s = 0, the packed tail and 4+2 row groups);
softmax against the scalar order for d 1 to 37 (logits below -708, -inf, NaN, inf - inf, the near-underflow band).
`crd-hesap-tensor-nn-kernels-at-<level>` runs the nn `[q8]`, `[i8]` and `[softmax]` gates per level
(`crd_add_kernel_level_tests` takes an ENVIRONMENT for the corpus).

| Tooth (win-debug) | Failed |
|---|---|
| Q8_0 AVX2 keeps the non-finite bytes | quantizer differential (143 checks) |
| amax: a NaN on the propagating operand | quantizer differential |
| u8 range: HEAD's `minps` operand order | quantizer differential |
| u8 AVX2 mask clears only NaN (lets ±inf through) | subnormal-range row (zero point 255, got 0) |
| scalar u8 casts an infinity | quantizer differential at `CRD_CPU_LEVEL=portable` (63 checks) |
| Q8_0 hadd tree pairs the wrong outputs | Q8_0 linear differential |
| portable lane combine rounds twice (mul + add) | Q8_0 linear at `CRD_CPU_LEVEL=portable` |
| packed tail mask one word wide | int8 differential (the slack sentinels) |
| row-major dot drops the high activation bit | int8 differential |
| softmax vector lanes skip the max-subtract | softmax differential |
| Q8_0 rows start below `lo` | Q8_0 linear (sentinel rows) |
| the runner takes the row-major entry for packed weights | nn `[i8]` gates |

linux-gcc-debug: the NaN-byte, finite-mask, high-bit and portable-combine teeth failed the same cases; each restore
was rebuilt and passed. Both lanes pass every hesap-tensor suite (166 CTests on win-debug, 91 selected on
linux-gcc-debug), the forced arms and `smoke_hesap_tensor` (the ADR-0096 isolation gate); bench_nn builds under
linux-gcc-debug's warning set; the ctest guards pass on win-debug; strict tidy, clang-format (the touched-line hunks;
nn.hpp and test_nn.cpp keep fewer pre-existing findings than HEAD) and Allman are clean.

## Board

linux-gcc-release archives, bench_nn compiled with the tree's own flags (`-O3 -mavx2 -mfma -msse4.2 -mf16c`, LTO; no
`-march=native`), the HEAD binary with `d30df2c0`'s hesap-tensor headers first, both linked to the same archives (SHA
prefixes `bc885acb` HEAD, `7680c73c` new); 11 interleaved rounds on `taskset -c 4`, best-of-50 each, medians in ns
per sample.

| Case | HEAD | avx2 | portable |
|---|---|---|---|
| MLP f32, batch 16 | 209.7 | 206.4 | 377.2 |
| MLP Q8_0, batch 16 | 343.6 | 344.2 | 4626.8 |
| MLP i8, batch 16 | 330.6 | 334.2 | 2997.1 |
| MLP Q8_0, batch 4096 | 363.5 | 365.5 | 5163.3 |
| MLP i8, batch 4096 | 336.1 | 331.0 | 3263.8 |
| CNN Q8_0, batch 8 | 1819.6 | 1823.2 | 3047.2 |
| CNN i8, batch 8 | 1842.9 | 1850.6 | 2699.2 |
| CNN f32, batch 4096 | 1864.8 | 1857.2 | 2041.3 |

The avx2 level is at HEAD parity on every row (within 1.6%, with signs that change between batch sizes). The same
binary measured against itself in this protocol stays within 0.7%; rebuilds of the new binary moved the f32 rows,
whose code did not change, by up to 4% (the layout floor). An earlier build with a four-deep amax chain and per-vector
NaN masks read MLP Q8_0 batch 16 2% behind HEAD; the depth-2 tree with the after-pack fix-up removed it. The portable
column is what an SSE2-only CPU runs today: Q8_0 13x and i8 9x behind avx2. Peers were not rerun; the 2026-07-05
board's peer rows were measured with AVX-VNNI on both sides.

## Left in the row

The AVX-VNNI feature variant of the avx2 tensor kernels (selected on `AvxVnni`, both bodies in the differential), the
dtypes SR draws and decomp sketches through a tensor-kernel Philox block fill, SSE2/SSE4.2 bodies for the nn routines
(portable Q8_0 13x behind), then the rest the row lists.
