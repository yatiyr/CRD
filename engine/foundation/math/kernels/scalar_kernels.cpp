// The scalar transcendental routines at one CPU level (SIMD-DISPATCH, ADR-0134 §5; the table in
// crd/math/scalar_kernels.hpp). Each entry is the family's own inline body compiled at this unit's level: at avx2
// every libm::fma is the instruction, at portable the C library's fused call or the integer body, one rounding either
// way, so every level returns the same bits. The entries take the level's own functions, never lambdas: GCC emits a
// namespace-scope lambda's thunk as a weak symbol under the table's name, outside the level namespace.

#include <crd/math/hyperbolic.hpp>
#include <crd/math/power.hpp>
#include <crd/math/scalar_kernels.hpp>
#include <crd/math/transcendental.hpp>

namespace crd::math
{

namespace lv = CRD_MATH_LEVEL_NS;

const ScalarMathKernels CRD_KERNEL_TABLE(kScalarMathKernels) = {
    &lv::exp, &lv::exp2, &lv::exp10,  &lv::expm1, &lv::log,   &lv::log2,  &lv::log10, &lv::log1p,
    &lv::sin, &lv::cos,  &lv::sincos, &lv::tan,   &lv::atan,  &lv::atan2, &lv::asin,  &lv::acos,
    &lv::pow, &lv::sinh, &lv::cosh,   &lv::tanh,  &lv::asinh, &lv::acosh, &lv::atanh,
};

} // namespace crd::math
