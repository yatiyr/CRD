#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/cholesky.hpp>
#include <crd/hesap/dense/detail/factor_arms.hpp>
#include <crd/hesap/dense/detail/syrk_microkernel.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "hesap_jobs_fixture.hpp"
#include "random_matrix.hpp"

#include <cmath>
#include <cstring>
#include <limits>

using crd::hesap::dense::Cholesky;
using crd::hesap::dense::factor_cholesky;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::solve_cholesky;
using crd::hesap::dense::Symmetric;
using crd_hesap_dense_tests::random_spd;
using Catch::Matchers::WithinAbs;

namespace
{

// Reconstruct A_check = L * L^T from packed cholesky storage and compare
// against the input symmetric A.
template <typename T>
void reconstruct_and_check(const Cholesky<T, Layout::RowMajor>& chol, const Symmetric<T>& a,
                           double tol)
{
    const crd::usize n = chol.n();
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            T sum = T{0};
            const crd::usize klimit = (i < j ? i : j) + 1;
            for (crd::usize p = 0; p < klimit; ++p)
            {
                sum += chol.packed().at(i, p) * chol.packed().at(j, p);
            }
            CHECK_THAT(static_cast<double>(sum),
                       WithinAbs(static_cast<double>(a.at(i, j)), tol));
        }
    }
}
} // namespace

TEST_CASE("Cholesky: 2x2 textbook factor", "[hesap][cholesky][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // A = [[4, 12], [12, 37]]  →  L = [[2, 0], [6, 1]]  (L*L^T = A)
    Symmetric<double> a(&alloc, 2);
    a.at(0, 0) = 4.0;
    a.at(1, 0) = 12.0;
    a.at(1, 1) = 37.0;

    Cholesky<double, Layout::RowMajor> chol(&alloc, 2);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);
    CHECK_THAT(chol.packed().at(0, 0), WithinAbs(2.0, 1e-12));
    CHECK_THAT(chol.packed().at(1, 0), WithinAbs(6.0, 1e-12));
    CHECK_THAT(chol.packed().at(1, 1), WithinAbs(1.0, 1e-12));
}

TEST_CASE("Cholesky: solve recovers x for A*x = b at N=4",
          "[hesap][cholesky][real][solve]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // A = diag-dominant SPD: rows mirror via Symmetric storage.
    Symmetric<double> a(&alloc, 4);
    // Lower triangle (incl. diagonal):
    a.at(0, 0) = 25.0;
    a.at(1, 0) =  5.0;  a.at(1, 1) = 18.0;
    a.at(2, 0) =  2.0;  a.at(2, 1) =  3.0;  a.at(2, 2) = 30.0;
    a.at(3, 0) =  1.0;  a.at(3, 1) =  2.0;  a.at(3, 2) =  4.0;  a.at(3, 3) = 40.0;

    // True x = [1, 2, 3, 4], so b = A*x  (use full symmetric access).
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

    Cholesky<double, Layout::RowMajor> chol(&alloc, 4);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);

    crd::containers::Span<double> x(b.data(), 4);
    solve_cholesky(chol, x);

    for (crd::usize i = 0; i < 4; ++i)
    {
        CHECK_THAT(x[i], WithinAbs(x_true[i], 1e-12));
    }
}

TEST_CASE("Cholesky: reconstruction L*L^T == A at N=8",
          "[hesap][cholesky][real]")
{
    crd::memory::TlsfAllocator alloc(512U * 1024U);
    constexpr crd::usize k_n = 8;
    Symmetric<double> a(&alloc, k_n);
    random_spd<double>(a, 42U);
    Cholesky<double, Layout::RowMajor> chol(&alloc, k_n);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);
    reconstruct_and_check<double>(chol, a, 1e-9);
}

TEST_CASE("Cholesky: reconstruction at N=64 (single trailing update)",
          "[hesap][cholesky][real]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    constexpr crd::usize k_n = 64;
    Symmetric<double> a(&alloc, k_n);
    random_spd<double>(a, 1729U);
    Cholesky<double, Layout::RowMajor> chol(&alloc, k_n);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);
    reconstruct_and_check<double>(chol, a, 1e-7);
}

TEST_CASE("Cholesky: reconstruction at N=128 (multi-block)",
          "[hesap][cholesky][real]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(32U * 1024U * 1024U));
    constexpr crd::usize k_n = 128;
    Symmetric<double> a(&alloc, k_n);
    random_spd<double>(a, 314159U);
    Cholesky<double, Layout::RowMajor> chol(&alloc, k_n);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);
    reconstruct_and_check<double>(chol, a, 1e-7);
}

TEST_CASE("Cholesky: detects non-positive-definite", "[hesap][cholesky][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // Indefinite matrix: diagonal has a negative entry.
    Symmetric<double> a(&alloc, 3);
    a.at(0, 0) = 4.0;
    a.at(1, 0) = 1.0; a.at(1, 1) = -2.0;
    a.at(2, 0) = 1.0; a.at(2, 1) =  1.0; a.at(2, 2) = 3.0;

    Cholesky<double, Layout::RowMajor> chol(&alloc, 3);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() != 0U);
    REQUIRE(chol.is_singular());
}

TEST_CASE("Cholesky: f32 solve at N=32", "[hesap][cholesky][real][f32]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    constexpr crd::usize k_n = 32;
    Symmetric<float> a(&alloc, k_n);
    random_spd<float>(a, 7U);
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

    Cholesky<float, Layout::RowMajor> chol(&alloc, k_n);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);
    crd::containers::Span<float> x(b.data(), k_n);
    solve_cholesky(chol, x);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        CHECK_THAT(static_cast<double>(x[i]),
                   WithinAbs(static_cast<double>(x_true[i]), 1e-3));
    }
}

TEST_CASE("Cholesky: multi-RHS solve at N=16, nrhs=3",
          "[hesap][cholesky][real][solve][multi-rhs]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    constexpr crd::usize k_n = 16;
    constexpr crd::usize k_rhs = 3;
    Symmetric<double> a(&alloc, k_n);
    random_spd<double>(a, 999U);
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

    Cholesky<double, Layout::RowMajor> chol(&alloc, k_n);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);
    solve_cholesky(chol, b.view());
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize r = 0; r < k_rhs; ++r)
        {
            CHECK_THAT(b.at(i, r), WithinAbs(x_true.at(i, r), 1e-9));
        }
    }
}

TEST_CASE("Cholesky: determinism: factor is bit-identical across runs at N=128",
          "[hesap][cholesky][real][determinism]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    constexpr crd::usize k_n = 128;
    Symmetric<double> a1(&alloc, k_n);
    Symmetric<double> a2(&alloc, k_n);
    random_spd<double>(a1, 271828U);
    random_spd<double>(a2, 271828U);

    Cholesky<double, Layout::RowMajor> chol1(&alloc, k_n);
    Cholesky<double, Layout::RowMajor> chol2(&alloc, k_n);
    factor_cholesky(chol1, a1);
    factor_cholesky(chol2, a2);

    // Only check the LOWER triangle for bit-equality; the upper triangle
    // is GEMM-trailing-update garbage that we don't read on solve.
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            REQUIRE(chol1.packed().at(i, j) == chol2.packed().at(i, j));
        }
    }
}

TEST_CASE("Cholesky: allocator propagation: TLSF only, no malloc",
          "[hesap][cholesky][real][allocator]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    constexpr crd::usize k_n = 128;
    Symmetric<double> a(&alloc, k_n);
    random_spd<double>(a, 1U);
    Cholesky<double, Layout::RowMajor> chol(&alloc, k_n);
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);
}

namespace
{
using crd::hesap::dense::detail::CholeskyPick;
using crd::hesap::dense::detail::SyrkPick;

// A's lower triangle factored through factor_cholesky_arm(pick).
template <typename T>
void cholesky_with(const CholeskyPick& pick, const Symmetric<T>& a, Cholesky<T, Layout::RowMajor>& chol,
                   crd::memory::IAllocator* alloc)
{
    const crd::usize n = a.n();
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            chol.packed().at(i, j) = j <= i ? a.at(i, j) : T{0};
        }
    }
    crd::hesap::dense::detail::factor_cholesky_arm<T, Layout::RowMajor>(pick, chol, alloc);
}

// The factor's lower triangle (its contract) and info, bit for bit.
template <typename T> bool cholesky_same(const Cholesky<T, Layout::RowMajor>& x, const Cholesky<T, Layout::RowMajor>& y)
{
    if (x.info() != y.info())
    {
        return false;
    }
    for (crd::usize i = 0; i < x.n(); ++i)
    {
        if (std::memcmp(&x.packed().at(i, 0), &y.packed().at(i, 0), (i + 1) * sizeof(T)) != 0)
        {
            return false;
        }
    }
    return true;
}

// The backward error of a factor: |A - L L^T| <= 4 n eps (|L| |L^T|) elementwise (Higham 10.3, with room for the
// check's own sums).
template <typename T> bool cholesky_backward_stable(const Cholesky<T, Layout::RowMajor>& chol, const Symmetric<T>& a)
{
    const crd::usize n = chol.n();
    const double tol = 4.0 * static_cast<double>(n) * static_cast<double>(std::numeric_limits<T>::epsilon());
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            double sum = 0.0;
            double bound = 0.0;
            for (crd::usize p = 0; p <= j; ++p)
            {
                const double x = static_cast<double>(chol.packed().at(i, p)) * chol.packed().at(j, p);
                sum += x;
                bound += std::fabs(x);
            }
            if (std::fabs(static_cast<double>(a.at(i, j)) - sum) > tol * bound)
            {
                return false;
            }
        }
    }
    return true;
}

template <typename T> void check_cholesky_levers()
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    const CholeskyPick bit_picks[] = {
        CholeskyPick{},
        CholeskyPick{.trsm_min = 0},
        CholeskyPick{.trsm_min = ~crd::usize{0}, .syrk_mmk = 0},
        CholeskyPick{.trsm_min = 0, .syrk_mmk = ~crd::usize{0}},
    };
    // The cuts and blocks that regroup the sums: blocked at the default block and at 16, unblocked at every n.
    const CholeskyPick groups[] = {
        CholeskyPick{.unblocked_max = 0},
        CholeskyPick{.unblocked_max = 0, .block = 16},
        CholeskyPick{.unblocked_max = ~crd::usize{0}},
    };
    const crd::u32 jobs[] = {4U, 8U, 16U, 32U};
    // Inputs: SPD, IEEE specials (a NaN, -0 and a subnormal off the diagonal: whatever the NaN does to the factor and
    // its info, it does on every job count), and a negative diagonal at 70 (info 71).
    const crd::usize sizes[] = {1U, 63U, 64U, 100U, 300U};
    for (const crd::usize n : sizes)
    {
        for (int input = 0; input < 3; ++input)
        {
            if ((input == 1 && n < 64) || (input == 2 && n < 100))
            {
                continue;
            }
            Symmetric<T> a(&alloc, n);
            random_spd<T>(a, 3U + static_cast<crd::u32>(n));
            if (input == 1)
            {
                a.at(50, 3) = std::numeric_limits<T>::quiet_NaN();
                a.at(60, 1) = T{-0.0};
                a.at(62, 10) = std::numeric_limits<T>::min() / T{4};
            }
            if (input == 2)
            {
                a.at(70, 70) = T{-1000};
            }
            for (const CholeskyPick& group : groups)
            {
                Cholesky<T, Layout::RowMajor> ref(&alloc, n);
                CholeskyPick serial = group;
                serial.workers = 1;
                cholesky_with(serial, a, ref, &alloc);
                INFO("n " << n << " input " << input << " unblocked to " << group.unblocked_max << " block "
                          << group.block);
                if (input == 0)
                {
                    CHECK(ref.info() == 0U);
                    CHECK(cholesky_backward_stable(ref, a));
                }
                if (input == 2)
                {
                    CHECK(ref.info() == 71U);
                }
                for (const CholeskyPick& base : bit_picks)
                {
                    for (const crd::u32 w : jobs)
                    {
                        CholeskyPick pick = base;
                        pick.unblocked_max = group.unblocked_max;
                        pick.block = group.block;
                        pick.workers = w;
                        Cholesky<T, Layout::RowMajor> chol(&alloc, n);
                        cholesky_with(pick, a, chol, &alloc);
                        INFO("jobs " << w << " trsm from " << base.trsm_min << " syrk above " << base.syrk_mmk);
                        CHECK(cholesky_same(chol, ref));
                    }
                }
            }
        }
    }
}

// syrk_lower_minus split over its column panels at 4 to 32 jobs against one job, bit for bit over C's lower and
// upper triangles, its row padding and a guard ring after it.
template <typename T> void check_syrk_levers()
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    struct Shape
    {
        crd::usize m, k;
    };
    const Shape shapes[] = {{1U, 1U}, {5U, 3U}, {100U, 70U}, {300U, 300U}, {130U, 600U}};
    const T guard = static_cast<T>(-7.75);
    for (const Shape& sh : shapes)
    {
        Matrix<T, Layout::RowMajor> a(&alloc, sh.m, sh.k);
        crd_hesap_dense_tests::random_general(a, 11U + static_cast<crd::u32>(sh.m));
        if (sh.m >= 100)
        {
            a.at(7, 2) = std::numeric_limits<T>::quiet_NaN();
            a.at(40, 9) = std::numeric_limits<T>::infinity();
            a.at(41, 0) = T{-0.0};
            a.at(90, 5) = std::numeric_limits<T>::min() / T{4};
        }
        const crd::usize ld = sh.m + 3U;
        const crd::usize words = sh.m * ld + 16U;
        const auto inside = [&](crd::usize i)
        {
            return i < sh.m * ld && i % ld < sh.m;
        };
        crd::containers::Array<T> c(&alloc);
        c.resize(words);
        for (crd::usize i = 0; i < words; ++i)
        {
            c[i] = inside(i) ? static_cast<T>(static_cast<double>(i % 97) * 0.125) : guard;
        }
        crd::containers::Array<T> ref(&alloc);
        ref.resize(words);
        std::memcpy(ref.data(), c.data(), words * sizeof(T));
        crd::hesap::dense::detail::syrk_lower_minus<T, Layout::RowMajor>(
            a.cview(), crd::hesap::dense::MatrixView<T, Layout::RowMajor>(ref.data(), sh.m, sh.m, ld), &alloc,
            SyrkPick{.workers = 1});
        for (crd::usize i = 0; i < words; ++i)
        {
            if (!inside(i))
            {
                REQUIRE(std::memcmp(&ref[i], &guard, sizeof(T)) == 0);
            }
        }
        crd::containers::Array<T> out(&alloc);
        out.resize(words);
        for (const crd::u32 w : {4U, 8U, 16U, 32U})
        {
            std::memcpy(out.data(), c.data(), words * sizeof(T));
            crd::hesap::dense::detail::syrk_lower_minus<T, Layout::RowMajor>(
                a.cview(), crd::hesap::dense::MatrixView<T, Layout::RowMajor>(out.data(), sh.m, sh.m, ld), &alloc,
                SyrkPick{.parallel_mmk = 0, .workers = w});
            INFO("m " << sh.m << " k " << sh.k << " jobs " << w);
            CHECK(std::memcmp(out.data(), ref.data(), words * sizeof(T)) == 0);
        }
    }
}
} // namespace

TEST_CASE("factor_cholesky_arm: every jobs lever at 4 to 32 jobs gives the one-job factor's bits",
          "[hesap][cholesky][parallel][jobs]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    check_cholesky_levers<float>();
    check_cholesky_levers<double>();
}

TEST_CASE("syrk_lower_minus: the column panels split at 4 to 32 jobs give the one-job sweep's bits",
          "[hesap][cholesky][parallel][jobs][syrk]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    check_syrk_levers<float>();
    check_syrk_levers<double>();
}
