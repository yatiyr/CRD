// bench_fft_f32 — the complex-f32 forward FFT against Intel MKL DFTI, one thread (scripts/run_bench_fft.sh, at each CPU
// level). Every power of two from 2^10 to 2^22 on identical data, each plan built once outside timing. At each size
// every forward algorithm the planner offers there (fft::detail::forward_builds: the one this level runs first,
// fft::detail::level_forward, then the Stockham passes, the standalone-hier, IP4-AoS, the three- and four-pass
// deep-split, the four-step, the six-step, the f32 1M tiled four-step and the f32 2K to 8K in-place three passes) runs
// in this one binary, interleaved with MKL every rep, so all share the host's state; a level's winners become its plans
// (scripts/gen_fft_level_plans.py reads the boards' logs). The input is copied in before each timed transform, never
// inside the timing (as the pre-SIMD-DISPATCH board did), and each timed transform follows an untimed one of the same
// side, so it runs warm, as in a stream of transforms (a cold transform after the other sides ranked the arms apart
// from the profile loop: 2026-11-01). Each Cerid arm and MKL is measured against an FFTW f64 transform of the same f32
// input values and fails above the ADR-0092 §5 f32 class (2e-4, 1e-3 from the four-step band): a fast wrong answer is
// not a win, for either side. Each Cerid row ends with the FNV-1a digest of that gated transform's output (`bits`), so
// a board of two builds shows they compute the same bits (ab_fft_f32.sh). Cerid's Complex<f32> shares MKL's interleaved
// {re, im} layout. Every side transforms one buffer at a declared alignment (--offset), never at the one the
// allocator's history gave.
//
//   bench_fft_f32 [--sizes A-B] [--planned] [--offset B]
//       the board over 2^A to 2^B (default 10-22); --planned times only the level's planned forward beside MKL (the
//       fast A/B board's arms, scripts/ab-board.py)
//   bench_fft_f32 --loop <side> --sizes A-A --iterations K [--distribution] [--offset B]
//       the profile loop: one side's forward at 2^A, K times, nothing else in the loop but the input copy each
//       transform starts from (as the board's); <side> is planned, an algorithm name the planner offers there, mkl,
//       or copy (the input copy alone: the control whose counters perf stat subtracts). The side is gated against the
//       FFTW f64 truth first, and its line ends with the FNV-1a digest of that transform's output (`bits`: two builds
//       of one side compute the same bits when they match); a CRD_HESAP_FFT_PROFILE tree adds the phase counters.
//       --distribution also times each forward alone (its copy untimed, as the board's) and prints the stream's
//       per-transform minimum, 10th percentile, median, 90th percentile, mean and maximum: the board's statistic
//       (the best warm transform) and the loop's (the mean of a stream) side by side, from one stream.
//   --offset B starts the transformed buffer B bytes past a 64-byte boundary (whole complex values, below 4096;
//   default 0, a cache-line-aligned buffer), on the board and in the loop: a side's sensitivity to its caller's
//   alignment (16 is a malloc'd buffer's) and placement. docs/bench/README.md#fast-ab-boards.

#include <crd/containers/array.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/core/fnv1a.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/memory/construct.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
constexpr int kFirstLg = 10;
constexpr int kLastLg = 22;
constexpr usize kMaxOffset = 4096; // --offset: below one page, in whole complex values past a cache line
constexpr usize kLine = 64;

struct Arm
{
    const char* name;
    const fft::FftPlan<f32>* plan;
};

struct Options
{
    int lg_first = kFirstLg;
    int lg_last = kLastLg;
    bool planned_only = false;
    const char* loop_side = nullptr;
    long iterations = 0;
    bool distribution = false;
    usize offset = 0;
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

// One size's identical data: the deterministic input in [-1, 1) (the f64 board's) and its truth, FFTW f64 of the same
// f32 values widened exactly (planned before the buffers are filled).
struct Data
{
    cont::Array<Complex<f32>> base;
    cont::Array<Complex<f64>> truth;

    Data(crd::memory::IAllocator* alloc, usize n) : base(alloc), truth(alloc)
    {
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
        cont::Array<Complex<f64>> wide(alloc);
        wide.resize(n);
        truth.resize(n);
        fftw_plan tp = fftw_plan_dft_1d(static_cast<int>(n), reinterpret_cast<fftw_complex*>(wide.data()),
                                        reinterpret_cast<fftw_complex*>(truth.data()), FFTW_FORWARD, FFTW_ESTIMATE);
        for (usize i = 0; i < n; ++i)
        {
            wide[i] = Complex<f64>{static_cast<f64>(base[i].re), static_cast<f64>(base[i].im)};
        }
        fftw_execute(tp);
        fftw_destroy_plan(tp);
    }
};

DFTI_DESCRIPTOR_HANDLE mkl_plan(usize n)
{
    DFTI_DESCRIPTOR_HANDLE h = nullptr;
    DftiCreateDescriptor(&h, DFTI_SINGLE, DFTI_COMPLEX, 1, static_cast<MKL_LONG>(n));
    DftiSetValue(h, DFTI_PLACEMENT, DFTI_INPLACE);
    DftiCommitDescriptor(h);
    return h;
}

// The transformed buffer of n values in `work`, `offset` bytes past a 64-byte boundary (--offset): the board and the
// loop time every side at one declared alignment, not at the one the allocator's history gave (a 16-byte-aligned
// buffer ran the 8K forward 1.2x to 1.6x slower than a 64-byte-aligned one on the benchmark machine, MKL's 1.3x:
// 2026-11-01).
Complex<f32>* place(cont::Array<Complex<f32>>& work, usize n, usize offset)
{
    work.resize(n + (kMaxOffset + kLine) / sizeof(Complex<f32>));
    const auto start = reinterpret_cast<std::uintptr_t>(work.data());
    return work.data() + ((kLine - start % kLine) % kLine + offset) / sizeof(Complex<f32>);
}

// One size: every arm and MKL timed best of `reps`, interleaved per rep, and gated against the f64 truth. Returns
// false when an error is above the bound.
bool board(crd::memory::IAllocator* alloc, int lg, const Arm* arms, usize arm_count, usize offset)
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

    const Data data(alloc, n);
    cont::Array<Complex<f32>> work(alloc);
    Complex<f32>* const x = place(work, n, offset);
    auto reset = [&]
    {
        std::memcpy(x, data.base.data(), n * sizeof(Complex<f32>));
    };

    DFTI_DESCRIPTOR_HANDLE h = mkl_plan(n);

    // Each side's error and each arm's digest, from one untimed transform.
    double err[kMaxArms + 1] = {};
    crd::u64 bits[kMaxArms] = {};
    for (usize a = 0; a < arm_count; ++a)
    {
        reset();
        arms[a].plan->execute(cont::Span<Complex<f32>>(x, n), fft::FftDirection::Forward);
        err[a] = max_rel(x, data.truth.data(), n);
        bits[a] = crd::fnv1a_64(x, n * sizeof(Complex<f32>));
    }
    reset();
    DftiComputeForward(h, x);
    err[arm_count] = max_rel(x, data.truth.data(), n);

    double best[kMaxArms + 1] = {};
    std::fill(best, best + arm_count + 1, 1e300);
    // Each timed transform runs warm: an untimed transform of the same side precedes it, so its tables and code are
    // where a stream of its transforms keeps them (the --loop profile's state), not where the other sides left them.
    const auto forward = [&](usize a)
    {
        arms[a].plan->execute(cont::Span<Complex<f32>>(x, n), fft::FftDirection::Forward);
    };
    for (int r = 0; r < reps; ++r)
    {
        for (usize a = 0; a < arm_count; ++a)
        {
            reset();
            forward(a);
            reset();
            best[a] = std::min(best[a], time_ms([&] { forward(a); }));
        }
        reset();
        DftiComputeForward(h, x);
        reset();
        best[arm_count] = std::min(best[arm_count], time_ms([&] { DftiComputeForward(h, x); }));
    }
    DftiFreeDescriptor(&h);

    const double mkl = best[arm_count];
    const double limit = bound(n);
    bool ok = err[arm_count] <= limit;
    usize winner = 0;
    for (usize a = 0; a < arm_count; ++a)
    {
        std::printf("%-9zu | %-15s | %10.5f | %10.5f | %6.2fx | %.1e | %.1e | %016llx\n", n, arms[a].name, best[a],
                    mkl, mkl / best[a], err[a], err[arm_count], static_cast<unsigned long long>(bits[a]));
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

fft::FftAlgorithm planned_at(usize n)
{
    return fft::detail::level_forward<f32>(n, fft::fft_kernels().level, crd::math::kernel_determinism());
}

// The forward named `side` at n: planned, or an algorithm name forward_builds offers there. False for any other name.
bool find_algorithm(const char* side, usize n, fft::FftAlgorithm* out)
{
    if (std::strcmp(side, "planned") == 0)
    {
        *out = planned_at(n);
        return true;
    }
    const auto named = [&](fft::FftAlgorithm forward)
    {
        return std::strcmp(side, fft::fft_algorithm_name(forward)) == 0 && fft::detail::forward_builds<f32>(n, forward);
    };
    const fft::FftAlgorithm* found = std::ranges::find_if(fft::detail::kFftAlgorithms, named);
    if (found == std::ranges::end(fft::detail::kFftAlgorithms))
    {
        return false;
    }
    *out = *found;
    return true;
}

// The stream's per-transform times (ms, sorted in place): its minimum, percentiles, mean and maximum in microseconds.
void print_distribution(cont::Array<double>& each)
{
    std::ranges::sort(each);
    const usize count = each.size();
    const auto at = [&](usize percent)
    {
        return each[(count - 1) * percent / 100] * 1000.0;
    };
    double sum = 0.0;
    for (const double ms : each)
    {
        sum += ms;
    }
    std::printf("each transform (without its copy, us): min %.4f p10 %.4f median %.4f p90 %.4f mean %.4f max %.4f\n",
                at(0), at(10), at(50), at(90), sum / static_cast<double>(count) * 1000.0, at(100));
}

// The profile loop (--loop): `iterations` forwards of one side at 2^lg, each from a fresh copy of the input in a
// buffer `offset` bytes past a cache line; with `distribution`, each forward is also timed alone. Returns the exit
// status.
int loop(crd::memory::TlsfAllocator& alloc, const Options& options)
{
    const char* const side = options.loop_side;
    const long iterations = options.iterations;
    const bool distribution = options.distribution;
    const usize offset = options.offset;
    const usize n = usize{1} << options.lg_first;
    const bool mkl = std::strcmp(side, "mkl") == 0;
    const bool copy = std::strcmp(side, "copy") == 0;
    fft::FftAlgorithm forward = fft::FftAlgorithm{};
    if (!mkl && !copy && !find_algorithm(side, n, &forward))
    {
        std::fprintf(stderr, "bench_fft_f32: %s is not planned, mkl, copy or a forward the planner offers at n=%zu\n",
                     side, n);
        return 2;
    }
    const Data data(&alloc, n);
    cont::Array<Complex<f32>> work(&alloc);
    Complex<f32>* const x = place(work, n, offset);
    fft::FftPlan<f32>* plan = nullptr;
    DFTI_DESCRIPTOR_HANDLE h = nullptr;
    if (mkl)
    {
        h = mkl_plan(n);
    }
    else if (!copy)
    {
        plan = crd::memory::construct_or_fatal<fft::FftPlan<f32>>(alloc, kPlanOutOfMemory, &alloc, n, forward);
    }
    const auto execute = [&]
    {
        if (mkl)
        {
            DftiComputeForward(h, x);
        }
        else if (plan != nullptr)
        {
            plan->execute(cont::Span<Complex<f32>>(x, n), fft::FftDirection::Forward);
        }
    };
    const auto transform = [&]
    {
        std::memcpy(x, data.base.data(), n * sizeof(Complex<f32>));
        execute();
    };
    transform();
    const double err = copy ? 0.0 : max_rel(x, data.truth.data(), n);
    const crd::u64 bits = crd::fnv1a_64(x, n * sizeof(Complex<f32>));
    int status = 0;
    if (err > bound(n))
    {
        std::fprintf(stderr, "n=%zu: %s's error %.1e against the f64 truth is above the bound %.1e\n", n, side, err,
                     bound(n));
        status = 1;
    }
    cont::Array<double> each(&alloc);
    each.resize(distribution ? static_cast<usize>(iterations) : 0);
    const double ms = time_ms(
        [&]
        {
            for (long i = 0; i < iterations; ++i)
            {
                if (distribution)
                {
                    std::memcpy(x, data.base.data(), n * sizeof(Complex<f32>));
                    each[static_cast<usize>(i)] = time_ms(execute);
                }
                else
                {
                    transform();
                }
            }
        });
    std::printf("loop n=%zu side=%s forward=%s iterations=%ld: %.6f ms per transform (with its copy), err %.1e "
                "bits=%016llx\n",
                n, side, mkl || copy ? side : fft::fft_algorithm_name(forward), iterations,
                ms / static_cast<double>(iterations), err, static_cast<unsigned long long>(bits));
    if (distribution)
    {
        print_distribution(each);
    }
#ifdef CRD_FFT_PROFILE
    fft::prof::dump_four_step();
#endif
    if (h != nullptr)
    {
        DftiFreeDescriptor(&h);
    }
    if (plan != nullptr)
    {
        crd::memory::destroy(alloc, plan);
    }
    return status;
}

bool parse_options(int argc, char** argv, Options* options)
{
    for (int i = 1; i < argc; ++i)
    {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--planned") == 0)
        {
            options->planned_only = true;
        }
        else if (std::strcmp(argv[i], "--sizes") == 0 && has_value)
        {
            if (std::sscanf(argv[++i], "%d-%d", &options->lg_first, &options->lg_last) != 2)
            {
                return false;
            }
        }
        else if (std::strcmp(argv[i], "--loop") == 0 && has_value)
        {
            options->loop_side = argv[++i];
        }
        else if (std::strcmp(argv[i], "--distribution") == 0)
        {
            options->distribution = true;
        }
        else if (std::strcmp(argv[i], "--offset") == 0 && has_value)
        {
            options->offset = static_cast<usize>(std::strtoul(argv[++i], nullptr, 10));
            if (options->offset >= kMaxOffset || options->offset % sizeof(Complex<f32>) != 0)
            {
                return false;
            }
        }
        else if (std::strcmp(argv[i], "--iterations") == 0 && has_value)
        {
            options->iterations = std::strtol(argv[++i], nullptr, 10);
        }
        else
        {
            return false;
        }
    }
    const bool sizes = kFirstLg <= options->lg_first && options->lg_first <= options->lg_last &&
                       options->lg_last <= kLastLg;
    if (options->loop_side != nullptr)
    {
        return sizes && options->lg_first == options->lg_last && options->iterations > 0 && !options->planned_only;
    }
    return sizes && options->iterations == 0 && !options->distribution;
}
} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parse_options(argc, argv, &options))
    {
        std::fprintf(stderr, "usage: bench_fft_f32 [--sizes A-B] [--planned] [--offset B]\n"
                             "       bench_fft_f32 --loop <planned|algorithm|mkl|copy> --sizes A-A --iterations K "
                             "[--distribution] [--offset B]\n"
                             "       (10 <= A <= B <= 22)\n");
        return 2;
    }
    crd::memory::TlsfAllocator alloc(2ULL << 30);
    char version[256] = {};
    mkl_get_version_string(version, static_cast<int>(sizeof(version)));
    std::printf("MKL: %s\n", version);
    std::printf("fft kernel level: %s\n", crd::cpu_level_name(fft::fft_kernels().level)); // the level's own plans
    if (options.loop_side != nullptr)
    {
        return loop(alloc, options);
    }
    std::printf("=== complex-f32 forward FFT, best of reps (ms); ratio = MKL / Cerid; errors against FFTW f64 ===\n");
    std::printf("%-9s | %-15s | %10s | %10s | %7s | %-7s | %-7s | %s\n", "n", "Cerid arm", "Cerid", "MKL", "ratio",
                "Cerid err", "MKL err", "bits");

    bool ok = true;
    for (int lg = options.lg_first; lg <= options.lg_last; ++lg)
    {
        const usize n = usize{1} << lg;
        Arm arms[kMaxArms] = {};
        fft::FftPlan<f32>* plans[kMaxArms] = {};
        usize arm_count = 0;
        const fft::FftAlgorithm planned = planned_at(n);
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
            if (!options.planned_only && forward != planned && fft::detail::forward_builds<f32>(n, forward))
            {
                add(forward);
            }
        }
        ok = board(&alloc, lg, arms, arm_count, options.offset) && ok;
        for (usize a = arm_count; a-- > 0;)
        {
            crd::memory::destroy(alloc, plans[a]);
        }
    }
    return ok ? 0 : 1;
}
