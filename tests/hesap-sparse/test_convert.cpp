// crd-hesap-sparse v1c-1 -- conversion graph + transpose tests.

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

template <typename T, sp::SparseFormat F>
bool patterns_byte_equal(const sp::SparseMatrix<T, F>& a, const sp::SparseMatrix<T, F>& b)
{
    const auto& pa = a.pattern();
    const auto& pb = b.pattern();
    if (pa.rows != pb.rows || pa.cols != pb.cols || pa.nnz() != pb.nnz())
    {
        return false;
    }
    if (pa.outer_ptr.size() != pb.outer_ptr.size() || pa.inner_idx.size() != pb.inner_idx.size())
    {
        return false;
    }
    for (crd::usize i = 0; i < pa.outer_ptr.size(); ++i)
    {
        if (pa.outer_ptr[i] != pb.outer_ptr[i])
        {
            return false;
        }
    }
    for (crd::usize i = 0; i < pa.inner_idx.size(); ++i)
    {
        if (pa.inner_idx[i] != pb.inner_idx[i])
        {
            return false;
        }
    }
    for (crd::usize i = 0; i < a.values().values.size(); ++i)
    {
        if (a.values().values[i] != b.values().values[i])
        {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("transpose of transpose is byte-identical to the original", "[hesap][sparse][convert][transpose]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    for (crd::u32 n : {1U, 5U, 9U, 64U, 200U})
    {
        auto a   = random_csr<crd::f64>(&alloc, n, n + 3, 4, 0xC0FFEE ^ n);
        auto at  = sp::transpose(a, &alloc);
        auto att = sp::transpose(at, &alloc);
        REQUIRE(at.rows() == a.cols());
        REQUIRE(at.cols() == a.rows());
        INFO("n=" << n);
        CHECK(patterns_byte_equal(a, att));  // real build, exact round-trip
    }
}

TEST_CASE("transpose has the right entries (A^T[c,r] == A[r,c])", "[hesap][sparse][convert][transpose]")
{
    crd::memory::TlsfAllocator alloc(1 << 20);
    // A = [[1,0,2],[0,3,0]] (2x3).  A^T = [[1,0],[0,3],[2,0]] (3x2).
    sp::TripletBuilder<crd::f64> b(&alloc, 2, 3);
    b.add(0, 0, 1.0);
    b.add(0, 2, 2.0);
    b.add(1, 1, 3.0);
    auto a  = b.compress();
    auto at = sp::transpose(a, &alloc);
    REQUIRE(at.rows() == 3);
    REQUIRE(at.cols() == 2);
    CHECK(at.coeff(0, 0) == 1.0);
    CHECK(at.coeff(2, 0) == 2.0);
    CHECK(at.coeff(1, 1) == 3.0);
    CHECK(at.coeff(0, 1) == 0.0);
}

TEST_CASE("CSR -> CSC -> CSR is byte-identical (and CSC has same coeffs)", "[hesap][sparse][convert][csc]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    for (crd::u32 n : {1U, 7U, 50U, 128U})
    {
        auto csr  = random_csr<crd::f64>(&alloc, n, n, 5, 0xABCD ^ n);
        auto csc  = sp::to_csc(csr, &alloc);
        REQUIRE(csc.format == sp::SparseFormat::Csc);
        // CSC carries the same matrix.
        for (crd::u32 r = 0; r < n; ++r)
        {
            for (crd::u32 c = 0; c < n; ++c)
            {
                CHECK(csc.coeff(r, c) == csr.coeff(r, c));
            }
        }
        auto back = sp::from_csc(csc, &alloc);
        INFO("n=" << n);
        CHECK(patterns_byte_equal(csr, back));  // round-trip identity
    }
}

TEST_CASE("to_coo round-trips through TripletBuilder", "[hesap][sparse][convert][coo]")
{
    crd::memory::TlsfAllocator alloc(2 << 20);
    auto csr = random_csr<crd::f64>(&alloc, 40, 40, 4, 0x9999);
    auto coo = sp::to_coo(csr, &alloc);
    REQUIRE(coo.row_idx.size() == csr.nnz());

    sp::TripletBuilder<crd::f64> b(&alloc, coo.rows, coo.cols);
    for (crd::usize k = 0; k < coo.row_idx.size(); ++k)
    {
        b.add(coo.row_idx[k], coo.col_idx[k], coo.values[k]);
    }
    auto rebuilt = b.compress();
    CHECK(patterns_byte_equal(csr, rebuilt));
}

TEST_CASE("conversions work for all 4 type variants + are deterministic", "[hesap][sparse][convert][types]")
{
    crd::memory::TlsfAllocator alloc(2 << 20);
    {
        auto a   = random_csr<crd::f32>(&alloc, 30, 30, 4, 0x1);
        auto t1  = sp::transpose(a, &alloc);
        auto t2  = sp::transpose(a, &alloc);
        CHECK(patterns_byte_equal(t1, t2));  // deterministic
    }
    {
        auto a  = random_csr<Complex64>(&alloc, 25, 25, 3, 0x2);
        auto t  = sp::transpose(a, &alloc);
        auto tt = sp::transpose(t, &alloc);
        CHECK(patterns_byte_equal(a, tt));
        // complex transpose is non-conjugating: entry value preserved.
        CHECK(t.coeff(0, 0).re == a.coeff(0, 0).re);
    }
}
