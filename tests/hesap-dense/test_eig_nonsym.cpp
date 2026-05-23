#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/eig_nonsym.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <cmath>

using crd::hesap::dense::balance;
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

TEST_CASE("hessenberg: A = Q·H·Qᵀ reconstruction (f64, n=6)", "[hesap][eig][nonsym][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    check_hessenberg<double>(&alloc, 6, 1e-11);
}

TEST_CASE("hessenberg: A = Q·H·Qᵀ reconstruction (f64, n=32)", "[hesap][eig][nonsym][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    check_hessenberg<double>(&alloc, 32, 1e-10);
}

TEST_CASE("hessenberg: A = Q·H·Qᵀ reconstruction (f64, n=64)", "[hesap][eig][nonsym][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    check_hessenberg<double>(&alloc, 64, 1e-9);
}

TEST_CASE("hessenberg: A = Q·H·Qᵀ reconstruction (f64, n=160 — multi-panel blocked)",
          "[hesap][eig][nonsym][real][blocked]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    check_hessenberg<double>(&alloc, 160, 1e-8);
    check_hessenberg<double>(&alloc, 320, 1e-8);  // 10 panels — many-panel robustness
    check_hessenberg<double>(&alloc, 512, 1e-7);  // 15 panels — large-n robustness
}

TEST_CASE("hessenberg: A = Q·H·Qᵀ reconstruction (f32, n=24)", "[hesap][eig][nonsym][real][f32]")
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

TEST_CASE("real_schur: H = Z·T·Zᵀ recon + Z orthogonal + quasi-triangular",
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

TEST_CASE("eig pipeline: A = (Q·Zs)·T·(Q·Zs)ᵀ via hessenberg + real_schur",
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
            double zt = 0.0;
            for (crd::usize p = 0; p < n; ++p)
                zt += z.at(i, p) * s.t.at(p, j);
            // (zt row i) · Zᵀ col j
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
