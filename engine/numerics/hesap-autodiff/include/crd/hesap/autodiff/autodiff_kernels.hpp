// The table of the hesap-autodiff kernels (SIMD-DISPATCH, ADR-0134 §5): one per CPU level the kernel unit is built for
// (engine/numerics/hesap-autodiff/kernels/autodiff_kernels.cpp), resolved by autodiff_kernels() in
// crd/hesap/autodiff/autodiff_dispatch.hpp. The header holds only data declarations, so a kernel unit compiles nothing
// from it. Every routine is bit-exact across levels (ADR-0063), NaN payloads aside: the forward jets' chains are per
// lane and single-rounded on every level (crd/hesap/autodiff/jet_simd.hpp).

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::autodiff
{

// The engine's canned scalar functions f: R^n → R (crd/hesap/autodiff/canned_functions.hpp), numbered as the hesap.ad
// commands' `func` argument.
enum class CannedFunction : crd::u8
{
    Rosenbrock = 0,
    Sphere = 1,
    Cubes = 2,
};

// The longest point a canned gradient takes.
inline constexpr int kCannedMaxN = 32;

struct AutodiffKernels
{
    // ∇f(x) of a canned function at x[0..n), 1 <= n <= kCannedMaxN, into g[0..n): the forward gradient driver over
    // JetPackD<8> tiles.
    void (*gradient_f64)(CannedFunction f, const crd::f64* x, int n, crd::f64* g) noexcept;
};

extern const AutodiffKernels kAutodiffKernelsPortable;
extern const AutodiffKernels kAutodiffKernelsSse2;
extern const AutodiffKernels kAutodiffKernelsAvx2;
extern const AutodiffKernels kAutodiffKernelsAvx512;

} // namespace crd::hesap::autodiff
