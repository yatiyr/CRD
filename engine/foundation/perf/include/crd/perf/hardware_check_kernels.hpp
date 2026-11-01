#pragma once

// The hardware check's compute arm (crd/perf/hardware_check.hpp), one table per CPU level (SIMD-DISPATCH, ADR-0134
// §5): the same recurrence on 16 lanes, on scalars (portable) and on sse2, avx2, avx512 or neon vectors. No Cerid code:
// plain arithmetic and intrinsics, so a wrong answer is the machine's. Each step of each lane, in this order:
//
//   x <- lo32(x) * 0x9E3779B1 + ((x >> 29) ^ (x << 7))      (u64, modulo 2^64)
//   y <- 3.99 * (y * (1.0 - y))                               (double: the logistic map, chaotic on (0, 1))
//   z <- (y + 3.0) / (z + 1.0)                                (double, the new y; stays in (0.6, 4))
//   c <- rotl(c, 1) ^ (bits(y) + bits(z))                     (u64: every step's y and z folded in)
//
// Every double operation is one IEEE operation rounded to nearest, with no multiply followed by an add to fuse, so
// each level gives the bits the recurrence evaluated at compile time gives.

#include <crd/core/types.hpp>

namespace crd::perf
{

inline constexpr crd::u32 kHardwareComputeLanes = 16U;

struct alignas(64) HardwareComputeState
{
    crd::u64 x[kHardwareComputeLanes]; // NOLINT(modernize-avoid-c-arrays): vector lanes
    double y[kHardwareComputeLanes];   // NOLINT(modernize-avoid-c-arrays)
    double z[kHardwareComputeLanes];   // NOLINT(modernize-avoid-c-arrays)
    crd::u64 c[kHardwareComputeLanes]; // NOLINT(modernize-avoid-c-arrays)
};

struct HardwareCheckKernels
{
    // Runs `steps` steps of the recurrence on every lane of `state`, in place.
    void (*compute)(HardwareComputeState& state, crd::u32 steps) noexcept;
};

// The levels with a body; hardware_check.cpp refuses a build that compiles any other.
extern const HardwareCheckKernels kHardwareCheckKernelsPortable;
extern const HardwareCheckKernels kHardwareCheckKernelsSse2;
extern const HardwareCheckKernels kHardwareCheckKernelsAvx2;
extern const HardwareCheckKernels kHardwareCheckKernelsAvx512;
extern const HardwareCheckKernels kHardwareCheckKernelsNeon;

} // namespace crd::perf
