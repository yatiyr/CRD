// crd-hesap-sparse v1c-3 -- diag / scale_rows / triu / tril / submatrix tests.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstdint>

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
} // namespace

TEST_CASE("extract_diagonal returns A[i,i]", "[hesap][sparse][structural][diag]")
{
    crd::memory::TlsfAllocator alloc(2 << 20);
    sp::TripletBuilder<crd::f64> b(&alloc, 3, 3);
    b.add(0, 0, 5.0);
    b.add(1, 2, 9.0);  // off-diagonal
    b.add(2, 2, 7.0);
    auto a = b.compress();
    auto d = sp::extract_diagonal(a, &alloc);
    REQUIRE(d.size() == 3);
    CHECK(d[0] == 5.0);
    CHECK(d[1] == 0.0);  // (1,1) absent
    CHECK(d[2] == 7.0);
}

TEST_CASE("triu/tril partition the matrix (triu(k=1)+diag+tril(k=-1) == A)", "[hesap][sparse][structural][tri]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    auto a = random_csr<crd::f64>(&alloc, 50, 50, 6, 0x111);
    auto u = sp::triu(a, 0, &alloc);   // col >= row
    auto l = sp::tril(a, -1, &alloc);  // col < row
    // Every entry of A is in exactly one of {triu(0), tril(-1)}.
    for (crd::u32 i = 0; i < 50; ++i)
    {
        for (crd::u32 j = 0; j < 50; ++j)
        {
            const crd::f64 av = a.coeff(i, j);
            if (j >= i)
            {
                CHECK(u.coeff(i, j) == av);
                CHECK(l.coeff(i, j) == 0.0);
            }
            else
            {
                CHECK(l.coeff(i, j) == av);
                CHECK(u.coeff(i, j) == 0.0);
            }
        }
    }
}

TEST_CASE("scale_rows multiplies row i by d[i]", "[hesap][sparse][structural][scale]")
{
    crd::memory::TlsfAllocator alloc(2 << 20);
    auto a = random_csr<crd::f64>(&alloc, 20, 20, 5, 0x222);
    crd::containers::Array<crd::f64> d(&alloc);
    for (crd::u32 i = 0; i < 20; ++i)
    {
        d.push_back(static_cast<crd::f64>(i + 1));
    }
    auto c = sp::scale_rows<crd::f64>(crd::containers::ConstSpan<crd::f64>{d.data(), d.size()}, a, &alloc);
    CHECK(c.pattern().topology_hash == a.pattern().topology_hash);  // structure preserved
    for (crd::u32 i = 0; i < 20; ++i)
    {
        for (crd::u32 j = 0; j < 20; ++j)
        {
            CHECK(c.coeff(i, j) == (static_cast<crd::f64>(i + 1) * a.coeff(i, j)));
        }
    }
}

TEST_CASE("submatrix extracts a reindexed block", "[hesap][sparse][structural][slice]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    auto a = random_csr<crd::f64>(&alloc, 30, 30, 6, 0x333);
    auto s = sp::submatrix(a, 5, 15, 8, 20, &alloc);  // rows[5,15) cols[8,20)
    REQUIRE(s.rows() == 10);
    REQUIRE(s.cols() == 12);
    for (crd::u32 i = 0; i < 10; ++i)
    {
        for (crd::u32 j = 0; j < 12; ++j)
        {
            CHECK(s.coeff(i, j) == a.coeff(i + 5, j + 8));
        }
    }
}

TEST_CASE("structural ops work for complex + are deterministic", "[hesap][sparse][structural][types]")
{
    crd::memory::TlsfAllocator alloc(2 << 20);
    auto a  = random_csr<Complex64>(&alloc, 25, 25, 4, 0x444);
    auto u1 = sp::triu(a, 0, &alloc);
    auto u2 = sp::triu(a, 0, &alloc);
    REQUIRE(u1.nnz() == u2.nnz());
    for (crd::usize k = 0; k < u1.nnz(); ++k)
    {
        CHECK(u1.values().values[k].re == u2.values().values[k].re);
        CHECK(u1.values().values[k].im == u2.values().values[k].im);
    }
    auto d = sp::extract_diagonal(a, &alloc);
    CHECK(d.size() == 25);
}
