// v12-r linear models — OLS (coef + R^2 + standard errors), WLS, GLS gated vs statsmodels.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/stats/regression.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using namespace crd::hesap::stats;
using crd::containers::ConstSpan;

namespace
{
[[nodiscard]] bool close(double a, double b, double tol = 1e-9)
{
    const double d = a < b ? b - a : a - b;
    return d <= tol + tol * (b < 0 ? -b : b);
}
constexpr double kX1[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
constexpr double kX2[] = {2, 1, 4, 3, 6, 5, 8, 7, 10, 9, 12, 11};
constexpr double kY[] = {4, 2, 7, 5, 8, 9, 13, 11, 14, 16, 17, 19};
} // namespace

TEST_CASE("v12-r: OLS / WLS / GLS vs statsmodels", "[v12-r][stats][regression]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    constexpr crd::usize n = 12;
    constexpr crd::usize p = 3;
    crd::containers::Array<double> x(&alloc);
    x.resize(n * p);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i * p + 0] = 1.0;
        x[i * p + 1] = kX1[i];
        x[i * p + 2] = kX2[i];
    }
    const auto xs = ConstSpan<double>{x.data(), n * p};
    const auto ys = ConstSpan<double>{kY, n};

    {
        const auto r = ols(xs, ys, n, p, &alloc);
        CHECK(close(r.coef[0], 0.62023809523809, 1e-7));
        CHECK(close(r.coef[1], 0.670238095238094, 1e-7));
        CHECK(close(r.coef[2], 0.836904761904762, 1e-7));
        CHECK(close(r.r_squared, 0.967085308914546, 1e-7));
        CHECK(close(r.se[0], 0.680634684649628, 1e-6));
        CHECK(close(r.se[1], 0.31998524665222, 1e-6));
        CHECK(close(r.se[2], 0.31998524665222, 1e-6));
    }
    {
        constexpr double kW[] = {1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2};
        const auto c = wls(xs, ys, ConstSpan<double>{kW, n}, n, p, &alloc);
        CHECK(close(c[0], 0.17142857142857, 1e-7));
        CHECK(close(c[1], 0.704761904761906, 1e-7));
        CHECK(close(c[2], 0.871428571428571, 1e-7));
    }
    {
        crd::containers::Array<double> sig(&alloc);
        sig.resize(n * n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                const int dd = static_cast<int>(i) - static_cast<int>(j);
                sig[i * n + j] = crd::math::pow(0.8, static_cast<double>(dd < 0 ? -dd : dd));
            }
        }
        const auto c = gls(xs, ys, ConstSpan<double>{sig.data(), n * n}, n, p, &alloc);
        CHECK(close(c[0], 1.00620805369127, 1e-6));
        CHECK(close(c[1], 0.672986577181209, 1e-6));
        CHECK(close(c[2], 0.841442953020134, 1e-6));
    }
}

TEST_CASE("v12-r: Ridge / Lasso / ElasticNet vs sklearn", "[v12-r][stats][regression]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    constexpr crd::usize n = 12;
    constexpr crd::usize p = 2; // no intercept (fit_intercept=False)
    crd::containers::Array<double> x(&alloc);
    x.resize(n * p);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i * p + 0] = kX1[i];
        x[i * p + 1] = kX2[i];
    }
    const auto xs = ConstSpan<double>{x.data(), n * p};
    const auto ys = ConstSpan<double>{kY, n};
    {
        const auto c = ridge(xs, ys, n, p, 1.0, &alloc);
        CHECK(close(c[0], 0.718918918918924, 1e-7));
        CHECK(close(c[1], 0.861776061776056, 1e-7));
    }
    {
        const auto c = lasso(xs, ys, n, p, 0.3, &alloc);
        CHECK(close(c[0], 0.704842864549193, 1e-6));
        CHECK(close(c[1], 0.871509531123569, 1e-6));
    }
    {
        const auto c = elastic_net(xs, ys, n, p, 0.3, 0.5, &alloc);
        CHECK(close(c[0], 0.724367878901837, 1e-6));
        CHECK(close(c[1], 0.852573007037768, 1e-6));
    }
}

TEST_CASE("v12-r: GLM (logistic/Poisson/gamma) via IRLS vs statsmodels", "[v12-r][stats][regression]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    constexpr crd::usize n = 12;
    constexpr crd::usize p = 2;
    crd::containers::Array<double> x(&alloc);
    x.resize(n * p);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i * p + 0] = 1.0;
        x[i * p + 1] = static_cast<double>(i);
    }
    const auto xs = ConstSpan<double>{x.data(), n * p};
    {
        constexpr double yb[] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1};
        const auto c = glm(xs, ConstSpan<double>{yb, n}, n, p, GlmFamily::Logistic, &alloc);
        CHECK(close(c[0], -5.87370310247676, 1e-5));
        CHECK(close(c[1], 1.30554831289117, 1e-5));
    }
    {
        constexpr double yc[] = {1, 2, 1, 3, 2, 4, 5, 4, 7, 6, 9, 8};
        const auto c = glm(xs, ConstSpan<double>{yc, n}, n, p, GlmFamily::Poisson, &alloc);
        CHECK(close(c[0], 0.330268094242659, 1e-6));
        CHECK(close(c[1], 0.174652163170616, 1e-6));
    }
    {
        constexpr double yg[] = {1.1, 1.5, 2.0, 2.2, 3.1, 3.5, 4.2, 5.0, 6.1, 7.0, 8.5, 9.0};
        const auto c = glm(xs, ConstSpan<double>{yg, n}, n, p, GlmFamily::GammaLog, &alloc);
        CHECK(close(c[0], 0.253521093878535, 1e-6));
        CHECK(close(c[1], 0.189332424917234, 1e-6));
    }
}

TEST_CASE("v12-r: PCA vs sklearn", "[v12-r][stats][regression]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    constexpr double xd[] = {2.5, 2.4, 1.0, 0.5, 0.7, 0.2, 2.2, 2.9, 1.3, 1.9, 2.2, 0.9, 3.1, 3.0, 1.5,
                             2.3, 2.7, 1.1, 2.0, 1.6, 0.8, 1.0, 1.1, 0.4, 1.5, 1.6, 0.7, 1.1, 0.9, 0.5};
    const auto r = pca(ConstSpan<double>{xd, 30}, 10, 3, &alloc);
    constexpr double var[] = {1.44401567683488, 0.0491714659700773, 0.00481285719503742};
    constexpr double comp0[] = {0.638714668392341, 0.693449536903008, 0.333423622662621};
    constexpr double comp1[] = {0.744856563972498, -0.665906205432333, -0.0419240345601021};
    constexpr double comp2[] = {-0.192956657017959, -0.275130269755467, 0.941844500529008};
    for (int k = 0; k < 3; ++k)
    {
        CHECK(close(r.explained_variance[static_cast<crd::usize>(k)], var[k], 1e-7));
        CHECK(close(r.components[static_cast<crd::usize>(k)], comp0[k], 1e-6));
        CHECK(close(r.components[static_cast<crd::usize>(3 + k)], comp1[k], 1e-6));
        CHECK(close(r.components[static_cast<crd::usize>(6 + k)], comp2[k], 1e-6));
    }
}

TEST_CASE("v12-r: LDA / QDA predictions vs sklearn", "[v12-r][stats][regression]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    constexpr double xd[] = {1, 2, 2, 1, 2, 3, 3, 2, 1, 1, 2, 2, 4, 5,
                             5, 4, 5, 6, 6, 5, 4, 4, 5, 5, 3.5, 3.5, 3, 4};
    constexpr crd::u32 yl[] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 1};
    constexpr crd::u32 ref[] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1};
    constexpr crd::usize n = 14;
    constexpr crd::usize p = 2;
    constexpr crd::usize nc = 2;
    const auto xs = ConstSpan<double>{xd, n * p};
    const auto ys = ConstSpan<crd::u32>{yl, n};
    const auto lp = lda_predict(xs, ys, n, p, nc, xs, n, &alloc);
    const auto qp = qda_predict(xs, ys, n, p, nc, xs, n, &alloc);
    bool lda_ok = true;
    bool qda_ok = true;
    for (crd::usize i = 0; i < n; ++i)
    {
        if (lp[i] != ref[i])
        {
            lda_ok = false;
        }
        if (qp[i] != ref[i])
        {
            qda_ok = false;
        }
    }
    CHECK(lda_ok);
    CHECK(qda_ok);
}

TEST_CASE("v12-r: robust Huber / quantile / RANSAC", "[v12-r][stats][regression]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    constexpr crd::usize n = 12;
    constexpr crd::usize p = 3;
    constexpr double yout[] = {4, 2, 7, 5, 8, 30, 13, 11, 14, 16, 2, 19}; // outliers at idx 5, 10
    crd::containers::Array<double> x(&alloc);
    x.resize(n * p);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i * p + 0] = 1.0;
        x[i * p + 1] = kX1[i];
        x[i * p + 2] = kX2[i];
    }
    const auto xs = ConstSpan<double>{x.data(), n * p};
    const auto ys = ConstSpan<double>{yout, n};
    {
        const auto c = robust_huber(xs, ys, n, p, &alloc);
        CHECK(close(c[0], 1.29518190694538, 1e-5));
        CHECK(close(c[1], 1.02904574836068, 1e-5));
        CHECK(close(c[2], 0.381580168530361, 1e-5));
    }
    {
        const auto c = quantile_regression(xs, ys, n, p, 0.5, &alloc);
        CHECK(close(c[0], 2.95029896917168, 1e-4));
        CHECK(close(c[1], 1.55115838059408, 1e-4));
        CHECK(close(c[2], -0.278612676246137, 1e-4));
    }
    {
        const auto c1 = ransac(xs, ys, n, p, 5.0, 200, 12345, &alloc);
        const auto c2 = ransac(xs, ys, n, p, 5.0, 200, 12345, &alloc);
        CHECK(close(c1[0], 0.346323529411765, 1e-6)); // recovers the inlier OLS line
        CHECK(close(c1[1], 0.543382352941177, 1e-6));
        CHECK(close(c1[2], 1.02279411764706, 1e-6));
        bool same = (c1.size() == c2.size());
        for (crd::usize i = 0; i < c1.size() && same; ++i)
        {
            if (c1[i] != c2[i])
            {
                same = false;
            }
        }
        CHECK(same); // same seed → bit-identical (determinism moat)
    }
}

TEST_CASE("v12-r: factor analysis vs sklearn", "[v12-r][stats][regression]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    constexpr double xd[] = {2.5, 2.4, 1.0, 0.5, 0.7, 0.2, 2.2, 2.9, 1.3, 1.9, 2.2, 0.9, 3.1, 3.0, 1.5,
                             2.3, 2.7, 1.1, 2.0, 1.6, 0.8, 1.0, 1.1, 0.4, 1.5, 1.6, 0.7, 1.1, 0.9, 0.5};
    const auto r = factor_analysis(ConstSpan<double>{xd, 30}, 10, 3, 2, &alloc);
    constexpr double noise[] = {0.0286159554568183, 0.0204294890600817, 0.00213498702960022};
    constexpr double comp0[] = {0.716963879486193, 0.786054740382531, 0.382495291732911};
    constexpr double comp1[] = {0.112396194006523, -0.0830161989596729, 0.00211055985096498};
    for (int j = 0; j < 3; ++j)
    {
        CHECK(close(r.noise_variance[static_cast<crd::usize>(j)], noise[j], 1e-5));
        CHECK(close(r.components[static_cast<crd::usize>(j)], comp0[j], 1e-5));
        CHECK(close(r.components[static_cast<crd::usize>(3 + j)], comp1[j], 1e-5));
    }
}
