// bench_hesap_factor_parallel — the dense factors' crd-jobs board (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE).
//
// One process per thread count: `--threads N` starts crd::jobs on N workers (default: the jobs default) and times, at
// each size, the one-job factor (the control) and factor_lu_arm / factor_cholesky_arm on N jobs down each arm of
// detail::LuPick and detail::CholeskyPick (production's levers; LU's trailing update serial below gemm_parallel_auto's
// 256K and its U12 solve split by columns; Cholesky's L21 solve and trailing syrk always or never split; the panel
// width and the unblocked cut), round by round in turn so the arms share the host's state; the median of the rounds
// is reported per size with its speedup over one job. Arms that keep the panel width and the cut are gated bit for
// bit against the control (ADR-0063: each job solves disjoint rows or columns); the others regroup the trailing sums
// and are gated by the factor's elementwise backward-error bound, |PA - LU| <= 4 n eps |L| |U| (|A - L L^T| for
// Cholesky). A miss exits 1. The level line names the dense kernels' level (CRD_CPU_LEVEL forces it).
//   bench_hesap_factor_parallel [--threads N]

#include <crd/core/cpu_features.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/dense_dispatch.hpp>
#include <crd/hesap/dense/detail/factor_arms.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace
{
using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::hesap::dense::Cholesky;
using crd::hesap::dense::Layout;
using crd::hesap::dense::LU;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::Trans;
using crd::hesap::dense::detail::CholeskyPick;
using crd::hesap::dense::detail::LuPick;
using Clock = std::chrono::steady_clock;

constexpr int kRounds = 7;
constexpr usize kNever = ~usize{0};
bool g_failed = false;

template <typename Pick> struct Arm
{
    const char* name;
    Pick pick;
    bool same_bits; // the control's panel width and cut: gated bit for bit, otherwise by the backward-error bound
};

const Arm<LuPick> kLuArms[] = {
    {"production", LuPick{}, true},
    {"trailing-serial-256K", LuPick{.trailing_min = 256U * 1024U}, true},
    {"trsm-columns-64K", LuPick{.trsm_min = 64U * 1024U}, true},
    {"trsm-columns-all", LuPick{.trsm_min = 0}, true},
    {"block-32", LuPick{.block = 32}, false},
    {"block-128", LuPick{.block = 128}, false},
};

const Arm<CholeskyPick> kCholeskyArms[] = {
    {"production", CholeskyPick{}, true},
    {"trsm-rows-all", CholeskyPick{.trsm_min = 0}, true},
    {"trsm-rows-never", CholeskyPick{.trsm_min = kNever}, true},
    {"syrk-split-all", CholeskyPick{.syrk_mmk = 0}, true},
    {"syrk-split-never", CholeskyPick{.syrk_mmk = kNever}, true},
    {"unblocked-to-128", CholeskyPick{.unblocked_max = 128}, false},
    {"unblocked-to-512", CholeskyPick{.unblocked_max = 512}, false},
    {"block-32", CholeskyPick{.block = 32}, false},
    {"block-128", CholeskyPick{.block = 128}, false},
};

f64 median(f64* times, int n)
{
    std::sort(times, times + n);
    return times[n / 2];
}

f64 seconds(Clock::time_point t0)
{
    return std::chrono::duration<f64>(Clock::now() - t0).count();
}

template <typename T> void fill(Matrix<T>& x, u32 seed)
{
    u32 s = seed;
    for (usize i = 0; i < x.rows(); ++i)
    {
        for (usize j = 0; j < x.cols(); ++j)
        {
            s = s * 1664525U + 1013904223U;
            x(i, j) = static_cast<T>(static_cast<f64>(s >> 8) / 16777216.0 - 0.5);
        }
    }
}

template <typename T> void copy_into(const Matrix<T>& a, Matrix<T>& to)
{
    for (usize i = 0; i < a.rows(); ++i)
    {
        std::memcpy(&to(i, 0), &a(i, 0), a.cols() * sizeof(T));
    }
}

// The elementwise bound check: |want - lr| <= 4 n eps bound, with lr = op product and bound its magnitudes' product.
template <typename T>
bool within_bound(const Matrix<T>& want, const Matrix<T>& lr, const Matrix<T>& bound, usize n, bool lower_only)
{
    const f64 tol = 4.0 * static_cast<f64>(n) * static_cast<f64>(std::numeric_limits<T>::epsilon());
    for (usize i = 0; i < n; ++i)
    {
        for (usize j = 0; j < (lower_only ? i + 1 : n); ++j)
        {
            const f64 err = std::fabs(static_cast<f64>(want(i, j)) - static_cast<f64>(lr(i, j)));
            if (!(err <= tol * static_cast<f64>(bound(i, j)) + std::numeric_limits<f64>::min()))
            {
                return false;
            }
        }
    }
    return true;
}

// |PA - LU| <= 4 n eps |L| |U| elementwise (Higham 9.3), the products formed by gemm.
template <typename T> bool lu_backward_stable(const LU<T>& lu, const Matrix<T>& a, crd::memory::IAllocator* alloc)
{
    const usize n = lu.n();
    if (lu.info() != 0)
    {
        return false;
    }
    Matrix<T> l(alloc, n, n);
    Matrix<T> u(alloc, n, n);
    Matrix<T> al(alloc, n, n);
    Matrix<T> au(alloc, n, n);
    for (usize i = 0; i < n; ++i)
    {
        for (usize j = 0; j < n; ++j)
        {
            const T x = lu.packed()(i, j);
            l(i, j) = j < i ? x : T{0};
            u(i, j) = j >= i ? x : T{0};
            al(i, j) = std::fabs(l(i, j));
            au(i, j) = std::fabs(u(i, j));
        }
        l(i, i) = T{1};
        al(i, i) = T{1};
    }
    Matrix<T> lr(alloc, n, n);
    Matrix<T> bound(alloc, n, n);
    crd::hesap::dense::gemm<T, Layout::RowMajor>(T{1}, l.cview(), u.cview(), T{0}, lr.view(), Trans::None, Trans::None,
                                                 alloc);
    crd::hesap::dense::gemm<T, Layout::RowMajor>(T{1}, al.cview(), au.cview(), T{0}, bound.view(), Trans::None,
                                                 Trans::None, alloc);
    // P A: the pivots replayed in order on the rows of A.
    Matrix<T> pa(alloc, n, n);
    copy_into(a, pa);
    for (usize k = 0; k < n; ++k)
    {
        const usize r = lu.permutation().pivot_at(k);
        for (usize j = 0; r != k && j < n; ++j)
        {
            std::swap(pa(k, j), pa(r, j));
        }
    }
    return within_bound(pa, lr, bound, n, false);
}

// |A - L L^T| <= 4 n eps |L| |L^T| elementwise on the lower triangle (Higham 10.3).
template <typename T>
bool cholesky_backward_stable(const Cholesky<T>& chol, const Matrix<T>& a, crd::memory::IAllocator* alloc)
{
    const usize n = chol.n();
    if (chol.info() != 0)
    {
        return false;
    }
    Matrix<T> l(alloc, n, n);
    Matrix<T> al(alloc, n, n);
    for (usize i = 0; i < n; ++i)
    {
        for (usize j = 0; j < n; ++j)
        {
            l(i, j) = j <= i ? chol.packed()(i, j) : T{0};
            al(i, j) = std::fabs(l(i, j));
        }
    }
    Matrix<T> lr(alloc, n, n);
    Matrix<T> bound(alloc, n, n);
    crd::hesap::dense::gemm<T, Layout::RowMajor>(T{1}, l.cview(), l.cview(), T{0}, lr.view(), Trans::None,
                                                 Trans::Transpose, alloc);
    crd::hesap::dense::gemm<T, Layout::RowMajor>(T{1}, al.cview(), al.cview(), T{0}, bound.view(), Trans::None,
                                                 Trans::Transpose, alloc);
    return within_bound(a, lr, bound, n, true);
}

// One factor of a's copy (copy untimed); returns the factor's seconds.
template <typename T> f64 time_factor(const LuPick& pick, const Matrix<T>& a, LU<T>& lu, crd::memory::IAllocator* m)
{
    copy_into(a, lu.packed());
    const Clock::time_point t0 = Clock::now();
    crd::hesap::dense::detail::factor_lu_arm<T, Layout::RowMajor>(pick, lu, m);
    return seconds(t0);
}

template <typename T>
f64 time_factor(const CholeskyPick& pick, const Matrix<T>& a, Cholesky<T>& chol, crd::memory::IAllocator* m)
{
    copy_into(a, chol.packed());
    const Clock::time_point t0 = Clock::now();
    crd::hesap::dense::detail::factor_cholesky_arm<T, Layout::RowMajor>(pick, chol, m);
    return seconds(t0);
}

template <typename T> bool same_bits(const LU<T>& x, const LU<T>& y)
{
    const usize n = x.n();
    if (x.info() != y.info() || std::memcmp(x.packed().data(), y.packed().data(), n * x.packed().ld() * sizeof(T)) != 0)
    {
        return false;
    }
    for (usize i = 0; i < n; ++i)
    {
        if (x.permutation().pivot_at(i) != y.permutation().pivot_at(i))
        {
            return false;
        }
    }
    return true;
}

template <typename T> bool same_bits(const Cholesky<T>& x, const Cholesky<T>& y)
{
    if (x.info() != y.info())
    {
        return false;
    }
    for (usize i = 0; i < x.n(); ++i)
    {
        if (std::memcmp(&x.packed()(i, 0), &y.packed()(i, 0), (i + 1) * sizeof(T)) != 0)
        {
            return false;
        }
    }
    return true;
}

template <typename T> bool backward_stable(const LU<T>& f, const Matrix<T>& a, crd::memory::IAllocator* alloc)
{
    return lu_backward_stable(f, a, alloc);
}

template <typename T> bool backward_stable(const Cholesky<T>& f, const Matrix<T>& a, crd::memory::IAllocator* alloc)
{
    return cholesky_backward_stable(f, a, alloc);
}

// The board of one factor at one size: Factor is LU<T> or Cholesky<T>, a its input (A, or the SPD A in full).
template <typename T, typename Factor, typename Pick, usize N>
void board(const char* label, const char* factor, const Arm<Pick> (&arms)[N], const Matrix<T>& a, f64 flops,
           u32 workers, crd::memory::IAllocator* alloc)
{
    const usize n = a.rows();
    const int reps = static_cast<int>(std::clamp(5e7 / flops, 1.0, 256.0));
    Pick serial_pick{};
    serial_pick.workers = 1;
    Factor control(alloc, n);
    Factor out(alloc, n);
    f64 serial_times[kRounds] = {};
    f64 times[N][kRounds] = {};
    bool live[N] = {};
    std::fill(live, live + N, true);
    for (int round = -1; round < kRounds; ++round) // round -1 warms every path and gates it
    {
        f64 s = 0.0;
        for (int r = 0; r < reps; ++r)
        {
            s += time_factor(serial_pick, a, control, alloc);
        }
        if (round >= 0)
        {
            serial_times[round] = s;
        }
        for (usize i = 0; i < N; ++i)
        {
            if (!live[i])
            {
                continue;
            }
            Pick pick = arms[i].pick;
            pick.workers = workers;
            f64 t = 0.0;
            for (int r = 0; r < reps; ++r)
            {
                t += time_factor(pick, a, out, alloc);
            }
            if (round >= 0)
            {
                times[i][round] = t;
            }
            if (round == -1 && !(arms[i].same_bits ? same_bits(out, control) : backward_stable(out, a, alloc)))
            {
                std::fprintf(stderr, "GATE MISS: %s %s n %zu arm %s: %s\n", label, factor, n, arms[i].name,
                             arms[i].same_bits ? "the factor differs from one job's" : "past the backward-error bound");
                g_failed = true;
                live[i] = false;
            }
        }
    }
    if (!backward_stable(control, a, alloc))
    {
        std::fprintf(stderr, "GATE MISS: %s %s n %zu: one job's factor is past the backward-error bound\n", label,
                     factor, n);
        g_failed = true;
    }
    const f64 serial_s = median(serial_times, kRounds) / reps;
    std::printf("%s %s n %zu: one job %.4f ms\n", label, factor, n, serial_s * 1e3);
    for (usize i = 0; i < N; ++i)
    {
        if (!live[i])
        {
            continue;
        }
        const f64 s = median(times[i], kRounds) / reps;
        std::printf("  %-22s %10.4f ms  %8.2f GFLOPS  %5.2fx one job\n", arms[i].name, s * 1e3,
                    s > 0.0 ? flops / (s * 1e9) : 0.0, s > 0.0 ? serial_s / s : 0.0);
    }
}

template <typename T> void run_type(const char* label, u32 workers)
{
    const usize sizes[] = {64, 128, 256, 384, 512, 1024, 2048};
    for (const usize n : sizes)
    {
        crd::memory::TlsfAllocator alloc(1024ULL * 1024ULL * 1024ULL);
        const f64 nd = static_cast<f64>(n);
        Matrix<T> a(&alloc, n, n);
        fill(a, 1U + static_cast<u32>(n));
        board<T, LU<T>>(label, "LU", kLuArms, a, 2.0 * nd * nd * nd / 3.0, workers, &alloc);
        // The SPD A = B B^T + n I, its upper triangle zero as factor_cholesky's Symmetric form leaves it.
        Matrix<T> spd(&alloc, n, n);
        crd::hesap::dense::gemm<T, Layout::RowMajor>(T{1}, a.cview(), a.cview(), T{0}, spd.view(), Trans::None,
                                                     Trans::Transpose, &alloc);
        for (usize i = 0; i < n; ++i)
        {
            spd(i, i) += static_cast<T>(n);
            for (usize j = i + 1; j < n; ++j)
            {
                spd(i, j) = T{0};
            }
        }
        board<T, Cholesky<T>>(label, "Cholesky", kCholeskyArms, spd, nd * nd * nd / 3.0, workers, &alloc);
    }
}
} // namespace

int main(int argc, char** argv)
{
    u32 workers = 0;
    if (argc == 3 && std::strcmp(argv[1], "--threads") == 0)
    {
        workers = static_cast<u32>(std::strtoul(argv[2], nullptr, 10));
    }
    else if (argc != 1)
    {
        std::fprintf(stderr, "usage: bench_hesap_factor_parallel [--threads N]\n");
        return 2;
    }
    crd::jobs::Config cfg;
    if (workers != 0)
    {
        cfg.num_threads = workers;
    }
    crd::jobs::init(cfg);
    const u32 jobs = crd::jobs::num_workers();
    std::printf("[bench_hesap_factor_parallel] the dense factors' levers on %u jobs against one job, median of %d "
                "rounds. hesap-dense level %s (selected %s)\n",
                jobs, kRounds, crd::cpu_level_name(crd::hesap::dense::dense_kernels().level),
                crd::cpu_level_name(crd::cpu_selection().selected));
    run_type<f32>("f32", jobs);
    run_type<f64>("f64", jobs);
    crd::jobs::shutdown();
    if (g_failed)
    {
        std::fprintf(stderr, "bench_hesap_factor_parallel: a gate failed\n");
        return 1;
    }
    return 0;
}
