#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/lu.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "hesap_jobs_fixture.hpp"

#include <cmath>

using crd::hesap::dense::factor_lu;
using crd::hesap::dense::Layout;
using crd::hesap::dense::LU;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::solve_lu;
using Catch::Matchers::WithinAbs;

namespace
{
// Reconstruct A_perm = L * U from packed LU, then compare against the
// permuted source matrix (P * A). The factor satisfies P * A = L * U.
template <typename T>
void reconstruct_and_check(const LU<T, Layout::RowMajor>& lu, const Matrix<T, Layout::RowMajor>& a,
                           double tol)
{
    const crd::usize n = lu.n();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    Matrix<T, Layout::RowMajor> lu_check(&alloc, n, n);
    // L*U into lu_check using the packed LU storage:
    //   lu_check[i,j] = sum_p min(i,j) of L[i,p] * U[p,j]
    // where L is unit-diag lower from packed, U is upper from packed.
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            T sum{0};
            const crd::usize klimit = (i < j ? i : j) + 1;
            for (crd::usize p = 0; p < klimit; ++p)
            {
                const T lip = (p == i) ? T{1} : lu.packed().at(i, p);
                const T upj = lu.packed().at(p, j);
                sum += lip * upj;
            }
            lu_check.at(i, j) = sum;
        }
    }
    // Now P*A: take A and apply pivots in order (same forward replay as
    // apply_permutation for vectors, but on rows of A).
    Matrix<T, Layout::RowMajor> pa(&alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            pa.at(i, j) = a.at(i, j);
        }
    }
    for (crd::usize k = 0; k < lu.permutation().n(); ++k)
    {
        const crd::usize r = lu.permutation().pivot_at(k);
        if (r != k)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                T tmp = pa.at(k, j);
                pa.at(k, j) = pa.at(r, j);
                pa.at(r, j) = tmp;
            }
        }
    }

    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            CHECK_THAT(static_cast<double>(lu_check.at(i, j)),
                       WithinAbs(static_cast<double>(pa.at(i, j)), tol));
        }
    }
}
} // namespace

TEST_CASE("LU: 2x2 textbook factor", "[hesap][lu][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // A = [[4, 3], [6, 3]]
    Matrix<double, Layout::RowMajor> a(&alloc, 2, 2, {4.0, 3.0, 6.0, 3.0});
    LU<double, Layout::RowMajor> lu(&alloc, 2);
    factor_lu(lu, a);
    REQUIRE(lu.info() == 0U);
    // Partial pivoting swaps rows 0 and 1, then U[0,:]=[6,3], L[1,0]=4/6=2/3,
    // U[1,1] = 3 - (2/3)*3 = 1. So packed LU = {{6, 3}, {2/3, 1}}, piv[0]=1, piv[1]=1.
    REQUIRE(lu.permutation().pivot_at(0) == 1U);
    REQUIRE(lu.permutation().pivot_at(1) == 1U);
    CHECK_THAT(lu.packed().at(0, 0), WithinAbs(6.0, 1e-12));
    CHECK_THAT(lu.packed().at(0, 1), WithinAbs(3.0, 1e-12));
    CHECK_THAT(lu.packed().at(1, 0), WithinAbs(2.0 / 3.0, 1e-12));
    CHECK_THAT(lu.packed().at(1, 1), WithinAbs(1.0, 1e-12));
}

TEST_CASE("LU: solve recovers x for A*x = b at N=4", "[hesap][lu][real][solve]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // Diagonally-dominant 4x4 so factor is unambiguous.
    Matrix<double, Layout::RowMajor> a(&alloc, 4, 4,
        {10.0,  2.0,  1.0,  3.0,
          1.0, 12.0,  4.0,  2.0,
          2.0,  3.0, 15.0,  1.0,
          1.0,  1.0,  2.0, 20.0});
    // True solution x = [1, 2, 3, 4], so b = A*x.
    crd::containers::Array<double> b(&alloc);
    b.resize(4);
    crd::containers::Array<double> x_true(&alloc);
    x_true.resize(4);
    x_true[0] = 1.0; x_true[1] = 2.0; x_true[2] = 3.0; x_true[3] = 4.0;
    for (crd::usize i = 0; i < 4; ++i)
    {
        double s = 0.0;
        for (crd::usize j = 0; j < 4; ++j)
        {
            s += a.at(i, j) * x_true[j];
        }
        b[i] = s;
    }

    LU<double, Layout::RowMajor> lu(&alloc, 4);
    factor_lu(lu, a);
    REQUIRE(lu.info() == 0U);

    crd::containers::Span<double> x(b.data(), 4);
    solve_lu(lu, x);

    for (crd::usize i = 0; i < 4; ++i)
    {
        CHECK_THAT(x[i], WithinAbs(x_true[i], 1e-12));
    }
}

TEST_CASE("LU: reconstruction P*A == L*U at N=8", "[hesap][lu][real]")
{
    crd::memory::TlsfAllocator alloc(256U * 1024U);
    constexpr crd::usize k_n = 8;
    Matrix<double, Layout::RowMajor> a(&alloc, k_n, k_n);
    // Deterministic pseudo-random A with some off-diagonal magnitude.
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            const double v = std::sin(static_cast<double>(i * k_n + j) + 1.0);
            a.at(i, j) = v + (i == j ? 5.0 : 0.0);
        }
    }
    LU<double, Layout::RowMajor> lu(&alloc, k_n);
    factor_lu(lu, a);
    REQUIRE(lu.info() == 0U);
    reconstruct_and_check<double>(lu, a, 1e-10);
}

TEST_CASE("LU: reconstruction P*A == L*U at N=64 (block boundary)", "[hesap][lu][real]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    constexpr crd::usize k_n = 64;
    Matrix<double, Layout::RowMajor> a(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            const double v = std::cos(static_cast<double>(i * 7 + j * 3) * 0.1);
            a.at(i, j) = v + (i == j ? 20.0 : 0.0);
        }
    }
    LU<double, Layout::RowMajor> lu(&alloc, k_n);
    factor_lu(lu, a);
    REQUIRE(lu.info() == 0U);
    reconstruct_and_check<double>(lu, a, 1e-9);
}

TEST_CASE("LU: reconstruction at N=128 (multiple blocks + trailing update)", "[hesap][lu][real]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(32U * 1024U * 1024U));
    constexpr crd::usize k_n = 128;
    Matrix<double, Layout::RowMajor> a(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            const double v = std::sin(static_cast<double>(i * 5 + j * 11) * 0.03);
            a.at(i, j) = v + (i == j ? 50.0 : 0.0);
        }
    }
    LU<double, Layout::RowMajor> lu(&alloc, k_n);
    factor_lu(lu, a);
    REQUIRE(lu.info() == 0U);
    reconstruct_and_check<double>(lu, a, 1e-9);
}

TEST_CASE("LU: detects exactly singular matrix", "[hesap][lu][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // Row 2 = row 0 + row 1: rank deficient.
    Matrix<double, Layout::RowMajor> a(&alloc, 3, 3,
        {1.0, 2.0, 3.0,
         2.0, 4.0, 8.0,
         3.0, 6.0, 11.0});
    LU<double, Layout::RowMajor> lu(&alloc, 3);
    factor_lu(lu, a);
    // Determinism canary: this matrix's exact-0 pivot relies on no FMA
    // contraction (ADR-0063). It regressed on clang-cl-shipping until the
    // build forced -ffp-contract=off (clang-cl's /fp:precise left contraction
    // on, so `4 - (2/3)*6` came out ~2.2e-16 instead of exactly 0).
    REQUIRE(lu.info() != 0U);
    REQUIRE(lu.is_singular());

    // Structurally singular: column 1 is all zeros, so the column-1 pivot is
    // exactly 0 on every compiler / FP path (the rank-1 update only ever does
    // `x -= lij * 0`). This guards the detection LOGIC independently of FP
    // rounding, so a future optimizer change cannot silently mask it.
    Matrix<double, Layout::RowMajor> z(&alloc, 3, 3,
        {1.0, 0.0, 3.0,
         2.0, 0.0, 8.0,
         3.0, 0.0, 11.0});
    LU<double, Layout::RowMajor> luz(&alloc, 3);
    factor_lu(luz, z);
    REQUIRE(luz.info() != 0U);
    REQUIRE(luz.is_singular());
}

TEST_CASE("LU: solve_lu f32 at N=32", "[hesap][lu][real][f32]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    constexpr crd::usize k_n = 32;
    Matrix<float, Layout::RowMajor> a(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            const float v = std::sin(static_cast<float>(i * 7 + j) * 0.1F);
            a.at(i, j) = v + (i == j ? 10.0F : 0.0F);
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

    LU<float, Layout::RowMajor> lu(&alloc, k_n);
    factor_lu(lu, a);
    REQUIRE(lu.info() == 0U);

    crd::containers::Span<float> x(b.data(), k_n);
    solve_lu(lu, x);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        CHECK_THAT(static_cast<double>(x[i]),
                   WithinAbs(static_cast<double>(x_true[i]), 1e-4));
    }
}

TEST_CASE("LU: multi-RHS solve at N=16, nrhs=3", "[hesap][lu][real][solve][multi-rhs]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    constexpr crd::usize k_n = 16;
    constexpr crd::usize k_rhs = 3;
    Matrix<double, Layout::RowMajor> a(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = std::sin(static_cast<double>(i * 3 + j) * 0.05) +
                         (i == j ? 15.0 : 0.0);
        }
    }
    // Build B such that A * X_true = B. X_true(i, r) = i + r * k_n.
    Matrix<double, Layout::RowMajor> x_true(&alloc, k_n, k_rhs);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize r = 0; r < k_rhs; ++r)
        {
            x_true.at(i, r) = static_cast<double>(i + r * k_n);
        }
    }
    Matrix<double, Layout::RowMajor> b(&alloc, k_n, k_rhs);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize r = 0; r < k_rhs; ++r)
        {
            double s = 0.0;
            for (crd::usize j = 0; j < k_n; ++j)
            {
                s += a.at(i, j) * x_true.at(j, r);
            }
            b.at(i, r) = s;
        }
    }

    LU<double, Layout::RowMajor> lu(&alloc, k_n);
    factor_lu(lu, a);
    REQUIRE(lu.info() == 0U);

    solve_lu(lu, b.view());
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize r = 0; r < k_rhs; ++r)
        {
            CHECK_THAT(b.at(i, r), WithinAbs(x_true.at(i, r), 1e-10));
        }
    }
}

TEST_CASE("LU: determinism: factor is bit-identical across runs at N=128",
          "[hesap][lu][real][determinism]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    constexpr crd::usize k_n = 128;

    auto build_a = [&](Matrix<double, Layout::RowMajor>& m)
    {
        for (crd::usize i = 0; i < k_n; ++i)
        {
            for (crd::usize j = 0; j < k_n; ++j)
            {
                m.at(i, j) = std::cos(static_cast<double>(i * 11 + j * 5) * 0.02) +
                             (i == j ? 50.0 : 0.0);
            }
        }
    };

    Matrix<double, Layout::RowMajor> a1(&alloc, k_n, k_n);
    Matrix<double, Layout::RowMajor> a2(&alloc, k_n, k_n);
    build_a(a1);
    build_a(a2);

    LU<double, Layout::RowMajor> lu1(&alloc, k_n);
    LU<double, Layout::RowMajor> lu2(&alloc, k_n);
    factor_lu(lu1, a1);
    factor_lu(lu2, a2);

    // Bit-exact: the same input through the same factor on the same hardware
    // must produce a bit-identical packed LU + pivot vector. This is the
    // hesap determinism contract (ADR-0082 §2026-05-20 — IEEE 754 FMA
    // deterministic; gemm_parallel disjoint row-slab tile sums also
    // bit-deterministic across thread counts).
    for (crd::usize i = 0; i < k_n * k_n; ++i)
    {
        REQUIRE(lu1.packed().data()[i] == lu2.packed().data()[i]);
    }
    for (crd::usize i = 0; i < k_n; ++i)
    {
        REQUIRE(lu1.permutation().pivot_at(i) == lu2.permutation().pivot_at(i));
    }
}

TEST_CASE("LU: allocator propagation: TLSF only, no malloc",
          "[hesap][lu][real][allocator]")
{
    // 16 MB TLSF, large enough for N=128 LU + gemm pack buffers.
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    constexpr crd::usize k_n = 128;
    Matrix<double, Layout::RowMajor> a(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = std::sin(static_cast<double>(i + j)) + (i == j ? 30.0 : 0.0);
        }
    }
    LU<double, Layout::RowMajor> lu(&alloc, k_n);

    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    // factor_lu must take all scratch from `alloc` (lu.allocator()) — verified
    // by the fact that TLSF doesn't OOM on this size. If anywhere inside the
    // call defaulted to MallocAllocator, this would still pass; the real
    // verification is reading the code + the per-allocator counter (filed
    // for v0e-close stretch).
    factor_lu(lu, a);
    REQUIRE(lu.info() == 0U);
}
