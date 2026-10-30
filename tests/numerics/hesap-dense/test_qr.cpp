#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/detail/factor_arms.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/qr.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "hesap_jobs_fixture.hpp"

#include <cmath>
#include <cstring>
#include <limits>

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
    constexpr crd::usize k_n = 8;
    Matrix<double, Layout::RowMajor> a(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = std::sin(static_cast<double>(i * 3 + j) * 0.1) +
                         (i == j ? 5.0 : 0.0);
        }
    }
    Matrix<double, Layout::RowMajor> a_orig(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a_orig.at(i, j) = a.at(i, j);
        }
    }

    QR<double, Layout::RowMajor> qr(&alloc, k_n, k_n);
    factor_qr(qr, a);

    // Materialize Q.
    Matrix<double, Layout::RowMajor> q(&alloc, k_n, k_n);
    materialize_q<double>(qr, q);

    // Form Q * R from packed (R is upper triangle of qr.packed()).
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
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
    constexpr crd::usize k_n = 8;
    Matrix<double, Layout::RowMajor> a(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = std::cos(static_cast<double>(i + j * 2) * 0.2) + 1.0;
        }
    }
    QR<double, Layout::RowMajor> qr(&alloc, k_n, k_n);
    factor_qr(qr, a);
    Matrix<double, Layout::RowMajor> q(&alloc, k_n, k_n);
    materialize_q<double>(qr, q);

    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            double s = 0.0;
            for (crd::usize p = 0; p < k_n; ++p)
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
    constexpr crd::usize k_m = 6;
    constexpr crd::usize k_n = 3;
    // A has rank 3, no noise → LS solution exactly recovers x_true.
    Matrix<double, Layout::RowMajor> a(&alloc, k_m, k_n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        a.at(i, 0) = 1.0;
        a.at(i, 1) = static_cast<double>(i);
        a.at(i, 2) = static_cast<double>(i) * static_cast<double>(i);
    }
    crd::containers::Array<double> x_true(&alloc);
    x_true.resize(k_n);
    x_true[0] = 2.5; x_true[1] = -1.5; x_true[2] = 0.5;
    crd::containers::Array<double> b(&alloc);
    b.resize(k_m);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        double s = 0.0;
        for (crd::usize j = 0; j < k_n; ++j)
        {
            s += a.at(i, j) * x_true[j];
        }
        b[i] = s;
    }

    QR<double, Layout::RowMajor> qr(&alloc, k_m, k_n);
    factor_qr(qr, a);
    crd::containers::Span<double> bs(b.data(), k_m);
    solve_qr(qr, bs);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        CHECK_THAT(bs[i], WithinAbs(x_true[i], 1e-10));
    }
}

TEST_CASE("QR: f32 square solve at N=16", "[hesap][qr][real][f32]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    constexpr crd::usize k_n = 16;
    Matrix<float, Layout::RowMajor> a(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = std::sin(static_cast<float>(i * 5 + j) * 0.1F) +
                         (i == j ? 5.0F : 0.0F);
        }
    }
    crd::containers::Array<float> x_true(&alloc);
    x_true.resize(k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        x_true[i] = static_cast<float>(i + 1);
    }
    crd::containers::Array<float> b(&alloc);
    b.resize(k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        float s = 0.0F;
        for (crd::usize j = 0; j < k_n; ++j)
        {
            s += a.at(i, j) * x_true[j];
        }
        b[i] = s;
    }

    QR<float, Layout::RowMajor> qr(&alloc, k_n, k_n);
    factor_qr(qr, a);
    crd::containers::Span<float> bs(b.data(), k_n);
    solve_qr(qr, bs);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        CHECK_THAT(static_cast<double>(bs[i]),
                   WithinAbs(static_cast<double>(x_true[i]), 1e-3));
    }
}

TEST_CASE("QR: Q*R reconstruction at N=64 (block boundary)",
          "[hesap][qr][real][blocked]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    constexpr crd::usize k_n = 64;
    Matrix<double, Layout::RowMajor> a(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = std::sin(static_cast<double>(i * 7 + j) * 0.1) +
                         (i == j ? 10.0 : 0.0);
        }
    }
    Matrix<double, Layout::RowMajor> a_orig(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a_orig.at(i, j) = a.at(i, j);
        }
    }
    QR<double, Layout::RowMajor> qr(&alloc, k_n, k_n);
    factor_qr(qr, a);
    Matrix<double, Layout::RowMajor> q(&alloc, k_n, k_n);
    materialize_q<double>(qr, q);
    // Q*R should equal A_orig.
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
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
    constexpr crd::usize k_n = 128;
    Matrix<double, Layout::RowMajor> a(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = std::cos(static_cast<double>(i * 5 + j * 3) * 0.07) +
                         (i == j ? 20.0 : 0.0);
        }
    }
    Matrix<double, Layout::RowMajor> a_orig(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a_orig.at(i, j) = a.at(i, j);
        }
    }
    QR<double, Layout::RowMajor> qr(&alloc, k_n, k_n);
    factor_qr(qr, a);
    Matrix<double, Layout::RowMajor> q(&alloc, k_n, k_n);
    materialize_q<double>(qr, q);
    double max_err = 0.0;
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            double s = 0.0;
            for (crd::usize p = 0; p <= j; ++p)
            {
                s += q.at(i, p) * qr.packed().at(p, j);
            }
            const double d = std::abs(s - a_orig.at(i, j));
            if (d > max_err)
            {
                max_err = d;
            }
        }
    }
    REQUIRE(max_err < 1e-8);
}

TEST_CASE("QR: apply_q * apply_q_transpose is identity",
          "[hesap][qr][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    constexpr crd::usize k_n = 12;
    Matrix<double, Layout::RowMajor> a(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = std::sin(static_cast<double>(i + j)) + (i == j ? 3.0 : 0.0);
        }
    }
    QR<double, Layout::RowMajor> qr(&alloc, k_n, k_n);
    factor_qr(qr, a);

    crd::containers::Array<double> x(&alloc);
    x.resize(k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        x[i] = static_cast<double>(i + 1) * 0.5;
    }
    crd::containers::Array<double> x_orig(&alloc);
    x_orig.resize(k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        x_orig[i] = x[i];
    }
    crd::containers::Span<double> xs(x.data(), k_n);
    apply_q(qr, xs);
    apply_q_transpose(qr, xs);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        CHECK_THAT(x[i], WithinAbs(x_orig[i], 1e-12));
    }
}

namespace
{
using crd::hesap::dense::detail::QrPick;

// A's copy factored through factor_qr_arm(pick).
template <typename T>
void qr_with(const QrPick& pick, const Matrix<T, Layout::RowMajor>& a, QR<T, Layout::RowMajor>& qr)
{
    for (crd::usize i = 0; i < a.rows(); ++i)
    {
        for (crd::usize j = 0; j < a.cols(); ++j)
        {
            qr.packed().at(i, j) = a.at(i, j);
        }
    }
    crd::hesap::dense::detail::factor_qr_arm<T, Layout::RowMajor>(pick, qr);
}

template <typename T> bool qr_same(const QR<T, Layout::RowMajor>& x, const QR<T, Layout::RowMajor>& y)
{
    const crd::usize k = x.num_reflectors();
    return std::memcmp(x.packed().data(), y.packed().data(), x.rows() * x.packed().ld() * sizeof(T)) == 0 &&
           std::memcmp(x.taus().data(), y.taus().data(), k * sizeof(T)) == 0;
}

// The backward error of Householder QR (Higham Theorem 19.4): the computed R is the exact R of A + dA with
// |da_j| <= gamma~(m n) |a_j| column by column; Q R is formed in double from the stored reflectors and c = 1.
template <typename T>
bool qr_backward_stable(const QR<T, Layout::RowMajor>& qr, const Matrix<T, Layout::RowMajor>& a,
                        crd::memory::IAllocator* alloc)
{
    const crd::usize m = qr.rows();
    const crd::usize n = qr.cols();
    const crd::usize k = qr.num_reflectors();
    const double tol =
        static_cast<double>(m) * static_cast<double>(n) * static_cast<double>(std::numeric_limits<T>::epsilon());
    crd::containers::Array<double> x(alloc);
    x.resize(m);
    for (crd::usize j = 0; j < n; ++j)
    {
        for (crd::usize i = 0; i < m; ++i)
        {
            x[i] = i <= j ? static_cast<double>(qr.packed().at(i, j)) : 0.0;
        }
        for (crd::usize r = k; r-- > 0;)
        {
            double w = x[r];
            for (crd::usize i = r + 1; i < m; ++i)
            {
                w += static_cast<double>(qr.packed().at(i, r)) * x[i];
            }
            w *= static_cast<double>(qr.taus()[r]);
            x[r] -= w;
            for (crd::usize i = r + 1; i < m; ++i)
            {
                x[i] -= w * static_cast<double>(qr.packed().at(i, r));
            }
        }
        double err = 0.0;
        double norm = 0.0;
        for (crd::usize i = 0; i < m; ++i)
        {
            const double aij = static_cast<double>(a.at(i, j));
            err += (aij - x[i]) * (aij - x[i]);
            norm += aij * aij;
        }
        if (!(std::sqrt(err) <= tol * std::sqrt(norm) + std::numeric_limits<double>::min()))
        {
            return false;
        }
    }
    return true;
}

template <typename T> void check_qr_levers()
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    // The paths: the default cut and block, always blocked at blocks 8, 32 and 50, and always unblocked (m >= n).
    const QrPick paths[] = {
        QrPick{},
        QrPick{.unblocked_max = 0, .block = 8},
        QrPick{.unblocked_max = 0},
        QrPick{.unblocked_max = 0, .block = 50},
        QrPick{.unblocked_max = ~crd::usize{0}},
    };
    const crd::usize never = ~crd::usize{0};
    const crd::u32 jobs[] = {4U, 8U, 16U, 32U};
    struct Shape
    {
        crd::usize m, n;
    };
    // One element, a block's edge, square under the cut, tall past it and wide (always blocked).
    const Shape shapes[] = {{1, 1}, {33, 32}, {100, 100}, {200, 130}, {120, 190}};
    for (const Shape& sh : shapes)
    {
        // Inputs: random, IEEE specials (NaN, infinity, -0, a subnormal) and a zero column at 40 (its tau 0).
        for (int input = 0; input < 3; ++input)
        {
            if (input != 0 && sh.n < 64)
            {
                continue;
            }
            Matrix<T, Layout::RowMajor> a(&alloc, sh.m, sh.n);
            crd::u32 s = 5U + static_cast<crd::u32>(sh.m * 3U + sh.n);
            for (crd::usize i = 0; i < sh.m; ++i)
            {
                for (crd::usize j = 0; j < sh.n; ++j)
                {
                    s = s * 1664525U + 1013904223U;
                    a.at(i, j) = static_cast<T>(static_cast<double>(s >> 8) / 16777216.0 - 0.5);
                }
            }
            if (input == 1)
            {
                a.at(5, 3) = std::numeric_limits<T>::quiet_NaN();
                a.at(40, 50) = std::numeric_limits<T>::infinity();
                a.at(60, 1) = T{-0.0};
                a.at(10, 62) = std::numeric_limits<T>::min() / T{4};
            }
            if (input == 2)
            {
                for (crd::usize i = 0; i < sh.m; ++i)
                {
                    a.at(i, 40) = T{0};
                }
            }
            crd::usize seen[5] = {};
            crd::usize num_seen = 0;
            for (const QrPick& path : paths)
            {
                // A path this shape already took (its block, or 0 for unblocked) runs once.
                const crd::usize key = sh.m >= sh.n && sh.n <= path.unblocked_max ? 0 : path.block;
                bool taken = false;
                for (crd::usize i = 0; i < num_seen; ++i)
                {
                    taken = taken || seen[i] == key;
                }
                if (taken)
                {
                    continue;
                }
                seen[num_seen++] = key;
                QrPick one = path;
                one.workers = 1;
                QR<T, Layout::RowMajor> ref(&alloc, sh.m, sh.n);
                qr_with(one, a, ref);
                INFO("m " << sh.m << " n " << sh.n << " input " << input << " cut " << path.unblocked_max << " block "
                          << path.block);
                if (input != 1)
                {
                    CHECK(qr_backward_stable(ref, a, &alloc));
                }
                if (key == 0)
                {
                    continue; // the unblocked factor runs no jobs
                }
                for (const crd::usize trailing : {QrPick{}.trailing_min, crd::usize{0}, never})
                {
                    for (const crd::u32 w : jobs)
                    {
                        QrPick pick = path;
                        pick.trailing_min = trailing;
                        pick.workers = w;
                        QR<T, Layout::RowMajor> qr(&alloc, sh.m, sh.n);
                        qr_with(pick, a, qr);
                        INFO("trailing " << trailing << " jobs " << w);
                        CHECK(qr_same(qr, ref));
                    }
                }
            }
        }
    }
}
} // namespace

TEST_CASE("factor_qr_arm: every jobs lever at 4 to 32 jobs gives the one-job factor's bits",
          "[hesap][qr][parallel][jobs]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    check_qr_levers<float>();
    check_qr_levers<double>();
}
