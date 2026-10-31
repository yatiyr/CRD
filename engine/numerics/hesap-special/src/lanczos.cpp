// crd-hesap-special — the full scalar Lanczos lgamma and tgamma (crd/hesap/special/detail/lanczos_core.hpp): the
// reflection, pole, NaN and infinity cases around the half-plane core. General code; the batch kernels call these
// out of line for the lanes outside the core's half-plane, so every level gives these bits.

#include <crd/hesap/special/detail/lanczos_core.hpp>
#include <crd/math/cmath.hpp>

#include <limits>

namespace crd::hesap::special::detail
{
namespace
{

constexpr double kLnPi = 1.144729885849400174143427351353058712;
constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kInf = std::numeric_limits<double>::infinity();

} // namespace

double lanczos_lgamma(double x) noexcept
{
    if (lanczos_in_half(x))
    {
        return lanczos_lgamma_half(x);
    }
    if (x == kInf || crd::math::libm::is_nan(x))
    {
        return x;
    }
    if (x == crd::math::floor(x))
    {
        return kInf; // the poles 0, -1, -2, ... and -inf
    }
    const double s = crd::math::abs(crd::math::sin(kPi * x));
    return kLnPi - crd::math::log(s) - lanczos_lgamma_half(1.0 - x);
}

double lanczos_tgamma(double x) noexcept
{
    if (lanczos_in_half(x))
    {
        return crd::math::exp(lanczos_lgamma_half(x));
    }
    if (x == kInf || crd::math::libm::is_nan(x))
    {
        return x;
    }
    if (x == 0.0)
    {
        return crd::math::copysign(kInf, x); // the pole at zero keeps the zero's sign
    }
    if (x > 0.0)
    {
        return crd::math::exp(lanczos_lgamma(x)); // 0 < x < 0.5: Gamma > 0
    }
    if (x == crd::math::floor(x))
    {
        return std::numeric_limits<double>::quiet_NaN(); // the poles -1, -2, ... and -inf
    }
    const double s = crd::math::sin(kPi * x);
    return kPi / (s * crd::math::exp(lanczos_lgamma_half(1.0 - x)));
}

} // namespace crd::hesap::special::detail
