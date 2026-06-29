// crd-hesap-interp v13-e — scattered N-D RBF + Shepard. RBF kernels gated ≤1e-8 vs scipy.RBFInterpolator (2-D),
// the interpolation property s(xᵢ)=fᵢ, thin-plate polynomial reproduction, Wendland compact-support interpolation,
// Shepard IDW (interpolation + boundedness), + determinism.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/interp/interp.hpp>

#include <crd/containers/span.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

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
constexpr double kQ0[] = {0.3, 0.3};
constexpr double kQ1[] = {0.6, 0.7};
} // namespace

TEST_CASE("v13-e: RBF kernels vs scipy + interpolation property", "[v13-e][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 18);
    const auto pts = ConstSpan<double>{kPts, kN * kDim};
    const auto val = ConstSpan<double>{kVal, kN};
    auto check_rbf = [&](RbfKernel k, usize deg, double r0, double r1) {
        RbfInterpolant<double> rbf(&alloc);
        REQUIRE(rbf.build(pts, val, kN, kDim, k, 1.0, deg) == InterpStatus::Ok);
        CHECK(close(rbf.eval(ConstSpan<double>{kQ0, kDim}), r0, 1e-8)); // vs scipy
        CHECK(close(rbf.eval(ConstSpan<double>{kQ1, kDim}), r1, 1e-8));
        for (usize i = 0; i < kN; ++i) // interpolation property s(xᵢ)=fᵢ
        {
            CHECK(close(rbf.eval(ConstSpan<double>{&kPts[i * kDim], kDim}), kVal[i], 1e-7));
        }
    };
    check_rbf(RbfKernel::Gaussian, 0, 0.979572579938689, 1.90763595178452);
    check_rbf(RbfKernel::InverseMultiquadric, 0, 0.976062451902276, 1.91236602588276);
    check_rbf(RbfKernel::Multiquadric, 0, 1.0147836690821, 1.85321406975723);
    check_rbf(RbfKernel::ThinPlateSpline, 1, 1.04655339580819, 1.82898502879877);
    check_rbf(RbfKernel::Cubic, 1, 1.03900375849064, 1.81137702945063);
}

TEST_CASE("v13-e: thin-plate polynomial reproduction + Wendland compact-support", "[v13-e][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 18);
    const auto pts = ConstSpan<double>{kPts, kN * kDim};
    // linear data f = 2x − 3y + 1.5 ⇒ thin-plate (degree 1) reproduces it exactly
    double lin[kN];
    for (usize i = 0; i < kN; ++i)
    {
        lin[i] = 2.0 * kPts[i * kDim] - 3.0 * kPts[i * kDim + 1] + 1.5;
    }
    RbfInterpolant<double> tps(&alloc);
    REQUIRE(tps.build(pts, ConstSpan<double>{lin, kN}, kN, kDim, RbfKernel::ThinPlateSpline, 1.0, 1) ==
            InterpStatus::Ok);
    const double e0 = 2.0 * kQ0[0] - 3.0 * kQ0[1] + 1.5;
    const double e1 = 2.0 * kQ1[0] - 3.0 * kQ1[1] + 1.5;
    CHECK(close(tps.eval(ConstSpan<double>{kQ0, kDim}), e0, 1e-9)); // polynomial reproduction
    CHECK(close(tps.eval(ConstSpan<double>{kQ1, kDim}), e1, 1e-9));

    // Wendland (compact support, ε=0.5 ⇒ support radius 2 covers all points) — interpolation property
    RbfInterpolant<double> wnd(&alloc);
    REQUIRE(wnd.build(pts, ConstSpan<double>{kVal, kN}, kN, kDim, RbfKernel::Wendland, 0.5, 0) == InterpStatus::Ok);
    for (usize i = 0; i < kN; ++i)
    {
        CHECK(close(wnd.eval(ConstSpan<double>{&kPts[i * kDim], kDim}), kVal[i], 1e-7));
    }
}

TEST_CASE("v13-e: Shepard IDW + determinism", "[v13-e][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 16);
    const auto pts = ConstSpan<double>{kPts, kN * kDim};
    const auto val = ConstSpan<double>{kVal, kN};
    ShepardInterpolant<double> sh;
    REQUIRE(sh.build(pts, val, kN, kDim, 2.0) == InterpStatus::Ok);
    for (usize i = 0; i < kN; ++i) // interpolation property (the 0-distance limit)
    {
        CHECK(close(sh.eval(ConstSpan<double>{&kPts[i * kDim], kDim}), kVal[i], 1e-12));
    }
    // boundedness: IDW stays within [min,max] of the data (no overshoot)
    double vmin = kVal[0];
    double vmax = kVal[0];
    for (usize i = 1; i < kN; ++i)
    {
        vmin = kVal[i] < vmin ? kVal[i] : vmin;
        vmax = kVal[i] > vmax ? kVal[i] : vmax;
    }
    const double sv = sh.eval(ConstSpan<double>{kQ0, kDim});
    CHECK(sv >= vmin - 1e-12);
    CHECK(sv <= vmax + 1e-12);

    // determinism: RBF weights bit-identical across builds
    RbfInterpolant<double> a(&alloc);
    RbfInterpolant<double> b(&alloc);
    REQUIRE(a.build(pts, val, kN, kDim, RbfKernel::Gaussian, 1.0, 0) == InterpStatus::Ok);
    REQUIRE(b.build(pts, val, kN, kDim, RbfKernel::Gaussian, 1.0, 0) == InterpStatus::Ok);
    CHECK(a.eval(ConstSpan<double>{kQ0, kDim}) == b.eval(ConstSpan<double>{kQ0, kDim}));
}
