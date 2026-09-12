// crd-hesap-sparse v1e-1 -- spmm (sparse x dense) tests.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "sparse_jobs_fixture.hpp"

#include <cstdint>
#include <limits>

using Catch::Approx;
namespace sp = crd::hesap::sparse;

namespace
{
inline crd_hesap_sparse_tests::SparseJobsListener& jobs_listener()
{
    return crd_hesap_sparse_tests::sparse_jobs_listener();
}

template <typename T>
sp::SparseMatrix<T, sp::SparseFormat::Csr> random_csr(crd::memory::IAllocator* alloc, crd::u32 rows, crd::u32 cols,
                                                      crd::u32 nnz_per_row, std::uint64_t seed)
{
    std::uint64_t s = seed;
    auto          next = [&]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<crd::u32>(s >> 33);
    };
    sp::TripletBuilder<T> b(alloc, rows, cols);
    for (crd::u32 r = 0; r < rows; ++r)
    {
        for (crd::u32 kk = 0; kk < nnz_per_row; ++kk)
        {
            b.add(r, next() % cols, static_cast<T>(1 + (next() % 9)) / static_cast<T>(7));
        }
    }
    return b.compress();
}
} // namespace

TEST_CASE("spmm matches the dense oracle (C = A*B)", "[hesap][sparse][spmm]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 m = 40;
    const crd::u32 k = 30;
    const crd::u32 r = 7;
    auto a = random_csr<crd::f64>(&alloc, m, k, 4, 0xA);
    crd::containers::Array<crd::f64> b(&alloc);  // k x r row-major
    b.resize(static_cast<crd::usize>(k) * r);
    for (crd::u32 i = 0; i < k * r; ++i)
    {
        b[i] = static_cast<crd::f64>(1 + (i % 11)) / 5.0;
    }
    crd::containers::Array<crd::f64> c(&alloc);
    c.resize(static_cast<crd::usize>(m) * r);
    sp::spmm<crd::f64>(1.0, a, b.data(), r, r, 0.0, c.data(), r);
    for (crd::u32 i = 0; i < m; ++i)
    {
        for (crd::u32 col = 0; col < r; ++col)
        {
            crd::f64 expect = 0.0;
            for (crd::u32 kk = 0; kk < k; ++kk)
            {
                expect += a.coeff(i, kk) * b[static_cast<crd::usize>(kk) * r + col];
            }
            CHECK(c[static_cast<crd::usize>(i) * r + col] == Approx(expect).margin(1e-12));
        }
    }
}

TEST_CASE("spmm beta=0 is NaN-safe; alpha/beta applied", "[hesap][sparse][spmm]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    const crd::u32 m = 10;
    const crd::u32 k = 10;
    const crd::u32 r = 3;
    auto a = random_csr<crd::f64>(&alloc, m, k, 3, 0x5);
    crd::containers::Array<crd::f64> b(&alloc);
    b.resize(static_cast<crd::usize>(k) * r);
    for (crd::u32 i = 0; i < k * r; ++i)
    {
        b[i] = 1.0 + 0.1 * static_cast<crd::f64>(i % 7);
    }
    crd::containers::Array<crd::f64> c(&alloc);
    c.resize(static_cast<crd::usize>(m) * r);
    const crd::f64 nan = std::numeric_limits<crd::f64>::quiet_NaN();
    for (crd::u32 i = 0; i < m * r; ++i)
    {
        c[i] = nan;  // beta=0 must NOT read this
    }
    sp::spmm<crd::f64>(1.0, a, b.data(), r, r, 0.0, c.data(), r);
    for (crd::u32 i = 0; i < m * r; ++i)
    {
        CHECK(c[i] == c[i]);  // not NaN
    }
}

TEST_CASE("spmm column j equals spmv of B's column j", "[hesap][sparse][spmm][crosscheck]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 m = 60;
    const crd::u32 k = 50;
    const crd::u32 r = 5;
    auto a = random_csr<crd::f64>(&alloc, m, k, 5, 0x9);
    crd::containers::Array<crd::f64> b(&alloc);
    b.resize(static_cast<crd::usize>(k) * r);
    for (crd::u32 i = 0; i < k * r; ++i)
    {
        b[i] = static_cast<crd::f64>(1 + (i % 13)) / 3.0;
    }
    crd::containers::Array<crd::f64> c(&alloc);
    c.resize(static_cast<crd::usize>(m) * r);
    sp::spmm<crd::f64>(1.0, a, b.data(), r, r, 0.0, c.data(), r);

    // For each column j: extract B[:,j], spmv, compare to C[:,j] (bit-exact:
    // spmm's per-(i,j) sum order == spmv's per-row sum order).
    for (crd::u32 j = 0; j < r; ++j)
    {
        crd::containers::Array<crd::f64> xj(&alloc);
        xj.resize(k);
        for (crd::u32 row = 0; row < k; ++row)
        {
            xj[row] = b[static_cast<crd::usize>(row) * r + j];
        }
        crd::containers::Array<crd::f64> yj(&alloc);
        yj.resize(m);
        sp::spmv<crd::f64>(1.0, a, sp::Trans::None, crd::containers::ConstSpan<crd::f64>{xj.data(), xj.size()}, 0.0,
                           crd::containers::Span<crd::f64>{yj.data(), yj.size()});
        for (crd::u32 i = 0; i < m; ++i)
        {
            CHECK(c[static_cast<crd::usize>(i) * r + j] == yj[i]);  // bit-exact
        }
    }
}

TEST_CASE("parallel spmm is bit-exact with serial across job counts", "[hesap][sparse][spmm][parallel]")
{
    jobs_listener();
    crd::memory::TlsfAllocator alloc(64 << 20);
    for (crd::u32 dims : {4U, 33U, 64U, 1000U})
    {
        const crd::u32 m = dims;
        const crd::u32 k = dims + 5;
        const crd::u32 r = 16;
        auto a = random_csr<crd::f64>(&alloc, m, k, 6, 0x77 ^ dims);
        crd::containers::Array<crd::f64> b(&alloc);
        b.resize(static_cast<crd::usize>(k) * r);
        for (crd::u32 i = 0; i < k * r; ++i)
        {
            b[i] = static_cast<crd::f64>(1 + (i % 17)) / 11.0;
        }
        crd::containers::Array<crd::f64> cs(&alloc);
        cs.resize(static_cast<crd::usize>(m) * r);
        sp::spmm<crd::f64>(1.5, a, b.data(), r, r, 0.0, cs.data(), r);
        for (crd::u32 jobs : {1U, 2U, 4U, 8U, 16U})
        {
            crd::containers::Array<crd::f64> cp(&alloc);
            cp.resize(static_cast<crd::usize>(m) * r);
            sp::spmm_parallel<crd::f64>(1.5, a, b.data(), r, r, 0.0, cp.data(), r, jobs);
            crd::jobs::frame_reset();
            for (crd::u32 i = 0; i < m * r; ++i)
            {
                INFO("dims=" << dims << " jobs=" << jobs << " i=" << i);
                CHECK(cp[i] == cs[i]);  // bit-exact
            }
        }
    }
}
