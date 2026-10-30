// bench_hesap_autodiff_parallel — batch_gradient's crd-jobs board (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE).
//
// One process per thread count: `--threads N` starts crd::jobs on N workers (default: the jobs default) and times, for
// each loss and batch size, one job (the control) and batch_gradient_arm on N jobs down each arm of
// detail::BatchGradientPick (production's levers: the sample cut below which a batch runs on the calling thread, the
// fold's component split), round by round in turn so the arms share the host's state; the median of the rounds is
// reported with its speedup over one job, and the fold arms show what the kept-serial fold costs. Every arm is gated
// bit for bit against the control (ADR-0097: each sample's gradient on a reset tape, each component summed in
// ascending sample order); a miss exits 1. The tapes share one locked allocator; a tape keeps its arrays across
// resets, so the warm-up round is the only one that allocates.
//   bench_hesap_autodiff_parallel [--threads N]

#include <crd/hesap/autodiff/reverse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
namespace rev = crd::hesap::autodiff::reverse;
using crd::f64;
using crd::u32;
using crd::usize;
using rev::detail::BatchGradientPick;
using Clock = std::chrono::steady_clock;

constexpr int kRounds = 7;
constexpr u32 kMaxJobs = 64;
bool g_failed = false;

struct Arm
{
    const char* name;
    BatchGradientPick pick;
};

// fold_jobs 0 stands for the run's job count.
const Arm kArms[] = {
    {"production", BatchGradientPick{}},
    {"serial-below-64", BatchGradientPick{.jobs_min_samples = 64}},
    {"serial-below-1K", BatchGradientPick{.jobs_min_samples = 1024}},
    {"fold-split-4", BatchGradientPick{.fold_jobs = 4}},
    {"fold-split-jobs", BatchGradientPick{.fold_jobs = 0}},
};

// loss_s: `depth` rounds of h = (theta . x_s + h) / (1 + h^2), then (h - y_s)^2; depth sets a sample's tape length.
struct Loss
{
    const f64* x;
    const f64* y;
    int n;
    int depth;
    rev::Var operator()(const rev::Var* theta, int, int s) const
    {
        const f64* xs = x + static_cast<usize>(s) * static_cast<usize>(n);
        rev::Var h = theta[0] * 0.0;
        for (int d = 0; d < depth; ++d)
        {
            rev::Var acc = h + theta[0] * xs[0];
            for (int i = 1; i < n; ++i)
            {
                acc = acc + theta[i] * xs[i];
            }
            h = acc / (1.0 + acc * acc);
        }
        const rev::Var r = h - y[s];
        return r * r;
    }
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

struct Run
{
    rev::Tape** tapes;
    u32 jobs;
    crd::memory::IAllocator* alloc;
};

void board(const Run& run, int n, int depth, int samples)
{
    crd::containers::Array<f64> x(run.alloc);
    crd::containers::Array<f64> y(run.alloc);
    x.resize(static_cast<usize>(samples) * static_cast<usize>(n));
    y.resize(static_cast<usize>(samples));
    for (usize i = 0; i < x.size(); ++i)
    {
        x[i] = std::sin(0.3 + 1.7 * static_cast<f64>(i));
    }
    for (usize s = 0; s < y.size(); ++s)
    {
        y[s] = 0.4 + 0.2 * std::cos(0.7 + static_cast<f64>(s));
    }
    const Loss loss{x.data(), y.data(), n, depth};
    f64 theta[32];
    for (int i = 0; i < n; ++i)
    {
        theta[i] = 0.5 - 0.03 * i;
    }
    crd::containers::Array<f64> gbuf(run.alloc);
    gbuf.resize(x.size());
    f64 control[32];
    f64 out[32];
    const crd::containers::ConstSpan<f64> th{theta, static_cast<usize>(n)};
    const crd::containers::Span<f64> gb{gbuf.data(), gbuf.size()};
    const crd::containers::Span<rev::Tape*> tapes{run.tapes, run.jobs};

    constexpr usize arm_count = sizeof(kArms) / sizeof(kArms[0]);
    f64 serial_times[kRounds];
    f64 times[arm_count][kRounds];
    bool live[arm_count];
    for (bool& l : live)
    {
        l = true;
    }
    for (int round = -1; round < kRounds; ++round)
    {
        Clock::time_point t0 = Clock::now();
        rev::detail::batch_gradient_arm(BatchGradientPick{}, loss, th, samples, {control, static_cast<usize>(n)}, tapes,
                                        gb, 1U);
        if (round >= 0)
        {
            serial_times[round] = seconds(t0);
        }
        for (usize a = 0; a < arm_count; ++a)
        {
            if (!live[a])
            {
                continue;
            }
            BatchGradientPick pick = kArms[a].pick;
            if (pick.fold_jobs == 0)
            {
                pick.fold_jobs = run.jobs;
            }
            t0 = Clock::now();
            rev::detail::batch_gradient_arm(pick, loss, th, samples, {out, static_cast<usize>(n)}, tapes, gb, run.jobs);
            if (round >= 0)
            {
                times[a][round] = seconds(t0);
            }
            if (std::memcmp(out, control, static_cast<usize>(n) * sizeof(f64)) != 0)
            {
                std::fprintf(stderr, "GATE MISS: n %d depth %d S %d arm %s: the gradient differs from one job's\n", n,
                             depth, samples, kArms[a].name);
                g_failed = true;
                live[a] = false;
            }
        }
    }
    const f64 serial_s = median(serial_times, kRounds);
    std::printf("n %d depth %d S %d: one job %.4f ms\n", n, depth, samples, serial_s * 1e3);
    for (usize a = 0; a < arm_count; ++a)
    {
        if (live[a])
        {
            const f64 s = median(times[a], kRounds);
            std::printf("  %-18s %10.4f ms  %5.2fx one job\n", kArms[a].name, s * 1e3, s > 0.0 ? serial_s / s : 0.0);
        }
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
        std::fprintf(stderr, "usage: bench_hesap_autodiff_parallel [--threads N]\n");
        return 2;
    }
    crd::jobs::Config cfg;
    if (workers != 0)
    {
        cfg.num_threads = workers;
    }
    crd::jobs::init(cfg);
    const u32 jobs = std::min(crd::jobs::num_workers(), kMaxJobs);
    {
        crd::memory::TlsfAllocator base(256U << 20U);
        crd::memory::ThreadSafeAllocator shared(&base);
        crd::containers::Array<rev::Tape> tapes(&shared);
        tapes.reserve(jobs);
        rev::Tape* ptrs[kMaxJobs] = {};
        for (u32 j = 0; j < jobs; ++j)
        {
            ptrs[j] = &tapes.emplace_back(&shared);
        }
        std::printf("[bench_hesap_autodiff_parallel] batch_gradient's levers on %u jobs against one job, median of %d "
                    "rounds\n",
                    jobs, kRounds);
        const Run run{ptrs, jobs, &shared};
        const int sample_counts[] = {16, 256, 4096, 65536};
        for (const int n : {4, 32})
        {
            for (const int depth : {1, 16})
            {
                for (const int samples : sample_counts)
                {
                    board(run, n, depth, samples);
                }
            }
        }
    }
    crd::jobs::shutdown();
    if (g_failed)
    {
        std::fprintf(stderr, "bench_hesap_autodiff_parallel: a gate failed\n");
        return 1;
    }
    return 0;
}
