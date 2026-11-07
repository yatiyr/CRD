// The standard Pareto and the normal-Pareto mix as the link-model tests restate them, by the C library and independently
// of the engine: the value of each at a standard normal rank z, and the mix's deviation by a fine trapezoid.

#pragma once

#include <crd/core/types.hpp>

#include <cmath>

namespace link_laws
{

[[nodiscard]] inline crd::f64 pareto_standard(crd::f64 z, crd::f64 tail)
{
    const crd::f64 mean = tail / (tail - 1.0);
    const crd::f64 deviation = std::sqrt(tail / (tail - 2.0)) / (tail - 1.0);
    return (std::pow(0.5 * std::erfc(z * std::sqrt(0.5)), -1.0 / tail) - mean) / deviation;
}

[[nodiscard]] inline crd::f64 mix_deviation(crd::f64 tail)
{
    constexpr int steps = 200000;
    constexpr crd::f64 range = 14.0;
    constexpr crd::f64 pi = 3.14159265358979323846;
    crd::f64 covariance = 0.0;
    for (int i = 0; i <= steps; ++i)
    {
        const crd::f64 z = -range + (2.0 * range * i / steps);
        const crd::f64 density = std::exp(-0.5 * z * z) / std::sqrt(2.0 * pi);
        covariance += z * pareto_standard(z, tail) * density * (2.0 * range / steps);
    }
    return std::sqrt(0.0625 + 0.5625 + (2.0 * 0.25 * 0.75 * covariance));
}

} // namespace link_laws
