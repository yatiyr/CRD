// The scalar transcendental routines at one CPU level (SIMD-DISPATCH, ADR-0134 §5; the table in
// crd/math/scalar_kernels.hpp). Each entry is the family's own inline body compiled at this unit's level: at avx2
// every libm::fma is the instruction, at portable the C library's fused call or the integer body, one rounding either
// way, so every level returns the same bits.

#include <crd/math/hyperbolic.hpp>
#include <crd/math/power.hpp>
#include <crd/math/scalar_kernels.hpp>
#include <crd/math/transcendental.hpp>

namespace crd::math
{

namespace lv = CRD_MATH_LEVEL_NS;

const ScalarMathKernels CRD_KERNEL_TABLE(kScalarMathKernels) = {
    +[](double x) noexcept { return lv::exp(x); },
    +[](double x) noexcept { return lv::exp2(x); },
    +[](double x) noexcept { return lv::exp10(x); },
    +[](double x) noexcept { return lv::expm1(x); },
    +[](double x) noexcept { return lv::log(x); },
    +[](double x) noexcept { return lv::log2(x); },
    +[](double x) noexcept { return lv::log10(x); },
    +[](double x) noexcept { return lv::log1p(x); },
    +[](double x) noexcept { return lv::sin(x); },
    +[](double x) noexcept { return lv::cos(x); },
    +[](double x, double& s, double& c) noexcept { lv::sincos(x, s, c); },
    +[](double x) noexcept { return lv::tan(x); },
    +[](double x) noexcept { return lv::atan(x); },
    +[](double y, double x) noexcept { return lv::atan2(y, x); },
    +[](double x) noexcept { return lv::asin(x); },
    +[](double x) noexcept { return lv::acos(x); },
    +[](double x, double y) noexcept { return lv::pow(x, y); },
    +[](double x) noexcept { return lv::sinh(x); },
    +[](double x) noexcept { return lv::cosh(x); },
    +[](double x) noexcept { return lv::tanh(x); },
    +[](double x) noexcept { return lv::asinh(x); },
    +[](double x) noexcept { return lv::acosh(x); },
    +[](double x) noexcept { return lv::atanh(x); },
};

} // namespace crd::math
