#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/detail/dqds.hpp>
#include <crd/hesap/dense/detail/mrrr_vectors.hpp>
#include <crd/hesap/dense/detail/sturm_count.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "hesap_jobs_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

// =======================================================================
// Phase 3.1.6 v3a-3.1 — MRRR eigenvalue substrate (Sturm-count bisection).
// The first leaf of MRRR (dstemr): dlarra split + the dlarrc/dlarrk Sturm
// machinery + the per-block bisection driver. Gated against the closed-form
// Toeplitz spectrum and against the shipped `eig_sym` solver (oracle).
// =======================================================================

namespace detail = crd::hesap::dense::detail;
using crd::hesap::dense::eig_sym;
using crd::hesap::dense::Symmetric;

namespace
{
// Build a symmetric tridiagonal Symmetric<T> from (d, e) for the oracle.
template <typename T>
Symmetric<T> make_tridiag(crd::memory::IAllocator* alloc, const T* d, const T* e, int n)
{
    Symmetric<T> a(alloc, static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i)
    {
        a.at(static_cast<crd::usize>(i), static_cast<crd::usize>(i)) = d[i];
    }
    for (int i = 0; i < n - 1; ++i)
    {
        a.at(static_cast<crd::usize>(i) + 1, static_cast<crd::usize>(i)) = e[i];  // lower sub-diagonal
    }
    return a;
}

// Closed-form spectrum of the symmetric tridiagonal Toeplitz matrix with
// diagonal `a` and off-diagonal `b`:  lambda_k = a + 2|b| cos(k*pi/(n+1)).
template <typename R>
void toeplitz_spectrum(R a, R b, int n, R* out_ascending)
{
    const R pi = static_cast<R>(3.14159265358979323846);
    for (int k = 1; k <= n; ++k)
    {
        out_ascending[k - 1] = a + R{2} * std::abs(b) * std::cos(static_cast<R>(k) * pi / static_cast<R>(n + 1));
    }
    // cos is decreasing in k, so the array is currently descending; reverse it.
    for (int i = 0; i < n / 2; ++i)
    {
        const R t = out_ascending[i];
        out_ascending[i] = out_ascending[n - 1 - i];
        out_ascending[n - 1 - i] = t;
    }
}
} // namespace

TEST_CASE("sturm_negcount: diagonal matrix counts d_i <= x", "[hesap][eig][mrrr][sturm]")
{
    // Diagonal (e2 = 0): negcount(x) = #{ d_i <= x }.
    const double d[4] = {1.0, 3.0, 5.0, 7.0};
    const double e2[3] = {0.0, 0.0, 0.0};
    const double pivmin = detail::sturm_safmin<double>();

    CHECK(detail::sturm_negcount<double>(d, e2, 4, 0.0, pivmin) == 0);
    CHECK(detail::sturm_negcount<double>(d, e2, 4, 2.0, pivmin) == 1);
    CHECK(detail::sturm_negcount<double>(d, e2, 4, 4.0, pivmin) == 2);
    CHECK(detail::sturm_negcount<double>(d, e2, 4, 6.0, pivmin) == 3);
    CHECK(detail::sturm_negcount<double>(d, e2, 4, 8.0, pivmin) == 4);
}

TEST_CASE("sturm_interval_count == negcount(vu) - negcount(vl)", "[hesap][eig][mrrr][sturm]")
{
    // A generic unreduced tridiagonal.
    const double d[5] = {2.0, -1.0, 3.0, 0.5, 4.0};
    double e2[5];
    const double e[4] = {0.7, 1.3, 0.4, 1.1};
    for (int i = 0; i < 4; ++i)
    {
        e2[i] = e[i] * e[i];
    }
    const double pivmin = detail::compute_pivmin<double>(e, 5);

    const double vls[3] = {-3.0, 0.0, 2.5};
    const double vus[3] = {1.0, 3.0, 6.0};
    for (int t = 0; t < 3; ++t)
    {
        const int fused = detail::sturm_interval_count<double>(d, e2, 5, vls[t], vus[t], pivmin);
        const int diff = detail::sturm_negcount<double>(d, e2, 5, vus[t], pivmin) -
                         detail::sturm_negcount<double>(d, e2, 5, vls[t], pivmin);
        CHECK(fused == diff);
        CHECK(fused >= 0);
    }
    // Whole-spectrum interval contains all 5 eigenvalues.
    CHECK(detail::sturm_interval_count<double>(d, e2, 5, -100.0, 100.0, pivmin) == 5);
}

TEST_CASE("tridiag_split: small off-diagonal decouples blocks", "[hesap][eig][mrrr][split]")
{
    const double d[4] = {1.0, 2.0, 3.0, 4.0};
    double e[4] = {0.5, 1.0e-30, 0.5, 0.0};
    double e2[4];
    for (int i = 0; i < 4; ++i)
    {
        e2[i] = e[i] * e[i];
    }
    int isplit[4];
    const double spltol = std::sqrt(std::numeric_limits<double>::epsilon());
    const int nsplit = detail::tridiag_split<double>(d, e, e2, 4, spltol, isplit);

    CHECK(nsplit == 2);
    CHECK(isplit[0] == 1);  // first block rows 0..1
    CHECK(isplit[1] == 3);  // second block rows 2..3
    CHECK(e[1] == 0.0);     // the tiny coupling was zeroed
    CHECK(e2[1] == 0.0);

    // A fully-coupled matrix is a single block.
    double e_full[4] = {0.5, 0.5, 0.5, 0.0};
    double e2_full[4];
    for (int i = 0; i < 4; ++i)
    {
        e2_full[i] = e_full[i] * e_full[i];
    }
    int isplit_full[4];
    CHECK(detail::tridiag_split<double>(d, e_full, e2_full, 4, spltol, isplit_full) == 1);
    CHECK(isplit_full[0] == 3);
}

TEST_CASE("gershgorin_bounds bracket the spectrum", "[hesap][eig][mrrr][gershgorin]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const int n = 12;
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = 1.0 + 0.37 * static_cast<double>(i) - 0.05 * static_cast<double>(i * i);
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 0.4 + 0.11 * static_cast<double>(i);
    }

    double gl;
    double gu;
    detail::gershgorin_bounds<double>(d.data(), e.data(), n, gl, gu);

    const auto a = make_tridiag<double>(&alloc, d.data(), e.data(), n);
    const auto eig = eig_sym<double>(&alloc, a);
    const double lo = eig.values.data()[0];
    const double hi = eig.values.data()[n - 1];

    CHECK(gl <= lo);
    CHECK(gu >= hi);
}

TEST_CASE("tridiag_eigenvalues match the Toeplitz closed form", "[hesap][eig][mrrr][bisect]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const int n = 24;
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    crd::containers::Array<double> ework(n, &alloc);
    crd::containers::Array<double> e2work(n, &alloc);
    crd::containers::Array<double> w(n, &alloc);
    crd::containers::Array<double> exact(n, &alloc);
    crd::containers::Array<int> isplit(n, &alloc);
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = 2.0;
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 1.0;
    }

    const double reltol = 4.0 * std::numeric_limits<double>::epsilon();
    detail::tridiag_eigenvalues<double>(d.data(), e.data(), n, ework.data(), e2work.data(), isplit.data(), w.data(),
                                        nullptr, reltol);
    toeplitz_spectrum<double>(2.0, 1.0, n, exact.data());

    double worst = 0.0;
    for (int i = 0; i < n; ++i)
    {
        worst = std::max(worst, std::abs(w.data()[i] - exact.data()[i]));
    }
    CHECK(worst < 1.0e-10);
}

TEST_CASE("tridiag_eigenvalues match eig_sym on a random tridiagonal", "[hesap][eig][mrrr][bisect]")
{
    crd::memory::TlsfAllocator alloc(2U * 1024U * 1024U);
    const int n = 40;
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    crd::containers::Array<double> ework(n, &alloc);
    crd::containers::Array<double> e2work(n, &alloc);
    crd::containers::Array<double> w(n, &alloc);
    crd::containers::Array<int> isplit(n, &alloc);

    // Deterministic pseudo-random tridiagonal (LCG; no RNG dependency).
    crd::u64 s = 0x2545F4914F6CDD1DULL;
    auto next = [&s]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & 0xFFFFFFFFFFFFFULL) / static_cast<double>(1ULL << 52);
    };
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = 2.0 * next() - 1.0;
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 0.5 + next();  // bounded away from 0 → single unreduced block
    }

    detail::tridiag_eigenvalues<double>(d.data(), e.data(), n, ework.data(), e2work.data(), isplit.data(), w.data(),
                                        nullptr, 4.0 * std::numeric_limits<double>::epsilon());

    const auto a = make_tridiag<double>(&alloc, d.data(), e.data(), n);
    const auto eig = eig_sym<double>(&alloc, a);

    double worst = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double scale = std::max(1.0, std::abs(eig.values.data()[i]));
        worst = std::max(worst, std::abs(w.data()[i] - eig.values.data()[i]) / scale);
    }
    CHECK(worst < 1.0e-9);
}

TEST_CASE("tridiag_eigenvalues handle a reducible (block) matrix", "[hesap][eig][mrrr][bisect]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const int n = 8;
    // Two 4x4 blocks, coupling at index 3 set to zero.
    double d[8] = {4.0, 1.0, 3.0, 2.0, -1.0, 5.0, 0.0, 2.0};
    double e[8] = {0.6, 0.9, 0.3, 0.0, 0.7, 0.5, 0.8, 0.0};

    crd::containers::Array<double> ework(n, &alloc);
    crd::containers::Array<double> e2work(n, &alloc);
    crd::containers::Array<double> w(n, &alloc);
    crd::containers::Array<int> isplit(n, &alloc);

    detail::tridiag_eigenvalues<double>(d, e, n, ework.data(), e2work.data(), isplit.data(), w.data(), nullptr,
                                        4.0 * std::numeric_limits<double>::epsilon());

    const auto a = make_tridiag<double>(&alloc, d, e, n);  // a.at uses only tridiag entries; e[3]=0
    const auto eig = eig_sym<double>(&alloc, a);

    double worst = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double scale = std::max(1.0, std::abs(eig.values.data()[i]));
        worst = std::max(worst, std::abs(w.data()[i] - eig.values.data()[i]) / scale);
    }
    CHECK(worst < 1.0e-9);
    // Ascending output.
    for (int i = 1; i < n; ++i)
    {
        CHECK(w.data()[i] >= w.data()[i - 1]);
    }
}

TEST_CASE("tridiag_eigenvalues f32 Toeplitz", "[hesap][eig][mrrr][bisect]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const int n = 16;
    crd::containers::Array<float> d(n, &alloc);
    crd::containers::Array<float> e(n, &alloc);
    crd::containers::Array<float> ework(n, &alloc);
    crd::containers::Array<float> e2work(n, &alloc);
    crd::containers::Array<float> w(n, &alloc);
    crd::containers::Array<float> exact(n, &alloc);
    crd::containers::Array<int> isplit(n, &alloc);
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = 2.0F;
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 1.0F;
    }

    detail::tridiag_eigenvalues<float>(d.data(), e.data(), n, ework.data(), e2work.data(), isplit.data(), w.data(),
                                       nullptr, 4.0F * std::numeric_limits<float>::epsilon());
    toeplitz_spectrum<float>(2.0F, 1.0F, n, exact.data());

    float worst = 0.0F;
    for (int i = 0; i < n; ++i)
    {
        worst = std::max(worst, std::abs(w.data()[i] - exact.data()[i]));
    }
    CHECK(worst < 1.0e-4F);
}

TEST_CASE("tridiag_eigenvalues are bit-identical across runs (determinism)", "[hesap][eig][mrrr][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const int n = 20;
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    crd::containers::Array<double> ework(n, &alloc);
    crd::containers::Array<double> e2work(n, &alloc);
    crd::containers::Array<double> w1(n, &alloc);
    crd::containers::Array<double> w2(n, &alloc);
    crd::containers::Array<int> isplit(n, &alloc);
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = 1.5 - 0.1 * static_cast<double>(i);
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 0.6 + 0.02 * static_cast<double>(i);
    }

    const double reltol = 4.0 * std::numeric_limits<double>::epsilon();
    detail::tridiag_eigenvalues<double>(d.data(), e.data(), n, ework.data(), e2work.data(), isplit.data(), w1.data(),
                                        nullptr, reltol);
    detail::tridiag_eigenvalues<double>(d.data(), e.data(), n, ework.data(), e2work.data(), isplit.data(), w2.data(),
                                        nullptr, reltol);

    CHECK(std::memcmp(w1.data(), w2.data(), static_cast<crd::usize>(n) * sizeof(double)) == 0);
}

// =======================================================================
// v3a-3.1-dqds-a — dqds inner kernels + unshifted dqd driver.
// =======================================================================

TEST_CASE("dlasq6 one dqd step matches the independent Rutishauser transform", "[hesap][eig][mrrr][dqds]")
{
    // A positive qd array (q_k > 0, e_k > 0). n = 5 (dlasq6 needs n >= 3).
    const int n = 5;
    const double q[5] = {4.0, 3.0, 5.0, 2.0, 6.0};
    const double e[4] = {0.7, 1.1, 0.4, 0.9};

    // Independent stationary dqd (Rutishauser): (q,e) -> (qhat, ehat).
    double qhat[5];
    double ehat[4];
    {
        double dd = q[0];
        for (int i = 0; i < n - 1; ++i)
        {
            qhat[i] = dd + e[i];
            const double r = q[i + 1] / qhat[i];
            ehat[i] = e[i] * r;
            dd = dd * r;
        }
        qhat[n - 1] = dd;
    }

    // Lay out the 4-wide ping-pong qd array and run dlasq6 (pp = 0).
    double zbuf[4 * 5 + 4];
    for (double& v : zbuf)
    {
        v = 0.0;
    }
    detail::Z1<double> z{zbuf};
    for (int k = 1; k <= n; ++k)
    {
        z[4 * k - 3] = q[k - 1];
    }
    for (int k = 1; k <= n - 1; ++k)
    {
        z[4 * k - 1] = e[k - 1];
    }
    double dmin;
    double dmin1;
    double dmin2;
    double dn;
    double dnm1;
    double dnm2;
    detail::dlasq6<double>(1, n, z, 0, dmin, dmin1, dmin2, dn, dnm1, dnm2);

    // New q's land in the pong slots z[4k-2], new e's in z[4k].
    for (int k = 1; k <= n; ++k)
    {
        CHECK(std::abs(z[4 * k - 2] - qhat[k - 1]) <= 1.0e-12 * std::abs(qhat[k - 1]));
    }
    for (int k = 1; k <= n - 1; ++k)
    {
        CHECK(std::abs(z[4 * k] - ehat[k - 1]) <= 1.0e-12 * std::abs(ehat[k - 1]));
    }
}

TEST_CASE("dlasq5 with tau=0 reproduces dlasq6 (dqd is shift-0 dqds)", "[hesap][eig][mrrr][dqds]")
{
    const int n = 6;
    const double q[6] = {5.0, 3.0, 4.5, 2.5, 6.0, 3.5};
    const double e[5] = {0.5, 0.8, 0.3, 0.6, 0.4};

    auto setup = [&](double* buf) {
        for (int i = 0; i < 4 * n + 4; ++i)
        {
            buf[i] = 0.0;
        }
        detail::Z1<double> zz{buf};
        for (int k = 1; k <= n; ++k)
        {
            zz[4 * k - 3] = q[k - 1];
        }
        for (int k = 1; k <= n - 1; ++k)
        {
            zz[4 * k - 1] = e[k - 1];
        }
    };

    double z6[4 * 6 + 4];
    double z5[4 * 6 + 4];
    setup(z6);
    setup(z5);
    detail::Z1<double> zq6{z6};
    detail::Z1<double> zq5{z5};
    double a;
    double b;
    double c;
    double dd;
    double ee;
    double ff;
    detail::dlasq6<double>(1, n, zq6, 0, a, b, c, dd, ee, ff);
    double tau = 0.0;
    detail::dlasq5<double>(1, n, zq5, 0, tau, 0.0, a, b, c, dd, ee, ff, std::numeric_limits<double>::epsilon());

    for (int k = 1; k <= n; ++k)
    {
        CHECK(std::abs(zq6[4 * k - 2] - zq5[4 * k - 2]) <= 1.0e-12 * std::abs(zq6[4 * k - 2]));
    }
}

TEST_CASE("build_qd_ldlt reconstructs the shifted tridiagonal", "[hesap][eig][mrrr][dqds]")
{
    const int n = 5;
    const double d[5] = {4.0, 5.0, 6.0, 5.0, 4.0};
    const double e[4] = {1.0, 1.0, 1.0, 1.0};
    const double sigma = -2.0;  // strict lower bound (T - sigma I is PD)

    double q[6];
    double qe[5];
    REQUIRE(detail::build_qd_ldlt<double>(d, e, n, sigma, q, qe));

    // Reconstruct: (d[i]-sigma) == q[i+1] + lld[i],  e[i-1]^2 == lld[i]*q[i].
    CHECK(std::abs((d[0] - sigma) - q[1]) <= 1.0e-12);
    for (int i = 2; i <= n; ++i)
    {
        CHECK(std::abs((d[i - 1] - sigma) - (q[i] + qe[i - 1])) <= 1.0e-12 * std::abs(d[i - 1] - sigma));
        CHECK(std::abs(e[i - 2] * e[i - 2] - qe[i - 1] * q[i - 1]) <= 1.0e-12);
        CHECK(q[i] > 0.0);
    }
}

TEST_CASE("dqd_eigenvalues_unshifted matches the Toeplitz closed form", "[hesap][eig][mrrr][dqds]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const int n = 8;
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    crd::containers::Array<double> zbuf(4 * n + 8, &alloc);
    crd::containers::Array<double> q(n + 2, &alloc);
    crd::containers::Array<double> qe(n + 1, &alloc);
    crd::containers::Array<double> w(n, &alloc);
    crd::containers::Array<double> exact(n, &alloc);
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = 2.0;
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 1.0;
    }

    REQUIRE(detail::dqd_eigenvalues_unshifted<double>(d.data(), e.data(), n, zbuf.data(), q.data(), qe.data(),
                                                      w.data()));
    toeplitz_spectrum<double>(2.0, 1.0, n, exact.data());

    double worst = 0.0;
    for (int i = 0; i < n; ++i)
    {
        worst = std::max(worst, std::abs(w.data()[i] - exact.data()[i]));
    }
    CHECK(worst < 1.0e-9);
}

TEST_CASE("dqd_eigenvalues_unshifted matches eig_sym on a random PD tridiagonal", "[hesap][eig][mrrr][dqds]")
{
    crd::memory::TlsfAllocator alloc(2U * 1024U * 1024U);
    const int n = 10;
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    crd::containers::Array<double> zbuf(4 * n + 8, &alloc);
    crd::containers::Array<double> q(n + 2, &alloc);
    crd::containers::Array<double> qe(n + 1, &alloc);
    crd::containers::Array<double> w(n, &alloc);

    // Diagonally-dominant (well-separated, fast unshifted convergence).
    crd::u64 s = 0x9E3779B97F4A7C15ULL;
    auto next = [&s]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & 0xFFFFFFFFFFFFFULL) / static_cast<double>(1ULL << 52);
    };
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = 10.0 + 4.0 * next();
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 0.5 + 0.5 * next();
    }

    REQUIRE(detail::dqd_eigenvalues_unshifted<double>(d.data(), e.data(), n, zbuf.data(), q.data(), qe.data(),
                                                      w.data()));

    const auto a = make_tridiag<double>(&alloc, d.data(), e.data(), n);
    const auto eig = eig_sym<double>(&alloc, a);
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double scale = std::max(1.0, std::abs(eig.values.data()[i]));
        worst = std::max(worst, std::abs(w.data()[i] - eig.values.data()[i]) / scale);
    }
    CHECK(worst < 1.0e-9);
}

// =======================================================================
// v3a-3.1-dqds-b — shifted dqds driver (dlasq2/3/4) — the O(n^2) engine.
// =======================================================================

namespace
{
// Scratch bundle for the dqds drivers.
struct DqdsScratch
{
    crd::containers::Array<double> ework;
    crd::containers::Array<double> e2work;
    crd::containers::Array<int> isplit;
    crd::containers::Array<double> z;
    crd::containers::Array<double> q;
    crd::containers::Array<double> qe;
    DqdsScratch(int n, crd::memory::IAllocator* a)
        : ework(n, a), e2work(n, a), isplit(n, a), z(4 * n + 8, a), q(n + 2, a), qe(n + 1, a)
    {
    }
};
} // namespace

TEST_CASE("dqds_eigenvalues match eig_sym on a general unreduced tridiagonal", "[hesap][eig][mrrr][dqds]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);
    const int n = 40;
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    crd::containers::Array<double> w(n, &alloc);
    DqdsScratch sc(n, &alloc);

    crd::u64 s = 0xD1B54A32D192ED03ULL;
    auto next = [&s]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & 0xFFFFFFFFFFFFFULL) / static_cast<double>(1ULL << 52);
    };
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = 3.0 * next() - 1.0;  // general (indefinite-ish) diagonal
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 0.4 + next();  // bounded away from 0 → single unreduced block
    }

    REQUIRE(detail::dqds_eigenvalues<double>(d.data(), e.data(), n, sc.z.data(), sc.q.data(), sc.qe.data(), w.data()));

    const auto a = make_tridiag<double>(&alloc, d.data(), e.data(), n);
    const auto eig = eig_sym<double>(&alloc, a);
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double scale = std::max(1.0, std::abs(eig.values.data()[i]));
        worst = std::max(worst, std::abs(w.data()[i] - eig.values.data()[i]) / scale);
    }
    CHECK(worst < 1.0e-10);
}

TEST_CASE("dqds_eigenvalues high relative accuracy on a graded spectrum", "[hesap][eig][mrrr][dqds]")
{
    // A graded PD tridiagonal whose eigenvalues span many orders of magnitude —
    // the regime where dqds's high RELATIVE accuracy is the whole point.
    crd::memory::TlsfAllocator alloc(2U * 1024U * 1024U);
    const int n = 20;
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    crd::containers::Array<double> w(n, &alloc);
    DqdsScratch sc(n, &alloc);
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = std::pow(10.0, static_cast<double>(i) - 8.0);  // 1e-8 .. 1e11
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 0.01 * std::sqrt(d.data()[i] * d.data()[i + 1]);
    }

    REQUIRE(detail::dqds_eigenvalues<double>(d.data(), e.data(), n, sc.z.data(), sc.q.data(), sc.qe.data(), w.data()));

    const auto a = make_tridiag<double>(&alloc, d.data(), e.data(), n);
    const auto eig = eig_sym<double>(&alloc, a);
    double worst_rel = 0.0;
    for (int i = 0; i < n; ++i)
    {
        worst_rel = std::max(worst_rel, std::abs(w.data()[i] - eig.values.data()[i]) / std::abs(eig.values.data()[i]));
    }
    CHECK(worst_rel < 1.0e-10);  // RELATIVE accuracy even for the tiny eigenvalues
}

TEST_CASE("dqds_eigenvalues match the Toeplitz closed form at scale", "[hesap][eig][mrrr][dqds]")
{
    crd::memory::TlsfAllocator alloc(2U * 1024U * 1024U);
    const int n = 64;
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    crd::containers::Array<double> w(n, &alloc);
    crd::containers::Array<double> exact(n, &alloc);
    DqdsScratch sc(n, &alloc);
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = 2.0;
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 1.0;
    }

    REQUIRE(detail::dqds_eigenvalues<double>(d.data(), e.data(), n, sc.z.data(), sc.q.data(), sc.qe.data(), w.data()));
    toeplitz_spectrum<double>(2.0, 1.0, n, exact.data());
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
    {
        worst = std::max(worst, std::abs(w.data()[i] - exact.data()[i]));
    }
    CHECK(worst < 1.0e-12);
}

TEST_CASE("tridiag_eigenvalues_dqds match eig_sym on a reducible matrix", "[hesap][eig][mrrr][dqds]")
{
    crd::memory::TlsfAllocator alloc(2U * 1024U * 1024U);
    const int n = 12;
    double d[12] = {4.0, 1.0, 3.0, 2.0, -1.0, 5.0, 0.0, 2.0, 3.0, 1.5, 4.5, 2.5};
    double e[12] = {0.6, 0.9, 0.3, 0.0, 0.7, 0.5, 0.8, 0.0, 0.4, 0.9, 0.6, 0.0};
    crd::containers::Array<double> w(n, &alloc);
    DqdsScratch sc(n, &alloc);

    detail::tridiag_eigenvalues_dqds<double>(d, e, n, sc.ework.data(), sc.e2work.data(), sc.isplit.data(),
                                             sc.z.data(), sc.q.data(), sc.qe.data(), w.data(),
                                             4.0 * std::numeric_limits<double>::epsilon());

    const auto a = make_tridiag<double>(&alloc, d, e, n);
    const auto eig = eig_sym<double>(&alloc, a);
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double scale = std::max(1.0, std::abs(eig.values.data()[i]));
        worst = std::max(worst, std::abs(w.data()[i] - eig.values.data()[i]) / scale);
    }
    CHECK(worst < 1.0e-9);
    for (int i = 1; i < n; ++i)
    {
        CHECK(w.data()[i] >= w.data()[i - 1]);
    }
}

TEST_CASE("dqds_eigenvalues f32 Toeplitz", "[hesap][eig][mrrr][dqds]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const int n = 24;
    crd::containers::Array<float> d(n, &alloc);
    crd::containers::Array<float> e(n, &alloc);
    crd::containers::Array<float> w(n, &alloc);
    crd::containers::Array<float> exact(n, &alloc);
    crd::containers::Array<float> z(4 * n + 8, &alloc);
    crd::containers::Array<float> q(n + 2, &alloc);
    crd::containers::Array<float> qe(n + 1, &alloc);
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = 2.0F;
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 1.0F;
    }

    REQUIRE(detail::dqds_eigenvalues<float>(d.data(), e.data(), n, z.data(), q.data(), qe.data(), w.data()));
    toeplitz_spectrum<float>(2.0F, 1.0F, n, exact.data());
    float worst = 0.0F;
    for (int i = 0; i < n; ++i)
    {
        worst = std::max(worst, std::abs(w.data()[i] - exact.data()[i]));
    }
    CHECK(worst < 1.0e-4F);
}

TEST_CASE("dqds_eigenvalues are bit-identical across runs (determinism)", "[hesap][eig][mrrr][dqds]")
{
    crd::memory::TlsfAllocator alloc(2U * 1024U * 1024U);
    const int n = 32;
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    crd::containers::Array<double> w1(n, &alloc);
    crd::containers::Array<double> w2(n, &alloc);
    DqdsScratch sc(n, &alloc);
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = 1.3 - 0.07 * static_cast<double>(i);
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 0.5 + 0.01 * static_cast<double>(i);
    }

    REQUIRE(
        detail::dqds_eigenvalues<double>(d.data(), e.data(), n, sc.z.data(), sc.q.data(), sc.qe.data(), w1.data()));
    REQUIRE(
        detail::dqds_eigenvalues<double>(d.data(), e.data(), n, sc.z.data(), sc.q.data(), sc.qe.data(), w2.data()));
    CHECK(std::memcmp(w1.data(), w2.data(), static_cast<crd::usize>(n) * sizeof(double)) == 0);
}

TEST_CASE("eigvals_sym (public dqds path) matches eig_sym on a dense symmetric", "[hesap][eig][mrrr][eigvals]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    const int n = 32;
    Symmetric<double> a(&alloc, static_cast<crd::usize>(n));
    crd::u64 s = 0x0BADC0DE5EED1234ULL;
    auto next = [&s]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & 0xFFFFFFFFFFFFFULL) / static_cast<double>(1ULL << 52);
    };
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j <= i; ++j)
        {
            a.at(static_cast<crd::usize>(i), static_cast<crd::usize>(j)) = 2.0 * next() - 1.0;
        }
    }

    const auto vals = crd::hesap::dense::eigvals_sym<double>(&alloc, a);
    const auto eig = eig_sym<double>(&alloc, a);
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double scale = std::max(1.0, std::abs(eig.values.data()[i]));
        worst = std::max(worst, std::abs(vals.data()[i] - eig.values.data()[i]) / scale);
    }
    CHECK(worst < 1.0e-9);
    for (int i = 1; i < n; ++i)
    {
        CHECK(vals.data()[i] >= vals.data()[i - 1]);
    }
}

TEST_CASE("dlaneg (twisted Sturm count on L D L^T) matches the tridiagonal Sturm count",
          "[hesap][eig][mrrr][vectors]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const int n = 14;
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    crd::containers::Array<double> e2(n, &alloc);
    crd::containers::Array<double> dfac(n, &alloc);
    crd::containers::Array<double> lldfac(n, &alloc);
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = 1.0 + 0.5 * static_cast<double>(i);
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 0.6;
        e2.data()[i] = 0.36;
    }
    const double sroot = -3.0;  // RRR shift (T - sroot I, PD)
    dfac.data()[0] = d.data()[0] - sroot;
    for (int i = 1; i < n; ++i)
    {
        const double l = e.data()[i - 1] / dfac.data()[i - 1];
        dfac.data()[i] = (d.data()[i] - sroot) - e.data()[i - 1] * l;
        lldfac.data()[i - 1] = l * l * dfac.data()[i - 1];
    }
    const double pivmin = detail::compute_pivmin<double>(e.data(), n);

    for (double sig : {0.5, 2.0, 4.0, 7.0, 10.0})
    {
        // dlaneg counts eigenvalues of L D L^T below sig; those equal
        // eigenvalues of T below sig + sroot.
        const int twisted = detail::dlaneg<double>(n, dfac.data(), lldfac.data(), sig, pivmin, n);
        const int tri = detail::sturm_negcount<double>(d.data(), e2.data(), n, sig + sroot, pivmin);
        CHECK(twisted == tri);
    }
}

TEST_CASE("dlarrb_refine refines RRR eigenvalues to the true values", "[hesap][eig][mrrr][vectors]")
{
    crd::memory::TlsfAllocator alloc(2U * 1024U * 1024U);
    const int n = 16;
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    crd::containers::Array<double> dfac(n, &alloc);
    crd::containers::Array<double> lldfac(n, &alloc);
    crd::containers::Array<double> wtrue(n, &alloc);   // true eig of T
    crd::containers::Array<double> wrrr(n, &alloc);    // true eig of L D L^T = wtrue - sroot
    crd::containers::Array<double> wapx(n, &alloc);    // perturbed, to be refined
    crd::containers::Array<double> werr(n, &alloc);
    crd::containers::Array<double> ew(n, &alloc);
    crd::containers::Array<double> e2w(n, &alloc);
    crd::containers::Array<int> isp(n, &alloc);
    crd::containers::Array<double> zb(4 * n + 8, &alloc);
    crd::containers::Array<double> q(n + 2, &alloc);
    crd::containers::Array<double> qe(n + 1, &alloc);
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = 2.0 + 0.3 * static_cast<double>(i);
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 0.4;
    }

    detail::tridiag_eigenvalues_dqds<double>(d.data(), e.data(), n, ew.data(), e2w.data(), isp.data(), zb.data(),
                                             q.data(), qe.data(), wtrue.data(),
                                             4.0 * std::numeric_limits<double>::epsilon());

    const double sroot = -2.0;
    dfac.data()[0] = d.data()[0] - sroot;
    for (int i = 1; i < n; ++i)
    {
        const double l = e.data()[i - 1] / dfac.data()[i - 1];
        dfac.data()[i] = (d.data()[i] - sroot) - e.data()[i - 1] * l;
        lldfac.data()[i - 1] = l * l * dfac.data()[i - 1];
    }
    for (int k = 0; k < n; ++k)
    {
        wrrr.data()[k] = wtrue.data()[k] - sroot;
        wapx.data()[k] = wrrr.data()[k] + 0.02 * wrrr.data()[k] * ((k % 2 == 0) ? 1.0 : -1.0);  // perturb
        werr.data()[k] = 0.05 * std::abs(wrrr.data()[k]) + 0.01;
    }

    const double pivmin = detail::compute_pivmin<double>(e.data(), n);
    const double eps = std::numeric_limits<double>::epsilon();
    detail::dlarrb_refine<double>(n, dfac.data(), lldfac.data(), wapx.data(), werr.data(), 1, n, 4.0 * eps, 4.0 * eps,
                                  pivmin, 12.0, n);

    double worst = 0.0;
    for (int k = 0; k < n; ++k)
    {
        worst = std::max(worst, std::abs(wapx.data()[k] - wrrr.data()[k]) / std::max(1.0, std::abs(wrrr.data()[k])));
    }
    CHECK(worst < 1.0e-10);
}

TEST_CASE("dlar1v computes an eigenvector with tiny residual", "[hesap][eig][mrrr][vectors]")
{
    // Toeplitz tridiagonal T (d=2, e=1). Build RRR T - sigma I = L D L^T with
    // sigma below the spectrum (which is (0,4)); the eigenvectors of L D L^T are
    // exactly the eigenvectors of T. Run dlar1v at a closed-form eigenvalue and
    // check the original eigenpair residual ||T z - lambda z||.
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const int n = 16;
    crd::containers::Array<double> dorig(n, &alloc);
    crd::containers::Array<double> eorig(n, &alloc);
    crd::containers::Array<double> dfac(n, &alloc);
    crd::containers::Array<double> lfac(n, &alloc);
    crd::containers::Array<double> ldfac(n, &alloc);
    crd::containers::Array<double> lldfac(n, &alloc);
    crd::containers::Array<double> z(n + 2, &alloc);
    crd::containers::Array<double> work(4 * n + 8, &alloc);
    crd::containers::Array<double> spec(n, &alloc);
    for (int i = 0; i < n; ++i)
    {
        dorig.data()[i] = 2.0;
        z.data()[i] = 0.0;
    }
    z.data()[n] = 0.0;
    z.data()[n + 1] = 0.0;
    for (int i = 0; i < n - 1; ++i)
    {
        eorig.data()[i] = 1.0;
    }

    const double sigma = -1.0;  // below the spectrum -> L D L^T well-defined
    dfac.data()[0] = dorig.data()[0] - sigma;
    for (int i = 1; i < n; ++i)
    {
        lfac.data()[i - 1] = eorig.data()[i - 1] / dfac.data()[i - 1];
        dfac.data()[i] = (dorig.data()[i] - sigma) - eorig.data()[i - 1] * lfac.data()[i - 1];
    }
    for (int i = 0; i < n - 1; ++i)
    {
        ldfac.data()[i] = lfac.data()[i] * dfac.data()[i];
        lldfac.data()[i] = lfac.data()[i] * lfac.data()[i] * dfac.data()[i];
    }

    toeplitz_spectrum<double>(2.0, 1.0, n, spec.data());
    const double lambda = spec.data()[7];  // a middle eigenvalue
    const double pivmin = detail::compute_pivmin<double>(eorig.data(), n);

    const auto out = detail::dlar1v<double>(n, 1, n, lambda - sigma, detail::Z1<double>{dfac.data()},
                                            detail::Z1<double>{lfac.data()}, detail::Z1<double>{ldfac.data()},
                                            detail::Z1<double>{lldfac.data()}, pivmin, 0.0,
                                            detail::Z1<double>{z.data()}, 0, false,
                                            detail::Z1<double>{work.data()});

    // Normalize z (1-based in z.data()[0..n-1]) and check ||T z - lambda z||.
    double resid = 0.0;
    for (int p = 0; p < n; ++p)
    {
        double tz = dorig.data()[p] * (z.data()[p] * out.nrminv);
        if (p > 0)
        {
            tz += eorig.data()[p - 1] * (z.data()[p - 1] * out.nrminv);
        }
        if (p + 1 < n)
        {
            tz += eorig.data()[p] * (z.data()[p + 1] * out.nrminv);
        }
        resid = std::max(resid, std::abs(tz - lambda * (z.data()[p] * out.nrminv)));
    }
    CHECK(resid < 1.0e-12);
    CHECK(out.resid < 1.0e-12);  // dlar1v's own residual estimate
}

TEST_CASE("mrrr_compute_vectors: orthonormal on the glued Wilkinson W21+ (clustered, the hard-gate)",
          "[hesap][eig][mrrr][vectors][cluster]")
{
    // Wilkinson W21+ : diag = [10,9,..,1,0,1,..,10], offdiag = 1. Its largest
    // eigenvalues come in extremely close pairs => the classic MRRR cluster
    // stress test. Vectors must be orthonormal despite the tight clusters.
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    const int m = 10;
    const int n = 2 * m + 1;  // 21
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    crd::containers::Array<double> w(n, &alloc);
    crd::containers::Array<double> ew(n, &alloc);
    crd::containers::Array<double> e2w(n, &alloc);
    crd::containers::Array<int> isp(n, &alloc);
    crd::containers::Array<double> zb(4 * n + 8, &alloc);
    crd::containers::Array<double> q(n + 2, &alloc);
    crd::containers::Array<double> qe(n + 1, &alloc);
    crd::containers::Array<double> v(n * n, &alloc);
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = static_cast<double>(std::abs(i - m));
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 1.0;
    }

    detail::tridiag_eigenvalues_dqds<double>(d.data(), e.data(), n, ew.data(), e2w.data(), isp.data(), zb.data(),
                                             q.data(), qe.data(), w.data(),
                                             4.0 * std::numeric_limits<double>::epsilon());

    detail::mrrr_compute_vectors<double>(&alloc, n, d.data(), e.data(), w.data(), v.data(), n);

    double orth = 0.0;
    for (int a = 0; a < n; ++a)
    {
        for (int b = 0; b < n; ++b)
        {
            double dot = 0.0;
            for (int r = 0; r < n; ++r)
            {
                dot += v.data()[r * n + a] * v.data()[r * n + b];
            }
            orth = std::max(orth, std::abs(dot - (a == b ? 1.0 : 0.0)));
        }
    }
    INFO("||V^T V - I|| = " << orth);
    CHECK(orth < 1.0e-8);  // orthonormal despite tight clusters

    double resid = 0.0;
    for (int k = 0; k < n; ++k)
    {
        for (int p = 0; p < n; ++p)
        {
            double tv = d.data()[p] * v.data()[p * n + k];
            if (p > 0)
            {
                tv += e.data()[p - 1] * v.data()[(p - 1) * n + k];
            }
            if (p + 1 < n)
            {
                tv += e.data()[p] * v.data()[(p + 1) * n + k];
            }
            resid = std::max(resid, std::abs(tv - w.data()[k] * v.data()[p * n + k]));
        }
    }
    INFO("max residual = " << resid);
    CHECK(resid < 1.0e-9);
}

TEST_CASE("mrrr_single_rrr_vectors: orthonormal vectors + tiny residual (well-separated)",
          "[hesap][eig][mrrr][vectors]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);
    const int n = 24;
    crd::containers::Array<double> d(n, &alloc);
    crd::containers::Array<double> e(n, &alloc);
    crd::containers::Array<double> w(n, &alloc);
    crd::containers::Array<double> ework(n, &alloc);
    crd::containers::Array<double> e2work(n, &alloc);
    crd::containers::Array<int> isplit(n, &alloc);
    crd::containers::Array<double> v(n * n, &alloc);  // RowMajor, col k = eigenvector
    // Well-separated spectrum: graded diagonal, small coupling.
    for (int i = 0; i < n; ++i)
    {
        d.data()[i] = 3.0 * static_cast<double>(i);
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e.data()[i] = 0.5;
    }

    {
        crd::containers::Array<double> zb(4 * n + 8, &alloc);
        crd::containers::Array<double> q(n + 2, &alloc);
        crd::containers::Array<double> qe(n + 1, &alloc);
        detail::tridiag_eigenvalues_dqds<double>(d.data(), e.data(), n, ework.data(), e2work.data(),
                                                 isplit.data(), zb.data(), q.data(), qe.data(), w.data(),
                                                 4.0 * std::numeric_limits<double>::epsilon());
    }

    detail::mrrr_single_rrr_vectors<double>(&alloc, n, d.data(), e.data(), w.data(), v.data(), n);

    // ||V^T V - I||_inf.
    double orth = 0.0;
    for (int a = 0; a < n; ++a)
    {
        for (int b = 0; b < n; ++b)
        {
            double dot = 0.0;
            for (int r = 0; r < n; ++r)
            {
                dot += v.data()[r * n + a] * v.data()[r * n + b];
            }
            orth = std::max(orth, std::abs(dot - (a == b ? 1.0 : 0.0)));
        }
    }
    CHECK(orth < 1.0e-10);

    // max_k ||T v_k - lambda_k v_k||.
    double resid = 0.0;
    for (int k = 0; k < n; ++k)
    {
        for (int p = 0; p < n; ++p)
        {
            double tv = d.data()[p] * v.data()[p * n + k];
            if (p > 0)
            {
                tv += e.data()[p - 1] * v.data()[(p - 1) * n + k];
            }
            if (p + 1 < n)
            {
                tv += e.data()[p] * v.data()[(p + 1) * n + k];
            }
            resid = std::max(resid, std::abs(tv - w.data()[k] * v.data()[p * n + k]));
        }
    }
    CHECK(resid < 1.0e-10);
}

TEST_CASE("eig_sym_mrrr: full eigendecomposition matches eig_sym + orthonormal", "[hesap][eig][mrrr][full]")
{
    crd_hesap_dense_tests::hesap_jobs_listener();  // gemm_parallel back-transform needs jobs
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);
    const int n = 48;
    crd::hesap::dense::Symmetric<double> a(&alloc, static_cast<crd::usize>(n));
    crd::u64 s = 0xFEEDFACECAFEBEEFULL;
    auto next = [&s]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & 0xFFFFFFFFFFFFFULL) / static_cast<double>(1ULL << 52);
    };
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j <= i; ++j)
        {
            a.at(static_cast<crd::usize>(i), static_cast<crd::usize>(j)) = 2.0 * next() - 1.0;
        }
    }

    const auto em = crd::hesap::dense::eig_sym_mrrr<double>(&alloc, a);
    const auto ed = eig_sym<double>(&alloc, a);  // D&C oracle

    // Eigenvalues match.
    double verr = 0.0;
    for (int k = 0; k < n; ++k)
    {
        verr = std::max(verr, std::abs(em.values.data()[k] - ed.values.data()[k]) /
                                  std::max(1.0, std::abs(ed.values.data()[k])));
    }
    CHECK(verr < 1.0e-10);

    // Orthonormal + residual ||A v - lambda v||.
    const double* v = em.vectors.data();
    const crd::usize ld = em.vectors.ld();
    double orth = 0.0;
    for (int p = 0; p < n; ++p)
    {
        for (int qd = 0; qd < n; ++qd)
        {
            double dot = 0.0;
            for (int r = 0; r < n; ++r)
            {
                dot += v[r * ld + p] * v[r * ld + qd];
            }
            orth = std::max(orth, std::abs(dot - (p == qd ? 1.0 : 0.0)));
        }
    }
    CHECK(orth < 1.0e-9);

    double resid = 0.0;
    for (int k = 0; k < n; ++k)
    {
        const double lam = em.values.data()[k];
        for (int i = 0; i < n; ++i)
        {
            double av = 0.0;
            for (int jj = 0; jj < n; ++jj)
            {
                av += a.at(static_cast<crd::usize>(i), static_cast<crd::usize>(jj)) * v[jj * ld + k];
            }
            resid = std::max(resid, std::abs(av - lam * v[i * ld + k]));
        }
    }
    CHECK(resid < 1.0e-9);
}

TEST_CASE("eigvals_sym large-N (parallel multisection path) matches eig_sym", "[hesap][eig][mrrr][eigvals]")
{
    // n >= kEigvalsMultisectionThreshold (512) exercises the parallel
    // shared-Sturm multisection path over crd::jobs.
    crd_hesap_dense_tests::hesap_jobs_listener();  // ensure jobs is initialized
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);
    const int n = 700;
    Symmetric<double> a(&alloc, static_cast<crd::usize>(n));
    crd::u64 s = 0x5151BABE12345678ULL;
    auto next = [&s]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & 0xFFFFFFFFFFFFFULL) / static_cast<double>(1ULL << 52);
    };
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j <= i; ++j)
        {
            a.at(static_cast<crd::usize>(i), static_cast<crd::usize>(j)) = 2.0 * next() - 1.0;
        }
    }

    const auto vals = crd::hesap::dense::eigvals_sym<double>(&alloc, a);
    const auto eig = eig_sym<double>(&alloc, a);
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double scale = std::max(1.0, std::abs(eig.values.data()[i]));
        worst = std::max(worst, std::abs(vals.data()[i] - eig.values.data()[i]) / scale);
    }
    CHECK(worst < 1.0e-9);
    for (int i = 1; i < n; ++i)
    {
        CHECK(vals.data()[i] >= vals.data()[i - 1]);
    }
}
