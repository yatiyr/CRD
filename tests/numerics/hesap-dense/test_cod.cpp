#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/cod.hpp>
#include <crd/hesap/dense/detail/apply_q_block.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/qr.hpp>
#include <crd/hesap/dense/qr_colpiv.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <cmath>

using crd::hesap::dense::apply_q;
using crd::hesap::dense::COD;
using crd::hesap::dense::factor_cod;
using crd::hesap::dense::factor_qr_colpiv;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::QRColPiv;
using crd::hesap::dense::solve_cod;
using Catch::Matchers::WithinAbs;

namespace
{
// Materialize Q (m×m) from a QRColPiv factor by applying Q to the columns of I.
template <typename T>
void materialize_q(const QRColPiv<T, Layout::RowMajor>& qr, Matrix<T, Layout::RowMajor>& q_out)
{
    const crd::usize m = qr.rows();
    for (crd::usize col = 0; col < m; ++col)
    {
        crd::containers::Array<T> tmp(q_out.allocator());
        tmp.resize(m);
        for (crd::usize i = 0; i < m; ++i)
        {
            tmp[i] = (i == col) ? T{1} : T{0};
        }
        apply_q(qr, crd::containers::Span<T>{tmp.data(), m});
        for (crd::usize i = 0; i < m; ++i)
        {
            q_out.at(i, col) = tmp[i];
        }
    }
}
} // namespace

TEST_CASE("apply_q_block: BLAS-3 dlarfb matches scalar apply (all 4 modes)",
          "[hesap][colpiv][real][blocked]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    using crd::hesap::dense::apply_q_transpose;
    using crd::hesap::dense::factor_qr;
    using crd::hesap::dense::QR;
    // m large enough to cross the nb=32 block boundary so the multi-block path runs.
    constexpr crd::usize k_m = 70;
    constexpr crd::usize n = 40;
    Matrix<double, Layout::RowMajor> a(&alloc, k_m, n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            a.at(i, j) = std::sin(static_cast<double>(i * 7 + j) * 0.13) + (i == j ? 5.0 : 0.0);
        }
    }
    QR<double, Layout::RowMajor> qr(&alloc, k_m, n);
    factor_qr(qr, a);
    const double* qp = qr.packed().data();
    const crd::usize ld = qr.packed().ld();
    const crd::usize kk = qr.num_reflectors();
    const double* taus = qr.taus().data();

    constexpr crd::usize k_cols = 5;
    auto fill = [&](Matrix<double, Layout::RowMajor>& c) {
        for (crd::usize i = 0; i < c.rows(); ++i)
            for (crd::usize j = 0; j < c.cols(); ++j)
                c.at(i, j) = std::cos(static_cast<double>(i * 3 + j) * 0.21) + 0.5;
    };

    // Left modes: C is m × k_cols; column j transforms as op(Q)·col_j.
    for (bool trans : {true, false})
    {
        Matrix<double, Layout::RowMajor> c(&alloc, k_m, k_cols);
        fill(c);
        Matrix<double, Layout::RowMajor> ref(&alloc, k_m, k_cols);
        // Scalar reference: per-column apply.
        for (crd::usize j = 0; j < k_cols; ++j)
        {
            crd::containers::Array<double> col(&alloc);
            col.resize(k_m);
            for (crd::usize i = 0; i < k_m; ++i) col[i] = c.at(i, j);
            if (trans)
                apply_q_transpose(qr, crd::containers::Span<double>{col.data(), k_m});
            else
                crd::hesap::dense::apply_q(qr, crd::containers::Span<double>{col.data(), k_m});
            for (crd::usize i = 0; i < k_m; ++i) ref.at(i, j) = col[i];
        }
        crd::hesap::dense::detail::apply_q_block<double>(qp, ld, k_m, kk, taus, c.data(), c.ld(), k_m,
                                                         k_cols, /*right=*/false, trans, &alloc);
        double e = 0.0;
        for (crd::usize i = 0; i < k_m; ++i)
            for (crd::usize j = 0; j < k_cols; ++j)
                e = std::max(e, std::abs(c.at(i, j) - ref.at(i, j)));
        INFO("Left trans=" << trans);
        REQUIRE(e < 1e-11);
    }

    // Right modes: C is k_cols × m; row i transforms as row_i·op(Q).
    //   C·Qᵀ row i = (Q·row_iᵀ)ᵀ → scalar apply_q on the row.
    //   C·Q  row i = (Qᵀ·row_iᵀ)ᵀ → scalar apply_q_transpose on the row.
    for (bool trans : {true, false})
    {
        Matrix<double, Layout::RowMajor> c(&alloc, k_cols, k_m);
        fill(c);
        Matrix<double, Layout::RowMajor> ref(&alloc, k_cols, k_m);
        for (crd::usize i = 0; i < k_cols; ++i)
        {
            crd::containers::Array<double> row(&alloc);
            row.resize(k_m);
            for (crd::usize j = 0; j < k_m; ++j) row[j] = c.at(i, j);
            if (trans)
                crd::hesap::dense::apply_q(qr, crd::containers::Span<double>{row.data(), k_m});
            else
                apply_q_transpose(qr, crd::containers::Span<double>{row.data(), k_m});
            for (crd::usize j = 0; j < k_m; ++j) ref.at(i, j) = row[j];
        }
        // k_cols/k_m arg names trip the swapped-argument heuristic against params crows/ccols;
        // the call order is correct (unchanged, long-tested) -- false positive.
        // NOLINTNEXTLINE(readability-suspicious-call-argument)
        crd::hesap::dense::detail::apply_q_block<double>(qp, ld, k_m, kk, taus, c.data(), c.ld(),
                                                         k_cols, k_m, /*right=*/true, trans, &alloc);
        double e = 0.0;
        for (crd::usize i = 0; i < k_cols; ++i)
            for (crd::usize j = 0; j < k_m; ++j)
                e = std::max(e, std::abs(c.at(i, j) - ref.at(i, j)));
        INFO("Right trans=" << trans);
        REQUIRE(e < 1e-11);
    }
}

TEST_CASE("col-piv QR: A*P = Q*R reconstruction at 8x6", "[hesap][colpiv][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    constexpr crd::usize k_m = 8;
    constexpr crd::usize n = 6;
    Matrix<double, Layout::RowMajor> a(&alloc, k_m, n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            a.at(i, j) = std::sin(static_cast<double>(i * 3 + j) * 0.31) +
                         (i == j ? 4.0 : 0.0);
        }
    }
    QRColPiv<double, Layout::RowMajor> qr(&alloc, k_m, n);
    factor_qr_colpiv(qr, a);

    // Diagonal of R must be non-increasing in magnitude (Businger-Golub).
    for (crd::usize k = 1; k < n; ++k)
    {
        CHECK(std::abs(qr.packed().at(k, k)) <=
              std::abs(qr.packed().at(k - 1, k - 1)) + 1e-12);
    }
    // Full rank expected.
    CHECK(qr.rank() == n);

    Matrix<double, Layout::RowMajor> q(&alloc, k_m, k_m);
    materialize_q<double>(qr, q);

    // (Q*R)[i][j] should equal A[i][jpvt[j]] (R is upper-trapezoidal in packed).
    const auto& jpvt = qr.jpvt();
    double max_err = 0.0;
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            double s = 0.0;
            for (crd::usize p = 0; p <= j && p < k_m; ++p)
            {
                s += q.at(i, p) * qr.packed().at(p, j);
            }
            const double d = std::abs(s - a.at(i, jpvt[j]));
            if (d > max_err)
            {
                max_err = d;
            }
        }
    }
    REQUIRE(max_err < 1e-11);
}

TEST_CASE("col-piv QR: reveals rank of a rank-deficient matrix", "[hesap][colpiv][real][rank]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    constexpr crd::usize k_m = 6;
    constexpr crd::usize n = 4;
    // Column 3 := column 0 + column 1  → exactly rank 3.
    Matrix<double, Layout::RowMajor> a(&alloc, k_m, n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        a.at(i, 0) = std::sin(static_cast<double>(i) * 0.5) + 1.0;
        a.at(i, 1) = std::cos(static_cast<double>(i) * 0.3) + 2.0;
        a.at(i, 2) = static_cast<double>(i) * 0.25 - 0.5;
        a.at(i, 3) = a.at(i, 0) + a.at(i, 1);
    }
    QRColPiv<double, Layout::RowMajor> qr(&alloc, k_m, n);
    factor_qr_colpiv(qr, a);
    CHECK(qr.rank() == 3);
}

TEST_CASE("COD: min-norm solve matches QR on full-rank over-determined", "[hesap][cod][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    constexpr crd::usize k_m = 7;
    constexpr crd::usize n = 3;
    // Vandermonde-like full-rank A, exact b → recovers x_true.
    Matrix<double, Layout::RowMajor> a(&alloc, k_m, n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        a.at(i, 0) = 1.0;
        a.at(i, 1) = static_cast<double>(i);
        a.at(i, 2) = static_cast<double>(i) * static_cast<double>(i);
    }
    crd::containers::Array<double> x_true(&alloc);
    x_true.resize(n);
    x_true[0] = 3.0; x_true[1] = -2.0; x_true[2] = 0.75;
    crd::containers::Array<double> b(&alloc);
    b.resize(k_m);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        double s = 0.0;
        for (crd::usize j = 0; j < n; ++j)
        {
            s += a.at(i, j) * x_true[j];
        }
        b[i] = s;
    }

    COD<double, Layout::RowMajor> cod = factor_cod<double, Layout::RowMajor>(&alloc, a);
    CHECK(cod.rank == n);
    crd::containers::Array<double> x(&alloc);
    x.resize(n);
    solve_cod<double, Layout::RowMajor>(cod, crd::containers::ConstSpan<double>{b.data(), k_m},
                                        crd::containers::Span<double>{x.data(), n});
    for (crd::usize i = 0; i < n; ++i)
    {
        CHECK_THAT(x[i], WithinAbs(x_true[i], 1e-10));
    }
}

TEST_CASE("COD: rank-deficient min-norm solution (minimizer + min-norm)", "[hesap][cod][real][rank]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    constexpr crd::usize k_m = 6;
    constexpr crd::usize n = 4;
    // Column 3 := column 0 + column 1  → rank 3, null vector z = [1, 1, 0, -1].
    Matrix<double, Layout::RowMajor> a(&alloc, k_m, n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        a.at(i, 0) = std::sin(static_cast<double>(i) * 0.5) + 1.0;
        a.at(i, 1) = std::cos(static_cast<double>(i) * 0.3) + 2.0;
        a.at(i, 2) = static_cast<double>(i) * 0.25 - 0.5;
        a.at(i, 3) = a.at(i, 0) + a.at(i, 1);
    }
    crd::containers::Array<double> b(&alloc);
    b.resize(k_m);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        b[i] = std::sin(static_cast<double>(i) * 0.9 + 0.4) + 0.5;
    }

    COD<double, Layout::RowMajor> cod = factor_cod<double, Layout::RowMajor>(&alloc, a);
    REQUIRE(cod.rank == 3);
    crd::containers::Array<double> x(&alloc);
    x.resize(n);
    solve_cod<double, Layout::RowMajor>(cod, crd::containers::ConstSpan<double>{b.data(), k_m},
                                        crd::containers::Span<double>{x.data(), n});

    // (a) Minimizer: gradient A^T (A x - b) ≈ 0.
    crd::containers::Array<double> r(&alloc);
    r.resize(k_m);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        double ax = 0.0;
        for (crd::usize j = 0; j < n; ++j)
        {
            ax += a.at(i, j) * x[j];
        }
        r[i] = ax - b[i];
    }
    for (crd::usize j = 0; j < n; ++j)
    {
        double g = 0.0;
        for (crd::usize i = 0; i < k_m; ++i)
        {
            g += a.at(i, j) * r[i];
        }
        CHECK_THAT(g, WithinAbs(0.0, 1e-10));
    }

    // (b) Minimum norm: x ⊥ null(A); z = [1,1,0,-1] spans the null space.
    const double dot_z = x[0] + x[1] - x[3];
    CHECK_THAT(dot_z, WithinAbs(0.0, 1e-10));
}

TEST_CASE("COD: f32 rank-deficient min-norm minimizer", "[hesap][cod][real][f32]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    constexpr crd::usize k_m = 5;
    constexpr crd::usize n = 3;
    // Column 2 := 2 * column 0  → rank 2, null vector z = [2, 0, -1].
    Matrix<float, Layout::RowMajor> a(&alloc, k_m, n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        a.at(i, 0) = std::sin(static_cast<float>(i) * 0.4F) + 1.0F;
        a.at(i, 1) = static_cast<float>(i) * 0.3F + 1.0F;
        a.at(i, 2) = 2.0F * a.at(i, 0);
    }
    crd::containers::Array<float> b(&alloc);
    b.resize(k_m);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        b[i] = std::cos(static_cast<float>(i) * 0.7F) + 0.3F;
    }

    COD<float, Layout::RowMajor> cod = factor_cod<float, Layout::RowMajor>(&alloc, a);
    REQUIRE(cod.rank == 2);
    crd::containers::Array<float> x(&alloc);
    x.resize(n);
    solve_cod<float, Layout::RowMajor>(cod, crd::containers::ConstSpan<float>{b.data(), k_m},
                                       crd::containers::Span<float>{x.data(), n});

    // Gradient ≈ 0.
    for (crd::usize j = 0; j < n; ++j)
    {
        float g = 0.0F;
        for (crd::usize i = 0; i < k_m; ++i)
        {
            float ax = 0.0F;
            for (crd::usize k = 0; k < n; ++k)
            {
                ax += a.at(i, k) * x[k];
            }
            g += a.at(i, j) * (ax - b[i]);
        }
        CHECK_THAT(static_cast<double>(g), WithinAbs(0.0, 1e-4));
    }
    // x ⊥ null: z = [2, 0, -1] → 2*x0 - x2 == 0.
    CHECK_THAT(static_cast<double>(2.0F * x[0] - x[2]), WithinAbs(0.0, 1e-4));
}
