// The table of the hesap-special kernels (SIMD-DISPATCH, ADR-0134 §5): one per CPU level the kernel unit is built for
// (engine/numerics/hesap-special/kernels/special_kernels.cpp), resolved by special_kernels() in
// crd/hesap/special/special_dispatch.hpp. The header holds only data declarations, so a kernel unit compiles nothing
// from it. Every routine is bit-exact across levels (ADR-0063, ADR-0134 §7).

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::special
{

struct SpecialKernels
{
    // out[i] = detail::lanczos_lgamma(in[i]) for i in [0, n) (crd/hesap/special/detail/lanczos_core.hpp).
    void (*lgamma_lanczos)(crd::f64* out, const crd::f64* in, crd::usize n) noexcept;
    // out[i] = detail::lanczos_tgamma(in[i]) for i in [0, n).
    void (*tgamma_lanczos)(crd::f64* out, const crd::f64* in, crd::usize n) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const SpecialKernels kSpecialKernelsPortable;
extern const SpecialKernels kSpecialKernelsSse2;
extern const SpecialKernels kSpecialKernelsAvx2;

} // namespace crd::hesap::special
