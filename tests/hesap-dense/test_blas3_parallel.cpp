#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "hesap_jobs_fixture.hpp"
#include "random_matrix.hpp"

#include <cstring>

using crd::hesap::dense::gemm;
using crd::hesap::dense::gemm_parallel;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::Trans;
using crd_hesap_dense_tests::random_general;

// Lifetime: every TEST_CASE that uses gemm_parallel ends with crd::jobs::wait,
// so the fiber system is quiescent at fixture teardown. The fixture inits the
// job system ONCE for the binary; double-init crashes (see CLAUDE.md
// "jobs::init() in test binaries"). Shared with test_lu.cpp via the
// hesap_jobs_fixture.hpp header so both files agree on a single listener.
namespace
{
inline crd_hesap_dense_tests::HesapJobsListener& parallel_jobs_listener()
{
    return crd_hesap_dense_tests::hesap_jobs_listener();
}
} // namespace

namespace
{
template <typename T, Layout L>
[[nodiscard]] bool matrices_bit_identical(const Matrix<T, L>& a, const Matrix<T, L>& b) noexcept
{
    if (a.rows() != b.rows() || a.cols() != b.cols())
    {
        return false;
    }
    return std::memcmp(a.data(), b.data(), a.rows() * a.cols() * sizeof(T)) == 0;
}
} // namespace

TEST_CASE("gemm_parallel: num_workers=1 falls back to serial gemm",
          "[hesap][blas3][parallel][gemm_parallel]")
{
    parallel_jobs_listener();
    crd::memory::TlsfAllocator alloc(8 * 1024 * 1024);
    constexpr crd::usize n = 64;
    Matrix<crd::f32> a(&alloc, n, n);
    Matrix<crd::f32> b(&alloc, n, n);
    Matrix<crd::f32> c_serial(&alloc, n, n);
    Matrix<crd::f32> c_par(&alloc, n, n);
    random_general(a, 1U);
    random_general(b, 2U);
    random_general(c_serial, 3U);
    random_general(c_par, 3U);

    gemm<crd::f32, Layout::RowMajor>(1.5F, a, b, 0.5F, c_serial);
    gemm_parallel<crd::f32, Layout::RowMajor>(1U, 1.5F, a, b, 0.5F, c_par);
    REQUIRE(matrices_bit_identical(c_serial, c_par));
}

TEST_CASE("gemm_parallel: f32 bit-exact across worker counts at N=64",
          "[hesap][blas3][parallel][gemm_parallel][determinism]")
{
    parallel_jobs_listener();
    crd::memory::TlsfAllocator alloc(8 * 1024 * 1024);
    constexpr crd::usize n = 64;
    Matrix<crd::f32> a(&alloc, n, n);
    Matrix<crd::f32> b(&alloc, n, n);
    random_general(a, 11U);
    random_general(b, 22U);

    Matrix<crd::f32> c_serial(&alloc, n, n);
    random_general(c_serial, 33U);
    gemm<crd::f32, Layout::RowMajor>(1.0F, a, b, 0.0F, c_serial);

    for (crd::u32 nw : {2U, 4U, 8U, 16U})
    {
        Matrix<crd::f32> c_par(&alloc, n, n);
        random_general(c_par, 33U);
        gemm_parallel<crd::f32, Layout::RowMajor>(nw, 1.0F, a, b, 0.0F, c_par);
        CAPTURE(nw);
        REQUIRE(matrices_bit_identical(c_serial, c_par));
    }
}

TEST_CASE("gemm_parallel: f32 bit-exact across worker counts at N=256",
          "[hesap][blas3][parallel][gemm_parallel][determinism]")
{
    parallel_jobs_listener();
    crd::memory::TlsfAllocator alloc(8 * 1024 * 1024);
    constexpr crd::usize n = 256;
    Matrix<crd::f32> a(&alloc, n, n);
    Matrix<crd::f32> b(&alloc, n, n);
    random_general(a, 101U);
    random_general(b, 202U);

    Matrix<crd::f32> c_serial(&alloc, n, n);
    random_general(c_serial, 303U);
    gemm<crd::f32, Layout::RowMajor>(0.75F, a, b, 0.25F, c_serial);

    for (crd::u32 nw : {2U, 4U, 8U, 16U})
    {
        Matrix<crd::f32> c_par(&alloc, n, n);
        random_general(c_par, 303U);
        gemm_parallel<crd::f32, Layout::RowMajor>(nw, 0.75F, a, b, 0.25F, c_par);
        CAPTURE(nw);
        REQUIRE(matrices_bit_identical(c_serial, c_par));
    }
}

TEST_CASE("gemm_parallel: f64 bit-exact across worker counts at N=256",
          "[hesap][blas3][parallel][gemm_parallel][determinism]")
{
    parallel_jobs_listener();
    crd::memory::TlsfAllocator alloc(16 * 1024 * 1024);
    constexpr crd::usize n = 256;
    Matrix<crd::f64> a(&alloc, n, n);
    Matrix<crd::f64> b(&alloc, n, n);
    random_general(a, 7U);
    random_general(b, 13U);

    Matrix<crd::f64> c_serial(&alloc, n, n);
    random_general(c_serial, 19U);
    gemm<crd::f64, Layout::RowMajor>(1.0, a, b, 0.0, c_serial);

    for (crd::u32 nw : {2U, 4U, 8U, 16U})
    {
        Matrix<crd::f64> c_par(&alloc, n, n);
        random_general(c_par, 19U);
        gemm_parallel<crd::f64, Layout::RowMajor>(nw, 1.0, a, b, 0.0, c_par);
        CAPTURE(nw);
        REQUIRE(matrices_bit_identical(c_serial, c_par));
    }
}

TEST_CASE("gemm_parallel: f64 bit-exact across worker counts at N=1024",
          "[hesap][blas3][parallel][gemm_parallel][determinism][slow]")
{
    parallel_jobs_listener();
    crd::memory::TlsfAllocator alloc(64 * 1024 * 1024);
    constexpr crd::usize n = 1024;
    Matrix<crd::f64> a(&alloc, n, n);
    Matrix<crd::f64> b(&alloc, n, n);
    random_general(a, 555U);
    random_general(b, 666U);

    Matrix<crd::f64> c_serial(&alloc, n, n);
    random_general(c_serial, 777U);
    gemm<crd::f64, Layout::RowMajor>(1.0, a, b, 0.0, c_serial);

    for (crd::u32 nw : {4U, 16U})
    {
        Matrix<crd::f64> c_par(&alloc, n, n);
        random_general(c_par, 777U);
        gemm_parallel<crd::f64, Layout::RowMajor>(nw, 1.0, a, b, 0.0, c_par);
        CAPTURE(nw);
        REQUIRE(matrices_bit_identical(c_serial, c_par));
    }
}

TEST_CASE("gemm_parallel: transposed operands bit-exact",
          "[hesap][blas3][parallel][gemm_parallel][determinism]")
{
    parallel_jobs_listener();
    crd::memory::TlsfAllocator alloc(8 * 1024 * 1024);
    constexpr crd::usize m = 128;
    constexpr crd::usize n = 192;
    constexpr crd::usize k = 96;
    // For trans_a = Transpose: A is k x m (read as m x k after transpose).
    Matrix<crd::f32> a(&alloc, k, m);
    Matrix<crd::f32> b(&alloc, k, n);
    random_general(a, 41U);
    random_general(b, 43U);

    Matrix<crd::f32> c_serial(&alloc, m, n);
    random_general(c_serial, 47U);
    gemm<crd::f32, Layout::RowMajor>(0.5F, a, b, 0.5F, c_serial, Trans::Transpose, Trans::None);

    for (crd::u32 nw : {2U, 4U, 8U})
    {
        Matrix<crd::f32> c_par(&alloc, m, n);
        random_general(c_par, 47U);
        gemm_parallel<crd::f32, Layout::RowMajor>(nw, 0.5F, a, b, 0.5F, c_par, Trans::Transpose,
                                                  Trans::None);
        CAPTURE(nw);
        REQUIRE(matrices_bit_identical(c_serial, c_par));
    }
}

TEST_CASE("gemm_parallel: rectangular non-multiple shape bit-exact",
          "[hesap][blas3][parallel][gemm_parallel][determinism]")
{
    parallel_jobs_listener();
    crd::memory::TlsfAllocator alloc(8 * 1024 * 1024);
    constexpr crd::usize m = 137;
    constexpr crd::usize n = 211;
    constexpr crd::usize k = 89;
    Matrix<crd::f32> a(&alloc, m, k);
    Matrix<crd::f32> b(&alloc, k, n);
    random_general(a, 91U);
    random_general(b, 93U);

    Matrix<crd::f32> c_serial(&alloc, m, n);
    random_general(c_serial, 95U);
    gemm<crd::f32, Layout::RowMajor>(1.25F, a, b, -0.5F, c_serial);

    for (crd::u32 nw : {2U, 4U, 8U})
    {
        Matrix<crd::f32> c_par(&alloc, m, n);
        random_general(c_par, 95U);
        gemm_parallel<crd::f32, Layout::RowMajor>(nw, 1.25F, a, b, -0.5F, c_par);
        CAPTURE(nw);
        REQUIRE(matrices_bit_identical(c_serial, c_par));
    }
}

TEST_CASE("gemm_parallel_auto: tiny matrix routed serial",
          "[hesap][blas3][parallel][gemm_parallel_auto]")
{
    parallel_jobs_listener();
    crd::memory::TlsfAllocator alloc(64 * 1024);
    // mnk = 8*8*8 = 512 — well below kSerialThreshold = 256K → serial path.
    constexpr crd::usize n = 8;
    Matrix<crd::f64> a(&alloc, n, n);
    Matrix<crd::f64> b(&alloc, n, n);
    Matrix<crd::f64> c_serial(&alloc, n, n);
    Matrix<crd::f64> c_auto(&alloc, n, n);
    random_general(a, 7U);
    random_general(b, 13U);
    random_general(c_serial, 19U);
    random_general(c_auto, 19U);

    crd::hesap::dense::gemm<crd::f64, Layout::RowMajor>(1.5, a, b, -0.25, c_serial);
    crd::hesap::dense::gemm_parallel_auto<crd::f64, Layout::RowMajor>(1.5, a, b, -0.25, c_auto);
    REQUIRE(matrices_bit_identical(c_serial, c_auto));
}

TEST_CASE("gemm_parallel_auto: large matrix routed parallel + matches serial",
          "[hesap][blas3][parallel][gemm_parallel_auto]")
{
    parallel_jobs_listener();
    crd::memory::TlsfAllocator alloc(16 * 1024 * 1024);
    constexpr crd::usize n = 256;
    Matrix<crd::f64> a(&alloc, n, n);
    Matrix<crd::f64> b(&alloc, n, n);
    Matrix<crd::f64> c_serial(&alloc, n, n);
    Matrix<crd::f64> c_auto(&alloc, n, n);
    random_general(a, 41U);
    random_general(b, 43U);
    random_general(c_serial, 47U);
    random_general(c_auto, 47U);

    crd::hesap::dense::gemm<crd::f64, Layout::RowMajor>(1.0, a, b, 0.0, c_serial);
    crd::hesap::dense::gemm_parallel_auto<crd::f64, Layout::RowMajor>(1.0, a, b, 0.0, c_auto);
    REQUIRE(matrices_bit_identical(c_serial, c_auto));
}

TEST_CASE("gemm_parallel: repeated runs at same worker count are deterministic",
          "[hesap][blas3][parallel][gemm_parallel][determinism]")
{
    parallel_jobs_listener();
    crd::memory::TlsfAllocator alloc(16 * 1024 * 1024);
    constexpr crd::usize n = 256;
    Matrix<crd::f64> a(&alloc, n, n);
    Matrix<crd::f64> b(&alloc, n, n);
    random_general(a, 1111U);
    random_general(b, 2222U);

    Matrix<crd::f64> c_first(&alloc, n, n);
    random_general(c_first, 3333U);
    gemm_parallel<crd::f64, Layout::RowMajor>(8U, 1.0, a, b, 0.0, c_first);

    for (int run = 0; run < 4; ++run)
    {
        Matrix<crd::f64> c_again(&alloc, n, n);
        random_general(c_again, 3333U);
        gemm_parallel<crd::f64, Layout::RowMajor>(8U, 1.0, a, b, 0.0, c_again);
        CAPTURE(run);
        REQUIRE(matrices_bit_identical(c_first, c_again));
    }
}
