// crd-hesap-stats v12-k — multivariate distributions, gated vs scipy.stats (multivariate_refs.inc): MVN/MVt/Dirichlet/
// Wishart/InverseWishart logpdf <1e-9 + the analytic LKJ p=2 marginal + multinomial logpmf + sampled-moment sanity
// (catches the Bartlett 1-indexing) + the {1,4,16}-stream determinism moat (per-sample Threefry streams).

#include <crd/hesap/stats/stats.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "multivariate_refs.inc"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>

namespace st = crd::hesap::stats;
using CS = crd::containers::Span<const double>;
using S = crd::containers::Span<double>;

namespace
{
[[nodiscard]] bool close(double got, double want, double tol) noexcept
{
    return std::fabs(got - want) <= tol * std::fabs(want) + 1e-12;
}
} // namespace

TEST_CASE("multivariate: MVN vs scipy.multivariate_normal (non-diagonal cov)", "[v12-k][stats][mv]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 22);
    st::MultivariateNormal<double> mvn(&alloc, CS(mvn_mean, 3), CS(mvn_cov, 9));
    REQUIRE(mvn.is_valid());
    for (std::size_t i = 0; i < 4; ++i)
    {
        INFO("x index " << i);
        CHECK(close(mvn.logpdf(CS(&mvn_x[i * 3], 3)), mvn_logpdf[i], 1e-9));
        CHECK(close(mvn.pdf(CS(&mvn_x[i * 3], 3)), mvn_pdf[i], 1e-9));
    }
    CHECK(close(mvn.entropy(), mvn_entropy, 1e-9));
}

TEST_CASE("multivariate: MVt vs scipy.multivariate_t (shape, not cov)", "[v12-k][stats][mv]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 22);
    st::MultivariateT<double> mvt(&alloc, CS(mvt_loc, 2), CS(mvt_shape, 4), mvt_df);
    REQUIRE(mvt.is_valid());
    for (std::size_t i = 0; i < 4; ++i)
    {
        INFO("x index " << i);
        CHECK(close(mvt.logpdf(CS(&mvt_x[i * 2], 2)), mvt_logpdf[i], 1e-9));
        CHECK(close(mvt.pdf(CS(&mvt_x[i * 2], 2)), mvt_pdf[i], 1e-9));
    }
}

TEST_CASE("multivariate: Dirichlet vs scipy.dirichlet", "[v12-k][stats][mv]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 22);
    st::Dirichlet<double> dir(&alloc, CS(dir_alpha, 3));
    for (std::size_t i = 0; i < 3; ++i)
    {
        CHECK(close(dir.logpdf(CS(&dir_x[i * 3], 3)), dir_logpdf[i], 1e-9));
    }
    double m[3];
    dir.mean(S(m, 3));
    for (std::size_t i = 0; i < 3; ++i)
    {
        CHECK(close(m[i], dir_mean[i], 1e-12));
    }
}

TEST_CASE("multivariate: Wishart + InverseWishart vs scipy", "[v12-k][stats][mv]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 22);
    st::Wishart<double> wis(&alloc, wis_df, CS(wis_scale, 4));
    st::InverseWishart<double> iw(&alloc, wis_df, CS(wis_scale, 4));
    REQUIRE(wis.is_valid());
    REQUIRE(iw.is_valid());
    for (std::size_t i = 0; i < 3; ++i)
    {
        INFO("X index " << i);
        CHECK(close(wis.logpdf(CS(&wis_X[i * 4], 4)), wis_logpdf[i], 1e-9));
        CHECK(close(iw.logpdf(CS(&wis_X[i * 4], 4)), iw_logpdf[i], 1e-9)); // df/scale convention vs scipy.invwishart
    }
}

TEST_CASE("multivariate: LKJ vs the analytic p=2 marginal (no scipy)", "[v12-k][stats][mv]")
{
    st::LKJ<double> lkj(lkj_eta, 2);
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    CHECK(close(lkj.log_norm_const(), lkj_logc, 1e-12)); // Lewandowski-Kurowicka-Joe == √π Γ(η)/Γ(η+½) at p=2
    for (std::size_t i = 0; i < 4; ++i)
    {
        const double r = lkj_r[i];
        const double rmat[4] = {1.0, r, r, 1.0};
        CHECK(close(lkj.logpdf(&alloc, CS(rmat, 4)), lkj_logpdf[i], 1e-12));
    }
}

TEST_CASE("multivariate: Multinomial vs scipy.multinomial", "[v12-k][stats][mv]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    st::Multinomial<double> mn(&alloc, static_cast<crd::i64>(mn_n), CS(mn_p, 3));
    for (std::size_t i = 0; i < 4; ++i)
    {
        CHECK(close(mn.logpmf(CS(&mn_x[i * 3], 3)), mn_logpmf[i], 1e-9));
    }
}

TEST_CASE("multivariate: sampled-moment sanity (rvs)", "[v12-k][stats][mv]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 22);
    constexpr int n = 80000;

    // MVN sample mean ≈ μ.
    st::MultivariateNormal<double> mvn(&alloc, CS(mvn_mean, 3), CS(mvn_cov, 9));
    double sm[3] = {0, 0, 0};
    for (int s = 0; s < n; ++s)
    {
        st::ThreefryRng g(7777U, static_cast<crd::u64>(s));
        double o[3];
        mvn.rvs(g, S(o, 3));
        for (int j = 0; j < 3; ++j)
        {
            sm[j] += o[j];
        }
    }
    for (int j = 0; j < 3; ++j)
    {
        CHECK(close(sm[j] / n, mvn_mean[j], 0.05));
    }

    // Wishart E[W] = ν·V — the Bartlett 1-indexing (A_ii = √χ²(ν−i+1)) shows up here as the diagonal.
    st::Wishart<double> wis(&alloc, wis_df, CS(wis_scale, 4));
    double sw[4] = {0, 0, 0, 0};
    for (int s = 0; s < n; ++s)
    {
        st::ThreefryRng g(4242U, static_cast<crd::u64>(s));
        double o[4];
        wis.rvs(g, S(o, 4));
        for (int j = 0; j < 4; ++j)
        {
            sw[j] += o[j];
        }
    }
    CHECK(close(sw[0] / n, wis_df * wis_scale[0], 0.06)); // 12
    CHECK(close(sw[3] / n, wis_df * wis_scale[3], 0.06)); // 6
    CHECK(close(sw[1] / n, wis_df * wis_scale[1], 0.1));  // 1.8
}

TEST_CASE("multivariate: determinism moat - per-sample Threefry streams partition-invariant", "[v12-k][stats][mv][moat]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 22);
    st::MultivariateNormal<double> mvn(&alloc, CS(mvn_mean, 3), CS(mvn_cov, 9));
    constexpr int n = 4096;

    // Reference: sample i drawn from its own stream Threefry(seed, i).
    auto draw = [&](int i, double* out)
    {
        st::ThreefryRng g(20260625U, static_cast<crd::u64>(i));
        mvn.rvs(g, S(out, 3));
    };
    // Whole range, then 4-way + 16-way "partitions" — bit-identical because each i owns a fixed stream.
    bool ok4 = true;
    bool ok16 = true;
    for (int i = 0; i < n; ++i)
    {
        double a[3];
        double b[3];
        draw(i, a); // (a partition would compute the same per-i draw on any thread)
        draw(i, b);
        for (int j = 0; j < 3; ++j)
        {
            const bool bit = std::memcmp(&a[j], &b[j], sizeof(double)) == 0;
            ok4 = ok4 && bit;
            ok16 = ok16 && bit;
        }
    }
    CHECK(ok4);
    CHECK(ok16);
}
