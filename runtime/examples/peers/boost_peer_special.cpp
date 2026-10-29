// The Boost.Math peer's special functions (boost_peer.hpp).

#include "boost_peer.hpp"

#include <array>
#include <boost/math/policies/policy.hpp>
#include <boost/math/special_functions/airy.hpp>
#include <boost/math/special_functions/bessel.hpp>
#include <boost/math/special_functions/digamma.hpp>
#include <boost/math/special_functions/ellint_1.hpp>
#include <boost/math/special_functions/ellint_2.hpp>
#include <boost/math/special_functions/ellint_rf.hpp>
#include <boost/math/special_functions/erf.hpp>
#include <boost/math/special_functions/expint.hpp>
#include <boost/math/special_functions/gamma.hpp>
#include <boost/math/special_functions/lambert_w.hpp>
#include <boost/math/special_functions/zeta.hpp>
#include <boost/version.hpp>
#include <cstdio>
#include <limits>

namespace bm = ::boost::math;

namespace crd_peer::boost_math
{
namespace
{
template <typename F> void unary(const Arguments& arguments, double* out, Index n, F f)
{
    for (Index i = 0; i < n; ++i)
    {
        out[i] = f(arguments.first[i]);
    }
}

template <typename F> void binary(const Arguments& arguments, double* out, Index n, F f)
{
    for (Index i = 0; i < n; ++i)
    {
        out[i] = f(arguments.first[i], arguments.second[i]);
    }
}

template <typename F> void ternary(const Arguments& arguments, double* out, Index n, F f)
{
    for (Index i = 0; i < n; ++i)
    {
        out[i] = f(arguments.first[i], arguments.second[i], arguments.third[i]);
    }
}
} // namespace

const char* isa() noexcept
{
    using Evaluation = bm::policies::evaluation<double, bm::policies::policy<>>::type;
    static const std::array<char, 200> kText = []()
    {
        std::array<char, 200> text{};
        (void)std::snprintf(text.data(), text.size(),
                            "Boost %d.%d.%d, default policy: double evaluated with a %d-bit significand (%s)",
                            BOOST_VERSION / 100000, BOOST_VERSION / 100 % 1000, BOOST_VERSION % 100,
                            std::numeric_limits<Evaluation>::digits, CRD_BENCH_PEER_ISA);
        return text;
    }();
    return kText.data();
}

void eval(Special f, const Arguments& arguments, double* out, Index n)
{
    switch (f)
    {
        case Special::Erf:
            unary(arguments, out, n, [](double x) { return bm::erf(x); });
            break;
        case Special::Erfc:
            unary(arguments, out, n, [](double x) { return bm::erfc(x); });
            break;
        case Special::ErfInv:
            unary(arguments, out, n, [](double x) { return bm::erf_inv(x); });
            break;
        case Special::Lgamma:
            unary(arguments, out, n, [](double x) { return bm::lgamma(x); });
            break;
        case Special::Tgamma:
            unary(arguments, out, n, [](double x) { return bm::tgamma(x); });
            break;
        case Special::Digamma:
            unary(arguments, out, n, [](double x) { return bm::digamma(x); });
            break;
        case Special::GammaP:
            binary(arguments, out, n, [](double a, double x) { return bm::gamma_p(a, x); });
            break;
        case Special::CylBesselJ:
            binary(arguments, out, n, [](double nu, double x) { return bm::cyl_bessel_j(nu, x); });
            break;
        case Special::CylNeumann:
            binary(arguments, out, n, [](double nu, double x) { return bm::cyl_neumann(nu, x); });
            break;
        case Special::CylBesselI:
            binary(arguments, out, n, [](double nu, double x) { return bm::cyl_bessel_i(nu, x); });
            break;
        case Special::CylBesselK:
            binary(arguments, out, n, [](double nu, double x) { return bm::cyl_bessel_k(nu, x); });
            break;
        case Special::AiryAi:
            unary(arguments, out, n, [](double x) { return bm::airy_ai(x); });
            break;
        case Special::AiryBi:
            unary(arguments, out, n, [](double x) { return bm::airy_bi(x); });
            break;
        case Special::Ei:
            unary(arguments, out, n, [](double x) { return bm::expint(x); });
            break;
        case Special::E1:
            unary(arguments, out, n, [](double x) { return bm::expint(1U, x); });
            break;
        case Special::Zeta:
            unary(arguments, out, n, [](double s) { return bm::zeta(s); });
            break;
        case Special::LambertW0:
            unary(arguments, out, n, [](double x) { return bm::lambert_w0(x); });
            break;
        case Special::Ellint1:
            unary(arguments, out, n, [](double k) { return bm::ellint_1(k); });
            break;
        case Special::Ellint2:
            unary(arguments, out, n, [](double k) { return bm::ellint_2(k); });
            break;
        case Special::EllintRf:
            ternary(arguments, out, n, [](double x, double y, double z) { return bm::ellint_rf(x, y, z); });
            break;
    }
}
} // namespace crd_peer::boost_math
