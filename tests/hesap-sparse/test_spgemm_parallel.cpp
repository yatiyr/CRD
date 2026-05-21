// crd-hesap-sparse v1d-2 -- parallel spgemm: bit-exact with serial across
// worker / job counts (deterministic, thread-count-independent).

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "sparse_jobs_fixture.hpp"

#include <cstdint>

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
        // skewed degree -> exercises flop-balancing + load imbalance.
        const crd::u32 deg = (next() % 9 == 0) ? (nnz_per_row * 5) : nnz_per_row;
        for (crd::u32 k = 0; k < deg; ++k)
        {
            b.add(r, next() % cols, static_cast<T>(1 + (next() % 9)) / static_cast<T>(7));
        }
    }
    return b.compress();
}

template <typename T>
bool csr_byte_equal(const sp::SparseMatrix<T, sp::SparseFormat::Csr>& a,
                    const sp::SparseMatrix<T, sp::SparseFormat::Csr>& b)
{
    if (a.rows() != b.rows() || a.cols() != b.cols() || a.nnz() != b.nnz())
    {
        return false;
    }
    for (crd::usize i = 0; i < a.pattern().outer_ptr.size(); ++i)
    {
        if (a.pattern().outer_ptr[i] != b.pattern().outer_ptr[i])
        {
            return false;
        }
    }
    for (crd::usize i = 0; i < a.nnz(); ++i)
    {
        if (a.pattern().inner_idx[i] != b.pattern().inner_idx[i] || a.values().values[i] != b.values().values[i])
        {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("parallel spgemm is bit-exact with serial across job counts", "[hesap][sparse][spgemm][parallel]")
{
    jobs_listener();
    crd::memory::TlsfAllocator alloc(128 << 20);
    for (crd::u32 n : {1U, 8U, 64U, 500U, 2000U})
    {
        auto a      = random_csr<crd::f64>(&alloc, n, n, 5, 0xA1 ^ n);
        auto b      = random_csr<crd::f64>(&alloc, n, n, 5, 0xB2 ^ n);
        auto serial = sp::spgemm(a, b, &alloc);
        for (crd::u32 jobs : {1U, 2U, 4U, 8U, 16U, 64U})
        {
            auto par = sp::spgemm_parallel(a, b, &alloc, jobs);
            crd::jobs::frame_reset();
            INFO("n=" << n << " jobs=" << jobs);
            CHECK(csr_byte_equal(serial, par));  // bit-exact, any job count
        }
    }
}

TEST_CASE("parallel spgemm auto job count matches serial (non-square)", "[hesap][sparse][spgemm][parallel]")
{
    jobs_listener();
    crd::memory::TlsfAllocator alloc(64 << 20);
    auto a      = random_csr<crd::f64>(&alloc, 1500, 1200, 6, 0xCAFE);
    auto b      = random_csr<crd::f64>(&alloc, 1200, 1800, 6, 0xF00D);
    auto serial = sp::spgemm(a, b, &alloc);
    auto par    = sp::spgemm_parallel(a, b, &alloc, /*auto*/ 0);
    crd::jobs::frame_reset();
    CHECK(csr_byte_equal(serial, par));
}
