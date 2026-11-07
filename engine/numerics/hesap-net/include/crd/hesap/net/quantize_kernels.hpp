// The quantisation formats and the table of their kernels (HESAP-NET; SIMD-DISPATCH, ADR-0134 §5): one table per CPU
// level the kernel unit is built for (engine/numerics/hesap-net/kernels/quantize_kernels.cpp), resolved by
// quantize_kernels() in crd/hesap/net/quantize_dispatch.hpp. The header holds only data declarations, so a kernel unit
// compiles nothing from it. Every level gives the same bits: each lane rounds exactly as the scalar expression does.
// crd/hesap/net/quantize.hpp makes the formats and states what the codes mean.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::net
{

struct BoundedFloat
{
    crd::f32 min;
    crd::f32 delta;    // The bound's width, as max - min rounded once.
    crd::u32 max_code; // Codes run 0 to max_code, at most kBoundedMaxCode.
    crd::u32 bits;     // The bits max_code takes.
};

struct SmallestThree
{
    crd::u32 bits;               // Per stored component.
    crd::f32 scale;              // 2^bits - 1.
    crd::f32 inverse_scale;      // 1 / scale.
    crd::f32 smallest_remainder; // The least 1 - a^2 - b^2 - c^2 an encoder's word can decode to.
};

struct QuantizeKernels
{
    // codes[i] = the code of values[i]; NaN is the lowest code, anything else out of bounds its nearest bound. May not
    // overlap.
    void (*bounded_encode)(const BoundedFloat& format, const crd::f32* values, crd::usize count,
                           crd::u32* codes) noexcept;

    // values[i] = the value of codes[i]. The number of codes above max_code, whose value is format.min. May not
    // overlap.
    crd::usize (*bounded_decode)(const BoundedFloat& format, const crd::u32* codes, crd::usize count,
                                 crd::f32* values) noexcept;

    // words[i] = the word of the quaternion xyzw[4 * i .. 4 * i + 3]. May not overlap.
    void (*smallest_three_encode)(const SmallestThree& format, const crd::f32* xyzw, crd::usize count,
                                  crd::u64* words) noexcept;

    // xyzw[4 * i .. 4 * i + 3] = the unit quaternion of words[i]. The number of words no encoder writes. May not
    // overlap.
    crd::usize (*smallest_three_decode)(const SmallestThree& format, const crd::u64* words, crd::usize count,
                                        crd::f32* xyzw) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const QuantizeKernels kQuantizeKernelsPortable;
extern const QuantizeKernels kQuantizeKernelsSse2;
extern const QuantizeKernels kQuantizeKernelsAvx2;
extern const QuantizeKernels kQuantizeKernelsAvx512;
extern const QuantizeKernels kQuantizeKernelsNeon;

} // namespace crd::hesap::net
