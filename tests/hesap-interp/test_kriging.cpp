// crd-hesap-interp v13-f — Gaussian-process kriging (mean + predictive variance) gated vs sklearn
// GaussianProcessRegressor (RBF kernel), the interpolation property, std≈√noise at the data, + determinism.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/interp/interp.hpp>

#include <crd/containers/span.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using namespace crd::hesap::interp;
using crd::containers::ConstSpan;
using crd::usize;

namespace
{
[[nodiscard]] bool close(double a, double b, double tol)
{
    const double d = a < b ? b - a : a - b;
    return d <= tol + tol * (b < 0 ? -b : b);
}
constexpr double kPts[] = {0, 0, 1, 0, 0, 1, 1, 1, 0.5, 0.5, 0.2, 0.8, 0.8, 0.3, 0.4, 0.1};
constexpr double kVal[] = {1.0, 2.0, 0.5, 3.0, 1.5, 0.8, 2.2, 1.1};
constexpr usize kN = 8;
constexpr usize kDim = 2;
} // namespace

TEST_CASE("v13-f: Gaussian-process kriging (mean + variance) vs sklearn", "[v13-f][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 18);
    const auto pts = ConstSpan<double>{kPts, kN * kDim};
    const auto val = ConstSpan<double>{kVal, kN};
    GaussianProcessInterpolant<double> gp(&alloc);
    REQUIRE(gp.fit(pts, val, kN, kDim, 1.0, 1e-6) == InterpStatus::Ok);

    constexpr double q0[] = {0.3, 0.3};
    constexpr double q1[] = {0.6, 0.7};
    constexpr double mref[] = {1.0464006015276013, 1.8239454276617906};
    constexpr double sref[] = {0.03834423521226394, 0.039404929431704427};
    double m = 0.0;
    double v = 0.0;
    gp.predict(ConstSpan<double>{q0, 2}, m, v);
    CHECK(close(m, mref[0], 1e-8));           // mean vs sklearn
    CHECK(close(std::sqrt(v), sref[0], 1e-7)); // predictive std vs sklearn
    gp.predict(ConstSpan<double>{q1, 2}, m, v);
    CHECK(close(m, mref[1], 1e-8));
    CHECK(close(std::sqrt(v), sref[1], 1e-7));
    CHECK(close(gp.mean(ConstSpan<double>{q0, 2}), mref[0], 1e-8)); // mean() agrees with predict

    // interpolation property: mean ≈ value at the data (α=1e-6 ⇒ near-interpolation)
    for (usize i = 0; i < kN; ++i)
    {
        CHECK(std::abs(gp.mean(ConstSpan<double>{&kPts[i * kDim], kDim}) - kVal[i]) < 1e-3);
    }
    // predictive std at the data ≈ √noise (the model is confident where it has observed)
    double mt = 0.0;
    double vt = 0.0;
    gp.predict(ConstSpan<double>{&kPts[0], kDim}, mt, vt);
    CHECK(std::sqrt(vt) < 2e-3);

    // determinism: bit-identical fit
    GaussianProcessInterpolant<double> gp2(&alloc);
    REQUIRE(gp2.fit(pts, val, kN, kDim, 1.0, 1e-6) == InterpStatus::Ok);
    CHECK(gp.mean(ConstSpan<double>{q1, 2}) == gp2.mean(ConstSpan<double>{q1, 2}));
}
