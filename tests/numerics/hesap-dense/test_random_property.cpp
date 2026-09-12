#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/cholesky.hpp>
#include <crd/hesap/dense/condition.hpp>
#include <crd/hesap/dense/ldlt.hpp>
#include <crd/hesap/dense/lu.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/qr.hpp>
#include <crd/hesap/dense/refinement.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "hesap_jobs_fixture.hpp"
#include "random_matrix.hpp"

#include <cmath>

// Property-based tests (v0f): for a spread of seeds × sizes from the
// RandomMatrix factory, assert the defining algebraic property of each
// factorization (reconstruct == input, or solve recovers x). This catches
// regressions a handful of textbook fixtures would miss.

using crd::hesap::dense::Cholesky;
using crd::hesap::dense::factor_cholesky;
using crd::hesap::dense::factor_ldlt;
using crd::hesap::dense::factor_lu;
using crd::hesap::dense::factor_qr;
using crd::hesap::dense::Layout;
using crd::hesap::dense::LDLT;
using crd::hesap::dense::LU;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::QR;
using crd::hesap::dense::solve_ldlt;
using crd::hesap::dense::solve_lu;
using crd::hesap::dense::solve_qr;
using crd::hesap::dense::Symmetric;
using crd::hesap::dense::condition_estimate_1norm_symmetric;
using crd::hesap::dense::refine_cholesky;
using crd_hesap_dense_tests::random_diag_dominant;
using crd_hesap_dense_tests::random_spd;
using crd_hesap_dense_tests::random_spd_ill_conditioned;
using crd_hesap_dense_tests::random_symmetric_indefinite;

namespace
{
// Build b = A·x_true for a general matrix; returns matched x_true (= i+1).
template <typename T>
void make_rhs_general(const Matrix<T, Layout::RowMajor>& a, crd::containers::Array<T>& x_true,
                      crd::containers::Array<T>& b)
{
    const crd::usize n = a.rows();
    x_true.resize(n);
    b.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x_true[i] = static_cast<T>(i + 1) * static_cast<T>(0.5);
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        T s = T{0};
        for (crd::usize j = 0; j < n; ++j)
        {
            s += a.at(i, j) * x_true[j];
        }
        b[i] = s;
    }
}

template <typename T>
void make_rhs_symmetric(const Symmetric<T>& a, crd::containers::Array<T>& x_true,
                        crd::containers::Array<T>& b)
{
    const crd::usize n = a.n();
    x_true.resize(n);
    b.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x_true[i] = static_cast<T>(i + 1) * static_cast<T>(0.5);
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        T s = T{0};
        for (crd::usize j = 0; j < n; ++j)
        {
            s += a.at(i, j) * x_true[j];
        }
        b[i] = s;
    }
}
} // namespace

TEST_CASE("property: Cholesky reconstructs random SPD across seeds/sizes",
          "[hesap][property][cholesky]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    for (crd::usize n : {crd::usize{5}, crd::usize{17}, crd::usize{64}, crd::usize{129}})
    {
        for (crd::u32 seed : {1U, 7U, 31337U})
        {
            Symmetric<double> a(&alloc, n);
            random_spd(a, seed);
            Cholesky<double, Layout::RowMajor> chol(&alloc, n);
            factor_cholesky(chol, a);
            INFO("n=" << n << " seed=" << seed);
            REQUIRE(chol.info() == 0U);
            // L·Lᵀ == A (lower triangle).
            double max_err = 0.0;
            for (crd::usize i = 0; i < n; ++i)
            {
                for (crd::usize j = 0; j <= i; ++j)
                {
                    double s = 0.0;
                    for (crd::usize p = 0; p <= j; ++p)
                    {
                        s += chol.packed().at(i, p) * chol.packed().at(j, p);
                    }
                    const double d = std::abs(s - a.at(i, j));
                    if (d > max_err) max_err = d;
                }
            }
            REQUIRE(max_err < 1e-7);
        }
    }
}

TEST_CASE("property: LU solve recovers x for random diag-dominant",
          "[hesap][property][lu]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    for (crd::usize n : {crd::usize{4}, crd::usize{23}, crd::usize{80}, crd::usize{129}})
    {
        for (crd::u32 seed : {2U, 99U, 271828U})
        {
            Matrix<double, Layout::RowMajor> a(&alloc, n, n);
            random_diag_dominant(a, seed);
            crd::containers::Array<double> x_true(&alloc);
            crd::containers::Array<double> b(&alloc);
            make_rhs_general(a, x_true, b);

            LU<double, Layout::RowMajor> lu(&alloc, n);
            factor_lu(lu, a);
            INFO("n=" << n << " seed=" << seed);
            REQUIRE(lu.info() == 0U);
            solve_lu(lu, crd::containers::Span<double>(b.data(), n));
            for (crd::usize i = 0; i < n; ++i)
            {
                CHECK(std::abs(b[i] - x_true[i]) < 1e-9);
            }
        }
    }
}

TEST_CASE("property: LDLT solve recovers x for random symmetric-indefinite",
          "[hesap][property][ldlt]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    for (crd::usize n : {crd::usize{4}, crd::usize{16}, crd::usize{48}})
    {
        for (crd::u32 seed : {3U, 123U, 99999U})
        {
            Symmetric<double> a(&alloc, n);
            random_symmetric_indefinite(a, seed);
            crd::containers::Array<double> x_true(&alloc);
            crd::containers::Array<double> b(&alloc);
            make_rhs_symmetric(a, x_true, b);

            LDLT<double, Layout::RowMajor> ldlt(&alloc, n);
            factor_ldlt(ldlt, a);
            INFO("n=" << n << " seed=" << seed);
            if (ldlt.is_singular())
            {
                continue;  // a random indefinite draw can be singular; skip
            }
            solve_ldlt(ldlt, crd::containers::Span<double>(b.data(), n));
            for (crd::usize i = 0; i < n; ++i)
            {
                CHECK(std::abs(b[i] - x_true[i]) < 1e-7);
            }
        }
    }
}

TEST_CASE("property: QR solve recovers x for random diag-dominant square",
          "[hesap][property][qr]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    for (crd::usize n : {crd::usize{4}, crd::usize{20}, crd::usize{65}, crd::usize{130}})
    {
        for (crd::u32 seed : {5U, 444U, 80808U})
        {
            Matrix<double, Layout::RowMajor> a(&alloc, n, n);
            random_diag_dominant(a, seed);
            crd::containers::Array<double> x_true(&alloc);
            crd::containers::Array<double> b(&alloc);
            make_rhs_general(a, x_true, b);

            QR<double, Layout::RowMajor> qr(&alloc, n, n);
            factor_qr(qr, a);
            INFO("n=" << n << " seed=" << seed);
            solve_qr(qr, crd::containers::Span<double>(b.data(), n));
            for (crd::usize i = 0; i < n; ++i)
            {
                CHECK(std::abs(b[i] - x_true[i]) < 1e-8);
            }
        }
    }
}

TEST_CASE("property: ill-conditioned SPD -> condition estimate >= 1 + refinement does not worsen",
          "[hesap][property][condition][refinement]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    for (crd::usize n : {crd::usize{8}, crd::usize{32}, crd::usize{96}})
    {
        for (double kappa : {1.0e2, 1.0e4})
        {
            for (crd::u32 seed : {6U, 2024U})
            {
                Symmetric<double> a(&alloc, n);
                random_spd_ill_conditioned(a, seed, kappa);
                Cholesky<double, Layout::RowMajor> chol(&alloc, n);
                factor_cholesky(chol, a);
                INFO("n=" << n << " kappa=" << kappa << " seed=" << seed);
                REQUIRE(chol.info() == 0U);  // scaling keeps it PD

                const double cond = condition_estimate_1norm_symmetric(a, chol, &alloc);
                CHECK(cond >= 1.0);

                crd::containers::Array<double> x_true(&alloc);
                crd::containers::Array<double> b(&alloc);
                make_rhs_symmetric(a, x_true, b);
                crd::containers::Array<double> x(&alloc);
                x.resize(n);
                for (crd::usize i = 0; i < n; ++i)
                {
                    x[i] = 0.0;
                }
                const auto res = refine_cholesky(a, chol,
                                                 crd::containers::ConstSpan<double>(b.data(), n),
                                                 crd::containers::Span<double>(x.data(), n));
                // Refinement must never make the relative residual worse.
                CHECK(res.final_rel_residual <= res.initial_rel_residual + 1e-15);
            }
        }
    }
}
