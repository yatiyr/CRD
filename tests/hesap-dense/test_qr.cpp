#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/qr.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using crd::hesap::dense::apply_q;
using crd::hesap::dense::apply_q_transpose;
using crd::hesap::dense::factor_qr;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::QR;
using crd::hesap::dense::solve_qr;
using Catch::Matchers::WithinAbs;

namespace
{
// Apply the implicit Q from a QR factor to an n×n identity matrix to materialize Q
// as a dense Matrix, for testing orthogonality / Q*R reconstruction.
template <typename T>
void materialize_q(const QR<T, Layout::RowMajor>& qr, Matrix<T, Layout::RowMajor>& q_out)
{
    const crd::usize m = qr.rows();
    CRD_ASSERT_MSG(q_out.rows() == m && q_out.cols() == m, "Q is m×m");
    // Initialize q_out to identity.
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < m; ++j)
        {
            q_out.at(i, j) = (i == j) ? T{1} : T{0};
        }
    }
    // Apply Q to each column of identity → Q itself.
    for (crd::usize col = 0; col < m; ++col)
    {
        crd::containers::Array<T> tmp(q_out.allocator());
        tmp.resize(m);
        for (crd::usize i = 0; i < m; ++i)
        {
            tmp[i] = q_out.at(i, col);
        }
        crd::containers::Span<T> ts(tmp.data(), m);
        apply_q(qr, ts);
        for (crd::usize i = 0; i < m; ++i)
        {
            q_out.at(i, col) = tmp[i];
        }
    }
}
} // namespace

TEST_CASE("QR: 2x2 textbook factor", "[hesap][qr][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // A = [[1, 1], [1, 0]]. Classic QR:
    // ||col0|| = sqrt(2), so R[0,0] = -sqrt(2) (negative-sign convention).
    Matrix<double, Layout::RowMajor> a(&alloc, 2, 2, {1.0, 1.0, 1.0, 0.0});
    QR<double, Layout::RowMajor> qr(&alloc, 2, 2);
    factor_qr(qr, a);
    // R[0,0] should be -sqrt(2) ≈ -1.41421356.
    CHECK_THAT(qr.packed().at(0, 0), WithinAbs(-std::sqrt(2.0), 1e-12));
}

TEST_CASE("QR: solve square A*x = b at N=4", "[hesap][qr][real][solve]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    Matrix<double, Layout::RowMajor> a(&alloc, 4, 4,
        {10.0,  2.0,  1.0,  3.0,
          1.0, 12.0,  4.0,  2.0,
          2.0,  3.0, 15.0,  1.0,
          1.0,  1.0,  2.0, 20.0});
    crd::containers::Array<double> x_true(&alloc);
    x_true.resize(4);
    x_true[0] = 1.0; x_true[1] = 2.0; x_true[2] = 3.0; x_true[3] = 4.0;
    crd::containers::Array<double> b(&alloc);
    b.resize(4);
    for (crd::usize i = 0; i < 4; ++i)
    {
        double s = 0.0;
        for (crd::usize j = 0; j < 4; ++j)
        {
            s += a.at(i, j) * x_true[j];
        }
        b[i] = s;
    }

    QR<double, Layout::RowMajor> qr(&alloc, 4, 4);
    factor_qr(qr, a);
    crd::containers::Span<double> bs(b.data(), 4);
    solve_qr(qr, bs);
    for (crd::usize i = 0; i < 4; ++i)
    {
        CHECK_THAT(bs[i], WithinAbs(x_true[i], 1e-11));
    }
}

TEST_CASE("QR: Q*R reconstruction at N=8", "[hesap][qr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    constexpr crd::usize kN = 8;
    Matrix<double, Layout::RowMajor> a(&alloc, kN, kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j < kN; ++j)
        {
            a.at(i, j) = std::sin(static_cast<double>(i * 3 + j) * 0.1) +
                         (i == j ? 5.0 : 0.0);
        }
    }
    Matrix<double, Layout::RowMajor> a_orig(&alloc, kN, kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j < kN; ++j)
        {
            a_orig.at(i, j) = a.at(i, j);
        }
    }

    QR<double, Layout::RowMajor> qr(&alloc, kN, kN);
    factor_qr(qr, a);

    // Materialize Q.
    Matrix<double, Layout::RowMajor> q(&alloc, kN, kN);
    materialize_q<double>(qr, q);

    // Form Q * R from packed (R is upper triangle of qr.packed()).
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j < kN; ++j)
        {
            double s = 0.0;
            // Q[i, :] * R[:, j]; R[p, j] = qr.at(p, j) for p <= j else 0.
            for (crd::usize p = 0; p <= j; ++p)
            {
                s += q.at(i, p) * qr.packed().at(p, j);
            }
            CHECK_THAT(s, WithinAbs(a_orig.at(i, j), 1e-10));
        }
    }
}

TEST_CASE("QR: orthogonality Q^T * Q == I at N=8", "[hesap][qr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    constexpr crd::usize kN = 8;
    Matrix<double, Layout::RowMajor> a(&alloc, kN, kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j < kN; ++j)
        {
            a.at(i, j) = std::cos(static_cast<double>(i + j * 2) * 0.2) + 1.0;
        }
    }
    QR<double, Layout::RowMajor> qr(&alloc, kN, kN);
    factor_qr(qr, a);
    Matrix<double, Layout::RowMajor> q(&alloc, kN, kN);
    materialize_q<double>(qr, q);

    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j < kN; ++j)
        {
            double s = 0.0;
            for (crd::usize p = 0; p < kN; ++p)
            {
                s += q.at(p, i) * q.at(p, j);
            }
            CHECK_THAT(s, WithinAbs(i == j ? 1.0 : 0.0, 1e-12));
        }
    }
}

TEST_CASE("QR: least-squares solve for over-determined 6x3", "[hesap][qr][real][lstsq]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    constexpr crd::usize kM = 6;
    constexpr crd::usize kN = 3;
    // A has rank 3, no noise → LS solution exactly recovers x_true.
    Matrix<double, Layout::RowMajor> a(&alloc, kM, kN);
    for (crd::usize i = 0; i < kM; ++i)
    {
        a.at(i, 0) = 1.0;
        a.at(i, 1) = static_cast<double>(i);
        a.at(i, 2) = static_cast<double>(i) * static_cast<double>(i);
    }
    crd::containers::Array<double> x_true(&alloc);
    x_true.resize(kN);
    x_true[0] = 2.5; x_true[1] = -1.5; x_true[2] = 0.5;
    crd::containers::Array<double> b(&alloc);
    b.resize(kM);
    for (crd::usize i = 0; i < kM; ++i)
    {
        double s = 0.0;
        for (crd::usize j = 0; j < kN; ++j)
        {
            s += a.at(i, j) * x_true[j];
        }
        b[i] = s;
    }

    QR<double, Layout::RowMajor> qr(&alloc, kM, kN);
    factor_qr(qr, a);
    crd::containers::Span<double> bs(b.data(), kM);
    solve_qr(qr, bs);
    for (crd::usize i = 0; i < kN; ++i)
    {
        CHECK_THAT(bs[i], WithinAbs(x_true[i], 1e-10));
    }
}

TEST_CASE("QR: f32 square solve at N=16", "[hesap][qr][real][f32]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    constexpr crd::usize kN = 16;
    Matrix<float, Layout::RowMajor> a(&alloc, kN, kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j < kN; ++j)
        {
            a.at(i, j) = std::sin(static_cast<float>(i * 5 + j) * 0.1F) +
                         (i == j ? 5.0F : 0.0F);
        }
    }
    crd::containers::Array<float> x_true(&alloc);
    x_true.resize(kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        x_true[i] = static_cast<float>(i + 1);
    }
    crd::containers::Array<float> b(&alloc);
    b.resize(kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        float s = 0.0F;
        for (crd::usize j = 0; j < kN; ++j)
        {
            s += a.at(i, j) * x_true[j];
        }
        b[i] = s;
    }

    QR<float, Layout::RowMajor> qr(&alloc, kN, kN);
    factor_qr(qr, a);
    crd::containers::Span<float> bs(b.data(), kN);
    solve_qr(qr, bs);
    for (crd::usize i = 0; i < kN; ++i)
    {
        CHECK_THAT(static_cast<double>(bs[i]),
                   WithinAbs(static_cast<double>(x_true[i]), 1e-3));
    }
}

TEST_CASE("QR: Q*R reconstruction at N=64 (block boundary)",
          "[hesap][qr][real][blocked]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    constexpr crd::usize kN = 64;
    Matrix<double, Layout::RowMajor> a(&alloc, kN, kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j < kN; ++j)
        {
            a.at(i, j) = std::sin(static_cast<double>(i * 7 + j) * 0.1) +
                         (i == j ? 10.0 : 0.0);
        }
    }
    Matrix<double, Layout::RowMajor> a_orig(&alloc, kN, kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j < kN; ++j)
        {
            a_orig.at(i, j) = a.at(i, j);
        }
    }
    QR<double, Layout::RowMajor> qr(&alloc, kN, kN);
    factor_qr(qr, a);
    Matrix<double, Layout::RowMajor> q(&alloc, kN, kN);
    materialize_q<double>(qr, q);
    // Q*R should equal A_orig.
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j < kN; ++j)
        {
            double s = 0.0;
            for (crd::usize p = 0; p <= j; ++p)
            {
                s += q.at(i, p) * qr.packed().at(p, j);
            }
            CHECK_THAT(s, WithinAbs(a_orig.at(i, j), 1e-9));
        }
    }
}

TEST_CASE("QR: Q*R reconstruction at N=128 (multi-block trailing update)",
          "[hesap][qr][real][blocked]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    constexpr crd::usize kN = 128;
    Matrix<double, Layout::RowMajor> a(&alloc, kN, kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j < kN; ++j)
        {
            a.at(i, j) = std::cos(static_cast<double>(i * 5 + j * 3) * 0.07) +
                         (i == j ? 20.0 : 0.0);
        }
    }
    Matrix<double, Layout::RowMajor> a_orig(&alloc, kN, kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j < kN; ++j)
        {
            a_orig.at(i, j) = a.at(i, j);
        }
    }
    QR<double, Layout::RowMajor> qr(&alloc, kN, kN);
    factor_qr(qr, a);
    Matrix<double, Layout::RowMajor> q(&alloc, kN, kN);
    materialize_q<double>(qr, q);
    double max_err = 0.0;
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j < kN; ++j)
        {
            double s = 0.0;
            for (crd::usize p = 0; p <= j; ++p)
            {
                s += q.at(i, p) * qr.packed().at(p, j);
            }
            const double d = std::abs(s - a_orig.at(i, j));
            if (d > max_err) max_err = d;
        }
    }
    REQUIRE(max_err < 1e-8);
}

TEST_CASE("QR: apply_q * apply_q_transpose is identity",
          "[hesap][qr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    constexpr crd::usize kN = 12;
    Matrix<double, Layout::RowMajor> a(&alloc, kN, kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j < kN; ++j)
        {
            a.at(i, j) = std::sin(static_cast<double>(i + j)) + (i == j ? 3.0 : 0.0);
        }
    }
    QR<double, Layout::RowMajor> qr(&alloc, kN, kN);
    factor_qr(qr, a);

    crd::containers::Array<double> x(&alloc);
    x.resize(kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        x[i] = static_cast<double>(i + 1) * 0.5;
    }
    crd::containers::Array<double> x_orig(&alloc);
    x_orig.resize(kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        x_orig[i] = x[i];
    }
    crd::containers::Span<double> xs(x.data(), kN);
    apply_q(qr, xs);
    apply_q_transpose(qr, xs);
    for (crd::usize i = 0; i < kN; ++i)
    {
        CHECK_THAT(x[i], WithinAbs(x_orig[i], 1e-12));
    }
}
