#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/eig_nonsym.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <cmath>

using crd::hesap::Complex;
using crd::hesap::dense::balance;
using crd::hesap::dense::complex_schur;
using crd::hesap::dense::ComplexSchur;
using crd::hesap::dense::eig;
using crd::hesap::dense::EigNonsym;
using crd::hesap::dense::form_hessenberg_q;
using crd::hesap::dense::hessenberg;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::aed_deflate;
using crd::hesap::dense::AedResult;
using crd::hesap::dense::real_schur;
using crd::hesap::dense::schur_aed;
using crd::hesap::dense::RealSchur;
using crd::hesap::dense::reorder_schur;
using crd::hesap::dense::reorder_complex_schur;
using crd::hesap::dense::complex_aed_deflate;
using crd::hesap::dense::complex_schur_aed;
using Catch::Matchers::WithinAbs;

namespace
{
template <typename T>
void fill_general(Matrix<T, Layout::RowMajor>& a, T diag_boost)
{
    for (crd::usize i = 0; i < a.rows(); ++i)
    {
        for (crd::usize j = 0; j < a.cols(); ++j)
        {
            a.at(i, j) = static_cast<T>(std::sin(static_cast<double>(i * 7 + j * 3) * 0.17)) +
                         (i == j ? diag_boost : T{0});
        }
    }
}

// Verify A_orig == Q · H · Qᵀ + Q orthogonal + H upper-Hessenberg, for a fresh
// reduction of an n×n general matrix.
template <typename T>
void check_hessenberg(crd::memory::IAllocator* alloc, crd::usize n, double tol)
{
    Matrix<T, Layout::RowMajor> a(alloc, n, n);
    fill_general<T>(a, static_cast<T>(3));
    Matrix<T, Layout::RowMajor> a_orig(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            a_orig.at(i, j) = a.at(i, j);
        }
    }

    crd::containers::Array<T> tau(alloc);
    hessenberg<T>(a, 0, n - 1, tau);

    // H = upper triangle + first subdiagonal of `a`; zero below.
    Matrix<T, Layout::RowMajor> h(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            h.at(i, j) = (j + 1 >= i) ? a.at(i, j) : T{0};
        }
    }
    // Below-first-subdiagonal of H must be (structurally) zero.
    for (crd::usize i = 2; i < n; ++i)
    {
        for (crd::usize j = 0; j + 2 <= i; ++j)
        {
            CHECK(std::abs(static_cast<double>(h.at(i, j))) == 0.0);
        }
    }

    Matrix<T, Layout::RowMajor> q = form_hessenberg_q<T>(alloc, a, 0, n - 1, tau);

    // Q orthogonal: QᵀQ == I.
    double orth = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            double s = 0.0;
            for (crd::usize p = 0; p < n; ++p)
            {
                s += static_cast<double>(q.at(p, i)) * static_cast<double>(q.at(p, j));
            }
            orth = std::max(orth, std::abs(s - (i == j ? 1.0 : 0.0)));
        }
    }
    INFO("orthogonality ||QᵀQ - I||_max");
    REQUIRE(orth < tol);

    // Reconstruction: Q·H·Qᵀ == A_orig.  qh = Q·H, then qh·Qᵀ.
    Matrix<T, Layout::RowMajor> qh(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            double s = 0.0;
            for (crd::usize p = 0; p < n; ++p)
            {
                s += static_cast<double>(q.at(i, p)) * static_cast<double>(h.at(p, j));
            }
            qh.at(i, j) = static_cast<T>(s);
        }
    }
    double recon = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            double s = 0.0;
            for (crd::usize p = 0; p < n; ++p)
            {
                s += static_cast<double>(qh.at(i, p)) * static_cast<double>(q.at(j, p));  // (Qᵀ)[p][j]=Q[j][p]
            }
            recon = std::max(recon, std::abs(s - static_cast<double>(a_orig.at(i, j))));
        }
    }
    INFO("reconstruction ||Q·H·Qᵀ - A||_max");
    REQUIRE(recon < tol);
}
} // namespace

TEST_CASE("hessenberg: A = Q*H*Q^T reconstruction (f64, n=6)", "[hesap][eig][nonsym][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    check_hessenberg<double>(&alloc, 6, 1e-11);
}

TEST_CASE("hessenberg: A = Q*H*Q^T reconstruction (f64, n=32)", "[hesap][eig][nonsym][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    check_hessenberg<double>(&alloc, 32, 1e-10);
}

TEST_CASE("hessenberg: A = Q*H*Q^T reconstruction (f64, n=64)", "[hesap][eig][nonsym][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    check_hessenberg<double>(&alloc, 64, 1e-9);
}

TEST_CASE("hessenberg: A = Q*H*Q^T reconstruction (f64, n=160 - multi-panel blocked)",
          "[hesap][eig][nonsym][real][blocked]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    check_hessenberg<double>(&alloc, 160, 1e-8);
    check_hessenberg<double>(&alloc, 320, 1e-8);  // 10 panels — many-panel robustness
    check_hessenberg<double>(&alloc, 512, 1e-7);  // 15 panels — large-n robustness
}

TEST_CASE("hessenberg: A = Q*H*Q^T reconstruction (f32, n=24)", "[hesap][eig][nonsym][real][f32]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    check_hessenberg<float>(&alloc, 24, 1e-3);
}

namespace
{
// Build a clean upper-Hessenberg H from a random general A (via the reduction),
// then check real_schur: H = Z·T·Zᵀ, Z orthogonal, T quasi-upper-triangular.
template <typename T>
void check_schur(crd::memory::IAllocator* alloc, crd::usize n, double tol)
{
    Matrix<T, Layout::RowMajor> a(alloc, n, n);
    fill_general<T>(a, static_cast<T>(2));
    crd::containers::Array<T> tau(alloc);
    hessenberg<T>(a, 0, n - 1, tau);
    // Clean Hessenberg H (zero the reflector storage below the subdiagonal).
    Matrix<T, Layout::RowMajor> hmat(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            hmat.at(i, j) = (j + 1 >= i) ? a.at(i, j) : T{0};
        }
    }

    RealSchur<T> s = real_schur<T>(alloc, hmat, 0, n - 1, true);
    REQUIRE(s.converged);

    // Z orthogonal.
    double orth = 0.0;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            double acc = 0.0;
            for (crd::usize p = 0; p < n; ++p)
                acc += static_cast<double>(s.z.at(p, i)) * static_cast<double>(s.z.at(p, j));
            orth = std::max(orth, std::abs(acc - (i == j ? 1.0 : 0.0)));
        }
    INFO("Z orthogonality");
    REQUIRE(orth < tol);

    // T quasi-upper-triangular: nothing below the first subdiagonal.
    for (crd::usize i = 2; i < n; ++i)
        for (crd::usize j = 0; j + 2 <= i; ++j)
            CHECK(std::abs(static_cast<double>(s.t.at(i, j))) < tol);

    // Reconstruction H = Z·T·Zᵀ.
    Matrix<T, Layout::RowMajor> zt(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            double acc = 0.0;
            for (crd::usize p = 0; p < n; ++p)
                acc += static_cast<double>(s.z.at(i, p)) * static_cast<double>(s.t.at(p, j));
            zt.at(i, j) = static_cast<T>(acc);
        }
    double recon = 0.0;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            double acc = 0.0;
            for (crd::usize p = 0; p < n; ++p)
                acc += static_cast<double>(zt.at(i, p)) * static_cast<double>(s.z.at(j, p));
            recon = std::max(recon, std::abs(acc - static_cast<double>(hmat.at(i, j))));
        }
    INFO("recon ||Z·T·Zᵀ - H||");
    REQUIRE(recon < tol);

    // Trace invariant: Σ wr == trace(H).
    double trh = 0.0;
    double sumwr = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        trh += static_cast<double>(hmat.at(i, i));
        sumwr += static_cast<double>(s.wr[i]);
    }
    CHECK_THAT(sumwr, WithinAbs(trh, tol * static_cast<double>(n)));
}
} // namespace

TEST_CASE("real_schur: H = Z*T*Z^T recon + Z orthogonal + quasi-triangular",
          "[hesap][eig][nonsym][real][schur]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    check_schur<double>(&alloc, 8, 1e-10);
    check_schur<double>(&alloc, 32, 1e-9);
    check_schur<double>(&alloc, 64, 1e-8);
}

TEST_CASE("real_schur: 2x2 block recovers complex-conjugate eigenvalues",
          "[hesap][eig][nonsym][real][schur]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    // [[0,-1],[1,0]] has eigenvalues ±i.
    Matrix<double, Layout::RowMajor> h(&alloc, 2, 2, {0.0, -1.0, 1.0, 0.0});
    RealSchur<double> s = real_schur<double>(&alloc, h, 0, 1, true);
    REQUIRE(s.converged);
    CHECK_THAT(s.wr[0], WithinAbs(0.0, 1e-12));
    CHECK_THAT(s.wr[1], WithinAbs(0.0, 1e-12));
    CHECK_THAT(std::abs(s.wi[0]), WithinAbs(1.0, 1e-12));
    CHECK_THAT(std::abs(s.wi[1]), WithinAbs(1.0, 1e-12));
    CHECK_THAT(s.wi[0] + s.wi[1], WithinAbs(0.0, 1e-12));  // conjugate pair
}

TEST_CASE("real_schur: known real eigenvalues of a companion-like matrix",
          "[hesap][eig][nonsym][real][schur]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    // Upper-triangular + one subdiagonal: a 3x3 Hessenberg with a real spectrum.
    // [[2,1,1],[1,3,1],[0,1,4]] — symmetric tridiagonal ⇒ all-real eigenvalues.
    Matrix<double, Layout::RowMajor> h(&alloc, 3, 3, {2.0, 1.0, 1.0, 1.0, 3.0, 1.0, 0.0, 1.0, 4.0});
    RealSchur<double> s = real_schur<double>(&alloc, h, 0, 2, true);
    REQUIRE(s.converged);
    for (crd::usize i = 0; i < 3; ++i)
    {
        CHECK_THAT(s.wi[i], WithinAbs(0.0, 1e-10));  // all real
    }
    // Σλ = trace = 9, Σλ² = trace(H²).
    double sum = s.wr[0] + s.wr[1] + s.wr[2];
    CHECK_THAT(sum, WithinAbs(9.0, 1e-9));
}

TEST_CASE("eig pipeline: A = (Q*Zs)*T*(Q*Zs)^T via hessenberg + real_schur",
          "[hesap][eig][nonsym][real][schur]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const crd::usize n = 24;
    Matrix<double, Layout::RowMajor> a(&alloc, n, n);
    fill_general<double>(a, 2.0);
    Matrix<double, Layout::RowMajor> a_orig(&alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            a_orig.at(i, j) = a.at(i, j);

    crd::containers::Array<double> tau(&alloc);
    hessenberg<double>(a, 0, n - 1, tau);
    Matrix<double, Layout::RowMajor> q = form_hessenberg_q<double>(&alloc, a, 0, n - 1, tau);
    Matrix<double, Layout::RowMajor> hmat(&alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            hmat.at(i, j) = (j + 1 >= i) ? a.at(i, j) : 0.0;

    RealSchur<double> s = real_schur<double>(&alloc, hmat, 0, n - 1, true);
    REQUIRE(s.converged);

    // Z = Q · Zs ; check A_orig = Z · T · Zᵀ.
    Matrix<double, Layout::RowMajor> z(&alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            double acc = 0.0;
            for (crd::usize p = 0; p < n; ++p)
                acc += q.at(i, p) * s.z.at(p, j);
            z.at(i, j) = acc;
        }
    double recon = 0.0;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            double acc = 0.0;
            for (crd::usize p = 0; p < n; ++p)
            {
                double ztp = 0.0;
                for (crd::usize q2 = 0; q2 < n; ++q2)
                    ztp += z.at(i, q2) * s.t.at(q2, p);
                acc += ztp * z.at(j, p);
            }
            recon = std::max(recon, std::abs(acc - a_orig.at(i, j)));
        }
    REQUIRE(recon < 1e-8);
}

TEST_CASE("reorder_schur: reordered form is a valid Schur of the SAME matrix",
          "[hesap][eig][nonsym][real][reorder]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const crd::usize n = 12;
    // Build a real Schur form (t0, z0) of a clean Hessenberg H.
    Matrix<double, Layout::RowMajor> a(&alloc, n, n);
    fill_general<double>(a, 2.0);
    crd::containers::Array<double> tau(&alloc);
    hessenberg<double>(a, 0, n - 1, tau);
    Matrix<double, Layout::RowMajor> hmat(&alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            hmat.at(i, j) = (j + 1 >= i) ? a.at(i, j) : 0.0;
    RealSchur<double> s = real_schur<double>(&alloc, hmat, 0, n - 1, true);
    REQUIRE(s.converged);

    // Reorder a few (ifst, ilst) pairs; each must preserve H = Z·T·Zᵀ.
    const crd::usize pairs[][2] = {{5, 1}, {2, 9}, {0, 7}, {10, 3}};
    for (const auto& pr : pairs)
    {
        Matrix<double, Layout::RowMajor> t = s.t.clone();
        Matrix<double, Layout::RowMajor> z = s.z.clone();
        REQUIRE(reorder_schur<double>(t, z, pr[0], pr[1]));

        // Z orthogonal.
        double orth = 0.0;
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
            {
                double acc = 0.0;
                for (crd::usize p = 0; p < n; ++p)
                    acc += z.at(p, i) * z.at(p, j);
                orth = std::max(orth, std::abs(acc - (i == j ? 1.0 : 0.0)));
            }
        CHECK(orth < 1e-10);

        // T quasi-upper-triangular.
        for (crd::usize i = 2; i < n; ++i)
            for (crd::usize j = 0; j + 2 <= i; ++j)
                CHECK(std::abs(t.at(i, j)) < 1e-9);

        // Recon: Z·T·Zᵀ == H (same matrix, eigenvalues permuted).
        double recon = 0.0;
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
            {
                double acc = 0.0;
                for (crd::usize p = 0; p < n; ++p)
                {
                    double ztp = 0.0;
                    for (crd::usize q = 0; q < n; ++q)
                        ztp += z.at(i, q) * t.at(q, p);
                    acc += ztp * z.at(j, p);
                }
                recon = std::max(recon, std::abs(acc - hmat.at(i, j)));
            }
        INFO("reorder " << pr[0] << "->" << pr[1]);
        REQUIRE(recon < 1e-9);
    }
}

TEST_CASE("reorder_schur: moves a real eigenvalue ifst -> ilst",
          "[hesap][eig][nonsym][real][reorder]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    const crd::usize n = 8;
    // Symmetric A ⇒ all-real spectrum ⇒ Schur form is diagonal (all 1×1 blocks),
    // so reordering moves a specific diagonal eigenvalue.
    Matrix<double, Layout::RowMajor> a(&alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            a.at(i, j) = std::sin(static_cast<double>((i < j ? i * n + j : j * n + i)) * 0.3) +
                         (i == j ? 5.0 : 0.0);
    crd::containers::Array<double> tau(&alloc);
    hessenberg<double>(a, 0, n - 1, tau);
    Matrix<double, Layout::RowMajor> hmat(&alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            hmat.at(i, j) = (j + 1 >= i) ? a.at(i, j) : 0.0;
    RealSchur<double> s = real_schur<double>(&alloc, hmat, 0, n - 1, true);
    REQUIRE(s.converged);
    for (crd::usize i = 0; i < n; ++i)
    {
        REQUIRE(std::abs(s.wi[i]) < 1e-10);  // all real
    }

    const double moved = s.t.at(2, 2);  // eigenvalue at position 2
    Matrix<double, Layout::RowMajor> t = s.t.clone();
    Matrix<double, Layout::RowMajor> z = s.z.clone();
    REQUIRE(reorder_schur<double>(t, z, 2, 6));
    CHECK_THAT(t.at(6, 6), WithinAbs(moved, 1e-9));  // now at position 6
}

namespace
{
// Sorted (wr, wi) eigenvalue list for comparison (sort by wr then wi).
template <typename T>
void sorted_eigs(const crd::containers::Array<T>& wr, const crd::containers::Array<T>& wi,
                 crd::usize n, crd::containers::Array<double>& out)
{
    out.resize(2 * n);
    crd::containers::Array<crd::usize> idx(wr.allocator());
    idx.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        idx[i] = i;
    }
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = i + 1; j < n; ++j)
        {
            const bool gt = (wr[idx[i]] > wr[idx[j]]) ||
                            (wr[idx[i]] == wr[idx[j]] && wi[idx[i]] > wi[idx[j]]);
            if (gt)
            {
                const crd::usize t = idx[i];
                idx[i] = idx[j];
                idx[j] = t;
            }
        }
    for (crd::usize i = 0; i < n; ++i)
    {
        out[2 * i] = static_cast<double>(wr[idx[i]]);
        out[2 * i + 1] = static_cast<double>(wi[idx[i]]);
    }
}

// ‖H0 − Z·H·Zᵀ‖_max (similarity check).
template <typename T>
double sim_recon(const Matrix<T, Layout::RowMajor>& h0, const Matrix<T, Layout::RowMajor>& h,
                 const Matrix<T, Layout::RowMajor>& z, crd::usize n)
{
    double e = 0.0;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            double acc = 0.0;
            for (crd::usize p = 0; p < n; ++p)
            {
                double zh = 0.0;
                for (crd::usize q = 0; q < n; ++q)
                    zh += static_cast<double>(z.at(i, q)) * static_cast<double>(h.at(q, p));
                acc += zh * static_cast<double>(z.at(j, p));
            }
            e = std::max(e, std::abs(acc - static_cast<double>(h0.at(i, j))));
        }
    return e;
}

// First column of the shift polynomial p(H)·e1 = ∏_pairs (H² − sum·H + prod·I)·e1,
// where each shift pair {sr[2j]±i·si[2j], sr[2j+1]±i·si[2j+1]} contributes the
// real quadratic factor (sum = sr[2j]+sr[2j+1], prod = sr·sr − si·si). The
// implicit-Q theorem fixes a multishift sweep's Q so that Q·e1 ∝ this vector.
template <typename T>
void poly_first_col(const Matrix<T, Layout::RowMajor>& h0, crd::usize n, const double* sr,
                    const double* si, crd::usize ns, crd::memory::IAllocator* alloc,
                    crd::containers::Array<double>& out)
{
    out.resize(n);
    crd::containers::Array<double> hp(alloc);
    crd::containers::Array<double> hhp(alloc);
    hp.resize(n);
    hhp.resize(n);
    for (crd::usize i = 0; i < n; ++i)
        out[i] = (i == 0) ? 1.0 : 0.0;
    for (crd::usize j = 0; 2 * j + 1 < ns; ++j)
    {
        const double sum = sr[2 * j] + sr[2 * j + 1];
        const double prod = sr[2 * j] * sr[2 * j + 1] - si[2 * j] * si[2 * j + 1];
        for (crd::usize i = 0; i < n; ++i)
        {
            double acc = 0.0;
            for (crd::usize k = 0; k < n; ++k)
                acc += static_cast<double>(h0.at(i, k)) * out[k];
            hp[i] = acc;
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            double acc = 0.0;
            for (crd::usize k = 0; k < n; ++k)
                acc += static_cast<double>(h0.at(i, k)) * hp[k];
            hhp[i] = acc;
        }
        for (crd::usize i = 0; i < n; ++i)
            out[i] = hhp[i] - sum * hp[i] + prod * out[i];
    }
}

// Eigenvalue pair of the 2x2 block of `h0` at rows/cols (r, r+1) → (sr0,si0),
// (sr1,si1): a conjugate pair if the discriminant is negative, else two reals.
template <typename T>
void eig_2x2(const Matrix<T, Layout::RowMajor>& h0, crd::usize r, double& sr0, double& si0, double& sr1,
             double& si1)
{
    const double a = static_cast<double>(h0.at(r, r));
    const double b = static_cast<double>(h0.at(r, r + 1));
    const double c = static_cast<double>(h0.at(r + 1, r));
    const double d = static_cast<double>(h0.at(r + 1, r + 1));
    const double tr = a + d;
    const double disc = tr * tr * 0.25 - (a * d - b * c);
    if (disc < 0.0)
    {
        const double im = std::sqrt(-disc);
        sr0 = tr * 0.5;
        si0 = im;
        sr1 = tr * 0.5;
        si1 = -im;
    }
    else
    {
        const double s = std::sqrt(disc);
        sr0 = tr * 0.5 + s;
        si0 = 0.0;
        sr1 = tr * 0.5 - s;
        si1 = 0.0;
    }
}
} // namespace

TEST_CASE("aed_deflate: decoupled trailing window deflates fully",
          "[hesap][eig][nonsym][real][aed]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(32U * 1024U * 1024U));
    const crd::usize n = 16;
    const crd::usize nw = 6;
    Matrix<double, Layout::RowMajor> h0(&alloc, n, n);
    fill_general<double>(h0, 2.0);
    crd::containers::Array<double> tau(&alloc);
    hessenberg<double>(h0, 0, n - 1, tau);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            if (j + 1 < i) h0.at(i, j) = 0.0;  // clean Hessenberg
    // Decouple the trailing nw window: zero the spike coupling H(kwtop, kwtop-1).
    const crd::usize kwtop = n - nw;
    h0.at(kwtop, kwtop - 1) = 0.0;

    Matrix<double, Layout::RowMajor> h = h0.clone();
    Matrix<double, Layout::RowMajor> z(&alloc, n, n);
    z.set_identity();
    crd::containers::Array<double> wr(&alloc);
    crd::containers::Array<double> wi(&alloc);
    AedResult<double> r = aed_deflate<double>(&alloc, h, 0, n - 1, nw, z, true, 0, n - 1, true, wr, wi);
    CHECK(r.nd == nw);   // a decoupled window deflates entirely
    CHECK(r.ns == 0);
    CHECK(sim_recon<double>(h0, h, z, n) < 1e-9);
}

TEST_CASE("aed_deflate: preserves similarity + spectrum on a general window",
          "[hesap][eig][nonsym][real][aed]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const crd::usize n = 20;
    const crd::usize nw = 8;
    Matrix<double, Layout::RowMajor> h0(&alloc, n, n);
    fill_general<double>(h0, 1.5);
    crd::containers::Array<double> tau(&alloc);
    hessenberg<double>(h0, 0, n - 1, tau);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            if (j + 1 < i) h0.at(i, j) = 0.0;

    // Reference spectrum of H0.
    RealSchur<double> ref = real_schur<double>(&alloc, h0, 0, n - 1, false);
    REQUIRE(ref.converged);
    crd::containers::Array<double> ev_ref(&alloc);
    sorted_eigs<double>(ref.wr, ref.wi, n, ev_ref);

    Matrix<double, Layout::RowMajor> h = h0.clone();
    Matrix<double, Layout::RowMajor> z(&alloc, n, n);
    z.set_identity();
    crd::containers::Array<double> wr(&alloc);
    crd::containers::Array<double> wi(&alloc);
    AedResult<double> r = aed_deflate<double>(&alloc, h, 0, n - 1, nw, z, true, 0, n - 1, true, wr, wi);
    CHECK(r.nd + r.ns == nw);  // every window eigenvalue accounted for

    // Similarity preserved.
    CHECK(sim_recon<double>(h0, h, z, n) < 1e-8);

    // Spectrum preserved: eig(h) == eig(h0).
    RealSchur<double> after = real_schur<double>(&alloc, h, 0, n - 1, false);
    REQUIRE(after.converged);
    crd::containers::Array<double> ev_after(&alloc);
    sorted_eigs<double>(after.wr, after.wi, n, ev_after);
    double maxd = 0.0;
    for (crd::usize i = 0; i < 2 * n; ++i)
        maxd = std::max(maxd, std::abs(ev_after[i] - ev_ref[i]));
    REQUIRE(maxd < 1e-7);
}

TEST_CASE("schur_aed: AED-driven Schur matches real_schur (n > NMIN)",
          "[hesap][eig][nonsym][real][aed][driver]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    for (crd::usize n : {crd::usize{40}, crd::usize{140}, crd::usize{260}})  // 260 > NMIN ⇒ AED engages
    {
        Matrix<double, Layout::RowMajor> a(&alloc, n, n);
        fill_general<double>(a, 1.5);
        crd::containers::Array<double> tau(&alloc);
        hessenberg<double>(a, 0, n - 1, tau);
        Matrix<double, Layout::RowMajor> hmat(&alloc, n, n);
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
                hmat.at(i, j) = (j + 1 >= i) ? a.at(i, j) : 0.0;

        crd::usize sweeps = 0;
        RealSchur<double> s = schur_aed<double>(&alloc, hmat, 0, n - 1, true, &sweeps);
        REQUIRE(s.converged);

        // Z orthogonal + T quasi-triangular + recon H = Z·T·Zᵀ.
        double orth = 0.0;
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
            {
                double acc = 0.0;
                for (crd::usize p = 0; p < n; ++p)
                    acc += s.z.at(p, i) * s.z.at(p, j);
                orth = std::max(orth, std::abs(acc - (i == j ? 1.0 : 0.0)));
            }
        REQUIRE(orth < 1e-9);
        CHECK(sim_recon<double>(hmat, s.t, s.z, n) < 1e-7);
        for (crd::usize i = 2; i < n; ++i)
            for (crd::usize j = 0; j + 2 <= i; ++j)
                CHECK(std::abs(s.t.at(i, j)) < 1e-8);

        // Spectrum matches the pure-dlahqr reference.
        RealSchur<double> ref = real_schur<double>(&alloc, hmat, 0, n - 1, false);
        REQUIRE(ref.converged);
        crd::containers::Array<double> ev_aed(&alloc);
        crd::containers::Array<double> ev_ref(&alloc);
        sorted_eigs<double>(s.wr, s.wi, n, ev_aed);
        sorted_eigs<double>(ref.wr, ref.wi, n, ev_ref);
        double maxd = 0.0;
        for (crd::usize i = 0; i < 2 * n; ++i)
            maxd = std::max(maxd, std::abs(ev_aed[i] - ev_ref[i]));
        INFO("n=" << n << " spectrum diff");
        REQUIRE(maxd < 1e-6);
    }
}

// v3d-1c-4 (multishift train) M1: ONE small-bulge multishift sweep `dlaqr5`
// (ns=2) is a correct Francis double-shift sweep — verified by the implicit-Q
// characterization (which holds regardless of which bulge-chase variant is
// used): the swept matrix is an orthogonal SIMILARITY of H (h0 = Z·Hb·Zᵀ), Z is
// orthonormal, Hb stays upper-Hessenberg, AND Z·e1 is parallel to the shift-
// polynomial first column p = (H−s1·I)(H−s2·I)·e1 (this last is what makes the
// sweep THE double-shift sweep for these shifts, not just some similarity). The
// BLAS-3 lever (accumulate-into-U + gemm slab updates) is what M2/M3 exploit.
TEST_CASE("dlaqr5 (multishift train, ns=2): valid double-shift sweep [M1]",
          "[hesap][eig][nonsym][real][aed][multishift]")
{
    using crd::hesap::dense::detail::multishift_sweep;
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    for (crd::usize n : {crd::usize{12}, crd::usize{60}, crd::usize{200}})
    {
        Matrix<double, Layout::RowMajor> a(&alloc, n, n);
        fill_general<double>(a, 1.5);
        crd::containers::Array<double> tau(&alloc);
        hessenberg<double>(a, 0, n - 1, tau);
        Matrix<double, Layout::RowMajor> h0(&alloc, n, n);
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
                h0.at(i, j) = (j + 1 >= i) ? a.at(i, j) : 0.0;

        // Wilkinson shift pair = eigenvalues of the trailing 2x2 block.
        const double aa = h0.at(n - 2, n - 2);
        const double bb = h0.at(n - 2, n - 1);
        const double cc = h0.at(n - 1, n - 2);
        const double dd = h0.at(n - 1, n - 1);
        const double tr = aa + dd;
        const double det = aa * dd - bb * cc;
        const double disc = tr * tr * 0.25 - det;
        double r1r = 0.0;
        double r1i = 0.0;
        double r2r = 0.0;
        double r2i = 0.0;
        if (disc < 0.0)
        {
            const double im = std::sqrt(-disc);
            r1r = tr * 0.5;
            r1i = im;
            r2r = tr * 0.5;
            r2i = -im;
        }
        else
        {
            const double s = std::sqrt(disc);
            r1r = tr * 0.5 + s;
            r2r = tr * 0.5 - s;
        }

        // Train: one dlaqr5 sweep (ns=2, one bulge) accumulating Q into zb.
        Matrix<double, Layout::RowMajor> hb(&alloc, n, n);
        Matrix<double, Layout::RowMajor> zb(&alloc, n, n);
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
                hb.at(i, j) = h0.at(i, j);
        zb.set_identity();
        const double sr[2] = {r1r, r2r};
        const double si[2] = {r1i, r2i};
        multishift_sweep<double>(&alloc, n, 0, n - 1, sr, si, 2, hb.data(), hb.ld(), 0, n - 1, zb.data(),
                                 zb.ld(), true);

        INFO("n=" << n << (disc < 0.0 ? " complex shifts" : " real shifts"));

        // (1) Orthogonal similarity: h0 = zb · hb · zbᵀ.
        CHECK(sim_recon<double>(h0, hb, zb, n) < 1e-9);

        // (2) zb orthonormal.
        double orth = 0.0;
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
            {
                double acc = 0.0;
                for (crd::usize p = 0; p < n; ++p)
                    acc += zb.at(p, i) * zb.at(p, j);
                orth = std::max(orth, std::abs(acc - (i == j ? 1.0 : 0.0)));
            }
        CHECK(orth < 1e-9);

        // (3) hb stays upper-Hessenberg (below the first subdiagonal ~0).
        double below = 0.0;
        for (crd::usize i = 2; i < n; ++i)
            for (crd::usize j = 0; j + 2 <= i; ++j)
                below = std::max(below, std::abs(hb.at(i, j)));
        CHECK(below < 1e-8);

        // (4) zb·e1 ∝ shift-polynomial first column p = (H-s1)(H-s2)·e1 (real
        // form): this is what makes it THE double-shift sweep for these shifts.
        const double sum = r1r + r2r;
        const double prod = r1r * r2r - r1i * r2i;
        const double h00 = h0.at(0, 0);
        const double h10 = h0.at(1, 0);
        const double h01 = h0.at(0, 1);
        const double h11 = h0.at(1, 1);
        const double h21 = (n > 2) ? h0.at(2, 1) : 0.0;
        crd::containers::Array<double> p(&alloc);
        p.resize(n);
        for (crd::usize i = 0; i < n; ++i)
            p[i] = 0.0;
        p[0] = h00 * h00 + h01 * h10 - sum * h00 + prod;
        p[1] = h10 * (h00 + h11) - sum * h10;
        if (n > 2)
            p[2] = h10 * h21;
        double pp = 0.0;
        double zp = 0.0;
        double zz = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            pp += p[i] * p[i];
            zp += zb.at(i, 0) * p[i];
            zz += zb.at(i, 0) * zb.at(i, 0);
        }
        const double alpha = zp / pp;  // best-fit scale (handles the reflector sign)
        double par = 0.0;
        for (crd::usize i = 0; i < n; ++i)
            par += (zb.at(i, 0) - alpha * p[i]) * (zb.at(i, 0) - alpha * p[i]);
        INFO("first-col parallel residual=" << std::sqrt(par / zz));
        CHECK(std::sqrt(par / zz) < 1e-9);
    }
}

// v3d-1c-4 (multishift train) M1 — sub-block sweep (ktop>0, kbot<n-1): exercises
// the KACC22=1 far-update bookkeeping when the active block is NOT the whole
// matrix (the case M3/AED uses). The in-slab left update must stop at MIN(NDCOL,
// KBOT) and let the gemm own the columns beyond — otherwise the reflector is
// applied twice and the global similarity h0 = zb·hb·zbᵀ breaks.
TEST_CASE("dlaqr5 (multishift train, ns=2): sub-block sweep preserves similarity [M1]",
          "[hesap][eig][nonsym][real][aed][multishift]")
{
    using crd::hesap::dense::detail::multishift_sweep;
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    const crd::usize n = 40;
    const crd::usize ktop = 3;
    const crd::usize kbot = 35;
    Matrix<double, Layout::RowMajor> a(&alloc, n, n);
    fill_general<double>(a, 1.5);
    crd::containers::Array<double> tau(&alloc);
    hessenberg<double>(a, 0, n - 1, tau);
    Matrix<double, Layout::RowMajor> h0(&alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            h0.at(i, j) = (j + 1 >= i) ? a.at(i, j) : 0.0;
    // Decouple the block: enforce the dlaqr5 precondition H(ktop,ktop-1)=0,
    // H(kbot+1,kbot)=0.
    h0.at(ktop, ktop - 1) = 0.0;
    h0.at(kbot + 1, kbot) = 0.0;

    // Shifts from the trailing 2x2 of the block.
    const double aa = h0.at(kbot - 1, kbot - 1);
    const double bb = h0.at(kbot - 1, kbot);
    const double cc = h0.at(kbot, kbot - 1);
    const double dd = h0.at(kbot, kbot);
    const double trc = aa + dd;
    const double dsc = trc * trc * 0.25 - (aa * dd - bb * cc);
    double s1r = 0.0;
    double s1i = 0.0;
    double s2r = 0.0;
    double s2i = 0.0;
    if (dsc < 0.0)
    {
        const double im = std::sqrt(-dsc);
        s1r = trc * 0.5;
        s1i = im;
        s2r = trc * 0.5;
        s2i = -im;
    }
    else
    {
        const double sq = std::sqrt(dsc);
        s1r = trc * 0.5 + sq;
        s2r = trc * 0.5 - sq;
    }

    Matrix<double, Layout::RowMajor> hb(&alloc, n, n);
    Matrix<double, Layout::RowMajor> zb(&alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            hb.at(i, j) = h0.at(i, j);
    zb.set_identity();
    const double sr[2] = {s1r, s2r};
    const double si[2] = {s1i, s2i};
    multishift_sweep<double>(&alloc, n, ktop, kbot, sr, si, 2, hb.data(), hb.ld(), 0, n - 1, zb.data(),
                             zb.ld(), true);

    // Global similarity must hold even though the active block is a sub-block.
    CHECK(sim_recon<double>(h0, hb, zb, n) < 1e-9);
    double orth = 0.0;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            double acc = 0.0;
            for (crd::usize p = 0; p < n; ++p)
                acc += zb.at(p, i) * zb.at(p, j);
            orth = std::max(orth, std::abs(acc - (i == j ? 1.0 : 0.0)));
        }
    CHECK(orth < 1e-9);
    double below = 0.0;
    for (crd::usize i = 2; i < n; ++i)
        for (crd::usize j = 0; j + 2 <= i; ++j)
            below = std::max(below, std::abs(hb.at(i, j)));
    CHECK(below < 1e-8);
}

// v3d-1c-4 (multishift train) M2 — chain of nbmps>=2 bulges (ns=4, ns=6). The
// general train introduces nbmps bulges at the top, chases them as a packed
// chain, and the U-accumulation / inter-bulge delayed-update / BMP22-with-chain
// paths only fire here. Gate (implicit-Q, ns shifts): orthogonal similarity +
// orthonormal Z + Hessenberg + Z·e1 ∝ the degree-ns shift polynomial p(H)·e1.
TEST_CASE("dlaqr5 (multishift train): chain of nbmps>=2 bulges [M2]",
          "[hesap][eig][nonsym][real][aed][multishift]")
{
    using crd::hesap::dense::detail::multishift_sweep;
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    for (crd::usize ns : {crd::usize{4}, crd::usize{6}})
    {
        for (crd::usize n : {crd::usize{12}, crd::usize{60}})
        {
            Matrix<double, Layout::RowMajor> a(&alloc, n, n);
            fill_general<double>(a, 1.5);
            crd::containers::Array<double> tau(&alloc);
            hessenberg<double>(a, 0, n - 1, tau);
            Matrix<double, Layout::RowMajor> h0(&alloc, n, n);
            for (crd::usize i = 0; i < n; ++i)
                for (crd::usize j = 0; j < n; ++j)
                    h0.at(i, j) = (j + 1 >= i) ? a.at(i, j) : 0.0;

            // ns shifts = eigenvalue pairs of trailing 2x2 blocks at offsets 0,2,...
            double sr[6] = {0, 0, 0, 0, 0, 0};
            double si[6] = {0, 0, 0, 0, 0, 0};
            const crd::usize nbmps = ns / 2;
            for (crd::usize j = 0; j < nbmps; ++j)
                eig_2x2<double>(h0, n - 2 - 2 * j, sr[2 * j], si[2 * j], sr[2 * j + 1], si[2 * j + 1]);

            Matrix<double, Layout::RowMajor> hb(&alloc, n, n);
            Matrix<double, Layout::RowMajor> zb(&alloc, n, n);
            for (crd::usize i = 0; i < n; ++i)
                for (crd::usize j = 0; j < n; ++j)
                    hb.at(i, j) = h0.at(i, j);
            zb.set_identity();
            multishift_sweep<double>(&alloc, n, 0, n - 1, sr, si, ns, hb.data(), hb.ld(), 0, n - 1,
                                     zb.data(), zb.ld(), true);

            INFO("ns=" << ns << " n=" << n);
            CHECK(sim_recon<double>(h0, hb, zb, n) < 1e-9);

            double orth = 0.0;
            for (crd::usize i = 0; i < n; ++i)
                for (crd::usize j = 0; j < n; ++j)
                {
                    double acc = 0.0;
                    for (crd::usize p = 0; p < n; ++p)
                        acc += zb.at(p, i) * zb.at(p, j);
                    orth = std::max(orth, std::abs(acc - (i == j ? 1.0 : 0.0)));
                }
            CHECK(orth < 1e-9);

            double below = 0.0;
            for (crd::usize i = 2; i < n; ++i)
                for (crd::usize j = 0; j + 2 <= i; ++j)
                    below = std::max(below, std::abs(hb.at(i, j)));
            CHECK(below < 1e-8);

            // Z·e1 ∝ degree-ns shift polynomial first column.
            crd::containers::Array<double> p(&alloc);
            poly_first_col<double>(h0, n, sr, si, ns, &alloc, p);
            double pp = 0.0;
            double zp = 0.0;
            double zz = 0.0;
            for (crd::usize i = 0; i < n; ++i)
            {
                pp += p[i] * p[i];
                zp += zb.at(i, 0) * p[i];
                zz += zb.at(i, 0) * zb.at(i, 0);
            }
            const double alpha = zp / pp;
            double par = 0.0;
            for (crd::usize i = 0; i < n; ++i)
                par += (zb.at(i, 0) - alpha * p[i]) * (zb.at(i, 0) - alpha * p[i]);
            INFO("first-col parallel residual=" << std::sqrt(par / zz));
            CHECK(std::sqrt(par / zz) < 1e-9);
        }
    }
}

// v3d-2a — dlaln2 isolation gate: the 1×1/2×2 solver returns X with
// (ca·op(A) − w·D)·X = scale·B exactly when C is well-conditioned (info==0).
// Covers na∈{1,2} × nw∈{1,2} (real + complex w) × ltrans∈{false,true}.
namespace
{
// max residual ‖(ca·op(A) − w·D)·X − scale·B‖∞ (complex when nw==2).
double laln2_residual(bool ltrans, int na, int nw, double ca, const double* a, double d1, double d2,
                      const double* b, double wr, double wi, const double* x, double scale)
{
    double cr[2][2] = {{0, 0}, {0, 0}};
    double ci[2][2] = {{0, 0}, {0, 0}};
    for (int i = 0; i < na; ++i)
        for (int j = 0; j < na; ++j)
        {
            const double aij = ltrans ? a[j * 2 + i] : a[i * 2 + j];
            const double di = (j == 0) ? d1 : d2;
            cr[i][j] = ca * aij - (i == j ? wr * di : 0.0);
            ci[i][j] = (i == j && nw == 2) ? -wi * di : 0.0;
        }
    double e = 0.0;
    for (int i = 0; i < na; ++i)
    {
        if (nw == 1)
        {
            double r = -scale * b[i * 2 + 0];
            for (int j = 0; j < na; ++j)
                r += cr[i][j] * x[j * 2 + 0];
            e = std::max(e, std::abs(r));
        }
        else
        {
            double rr = -scale * b[i * 2 + 0];
            double ri = -scale * b[i * 2 + 1];
            for (int j = 0; j < na; ++j)
            {
                rr += cr[i][j] * x[j * 2 + 0] - ci[i][j] * x[j * 2 + 1];
                ri += cr[i][j] * x[j * 2 + 1] + ci[i][j] * x[j * 2 + 0];
            }
            e = std::max(e, std::abs(rr) + std::abs(ri));
        }
    }
    return e;
}
} // namespace

TEST_CASE("dlaln2: (ca*op(A) - w*D)*X = scale*B for 1x1/2x2 real+complex [v3d-2a]",
          "[hesap][eig][nonsym][real][dlaln2]")
{
    using crd::hesap::dense::detail::lin_solve_2x2;
    crd::u32 s = 0x1234567U;
    auto rnd = [&]() {
        s = s * 1664525U + 1013904223U;
        return static_cast<double>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
    };
    const double smin = 1e-13;
    for (int trial = 0; trial < 200; ++trial)
    {
        for (int na = 1; na <= 2; ++na)
        {
            for (int nw = 1; nw <= 2; ++nw)
            {
                for (int lt = 0; lt <= 1; ++lt)
                {
                    // Diagonally dominant A + modest w ⇒ C well-conditioned (info==0).
                    double a[4] = {0, 0, 0, 0};
                    double b[4] = {0, 0, 0, 0};
                    a[0] = 4.0 + rnd();
                    if (na == 2)
                    {
                        a[1] = rnd();
                        a[2] = rnd();
                        a[3] = 4.0 + rnd();
                    }
                    b[0] = rnd();
                    b[2] = (na == 2) ? rnd() : 0.0;
                    if (nw == 2)
                    {
                        b[1] = rnd();
                        b[3] = (na == 2) ? rnd() : 0.0;
                    }
                    const double ca = 1.0;
                    const double d1 = 1.0;
                    const double d2 = 1.0;
                    const double wr = 0.3 + rnd() * 0.1;
                    const double wi = (nw == 2) ? (0.5 + rnd() * 0.1) : 0.0;
                    double x[4] = {0, 0, 0, 0};
                    double scale = 1.0;
                    double xnorm = 0.0;
                    int info = 0;
                    lin_solve_2x2<double>(lt != 0, na, nw, smin, ca, a, d1, d2, b, wr, wi, x, scale,
                                          xnorm, info);
                    if (info != 0)
                        continue;  // perturbed coefficient — equation no longer exact
                    const double res =
                        laln2_residual(lt != 0, na, nw, ca, a, d1, d2, b, wr, wi, x, scale);
                    CHECK(res < 1e-11);
                    CHECK(scale > 0.0);
                    CHECK(scale <= 1.0 + 1e-15);
                }
            }
        }
    }
}

// v3d-2a — dtrevc gate: right eigenvectors of a real Schur form T satisfy
// T·vₖ = λₖ·vₖ for every eigenpair (real eigenvalue → real vector; complex
// 2×2 block → conjugate-pair complex vectors assembled from the two packed
// columns). Residual checked in complex arithmetic.
TEST_CASE("dtrevc: T*v = lambda*v for every eigenpair of a real Schur form [v3d-2a]",
          "[hesap][eig][nonsym][real][dtrevc]")
{
    using crd::hesap::dense::detail::schur_right_eigvecs;
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    for (crd::usize n : {crd::usize{8}, crd::usize{20}, crd::usize{50}})
    {
        Matrix<double, Layout::RowMajor> a(&alloc, n, n);
        fill_general<double>(a, 0.5);  // modest diag → generates complex pairs too
        crd::containers::Array<double> tau(&alloc);
        hessenberg<double>(a, 0, n - 1, tau);
        Matrix<double, Layout::RowMajor> h(&alloc, n, n);
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
                h.at(i, j) = (j + 1 >= i) ? a.at(i, j) : 0.0;

        RealSchur<double> sch = real_schur<double>(&alloc, h, 0, n - 1, true);
        REQUIRE(sch.converged);
        const Matrix<double>& t = sch.t;
        Matrix<double> vr = schur_right_eigvecs<double>(&alloc, t);

        bool saw_complex = false;
        double worst = 0.0;
        for (crd::usize k = 0; k < n; ++k)
        {
            // Assemble complex eigenvector v_k (re + i·im) from the packed columns.
            crd::containers::Array<double> vre(&alloc);
            crd::containers::Array<double> vim(&alloc);
            vre.resize(n);
            vim.resize(n);
            const double lr = sch.wr[k];
            const double li = sch.wi[k];
            if (li == 0.0)
            {
                for (crd::usize i = 0; i < n; ++i)
                {
                    vre[i] = vr.at(i, k);
                    vim[i] = 0.0;
                }
            }
            else if (li > 0.0)  // first of pair (columns k, k+1 = re, im)
            {
                saw_complex = true;
                for (crd::usize i = 0; i < n; ++i)
                {
                    vre[i] = vr.at(i, k);
                    vim[i] = vr.at(i, k + 1);
                }
            }
            else  // second of pair: conjugate (columns k-1, k = re, im)
            {
                for (crd::usize i = 0; i < n; ++i)
                {
                    vre[i] = vr.at(i, k - 1);
                    vim[i] = -vr.at(i, k);
                }
            }
            // Residual ‖T·v − λ·v‖∞ (complex), relative to ‖v‖∞.
            double vnorm = 0.0;
            for (crd::usize i = 0; i < n; ++i)
                vnorm = std::max(vnorm, std::abs(vre[i]) + std::abs(vim[i]));
            for (crd::usize i = 0; i < n; ++i)
            {
                double tr = 0.0;
                double ti = 0.0;
                for (crd::usize j = 0; j < n; ++j)
                {
                    tr += t.at(i, j) * vre[j];
                    ti += t.at(i, j) * vim[j];
                }
                const double rr = tr - (lr * vre[i] - li * vim[i]);
                const double ri = ti - (lr * vim[i] + li * vre[i]);
                worst = std::max(worst, (std::abs(rr) + std::abs(ri)) / vnorm);
            }
        }
        INFO("n=" << n << " worst rel residual=" << worst);
        CHECK(worst < 1e-9);
        CHECK(saw_complex);  // the fixtures must exercise the complex path
    }
}

TEST_CASE("balance: isolates corner eigenvalues + preserves trace", "[hesap][eig][nonsym][real][balance]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    constexpr crd::usize kN = 5;
    // Column 0 zero below the diagonal → isolates an eigenvalue at the TOP.
    // Row n-1 zero left of the diagonal → isolates an eigenvalue at the BOTTOM.
    Matrix<double, Layout::RowMajor> a(&alloc, kN, kN);
    fill_general<double>(a, 4.0);
    for (crd::usize i = 1; i < kN; ++i)
    {
        a.at(i, 0) = 0.0;          // column 0 isolates at top
        a.at(kN - 1, i - 1) = 0.0; // row n-1 isolates at bottom (cols 0..n-2)
    }
    double trace_before = 0.0;
    for (crd::usize i = 0; i < kN; ++i)
    {
        trace_before += a.at(i, i);
    }

    crd::containers::Array<double> scale(&alloc);
    crd::usize ilo = 0;
    crd::usize ihi = 0;
    balance<double>(a, scale, ilo, ihi);

    CHECK(ilo == 1);        // top eigenvalue isolated
    CHECK(ihi == kN - 2);   // bottom eigenvalue isolated

    double trace_after = 0.0;
    for (crd::usize i = 0; i < kN; ++i)
    {
        trace_after += a.at(i, i);
    }
    CHECK_THAT(trace_after, WithinAbs(trace_before, 1e-9));  // similarity invariant
}

TEST_CASE("balance: reduces the block 1-norm of an unbalanced matrix",
          "[hesap][eig][nonsym][real][balance]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    constexpr crd::usize kN = 4;
    // Strongly unbalanced: huge row 0 / tiny column 0 (off-diagonal), balanced
    // away by a diagonal similarity.
    Matrix<double, Layout::RowMajor> a(&alloc, kN, kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j < kN; ++j)
        {
            a.at(i, j) = (i == j) ? 2.0 : 1.0;
        }
    }
    a.at(0, 1) = 1e6;  // huge
    a.at(1, 0) = 1e-6;  // tiny — product 1, diagonal scaling balances it
    auto fro = [&]() {
        double s = 0.0;
        for (crd::usize i = 0; i < kN; ++i)
            for (crd::usize j = 0; j < kN; ++j)
                if (i != j) s += a.at(i, j) * a.at(i, j);
        return std::sqrt(s);
    };
    const double before = fro();
    crd::containers::Array<double> scale(&alloc);
    crd::usize ilo = 0;
    crd::usize ihi = 0;
    balance<double>(a, scale, ilo, ihi);
    const double after = fro();
    CHECK(after < before);  // norm reduced
}

TEST_CASE("hessenberg: trivial blocks (n<=2) are no-ops", "[hesap][eig][nonsym][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U));
    Matrix<double, Layout::RowMajor> a(&alloc, 2, 2, {1.0, 2.0, 3.0, 4.0});
    crd::containers::Array<double> tau(&alloc);
    hessenberg<double>(a, 0, 1, tau);
    // 2×2 is already Hessenberg; unchanged.
    CHECK_THAT(a.at(1, 0), WithinAbs(3.0, 1e-15));
    Matrix<double, Layout::RowMajor> q = form_hessenberg_q<double>(&alloc, a, 0, 1, tau);
    CHECK_THAT(q.at(0, 0), WithinAbs(1.0, 1e-15));
    CHECK_THAT(q.at(1, 1), WithinAbs(1.0, 1e-15));
    CHECK_THAT(q.at(0, 1), WithinAbs(0.0, 1e-15));
}

// =======================================================================
// v3d-2b — full non-symmetric eig() pipeline: balance → hessenberg →
// schur_aed → dtrevc → 3-stage back-transform. Gate = per-eigenpair
// residual ‖A·vₖ − λₖ·vₖ‖∞ / ‖vₖ‖∞ in complex arithmetic.
// =======================================================================
namespace
{
// Returns the worst per-eigenpair relative residual; sets `saw_complex` if any
// eigenvalue had a nonzero imaginary part. `a` is the ORIGINAL matrix.
template <typename T>
double eig_worst_residual(const Matrix<T, Layout::RowMajor>& a, const EigNonsym<T>& e,
                          bool& saw_complex)
{
    const crd::usize n = a.rows();
    double worst = 0.0;
    saw_complex = false;
    for (crd::usize k = 0; k < n; ++k)
    {
        const double lr = static_cast<double>(e.values.data()[k].re);
        const double li = static_cast<double>(e.values.data()[k].im);
        if (li != 0.0)
        {
            saw_complex = true;
        }
        double vnorm = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            const double vre = static_cast<double>(e.vectors.at(i, k).re);
            const double vim = static_cast<double>(e.vectors.at(i, k).im);
            vnorm = std::max(vnorm, std::abs(vre) + std::abs(vim));
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            double avre = 0.0;
            double avim = 0.0;
            for (crd::usize j = 0; j < n; ++j)
            {
                const double aij = static_cast<double>(a.at(i, j));
                avre += aij * static_cast<double>(e.vectors.at(j, k).re);
                avim += aij * static_cast<double>(e.vectors.at(j, k).im);
            }
            const double vre = static_cast<double>(e.vectors.at(i, k).re);
            const double vim = static_cast<double>(e.vectors.at(i, k).im);
            const double rr = avre - (lr * vre - li * vim);
            const double ri = avim - (lr * vim + li * vre);
            worst = std::max(worst, (std::abs(rr) + std::abs(ri)) / vnorm);
        }
    }
    return worst;
}

// Each eigenvector column should be Euclidean-norm 1 (D(non-sym)-4).
template <typename T>
double eig_worst_norm_dev(const EigNonsym<T>& e, crd::usize n)
{
    double worst = 0.0;
    for (crd::usize k = 0; k < n; ++k)
    {
        double s = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            const double vre = static_cast<double>(e.vectors.at(i, k).re);
            const double vim = static_cast<double>(e.vectors.at(i, k).im);
            s += vre * vre + vim * vim;
        }
        worst = std::max(worst, std::abs(std::sqrt(s) - 1.0));
    }
    return worst;
}
} // namespace

TEST_CASE("eig: A*v = lambda*v forevery eigenpair of a general matrix (f64)",
          "[hesap][eig][nonsym][real][eig]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    for (crd::usize n : {crd::usize{8}, crd::usize{20}, crd::usize{50}})
    {
        Matrix<double, Layout::RowMajor> a(&alloc, n, n);
        fill_general<double>(a, 0.5);  // modest diag → real + complex eigenpairs
        Matrix<double, Layout::RowMajor> a_orig(&alloc, n, n);
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
                a_orig.at(i, j) = a.at(i, j);

        EigNonsym<double> e = eig<double>(&alloc, a);
        bool saw_complex = false;
        const double worst = eig_worst_residual<double>(a_orig, e, saw_complex);
        INFO("n=" << n << " worst rel residual=" << worst);
        CHECK(worst < 1e-9);
        CHECK(saw_complex);  // these fixtures generate complex pairs
        CHECK(eig_worst_norm_dev<double>(e, n) < 1e-12);

        // trace invariant: Σ Re(λ) == trace(A), Σ Im(λ) == 0.
        double trace = 0.0;
        double sumre = 0.0;
        double sumim = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            trace += a_orig.at(i, i);
            sumre += e.values.data()[i].re;
            sumim += e.values.data()[i].im;
        }
        CHECK_THAT(sumre, WithinAbs(trace, 1e-9 * (1.0 + std::abs(trace))));
        CHECK_THAT(sumim, WithinAbs(0.0, 1e-10));
    }
}

TEST_CASE("eig: exercises dgebak permutation (corner-isolated eigenvalues)",
          "[hesap][eig][nonsym][real][eig][balance]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const crd::usize n = 12;
    Matrix<double, Layout::RowMajor> a(&alloc, n, n);
    fill_general<double>(a, 0.5);
    // Column 0 zero below diagonal → isolates an eigenvalue at the TOP.
    // Row n-1 zero left of diagonal → isolates an eigenvalue at the BOTTOM.
    for (crd::usize i = 1; i < n; ++i)
    {
        a.at(i, 0) = 0.0;
        a.at(n - 1, i - 1) = 0.0;
    }
    Matrix<double, Layout::RowMajor> a_orig(&alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            a_orig.at(i, j) = a.at(i, j);

    EigNonsym<double> e = eig<double>(&alloc, a);
    bool saw_complex = false;
    const double worst = eig_worst_residual<double>(a_orig, e, saw_complex);
    INFO("corner-isolated worst rel residual=" << worst);
    CHECK(worst < 1e-9);
    CHECK(eig_worst_norm_dev<double>(e, n) < 1e-12);
}

TEST_CASE("eig: exercises dgebak scaling (badly-scaled A = D*B*D^-1)",
          "[hesap][eig][nonsym][real][eig][balance]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const crd::usize n = 10;
    // Well-conditioned B, then similarity-scale by a wildly varying diagonal D so
    // `balance` must scale it back. Eigenvalues are invariant under D·B·D^-1, but
    // the dgebak scaling branch is exercised + the residual must stay tight.
    Matrix<double, Layout::RowMajor> b(&alloc, n, n);
    fill_general<double>(b, 0.5);
    crd::containers::Array<double> d(&alloc);
    d.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        d[i] = std::pow(2.0, static_cast<double>(static_cast<crd::isize>(i) - 5) * 3.0);  // 2^-15 .. 2^12
    }
    Matrix<double, Layout::RowMajor> a(&alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            a.at(i, j) = d[i] * b.at(i, j) / d[j];
    Matrix<double, Layout::RowMajor> a_orig(&alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            a_orig.at(i, j) = a.at(i, j);

    EigNonsym<double> e = eig<double>(&alloc, a);
    bool saw_complex = false;
    const double worst = eig_worst_residual<double>(a_orig, e, saw_complex);
    INFO("badly-scaled worst rel residual=" << worst);
    CHECK(worst < 1e-9);
}

TEST_CASE("eig: A*v = lambda*v fora general matrix (f32)", "[hesap][eig][nonsym][real][eig][f32]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(32U * 1024U * 1024U));
    const crd::usize n = 16;
    Matrix<float, Layout::RowMajor> a(&alloc, n, n);
    fill_general<float>(a, 0.5F);
    Matrix<float, Layout::RowMajor> a_orig(&alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            a_orig.at(i, j) = a.at(i, j);

    EigNonsym<float> e = eig<float>(&alloc, a);
    bool saw_complex = false;
    const double worst = eig_worst_residual<float>(a_orig, e, saw_complex);
    INFO("f32 worst rel residual=" << worst);
    CHECK(worst < 1e-3);
    CHECK(eig_worst_norm_dev<float>(e, n) < 1e-5);
}

// =======================================================================
// v3d-2c-1 — complex Hessenberg reduction (zgehd2) + unitary Q (zunghr).
// Gate: A = Q·H·Qᴴ recon, Q unitary, H upper-Hessenberg w/ real subdiagonal.
// =======================================================================
namespace
{
template <typename R>
void check_hessenberg_complex(crd::memory::IAllocator* alloc, crd::usize n, double tol)
{
    using C = crd::hesap::Complex<R>;
    Matrix<C, Layout::RowMajor> a(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            const auto re = static_cast<R>(std::sin(static_cast<double>(i * 7 + j * 3) * 0.17));
            const auto im = static_cast<R>(std::cos(static_cast<double>(i * 5 + j * 11) * 0.13));
            a.at(i, j) = C{re, im};
        }
    }
    Matrix<C, Layout::RowMajor> a_orig(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            a_orig.at(i, j) = a.at(i, j);

    crd::containers::Array<C> tau(alloc);
    hessenberg<C>(a, 0, n - 1, tau);

    // H = upper triangle + first subdiagonal (the strict-lower of `a` holds the
    // stored reflector tails, NOT H — so H is the masked matrix; the recon below
    // is what proves the reduction produced Hessenberg structure). The zgehd2
    // subdiagonal must be real.
    Matrix<C, Layout::RowMajor> h(alloc, n, n);
    double subimag = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            if (j + 1 >= i)
            {
                h.at(i, j) = a.at(i, j);
                if (i >= 1 && j == i - 1)
                    subimag = std::max(subimag, std::abs(static_cast<double>(a.at(i, j).im)));
            }
            else
            {
                h.at(i, j) = C{R{0}, R{0}};
            }
        }
    }

    Matrix<C, Layout::RowMajor> q = form_hessenberg_q<C>(alloc, a, 0, n - 1, tau);

    // Q unitary: ‖Qᴴ·Q − I‖_max.
    double uni = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            double sr = 0.0;
            double si = 0.0;
            for (crd::usize k = 0; k < n; ++k)
            {
                const auto qk_i = q.at(k, i);  // conj(Q[k,i])·Q[k,j]
                const auto qk_j = q.at(k, j);
                sr += static_cast<double>(qk_i.re) * static_cast<double>(qk_j.re) +
                      static_cast<double>(qk_i.im) * static_cast<double>(qk_j.im);
                si += static_cast<double>(qk_i.re) * static_cast<double>(qk_j.im) -
                      static_cast<double>(qk_i.im) * static_cast<double>(qk_j.re);
            }
            const double want = (i == j) ? 1.0 : 0.0;
            uni = std::max(uni, std::abs(sr - want) + std::abs(si));
        }
    }

    // recon: M = Q·H, then R = M·Qᴴ; compare to a_orig.
    Matrix<C, Layout::RowMajor> m(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            C acc{R{0}, R{0}};
            for (crd::usize k = 0; k < n; ++k)
                acc = acc + q.at(i, k) * h.at(k, j);
            m.at(i, j) = acc;
        }
    double recon = 0.0;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            // R[i,j] = Σ_k M[i,k]·conj(Q[j,k])
            double rr = 0.0;
            double ri = 0.0;
            for (crd::usize k = 0; k < n; ++k)
            {
                const auto mk = m.at(i, k);
                const auto qk = q.at(j, k);  // conj
                rr += static_cast<double>(mk.re) * static_cast<double>(qk.re) +
                      static_cast<double>(mk.im) * static_cast<double>(qk.im);
                ri += static_cast<double>(mk.im) * static_cast<double>(qk.re) -
                      static_cast<double>(mk.re) * static_cast<double>(qk.im);
            }
            recon = std::max(recon, std::abs(rr - static_cast<double>(a_orig.at(i, j).re)) +
                                        std::abs(ri - static_cast<double>(a_orig.at(i, j).im)));
        }

    INFO("n=" << n << " recon=" << recon << " unitary=" << uni << " subimag=" << subimag);
    CHECK(recon < tol);
    CHECK(uni < tol);
    CHECK(subimag < tol);
}
} // namespace

TEST_CASE("hessenberg(complex): A = Q*H*Q^H recon (c64, n=6/32/64)",
          "[hesap][eig][nonsym][complex][hessenberg]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    check_hessenberg_complex<double>(&alloc, 6, 1e-12);
    check_hessenberg_complex<double>(&alloc, 32, 1e-12);
    check_hessenberg_complex<double>(&alloc, 64, 1e-12);
}

TEST_CASE("hessenberg(complex): A = Q*H*Q^H recon (c64, n=160 multi-panel)",
          "[hesap][eig][nonsym][complex][hessenberg]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    check_hessenberg_complex<double>(&alloc, 160, 1e-11);
    check_hessenberg_complex<double>(&alloc, 256, 1e-11);
}

TEST_CASE("hessenberg(complex): A = Q*H*Q^H recon (c32, n=24)",
          "[hesap][eig][nonsym][complex][hessenberg][f32]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(32U * 1024U * 1024U));
    check_hessenberg_complex<float>(&alloc, 24, 5e-4);
}

// =======================================================================
// v3d-2c-2 — complex single-shift Schur (zlahqr) + complex balance (zgebal).
// Gate: H = Z·T·Zᴴ recon, Z unitary, T upper-triangular, eigenvalues on diag.
// =======================================================================
namespace
{
template <typename R>
void check_complex_schur(crd::memory::IAllocator* alloc, crd::usize n, double tol)
{
    using C = crd::hesap::Complex<R>;
    // Random complex matrix → complex Hessenberg H (the zlahqr input).
    Matrix<C, Layout::RowMajor> a(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            a.at(i, j) = C{static_cast<R>(std::sin(static_cast<double>(i * 7 + j * 3) * 0.17)),
                           static_cast<R>(std::cos(static_cast<double>(i * 5 + j * 11) * 0.13))};
    crd::containers::Array<C> tau(alloc);
    hessenberg<C>(a, 0, n - 1, tau);
    Matrix<C, Layout::RowMajor> hmat(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            hmat.at(i, j) = (j + 1 >= i) ? a.at(i, j) : C{R{0}, R{0}};

    ComplexSchur<C> sch = complex_schur<C>(alloc, hmat, 0, n - 1, true);
    REQUIRE(sch.converged);
    const Matrix<C>& t = sch.t;
    const Matrix<C>& zz = sch.z;

    // T upper-triangular: strict-lower (incl. subdiagonal) ~0.
    double below = 0.0;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j + 1 <= i; ++j)  // j < i
            below = std::max(below, std::abs(static_cast<double>(t.at(i, j).re)) +
                                        std::abs(static_cast<double>(t.at(i, j).im)));

    // Z unitary: ‖Zᴴ·Z − I‖_max.
    double uni = 0.0;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            double sr = 0.0;
            double si = 0.0;
            for (crd::usize p = 0; p < n; ++p)
            {
                const auto zi = zz.at(p, i);  // conj(Z[p,i])·Z[p,j]
                const auto zj = zz.at(p, j);
                sr += static_cast<double>(zi.re) * static_cast<double>(zj.re) +
                      static_cast<double>(zi.im) * static_cast<double>(zj.im);
                si += static_cast<double>(zi.re) * static_cast<double>(zj.im) -
                      static_cast<double>(zi.im) * static_cast<double>(zj.re);
            }
            uni = std::max(uni, std::abs(sr - (i == j ? 1.0 : 0.0)) + std::abs(si));
        }

    // recon: M = Z·T, R = M·Zᴴ; compare to H.
    Matrix<C, Layout::RowMajor> m(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            C acc{R{0}, R{0}};
            for (crd::usize p = 0; p < n; ++p)
                acc = acc + zz.at(i, p) * t.at(p, j);
            m.at(i, j) = acc;
        }
    double recon = 0.0;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            double rr = 0.0;
            double ri = 0.0;
            for (crd::usize p = 0; p < n; ++p)
            {
                const auto mp = m.at(i, p);
                const auto zp = zz.at(j, p);  // conj
                rr += static_cast<double>(mp.re) * static_cast<double>(zp.re) +
                      static_cast<double>(mp.im) * static_cast<double>(zp.im);
                ri += static_cast<double>(mp.im) * static_cast<double>(zp.re) -
                      static_cast<double>(mp.re) * static_cast<double>(zp.im);
            }
            recon = std::max(recon, std::abs(rr - static_cast<double>(hmat.at(i, j).re)) +
                                        std::abs(ri - static_cast<double>(hmat.at(i, j).im)));
        }

    // Eigenvalues are the diagonal of T.
    double wdiag = 0.0;
    for (crd::usize k = 0; k < n; ++k)
        wdiag = std::max(wdiag, std::abs(static_cast<double>(sch.w.data()[k].re - t.at(k, k).re)) +
                                    std::abs(static_cast<double>(sch.w.data()[k].im - t.at(k, k).im)));

    INFO("n=" << n << " recon=" << recon << " unitary=" << uni << " below=" << below
              << " wdiag=" << wdiag);
    CHECK(recon < tol);
    CHECK(uni < tol);
    CHECK(below < tol);
    CHECK(wdiag < 1e-14);
}
} // namespace

TEST_CASE("complex_schur: H = Z*T*Z^H recon, T upper-triangular (c64, n=8/20/50)",
          "[hesap][eig][nonsym][complex][schur]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    check_complex_schur<double>(&alloc, 8, 1e-9);
    check_complex_schur<double>(&alloc, 20, 1e-9);
    check_complex_schur<double>(&alloc, 50, 1e-8);
}

TEST_CASE("complex_schur: larger + f32 (c64 n=128, c32 n=24)",
          "[hesap][eig][nonsym][complex][schur]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    check_complex_schur<double>(&alloc, 128, 1e-8);
    check_complex_schur<float>(&alloc, 24, 1e-3);
}

// =======================================================================
// v3d-2c-2b-1 — reorder_complex_schur (ztrexc). Gate: the reordered form is a
// valid Schur of the SAME matrix (Z unitary, T upper-triangular, recon), and
// the chosen eigenvalue lands at the target position.
// =======================================================================
namespace
{
// Build an upper-triangular complex Schur form (t, z) of a random complex
// Hessenberg H, so reorder_complex_schur can be exercised on it. Returns H in
// `h_out` for the recon check.
template <typename R>
void make_complex_schur(crd::memory::IAllocator* alloc, crd::usize n,
                        Matrix<crd::hesap::Complex<R>, Layout::RowMajor>& h_out,
                        ComplexSchur<crd::hesap::Complex<R>>& sch_out)
{
    using C = crd::hesap::Complex<R>;
    Matrix<C, Layout::RowMajor> a(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            a.at(i, j) = C{static_cast<R>(std::sin(static_cast<double>(i * 7 + j * 3) * 0.17)),
                           static_cast<R>(std::cos(static_cast<double>(i * 5 + j * 11) * 0.13))};
    crd::containers::Array<C> tau(alloc);
    hessenberg<C>(a, 0, n - 1, tau);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            h_out.at(i, j) = (j + 1 >= i) ? a.at(i, j) : C{R{0}, R{0}};
    sch_out = complex_schur<C>(alloc, h_out, 0, n - 1, true);
    REQUIRE(sch_out.converged);
}

// Z·T·Zᴴ vs H (max |·|₁ entrywise error) + Z-unitarity + strict-lower-of-T.
template <typename R>
void check_reorder_invariants(crd::usize n, const Matrix<crd::hesap::Complex<R>>& t,
                              const Matrix<crd::hesap::Complex<R>>& zz,
                              const Matrix<crd::hesap::Complex<R>, Layout::RowMajor>& h, double tol)
{
    double below = 0.0;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < i; ++j)
            below = std::max(below, std::abs(static_cast<double>(t.at(i, j).re)) +
                                        std::abs(static_cast<double>(t.at(i, j).im)));
    double uni = 0.0;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            double sr = 0.0;
            double si = 0.0;
            for (crd::usize p = 0; p < n; ++p)
            {
                const auto zi = zz.at(p, i);
                const auto zj = zz.at(p, j);
                sr += static_cast<double>(zi.re) * static_cast<double>(zj.re) +
                      static_cast<double>(zi.im) * static_cast<double>(zj.im);
                si += static_cast<double>(zi.re) * static_cast<double>(zj.im) -
                      static_cast<double>(zi.im) * static_cast<double>(zj.re);
            }
            uni = std::max(uni, std::abs(sr - (i == j ? 1.0 : 0.0)) + std::abs(si));
        }
    // recon: (Z·T)·Zᴴ vs H.
    double recon = 0.0;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            double rr = 0.0;
            double ri = 0.0;
            for (crd::usize p = 0; p < n; ++p)  // (Z·T)[i,p]
            {
                double mr = 0.0;
                double mi = 0.0;
                for (crd::usize q = 0; q < n; ++q)
                {
                    const auto zq = zz.at(i, q);
                    const auto tq = t.at(q, p);
                    mr += static_cast<double>(zq.re) * static_cast<double>(tq.re) -
                          static_cast<double>(zq.im) * static_cast<double>(tq.im);
                    mi += static_cast<double>(zq.re) * static_cast<double>(tq.im) +
                          static_cast<double>(zq.im) * static_cast<double>(tq.re);
                }
                const auto zp = zz.at(j, p);  // conj(Z[j,p])
                rr += mr * static_cast<double>(zp.re) + mi * static_cast<double>(zp.im);
                ri += mi * static_cast<double>(zp.re) - mr * static_cast<double>(zp.im);
            }
            recon = std::max(recon, std::abs(rr - static_cast<double>(h.at(i, j).re)) +
                                        std::abs(ri - static_cast<double>(h.at(i, j).im)));
        }
    INFO("recon=" << recon << " unitary=" << uni << " below=" << below);
    CHECK(below < tol);
    CHECK(uni < tol);
    CHECK(recon < tol);
}
} // namespace

TEST_CASE("reorder_complex_schur: reordered form is a valid Schur of the SAME matrix (c64)",
          "[hesap][eig][nonsym][complex][reorder]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const crd::usize n = 12;
    Matrix<Complex<double>, Layout::RowMajor> h(&alloc, n, n);
    ComplexSchur<Complex<double>> sch(&alloc);
    make_complex_schur<double>(&alloc, n, h, sch);

    // Exercise forward + backward moves, including ilst==0 (the usize-underflow
    // edge the cursor loop guards).
    const crd::usize pairs[][2] = {{5, 1}, {2, 9}, {7, 0}, {10, 3}, {0, 11}};
    for (const auto& pr : pairs)
    {
        Matrix<Complex<double>> t = sch.t.clone();
        Matrix<Complex<double>> z = sch.z.clone();
        REQUIRE(reorder_complex_schur<Complex<double>>(t, z, pr[0], pr[1]));
        check_reorder_invariants<double>(n, t, z, h, 1e-9);
    }
}

TEST_CASE("reorder_complex_schur: moves the chosen eigenvalue ifst -> ilst (c64, c32)",
          "[hesap][eig][nonsym][complex][reorder]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    {
        const crd::usize n = 10;
        Matrix<Complex<double>, Layout::RowMajor> h(&alloc, n, n);
        ComplexSchur<Complex<double>> sch(&alloc);
        make_complex_schur<double>(&alloc, n, h, sch);
        // Move up (3->7 is down the diagonal; 7->2 is up).
        const Complex<double> moved_down = sch.t.at(3, 3);
        Matrix<Complex<double>> t1 = sch.t.clone();
        Matrix<Complex<double>> z1 = sch.z.clone();
        REQUIRE(reorder_complex_schur<Complex<double>>(t1, z1, 3, 7));
        CHECK_THAT(t1.at(7, 7).re, WithinAbs(moved_down.re, 1e-9));
        CHECK_THAT(t1.at(7, 7).im, WithinAbs(moved_down.im, 1e-9));

        const Complex<double> moved_up = sch.t.at(7, 7);
        Matrix<Complex<double>> t2 = sch.t.clone();
        Matrix<Complex<double>> z2 = sch.z.clone();
        REQUIRE(reorder_complex_schur<Complex<double>>(t2, z2, 7, 2));
        CHECK_THAT(t2.at(2, 2).re, WithinAbs(moved_up.re, 1e-9));
        CHECK_THAT(t2.at(2, 2).im, WithinAbs(moved_up.im, 1e-9));
    }
    {
        const crd::usize n = 8;
        Matrix<Complex<float>, Layout::RowMajor> h(&alloc, n, n);
        ComplexSchur<Complex<float>> sch(&alloc);
        make_complex_schur<float>(&alloc, n, h, sch);
        const Complex<float> moved = sch.t.at(1, 1);
        Matrix<Complex<float>> t = sch.t.clone();
        Matrix<Complex<float>> z = sch.z.clone();
        REQUIRE(reorder_complex_schur<Complex<float>>(t, z, 1, 6));
        CHECK_THAT(t.at(6, 6).re, WithinAbs(moved.re, 1e-3F));
        CHECK_THAT(t.at(6, 6).im, WithinAbs(moved.im, 1e-3F));
        check_reorder_invariants<float>(n, t, z, h, 1e-3);
    }
}

// =======================================================================
// v3d-2c-2b-2 — complex_aed_deflate (zlaqr2). Gates: nd+ns==nw; a decoupled
// trailing window deflates fully; a general window keeps H unitarily similar
// (z·H·zᴴ == H0 over the WHOLE matrix — catches a coupling-conj error) AND
// the spectrum invariant (catches a similarity sign error the in-window recon
// would miss).
// =======================================================================
namespace
{
// Build a complex upper-Hessenberg H (n×n) from a deterministic random matrix.
template <typename R>
void make_complex_hessenberg(crd::memory::IAllocator* alloc, crd::usize n,
                             Matrix<crd::hesap::Complex<R>, Layout::RowMajor>& h_out)
{
    using C = crd::hesap::Complex<R>;
    Matrix<C, Layout::RowMajor> a(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            a.at(i, j) = C{static_cast<R>(std::sin(static_cast<double>(i * 9 + j * 2) * 0.21)),
                           static_cast<R>(std::cos(static_cast<double>(i * 3 + j * 7) * 0.19))};
    crd::containers::Array<C> tau(alloc);
    hessenberg<C>(a, 0, n - 1, tau);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            h_out.at(i, j) = (j + 1 >= i) ? a.at(i, j) : C{R{0}, R{0}};
}

// Sorted eigenvalues of a complex upper-Hessenberg H (full complex Schur).
template <typename R>
crd::containers::Array<crd::hesap::Complex<R>> sorted_spectrum(
    crd::memory::IAllocator* alloc, const Matrix<crd::hesap::Complex<R>, Layout::RowMajor>& h)
{
    using C = crd::hesap::Complex<R>;
    const crd::usize n = h.rows();
    ComplexSchur<C> sch = complex_schur<C>(alloc, h, 0, n - 1, true);
    crd::containers::Array<C> w(alloc);
    w.resize(n);
    for (crd::usize k = 0; k < n; ++k)
        w[k] = sch.w.data()[k];
    std::sort(w.data(), w.data() + n, [](const C& a, const C& b) {
        if (a.re != b.re)
            return a.re < b.re;
        return a.im < b.im;
    });
    return w;
}

// z·H·zᴴ vs H0 (max |·|₁ entrywise) over the WHOLE n×n.
template <typename R>
double similarity_recon(crd::usize n, const Matrix<crd::hesap::Complex<R>>& z,
                        const Matrix<crd::hesap::Complex<R>, Layout::RowMajor>& h,
                        const Matrix<crd::hesap::Complex<R>, Layout::RowMajor>& h0)
{
    using C = crd::hesap::Complex<R>;
    double worst = 0.0;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            double rr = 0.0;
            double ri = 0.0;
            for (crd::usize p = 0; p < n; ++p)  // (z·H)[i,p]
            {
                double mr = 0.0;
                double mi = 0.0;
                for (crd::usize q = 0; q < n; ++q)
                {
                    const C zq = z.at(i, q);
                    const C hq = h.at(q, p);
                    mr += static_cast<double>(zq.re) * static_cast<double>(hq.re) -
                          static_cast<double>(zq.im) * static_cast<double>(hq.im);
                    mi += static_cast<double>(zq.re) * static_cast<double>(hq.im) +
                          static_cast<double>(zq.im) * static_cast<double>(hq.re);
                }
                const C zp = z.at(j, p);  // conj(z[j,p])
                rr += mr * static_cast<double>(zp.re) + mi * static_cast<double>(zp.im);
                ri += mi * static_cast<double>(zp.re) - mr * static_cast<double>(zp.im);
            }
            worst = std::max(worst, std::abs(rr - static_cast<double>(h0.at(i, j).re)) +
                                        std::abs(ri - static_cast<double>(h0.at(i, j).im)));
        }
    return worst;
}
} // namespace

TEST_CASE("complex_aed_deflate: general window stays unitarily similar + spectrum invariant (c64)",
          "[hesap][eig][nonsym][complex][aed]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    const crd::usize n = 20;
    Matrix<Complex<double>, Layout::RowMajor> h0(&alloc, n, n);
    make_complex_hessenberg<double>(&alloc, n, h0);
    auto spec0 = sorted_spectrum<double>(&alloc, h0);

    const crd::usize nw = 8;  // jw=8, kwtop=12 > ktop=0 ⇒ the coupling column is exercised
    Matrix<Complex<double>, Layout::RowMajor> h = h0.clone();
    Matrix<Complex<double>> z(&alloc, n, n);
    z.set_identity();
    crd::containers::Array<Complex<double>> w(&alloc);
    AedResult<Complex<double>> res =
        complex_aed_deflate<Complex<double>>(&alloc, h, 0, n - 1, nw, z, true, 0, n - 1, true, w);

    CHECK(res.nd + res.ns == nw);
    const double recon = similarity_recon<double>(n, z, h, h0);
    auto spec1 = sorted_spectrum<double>(&alloc, h);
    double spec_err = 0.0;
    for (crd::usize k = 0; k < n; ++k)
        spec_err = std::max(spec_err, std::abs(static_cast<double>(spec1[k].re - spec0[k].re)) +
                                          std::abs(static_cast<double>(spec1[k].im - spec0[k].im)));
    INFO("recon=" << recon << " spec_err=" << spec_err << " nd=" << res.nd << " ns=" << res.ns);
    CHECK(recon < 1e-8);
    CHECK(spec_err < 1e-7);
}

TEST_CASE("complex_aed_deflate: a decoupled trailing window deflates fully (c64)",
          "[hesap][eig][nonsym][complex][aed]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    const crd::usize n = 18;
    const crd::usize nw = 6;  // jw=6, kwtop=12
    Matrix<Complex<double>, Layout::RowMajor> h0(&alloc, n, n);
    make_complex_hessenberg<double>(&alloc, n, h0);
    // Decouple the window: zero the coupling subdiagonal so s == 0 ⇒ every
    // eigenvalue in the window is deflatable.
    const crd::usize kwtop = n - nw;
    h0.at(kwtop, kwtop - 1) = Complex<double>{0.0, 0.0};

    Matrix<Complex<double>, Layout::RowMajor> h = h0.clone();
    Matrix<Complex<double>> z(&alloc, n, n);
    z.set_identity();
    crd::containers::Array<Complex<double>> w(&alloc);
    AedResult<Complex<double>> res =
        complex_aed_deflate<Complex<double>>(&alloc, h, 0, n - 1, nw, z, true, 0, n - 1, true, w);

    INFO("nd=" << res.nd << " ns=" << res.ns);
    CHECK(res.nd == nw);
    CHECK(res.ns == 0);
    CHECK(res.nd + res.ns == nw);
    // Still a valid similarity of the decoupled H0.
    CHECK(similarity_recon<double>(n, z, h, h0) < 1e-8);
}

TEST_CASE("complex_aed_deflate: c32 general window", "[hesap][eig][nonsym][complex][aed][f32]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const crd::usize n = 14;
    const crd::usize nw = 5;
    Matrix<Complex<float>, Layout::RowMajor> h0(&alloc, n, n);
    make_complex_hessenberg<float>(&alloc, n, h0);
    Matrix<Complex<float>, Layout::RowMajor> h = h0.clone();
    Matrix<Complex<float>> z(&alloc, n, n);
    z.set_identity();
    crd::containers::Array<Complex<float>> w(&alloc);
    AedResult<Complex<float>> res =
        complex_aed_deflate<Complex<float>>(&alloc, h, 0, n - 1, nw, z, true, 0, n - 1, true, w);
    CHECK(res.nd + res.ns == nw);
    INFO("recon=" << similarity_recon<float>(n, z, h, h0));
    CHECK(similarity_recon<float>(n, z, h, h0) < 1e-3);
}

// =======================================================================
// v3d-2c-2b-3 — complex_schur_aed (zlaqr0 driver + zlaqr5 multishift sweep).
// Gate: T upper-triangular, Z unitary, H = Z·T·Zᴴ recon, AND the spectrum
// matches single-shift complex_schur (our own baseline — refs AV at n≥256).
// =======================================================================
namespace
{
template <typename R>
void check_complex_schur_aed(crd::memory::IAllocator* alloc, crd::usize n, double recon_tol,
                             double spec_tol)
{
    using C = crd::hesap::Complex<R>;
    Matrix<C, Layout::RowMajor> h(alloc, n, n);
    make_complex_hessenberg<R>(alloc, n, h);

    crd::usize sweeps = 0;
    ComplexSchur<C> sch = complex_schur_aed<C>(alloc, h, 0, n - 1, true, &sweeps);
    REQUIRE(sch.converged);
    const Matrix<C>& t = sch.t;
    const Matrix<C>& zz = sch.z;

    // T upper-triangular.
    double below = 0.0;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < i; ++j)
            below = std::max(below, std::abs(static_cast<double>(t.at(i, j).re)) +
                                        std::abs(static_cast<double>(t.at(i, j).im)));
    // Z unitary.
    double uni = 0.0;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            double sr = 0.0;
            double si = 0.0;
            for (crd::usize p = 0; p < n; ++p)
            {
                const auto zi = zz.at(p, i);
                const auto zj = zz.at(p, j);
                sr += static_cast<double>(zi.re) * static_cast<double>(zj.re) +
                      static_cast<double>(zi.im) * static_cast<double>(zj.im);
                si += static_cast<double>(zi.re) * static_cast<double>(zj.im) -
                      static_cast<double>(zi.im) * static_cast<double>(zj.re);
            }
            uni = std::max(uni, std::abs(sr - (i == j ? 1.0 : 0.0)) + std::abs(si));
        }
    // recon Z·T·Zᴴ == H over the whole matrix.
    const double recon = similarity_recon<R>(n, zz, t, h);

    // Spectrum matches single-shift complex_schur (our own baseline).
    auto spec_ss = sorted_spectrum<R>(alloc, h);
    crd::containers::Array<C> spec_aed(alloc);
    spec_aed.resize(n);
    for (crd::usize k = 0; k < n; ++k)
        spec_aed[k] = sch.w.data()[k];
    std::sort(spec_aed.data(), spec_aed.data() + n, [](const C& a, const C& b) {
        if (a.re != b.re)
            return a.re < b.re;
        return a.im < b.im;
    });
    double spec_err = 0.0;
    for (crd::usize k = 0; k < n; ++k)
        spec_err = std::max(spec_err, std::abs(static_cast<double>(spec_aed[k].re - spec_ss[k].re)) +
                                          std::abs(static_cast<double>(spec_aed[k].im - spec_ss[k].im)));

    INFO("n=" << n << " sweeps=" << sweeps << " below=" << below << " uni=" << uni
              << " recon=" << recon << " spec_err=" << spec_err);
    CHECK(below < recon_tol);
    CHECK(uni < recon_tol);
    CHECK(recon < recon_tol);
    CHECK(spec_err < spec_tol);
}
} // namespace

TEST_CASE("complex_schur_aed: recon + spectrum vs single-shift (c64 n=40 crossover)",
          "[hesap][eig][nonsym][complex][aed][schur]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    check_complex_schur_aed<double>(&alloc, 40, 1e-9, 1e-7);
}

TEST_CASE("complex_schur_aed: AED engages (c64 n=160/260)",
          "[hesap][eig][nonsym][complex][aed][schur]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(512U * 1024U * 1024U));
    check_complex_schur_aed<double>(&alloc, 160, 1e-9, 1e-8);  // > NMIN=150 ⇒ AED engages
    check_complex_schur_aed<double>(&alloc, 260, 1e-9, 1e-8);
}

TEST_CASE("complex_schur_aed: c32", "[hesap][eig][nonsym][complex][aed][schur][f32]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    check_complex_schur_aed<float>(&alloc, 100, 1e-3, 1e-2);
}

namespace
{
// PRNG random complex Hessenberg (the generic case — NOT smooth sin/cos; matches
// the bench matrix that exposed the recon bug).
template <typename R>
void make_complex_hessenberg_prng(crd::memory::IAllocator* alloc, crd::usize n, crd::u32 seed,
                                  Matrix<crd::hesap::Complex<R>, Layout::RowMajor>& h_out)
{
    using C = crd::hesap::Complex<R>;
    Matrix<C, Layout::RowMajor> a(alloc, n, n);
    crd::u32 s = seed + static_cast<crd::u32>(n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            s = s * 1664525U + 1013904223U;
            const R re = static_cast<R>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * static_cast<R>(0.001);
            s = s * 1664525U + 1013904223U;
            const R im = static_cast<R>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * static_cast<R>(0.001);
            a.at(i, j) = C{re, im};
        }
    crd::containers::Array<C> tau(alloc);
    hessenberg<C>(a, 0, n - 1, tau);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            h_out.at(i, j) = (j + 1 >= i) ? a.at(i, j) : C{R{0}, R{0}};
}
} // namespace

TEST_CASE("complex_aed_deflate: REPRO random + kbot<n-1 exercises slab_left_h (c64)",
          "[hesap][eig][nonsym][complex][aed][regression]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    const crd::usize n = 24;
    Matrix<Complex<double>, Layout::RowMajor> h0(&alloc, n, n);
    make_complex_hessenberg_prng<double>(&alloc, n, 4242U, h0);
    const crd::usize nw = 6;
    Matrix<Complex<double>, Layout::RowMajor> h = h0.clone();
    Matrix<Complex<double>> z(&alloc, n, n);
    z.set_identity();
    crd::containers::Array<Complex<double>> w(&alloc);
    // kbot = n-3 (< n-1) ⇒ kbot+1 < n ⇒ the slab_left_h branch runs (never hit by
    // the original 2b-2 fixture which used kbot=n-1).
    AedResult<Complex<double>> res =
        complex_aed_deflate<Complex<double>>(&alloc, h, 0, n - 3, nw, z, true, 0, n - 1, true, w);
    CHECK(res.nd + res.ns == nw);
    CHECK(similarity_recon<double>(n, z, h, h0) < 1e-8);
}

TEST_CASE("complex_aed_deflate: REPRO large window forces partial deflation + spike (c64)",
          "[hesap][eig][nonsym][complex][aed][regression]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    const crd::usize n = 128;
    Matrix<Complex<double>, Layout::RowMajor> h0(&alloc, n, n);
    make_complex_hessenberg_prng<double>(&alloc, n, 88017U, h0);
    const crd::usize nw = 42;  // large window ⇒ partial deflation (ns>1, s!=0) ⇒ spike path
    Matrix<Complex<double>, Layout::RowMajor> h = h0.clone();
    Matrix<Complex<double>> z(&alloc, n, n);
    z.set_identity();
    crd::containers::Array<Complex<double>> w(&alloc);
    AedResult<Complex<double>> res =
        complex_aed_deflate<Complex<double>>(&alloc, h, 0, n - 1, nw, z, true, 0, n - 1, true, w);
    const double recon = similarity_recon<double>(n, z, h, h0);
    INFO("nd=" << res.nd << " ns=" << res.ns << " recon=" << recon);
    CHECK(res.nd + res.ns == nw);
    CHECK(recon < 1e-8);
}

TEST_CASE("complex_schur_aed: REPRO random matrix single call (c64 n=128)",
          "[hesap][eig][nonsym][complex][aed][regression]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    const crd::usize n = 128;
    Matrix<Complex<double>, Layout::RowMajor> h(&alloc, n, n);
    make_complex_hessenberg_prng<double>(&alloc, n, 88017U, h);
    crd::usize sweeps = 0;
    ComplexSchur<Complex<double>> sch = complex_schur_aed<Complex<double>>(&alloc, h, 0, n - 1, true, &sweeps);
    REQUIRE(sch.converged);
    const double recon = similarity_recon<double>(n, sch.z, sch.t, h);
    INFO("n=" << n << " sweeps=" << sweeps << " recon=" << recon);
    CHECK(recon < 1e-8);
}

// ISOLATION: one complex multishift sweep is a unitary similarity for ANY shifts,
// so Z·H'·Zᴴ == H_orig with Z=I. Bisects the sweep from the AED driver.
TEST_CASE("complex_multishift_sweep: implicit-Q similarity (c64)",
          "[hesap][eig][nonsym][complex][aed][regression]")
{
    using C = Complex<double>;
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    const crd::usize n = 80;
    // (ktop, kbot) sweep blocks: full, then partial (interior) blocks as the AED
    // driver produces them (ktop>0 after the first block; kbot<n-1 after a bottom
    // deflation). The far-update's rows-above / cols-right slabs differ per case.
    const crd::usize blocks[][2] = {{0, n - 1}, {0, n - 6}, {6, n - 1}, {6, n - 6}};
    for (const auto& blk : blocks)
    {
        // Large nshifts (nbmps = nshifts/2 bulges) exercises the multi-bulge chain
        // machinery — inter-bulge delayed transforms, bmp22, U-accumulation — that
        // the AED driver hits (ns≈40) but small nshifts does not.
        for (crd::usize nshifts : {crd::usize{2}, crd::usize{4}, crd::usize{20}, crd::usize{40}})
        {
            Matrix<C, Layout::RowMajor> h0(&alloc, n, n);
            make_complex_hessenberg_prng<double>(&alloc, n, 7000U + static_cast<crd::u32>(blk[0] + nshifts), h0);
            // Decouple the block (the sweep's precondition — the AED driver
            // guarantees this post-deflation): zero the couplings at the block
            // boundaries in the reference too, so z·H'·zᴴ == H0 must hold.
            if (blk[0] > 0)
                h0.at(blk[0], blk[0] - 1) = C{0.0, 0.0};
            if (blk[1] + 1 < n)
                h0.at(blk[1] + 1, blk[1]) = C{0.0, 0.0};
            Matrix<C, Layout::RowMajor> h = h0.clone();
            Matrix<C> z(&alloc, n, n);
            z.set_identity();
            crd::containers::Array<C> shifts(&alloc);
            shifts.resize(nshifts);
            for (crd::usize i = 0; i < nshifts; ++i)
                shifts[i] = h0.at(blk[1] - i, blk[1] - i);
            crd::hesap::dense::detail::complex_multishift_sweep<C>(
                &alloc, n, blk[0], blk[1], shifts.data(), nshifts, h.data(), h.ld(), 0, n - 1,
                z.data(), z.ld(), true);
            const double recon = similarity_recon<double>(n, z, h, h0);
            INFO("ktop=" << blk[0] << " kbot=" << blk[1] << " nshifts=" << nshifts
                         << " recon=" << recon);
            CHECK(recon < 1e-9);
        }
    }
}

// =======================================================================
// v3d-2c-3 — public complex eig: ztrevc + back-transform + normalization.
// Gate: per-eigenpair residual ‖A·vₖ − λₖ·vₖ‖∞/‖vₖ‖∞, ‖vₖ‖₂=1, and the
// largest-magnitude component is real-positive (D(non-sym)-4). Random + near-
// defective (duplicated eigenvalue → exercises the ztrevc smin floor).
// =======================================================================
namespace
{
template <typename R>
void check_eig_complex(crd::memory::IAllocator* alloc,
                       const Matrix<crd::hesap::Complex<R>, Layout::RowMajor>& a, double tol)
{
    using C = crd::hesap::Complex<R>;
    const crd::usize n = a.rows();
    EigNonsym<C> e = eig<C>(alloc, a);
    for (crd::usize k = 0; k < n; ++k)
    {
        const C lam = e.values.data()[k];
        // residual ‖A·v − λ·v‖∞ / ‖v‖∞
        double res = 0.0;
        double vinf = 0.0;
        double norm2 = 0.0;
        crd::usize istar = 0;
        double best = -1.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            const C v = e.vectors.at(i, k);
            vinf = std::max(vinf, std::abs(static_cast<double>(v.re)) + std::abs(static_cast<double>(v.im)));
            // Pivot by MODULUS (re²+im²) — the LAPACK/Eigen convention eig uses for
            // D(non-sym)-4, NOT cabs1; the two pick different components.
            const double mag2 = static_cast<double>(v.re) * static_cast<double>(v.re) +
                                static_cast<double>(v.im) * static_cast<double>(v.im);
            norm2 += mag2;
            if (mag2 > best)
            {
                best = mag2;
                istar = i;
            }
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            C av{R{0}, R{0}};
            for (crd::usize j = 0; j < n; ++j)
                av = av + a.at(i, j) * e.vectors.at(j, k);
            const C r = av - lam * e.vectors.at(i, k);
            res = std::max(res, std::abs(static_cast<double>(r.re)) + std::abs(static_cast<double>(r.im)));
        }
        INFO("k=" << k << " res=" << (res / std::max(vinf, 1e-300)) << " norm2=" << std::sqrt(norm2));
        CHECK(res / std::max(vinf, 1e-300) < tol);
        CHECK(std::abs(std::sqrt(norm2) - 1.0) < tol);  // ‖v‖₂ = 1
        // D(non-sym)-4: largest-magnitude component real-positive.
        const C piv = e.vectors.at(istar, k);
        CHECK(static_cast<double>(piv.re) > 0.0);
        CHECK(std::abs(static_cast<double>(piv.im)) < tol);
    }
}
} // namespace

TEST_CASE("eig(complex): residual + norm + phase on random matrices (c64 n=20/60)",
          "[hesap][eig][nonsym][complex][eig]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    for (crd::usize n : {crd::usize{20}, crd::usize{60}})
    {
        Matrix<Complex<double>, Layout::RowMajor> a(&alloc, n, n);
        crd::u32 s = 51001U + static_cast<crd::u32>(n);
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
            {
                s = s * 1664525U + 1013904223U;
                const double re = static_cast<double>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
                s = s * 1664525U + 1013904223U;
                const double im = static_cast<double>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
                a.at(i, j) = Complex<double>{re, im};
            }
        check_eig_complex<double>(&alloc, a, 1e-9);
    }
}

TEST_CASE("eig(complex): near-defective duplicated eigenvalue (smin floor) (c64)",
          "[hesap][eig][nonsym][complex][eig]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const crd::usize n = 10;
    // Upper-triangular A with a duplicated diagonal eigenvalue 2.0 at (0,0),(1,1)
    // plus random strict-upper coupling → ztrevc hits the smin near-defective floor.
    Matrix<Complex<double>, Layout::RowMajor> a(&alloc, n, n);
    crd::u32 s = 909091U;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            if (j < i)
            {
                a.at(i, j) = Complex<double>{0.0, 0.0};
            }
            else if (i == j)
            {
                a.at(i, j) = Complex<double>{(i <= 1) ? 2.0 : 2.0 + static_cast<double>(i), 0.3};
            }
            else
            {
                s = s * 1664525U + 1013904223U;
                const double re = static_cast<double>(static_cast<crd::i32>(s >> 8) % 1000 - 500) * 0.002;
                s = s * 1664525U + 1013904223U;
                const double im = static_cast<double>(static_cast<crd::i32>(s >> 8) % 1000 - 500) * 0.002;
                a.at(i, j) = Complex<double>{re, im};
            }
        }
    // Make A non-triangular via a real Givens SIMILARITY on (n-2, n-1) — preserves
    // the spectrum (incl. the duplicated 2.0 eigenvalue at indices 0,1) but leaves
    // a non-isolatable active block, so `balance` does not fully reduce. The
    // duplicate still drives ztrevc into the smin floor. (Fully-reducible/triangular
    // input itself trips a pre-existing balance ihi-underflow — tracked follow-on
    // `v3d-eig-fully-reducible-input`, affects real eig too.)
    {
        const double cg = 0.6;
        const double sg = 0.8;
        const crd::usize p = n - 2;
        for (crd::usize j = 0; j < n; ++j)  // G·A over rows (p, p+1)
        {
            const Complex<double> r0 = a.at(p, j);
            const Complex<double> r1 = a.at(p + 1, j);
            a.at(p, j) = Complex<double>{cg * r0.re + sg * r1.re, cg * r0.im + sg * r1.im};
            a.at(p + 1, j) = Complex<double>{cg * r1.re - sg * r0.re, cg * r1.im - sg * r0.im};
        }
        for (crd::usize i = 0; i < n; ++i)  // ·Gᴴ over cols (p, p+1)
        {
            const Complex<double> c0 = a.at(i, p);
            const Complex<double> c1 = a.at(i, p + 1);
            a.at(i, p) = Complex<double>{cg * c0.re + sg * c1.re, cg * c0.im + sg * c1.im};
            a.at(i, p + 1) = Complex<double>{cg * c1.re - sg * c0.re, cg * c1.im - sg * c0.im};
        }
    }
    check_eig_complex<double>(&alloc, a, 1e-7);
}

TEST_CASE("eig(complex): c32 random", "[hesap][eig][nonsym][complex][eig][f32]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(32U * 1024U * 1024U));
    const crd::usize n = 16;
    Matrix<Complex<float>, Layout::RowMajor> a(&alloc, n, n);
    crd::u32 s = 31337U;
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
        {
            s = s * 1664525U + 1013904223U;
            const float re = static_cast<float>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001F;
            s = s * 1664525U + 1013904223U;
            const float im = static_cast<float>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001F;
            a.at(i, j) = Complex<float>{re, im};
        }
    check_eig_complex<float>(&alloc, a, 1e-3);
}

TEST_CASE("balance(complex): isolates corner eigenvalues + preserves trace",
          "[hesap][eig][nonsym][complex][balance]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    constexpr crd::usize kN = 6;
    Matrix<Complex<double>, Layout::RowMajor> a(&alloc, kN, kN);
    for (crd::usize i = 0; i < kN; ++i)
        for (crd::usize j = 0; j < kN; ++j)
            a.at(i, j) = Complex<double>{std::sin(0.3 * static_cast<double>(i * 4 + j)) + 4.0,
                                         std::cos(0.2 * static_cast<double>(i + j * 3))};
    // Column 0 zero below diag → top isolation; row n-1 zero left of diag → bottom.
    for (crd::usize i = 1; i < kN; ++i)
    {
        a.at(i, 0) = Complex<double>{0.0, 0.0};
        a.at(kN - 1, i - 1) = Complex<double>{0.0, 0.0};
    }
    Complex<double> trace_before{0.0, 0.0};
    for (crd::usize i = 0; i < kN; ++i)
        trace_before = trace_before + a.at(i, i);

    crd::containers::Array<double> scale(&alloc);
    crd::usize ilo = 0;
    crd::usize ihi = 0;
    balance<Complex<double>>(a, scale, ilo, ihi);

    CHECK(ilo == 1);
    CHECK(ihi == kN - 2);
    Complex<double> trace_after{0.0, 0.0};
    for (crd::usize i = 0; i < kN; ++i)
        trace_after = trace_after + a.at(i, i);
    CHECK_THAT(trace_after.re, WithinAbs(trace_before.re, 1e-9));
    CHECK_THAT(trace_after.im, WithinAbs(trace_before.im, 1e-9));
}
