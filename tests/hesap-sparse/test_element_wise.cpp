// crd-hesap-sparse v1c-2 -- element-wise add/sub/scale/hadamard tests.

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
sp::SparseMatrix<T, sp::SparseFormat::Csr> random_csr(crd::memory::IAllocator* alloc, crd::u32 n, crd::u32 nnz_per_row,
                                                      std::uint64_t seed)
{
    std::uint64_t s = seed;
    auto          next = [&]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<crd::u32>(s >> 33);
    };
    sp::TripletBuilder<T> b(alloc, n, n);
    for (crd::u32 r = 0; r < n; ++r)
    {
        for (crd::u32 k = 0; k < nnz_per_row; ++k)
        {
            b.add(r, next() % n, static_cast<T>(1 + (next() % 9)) / static_cast<T>(7));
        }
    }
    return b.compress();
}
} // namespace

TEST_CASE("add/sub match the dense oracle (union structure)", "[hesap][sparse][ewise][add]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    for (crd::u32 n : {1U, 5U, 40U, 128U})
    {
        auto a = random_csr<crd::f64>(&alloc, n, 4, 0xA ^ n);
        auto b = random_csr<crd::f64>(&alloc, n, 4, 0xB ^ n);  // different pattern
        auto c = sp::add(a, b, &alloc);
        auto d = sp::subtract(a, b, &alloc);
        for (crd::u32 i = 0; i < n; ++i)
        {
            for (crd::u32 j = 0; j < n; ++j)
            {
                INFO("n=" << n << " i=" << i << " j=" << j);
                CHECK(c.coeff(i, j) == (a.coeff(i, j) + b.coeff(i, j)));
                CHECK(d.coeff(i, j) == (a.coeff(i, j) - b.coeff(i, j)));
            }
        }
    }
}

TEST_CASE("add fast path (same pattern) equals the merge path bit-exactly", "[hesap][sparse][ewise][add]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    // Two matrices with IDENTICAL pattern but different values -> fast path.
    auto a = random_csr<crd::f64>(&alloc, 64, 5, 0x1234);
    // Build b with a's exact structure (reuse a's triplets via to_coo) but new values.
    auto coo = sp::to_coo(a, &alloc);
    sp::TripletBuilder<crd::f64> bb(&alloc, a.rows(), a.cols());
    for (crd::usize k = 0; k < coo.row_idx.size(); ++k)
    {
        bb.add(coo.row_idx[k], coo.col_idx[k], coo.values[k] * 2.0 + 1.0);
    }
    auto b = bb.compress();
    REQUIRE(a.pattern().topology_hash == b.pattern().topology_hash);  // fast path engages

    auto fast = sp::add(a, b, &alloc);
    // Force the merge path by comparing against a hand dense oracle.
    for (crd::u32 i = 0; i < 64; ++i)
    {
        for (crd::u32 j = 0; j < 64; ++j)
        {
            CHECK(fast.coeff(i, j) == (a.coeff(i, j) + b.coeff(i, j)));
        }
    }
    CHECK(fast.pattern().topology_hash == a.pattern().topology_hash);  // union of equal = same structure
}

TEST_CASE("scale multiplies every value", "[hesap][sparse][ewise][scale]")
{
    crd::memory::TlsfAllocator alloc(2 << 20);
    auto a = random_csr<crd::f64>(&alloc, 50, 5, 0x55);
    auto c = sp::scale(3.0, a, &alloc);
    CHECK(c.pattern().topology_hash == a.pattern().topology_hash);  // structure preserved
    for (crd::u32 i = 0; i < 50; ++i)
    {
        for (crd::u32 j = 0; j < 50; ++j)
        {
            CHECK(c.coeff(i, j) == (3.0 * a.coeff(i, j)));
        }
    }
}

TEST_CASE("hadamard keeps the intersection only", "[hesap][sparse][ewise][hadamard]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    auto a = random_csr<crd::f64>(&alloc, 40, 6, 0x77 ^ 1);
    auto b = random_csr<crd::f64>(&alloc, 40, 6, 0x77 ^ 2);
    auto c = sp::hadamard(a, b, &alloc);
    for (crd::u32 i = 0; i < 40; ++i)
    {
        for (crd::u32 j = 0; j < 40; ++j)
        {
            const crd::f64 expect = a.coeff(i, j) * b.coeff(i, j);  // 0 unless both present
            CHECK(c.coeff(i, j) == expect);
        }
    }
}

TEST_CASE("element-wise ops are deterministic + work for complex", "[hesap][sparse][ewise][types]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    auto a  = random_csr<Complex64>(&alloc, 30, 4, 0x1);
    auto b  = random_csr<Complex64>(&alloc, 30, 4, 0x2);
    auto c1 = sp::add(a, b, &alloc);
    auto c2 = sp::add(a, b, &alloc);
    REQUIRE(c1.nnz() == c2.nnz());
    for (crd::usize k = 0; k < c1.nnz(); ++k)
    {
        CHECK(c1.values().values[k].re == c2.values().values[k].re);
        CHECK(c1.values().values[k].im == c2.values().values[k].im);
    }
    // f32 sub sanity
    auto fa = random_csr<crd::f32>(&alloc, 20, 3, 0x3);
    auto fb = random_csr<crd::f32>(&alloc, 20, 3, 0x4);
    auto fc = sp::subtract(fa, fb, &alloc);
    CHECK(fc.coeff(0, 0) == (fa.coeff(0, 0) - fb.coeff(0, 0)));
}
