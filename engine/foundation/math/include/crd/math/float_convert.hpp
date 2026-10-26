#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/math/float_convert_kernels.hpp>
#include <crd/math/float_convert_scalar.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <span>

// ---------------------------------------------------------------------------
// crd-math float_convert — small-float format conversions (f16 / bf16 / FP8
// e4m3fn / e5m2), RNE and stochastic rounding. The scalar cores are in
// float_convert_scalar.hpp; the batch converts below run the kernel level the
// CPU selection resolves (float_convert_kernels.hpp, ADR-0134).
//
// MIGRATED here from crd-hesap-tensor (v14-a) per user direction 2026-07-02:
// these are engine-base primitives (pure f32/u16/u8 signatures, constexpr
// scalar cores) intended for reuse — the tensor module's storage
// dtypes, the asset cooker's half-float texture/vertex payloads, v17 GPU's
// CPU-side conversion oracles, eylem quantized replay, v18 model I/O.
// crd-math is the owning module for reusable deterministic numeric
// primitives (SANITY #8).
//
// Semantics (reference-pinned, gated in tests/numerics/hesap-tensor vs the ml_dtypes
// corpus — the JAX/safetensors frontier):
//   - All narrowings are IEEE RNE. bf16 uses the carry trick.
//   - e4m3fn (OCP): NO inf — the all-ones exponent field holds normals except
//     S.1111.111 (NaN); overflow AND f32-inf narrow to NaN.
//   - NaN narrowing is payload-propagating (truncate + force quiet) — the
//     exact F16C VCVTPS2PH behavior, so the SIMD batch paths are bit-identical
//     to the scalar cores on EVERY input, NaN payloads included (gated).
//   - f32 subnormal inputs underflow to ±0 for EBITS < 8 targets (their
//     ranges end far above the f32 subnormal band); bf16 handles them exactly.
//   - Stochastic rounding (the *_sr variants) takes the uniform draw as an
//     ARGUMENT — pure bit math, unbiased on the linear grid (add the draw
//     below the discarded bits, truncate), saturating at max finite (never
//     manufactures inf/NaN from a finite input). Key/seed policy lives with
//     the caller (crd-hesap-tensor keys Philox by canonical destination
//     index — ADR-0096 §4).
//
// The batch converts CRUSH the frontier peers (1M elems, 1 core, 2026-07-02):
// f32→f16 0.106 ns = 1.29× torch-F16C / 11.5× numpy · f16→f32 0.097 = 6.9×
// numpy · bf16 0.139 = 2.5× ml_dtypes · e4m3 0.608 = 2.2× ml_dtypes.
// ---------------------------------------------------------------------------

namespace crd::math
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const KernelLevel<FloatConvertKernels>> float_convert_levels() noexcept;
[[nodiscard]] const KernelLevel<FloatConvertKernels>& float_convert_kernels() noexcept;

// =======================================================================
// Batch converts — the crush kernels. Contract: BIT-IDENTICAL to the scalar
// converts on every input, on every kernel level (tests/foundation/math
// test_kernel_dispatch.cpp runs each level the host executes; the
// hesap-tensor corpus gates the selected one). f16 rides F16C
// (VCVTPS2PH/VCVTPH2PS, hardware IEEE RNE incl. subnormals) and fp8 the
// 8-wide AVX2 narrower on the AVX2 level; the portable level is the scalar
// loops.
// =======================================================================

inline void convert_f32_to_f16(std::span<const crd::f32> src, std::span<crd::u16> dst) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size(), "convert_f32_to_f16: size mismatch");
    float_convert_kernels().table->f32_to_f16(src.data(), dst.data(), src.size());
}

inline void convert_f16_to_f32(std::span<const crd::u16> src, std::span<crd::f32> dst) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size(), "convert_f16_to_f32: size mismatch");
    float_convert_kernels().table->f16_to_f32(src.data(), dst.data(), src.size());
}

inline void convert_f32_to_bf16(std::span<const crd::f32> src, std::span<crd::u16> dst) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size(), "convert_f32_to_bf16: size mismatch");
    float_convert_kernels().table->f32_to_bf16(src.data(), dst.data(), src.size());
}

inline void convert_f32_to_fp8_e4m3(std::span<const crd::f32> src, std::span<crd::u8> dst) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size(), "convert_f32_to_fp8_e4m3: size mismatch");
    float_convert_kernels().table->f32_to_fp8_e4m3(src.data(), dst.data(), src.size());
}

inline void convert_f32_to_fp8_e5m2(std::span<const crd::f32> src, std::span<crd::u8> dst) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size(), "convert_f32_to_fp8_e5m2: size mismatch");
    float_convert_kernels().table->f32_to_fp8_e5m2(src.data(), dst.data(), src.size());
}

// Stochastic rounding: rand[k] is element k's uniform draw; the caller owns the draw policy (crd-hesap-tensor's
// Philox keying). Bit-identical to the per-element scalar SR on every input.

inline void convert_f32_to_f16_sr(std::span<const crd::f32> src, std::span<crd::u16> dst,
                                  std::span<const crd::u32> rand) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size() && src.size() == rand.size(), "convert_f32_to_f16_sr: size mismatch");
    float_convert_kernels().table->f32_to_f16_sr(src.data(), dst.data(), rand.data(), src.size());
}

inline void convert_f32_to_bf16_sr(std::span<const crd::f32> src, std::span<crd::u16> dst,
                                   std::span<const crd::u32> rand) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size() && src.size() == rand.size(), "convert_f32_to_bf16_sr: size mismatch");
    float_convert_kernels().table->f32_to_bf16_sr(src.data(), dst.data(), rand.data(), src.size());
}

inline void convert_f32_to_fp8_e4m3_sr(std::span<const crd::f32> src, std::span<crd::u8> dst,
                                       std::span<const crd::u32> rand) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size() && src.size() == rand.size(), "convert_f32_to_fp8_e4m3_sr: size mismatch");
    float_convert_kernels().table->f32_to_fp8_e4m3_sr(src.data(), dst.data(), rand.data(), src.size());
}

inline void convert_f32_to_fp8_e5m2_sr(std::span<const crd::f32> src, std::span<crd::u8> dst,
                                       std::span<const crd::u32> rand) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size() && src.size() == rand.size(), "convert_f32_to_fp8_e5m2_sr: size mismatch");
    float_convert_kernels().table->f32_to_fp8_e5m2_sr(src.data(), dst.data(), rand.data(), src.size());
}

} // namespace crd::math
