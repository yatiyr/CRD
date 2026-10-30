// bench_hesap_fft_parallel — the batched transforms' crd-jobs board (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE).
//
// One process per thread count: `--threads N` starts crd::jobs on N workers (default: the jobs default) and times one
// job (the control) and N jobs down each arm of fft::detail::BatchPick (production's levers: the transform-count cut,
// the work cut, the forced jobs), round by round in turn so the arms share the host's state; the median of the rounds
// is reported with its speedup over one job. Workloads: hesap-dsp's segment periodograms (Welch, the spectrogram) and
// stft, hesap-wavelet's cwt and NdFftPlan's axis lines (on a plan of N job sets; production's plan holds one, so its
// control is today's serial transform). Every arm is gated bit for bit against the control (each transform writes only
// its own output); a miss exits 1.
//   bench_hesap_fft_parallel [--threads N]

#include <crd/hesap/dsp/spectral.hpp>
#include <crd/hesap/fft/detail/batch_arms.hpp>
#include <crd/hesap/fft/nd_fft.hpp>
#include <crd/hesap/wavelet/cwt.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::hesap::Complex;
using crd::hesap::fft::detail::BatchPick;
namespace cont = crd::containers;
namespace fft = crd::hesap::fft;
using Clock = std::chrono::steady_clock;

constexpr int kRounds = 7;
bool g_failed = false;

struct Arm
{
    const char* name;
    BatchPick pick;
};

const Arm kArms[] = {
    {"production", BatchPick{}},
    {"every-batch", BatchPick{.min_transforms = 0}},
    {"work-from-256K", BatchPick{.min_work = usize{1} << 18U}},
    {"work-from-4M", BatchPick{.min_work = usize{1} << 22U}},
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

// Times `run(pick)` (its result's bytes in `out`) for one job and every arm on `workers` forced jobs, gating each
// arm's bytes against one job's.
template <typename Run>
void board(const char* label, Run run, cont::Array<unsigned char>& out, cont::Array<unsigned char>& control,
           u32 workers)
{
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
        run(BatchPick{.min_transforms = 0, .workers = 1}, true);
        if (round >= 0)
        {
            serial_times[round] = seconds(t0);
        }
        control.resize(out.size());
        std::memcpy(control.data(), out.data(), out.size());
        for (usize a = 0; a < arm_count; ++a)
        {
            if (!live[a])
            {
                continue;
            }
            BatchPick pick = kArms[a].pick;
            pick.workers = workers;
            std::memset(out.data(), 0, out.size());
            t0 = Clock::now();
            run(pick, false);
            if (round >= 0)
            {
                times[a][round] = seconds(t0);
            }
            if (out.size() != control.size() || std::memcmp(out.data(), control.data(), out.size()) != 0)
            {
                std::fprintf(stderr, "GATE MISS: %s arm %s: the outputs differ from one job's\n", label, kArms[a].name);
                g_failed = true;
                live[a] = false;
            }
        }
    }
    const f64 serial_s = median(serial_times, kRounds);
    std::printf("%s: one job %.4f ms\n", label, serial_s * 1e3);
    for (usize a = 0; a < arm_count; ++a)
    {
        if (live[a])
        {
            const f64 s = median(times[a], kRounds);
            std::printf("  %-18s %10.4f ms  %5.2fx one job\n", kArms[a].name, s * 1e3, s > 0.0 ? serial_s / s : 0.0);
        }
    }
}

template <typename T> void keep(cont::Array<unsigned char>& out, const T* data, usize count)
{
    out.resize(count * sizeof(T));
    std::memcpy(out.data(), data, count * sizeof(T));
}

template <typename T> void signal(cont::Array<T>& x, usize n)
{
    x.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        const f64 t = static_cast<f64>(i);
        x[i] = static_cast<T>(std::sin(0.0137 * t) + 0.3 * std::cos(0.731 * t) + 0.05 * std::sin(2.9 * t));
    }
}

void segments(crd::memory::IAllocator* alloc, cont::Array<unsigned char>& out, cont::Array<unsigned char>& control,
              u32 jobs)
{
    char label[96];
    cont::Array<f64> x(alloc);
    signal(x, usize{1} << 20U);
    for (const usize nperseg : {usize{256}, usize{4096}})
    {
        std::snprintf(label, sizeof(label), "segment periodograms f64 1M samples nperseg %zu", nperseg);
        board(
            label,
            [&](const BatchPick& pick, bool)
            {
                usize nseg = 0;
                usize nfreq = 0;
                const auto pg = crd::hesap::dsp::detail::segment_periodograms_arm<f64>(
                    alloc, cont::ConstSpan<f64>(x.data(), x.size()), 1000.0, nperseg, nperseg / 2, nseg, nfreq, pick);
                keep(out, pg.data(), pg.size());
            },
            out, control, jobs);
    }
    cont::Array<f32> xf(alloc);
    signal(xf, usize{1} << 20U);
    for (const usize nperseg : {usize{64}, usize{1024}})
    {
        std::snprintf(label, sizeof(label), "stft f32 1M samples nperseg %zu", nperseg);
        board(
            label,
            [&](const BatchPick& pick, bool)
            {
                usize nframes = 0;
                usize nfreq = 0;
                const auto z = crd::hesap::dsp::detail::stft_arm<f32>(alloc, cont::ConstSpan<f32>(xf.data(), xf.size()),
                                                                      nperseg, nperseg / 2, nframes, nfreq, pick);
                keep(out, z.data(), z.size());
            },
            out, control, jobs);
    }
}

void scales(crd::memory::IAllocator* alloc, cont::Array<unsigned char>& out, cont::Array<unsigned char>& control,
            u32 jobs)
{
    namespace wv = crd::hesap::wavelet;
    const auto w = wv::continuous_wavelet("cmor1.5-1.0");
    char label[96];
    for (const usize n : {usize{1024}, usize{16384}})
    {
        cont::Array<f64> x(alloc);
        signal(x, n);
        cont::Array<f64> sc(alloc);
        sc.resize(64);
        for (usize s = 0; s < sc.size(); ++s)
        {
            sc[s] = 1.0 + 0.5 * static_cast<f64>(s);
        }
        std::snprintf(label, sizeof(label), "cwt f64 n %zu, 64 scales", n);
        board(
            label,
            [&](const BatchPick& pick, bool)
            {
                const auto r = wv::detail::cwt_arm<f64>(alloc, cont::ConstSpan<f64>(x.data(), x.size()),
                                                        cont::ConstSpan<f64>(sc.data(), sc.size()), w, 1.0, 12, pick);
                keep(out, r.coeffs.data(), r.coeffs.size());
            },
            out, control, jobs);
    }
}

template <typename T>
void nd(crd::memory::IAllocator* alloc, const char* type, const usize* dims, usize ndim,
        cont::Array<unsigned char>& out, cont::Array<unsigned char>& control, u32 jobs)
{
    usize total = 1;
    for (usize i = 0; i < ndim; ++i)
    {
        total *= dims[i];
    }
    cont::Array<Complex<T>> x(alloc);
    cont::Array<Complex<T>> work(alloc);
    x.resize(total);
    work.resize(total);
    for (usize i = 0; i < total; ++i)
    {
        const f64 t = static_cast<f64>(i);
        x[i] = Complex<T>{static_cast<T>(std::sin(0.37 * t)), static_cast<T>(std::cos(1.13 * t))};
    }
    const fft::NdFftPlan<T> serial(alloc, cont::ConstSpan<usize>(dims, ndim));
    const fft::NdFftPlan<T> sets(alloc, cont::ConstSpan<usize>(dims, ndim), jobs);
    char label[96];
    if (ndim == 2)
    {
        std::snprintf(label, sizeof(label), "NdFftPlan %s %zu x %zu", type, dims[0], dims[1]);
    }
    else
    {
        std::snprintf(label, sizeof(label), "NdFftPlan %s %zu x %zu x %zu", type, dims[0], dims[1], dims[2]);
    }
    board(
        label,
        [&](const BatchPick& pick, bool control_run)
        {
            std::memcpy(work.data(), x.data(), total * sizeof(Complex<T>));
            const fft::NdFftPlan<T>& plan = control_run ? serial : sets;
            plan.execute_arm(cont::Span<Complex<T>>(work.data(), total), fft::FftDirection::Forward, pick);
            keep(out, work.data(), total);
        },
        out, control, jobs);
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
        std::fprintf(stderr, "usage: bench_hesap_fft_parallel [--threads N]\n");
        return 2;
    }
    crd::jobs::Config cfg;
    if (workers != 0)
    {
        cfg.num_threads = workers;
    }
    crd::jobs::init(cfg);
    const u32 jobs = crd::jobs::num_workers();
    std::printf("[bench_hesap_fft_parallel] the batched transforms' levers on %u jobs against one job, median of %d "
                "rounds\n",
                jobs, kRounds);
    {
        crd::memory::TlsfAllocator alloc(1024U << 20U);
        cont::Array<unsigned char> out(&alloc);
        cont::Array<unsigned char> control(&alloc);
        segments(&alloc, out, control, jobs);
        scales(&alloc, out, control, jobs);
        const usize square[2] = {512, 512};
        const usize cube[3] = {64, 64, 64};
        const usize bluestein[2] = {1000, 768};
        const usize wide[2] = {1024, 1024};
        nd<f64>(&alloc, "f64", square, 2, out, control, jobs);
        nd<f64>(&alloc, "f64", cube, 3, out, control, jobs);
        nd<f64>(&alloc, "f64", bluestein, 2, out, control, jobs);
        nd<f32>(&alloc, "f32", wide, 2, out, control, jobs);
    }
    crd::jobs::shutdown();
    if (g_failed)
    {
        std::fprintf(stderr, "bench_hesap_fft_parallel: a gate failed\n");
        return 1;
    }
    return 0;
}
