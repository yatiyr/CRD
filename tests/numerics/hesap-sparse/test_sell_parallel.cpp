// crd-hesap-sparse v1b-2 -- parallel SELL spmv: bit-exact with serial across
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
        // power-law-ish: a few long rows to exercise σ + load imbalance.
        const crd::u32 deg = (next() % 11 == 0) ? (nnz_per_row * 6) : nnz_per_row;
        for (crd::u32 k = 0; k < deg; ++k)
        {
            b.add(r, next() % n, static_cast<T>(1 + (next() % 9)) / static_cast<T>(7));
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

TEST_CASE("parallel SELL spmv is bit-exact with serial across job counts", "[hesap][sparse][sell][parallel]")
{
    jobs_listener();  // init crd::jobs once for the binary
    crd::memory::TlsfAllocator alloc(32 << 20);

    for (crd::u32 n : {1U, 4U, 9U, 100U, 4096U})
    {
        auto csr  = random_csr<crd::f64>(&alloc, n, 5, 0x1234 ^ n);
        auto sell = sp::to_sell(csr, &alloc);
        crd::containers::Array<crd::f64> x(&alloc);
        x.reserve(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x.push_back(static_cast<crd::f64>(1 + (i % 17)) / 13.0);
        }

        crd::containers::Array<crd::f64> y_serial(&alloc);
        y_serial.resize(n);
        sp::spmv_sell<crd::f64>(1.0, sell, cspan(x), 0.0, mspan(y_serial));

        for (crd::u32 jobs : {1U, 2U, 4U, 8U, 16U, 64U})
        {
            crd::containers::Array<crd::f64> y_par(&alloc);
            y_par.resize(n);
            sp::spmv_sell_parallel<crd::f64>(1.0, sell, cspan(x), 0.0, mspan(y_par), jobs);
            crd::jobs::frame_reset();  // reclaim the parallel_for JobDecl arena
            for (crd::u32 i = 0; i < n; ++i)
            {
                INFO("n=" << n << " jobs=" << jobs << " i=" << i);
                CHECK(y_par[i] == y_serial[i]);  // bit-exact vs serial, any job count
            }
        }
    }
}

TEST_CASE("parallel SELL spmv auto job count matches serial", "[hesap][sparse][sell][parallel]")
{
    jobs_listener();
    crd::memory::TlsfAllocator alloc(16 << 20);
    auto csr  = random_csr<crd::f64>(&alloc, 2000, 8, 0xABCDEF);
    auto sell = sp::to_sell(csr, &alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    x.reserve(2000);
    for (crd::u32 i = 0; i < 2000; ++i)
    {
        x.push_back(static_cast<crd::f64>(1 + (i % 23)) / 7.0);
    }
    crd::containers::Array<crd::f64> y_serial(&alloc);
    y_serial.resize(2000);
    crd::containers::Array<crd::f64> y_par(&alloc);
    y_par.resize(2000);
    sp::spmv_sell<crd::f64>(2.0, sell, cspan(x), 0.0, mspan(y_serial));
    sp::spmv_sell_parallel<crd::f64>(2.0, sell, cspan(x), 0.0, mspan(y_par), /*auto*/ 0);
    crd::jobs::frame_reset();
    for (crd::u32 i = 0; i < 2000; ++i)
    {
        CHECK(y_par[i] == y_serial[i]);
    }
}
