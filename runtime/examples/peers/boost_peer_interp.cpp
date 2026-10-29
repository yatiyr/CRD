// The Boost.Math peer's interpolants (boost_peer.hpp).

#include "boost_peer.hpp"

#include <boost/math/interpolators/barycentric_rational.hpp>
#include <boost/math/interpolators/bilinear_uniform.hpp>
#include <boost/math/interpolators/cardinal_cubic_b_spline.hpp>
#include <boost/math/interpolators/makima.hpp>
#include <boost/math/interpolators/pchip.hpp>
#include <cstddef>
#include <type_traits>
#include <variant>
#include <vector>

namespace bmi = ::boost::math::interpolators;

namespace crd_peer::boost_math
{
using Samples = std::vector<double>;

struct Interpolator
{
    std::variant<bmi::pchip<Samples>, bmi::makima<Samples>, bmi::barycentric_rational<double>,
                 bmi::cardinal_cubic_b_spline<double>, bmi::bilinear_uniform<Samples>>
        f;
};

namespace
{
Samples copy(const double* values, Index n)
{
    return Samples(values, values + n);
}
} // namespace

Interpolator* build(Interpolant kind, const Curve& curve)
{
    const auto n = static_cast<std::size_t>(curve.n);
    switch (kind)
    {
        case Interpolant::Pchip:
            return new Interpolator{bmi::pchip<Samples>(copy(curve.x, curve.n), copy(curve.y, curve.n),
                                                        curve.left_derivative, curve.right_derivative)};
        case Interpolant::Makima:
            return new Interpolator{bmi::makima<Samples>(copy(curve.x, curve.n), copy(curve.y, curve.n),
                                                         curve.left_derivative, curve.right_derivative)};
        case Interpolant::FloaterHormann:
            return new Interpolator{
                bmi::barycentric_rational<double>(curve.x, curve.y, n, static_cast<std::size_t>(curve.order))};
        case Interpolant::CardinalCubic:
            return new Interpolator{bmi::cardinal_cubic_b_spline<double>(
                curve.y, n, curve.x[0], curve.x[1] - curve.x[0], curve.left_derivative, curve.right_derivative)};
    }
    return nullptr;
}

Interpolator* build(const Field& field)
{
    const auto rows = static_cast<std::size_t>(field.rows);
    const auto cols = static_cast<std::size_t>(field.cols);
    return new Interpolator{bmi::bilinear_uniform<Samples>(copy(field.values, field.rows * field.cols), rows, cols,
                                                           field.dx, field.dy, field.x0, field.y0)};
}

void destroy(Interpolator* interpolator) noexcept
{
    delete interpolator;
}

void eval(const Interpolator& interpolator, const double* x, const double* y, double* out, Index n)
{
    std::visit(
        [&](const auto& f)
        {
            if constexpr (std::is_same_v<std::decay_t<decltype(f)>, bmi::bilinear_uniform<Samples>>)
            {
                for (Index i = 0; i < n; ++i)
                {
                    out[i] = f(x[i], y[i]);
                }
            }
            else
            {
                for (Index i = 0; i < n; ++i)
                {
                    out[i] = f(x[i]);
                }
            }
        },
        interpolator.f);
}
} // namespace crd_peer::boost_math
