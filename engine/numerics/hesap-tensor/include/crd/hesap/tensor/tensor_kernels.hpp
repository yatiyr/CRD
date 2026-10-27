// The table of the hesap-tensor kernels (SIMD-DISPATCH, ADR-0134 §5): one per CPU level the kernel unit is built for
// (engine/numerics/hesap-tensor/kernels/tensor_kernels.cpp), resolved by tensor_kernels() in
// crd/hesap/tensor/tensor_dispatch.hpp. The header holds only data declarations, so a kernel unit compiles nothing from
// it. Every routine is bit-exact across levels (ADR-0063, ADR-0134 §7): the permute is copies and IEEE multiplies, and
// the binned sum is partition-independent by construction.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::tensor
{

namespace detail
{
struct BinnedRungs; // crd/hesap/tensor/binned_core.hpp
} // namespace detail

// One (a, b) region of an f32 permute: dst(ia, ib) = src(ia, ib) (scale: alpha * src(ia, ib)) over [0, na) x [0, nb),
// with dst(ia, ib) at dst + ia*ra + ib and src(ia, ib) at src + ia*sa + ib*sb, in tile_a x tile_b tiles, a-tiles outer.
// stream asks for non-temporal stores (dst 32-byte aligned, ra and tile_b multiples of 8), published before the call
// returns; a level without them stores normally.
struct PermuteRegionF32
{
    crd::f32* dst;
    crd::i64 ra;
    const crd::f32* src;
    crd::i64 sa;
    crd::i64 sb;
    crd::u64 na;
    crd::u64 nb;
    crd::u64 tile_a;
    crd::u64 tile_b;
    crd::f32 alpha;
    bool scale;
    bool stream;
};

struct TensorKernels
{
    // The f32 region of a permute (PermuteRegionF32).
    void (*permute_region_f32)(const PermuteRegionF32& region) noexcept;
    // The Tier R sum: deposit p[0..n) into acc (the binnedBLAS dbdsum protocol), bit-identical to the scalar
    // deposits in any partition.
    void (*binned_sum_f64)(detail::BinnedRungs& acc, const crd::f64* p, crd::u64 n) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const TensorKernels kTensorKernelsPortable;
extern const TensorKernels kTensorKernelsAvx2;

} // namespace crd::hesap::tensor
