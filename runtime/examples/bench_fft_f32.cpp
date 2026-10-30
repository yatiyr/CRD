// bench_fft_f32 — the complex-f32 forward FFT against Intel MKL DFTI, one thread (scripts/run_bench_fft.sh, at each CPU
// level). Every power of two from 2^10 to 2^22 on identical data, each plan built once outside timing. At each size
// every forward algorithm the planner offers there (fft::detail::forward_builds: the one this level runs first,
// fft::detail::level_forward, then the Stockham passes, the standalone-hier, IP4-AoS, the three- and four-pass
// deep-split, the four-step, the six-step and the f32 1M tiled four-step) runs in this one binary, interleaved with MKL
// every rep, so all share the host's state; a level's winners become its plans (scripts/gen_fft_level_plans.py reads
// the boards' logs). The input is copied in before each timed transform, never inside the timing (as
// the pre-SIMD-DISPATCH board did). Each Cerid arm and MKL is measured against an FFTW f64 transform of the same f32
// input values and fails above the ADR-0092 §5 f32 class (2e-4, 1e-3 from the four-step band): a fast wrong answer is
// not a win, for either side. Cerid's Complex<f32> shares MKL's interleaved {re, im} layout.

#include <crd/containers/array.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/memory/construct.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fftw3.h>
#include <mkl_dfti.h>
#include <mkl_service.h>

using crd::f32;
using crd::f64;
using crd::usize;
namespace fft = crd::hesap::fft;
namespace cont = crd::containers;
using crd::hesap::Complex;

namespace
{
constexpr usize kLarge = usize{1} << 20;
constexpr usize kMaxArms = sizeof(fft::detail::kFftAlgorithms) / sizeof(fft::detail::kFftAlgorithms[0]);
constexpr const char* kPlanOutOfMemory = "bench_fft_f32: out of memory for a plan";

struct Arm
{
    const char* name;
    const fft::FftPlan<f32>* plan;
};

template <typename Fn> double time_ms(const Fn& fn)
{
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// max |x - truth| / (1 + max |truth|) over n values (test_fft_kernels.cpp's form).
double max_rel(const Complex<f32>* x, const Complex<f64>* truth, usize n)
{
    double maxmag = 0.0;
    double maxdiff = 0.0;
    for (usize i = 0; i < n; ++i)
    {
        maxmag = std::max(maxmag, std::hypot(truth[i].re, truth[i].im));
        const double dre = static_cast<f64>(x[i].re) - truth[i].re;
        const double dim = static_cast<f64>(x[i].im) - truth[i].im;
        maxdiff = std::max(maxdiff, std::hypot(dre, dim));
    }
    return maxdiff / (1.0 + maxmag);
}

// The ADR-0092 §5 f32 class of the fft tests.
double bound(usize n)
{
    return n >= (usize{1} << 19) ? 1e-3 : 2e-4;
}

// One size: every arm and MKL timed best of `reps`, interleaved per rep, and gated against the f64 truth. Returns
// false when an error is above the bound.
bool board(crd::memory::IAllocator* alloc, int lg, const Arm* arms, usize arm_count)
{
    const usize n = usize{1} << lg;
    int reps = 8;
    if (n <= (usize{1} << 16))
    {
        reps = 200;
    }
    else if (n <= kLarge)
    {
        reps = 30;
    }

    // Deterministic input in [-1, 1), as the f64 board's.
    cont::Array<Complex<f32>> base(alloc);
    base.resize(n);
    crd::u64 s = 0x12345 ^ n;
    for (usize i = 0; i < n; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const double a = static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53) * 2.0 - 1.0;
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const double b = static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53) * 2.0 - 1.0;
        base[i] = Complex<f32>{static_cast<f32>(a), static_cast<f32>(b)};
    }

    // The truth: FFTW f64 of the same f32 values, widened exactly (planned before the buffers are filled).
    cont::Array<Complex<f64>> wide(alloc);
    wide.resize(n);
    cont::Array<Complex<f64>> truth(alloc);
    truth.resize(n);
    fftw_plan tp = fftw_plan_dft_1d(static_cast<int>(n), reinterpret_cast<fftw_complex*>(wide.data()),
                                    reinterpret_cast<fftw_complex*>(truth.data()), FFTW_FORWARD, FFTW_ESTIMATE);
    for (usize i = 0; i < n; ++i)
    {
        wide[i] = Complex<f64>{static_cast<f64>(base[i].re), static_cast<f64>(base[i].im)};
    }
    fftw_execute(tp);
    fftw_destroy_plan(tp);

    cont::Array<Complex<f32>> work(alloc);
    work.resize(n);
    auto reset = [&]
    {
        std::memcpy(work.data(), base.data(), n * sizeof(Complex<f32>));
    };

    DFTI_DESCRIPTOR_HANDLE h = nullptr;
    DftiCreateDescriptor(&h, DFTI_SINGLE, DFTI_COMPLEX, 1, static_cast<MKL_LONG>(n));
    DftiSetValue(h, DFTI_PLACEMENT, DFTI_INPLACE);
    DftiCommitDescriptor(h);

    // Each side's error, from one untimed transform.
    double err[kMaxArms + 1] = {};
    for (usize a = 0; a < arm_count; ++a)
    {
        reset();
        arms[a].plan->execute(cont::Span<Complex<f32>>(work.data(), n), fft::FftDirection::Forward);
        err[a] = max_rel(work.data(), truth.data(), n);
    }
    reset();
    DftiComputeForward(h, work.data());
    err[arm_count] = max_rel(work.data(), truth.data(), n);

    double best[kMaxArms + 1] = {};
    std::fill(best, best + arm_count + 1, 1e300);
    for (int r = 0; r < reps; ++r)
    {
        for (usize a = 0; a < arm_count; ++a)
        {
            reset();
            const double ms = time_ms(
                [&] { arms[a].plan->execute(cont::Span<Complex<f32>>(work.data(), n), fft::FftDirection::Forward); });
            best[a] = std::min(best[a], ms);
        }
        reset();
        const double ms = time_ms([&] { DftiComputeForward(h, work.data()); });
        best[arm_count] = std::min(best[arm_count], ms);
    }
    DftiFreeDescriptor(&h);

    const double mkl = best[arm_count];
    const double limit = bound(n);
    bool ok = err[arm_count] <= limit;
    usize winner = 0;
    for (usize a = 0; a < arm_count; ++a)
    {
        std::printf("%-9zu | %-15s | %9.3f | %9.3f | %6.2fx | %.1e | %.1e\n", n, arms[a].name, best[a], mkl,
                    mkl / best[a], err[a], err[arm_count]);
        ok = ok && err[a] <= limit;
        winner = best[a] < best[winner] ? a : winner;
    }
    if (arm_count > 1)
    {
        std::printf("n=%zu winner: %s at %.2fx MKL\n", n, arms[winner].name, mkl / best[winner]);
    }
    // The canonical planner's forward (CrossPlatform's on every level), which the plan generator compares against.
    std::printf("n=%zu canonical: %s\n", n, fft::fft_algorithm_name(fft::detail::planned_forward<f32>(n)));
    if (!ok)
    {
        std::fprintf(stderr, "n=%zu: an error against the f64 truth is above the bound %.1e\n", n, limit);
    }
    return ok;
}
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(2ULL << 30);
    char version[256] = {};
    mkl_get_version_string(version, static_cast<int>(sizeof(version)));
    std::printf("MKL: %s\n", version);
    std::printf("fft kernel level: %s\n", crd::cpu_level_name(fft::fft_kernels().level)); // the level's own plans
    std::printf("=== complex-f32 forward FFT, best of reps (ms); ratio = MKL / Cerid; errors against FFTW f64 ===\n");
    std::printf("%-9s | %-15s | %9s | %9s | %7s | %-7s | %s\n", "n", "Cerid arm", "Cerid", "MKL", "ratio", "Cerid err",
                "MKL err");

    bool ok = true;
    for (int lg = 10; lg <= 22; ++lg)
    {
        const usize n = usize{1} << lg;
        Arm arms[kMaxArms] = {};
        fft::FftPlan<f32>* plans[kMaxArms] = {};
        usize arm_count = 0;
        const fft::FftAlgorithm planned =
            fft::detail::level_forward<f32>(n, fft::fft_kernels().level, crd::math::kernel_determinism());
        const auto add = [&](fft::FftAlgorithm forward)
        {
            plans[arm_count] =
                crd::memory::construct_or_fatal<fft::FftPlan<f32>>(alloc, kPlanOutOfMemory, &alloc, n, forward);
            arms[arm_count] = Arm{fft::fft_algorithm_name(forward), plans[arm_count]};
            ++arm_count;
        };
        add(planned);
        for (const fft::FftAlgorithm forward : fft::detail::kFftAlgorithms)
        {
            if (forward != planned && fft::detail::forward_builds<f32>(n, forward))
            {
                add(forward);
            }
        }
        ok = board(&alloc, lg, arms, arm_count) && ok;
        for (usize a = arm_count; a-- > 0;)
        {
            crd::memory::destroy(alloc, plans[a]);
        }
    }
    return ok ? 0 : 1;
}
