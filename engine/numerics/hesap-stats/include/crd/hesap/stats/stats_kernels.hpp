// The table of the hesap-stats kernels (SIMD-DISPATCH, ADR-0134 §5): one per CPU level the kernel unit is built for
// (engine/numerics/hesap-stats/kernels/stats_kernels.cpp), resolved by stats_kernels() in
// crd/hesap/stats/stats_dispatch.hpp. The header holds only data declarations, so a kernel unit compiles nothing from
// it. Every routine is bit-exact across levels (ADR-0063, ADR-0134 §7).

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::stats
{

struct StatsKernels
{
    // Philox4x32-10 blocks first_block .. first_block + blocks - 1 (mod 2^64) of (seed, stream): the blocks' u32 words
    // in block order, 16 bytes per block at out (any alignment), which a little-endian u64 array reads as
    // PhiloxRng::next_u64's stream.
    void (*philox4x32_blocks)(crd::u64 seed, crd::u64 stream, crd::u64 first_block, void* out,
                              crd::usize blocks) noexcept;
    // Threefry4x64-20 blocks counter .. counter + blocks - 1 (256-bit, threefry_counter_add) under key, four u64 per
    // block in block order.
    void (*threefry4x64_blocks)(const crd::u64 (&key)[4], const crd::u64 (&counter)[4], crd::u64* out,
                                crd::usize blocks) noexcept;
    // The data part of the Student-t score in nu: the sum over xs[0..n) of
    // -log(1 + x^2/nu)/2 + (nu + 1) x^2 / (2 nu (nu + x^2)), four lanes in element order mod 4, then the remainder.
    crd::f64 (*student_t_nu_score_sum)(const crd::f64* xs, crd::usize n, crd::f64 nu) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const StatsKernels kStatsKernelsPortable;
extern const StatsKernels kStatsKernelsSse2;
extern const StatsKernels kStatsKernelsAvx2;
extern const StatsKernels kStatsKernelsAvx512;

} // namespace crd::hesap::stats
