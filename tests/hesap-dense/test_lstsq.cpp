#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/hesap/dense/lstsq.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <cmath>

using crd::hesap::Complex;
using crd::hesap::dense::Layout;
using crd::hesap::dense::lstsq;
using crd::hesap::dense::LstSq;
using crd::hesap::dense::LstSqMethod;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::pinv;
using crd::hesap::dense::Vector;
using Catch::Matchers::WithinAbs;

namespace
{
struct LstsqAnchorPull
{
    LstsqAnchorPull() noexcept { crd::hesap::dense::register_lstsq_cli_anchor(); }
};
const LstsqAnchorPull kLstsqAnchorPull;

// b_i = sum_j A_ij x_j  (real).
template <typename T>
void make_rhs(const Matrix<T>& a, const crd::containers::Array<T>& x, crd::containers::Array<T>& b)
{
    b.resize(a.rows());
    for (crd::usize i = 0; i < a.rows(); ++i)
    {
        T s = T{0};
        for (crd::usize j = 0; j < a.cols(); ++j)
        {
            s += a.at(i, j) * x[j];
        }
        b[i] = s;
    }
}
} // namespace

TEST_CASE("lstsq: full-rank over-determined recovers x (QR/COD/SVD all agree)",
          "[hesap][lstsq][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    constexpr crd::usize k_m = 9;
    constexpr crd::usize k_n = 4;
    Matrix<double, Layout::RowMajor> a(&alloc, k_m, k_n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = std::sin(static_cast<double>(i * 2 + j) * 0.3) + (i == j ? 3.0 : 0.0);
        }
    }
    crd::containers::Array<double> x_true(&alloc);
    x_true.resize(k_n);
    x_true[0] = 1.5; x_true[1] = -2.0; x_true[2] = 0.25; x_true[3] = 3.0;
    crd::containers::Array<double> b(&alloc);
    make_rhs<double>(a, x_true, b);
    Vector<double> bv(&alloc, k_m);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        bv(i) = b[i];
    }

    for (auto method : {LstSqMethod::QR, LstSqMethod::COD, LstSqMethod::SVD, LstSqMethod::Auto})
    {
        LstSq<double> r = lstsq<double>(&alloc, a, bv, method);
        CHECK(r.rank == k_n);
        for (crd::usize j = 0; j < k_n; ++j)
        {
            CHECK_THAT(r.x.at(j, 0), WithinAbs(x_true[j], 1e-9));
        }
        CHECK_THAT(r.residual(0), WithinAbs(0.0, 1e-9));
    }
}

TEST_CASE("lstsq: rank-deficient min-norm (COD/SVD) - minimizer + min-norm",
          "[hesap][lstsq][real][rank]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    constexpr crd::usize k_m = 7;
    constexpr crd::usize k_n = 4;
    // col3 = col0 + col1 → rank 3, null vector z = [1,1,0,-1].
    Matrix<double, Layout::RowMajor> a(&alloc, k_m, k_n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        a.at(i, 0) = std::sin(static_cast<double>(i) * 0.5) + 1.0;
        a.at(i, 1) = std::cos(static_cast<double>(i) * 0.4) + 2.0;
        a.at(i, 2) = static_cast<double>(i) * 0.2 - 0.3;
        a.at(i, 3) = a.at(i, 0) + a.at(i, 1);
    }
    Vector<double> bv(&alloc, k_m);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        bv(i) = std::sin(static_cast<double>(i) * 0.8 + 0.2) + 0.4;
    }

    for (auto method : {LstSqMethod::COD, LstSqMethod::SVD})
    {
        LstSq<double> r = lstsq<double>(&alloc, a, bv, method);
        CHECK(r.rank == 3);
        // Minimizer: Aᵀ(Ax - b) ≈ 0.
        crd::containers::Array<double> resid(&alloc);
        resid.resize(k_m);
        for (crd::usize i = 0; i < k_m; ++i)
        {
            double ax = 0.0;
            for (crd::usize j = 0; j < k_n; ++j)
            {
                ax += a.at(i, j) * r.x.at(j, 0);
            }
            resid[i] = ax - bv(i);
        }
        for (crd::usize j = 0; j < k_n; ++j)
        {
            double g = 0.0;
            for (crd::usize i = 0; i < k_m; ++i)
            {
                g += a.at(i, j) * resid[i];
            }
            CHECK_THAT(g, WithinAbs(0.0, 1e-9));
        }
        // Min-norm: x ⊥ z = [1,1,0,-1].
        CHECK_THAT(r.x.at(0, 0) + r.x.at(1, 0) - r.x.at(3, 0), WithinAbs(0.0, 1e-9));
    }
}

TEST_CASE("lstsq: underdetermined min-norm (m < n) is consistent + in row space",
          "[hesap][lstsq][real][underdetermined]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    constexpr crd::usize k_m = 3;
    constexpr crd::usize k_n = 6;
    Matrix<double, Layout::RowMajor> a(&alloc, k_m, k_n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = std::cos(static_cast<double>(i * 5 + j) * 0.21) + 1.0;
        }
    }
    Vector<double> bv(&alloc, k_m);
    bv(0) = 1.0; bv(1) = -0.5; bv(2) = 2.0;

    LstSq<double> r = lstsq<double>(&alloc, a, bv, LstSqMethod::COD);
    CHECK(r.rank == k_m);  // full row rank
    // Consistent: A x == b (residual ≈ 0).
    CHECK_THAT(r.residual(0), WithinAbs(0.0, 1e-10));
    for (crd::usize i = 0; i < k_m; ++i)
    {
        double ax = 0.0;
        for (crd::usize j = 0; j < k_n; ++j)
        {
            ax += a.at(i, j) * r.x.at(j, 0);
        }
        CHECK_THAT(ax, WithinAbs(bv(i), 1e-10));
    }
    // Min-norm underdetermined solution lies in row(A): x = Aᵀ w. Verified by
    // comparing against the SVD path (which also gives the unique min-norm x).
    LstSq<double> rs = lstsq<double>(&alloc, a, bv, LstSqMethod::SVD);
    for (crd::usize j = 0; j < k_n; ++j)
    {
        CHECK_THAT(r.x.at(j, 0), WithinAbs(rs.x.at(j, 0), 1e-9));
    }
}

TEST_CASE("lstsq: multi-RHS solves each column independently", "[hesap][lstsq][real][multirhs]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    constexpr crd::usize k_m = 8;
    constexpr crd::usize k_n = 3;
    constexpr crd::usize rhs = 2;
    Matrix<double, Layout::RowMajor> a(&alloc, k_m, k_n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        a.at(i, 0) = 1.0;
        a.at(i, 1) = static_cast<double>(i) * 0.5;
        a.at(i, 2) = std::sin(static_cast<double>(i));
    }
    crd::containers::Array<double> x0(&alloc);
    crd::containers::Array<double> x1(&alloc);
    x0.resize(k_n); x1.resize(k_n);
    x0[0] = 1.0; x0[1] = 2.0; x0[2] = -1.0;
    x1[0] = -3.0; x1[1] = 0.5; x1[2] = 4.0;
    crd::containers::Array<double> b0(&alloc);
    crd::containers::Array<double> b1(&alloc);
    make_rhs<double>(a, x0, b0);
    make_rhs<double>(a, x1, b1);
    Matrix<double, Layout::RowMajor> b(&alloc, k_m, rhs);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        b.at(i, 0) = b0[i];
        b.at(i, 1) = b1[i];
    }

    LstSq<double> r = lstsq<double>(&alloc, a, b, LstSqMethod::COD);
    for (crd::usize j = 0; j < k_n; ++j)
    {
        CHECK_THAT(r.x.at(j, 0), WithinAbs(x0[j], 1e-9));
        CHECK_THAT(r.x.at(j, 1), WithinAbs(x1[j], 1e-9));
    }
}

TEST_CASE("pinv: square invertible A+ == A^-1", "[hesap][pinv][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    constexpr crd::usize k_n = 4;
    Matrix<double, Layout::RowMajor> a(&alloc, k_n, k_n,
        {10.0,  2.0,  1.0,  3.0,
          1.0, 12.0,  4.0,  2.0,
          2.0,  3.0, 15.0,  1.0,
          1.0,  1.0,  2.0, 20.0});
    Matrix<double> p = pinv<double>(&alloc, a);
    // A⁺ A == I.
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            double s = 0.0;
            for (crd::usize k = 0; k < k_n; ++k)
            {
                s += p.at(i, k) * a.at(k, j);
            }
            CHECK_THAT(s, WithinAbs(i == j ? 1.0 : 0.0, 1e-10));
        }
    }
}

TEST_CASE("pinv: Moore-Penrose A A+ A == A on rank-deficient rectangular",
          "[hesap][pinv][real][rank]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    constexpr crd::usize k_m = 6;
    constexpr crd::usize k_n = 4;
    Matrix<double, Layout::RowMajor> a(&alloc, k_m, k_n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        a.at(i, 0) = std::sin(static_cast<double>(i) * 0.5) + 1.0;
        a.at(i, 1) = std::cos(static_cast<double>(i) * 0.3) + 2.0;
        a.at(i, 2) = static_cast<double>(i) * 0.25 - 0.5;
        a.at(i, 3) = a.at(i, 0) + a.at(i, 1);  // dependent
    }
    Matrix<double> p = pinv<double>(&alloc, a);  // n x m
    // (A A⁺ A)[i][j] == A[i][j].
    Matrix<double> aap(&alloc, k_m, k_m);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < k_m; ++j)
        {
            double s = 0.0;
            for (crd::usize k = 0; k < k_n; ++k)
            {
                s += a.at(i, k) * p.at(k, j);
            }
            aap.at(i, j) = s;  // A A⁺ (m x m)
        }
    }
    double max_err = 0.0;
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            double s = 0.0;
            for (crd::usize k = 0; k < k_m; ++k)
            {
                s += aap.at(i, k) * a.at(k, j);
            }
            max_err = std::max(max_err, std::abs(s - a.at(i, j)));
        }
    }
    REQUIRE(max_err < 1e-9);
    // A A⁺ is symmetric (a Moore-Penrose condition).
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < k_m; ++j)
        {
            CHECK_THAT(aap.at(i, j), WithinAbs(aap.at(j, i), 1e-9));
        }
    }
}

TEST_CASE("lstsq: complex full-rank over-determined recovers x", "[hesap][lstsq][complex]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    using Cx = Complex<double>;
    constexpr crd::usize k_m = 7;
    constexpr crd::usize k_n = 3;
    Matrix<Cx, Layout::RowMajor> a(&alloc, k_m, k_n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = Cx{std::sin(static_cast<double>(i + j) * 0.3) + (i == j ? 3.0 : 0.0),
                            std::cos(static_cast<double>(i * 2 + j) * 0.2)};
        }
    }
    crd::containers::Array<Cx> x_true(&alloc);
    x_true.resize(k_n);
    x_true[0] = Cx{1.0, -0.5}; x_true[1] = Cx{-2.0, 1.5}; x_true[2] = Cx{0.75, 0.25};
    Vector<Cx> bv(&alloc, k_m);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        Cx s{0.0, 0.0};
        for (crd::usize j = 0; j < k_n; ++j)
        {
            s = s + a.at(i, j) * x_true[j];
        }
        bv(i) = s;
    }

    LstSq<Cx> r = lstsq<Cx>(&alloc, a, bv);
    CHECK(r.rank == k_n);
    for (crd::usize j = 0; j < k_n; ++j)
    {
        CHECK_THAT(r.x.at(j, 0).re, WithinAbs(x_true[j].re, 1e-9));
        CHECK_THAT(r.x.at(j, 0).im, WithinAbs(x_true[j].im, 1e-9));
    }
    CHECK_THAT(r.residual(0), WithinAbs(0.0, 1e-9));
}

TEST_CASE("pinv: complex Moore-Penrose A A+ A == A", "[hesap][pinv][complex]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    using Cx = Complex<double>;
    constexpr crd::usize k_m = 5;
    constexpr crd::usize k_n = 3;
    Matrix<Cx, Layout::RowMajor> a(&alloc, k_m, k_n);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = Cx{std::sin(static_cast<double>(i * 3 + j) * 0.4) + (i == j ? 2.0 : 0.0),
                            std::cos(static_cast<double>(i + j * 2) * 0.3)};
        }
    }
    Matrix<Cx> p = pinv<Cx>(&alloc, a);  // n x m
    double max_err = 0.0;
    // Compute A A⁺ (m x m) then (A A⁺) A and compare to A.
    Matrix<Cx> aap(&alloc, k_m, k_m);
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < k_m; ++j)
        {
            Cx s{0.0, 0.0};
            for (crd::usize k = 0; k < k_n; ++k)
            {
                s = s + a.at(i, k) * p.at(k, j);
            }
            aap.at(i, j) = s;
        }
    }
    for (crd::usize i = 0; i < k_m; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            Cx s{0.0, 0.0};
            for (crd::usize k = 0; k < k_m; ++k)
            {
                s = s + aap.at(i, k) * a.at(k, j);
            }
            max_err = std::max(max_err, crd::hesap::abs(s - a.at(i, j)));
        }
    }
    REQUIRE(max_err < 1e-9);
}

TEST_CASE("lstsq CLI: hesap.dense.lstsq.f64 returns the solution", "[hesap][lstsq][cli]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(2U * 1024U * 1024U));
    const auto* cmd = crd::hesap::cli::CommandRegistry::global().find("hesap.dense.lstsq.f64");
    REQUIRE(cmd != nullptr);

    // A = [[1,0],[0,1],[1,1]] (3x2), b = [1, 2, 3] → LS solution [1, 2].
    crd::hesap::cli::CommandArgs args{&alloc};
    args.set_u64("m", 3);
    args.set_u64("n", 2);
    crd::containers::Array<crd::f64> aflat(&alloc);
    aflat.resize(6);
    aflat[0] = 1.0; aflat[1] = 0.0;
    aflat[2] = 0.0; aflat[3] = 1.0;
    aflat[4] = 1.0; aflat[5] = 1.0;
    args.set_f64_array("A", crd::containers::ConstSpan<crd::f64>{aflat.data(), aflat.size()});
    crd::containers::Array<crd::f64> bflat(&alloc);
    bflat.resize(3);
    bflat[0] = 1.0; bflat[1] = 2.0; bflat[2] = 3.0;
    args.set_f64_array("b", crd::containers::ConstSpan<crd::f64>{bflat.data(), bflat.size()});

    auto res = cmd->impl(args);
    REQUIRE(res.ok);
    const auto* blob = std::get_if<crd::hesap::cli::ResultBinaryBlob>(&res.value);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == 2 * sizeof(crd::f64));
    const auto* x = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    // Normal equations: AᵀA = [[2,1],[1,2]], Aᵀb = [4,5] → x = [1, 2].
    CHECK_THAT(x[0], WithinAbs(1.0, 1e-9));
    CHECK_THAT(x[1], WithinAbs(2.0, 1e-9));
}
