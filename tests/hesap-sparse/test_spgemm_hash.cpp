// crd-hesap-sparse v1g-2 -- hash-accumulator spgemm (lifts the 4M-col cap).

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "sparse_jobs_fixture.hpp"

#include <cstdint>

namespace sp = crd::hesap::sparse;

namespace
{
template <typename T>
sp::SparseMatrix<T, sp::SparseFormat::Csr> rand_csr(crd::memory::IAllocator* alloc, crd::u32 rows, crd::u32 cols,
                                                    crd::u32 per_row, std::uint64_t seed)
{
    std::uint64_t s    = seed;
    auto          next = [&]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<crd::u32>(s >> 33);
    };
    sp::TripletBuilder<T> tb(alloc, rows, cols);
    for (crd::u32 i = 0; i < rows; ++i)
    {
        for (crd::u32 t = 0; t < per_row; ++t)
        {
            tb.add(i, next() % cols, static_cast<T>(1 + (next() % 9)) / static_cast<T>(7));
        }
    }
    return tb.compress();
}
} // namespace

TEST_CASE("hash spgemm kernel is bit-exact with the dense SPA path", "[hesap][sparse][spgemm][hash]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    const crd::u32 m = 800;
    const crd::u32 k = 600;
    const crd::u32 n = 2000;  // in-cap -> dense path is the reference
    auto           a = rand_csr<crd::f64>(&alloc, m, k, 5, 0xA1);
    auto           b = rand_csr<crd::f64>(&alloc, k, n, 5, 0xB2);

    auto dense = sp::spgemm<crd::f64>(a, b, &alloc);

    // Run the hash kernel directly and compare bit-for-bit.
    sp::SparsePattern              pat(&alloc);
    sp::SparseValues<crd::f64>     vals(&alloc);
    pat.rows = m;
    pat.cols = n;
    pat.outer_ptr.resize(static_cast<crd::usize>(m) + 1);
    pat.outer_ptr[0] = 0;
    sp::detail::SpgemmHash<crd::f64> h(&alloc);
    sp::detail::spgemm_rows_hash<crd::f64>(a, b, 0, m, h, pat.inner_idx, vals.values, pat.outer_ptr);

    REQUIRE(pat.inner_idx.size() == dense.nnz());
    for (crd::u32 i = 0; i <= m; ++i)
    {
        REQUIRE(pat.outer_ptr[i] == dense.pattern().outer_ptr[i]);
    }
    for (crd::usize z = 0; z < dense.nnz(); ++z)
    {
        REQUIRE(pat.inner_idx[z] == dense.pattern().inner_idx[z]);
        REQUIRE(vals.values[z] == dense.values().values[z]);  // bit-exact
    }
}

TEST_CASE("spgemm lifts the 4M-column cap via the hash path", "[hesap][sparse][spgemm][hash]")
{
    crd_hesap_sparse_tests::sparse_jobs_listener();
    crd::memory::TlsfAllocator alloc(64 << 20);
    const crd::u32 m = 200;
    const crd::u32 k = 150;
    const crd::u32 n = 5'000'001U;  // > kMaxSpaCols (4M) -> hash path

    // B[r] has one entry at a large, distinct column; A is dense-ish over k.
    sp::TripletBuilder<crd::f64> bb(&alloc, k, n);
    for (crd::u32 r = 0; r < k; ++r)
    {
        const crd::u32 col = 4'200'000U + r * 5U;  // all > 4M, distinct
        bb.add(r, col, static_cast<crd::f64>(r + 1));
    }
    auto b = bb.compress();
    auto a = rand_csr<crd::f64>(&alloc, m, k, 4, 0xCAFE);

    auto c_ser = sp::spgemm<crd::f64>(a, b, &alloc);          // hash (serial)
    REQUIRE(c_ser.cols() == n);
    REQUIRE(c_ser.nnz() > 0);

    // Hand-check one row: C[i, 4.2M+5r] = sum over A[i] entries at col r of a_ir * (r+1).
    const auto& ao = a.pattern().outer_ptr;
    const auto& ai = a.pattern().inner_idx;
    const auto& av = a.values().values;
    for (crd::u32 r = 0; r < k; ++r)
    {
        const crd::u32 ccol = 4'200'000U + r * 5U;
        crd::f64       expect = 0.0;
        for (crd::u32 t = ao[2]; t < ao[3]; ++t)  // row i = 2
        {
            if (ai[t] == r)
            {
                expect += av[t] * static_cast<crd::f64>(r + 1);
            }
        }
        if (expect != 0.0)
        {
            CHECK(c_ser.coeff(2, ccol) == expect);
        }
    }

    // Parallel hash path is bit-exact with serial.
    for (crd::u32 jobs : {1U, 2U, 4U, 8U})
    {
        auto c_par = sp::spgemm_parallel<crd::f64>(a, b, &alloc, jobs);
        crd::jobs::frame_reset();
        REQUIRE(c_par.nnz() == c_ser.nnz());
        for (crd::usize z = 0; z < c_ser.nnz(); ++z)
        {
            INFO("jobs=" << jobs << " z=" << z);
            REQUIRE(c_par.pattern().inner_idx[z] == c_ser.pattern().inner_idx[z]);
            REQUIRE(c_par.values().values[z] == c_ser.values().values[z]);
        }
    }
}
