// The table of the batch small-float conversion kernels (SIMD-DISPATCH, ADR-0134 §5): one per CPU level the
// conversion kernel unit is built for (engine/foundation/math/kernels/float_convert_kernels.cpp), resolved by
// float_convert_kernels() in crd/math/float_convert.hpp. The header holds only data declarations, so a kernel unit
// compiles nothing from it.

#pragma once

#include <crd/core/types.hpp>

namespace crd::math
{

// Each routine converts n elements of src into dst; rand[k] is element k's uniform draw (stochastic rounding).
struct FloatConvertKernels
{
    void (*f32_to_f16)(const crd::f32* src, crd::u16* dst, crd::usize n) noexcept;
    void (*f16_to_f32)(const crd::u16* src, crd::f32* dst, crd::usize n) noexcept;
    void (*f32_to_bf16)(const crd::f32* src, crd::u16* dst, crd::usize n) noexcept;
    void (*f32_to_fp8_e4m3)(const crd::f32* src, crd::u8* dst, crd::usize n) noexcept;
    void (*f32_to_fp8_e5m2)(const crd::f32* src, crd::u8* dst, crd::usize n) noexcept;
    void (*f32_to_f16_sr)(const crd::f32* src, crd::u16* dst, const crd::u32* rand, crd::usize n) noexcept;
    void (*f32_to_bf16_sr)(const crd::f32* src, crd::u16* dst, const crd::u32* rand, crd::usize n) noexcept;
    void (*f32_to_fp8_e4m3_sr)(const crd::f32* src, crd::u8* dst, const crd::u32* rand, crd::usize n) noexcept;
    void (*f32_to_fp8_e5m2_sr)(const crd::f32* src, crd::u8* dst, const crd::u32* rand, crd::usize n) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const FloatConvertKernels kFloatConvertKernelsPortable;
extern const FloatConvertKernels kFloatConvertKernelsSse2;
extern const FloatConvertKernels kFloatConvertKernelsSse42;
extern const FloatConvertKernels kFloatConvertKernelsAvx2;

} // namespace crd::math
