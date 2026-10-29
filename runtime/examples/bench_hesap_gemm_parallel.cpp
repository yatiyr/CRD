// bench_hesap_gemm_parallel — gemm_parallel's crd-jobs board (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE).
//
// One process per thread count: `--threads N` starts crd::jobs on N workers (default: the jobs default) and times, at
// each product, serial gemm (the control) and gemm_parallel on N jobs down each arm of detail::GemmParallelPick
// (production's levers, the shape's whole Mc block per job, a parallel B pack, always packed, always resident where
// admitted, a row-split beta-only scale), round by round in turn so the arms share the host's state; the median of the
// rounds is reported per product with its speedup over serial (a sample repeats a small product to about 50 MFLOP).
// Every arm's C is gated bit for bit against serial gemm's (ADR-0063: each job writes disjoint rows of C or panels of
// the packed B); a miss exits 1. The products span gemm_parallel_auto's serial threshold (m n k 256K), the resident
// threshold (200M) and the Mc block, square, skinny and with op(B) transposed. The level line names the dense
// kernels' level (CRD_CPU_LEVEL forces it).
//   bench_hesap_gemm_parallel [--threads N]

#include <crd/core/cpu_features.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/dense_dispatch.hpp>
#include <crd/hesap/dense/detail/gemm_arms.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::Trans;
using crd::hesap::dense::detail::GemmMcSplit;
using crd::hesap::dense::detail::GemmParallelPick;
using crd::hesap::dense::detail::GemmSplit;
using Clock = std::chrono::steady_clock;

constexpr int kRounds = 7;
bool g_failed = false;

struct Arm
{
    const char* name;
    GemmParallelPick pick;
};

const Arm kArms[] = {
    {"production", GemmParallelPick{}},
    {"mc-block", GemmParallelPick{.mc = GemmMcSplit::Block}},
    {"pack-b-parallel", GemmParallelPick{.pack_b = GemmSplit::Parallel}},
    {"packed", GemmParallelPick{.resident_mnk = 0}},
    {"resident", GemmParallelPick{.resident_mnk = ~usize{0}}},
    {"beta-only-parallel", GemmParallelPick{.beta_only = GemmSplit::Parallel}},
};
constexpr usize kArmCount = sizeof(kArms) / sizeof(kArms[0]);

struct Product
{
    usize m, n, k;
    Trans tb;
};

f64 median(f64* times, int n)
{
    std::sort(times, times + n);
    return times[n / 2];
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

f64 seconds(Clock::time_point t0)
{
    return std::chrono::duration<f64>(Clock::now() - t0).count();
}

template <typename T> void board(const char* label, const Product& p, u32 workers)
{
    crd::memory::TlsfAllocator alloc(512ULL * 1024ULL * 1024ULL);
    Matrix<T> a(&alloc, p.m, p.k);
    Matrix<T> b(&alloc, p.tb == Trans::None ? p.k : p.n, p.tb == Trans::None ? p.n : p.k);
    Matrix<T> c0(&alloc, p.m, p.n);
    fill(a, 1U);
    fill(b, 2U);
    fill(c0, 3U);
    const T alpha = static_cast<T>(1.25);
    const T beta = static_cast<T>(-0.5);
    const usize bytes = p.m * p.n * sizeof(T);
    // Each sample repeats the product until it holds about 50 MFLOP (C accumulates: serial repeats it alike).
    const f64 flops = 2.0 * static_cast<f64>(p.m) * static_cast<f64>(p.n) * static_cast<f64>(p.k);
    const f64 work = std::max(flops, static_cast<f64>(p.m * p.n));
    const int reps = static_cast<int>(std::clamp(5e7 / work, 1.0, 4096.0));
    Matrix<T> serial(&alloc, p.m, p.n);
    Matrix<T> out(&alloc, p.m, p.n);
    f64 serial_times[kRounds] = {};
    f64 times[kArmCount][kRounds] = {};
    bool live[kArmCount] = {};
    for (usize i = 0; i < kArmCount; ++i)
    {
        live[i] = p.k == 0 || kArms[i].pick.beta_only == GemmSplit::Serial; // the beta-only arm differs at k 0 only
    }
    for (int round = -1; round < kRounds; ++round) // round -1 warms every path and gates it
    {
        std::memcpy(serial.data(), c0.data(), bytes);
        const Clock::time_point s0 = Clock::now();
        for (int r = 0; r < reps; ++r)
        {
            crd::hesap::dense::gemm<T, Layout::RowMajor>(alpha, a.cview(), b.cview(), beta, serial.view(), Trans::None,
                                                         p.tb, &alloc);
        }
        if (round >= 0)
        {
            serial_times[round] = seconds(s0);
        }
        for (usize i = 0; i < kArmCount; ++i)
        {
            if (!live[i])
            {
                continue;
            }
            std::memcpy(out.data(), c0.data(), bytes);
            const Clock::time_point t0 = Clock::now();
            for (int r = 0; r < reps; ++r)
            {
                crd::hesap::dense::detail::gemm_parallel_arm<T, Layout::RowMajor>(
                    kArms[i].pick, workers, alpha, a.cview(), b.cview(), beta, out.view(), Trans::None, p.tb, &alloc);
            }
            if (round >= 0)
            {
                times[i][round] = seconds(t0);
            }
            if (std::memcmp(out.data(), serial.data(), bytes) != 0)
            {
                std::fprintf(stderr, "GATE MISS: %s %zux%zux%zu arm %s: C differs from serial gemm\n", label, p.m, p.n,
                             p.k, kArms[i].name);
                g_failed = true;
                live[i] = false;
            }
        }
    }
    const f64 serial_s = median(serial_times, kRounds) / reps;
    std::printf("%s m %zu n %zu k %zu%s: serial %.4f ms\n", label, p.m, p.n, p.k,
                p.tb == Trans::None ? "" : " op(B) transposed", serial_s * 1e3);
    for (usize i = 0; i < kArmCount; ++i)
    {
        if (!live[i])
        {
            continue;
        }
        const f64 s = median(times[i], kRounds) / reps;
        std::printf("  %-20s %10.4f ms  %8.2f GFLOPS  %5.2fx serial\n", kArms[i].name, s * 1e3,
                    s > 0.0 ? flops / (s * 1e9) : 0.0, s > 0.0 ? serial_s / s : 0.0);
    }
}

template <typename T> void run_type(const char* label, u32 workers)
{
    const Product products[] = {
        {32, 32, 32, Trans::None},    {48, 48, 48, Trans::None},       {64, 64, 64, Trans::None},
        {96, 96, 96, Trans::None},    {128, 128, 128, Trans::None},    {256, 256, 256, Trans::None},
        {512, 512, 512, Trans::None}, {1024, 1024, 1024, Trans::None}, {2048, 2048, 2048, Trans::None},
        {64, 4096, 512, Trans::None}, {4096, 64, 512, Trans::None},    {512, 512, 512, Trans::Transpose},
        {2048, 2048, 0, Trans::None},
    };
    for (const Product& p : products)
    {
        board<T>(label, p, workers);
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
        std::fprintf(stderr, "usage: bench_hesap_gemm_parallel [--threads N]\n");
        return 2;
    }
    crd::jobs::Config cfg;
    if (workers != 0)
    {
        cfg.num_threads = workers;
    }
    crd::jobs::init(cfg);
    const u32 jobs = crd::jobs::num_workers();
    std::printf("[bench_hesap_gemm_parallel] gemm_parallel's levers on %u jobs against serial gemm, median of %d "
                "rounds. hesap-dense level %s (selected %s)\n",
                jobs, kRounds, crd::cpu_level_name(crd::hesap::dense::dense_kernels().level),
                crd::cpu_level_name(crd::cpu_selection().selected));
    run_type<f32>("f32", jobs);
    run_type<f64>("f64", jobs);
    crd::jobs::shutdown();
    if (g_failed)
    {
        std::fprintf(stderr, "bench_hesap_gemm_parallel: a gate failed\n");
        return 1;
    }
    return 0;
}
