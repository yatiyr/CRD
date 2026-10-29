// The Boost.Math peer's distributions (boost_peer.hpp).

#include "boost_peer.hpp"

#include <boost/math/distributions/beta.hpp>
#include <boost/math/distributions/binomial.hpp>
#include <boost/math/distributions/cauchy.hpp>
#include <boost/math/distributions/chi_squared.hpp>
#include <boost/math/distributions/exponential.hpp>
#include <boost/math/distributions/extreme_value.hpp>
#include <boost/math/distributions/fisher_f.hpp>
#include <boost/math/distributions/gamma.hpp>
#include <boost/math/distributions/geometric.hpp>
#include <boost/math/distributions/lognormal.hpp>
#include <boost/math/distributions/negative_binomial.hpp>
#include <boost/math/distributions/normal.hpp>
#include <boost/math/distributions/pareto.hpp>
#include <boost/math/distributions/poisson.hpp>
#include <boost/math/distributions/students_t.hpp>
#include <boost/math/distributions/weibull.hpp>

namespace bm = ::boost::math;

namespace crd_peer::boost_math
{
namespace
{
template <typename D> void apply(const D& distribution, Density density, const double* x, double* out, Index n)
{
    if (density == Density::Pdf)
    {
        for (Index i = 0; i < n; ++i)
        {
            out[i] = bm::pdf(distribution, x[i]);
        }
    }
    else
    {
        for (Index i = 0; i < n; ++i)
        {
            out[i] = bm::cdf(distribution, x[i]);
        }
    }
}
} // namespace

void eval(Distribution distribution, Parameters parameters, Density density, const double* x, double* out, Index n)
{
    const double a = parameters.first;
    const double b = parameters.second;
    switch (distribution)
    {
        case Distribution::Normal:
            apply(bm::normal(a, b), density, x, out, n);
            break;
        case Distribution::LogNormal:
            apply(bm::lognormal(a, b), density, x, out, n);
            break;
        case Distribution::Gamma:
            apply(bm::gamma_distribution<double>(a, b), density, x, out, n);
            break;
        case Distribution::Beta:
            apply(bm::beta_distribution<double>(a, b), density, x, out, n);
            break;
        case Distribution::Exponential:
            apply(bm::exponential(a), density, x, out, n);
            break;
        case Distribution::ChiSquared:
            apply(bm::chi_squared(a), density, x, out, n);
            break;
        case Distribution::StudentsT:
            apply(bm::students_t(a), density, x, out, n);
            break;
        case Distribution::FisherF:
            apply(bm::fisher_f(a, b), density, x, out, n);
            break;
        case Distribution::Cauchy:
            apply(bm::cauchy(a, b), density, x, out, n);
            break;
        case Distribution::Weibull:
            apply(bm::weibull(a, b), density, x, out, n);
            break;
        case Distribution::ExtremeValue:
            apply(bm::extreme_value(a, b), density, x, out, n);
            break;
        case Distribution::Pareto:
            apply(bm::pareto(a, b), density, x, out, n);
            break;
        case Distribution::Binomial:
            apply(bm::binomial(a, b), density, x, out, n);
            break;
        case Distribution::Poisson:
            apply(bm::poisson(a), density, x, out, n);
            break;
        case Distribution::Geometric:
            apply(bm::geometric(a), density, x, out, n);
            break;
        case Distribution::NegativeBinomial:
            apply(bm::negative_binomial(a, b), density, x, out, n);
            break;
    }
}
} // namespace crd_peer::boost_math
