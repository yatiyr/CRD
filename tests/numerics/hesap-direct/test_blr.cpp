#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/cholesky.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/direct/blr.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

using crd::hesap::dense::Matrix;
using crd::hesap::direct::blr_cholesky_factor;
using crd::hesap::direct::blr_cholesky_factor_lr;
using crd::hesap::direct::blr_cholesky_solve;
using crd::hesap::direct::blr_to_dense_sym;
using crd::hesap::direct::BlrMatrix;
using crd::hesap::direct::compress_blr_sym;
using crd::hesap::direct::detail::low_rank_recompress;
using crd::hesap::direct::factor_front_cholesky_blr;

namespace
{
template <typename T>
T frob(const Matrix<T>& a) noexcept
{
    T s = T{0};
    for (crd::usize i = 0; i < a.size(); ++i)
    {
        s += a.data()[i] * a.data()[i];
    }
    return std::sqrt(s);
}

template <typename T>
T frob_diff(const Matrix<T>& a, const Matrix<T>& b) noexcept
{
    T s = T{0};
    for (crd::usize i = 0; i < a.rows(); ++i)
    {
        for (crd::usize j = 0; j < a.cols(); ++j)
        {
            const T d = a.at(i, j) - b.at(i, j);
            s += d * d;
        }
    }
    return std::sqrt(s);
}

// Symmetric, smooth algebraically-decaying kernel A_ij = 1/(1 + 0.3·|i−j|):
// the classic H-matrix example whose off-diagonal blocks are numerically
// LOW-RANK (exercises the compression path).
template <typename T>
Matrix<T> smooth_kernel(crd::memory::IAllocator* alloc, crd::usize n)
{
    Matrix<T> a(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            const double d = static_cast<double>(i) - static_cast<double>(j);
            const T v = static_cast<T>(1.0 / (1.0 + 0.3 * (d < 0 ? -d : d)));
            a.at(i, j) = v;
            a.at(j, i) = v;
        }
    }
    return a;
}

// Symmetric, oscillatory ⇒ off-diagonal blocks FULL-rank (dense-fallback path).
template <typename T>
Matrix<T> generic_sym(crd::memory::IAllocator* alloc, crd::usize n)
{
    Matrix<T> a(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            const T v = static_cast<T>(std::sin(static_cast<double>(i * 7 + j * 3 + 1) * 0.7));
            a.at(i, j) = v;
            a.at(j, i) = v;
        }
    }
    return a;
}

template <typename T>
bool has_lowrank(const BlrMatrix<T>& b) noexcept
{
    for (crd::usize i = 0; i < b.nb; ++i)
    {
        for (crd::usize j = 0; j < i; ++j)
        {
            if (b.at(i, j).is_lowrank)
            {
                return true;
            }
        }
    }
    return false;
}

// SPD smooth kernel: Gaussian RBF exp(-c·d²) (a positive-definite kernel ⇒ SPD,
// off-diagonal blocks numerically LOW-RANK) + 1·I regularization (well-conditioned).
template <typename T>
Matrix<T> spd_kernel(crd::memory::IAllocator* alloc, crd::usize n)
{
    Matrix<T> a(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            const double d = static_cast<double>(i) - static_cast<double>(j);
            const T v = static_cast<T>(std::exp(-0.0005 * d * d));
            a.at(i, j) = v;
            a.at(j, i) = v;
        }
        a.at(i, i) += static_cast<T>(1.0);  // regularize ⇒ smallest eigenvalue ≥ 1 ⇒ SPD
    }
    return a;
}

// Generic SPD with FULL-rank off-diagonal (A = M·Mᵀ + n·I): L's off-diagonal
// blocks are full-rank ⇒ kept dense ⇒ the BLR factor is exact.
template <typename T>
Matrix<T> spd_full(crd::memory::IAllocator* alloc, crd::usize n)
{
    Matrix<T> m(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            m.at(i, j) = static_cast<T>(std::sin(static_cast<double>(i * 13 + j * 5 + 1) * 0.41));
        }
    }
    Matrix<T> a(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            T s = T{0};
            for (crd::usize k = 0; k < n; ++k)
            {
                s += m.at(i, k) * m.at(j, k);
            }
            a.at(i, j) = s + (i == j ? static_cast<T>(n) : T{0});
        }
    }
    return a;
}

template <typename T>
void matvec(const Matrix<T>& a, const T* x, T* y) noexcept
{
    for (crd::usize i = 0; i < a.rows(); ++i)
    {
        T s = T{0};
        for (crd::usize j = 0; j < a.cols(); ++j)
        {
            s += a.at(i, j) * x[j];
        }
        y[i] = s;
    }
}

// Relative residual ‖A·x − b‖ / ‖b‖ of a solved system.
template <typename T>
T solve_residual(crd::memory::IAllocator* alloc, const Matrix<T>& a, const crd::containers::Array<T>& x,
                 const crd::containers::Array<T>& b)
{
    const crd::usize n = a.rows();
    crd::containers::Array<T> ax(alloc);
    ax.resize(n);
    matvec<T>(a, x.data(), ax.data());
    T rn = T{0};
    T bn = T{0};
    for (crd::usize i = 0; i < n; ++i)
    {
        rn += (ax[i] - b[i]) * (ax[i] - b[i]);
        bn += b[i] * b[i];
    }
    return std::sqrt(rn) / std::sqrt(bn);
}
} // namespace

TEST_CASE("v5e-3a BLR: generic full-rank matrix reconstructs exactly (dense fallback)", "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const Matrix<double> a = generic_sym<double>(&alloc, 100);
    const BlrMatrix<double> b = compress_blr_sym<double>(&alloc, a, 32, 1e-12);
    const Matrix<double> recon = blr_to_dense_sym<double>(&alloc, b);
    CHECK(frob_diff<double>(a, recon) < 1e-10 * frob<double>(a));  // full-rank off-diag ⇒ kept dense ⇒ exact
}

TEST_CASE("v5e-3a BLR: smooth kernel compresses low-rank + reconstructs within tol", "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const Matrix<double> a = smooth_kernel<double>(&alloc, 256);
    const BlrMatrix<double> b = compress_blr_sym<double>(&alloc, a, 32, 1e-8);
    CHECK(has_lowrank<double>(b));  // the low-rank path is genuinely exercised
    const Matrix<double> recon = blr_to_dense_sym<double>(&alloc, b);
    CHECK(frob_diff<double>(a, recon) < 1e-5 * frob<double>(a));  // ≲ tol·‖A‖ (modest accumulation constant)
}

TEST_CASE("v5e-3a BLR: tighter tol => smaller reconstruction error (monotone)", "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const Matrix<double> a = smooth_kernel<double>(&alloc, 256);
    const double e_loose =
        frob_diff<double>(a, blr_to_dense_sym<double>(&alloc, compress_blr_sym<double>(&alloc, a, 32, 1e-4)));
    const double e_tight =
        frob_diff<double>(a, blr_to_dense_sym<double>(&alloc, compress_blr_sym<double>(&alloc, a, 32, 1e-10)));
    CHECK(e_tight <= e_loose);  // tighter tolerance never worsens the reconstruction
}

TEST_CASE("v5e-3a BLR: compression is deterministic (bit-identical re-runs)", "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const Matrix<double> a = smooth_kernel<double>(&alloc, 128);
    const Matrix<double> r1 = blr_to_dense_sym<double>(&alloc, compress_blr_sym<double>(&alloc, a, 32, 1e-8));
    const Matrix<double> r2 = blr_to_dense_sym<double>(&alloc, compress_blr_sym<double>(&alloc, a, 32, 1e-8));
    bool ident = (r1.size() == r2.size());
    for (crd::usize i = 0; ident && i < r1.size(); ++i)
    {
        ident = (r1.data()[i] == r2.data()[i]);
    }
    CHECK(ident);
}

TEST_CASE("v5e-3a BLR: f32 smooth-kernel reconstruct", "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const Matrix<float> a = smooth_kernel<float>(&alloc, 128);
    const BlrMatrix<float> b = compress_blr_sym<float>(&alloc, a, 32, 1e-4F);
    const Matrix<float> recon = blr_to_dense_sym<float>(&alloc, b);
    CHECK(frob_diff<float>(a, recon) < 1e-2F * frob<float>(a));
}

// ---------- v5e-3b: BLR Cholesky factor + solve ----------

TEST_CASE("v5e-3b BLR-Cholesky: SPD smooth kernel -- low-rank L + solve within tol", "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    constexpr crd::usize n = 256;
    const Matrix<double> a = spd_kernel<double>(&alloc, n);
    crd::containers::Array<double> xt(&alloc);
    crd::containers::Array<double> b(&alloc);
    xt.resize(n);
    b.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        xt[i] = std::sin(static_cast<double>(i) * 0.5 + 0.2);
    }
    matvec<double>(a, xt.data(), b.data());  // b = A·x_true

    BlrMatrix<double> l(&alloc);
    REQUIRE(blr_cholesky_factor<double>(&alloc, a, 32, 1e-9, l));
    CHECK(has_lowrank<double>(l));  // L genuinely carries low-rank off-diagonal blocks

    crd::containers::Array<double> x(&alloc);
    x.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    blr_cholesky_solve<double>(l, x.data());
    CHECK(solve_residual<double>(&alloc, a, x, b) < 1e-5);  // A ≈ L·Lᵀ within the BLR tol
}

TEST_CASE("v5e-3b BLR-Cholesky: generic full-rank SPD solves exactly (dense fallback)", "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    constexpr crd::usize n = 96;
    const Matrix<double> a = spd_full<double>(&alloc, n);
    crd::containers::Array<double> xt(&alloc);
    crd::containers::Array<double> b(&alloc);
    xt.resize(n);
    b.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        xt[i] = std::cos(static_cast<double>(i) * 0.3 + 0.1);
    }
    matvec<double>(a, xt.data(), b.data());

    BlrMatrix<double> l(&alloc);
    REQUIRE(blr_cholesky_factor<double>(&alloc, a, 32, 1e-12, l));
    crd::containers::Array<double> x(&alloc);
    x.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    blr_cholesky_solve<double>(l, x.data());
    CHECK(solve_residual<double>(&alloc, a, x, b) < 1e-9);  // full-rank ⇒ dense ⇒ exact
}

TEST_CASE("v5e-3b BLR-Cholesky: non-SPD matrix => factor returns false", "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    Matrix<double> a = spd_kernel<double>(&alloc, 64);
    a.at(0, 0) -= 1.0e3;  // force a strongly non-positive pivot
    BlrMatrix<double> l(&alloc);
    CHECK_FALSE(blr_cholesky_factor<double>(&alloc, a, 32, 1e-9, l));
}

TEST_CASE("v5e-3b BLR-Cholesky: factor + solve deterministic (bit-identical re-runs)", "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    constexpr crd::usize n = 192;
    const Matrix<double> a = spd_kernel<double>(&alloc, n);
    crd::containers::Array<double> b(&alloc);
    b.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        b[i] = std::sin(static_cast<double>(i) * 0.7 + 0.3);
    }
    auto run = [&](crd::containers::Array<double>& out)
    {
        BlrMatrix<double> l(&alloc);
        REQUIRE(blr_cholesky_factor<double>(&alloc, a, 32, 1e-9, l));
        out.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            out[i] = b[i];
        }
        blr_cholesky_solve<double>(l, out.data());
    };
    crd::containers::Array<double> x1(&alloc);
    crd::containers::Array<double> x2(&alloc);
    run(x1);
    run(x2);
    bool ident = true;
    for (crd::usize i = 0; i < n && ident; ++i)
    {
        ident = (x1[i] == x2[i]);
    }
    CHECK(ident);
}

TEST_CASE("v5e-3b BLR-Cholesky: f32 SPD kernel solve", "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    constexpr crd::usize n = 128;
    const Matrix<float> a = spd_kernel<float>(&alloc, n);
    crd::containers::Array<float> xt(&alloc);
    crd::containers::Array<float> b(&alloc);
    xt.resize(n);
    b.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        xt[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.5 + 0.2));
    }
    matvec<float>(a, xt.data(), b.data());
    BlrMatrix<float> l(&alloc);
    REQUIRE(blr_cholesky_factor<float>(&alloc, a, 32, 1e-4F, l));
    crd::containers::Array<float> x(&alloc);
    x.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    blr_cholesky_solve<float>(l, x.data());
    CHECK(solve_residual<float>(&alloc, a, x, b) < 1e-2F);
}

// ---------- v5e-3c: LR×LR update + recompression (the flop-saving factor) ----------

namespace
{
// u·vᵀ expanded to dense (m×n), for recompress validation.
template <typename T>
Matrix<T> lr_dense(crd::memory::IAllocator* alloc, const Matrix<T>& u, const Matrix<T>& v)
{
    const crd::usize m = u.rows();
    const crd::usize n = v.rows();
    const crd::usize r = u.cols();
    Matrix<T> out(alloc, m, n);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            T s = T{0};
            for (crd::usize k = 0; k < r; ++k)
            {
                s += u.at(i, k) * v.at(j, k);
            }
            out.at(i, j) = s;
        }
    }
    return out;
}
} // namespace

TEST_CASE("v5e-3c low_rank_recompress: redundant sum recompresses to true rank, exact", "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    constexpr crd::usize m = 40;
    constexpr crd::usize n = 48;
    constexpr crd::usize rc = 4;
    // uc columns u0,u1,u0,u1 ⇒ the product uc·vcᵀ has true rank ≤ 2.
    Matrix<double> uc(&alloc, m, rc);
    for (crd::usize i = 0; i < m; ++i)
    {
        const double u0 = std::sin(static_cast<double>(i) * 0.3);
        const double u1 = std::cos(static_cast<double>(i) * 0.2);
        uc.at(i, 0) = u0;
        uc.at(i, 1) = u1;
        uc.at(i, 2) = u0;
        uc.at(i, 3) = u1;
    }
    Matrix<double> vc(&alloc, n, rc);
    for (crd::usize j = 0; j < n; ++j)
    {
        for (crd::usize k = 0; k < rc; ++k)
        {
            vc.at(j, k) = std::sin(static_cast<double>(j) * 0.1 * static_cast<double>(k + 1) + 0.5);
        }
    }
    const Matrix<double> d_ref = lr_dense<double>(&alloc, uc, vc);

    Matrix<double> un(&alloc);
    Matrix<double> vn(&alloc);
    crd::usize r = 0;
    low_rank_recompress<double>(&alloc, uc, vc, 1e-12, 0, un, vn, r);
    CHECK(r <= 2);  // the rank-2 redundancy is detected

    const Matrix<double> d_rec = lr_dense<double>(&alloc, un, vn);
    CHECK(frob_diff<double>(d_ref, d_rec) < 1e-9 * frob<double>(d_ref));  // exact reconstruction
}

TEST_CASE("v5e-3c BLR-Cholesky (LR arithmetic): solve within tol + matches the dense oracle",
          "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    constexpr crd::usize n = 256;
    const Matrix<double> a = spd_kernel<double>(&alloc, n);
    crd::containers::Array<double> xt(&alloc);
    crd::containers::Array<double> b(&alloc);
    xt.resize(n);
    b.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        xt[i] = std::sin(static_cast<double>(i) * 0.5 + 0.2);
    }
    matvec<double>(a, xt.data(), b.data());

    // LR-arithmetic factor.
    BlrMatrix<double> llr(&alloc);
    REQUIRE(blr_cholesky_factor_lr<double>(&alloc, a, 32, 1e-10, llr));
    CHECK(has_lowrank<double>(llr));
    crd::containers::Array<double> xlr(&alloc);
    xlr.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        xlr[i] = b[i];
    }
    blr_cholesky_solve<double>(llr, xlr.data());
    CHECK(solve_residual<double>(&alloc, a, xlr, b) < 1e-4);  // LR×LR updates + recompression accurate

    // Dense oracle (v5e-3b): the two solves must agree within the BLR tolerance.
    BlrMatrix<double> ld(&alloc);
    REQUIRE(blr_cholesky_factor<double>(&alloc, a, 32, 1e-10, ld));
    crd::containers::Array<double> xd(&alloc);
    xd.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        xd[i] = b[i];
    }
    blr_cholesky_solve<double>(ld, xd.data());
    double diff = 0.0;
    double nrm = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        diff += (xlr[i] - xd[i]) * (xlr[i] - xd[i]);
        nrm += xd[i] * xd[i];
    }
    CHECK(std::sqrt(diff) < 1e-3 * std::sqrt(nrm));  // LR-arithmetic ≈ dense-oracle factor
}

TEST_CASE("v5e-3c BLR-Cholesky (LR arithmetic): deterministic factor+solve", "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    constexpr crd::usize n = 192;
    const Matrix<double> a = spd_kernel<double>(&alloc, n);
    crd::containers::Array<double> b(&alloc);
    b.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        b[i] = std::sin(static_cast<double>(i) * 0.7 + 0.3);
    }
    auto run = [&](crd::containers::Array<double>& out)
    {
        BlrMatrix<double> l(&alloc);
        REQUIRE(blr_cholesky_factor_lr<double>(&alloc, a, 32, 1e-10, l));
        out.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            out[i] = b[i];
        }
        blr_cholesky_solve<double>(l, out.data());
    };
    crd::containers::Array<double> x1(&alloc);
    crd::containers::Array<double> x2(&alloc);
    run(x1);
    run(x2);
    bool ident = true;
    for (crd::usize i = 0; i < n && ident; ++i)
    {
        ident = (x1[i] == x2[i]);
    }
    CHECK(ident);  // moat: the LR×LR updates accumulate in fixed k-order ⇒ bit-identical
}

TEST_CASE("v5e-3c BLR-Cholesky (LR arithmetic): f32", "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    constexpr crd::usize n = 128;
    const Matrix<float> a = spd_kernel<float>(&alloc, n);
    crd::containers::Array<float> xt(&alloc);
    crd::containers::Array<float> b(&alloc);
    xt.resize(n);
    b.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        xt[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.5 + 0.2));
    }
    matvec<float>(a, xt.data(), b.data());
    BlrMatrix<float> l(&alloc);
    REQUIRE(blr_cholesky_factor_lr<float>(&alloc, a, 32, 1e-4F, l));
    crd::containers::Array<float> x(&alloc);
    x.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    blr_cholesky_solve<float>(l, x.data());
    CHECK(solve_residual<float>(&alloc, a, x, b) < 1e-2F);
}

// L·Lᵀ from the lower triangle of `l` (m×m), for front-factor validation.
namespace
{
template <typename T>
Matrix<T> lower_llt(crd::memory::IAllocator* alloc, const Matrix<T>& l, crd::usize m)
{
    Matrix<T> out(alloc, m, m);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < m; ++j)
        {
            const crd::usize kmax = (i < j ? i : j);
            T s = T{0};
            for (crd::usize k = 0; k <= kmax; ++k)
            {
                s += l.at(i, k) * l.at(j, k);
            }
            out.at(i, j) = s;
        }
    }
    return out;
}
} // namespace

TEST_CASE("v5e-3d factor_front_cholesky_blr: full front (npiv=m) => L*L^T = A", "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    constexpr crd::usize m = 320;
    const Matrix<double> a = spd_kernel<double>(&alloc, m);
    Matrix<double> front = a.clone();
    REQUIRE(factor_front_cholesky_blr<double>(&alloc, front, m, 128, 1e-9));  // npiv = m ⇒ full factor
    const Matrix<double> llt = lower_llt<double>(&alloc, front, m);
    CHECK(frob_diff<double>(a, llt) < 1e-5 * frob<double>(a));
}

TEST_CASE("v5e-3d factor_front_cholesky_blr: partial front => L + Schur complete to A", "[hesap][blr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    constexpr crd::usize m = 384;
    constexpr crd::usize npiv = 256;
    constexpr crd::usize ms = m - npiv;
    const Matrix<double> a = spd_kernel<double>(&alloc, m);
    Matrix<double> front = a.clone();
    REQUIRE(factor_front_cholesky_blr<double>(&alloc, front, npiv, 128, 1e-9));  // L11,L21 + Schur S

    // Complete: extract the symmetric Schur S (trailing lower, mirrored), factor it.
    Matrix<double> s(&alloc, ms, ms);
    for (crd::usize i = 0; i < ms; ++i)
    {
        for (crd::usize j = 0; j < ms; ++j)
        {
            const crd::usize a2 = (i >= j) ? i : j;  // read lower
            const crd::usize b2 = (i >= j) ? j : i;
            s.at(i, j) = front.at(npiv + a2, npiv + b2);
        }
    }
    REQUIRE(factor_front_cholesky_blr<double>(&alloc, s, ms, 128, 1e-9));  // S → L22

    // Assemble the full lower-triangular L and verify L·Lᵀ = A.
    Matrix<double> l(&alloc, m, m);
    l.set_zero();
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            l.at(i, j) = (j < npiv) ? front.at(i, j) : s.at(i - npiv, j - npiv);  // [L11,L21 ; L22]
        }
    }
    const Matrix<double> llt = lower_llt<double>(&alloc, l, m);
    CHECK(frob_diff<double>(a, llt) < 1e-5 * frob<double>(a));  // partial factor + Schur are correct
}

// ---------- v5e-3d GATE: single-front crossover (manual bench, hidden tag) ----------
// Build optimized (linux-gcc-release) and run:
//   crd-hesap-direct-tests "[blr-bench]"
// Times the BLR-arithmetic factor vs a fast blocked dense Cholesky on ONE realistic
// (smooth, moderately-low-rank) separator front at the size that occurs at n≥110K.
// Answers the advisor's gate: within-front BLR speedup + achievable rate ⇒ whether
// the driver targets a speed-crush or parity+moat. NOT run in the normal suite ([.]).
namespace
{
// Smooth Gaussian kernel with decay scaled to n (c = 4/n²): gentle across the whole
// front ⇒ off-diagonal blocks are MODERATELY low-rank (like a real PDE Schur),
// NOT near-banded. SPD via +1 regularization.
template <typename T>
Matrix<T> bench_front(crd::memory::IAllocator* alloc, crd::usize n)
{
    const double c = 4.0 / (static_cast<double>(n) * static_cast<double>(n));
    Matrix<T> a(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            const double d = static_cast<double>(i) - static_cast<double>(j);
            const T v = static_cast<T>(std::exp(-c * d * d));
            a.at(i, j) = v;
            a.at(j, i) = v;
        }
        a.at(i, i) += static_cast<T>(1.0);
    }
    return a;
}
} // namespace

TEST_CASE("v5e-3d BLR front crossover (manual bench)", "[.][blr-bench]")
{
    using Clock = std::chrono::steady_clock;
    const auto secs = [](Clock::time_point a, Clock::time_point b)
    { return std::chrono::duration<double>(b - a).count(); };

    for (crd::usize n : {1024U, 2048U, 4096U})
    {
        crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1024U) * 1024U * 1024U);
        const Matrix<double> a = bench_front<double>(&alloc, n);

        // Fast blocked dense Cholesky baseline (gemm-backed).
        crd::hesap::dense::Cholesky<double> chol(&alloc, n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j <= i; ++j)
            {
                chol.packed().at(i, j) = a.at(i, j);
            }
        }
        const auto t0 = Clock::now();
        crd::hesap::dense::factor_cholesky<double>(chol);
        const auto t1 = Clock::now();
        const double td = secs(t0, t1);

        // BLR-arithmetic factor (block 256, ε=1e-6).
        BlrMatrix<double> l(&alloc);
        const auto t2 = Clock::now();
        const bool ok = blr_cholesky_factor_lr<double>(&alloc, a, 256, 1e-6, l);
        const auto t3 = Clock::now();
        const double tb = secs(t2, t3);
        REQUIRE(ok);

        crd::usize maxr = 0;
        for (crd::usize i = 0; i < l.nb; ++i)
        {
            for (crd::usize j = 0; j < i; ++j)
            {
                if (l.at(i, j).is_lowrank && l.at(i, j).rank > maxr)
                {
                    maxr = l.at(i, j).rank;
                }
            }
        }
        const double dense_gf = (static_cast<double>(n) * static_cast<double>(n) * static_cast<double>(n) / 3.0) /
                                td / 1e9;
        std::fprintf(stderr,
                     "[blr-front] n=%4zu  dense=%.4fs (%5.1f GF/s)  blr=%.4fs  speedup=%.2fx  maxrank=%zu\n", n, td,
                     dense_gf, tb, td / tb, maxr);
    }
}
