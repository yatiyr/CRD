#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "hesap_jobs_fixture.hpp"

#include <cstring>

using crd::hesap::dense::gemm;
using crd::hesap::dense::gemm_parallel;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::Trans;

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
void fill_matrix(Matrix<T, L>& m, crd::u32 seed)
{
    crd::u32 s = seed;
    for (crd::usize i = 0; i < m.rows(); ++i)
    {
        for (crd::usize j = 0; j < m.cols(); ++j)
        {
            s = s * 1664525U + 1013904223U;
            const T raw = static_cast<T>(static_cast<crd::i32>(s >> 8) % 1000) * static_cast<T>(0.001);
            m(i, j) = raw;
        }
    }
}

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
    constexpr crd::usize kN = 64;
    Matrix<crd::f32> a(&alloc, kN, kN);
    Matrix<crd::f32> b(&alloc, kN, kN);
    Matrix<crd::f32> c_serial(&alloc, kN, kN);
    Matrix<crd::f32> c_par(&alloc, kN, kN);
    fill_matrix(a, 1U);
    fill_matrix(b, 2U);
    fill_matrix(c_serial, 3U);
    fill_matrix(c_par, 3U);

    gemm<crd::f32, Layout::RowMajor>(1.5F, a, b, 0.5F, c_serial);
    gemm_parallel<crd::f32, Layout::RowMajor>(1U, 1.5F, a, b, 0.5F, c_par);
    REQUIRE(matrices_bit_identical(c_serial, c_par));
}

TEST_CASE("gemm_parallel: f32 bit-exact across worker counts at N=64",
          "[hesap][blas3][parallel][gemm_parallel][determinism]")
{
    parallel_jobs_listener();
    crd::memory::TlsfAllocator alloc(8 * 1024 * 1024);
    constexpr crd::usize kN = 64;
    Matrix<crd::f32> a(&alloc, kN, kN);
    Matrix<crd::f32> b(&alloc, kN, kN);
    fill_matrix(a, 11U);
    fill_matrix(b, 22U);

    Matrix<crd::f32> c_serial(&alloc, kN, kN);
    fill_matrix(c_serial, 33U);
    gemm<crd::f32, Layout::RowMajor>(1.0F, a, b, 0.0F, c_serial);

    for (crd::u32 nw : {2U, 4U, 8U, 16U})
    {
        Matrix<crd::f32> c_par(&alloc, kN, kN);
        fill_matrix(c_par, 33U);
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
    constexpr crd::usize kN = 256;
    Matrix<crd::f32> a(&alloc, kN, kN);
    Matrix<crd::f32> b(&alloc, kN, kN);
    fill_matrix(a, 101U);
    fill_matrix(b, 202U);

    Matrix<crd::f32> c_serial(&alloc, kN, kN);
    fill_matrix(c_serial, 303U);
    gemm<crd::f32, Layout::RowMajor>(0.75F, a, b, 0.25F, c_serial);

    for (crd::u32 nw : {2U, 4U, 8U, 16U})
    {
        Matrix<crd::f32> c_par(&alloc, kN, kN);
        fill_matrix(c_par, 303U);
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
    constexpr crd::usize kN = 256;
    Matrix<crd::f64> a(&alloc, kN, kN);
    Matrix<crd::f64> b(&alloc, kN, kN);
    fill_matrix(a, 7U);
    fill_matrix(b, 13U);

    Matrix<crd::f64> c_serial(&alloc, kN, kN);
    fill_matrix(c_serial, 19U);
    gemm<crd::f64, Layout::RowMajor>(1.0, a, b, 0.0, c_serial);

    for (crd::u32 nw : {2U, 4U, 8U, 16U})
    {
        Matrix<crd::f64> c_par(&alloc, kN, kN);
        fill_matrix(c_par, 19U);
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
    constexpr crd::usize kN = 1024;
    Matrix<crd::f64> a(&alloc, kN, kN);
    Matrix<crd::f64> b(&alloc, kN, kN);
    fill_matrix(a, 555U);
    fill_matrix(b, 666U);

    Matrix<crd::f64> c_serial(&alloc, kN, kN);
    fill_matrix(c_serial, 777U);
    gemm<crd::f64, Layout::RowMajor>(1.0, a, b, 0.0, c_serial);

    for (crd::u32 nw : {4U, 16U})
    {
        Matrix<crd::f64> c_par(&alloc, kN, kN);
        fill_matrix(c_par, 777U);
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
    constexpr crd::usize kM = 128;
    constexpr crd::usize kN = 192;
    constexpr crd::usize kK = 96;
    // For trans_a = Transpose: A is kK x kM (read as kM x kK after transpose).
    Matrix<crd::f32> a(&alloc, kK, kM);
    Matrix<crd::f32> b(&alloc, kK, kN);
    fill_matrix(a, 41U);
    fill_matrix(b, 43U);

    Matrix<crd::f32> c_serial(&alloc, kM, kN);
    fill_matrix(c_serial, 47U);
    gemm<crd::f32, Layout::RowMajor>(0.5F, a, b, 0.5F, c_serial, Trans::Transpose, Trans::None);

    for (crd::u32 nw : {2U, 4U, 8U})
    {
        Matrix<crd::f32> c_par(&alloc, kM, kN);
        fill_matrix(c_par, 47U);
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
    constexpr crd::usize kM = 137;
    constexpr crd::usize kN = 211;
    constexpr crd::usize kK = 89;
    Matrix<crd::f32> a(&alloc, kM, kK);
    Matrix<crd::f32> b(&alloc, kK, kN);
    fill_matrix(a, 91U);
    fill_matrix(b, 93U);

    Matrix<crd::f32> c_serial(&alloc, kM, kN);
    fill_matrix(c_serial, 95U);
    gemm<crd::f32, Layout::RowMajor>(1.25F, a, b, -0.5F, c_serial);

    for (crd::u32 nw : {2U, 4U, 8U})
    {
        Matrix<crd::f32> c_par(&alloc, kM, kN);
        fill_matrix(c_par, 95U);
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
    constexpr crd::usize kN = 8;
    Matrix<crd::f64> a(&alloc, kN, kN);
    Matrix<crd::f64> b(&alloc, kN, kN);
    Matrix<crd::f64> c_serial(&alloc, kN, kN);
    Matrix<crd::f64> c_auto(&alloc, kN, kN);
    fill_matrix(a, 7U);
    fill_matrix(b, 13U);
    fill_matrix(c_serial, 19U);
    fill_matrix(c_auto, 19U);

    crd::hesap::dense::gemm<crd::f64, Layout::RowMajor>(1.5, a, b, -0.25, c_serial);
    crd::hesap::dense::gemm_parallel_auto<crd::f64, Layout::RowMajor>(1.5, a, b, -0.25, c_auto);
    REQUIRE(matrices_bit_identical(c_serial, c_auto));
}

TEST_CASE("gemm_parallel_auto: large matrix routed parallel + matches serial",
          "[hesap][blas3][parallel][gemm_parallel_auto]")
{
    parallel_jobs_listener();
    crd::memory::TlsfAllocator alloc(16 * 1024 * 1024);
    constexpr crd::usize kN = 256;
    Matrix<crd::f64> a(&alloc, kN, kN);
    Matrix<crd::f64> b(&alloc, kN, kN);
    Matrix<crd::f64> c_serial(&alloc, kN, kN);
    Matrix<crd::f64> c_auto(&alloc, kN, kN);
    fill_matrix(a, 41U);
    fill_matrix(b, 43U);
    fill_matrix(c_serial, 47U);
    fill_matrix(c_auto, 47U);

    crd::hesap::dense::gemm<crd::f64, Layout::RowMajor>(1.0, a, b, 0.0, c_serial);
    crd::hesap::dense::gemm_parallel_auto<crd::f64, Layout::RowMajor>(1.0, a, b, 0.0, c_auto);
    REQUIRE(matrices_bit_identical(c_serial, c_auto));
}

TEST_CASE("gemm_parallel: repeated runs at same worker count are deterministic",
          "[hesap][blas3][parallel][gemm_parallel][determinism]")
{
    parallel_jobs_listener();
    crd::memory::TlsfAllocator alloc(16 * 1024 * 1024);
    constexpr crd::usize kN = 256;
    Matrix<crd::f64> a(&alloc, kN, kN);
    Matrix<crd::f64> b(&alloc, kN, kN);
    fill_matrix(a, 1111U);
    fill_matrix(b, 2222U);

    Matrix<crd::f64> c_first(&alloc, kN, kN);
    fill_matrix(c_first, 3333U);
    gemm_parallel<crd::f64, Layout::RowMajor>(8U, 1.0, a, b, 0.0, c_first);

    for (int run = 0; run < 4; ++run)
    {
        Matrix<crd::f64> c_again(&alloc, kN, kN);
        fill_matrix(c_again, 3333U);
        gemm_parallel<crd::f64, Layout::RowMajor>(8U, 1.0, a, b, 0.0, c_again);
        CAPTURE(run);
        REQUIRE(matrices_bit_identical(c_first, c_again));
    }
}
