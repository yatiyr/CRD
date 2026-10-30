// crd-hesap-sparse v1d-2 -- parallel spgemm: bit-exact with serial across
// worker / job counts (deterministic, thread-count-independent).

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "sparse_arm_fixture.hpp"
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
    CHECK(csr_byte_equal(serial, par));
}

// ---- spgemm_arm: every cut and job count gives serial spgemm's bits ----

namespace
{
using crd_hesap_sparse_tests::arm_csr;
using crd_hesap_sparse_tests::arm_value;
using crd_hesap_sparse_tests::same_bytes;

// Shape, topology hash, and every outer, inner and value byte.
template <typename T>
bool same_csr(const sp::SparseMatrix<T, sp::SparseFormat::Csr>& x, const sp::SparseMatrix<T, sp::SparseFormat::Csr>& y)
{
    return x.rows() == y.rows() && x.cols() == y.cols() && x.nnz() == y.nnz() &&
           x.pattern().topology_hash == y.pattern().topology_hash &&
           x.pattern().outer_ptr.size() == y.pattern().outer_ptr.size() &&
           same_bytes(x.pattern().outer_ptr.data(), y.pattern().outer_ptr.data(), x.pattern().outer_ptr.size()) &&
           same_bytes(x.pattern().inner_idx.data(), y.pattern().inner_idx.data(), x.nnz()) &&
           same_bytes(x.values().values.data(), y.values().values.data(), x.nnz());
}

constexpr crd::u32 kArmWorkers[] = {0, 1, 2, 3, 4, 8, 16, 32};

template <typename T>
void check_spgemm_arms(const sp::SparseMatrix<T, sp::SparseFormat::Csr>& a,
                       const sp::SparseMatrix<T, sp::SparseFormat::Csr>& b, crd::memory::IAllocator* alloc)
{
    const auto serial = sp::spgemm<T>(a, b, alloc);
    crd::containers::Array<crd::u64> prefix(alloc);
    sp::detail::spgemm_flop_prefix<T>(a, b, prefix);
    const crd::u64 flop = prefix[a.rows()];
    // On the jobs (0, the product's flops), serial below the cut (flops + 1) and the default.
    for (const crd::u64 cut : {crd::u64{0}, flop, flop + 1, ~crd::u64{0}})
    {
        for (const crd::u32 workers : kArmWorkers)
        {
            INFO("cut " << cut << " of " << flop << " flops, workers " << workers);
            CHECK(same_csr(sp::spgemm_arm<T>({.parallel_min_flop = cut, .workers = workers}, a, b, alloc), serial));
        }
    }
}

template <typename T> void check_spgemm_arm_type(crd::memory::IAllocator* alloc)
{
    for (const bool specials : {false, true})
    {
        INFO("specials " << specials);
        // Fewer rows than jobs, square, and a non-square chain.
        for (const crd::u32 n : {1U, 3U, 64U, 500U})
        {
            INFO("n " << n);
            const auto a = arm_csr<T>(alloc, n, n, 5, 0xA1U ^ n, specials);
            const auto b = arm_csr<T>(alloc, n, n, 5, 0xB2U ^ n, specials);
            check_spgemm_arms<T>(a, b, alloc);
        }
        const auto a = arm_csr<T>(alloc, 300, 200, 6, 0xCAFE, specials);
        const auto b = arm_csr<T>(alloc, 200, 450, 4, 0xF00D, specials);
        check_spgemm_arms<T>(a, b, alloc);
    }
}
} // namespace

TEST_CASE("spgemm_arm keeps serial spgemm's bits at every cut and job count",
          "[hesap][sparse][spgemm][parallel][jobs][determinism]")
{
    jobs_listener();
    crd::memory::TlsfAllocator alloc(128 << 20);
    {
        INFO("f64");
        check_spgemm_arm_type<crd::f64>(&alloc);
    }
    {
        INFO("f32");
        check_spgemm_arm_type<crd::f32>(&alloc);
    }
    {
        INFO("c64");
        check_spgemm_arm_type<crd::hesap::Complex64>(&alloc);
    }
    {
        INFO("c32");
        check_spgemm_arm_type<crd::hesap::Complex32>(&alloc);
    }
}

TEST_CASE("spgemm_arm's hash path and an empty A keep serial spgemm's bits and shape",
          "[hesap][sparse][spgemm][parallel][hash][jobs][determinism]")
{
    jobs_listener();
    crd::memory::TlsfAllocator alloc(64 << 20);
    // B past kMaxSpaCols columns (the hash accumulator), one entry per row at a distinct large column.
    const crd::u32 k = 150;
    const crd::u32 n = sp::kMaxSpaCols + 1U;
    sp::TripletBuilder<crd::f64> bb(&alloc, k, n);
    for (crd::u32 r = 0; r < k; ++r)
    {
        bb.add(r, n - 1U - r * 5U, arm_value<crd::f64>(r * 2654435761U, true));
        bb.add(r, r, arm_value<crd::f64>(r * 40503U + 7U, true));
    }
    const auto b = bb.compress();
    const auto a = arm_csr<crd::f64>(&alloc, 40, k, 4, 0xBEEF, true);
    check_spgemm_arms<crd::f64>(a, b, &alloc);
    // An empty A: every route is the serial product's 0 x n shape (spgemm_parallel's was 0 x 0 before the arm).
    const auto empty = sp::TripletBuilder<crd::f64>(&alloc, 0, 40).compress();
    const auto c = arm_csr<crd::f64>(&alloc, 40, 17, 3, 0x5EED, false);
    check_spgemm_arms<crd::f64>(empty, c, &alloc);
    CHECK(sp::spgemm_parallel<crd::f64>(empty, c, &alloc).cols() == 17);
}
