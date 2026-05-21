// crd-hesap-sparse v1d-1 -- serial spgemm (Gustavson) tests.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstdint>

using Catch::Approx;
using crd::hesap::Complex64;

namespace sp = crd::hesap::sparse;

namespace
{
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
        for (crd::u32 k = 0; k < nnz_per_row; ++k)
        {
            b.add(r, next() % cols, static_cast<T>(1 + (next() % 9)) / static_cast<T>(7));
        }
    }
    return b.compress();
}

template <typename T>
crd::containers::ConstSpan<T> cspan(const crd::containers::Array<T>& a)
{
    return crd::containers::ConstSpan<T>{a.data(), a.size()};
}
template <typename T>
crd::containers::Span<T> mspan(crd::containers::Array<T>& a)
{
    return crd::containers::Span<T>{a.data(), a.size()};
}
} // namespace

TEST_CASE("spgemm matches the dense oracle (small)", "[hesap][sparse][spgemm]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    // A (m x k), B (k x n); dense oracle capped at modest sizes (O(m*k*n)).
    const crd::u32 dims[][3] = {{1, 1, 1}, {4, 3, 5}, {50, 40, 60}, {120, 90, 100}};
    for (const auto& d : dims)
    {
        const crd::u32 m = d[0];
        const crd::u32 k = d[1];
        const crd::u32 n = d[2];
        auto a = random_csr<crd::f64>(&alloc, m, k, 3, 0xA ^ (m * 7 + n));
        auto b = random_csr<crd::f64>(&alloc, k, n, 3, 0xB ^ (k * 11 + n));
        auto c = sp::spgemm(a, b, &alloc);
        REQUIRE(c.rows() == m);
        REQUIRE(c.cols() == n);
        for (crd::u32 i = 0; i < m; ++i)
        {
            for (crd::u32 j = 0; j < n; ++j)
            {
                crd::f64 expect = 0.0;
                for (crd::u32 kk = 0; kk < k; ++kk)
                {
                    expect += a.coeff(i, kk) * b.coeff(kk, j);
                }
                INFO("m=" << m << " i=" << i << " j=" << j);
                CHECK(c.coeff(i, j) == Approx(expect).margin(1e-12));
            }
        }
    }
}

TEST_CASE("spgemm: (A*B)*x == A*(B*x) to round-off (large)", "[hesap][sparse][spgemm][crosscheck]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    const crd::u32 m = 4000;
    const crd::u32 k = 3000;
    const crd::u32 n = 3500;
    auto a  = random_csr<crd::f64>(&alloc, m, k, 8, 0x111);
    auto b  = random_csr<crd::f64>(&alloc, k, n, 8, 0x222);
    auto ab = sp::spgemm(a, b, &alloc);

    crd::containers::Array<crd::f64> x(&alloc);
    x.reserve(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x.push_back(static_cast<crd::f64>(1 + (i % 13)) / 7.0);
    }
    // lhs = (A*B)*x
    crd::containers::Array<crd::f64> lhs(&alloc);
    lhs.resize(m);
    sp::spmv<crd::f64>(1.0, ab, sp::Trans::None, cspan(x), 0.0, mspan(lhs));
    // rhs = A*(B*x)
    crd::containers::Array<crd::f64> bx(&alloc);
    bx.resize(k);
    sp::spmv<crd::f64>(1.0, b, sp::Trans::None, cspan(x), 0.0, mspan(bx));
    crd::containers::Array<crd::f64> rhs(&alloc);
    rhs.resize(m);
    sp::spmv<crd::f64>(1.0, a, sp::Trans::None, cspan(bx), 0.0, mspan(rhs));

    crd::f64 max_rel = 0.0;
    for (crd::u32 i = 0; i < m; ++i)
    {
        const crd::f64 denom = std::abs(rhs[i]) + 1e-30;
        max_rel = std::max(max_rel, std::abs(lhs[i] - rhs[i]) / denom);
    }
    CHECK(max_rel < 1e-10);
}

TEST_CASE("spgemm output is canonical column-sorted + deterministic", "[hesap][sparse][spgemm][determinism]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    auto a  = random_csr<crd::f64>(&alloc, 200, 150, 6, 0x55);
    auto b  = random_csr<crd::f64>(&alloc, 150, 180, 6, 0x66);
    auto c1 = sp::spgemm(a, b, &alloc);
    auto c2 = sp::spgemm(a, b, &alloc);
    REQUIRE(c1.nnz() == c2.nnz());
    // Columns sorted within each row.
    for (crd::u32 r = 0; r < c1.rows(); ++r)
    {
        const auto& pat = c1.pattern();
        for (crd::u32 t = pat.outer_ptr[r] + 1; t < pat.outer_ptr[r + 1]; ++t)
        {
            CHECK(pat.inner_idx[t - 1] < pat.inner_idx[t]);
        }
    }
    for (crd::usize i = 0; i < c1.nnz(); ++i)
    {
        CHECK(c1.values().values[i] == c2.values().values[i]);  // bit-exact run-to-run
        CHECK(c1.pattern().inner_idx[i] == c2.pattern().inner_idx[i]);
    }
}

TEST_CASE("spgemm f32 + identity sanity", "[hesap][sparse][spgemm][types]")
{
    crd::memory::TlsfAllocator alloc(2 << 20);
    // A * I == A (I is k x k identity).
    auto a = random_csr<crd::f32>(&alloc, 30, 25, 4, 0x7);
    sp::TripletBuilder<crd::f32> ib(&alloc, 25, 25);
    for (crd::u32 d = 0; d < 25; ++d)
    {
        ib.add(d, d, 1.0F);
    }
    auto id = ib.compress();
    auto c  = sp::spgemm(a, id, &alloc);
    for (crd::u32 i = 0; i < 30; ++i)
    {
        for (crd::u32 j = 0; j < 25; ++j)
        {
            CHECK(c.coeff(i, j) == a.coeff(i, j));
        }
    }
}

TEST_CASE("spgemm complex matches the dense oracle", "[hesap][sparse][spgemm][complex]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 m = 30;
    const crd::u32 k = 25;
    const crd::u32 n = 35;
    auto a = random_csr<Complex64>(&alloc, m, k, 3, 0xC1);
    auto b = random_csr<Complex64>(&alloc, k, n, 3, 0xC2);
    auto c = sp::spgemm(a, b, &alloc);
    for (crd::u32 i = 0; i < m; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            Complex64 expect{0.0, 0.0};
            for (crd::u32 kk = 0; kk < k; ++kk)
            {
                expect = expect + a.coeff(i, kk) * b.coeff(kk, j);
            }
            const Complex64 got = c.coeff(i, j);
            CHECK(got.re == Approx(expect.re).margin(1e-12));
            CHECK(got.im == Approx(expect.im).margin(1e-12));
        }
    }
}

TEST_CASE("spgemm_ata = A*transpose(A) and is symmetric (real)", "[hesap][sparse][spgemm][ata]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    auto a   = random_csr<crd::f64>(&alloc, 80, 60, 5, 0xA7);
    auto ata = sp::spgemm_ata(a, &alloc);
    REQUIRE(ata.rows() == 80);
    REQUIRE(ata.cols() == 80);
    // Equals the explicit compose.
    auto at  = sp::transpose(a, &alloc);
    auto ref = sp::spgemm(a, at, &alloc);
    REQUIRE(ata.nnz() == ref.nnz());
    for (crd::usize i = 0; i < ata.nnz(); ++i)
    {
        CHECK(ata.values().values[i] == ref.values().values[i]);
    }
    // A*A^T is symmetric.
    for (crd::u32 i = 0; i < 80; ++i)
    {
        for (crd::u32 j = 0; j < 80; ++j)
        {
            CHECK(ata.coeff(i, j) == Approx(ata.coeff(j, i)).margin(1e-12));
        }
    }
}

TEST_CASE("spgemm_ata complex (non-conjugating) sanity", "[hesap][sparse][spgemm][ata][complex]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    auto a   = random_csr<Complex64>(&alloc, 40, 30, 4, 0xB8);
    auto ata = sp::spgemm_ata(a, &alloc);
    auto at  = sp::transpose(a, &alloc);  // non-conjugating transpose
    auto ref = sp::spgemm(a, at, &alloc);
    REQUIRE(ata.nnz() == ref.nnz());
    for (crd::usize i = 0; i < ata.nnz(); ++i)
    {
        CHECK(ata.values().values[i].re == ref.values().values[i].re);
        CHECK(ata.values().values[i].im == ref.values().values[i].im);
    }
}
