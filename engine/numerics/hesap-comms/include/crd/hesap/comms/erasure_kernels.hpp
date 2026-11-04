// The table of the erasure-code kernels (HESAP-NET; SIMD-DISPATCH, ADR-0134 §5): one per CPU level the kernel unit is
// built for (engine/numerics/hesap-comms/kernels/erasure_kernels.cpp), resolved by erasure_kernels() in
// crd/hesap/comms/erasure_dispatch.hpp. The header holds only data declarations, so a kernel unit compiles nothing from
// it. Every level gives the same bytes: GF(2^8) arithmetic is exact.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::comms
{

// One GF(2^8) coefficient c, expanded for byte-shuffle multiplication (crd/hesap/comms/gf256.hpp, gf256_expand):
// lo[x] = c·x and hi[x] = c·(x << 4) for x < 16, so c·b = lo[b & 15] ^ hi[b >> 4] for every byte b.
struct alignas(16) GfCoefficient
{
    crd::u8 lo[16];
    crd::u8 hi[16];
};

struct ErasureKernels
{
    // For every output g < output_count and byte i < bytes, with row g = coefficients + g * count:
    //   outputs[g][i] = (accumulate ? outputs[g][i] : 0) ^ XOR over j < count of row[j]·sources[j][i],
    // the GF(2^8) dot product of the row with the source regions (any alignment). An output may not overlap a source
    // or another output.
    void (*dot)(const GfCoefficient* coefficients, const crd::u8* const* sources, crd::usize count,
                crd::u8* const* outputs, crd::usize output_count, crd::usize bytes, bool accumulate) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const ErasureKernels kErasureKernelsPortable;
extern const ErasureKernels kErasureKernelsSse2;
extern const ErasureKernels kErasureKernelsAvx2;
extern const ErasureKernels kErasureKernelsAvx512;
extern const ErasureKernels kErasureKernelsNeon;

} // namespace crd::hesap::comms
