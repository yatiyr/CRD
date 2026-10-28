# 2026-10-28 — SIMD-DISPATCH: the tensor zmm bodies and the AVX512-VNNI variant

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `06ff67a5`.

## CI first

Complete run 40649898318 (`3e849d96`) is still in progress; its two failed jobs are the ones already fixed
([dense and direct](2026-10-28-simd-dispatch-avx512-dense-direct.md): the fiber-keep test race on win-clang-cl-shipping,
the download stall on linux-gcc-debug-scalar). Every later push run was cancelled or is queued. DIAG.11c and DIAG.12
stay Needs CI behind this row.

## The variant

- `kernel_level.hpp`: `CRD_KERNEL_FEATURE_AVX512VNNI` is a variant of the avx512 level only, with the namespace
  `avx512_avx512vnni`, the suffix `Avx512Avx512Vnni` and `CRD_KERNEL_HAS_AVX512VNNI`.
- `CrdSimd.cmake`: the `avx512vnni` feature extends avx512 (`-mavx512vnni`; cl compiles it at any `/arch`).
  hesap-tensor builds `VARIANTS avx2+avxvnni avx512+avx512vnni`, and its dispatcher lists the new table after the plain
  avx512 one with `Avx512Vnni`.
- `crd_add_kernel_level_tests` takes `SDE_CPUS`: a suite adds the SDE models its variants select on, beside skx for
  every suite (ADR-0134 §8). The tensor and tensor-nn suites add `spr`, which has AVX-VNNI and AVX512-VNNI, so both
  variant tables run there. The arm forces `CRD_CPU_LEVEL=avx512`, which keeps only the level's features in
  `kernel_features`: the process runs the plain table, and the differential cases reach each variant through the host's
  usable features.

## The bodies

- **Permute**: the squares are 16x16 f32 (AVX2's in-lane stages, then two `shuffle_f32x4` stages), 8x8 64-bit and
  4x4 128-bit (`shuffle_f64x2`). AVX-512 has no addsub: a complex scale adds, then subtracts under a mask on the re
  lanes, so each lane keeps its true operation. The streamed path's contract stays 32-byte rows: a zmm row on a line
  boundary is one 64-byte non-temporal store, any other one two 32-byte stores, so no general-code gate changes.
- **Tier R**: three binned streams of eight lanes. Each stream carries one add per rung from element to element, and
  three streams already issue more work than that chain, so the larger register file buys no fourth stream.
- **Quantizers**: Q8_0 runs a block in two zmm; `vpmovsdb` saturates each lane in element order, the AVX2 packs chain
  without its permute. The u8 row runs sixteen lanes; the clamp raises the lanes to 0 before `vpmovusdb`, which reads a
  negative lane as a large unsigned one.
- **Softmax**: eight lanes with `crd_exp8`, then AVX2's four-lane step.
- **Q8_0 dots**: two blocks a zmm in the integer stage (`vpsignb` has no zmm form: x is negated where w is negative,
  and |w| = 0 makes the other lanes' product 0). Each block's f32 combine stays on its own eight lanes in block order,
  two ymm fmas a step, so the chain is one block a step's.
- **int8 dots**: the row-major dot runs 64 values a step and folds into the ymm accumulators for a last 32; the packed
  dot runs two 8-output blocks a step with one activation broadcast, an odd last block on the 8-output step. The VNNI
  variant runs `vpdpbusd` on zmm and its EVEX ymm form.

Every body keeps the portable bits (permute copies and the product's IEEE operations, a partition-independent binned
sum, exact integer dots in one f32 order).

## Tests

- `test_tensor_kernels.cpp`: the archive holds the avx512 VNNI table; the region case also streams to a destination
  32 bytes past a line (f32 rows already alternate); the binned lengths meet the AVX-512 gate (191 to 215) and
  super-block, with a later rollback magnitude and a near-DBL_MAX value in its second super-block.
- `test_kernel_dispatch.cpp`: the avx512 variant is picked with its feature, never below its level or above the cap.
- win-debug `[kernels]`: 122,929 assertions in 13 cases (portable, sse2, sse4.2, avx2, avx2+avx-vnni).
- SDE 10.13.1, `CRD_CPU_LEVEL=avx512`, `[cpu-level-applied]`: skx 122,929 assertions (avx512 for avx2+avx-vnni,
  17 s); spr 131,569 (all seven tables, 22 s); the nn suite under spr and the permute suite under skx pass.

## Board candidates (row close, on AVX-512 hardware)

- The zmm permute squares against avx2's at every element size, and the split non-temporal store against a line-only
  zmm store.
- The two-block Q8_0 integer stage, the 64-value row dot and the 16-output packed step, with and without VNNI.
- Three binned streams of eight lanes against avx2's four.
