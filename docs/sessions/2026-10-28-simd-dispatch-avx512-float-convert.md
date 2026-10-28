# 2026-10-28 — SIMD-DISPATCH: the float_convert zmm body

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `ea871dd1`. CI read for this batch:
[tensor zmm bodies](2026-10-28-simd-dispatch-avx512-tensor.md#ci-first).

## The body

- `I32Lanes` gains a lane-mask type `M`: the all-ones vector on SSE2, SSE4.2 and AVX2, an opmask at AVX-512. The one
  integer transcription of the scalar narrowing now uses mask operations (`inc` where a mask is set, `zero_where`,
  `mask_or`, `mask_and`, `mask_andnot`, `mask_not`) where it used a mask as the value -1, so the 128- and 256-bit
  levels emit the same instructions as before and AVX-512 needs no mask-to-vector moves.
- AVX-512 runs sixteen lanes and every step on the first min(n - i, 16) lanes, so a call ends with one masked step
  instead of a scalar tail: masked loads, `vpmovdw`/`vpmovdb` masked narrowing stores, and VCVTPS2PH/VCVTPH2PS for
  f16. A lane past n loads as +0 and is never patched.
- Stochastic rounding sent every f32 zero or subnormal lane to the scalar 64-bit patch (its target exponent is far
  below the subnormal floor); the scalar core returns the signed zero there, which the vector already gives, so the
  deep mask now leaves those lanes out on every level. The bits are unchanged; zero-heavy inputs stay on the vector.
- The bf16 stochastic narrowing is one register function shared by the levels.

## Tests

- `test_kernel_dispatch.cpp`: every routine at every length 0 to 33 from an odd offset, with 17 guard words after the
  output; f16 widening of short calls over the top normals, the infinity and the first NaNs, with guard words.
- win-debug: 64 assertions in 3 cases (portable, sse2, sse4.2, avx2).
- SDE 10.13.1 `-skx`, `CRD_CPU_LEVEL=avx512`, `[cpu-level-applied]`: 75 assertions, avx512 included.

## Board candidate (row close, on AVX-512 hardware)

- The sixteen-lane transcription and the masked last step against avx2's eight lanes and scalar tail.
