// The sparse LinearOps' crd-jobs picks (detail/operator_arms.hpp): every cut and forced job count gives the serial
// kernel's bits (SELL spmv, CSR spmm) for A x, the adjoint and the block apply, IEEE specials included with NaN
// payloads aside (ADR-0134 §7: a parallel body's operand order may pick another NaN's bits), and leaves every word
// outside the output alone.

#include "nan_aside.hpp"
#include "sparse_arm_fixture.hpp"
#include "sparse_jobs_fixture.hpp"

#include <crd/hesap/sparse/block_linear_op.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>

namespace sp = crd::hesap::sparse;

namespace
{
using crd_hesap_sparse_tests::arm_csr;
using crd_hesap_sparse_tests::arm_value;
using crd_hesap_sparse_tests::same_bytes;

constexpr crd::u32 kWorkers[] = {0, 1, 2, 4, 8, 16, 32};
constexpr crd::u32 kGuard = 5;               // words after each output
constexpr crd::u32 kGuardBits = 0x7FC0DEADU; // a NaN payload no kernel writes
constexpr crd::usize kNever = ~crd::usize{0};

template <typename T> T guard_value()
{
    using R = typename crd_hesap_sparse_tests::ArmReal<T>::type;
    R r{};
    if constexpr (sizeof(R) == 4)
    {
        std::memcpy(&r, &kGuardBits, sizeof(R));
    }
    else
    {
        const std::uint64_t bits = (std::uint64_t{0x7FF8DEAD} << 32) | kGuardBits;
        std::memcpy(&r, &bits, sizeof(R));
    }
    if constexpr (crd_hesap_sparse_tests::kArmComplex<T>)
    {
        return T{r, r};
    }
    else
    {
        return r;
    }
}

// An output of n words and kGuard guard words after it, every word the guard (so a word an apply skips stays NaN).
template <typename T> crd::containers::Array<T> guarded(crd::memory::IAllocator* alloc, crd::usize n)
{
    crd::containers::Array<T> y(alloc);
    y.resize(n + kGuard);
    for (crd::usize i = 0; i < y.size(); ++i)
    {
        y[i] = guard_value<T>();
    }
    return y;
}

template <typename T> bool guards_hold(const crd::containers::Array<T>& y, crd::usize from)
{
    const T g = guard_value<T>();
    for (crd::usize i = from; i < y.size(); ++i)
    {
        if (!same_bytes(&y[i], &g, 1))
        {
            return false;
        }
    }
    return true;
}

// Whether the n written words y[0, n) hold want's values, NaN payloads aside, and none of them still holds the guard.
template <typename T> bool same_written(const T* want, const T* y, crd::usize n)
{
    const T g = guard_value<T>();
    for (crd::usize i = 0; i < n; ++i)
    {
        if (same_bytes(&y[i], &g, 1))
        {
            UNSCOPED_INFO("word " << i << " was not written");
            return false;
        }
    }
    return crd::test::same_values_nan_aside(want, y, n);
}

template <typename T> crd::containers::Array<T> arm_vector(crd::memory::IAllocator* alloc, crd::usize n, bool specials)
{
    crd::containers::Array<T> x(alloc);
    x.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i] = arm_value<T>(static_cast<crd::u32>(i * 2654435761U + 17U), specials);
    }
    return x;
}

template <typename T> crd::containers::ConstSpan<T> head(const crd::containers::Array<T>& a, crd::usize n)
{
    return crd::containers::ConstSpan<T>{a.data(), n};
}

template <typename T> crd::containers::Span<T> head(crd::containers::Array<T>& a, crd::usize n)
{
    return crd::containers::Span<T>{a.data(), n};
}

// The three cuts: always on the jobs, never, and production's L2.
template <typename F> void for_each_pick(const F& check)
{
    for (const crd::usize cut : {crd::usize{0}, kNever, sp::sparse_resident_bytes()})
    {
        for (const crd::u32 workers : kWorkers)
        {
            INFO("cut " << cut << ", workers " << workers);
            check(sp::detail::OperatorPick{.parallel_min_bytes = cut, .workers = workers});
        }
    }
}

// The square op's A x and the least-squares op's A x and A^H y against the serial SELL spmv of the same matrices, and
// (finite inputs) against the CSR spmv.
template <typename T>
void check_sell_ops(const sp::SparseMatrix<T, sp::SparseFormat::Csr>& a, bool specials, crd::memory::IAllocator* alloc)
{
    const crd::u32 m = a.rows();
    const crd::u32 n = a.cols();
    const auto x = arm_vector<T>(alloc, n, specials);
    const auto xh = arm_vector<T>(alloc, m, specials);

    const auto sell = sp::to_sell<T>(a, alloc);
    auto want = guarded<T>(alloc, m);
    sp::spmv_sell<T>(T(1), sell, head(x, n), T(0), head(want, m));

    // A^H's serial bits come from the bytes-constructed op on the serial route.
    const sp::ParallelSpmvLeastSquaresOp<T> serial_ls(a, alloc, kNever);
    auto want_h = guarded<T>(alloc, n);
    REQUIRE(serial_ls.apply_adjoint(head(xh, m), head(want_h, n)));
    REQUIRE_FALSE(serial_ls.is_parallel());
    // The least-squares op's measure: the larger of A's and A^T's SELL (conjugation leaves the bytes alone).
    const crd::usize at_bytes = sp::to_sell<T>(sp::transpose<T>(a, alloc), alloc).stored_bytes();
    const crd::usize ls_bytes = sell.stored_bytes() > at_bytes ? sell.stored_bytes() : at_bytes;

    if (!specials)
    {
        const sp::SparseLinearOp<T> csr(a);
        auto csr_y = guarded<T>(alloc, m);
        auto csr_h = guarded<T>(alloc, n);
        REQUIRE(csr.apply(head(x, n), head(csr_y, m)));
        REQUIRE(csr.apply_adjoint(head(xh, m), head(csr_h, n)));
        CHECK(same_bytes(want.data(), csr_y.data(), m));
        CHECK(same_bytes(want_h.data(), csr_h.data(), n));
    }

    for_each_pick(
        [&](const sp::detail::OperatorPick& pick)
        {
            const sp::ParallelSpmvLeastSquaresOp<T> ls(a, alloc, pick);
            CHECK(ls.is_parallel() == (ls_bytes >= pick.parallel_min_bytes));
            auto y = guarded<T>(alloc, m);
            auto h = guarded<T>(alloc, n);
            REQUIRE(ls.apply(head(x, n), head(y, m)));
            REQUIRE(ls.apply_adjoint(head(xh, m), head(h, n)));
            CHECK(same_written(want.data(), y.data(), m));
            CHECK(same_written(want_h.data(), h.data(), n));
            CHECK(guards_hold(y, m));
            CHECK(guards_hold(h, n));
            if (m == n)
            {
                const sp::ParallelSparseLinearOp<T> op(a, alloc, pick);
                CHECK(op.is_parallel() == (sell.stored_bytes() >= pick.parallel_min_bytes));
                auto ys = guarded<T>(alloc, m);
                REQUIRE(op.apply(head(x, n), head(ys, m)));
                CHECK(same_written(want.data(), ys.data(), m));
                CHECK(guards_hold(ys, m));
            }
        });

    // The bytes constructor is the pick with the cut alone.
    if (m == n)
    {
        const sp::ParallelSparseLinearOp<T> by_bytes(a, alloc, crd::usize{0});
        const sp::ParallelSparseLinearOp<T> by_pick(a, alloc, sp::detail::OperatorPick{.parallel_min_bytes = 0});
        CHECK(by_bytes.is_parallel() == by_pick.is_parallel());
        auto yb = guarded<T>(alloc, m);
        auto yp = guarded<T>(alloc, m);
        REQUIRE(by_bytes.apply(head(x, n), head(yb, m)));
        REQUIRE(by_pick.apply(head(x, n), head(yp, m)));
        CHECK(same_bytes(yb.data(), yp.data(), yb.size()));
    }
}

// The block op's Y = A X (ldx and ldy past s) against serial spmm: every row's s columns, the padding columns (still
// the guard) and the guard words after Y.
template <typename T>
void check_spmm_op(const sp::SparseMatrix<T, sp::SparseFormat::Csr>& a, bool specials, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = a.rows();
    for (const crd::u32 s : {1U, 4U, 8U})
    {
        INFO("s " << s);
        const crd::u32 ldx = s + 1U;
        const crd::u32 ldy = s + 2U;
        const auto x = arm_vector<T>(alloc, static_cast<crd::usize>(n) * ldx, specials);
        const crd::usize ny = static_cast<crd::usize>(n) * ldy;
        auto want = guarded<T>(alloc, ny);
        sp::spmm<T>(T(1), a, x.data(), ldx, s, T(0), want.data(), ldy);
        for_each_pick(
            [&](const sp::detail::OperatorPick& pick)
            {
                const sp::ParallelSpmmLinearOp<T> op(a, pick);
                CHECK(op.is_parallel() == (a.nnz() * sizeof(T) >= pick.parallel_min_bytes));
                auto y = guarded<T>(alloc, ny);
                REQUIRE(op.apply_block(head(x, x.size()), ldx, head(y, ny), ldy, s));
                for (crd::u32 r = 0; r < n; ++r)
                {
                    const crd::usize row = static_cast<crd::usize>(r) * ldy;
                    INFO("row " << r);
                    CHECK(same_written(want.data() + row, y.data() + row, s));
                    CHECK(same_bytes(want.data() + row + s, y.data() + row + s, ldy - s));
                }
                CHECK(guards_hold(y, ny));
            });
        const sp::ParallelSpmmLinearOp<T> by_bytes(a, crd::usize{0});
        const sp::ParallelSpmmLinearOp<T> by_pick(a, sp::detail::OperatorPick{.parallel_min_bytes = 0});
        CHECK(by_bytes.is_parallel() == by_pick.is_parallel());
    }
}

template <typename T> void check_operator_arms(crd::memory::IAllocator* alloc)
{
    for (const bool specials : {false, true})
    {
        INFO("specials " << specials);
        // One row, slice tails around the SELL slice height, fewer rows than jobs, and a matrix past several slices.
        for (const crd::u32 n : {1U, 3U, 7U, 9U, 33U, 65U, 500U})
        {
            INFO("n " << n);
            const auto a = arm_csr<T>(alloc, n, n, 5, 0x0A11U ^ n, specials);
            check_sell_ops<T>(a, specials, alloc);
            check_spmm_op<T>(a, specials, alloc);
        }
        // The least-squares op's tall and wide shapes.
        check_sell_ops<T>(arm_csr<T>(alloc, 300, 170, 4, 0x7A11U, specials), specials, alloc);
        check_sell_ops<T>(arm_csr<T>(alloc, 90, 260, 6, 0x3D1EU, specials), specials, alloc);
    }
}
} // namespace

TEST_CASE("the sparse LinearOps' picks keep the serial kernels' bits at every cut and job count",
          "[hesap][sparse][operator][parallel][jobs][determinism]")
{
    crd_hesap_sparse_tests::sparse_jobs_listener();
    crd::memory::TlsfAllocator alloc(64U << 20);
    {
        INFO("f64");
        check_operator_arms<crd::f64>(&alloc);
    }
    {
        INFO("f32");
        check_operator_arms<crd::f32>(&alloc);
    }
    {
        INFO("c64");
        check_operator_arms<crd::hesap::Complex64>(&alloc);
    }
    {
        INFO("c32");
        check_operator_arms<crd::hesap::Complex32>(&alloc);
    }
}

TEST_CASE("the sparse LinearOps apply an empty matrix at every pick", "[hesap][sparse][operator][parallel][jobs]")
{
    crd_hesap_sparse_tests::sparse_jobs_listener();
    crd::memory::TlsfAllocator alloc(4U << 20);
    sp::TripletBuilder<crd::f64> b(&alloc, 0, 0);
    const auto a = b.compress();
    const crd::containers::ConstSpan<crd::f64> x{};
    for_each_pick(
        [&](const sp::detail::OperatorPick& pick)
        {
            const sp::ParallelSparseLinearOp<crd::f64> op(a, &alloc, pick);
            const sp::ParallelSpmvLeastSquaresOp<crd::f64> ls(a, &alloc, pick);
            const sp::ParallelSpmmLinearOp<crd::f64> block(a, pick);
            auto y = guarded<crd::f64>(&alloc, 0);
            CHECK(op.apply(x, head(y, 0)));
            CHECK(ls.apply(x, head(y, 0)));
            CHECK(ls.apply_adjoint(x, head(y, 0)));
            CHECK(block.apply_block(x, 1, head(y, 0), 1, 1));
            CHECK(guards_hold(y, 0));
        });
}
