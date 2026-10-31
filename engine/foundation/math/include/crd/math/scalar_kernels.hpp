// The scalar transcendental table (SIMD-DISPATCH, ADR-0134 §5): one entry per f64 routine of the exp/log, trig,
// power and hyperbolic families whose body runs fma, one table per CPU level the scalar kernel unit is built for
// (engine/foundation/math/kernels/scalar_kernels.cpp), resolved once by scalar_math(). General code of an x64 build
// without FMA (CRD_MATH_SCALAR_DISPATCH) calls these routines through the table instead of compiling their bodies, so
// a CPU with FMA runs each one with the instruction inline rather than a C library call per fma; the f32 overloads
// round the f64 routine as the bodies do. Every level computes the same bits: one rounding per fma either way.

#pragma once

#include <crd/core/kernel_level.hpp>
#include <crd/core/types.hpp>

namespace crd::math
{

struct ScalarMathKernels
{
    double (*exp)(double) noexcept;
    double (*exp2)(double) noexcept;
    double (*exp10)(double) noexcept;
    double (*expm1)(double) noexcept;
    double (*log)(double) noexcept;
    double (*log2)(double) noexcept;
    double (*log10)(double) noexcept;
    double (*log1p)(double) noexcept;
    double (*sin)(double) noexcept;
    double (*cos)(double) noexcept;
    void (*sincos)(double, double&, double&) noexcept;
    double (*tan)(double) noexcept;
    double (*atan)(double) noexcept;
    double (*atan2)(double, double) noexcept;
    double (*asin)(double) noexcept;
    double (*acos)(double) noexcept;
    double (*pow)(double, double) noexcept;
    double (*sinh)(double) noexcept;
    double (*cosh)(double) noexcept;
    double (*tanh)(double) noexcept;
    double (*asinh)(double) noexcept;
    double (*acosh)(double) noexcept;
    double (*atanh)(double) noexcept;
};

// The levels with a body: portable (the baseline ISA, whose fma calls the C library or the integer body) and avx2
// (the FMA instruction; avx512 runs it too). The dispatcher refuses a build that compiles any other.
extern const ScalarMathKernels kScalarMathKernelsPortable;
extern const ScalarMathKernels kScalarMathKernelsAvx2;

// The table this process runs (crd/math/scalar_dispatch.hpp lists every built one).
[[nodiscard]] const ScalarMathKernels& scalar_math() noexcept;

} // namespace crd::math

#if CRD_MATH_SCALAR_DISPATCH
namespace crd::math::inline CRD_MATH_LEVEL_NS
{
[[nodiscard]] inline double exp(double x) noexcept
{
    return scalar_math().exp(x);
}
[[nodiscard]] inline double exp2(double x) noexcept
{
    return scalar_math().exp2(x);
}
[[nodiscard]] inline double exp10(double x) noexcept
{
    return scalar_math().exp10(x);
}
[[nodiscard]] inline double expm1(double x) noexcept
{
    return scalar_math().expm1(x);
}
[[nodiscard]] inline double log(double x) noexcept
{
    return scalar_math().log(x);
}
[[nodiscard]] inline double log2(double x) noexcept
{
    return scalar_math().log2(x);
}
[[nodiscard]] inline double log10(double x) noexcept
{
    return scalar_math().log10(x);
}
[[nodiscard]] inline double log1p(double x) noexcept
{
    return scalar_math().log1p(x);
}
[[nodiscard]] inline double sin(double x) noexcept
{
    return scalar_math().sin(x);
}
[[nodiscard]] inline double cos(double x) noexcept
{
    return scalar_math().cos(x);
}
inline void sincos(double x, double& s_out, double& c_out) noexcept
{
    scalar_math().sincos(x, s_out, c_out);
}
[[nodiscard]] inline double tan(double x) noexcept
{
    return scalar_math().tan(x);
}
[[nodiscard]] inline double atan(double x) noexcept
{
    return scalar_math().atan(x);
}
[[nodiscard]] inline double atan2(double y, double x) noexcept
{
    return scalar_math().atan2(y, x);
}
[[nodiscard]] inline double asin(double x) noexcept
{
    return scalar_math().asin(x);
}
[[nodiscard]] inline double acos(double x) noexcept
{
    return scalar_math().acos(x);
}
[[nodiscard]] inline double pow(double x, double y) noexcept
{
    return scalar_math().pow(x, y);
}
[[nodiscard]] inline double sinh(double x) noexcept
{
    return scalar_math().sinh(x);
}
[[nodiscard]] inline double cosh(double x) noexcept
{
    return scalar_math().cosh(x);
}
[[nodiscard]] inline double tanh(double x) noexcept
{
    return scalar_math().tanh(x);
}
[[nodiscard]] inline double asinh(double x) noexcept
{
    return scalar_math().asinh(x);
}
[[nodiscard]] inline double acosh(double x) noexcept
{
    return scalar_math().acosh(x);
}
[[nodiscard]] inline double atanh(double x) noexcept
{
    return scalar_math().atanh(x);
}

// The f32 overloads, as the bodies define them: the f64 routine rounded into f32.
[[nodiscard]] inline float exp(float x) noexcept
{
    return static_cast<float>(exp(static_cast<double>(x)));
}
[[nodiscard]] inline float exp2(float x) noexcept
{
    return static_cast<float>(exp2(static_cast<double>(x)));
}
[[nodiscard]] inline float exp10(float x) noexcept
{
    return static_cast<float>(exp10(static_cast<double>(x)));
}
[[nodiscard]] inline float expm1(float x) noexcept
{
    return static_cast<float>(expm1(static_cast<double>(x)));
}
[[nodiscard]] inline float log(float x) noexcept
{
    return static_cast<float>(log(static_cast<double>(x)));
}
[[nodiscard]] inline float log2(float x) noexcept
{
    return static_cast<float>(log2(static_cast<double>(x)));
}
[[nodiscard]] inline float log10(float x) noexcept
{
    return static_cast<float>(log10(static_cast<double>(x)));
}
[[nodiscard]] inline float log1p(float x) noexcept
{
    return static_cast<float>(log1p(static_cast<double>(x)));
}
[[nodiscard]] inline float sin(float x) noexcept
{
    return static_cast<float>(sin(static_cast<double>(x)));
}
[[nodiscard]] inline float cos(float x) noexcept
{
    return static_cast<float>(cos(static_cast<double>(x)));
}
inline void sincos(float x, float& s_out, float& c_out) noexcept
{
    double sd = 0.0;
    double cd = 0.0;
    sincos(static_cast<double>(x), sd, cd);
    s_out = static_cast<float>(sd);
    c_out = static_cast<float>(cd);
}
[[nodiscard]] inline float tan(float x) noexcept
{
    return static_cast<float>(tan(static_cast<double>(x)));
}
[[nodiscard]] inline float atan(float x) noexcept
{
    return static_cast<float>(atan(static_cast<double>(x)));
}
[[nodiscard]] inline float atan2(float y, float x) noexcept
{
    return static_cast<float>(atan2(static_cast<double>(y), static_cast<double>(x)));
}
[[nodiscard]] inline float asin(float x) noexcept
{
    return static_cast<float>(asin(static_cast<double>(x)));
}
[[nodiscard]] inline float acos(float x) noexcept
{
    return static_cast<float>(acos(static_cast<double>(x)));
}
[[nodiscard]] inline float pow(float x, float y) noexcept
{
    return static_cast<float>(pow(static_cast<double>(x), static_cast<double>(y)));
}
[[nodiscard]] inline float sinh(float x) noexcept
{
    return static_cast<float>(sinh(static_cast<double>(x)));
}
[[nodiscard]] inline float cosh(float x) noexcept
{
    return static_cast<float>(cosh(static_cast<double>(x)));
}
[[nodiscard]] inline float tanh(float x) noexcept
{
    return static_cast<float>(tanh(static_cast<double>(x)));
}
[[nodiscard]] inline float asinh(float x) noexcept
{
    return static_cast<float>(asinh(static_cast<double>(x)));
}
[[nodiscard]] inline float acosh(float x) noexcept
{
    return static_cast<float>(acosh(static_cast<double>(x)));
}
[[nodiscard]] inline float atanh(float x) noexcept
{
    return static_cast<float>(atanh(static_cast<double>(x)));
}
} // namespace crd::math::inline CRD_MATH_LEVEL_NS
#endif
