#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/detail/secular.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <cmath>

using crd::hesap::dense::eig_sym;
using crd::hesap::dense::Symmetric;
namespace detail = crd::hesap::dense::detail;

namespace
{
// Relative residual of the secular equation 1 + rho*sum z^2/(d-lambda) at
// lambda, scaled by the largest term magnitude. (Near clustered poles the
// raw residual is ill-conditioned — f' is ~1e12 — so a machine-accurate root
// still gives a large raw f; the scaled residual is the meaningful measure.)
double secular_rel_residual(int n, const double* d, const double* z, double rho, double lambda)
{
    double f = 1.0;
    double maxterm = 1.0;
    for (int j = 0; j < n; ++j)
    {
        const double term = rho * z[j] * z[j] / (d[j] - lambda);
        f += term;
        maxterm = std::max(maxterm, std::abs(term));
    }
    return std::abs(f) / maxterm;
}

// Build M = diag(d) + rho*z*z^T as a Symmetric and return its eig_sym values.
void rank1_reference(crd::memory::IAllocator* alloc, int n, const double* d, const double* z,
                     double rho, crd::containers::Array<double>& out)
{
    Symmetric<double> m(alloc, static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j <= i; ++j)
        {
            double v = rho * z[i] * z[j];
            if (i == j)
            {
                v += d[i];
            }
            m.at(static_cast<crd::usize>(i), static_cast<crd::usize>(j)) = v;
        }
    }
    const auto eig = eig_sym<double>(alloc, m);
    out.resize(static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i)
    {
        out[static_cast<crd::usize>(i)] = eig.values.data()[i];
    }
}

void check_case(crd::memory::IAllocator* alloc, int n, const double* d, const double* z, double rho)
{
    crd::containers::Array<double> ref(alloc);
    rank1_reference(alloc, n, d, z, rho, ref);

    crd::containers::Array<double> delta(alloc);
    delta.resize(static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i)
    {
        const double lam = detail::secular_root<double>(i, n, d, z, rho, delta.data());
        // Interlacing: d[i] < lam < d[i+1] (or upper bound for the last root).
        CHECK(lam > d[i]);
        if (i < n - 1)
        {
            CHECK(lam < d[i + 1] + 1e-9);
        }
        // Secular equation satisfied (scaled relative residual).
        CHECK(secular_rel_residual(n, d, z, rho, lam) < 1e-9);
        // Matches the trusted dense eig of diag(d)+rho*z*z^T.
        CHECK(std::abs(lam - ref[static_cast<crd::usize>(i)]) < 1e-9);
        // delta[j] == d[j] - lam.
        for (int j = 0; j < n; ++j)
        {
            CHECK(std::abs(delta[static_cast<crd::usize>(j)] - (d[j] - lam)) < 1e-12);
        }
    }
}
} // namespace

TEST_CASE("secular: uniform poles, equal weights", "[hesap][eig][dc][secular]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const double d[] = {1.0, 2.0, 3.0, 4.0};
    const double z[] = {0.5, 0.5, 0.5, 0.5};
    check_case(&alloc, 4, d, z, 1.0);
}

TEST_CASE("secular: varied weights + larger rho", "[hesap][eig][dc][secular]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const double d[] = {-2.0, 0.5, 1.0, 3.5, 7.0};
    const double z[] = {1.3, -0.7, 2.1, 0.4, -1.1};
    check_case(&alloc, 5, d, z, 2.5);
}

TEST_CASE("secular: tightly clustered poles (anchor/shift stress)",
          "[hesap][eig][dc][secular]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    // Close gaps exercise the shifted-evaluation stability.
    const double d[] = {1.0, 1.0 + 1e-6, 1.0 + 2e-6, 5.0};
    const double z[] = {0.6, 0.6, 0.6, 0.9};
    check_case(&alloc, 4, d, z, 1.0);
}

TEST_CASE("secular: small rho (roots hug lower poles)", "[hesap][eig][dc][secular]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const double d[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
    const double z[] = {0.3, -0.4, 0.5, -0.2, 0.6, 0.1};
    check_case(&alloc, 6, d, z, 0.05);
}

TEST_CASE("secular: n=1 + n=2", "[hesap][eig][dc][secular]")
{
    crd::memory::TlsfAllocator alloc(256U * 1024U);
    const double d1[] = {2.0};
    const double z1[] = {1.5};
    check_case(&alloc, 1, d1, z1, 0.7);
    const double d2[] = {1.0, 4.0};
    const double z2[] = {0.8, 1.2};
    check_case(&alloc, 2, d2, z2, 1.0);
}

namespace
{
// Check rank1_eigensolve(diag(d)+rho*z*z^T): ascending, orthonormal,
// reconstruction A == V diag(lam) V^T, and values match eig_sym.
void check_rank1(crd::memory::IAllocator* alloc, int n, const double* d, const double* z,
                 double rho, double tol)
{
    crd::containers::Array<double> lam(alloc);
    crd::containers::Array<double> v(alloc);
    lam.resize(static_cast<crd::usize>(n));
    v.resize(static_cast<crd::usize>(n) * static_cast<crd::usize>(n));
    crd::hesap::dense::rank1_eigensolve<double>(alloc, static_cast<crd::usize>(n), d, z, rho,
                                                nullptr, lam.data(), v.data());
    const auto cn = static_cast<crd::usize>(n);
    // ascending
    for (crd::usize k = 1; k < cn; ++k)
    {
        CHECK(lam[k - 1] <= lam[k] + 1e-12);
    }
    // orthonormality ||V^T V - I||
    for (crd::usize i = 0; i < cn; ++i)
    {
        for (crd::usize j = 0; j < cn; ++j)
        {
            double dot = 0.0;
            for (crd::usize r = 0; r < cn; ++r)
            {
                dot += v[r * cn + i] * v[r * cn + j];
            }
            CHECK(std::abs(dot - (i == j ? 1.0 : 0.0)) < tol);
        }
    }
    // reconstruction A == V diag(lam) V^T against diag(d)+rho z z^T
    for (crd::usize a = 0; a < cn; ++a)
    {
        for (crd::usize b = 0; b <= a; ++b)
        {
            double recon = 0.0;
            for (crd::usize k = 0; k < cn; ++k)
            {
                recon += v[a * cn + k] * lam[k] * v[b * cn + k];
            }
            const double target = rho * z[a] * z[b] + (a == b ? d[a] : 0.0);
            CHECK(std::abs(recon - target) < tol);
        }
    }
    // values match the trusted dense eig_sym
    crd::containers::Array<double> ref(alloc);
    rank1_reference(alloc, n, d, z, rho, ref);
    for (crd::usize k = 0; k < cn; ++k)
    {
        CHECK(std::abs(lam[k] - ref[k]) < tol);
    }
}
} // namespace

TEST_CASE("rank1: distinct poles (no deflation)", "[hesap][eig][dc][rank1]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    const double d[] = {-2.0, 0.5, 1.0, 3.5, 7.0};
    const double z[] = {1.3, -0.7, 2.1, 0.4, -1.1};
    check_rank1(&alloc, 5, d, z, 2.5, 1e-9);
}

TEST_CASE("rank1: negligible-weight deflation (z has zeros)", "[hesap][eig][dc][rank1]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    const double d[] = {1.0, 2.0, 3.0, 4.0, 5.0};
    const double z[] = {0.9, 0.0, 0.7, 0.0, 0.5};  // two negligible weights
    check_rank1(&alloc, 5, d, z, 1.0, 1e-9);
}

TEST_CASE("rank1: equal-pole deflation (Givens)", "[hesap][eig][dc][rank1]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    const double d[] = {2.0, 2.0, 2.0, 5.0, 9.0};  // triple equal pole
    const double z[] = {0.6, 0.5, 0.4, 0.8, 0.3};
    check_rank1(&alloc, 5, d, z, 1.5, 1e-8);
}

TEST_CASE("rank1: unsorted input + negative rho", "[hesap][eig][dc][rank1]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    const double d[] = {5.0, 1.0, 3.0, -2.0};  // unsorted
    const double z[] = {0.5, 0.5, 0.5, 0.5};
    check_rank1(&alloc, 4, d, z, 1.0, 1e-9);
}

TEST_CASE("rank1: larger N=40 random (exercises gemm back-transform)",
          "[hesap][eig][dc][rank1]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    const int n = 40;
    crd::containers::Array<double> d(&alloc);
    crd::containers::Array<double> z(&alloc);
    d.resize(static_cast<crd::usize>(n));
    z.resize(static_cast<crd::usize>(n));
    crd::u32 s = 555U;
    for (int i = 0; i < n; ++i)
    {
        s = s * 1664525U + 1013904223U;
        d[static_cast<crd::usize>(i)] = static_cast<double>(static_cast<crd::i32>(s >> 8) % 2000) * 0.01;
        s = s * 1664525U + 1013904223U;
        z[static_cast<crd::usize>(i)] =
            static_cast<double>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
    }
    check_rank1(&alloc, n, d.data(), z.data(), 1.7, 1e-7);
}

namespace
{
// Check dc_tridiag_eig(d,e): ascending, orthonormal, and reconstruction
// T == Z diag(lam) Z^T against the tridiagonal T(d,e).
void check_dc_tridiag(crd::memory::IAllocator* alloc, int n, const double* d, const double* e,
                      double tol)
{
    const auto cn = static_cast<crd::usize>(n);
    crd::containers::Array<double> lam(alloc);
    crd::containers::Array<double> z(alloc);
    lam.resize(cn);
    z.resize(cn * cn);
    crd::hesap::dense::dc_tridiag_eig<double>(alloc, cn, d, e, lam.data(), z.data());

    for (crd::usize k = 1; k < cn; ++k)
    {
        CHECK(lam[k - 1] <= lam[k] + 1e-9);
    }
    // orthonormality
    double worst_ortho = 0.0;
    for (crd::usize i = 0; i < cn; ++i)
    {
        for (crd::usize j = 0; j < cn; ++j)
        {
            double dot = 0.0;
            for (crd::usize r = 0; r < cn; ++r)
            {
                dot += z[r * cn + i] * z[r * cn + j];
            }
            worst_ortho = std::max(worst_ortho, std::abs(dot - (i == j ? 1.0 : 0.0)));
        }
    }
    CHECK(worst_ortho < tol);
    // reconstruction: (Z diag(lam) Z^T)[a][b] vs tridiagonal entry.
    double worst_recon = 0.0;
    for (crd::usize a = 0; a < cn; ++a)
    {
        for (crd::usize b = 0; b < cn; ++b)
        {
            double recon = 0.0;
            for (crd::usize k = 0; k < cn; ++k)
            {
                recon += z[a * cn + k] * lam[k] * z[b * cn + k];
            }
            double target = 0.0;
            if (a == b)
            {
                target = d[a];
            }
            else if (a + 1 == b)
            {
                target = e[a];
            }
            else if (b + 1 == a)
            {
                target = e[b];
            }
            worst_recon = std::max(worst_recon, std::abs(recon - target));
        }
    }
    CHECK(worst_recon < tol);
}
} // namespace

TEST_CASE("dc_tridiag: base case n<=32 matches steqr", "[hesap][eig][dc][tridiag]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    const double d[] = {2.0, 2.0, 2.0, 2.0, 2.0};
    const double e[] = {1.0, 1.0, 1.0, 1.0};  // 1-2-1 tridiagonal, known spectrum
    check_dc_tridiag(&alloc, 5, d, e, 1e-10);
}

TEST_CASE("dc_tridiag: recursion N=50/120/300 random (reconstruction + ortho)",
          "[hesap][eig][dc][tridiag]")
{
    for (int n : {50, 120, 300})
    {
        crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
        crd::containers::Array<double> d(&alloc);
        crd::containers::Array<double> e(&alloc);
        d.resize(static_cast<crd::usize>(n));
        e.resize(static_cast<crd::usize>(n - 1));
        crd::u32 s = 31337U + static_cast<crd::u32>(n);
        for (int i = 0; i < n; ++i)
        {
            s = s * 1664525U + 1013904223U;
            d[static_cast<crd::usize>(i)] =
                static_cast<double>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.01;
        }
        for (int i = 0; i < n - 1; ++i)
        {
            s = s * 1664525U + 1013904223U;
            e[static_cast<crd::usize>(i)] =
                static_cast<double>(static_cast<crd::i32>(s >> 8) % 1000 + 1) * 0.01;  // nonzero
        }
        check_dc_tridiag(&alloc, n, d.data(), e.data(), 1e-7);
    }
}

TEST_CASE("dc_tridiag: deterministic (repeat run bit-identical)", "[hesap][eig][dc][tridiag]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const int n = 80;
    crd::containers::Array<double> d(&alloc);
    crd::containers::Array<double> e(&alloc);
    crd::containers::Array<double> l1(&alloc);
    crd::containers::Array<double> z1(&alloc);
    crd::containers::Array<double> l2(&alloc);
    crd::containers::Array<double> z2(&alloc);
    d.resize(static_cast<crd::usize>(n));
    e.resize(static_cast<crd::usize>(n - 1));
    l1.resize(static_cast<crd::usize>(n));
    z1.resize(static_cast<crd::usize>(n) * static_cast<crd::usize>(n));
    l2.resize(static_cast<crd::usize>(n));
    z2.resize(static_cast<crd::usize>(n) * static_cast<crd::usize>(n));
    crd::u32 s = 909U;
    for (int i = 0; i < n; ++i)
    {
        s = s * 1664525U + 1013904223U;
        d[static_cast<crd::usize>(i)] = static_cast<double>(static_cast<crd::i32>(s >> 8) % 1000) * 0.01;
    }
    for (int i = 0; i < n - 1; ++i)
    {
        e[static_cast<crd::usize>(i)] = 0.5;
    }
    crd::hesap::dense::dc_tridiag_eig<double>(&alloc, static_cast<crd::usize>(n), d.data(), e.data(),
                                             l1.data(), z1.data());
    crd::hesap::dense::dc_tridiag_eig<double>(&alloc, static_cast<crd::usize>(n), d.data(), e.data(),
                                             l2.data(), z2.data());
    for (crd::usize i = 0; i < static_cast<crd::usize>(n); ++i)
    {
        CHECK(l1[i] == l2[i]);
    }
    for (crd::usize i = 0; i < static_cast<crd::usize>(n) * static_cast<crd::usize>(n); ++i)
    {
        CHECK(z1[i] == z2[i]);
    }
}

namespace
{
using Cd = crd::hesap::Complex<double>;

// Build a random Hermitian matrix into H (lower triangle, real diagonal).
void build_hermitian(crd::hesap::dense::Hermitian<Cd>& h, crd::u32 seed)
{
    const crd::usize n = h.n();
    crd::u32 s = seed;
    auto nextd = [&]() {
        s = s * 1664525U + 1013904223U;
        return static_cast<double>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
    };
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < i; ++j)
        {
            h.at_lower(i, j) = Cd{nextd(), nextd()};  // off-diagonal complex
        }
        h.at_lower(i, i) = Cd{nextd(), 0.0};  // diagonal real
    }
}

// Hermitian value H(i,j) (at_value returns conj of the stored lower for i<j).
Cd herm_val(const crd::hesap::dense::Hermitian<Cd>& h, crd::usize i, crd::usize j)
{
    return h.at_value(i, j);
}

void check_herm(crd::memory::IAllocator* alloc, crd::usize n, crd::u32 seed, double tol)
{
    crd::hesap::dense::Hermitian<Cd> h(alloc, n);
    build_hermitian(h, seed);
    const auto eig = crd::hesap::dense::eig_herm<Cd>(alloc, h);
    const Cd* v = eig.vectors.data();
    const crd::usize ld = eig.vectors.ld();

    for (crd::usize k = 0; k < n; ++k)
    {
        CHECK(std::isfinite(eig.values.data()[k]));
        if (k > 0)
        {
            CHECK(eig.values.data()[k - 1] <= eig.values.data()[k] + 1e-9);
        }
    }
    // Orthonormality: V^H V = I  (sum_r conj(V[r][i]) V[r][j]).
    double worst_ortho = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            Cd dot{0.0, 0.0};
            for (crd::usize r = 0; r < n; ++r)
            {
                dot += crd::hesap::conj(v[r * ld + i]) * v[r * ld + j];
            }
            const double target = (i == j) ? 1.0 : 0.0;
            worst_ortho = std::max(worst_ortho, crd::hesap::abs(dot - Cd{target, 0.0}));
        }
    }
    CHECK(worst_ortho < tol);
    // Eigenpair residual: ||H v_k - lambda_k v_k||_inf.
    double worst_resid = 0.0;
    for (crd::usize k = 0; k < n; ++k)
    {
        const double lam = eig.values.data()[k];
        for (crd::usize i = 0; i < n; ++i)
        {
            Cd hv{0.0, 0.0};
            for (crd::usize j = 0; j < n; ++j)
            {
                hv += herm_val(h, i, j) * v[j * ld + k];
            }
            worst_resid = std::max(worst_resid, crd::hesap::abs(hv - v[i * ld + k] * lam));
        }
    }
    CHECK(worst_resid < tol);
}
} // namespace

TEST_CASE("eig_herm: random Hermitian N=4/8/64/200 (reconstruct + ortho + residual)",
          "[hesap][eig][dc][herm]")
{
    for (crd::usize n : {crd::usize{4}, crd::usize{8}, crd::usize{64}, crd::usize{200}})
    {
        crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
        check_herm(&alloc, n, 8000U + static_cast<crd::u32>(n), 1e-8);
    }
}

TEST_CASE("eig_herm: N=512 (D&C scale + overflow guard)", "[hesap][eig][dc][herm]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(512U * 1024U * 1024U));
    check_herm(&alloc, 512, 24601U, 1e-7);
}

TEST_CASE("eig_herm: f32 N=48", "[hesap][eig][dc][herm]")
{
    using Cf = crd::hesap::Complex<float>;
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    const crd::usize n = 48;
    crd::hesap::dense::Hermitian<Cf> h(&alloc, n);
    crd::u32 s = 4242U;
    auto nextf = [&]() {
        s = s * 1664525U + 1013904223U;
        return static_cast<float>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001F;
    };
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < i; ++j)
        {
            h.at_lower(i, j) = Cf{nextf(), nextf()};
        }
        h.at_lower(i, i) = Cf{nextf(), 0.0F};
    }
    const auto eig = crd::hesap::dense::eig_herm<Cf>(&alloc, h);
    const Cf* v = eig.vectors.data();
    const crd::usize ld = eig.vectors.ld();
    double worst = 0.0;
    for (crd::usize k = 0; k < n; ++k)
    {
        const float lam = eig.values.data()[k];
        for (crd::usize i = 0; i < n; ++i)
        {
            Cf hv{0.0F, 0.0F};
            for (crd::usize j = 0; j < n; ++j)
            {
                const Cf hij = (i >= j) ? h.at_lower(i, j) : crd::hesap::conj(h.at_lower(j, i));
                hv += hij * v[j * ld + k];
            }
            worst = std::max(worst, static_cast<double>(crd::hesap::abs(hv - v[i * ld + k] * lam)));
        }
    }
    CHECK(worst < 1e-3);
}

TEST_CASE("eig_herm: deterministic (repeat run bit-identical)", "[hesap][eig][dc][herm]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(32U * 1024U * 1024U));
    const crd::usize n = 96;
    crd::hesap::dense::Hermitian<Cd> h(&alloc, n);
    build_hermitian(h, 13579U);
    const auto e1 = crd::hesap::dense::eig_herm<Cd>(&alloc, h);
    const auto e2 = crd::hesap::dense::eig_herm<Cd>(&alloc, h);
    for (crd::usize k = 0; k < n; ++k)
    {
        CHECK(e1.values.data()[k] == e2.values.data()[k]);
    }
    for (crd::usize i = 0; i < n * e1.vectors.ld(); ++i)
    {
        CHECK(e1.vectors.data()[i].re == e2.vectors.data()[i].re);
        CHECK(e1.vectors.data()[i].im == e2.vectors.data()[i].im);
    }
}

TEST_CASE("secular: deterministic (repeat run bit-identical)", "[hesap][eig][dc][secular]")
{
    crd::memory::TlsfAllocator alloc(256U * 1024U);
    const double d[] = {-1.0, 0.0, 2.0, 5.0};
    const double z[] = {1.1, -0.9, 0.7, 1.3};
    crd::containers::Array<double> delta(&alloc);
    delta.resize(4);
    for (int i = 0; i < 4; ++i)
    {
        const double a = detail::secular_root<double>(i, 4, d, z, 1.5, delta.data());
        const double b = detail::secular_root<double>(i, 4, d, z, 1.5, delta.data());
        CHECK(a == b);
    }
}
