#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/detail/gemm_arms.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "hesap_jobs_fixture.hpp"
#include "random_matrix.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

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

TEST_CASE("gemm_parallel: many calls in one jobs session don't exhaust the frame arena (leak fix)",
          "[hesap][blas3][parallel][gemm_parallel][frame-arena]")
{
    parallel_jobs_listener();
    const crd::u32 nw = crd::jobs::num_workers();
    if (nw <= 1U)
    {
        SUCCEED("single worker: gemm_parallel takes the serial path, no parallel_for frame allocations");
        return;
    }
    crd::memory::TlsfAllocator alloc(16 * 1024 * 1024);
    constexpr crd::usize n = 64;
    // ColMajor skips the RowMajor small-gemm fastpath, so this exercises the MAIN gemm_parallel path whose
    // per-(jc,pc) parallel_for JobDecl arrays are the documented gemm-parallel-frame-arena-leak. Pre-fix,
    // each call leaks its JobDecls into the dispatching thread's 1 MB frame arena, and ~1000 calls exhaust
    // it (win-debug FrameArena assert / release corruption). The fix reclaims each call's JobDecls in place,
    // so this 4000-call loop must COMPLETE — that completion is the regression guard.
    Matrix<crd::f64, Layout::ColMajor> a(&alloc, n, n);
    Matrix<crd::f64, Layout::ColMajor> b(&alloc, n, n);
    Matrix<crd::f64, Layout::ColMajor> c(&alloc, n, n);
    random_general(a, 7U);
    random_general(b, 9U);
    random_general(c, 5U);

    Matrix<crd::f64, Layout::ColMajor> c_ref(&alloc, n, n);
    gemm<crd::f64, Layout::ColMajor>(1.25, a, b, 0.0, c_ref); // serial reference (beta=0 -> C = 1.25*A*B)

    constexpr crd::u32 num_calls = 4000U; // >> the ~1000-call pre-fix exhaustion threshold
    for (crd::u32 it = 0; it < num_calls; ++it)
    {
        gemm_parallel<crd::f64, Layout::ColMajor>(nw, 1.25, a, b, 0.0, c);
    }

    // Reaching here means the arena did not exhaust across 4000 parallel gemms; the result is still correct.
    double max_diff = 0.0;
    const crd::f64* cd = c.data();
    const crd::f64* rd = c_ref.data();
    for (crd::usize i = 0; i < n * n; ++i)
    {
        const double d = std::abs(cd[i] - rd[i]);
        if (d > max_diff)
        {
            max_diff = d;
        }
    }
    CHECK(max_diff < 1e-9);
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

    for (crd::u32 nw : {2U, 4U, 8U, 16U, 32U})
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

    for (crd::u32 nw : {2U, 4U, 8U, 16U, 32U})
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

    for (crd::u32 nw : {2U, 4U, 8U, 16U, 32U})
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

// ---- gemm_parallel's crd-jobs levers (detail::GemmParallelPick, SIMD-DISPATCH's folded jobs retune) ----
namespace
{
using crd::hesap::dense::MatrixView;
using crd::hesap::dense::detail::GemmMcSplit;
using crd::hesap::dense::detail::GemmParallelPick;
using crd::hesap::dense::detail::GemmSplit;

// The real scalar of a value type.
template <typename T> struct LeverScalar
{
    using type = T;
};

template <typename R> struct LeverScalar<crd::hesap::Complex<R>>
{
    using type = R;
};

template <typename T> [[nodiscard]] T lever_value(crd_hesap_dense_tests::Rng& rng) noexcept
{
    if constexpr (std::is_floating_point_v<T>)
    {
        return rng.signed_unit<T>();
    }
    else
    {
        using R = typename LeverScalar<T>::type;
        const R re = rng.signed_unit<R>();
        return T{re, rng.signed_unit<R>()};
    }
}

template <typename T> [[nodiscard]] T lever_special(crd::usize i) noexcept
{
    using S = typename LeverScalar<T>::type;
    const S values[] = {std::numeric_limits<S>::quiet_NaN(), std::numeric_limits<S>::infinity(),
                        -std::numeric_limits<S>::infinity(), S{-0.0}, std::numeric_limits<S>::denorm_min() * S{3}};
    const S v = values[i % 5U];
    if constexpr (std::is_floating_point_v<T>)
    {
        return v;
    }
    else
    {
        return T{v, S{-0.0}};
    }
}

// One product walked through every lever at 1 to 32 jobs, each against serial gemm bit for bit over C and the guard
// words around it (C is an inner view of a store one row and one column wider on every side).
struct LeverShape
{
    crd::usize m, n, k;
    Trans ta, tb;
    bool specials; // NaN, infinities, -0 and subnormals in A
    bool nan_c;    // C filled with NaN (beta 0 must store, not read)
};

template <typename T, Layout L> void walk_levers(const LeverShape& sh, T alpha, T beta, crd::u32 seed)
{
    crd::memory::TlsfAllocator alloc(64 * 1024 * 1024);
    const crd::usize a_rows = sh.ta == Trans::None ? sh.m : sh.k;
    const crd::usize a_cols = sh.ta == Trans::None ? sh.k : sh.m;
    const crd::usize b_rows = sh.tb == Trans::None ? sh.k : sh.n;
    const crd::usize b_cols = sh.tb == Trans::None ? sh.n : sh.k;
    Matrix<T, L> a(&alloc, a_rows, a_cols);
    Matrix<T, L> b(&alloc, b_rows, b_cols);
    crd_hesap_dense_tests::Rng rng(seed);
    for (crd::usize i = 0; i < a_rows; ++i)
    {
        for (crd::usize j = 0; j < a_cols; ++j)
        {
            a(i, j) = sh.specials && (i * 7U + j) % 31U == 0U ? lever_special<T>(i + j) : lever_value<T>(rng);
        }
    }
    for (crd::usize i = 0; i < b_rows; ++i)
    {
        for (crd::usize j = 0; j < b_cols; ++j)
        {
            b(i, j) = lever_value<T>(rng);
        }
    }
    Matrix<T, L> start(&alloc, sh.m + 2U, sh.n + 2U);
    for (crd::usize i = 0; i < sh.m + 2U; ++i)
    {
        for (crd::usize j = 0; j < sh.n + 2U; ++j)
        {
            start(i, j) = sh.nan_c ? lever_special<T>(0) : lever_value<T>(rng);
        }
    }
    const auto inner = [&](Matrix<T, L>& store)
    {
        return MatrixView<T, L>(store.data() + store.ld() + 1U, sh.m, sh.n, store.ld());
    };
    const crd::usize bytes = (sh.m + 2U) * (sh.n + 2U) * sizeof(T);
    Matrix<T, L> serial(&alloc, sh.m + 2U, sh.n + 2U);
    std::memcpy(serial.data(), start.data(), bytes);
    crd::hesap::dense::gemm<T, L>(alpha, a.cview(), b.cview(), beta, inner(serial), sh.ta, sh.tb, &alloc);
    for (crd::usize i = 0; i < sh.m + 2U; ++i)
    {
        for (crd::usize j = 0; j < sh.n + 2U; ++j)
        {
            if (i == 0 || j == 0 || i == sh.m + 1U || j == sh.n + 1U)
            {
                REQUIRE(std::memcmp(&serial(i, j), &start(i, j), sizeof(T)) == 0); // the guard words
            }
        }
    }
    Matrix<T, L> arm(&alloc, sh.m + 2U, sh.n + 2U);
    for (const crd::usize resident : {crd::usize{0}, GemmParallelPick{}.resident_mnk})
    {
        for (const GemmMcSplit mc : {GemmMcSplit::PerWorker, GemmMcSplit::Block})
        {
            for (const GemmSplit pack : {GemmSplit::Serial, GemmSplit::Parallel})
            {
                for (const GemmSplit beta_only : {GemmSplit::Serial, GemmSplit::Parallel})
                {
                    const GemmParallelPick pick{
                        .resident_mnk = resident, .mc = mc, .pack_b = pack, .beta_only = beta_only};
                    for (const crd::u32 nw : {1U, 4U, 8U, 16U, 32U})
                    {
                        std::memcpy(arm.data(), start.data(), bytes);
                        crd::hesap::dense::detail::gemm_parallel_arm<T, L>(pick, nw, alpha, a.cview(), b.cview(), beta,
                                                                           inner(arm), sh.ta, sh.tb, &alloc);
                        CAPTURE(resident, static_cast<int>(mc), static_cast<int>(pack), static_cast<int>(beta_only),
                                nw);
                        const bool same = std::memcmp(arm.data(), serial.data(), bytes) == 0;
                        REQUIRE(same);
                    }
                }
            }
        }
    }
}
} // namespace

TEST_CASE("gemm_parallel_arm: every lever at 1 to 32 jobs gives serial gemm's bits",
          "[hesap][blas3][parallel][gemm_parallel][determinism]")
{
    parallel_jobs_listener();
    // Resident (m n k below the pick's threshold, op(A) row-major) or packed (resident_mnk 0), past one depth group.
    walk_levers<crd::f32, Layout::RowMajor>({67, 150, 300, Trans::None, Trans::None, true, false}, 1.25F, -0.5F, 11U);
    // op(B) transposed: gemm_parallel_auto's m n k (it once multiplied the storage's dimensions, m k k here).
    walk_levers<crd::f64, Layout::ColMajor>({70, 33, 520, Trans::Transpose, Trans::Transpose, true, false}, -0.75, 1.0,
                                            12U);
    walk_levers<crd::f64, Layout::RowMajor>({45, 97, 263, Trans::Transpose, Trans::None, false, true}, 1.0, 0.0, 13U);
    walk_levers<crd::hesap::Complex32, Layout::ColMajor>(
        {29, 41, 270, Trans::ConjTranspose, Trans::ConjTranspose, true, false}, crd::hesap::Complex32{0.5F, -1.0F},
        crd::hesap::Complex32{0.25F, 0.5F}, 14U);
    // k == 0: C = beta C (serial or row-split), and beta 0 stores zero over a NaN C.
    walk_levers<crd::f32, Layout::RowMajor>({40, 21, 0, Trans::None, Trans::None, false, false}, 1.0F, 0.75F, 15U);
    walk_levers<crd::f64, Layout::ColMajor>({40, 21, 0, Trans::None, Trans::Transpose, false, true}, 1.0, 0.0, 16U);
}
