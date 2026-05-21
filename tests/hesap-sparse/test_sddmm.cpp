// crd-hesap-sparse v1e-2 -- SDDMM + complex spmm/sddmm tests.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "sparse_jobs_fixture.hpp"

#include <cstdint>

using Catch::Approx;
using crd::hesap::Complex64;
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

TEST_CASE("sddmm samples X*Y^T at the mask pattern", "[hesap][sparse][sddmm]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 m = 30;
    const crd::u32 n = 40;
    const crd::u32 r = 6;
    auto mask = random_csr<crd::f64>(&alloc, m, n, 4, 0x9A3B);

    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(static_cast<crd::usize>(m) * r);
    crd::containers::Array<crd::f64> y(&alloc);
    y.resize(static_cast<crd::usize>(n) * r);
    for (crd::u32 i = 0; i < x.size(); ++i)
    {
        x[i] = static_cast<crd::f64>(1 + (i % 7)) / 3.0;
    }
    for (crd::u32 i = 0; i < y.size(); ++i)
    {
        y[i] = static_cast<crd::f64>(1 + (i % 5)) / 4.0;
    }

    auto c = sp::sddmm<crd::f64>(mask, x.data(), r, y.data(), r, r, 2.0, &alloc);
    REQUIRE(c.pattern().topology_hash == mask.pattern().topology_hash);  // same pattern
    for (crd::u32 i = 0; i < m; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            if (mask.coeff(i, j) != 0.0)  // masked position
            {
                crd::f64 dot = 0.0;
                for (crd::u32 col = 0; col < r; ++col)
                {
                    dot += x[static_cast<crd::usize>(i) * r + col] * y[static_cast<crd::usize>(j) * r + col];
                }
                CHECK(c.coeff(i, j) == Approx(2.0 * dot).margin(1e-12));
            }
        }
    }
}

TEST_CASE("parallel sddmm is bit-exact with serial", "[hesap][sparse][sddmm][parallel]")
{
    jobs_listener();
    crd::memory::TlsfAllocator alloc(32 << 20);
    const crd::u32 m = 600;
    const crd::u32 n = 500;
    const crd::u32 r = 16;
    auto mask = random_csr<crd::f64>(&alloc, m, n, 8, 0x5151);
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(static_cast<crd::usize>(m) * r);
    crd::containers::Array<crd::f64> y(&alloc);
    y.resize(static_cast<crd::usize>(n) * r);
    for (crd::u32 i = 0; i < x.size(); ++i)
    {
        x[i] = static_cast<crd::f64>(1 + (i % 13)) / 11.0;
    }
    for (crd::u32 i = 0; i < y.size(); ++i)
    {
        y[i] = static_cast<crd::f64>(1 + (i % 17)) / 13.0;
    }
    auto cs = sp::sddmm<crd::f64>(mask, x.data(), r, y.data(), r, r, 1.0, &alloc);
    for (crd::u32 jobs : {1U, 2U, 4U, 8U, 16U})
    {
        auto cp = sp::sddmm_parallel<crd::f64>(mask, x.data(), r, y.data(), r, r, 1.0, &alloc, jobs);
        crd::jobs::frame_reset();
        REQUIRE(cp.nnz() == cs.nnz());
        for (crd::usize k = 0; k < cs.nnz(); ++k)
        {
            INFO("jobs=" << jobs << " k=" << k);
            CHECK(cp.values().values[k] == cs.values().values[k]);
        }
    }
}

TEST_CASE("spmm complex matches dense oracle", "[hesap][sparse][spmm][complex]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 m = 20;
    const crd::u32 k = 18;
    const crd::u32 r = 4;
    auto a = random_csr<Complex64>(&alloc, m, k, 3, 0xCC);
    crd::containers::Array<Complex64> b(&alloc);
    b.resize(static_cast<crd::usize>(k) * r);
    for (crd::u32 i = 0; i < b.size(); ++i)
    {
        b[i] = Complex64{static_cast<crd::f64>(1 + i % 5), static_cast<crd::f64>(i % 3)};
    }
    crd::containers::Array<Complex64> c(&alloc);
    c.resize(static_cast<crd::usize>(m) * r);
    sp::spmm<Complex64>(Complex64{1.0, 0.0}, a, b.data(), r, r, Complex64{0.0, 0.0}, c.data(), r);
    for (crd::u32 i = 0; i < m; ++i)
    {
        for (crd::u32 col = 0; col < r; ++col)
        {
            Complex64 expect{0.0, 0.0};
            for (crd::u32 kk = 0; kk < k; ++kk)
            {
                expect = expect + a.coeff(i, kk) * b[static_cast<crd::usize>(kk) * r + col];
            }
            const Complex64 got = c[static_cast<crd::usize>(i) * r + col];
            CHECK(got.re == Approx(expect.re).margin(1e-12));
            CHECK(got.im == Approx(expect.im).margin(1e-12));
        }
    }
}

TEST_CASE("sddmm complex (non-conjugating) sanity", "[hesap][sparse][sddmm][complex]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    const crd::u32 m = 15;
    const crd::u32 n = 15;
    const crd::u32 r = 3;
    auto mask = random_csr<Complex64>(&alloc, m, n, 4, 0xDD);
    crd::containers::Array<Complex64> x(&alloc);
    x.resize(static_cast<crd::usize>(m) * r);
    crd::containers::Array<Complex64> y(&alloc);
    y.resize(static_cast<crd::usize>(n) * r);
    for (crd::u32 i = 0; i < x.size(); ++i)
    {
        x[i] = Complex64{static_cast<crd::f64>(1 + i % 4), static_cast<crd::f64>(i % 2)};
    }
    for (crd::u32 i = 0; i < y.size(); ++i)
    {
        y[i] = Complex64{static_cast<crd::f64>(1 + i % 3), static_cast<crd::f64>(i % 2)};
    }
    auto c = sp::sddmm<Complex64>(mask, x.data(), r, y.data(), r, r, Complex64{1.0, 0.0}, &alloc);
    for (crd::u32 i = 0; i < m; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            if (mask.coeff(i, j).re != 0.0 || mask.coeff(i, j).im != 0.0)
            {
                Complex64 dot{0.0, 0.0};
                for (crd::u32 col = 0; col < r; ++col)
                {
                    dot = dot + x[static_cast<crd::usize>(i) * r + col] * y[static_cast<crd::usize>(j) * r + col];
                }
                const Complex64 got = c.coeff(i, j);
                CHECK(got.re == Approx(dot.re).margin(1e-12));
                CHECK(got.im == Approx(dot.im).margin(1e-12));
            }
        }
    }
}
