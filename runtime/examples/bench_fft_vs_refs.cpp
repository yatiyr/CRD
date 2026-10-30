// bench_fft_vs_refs — v10 FFT throughput shootout vs the gold standards (scripts/run_bench_fft.sh, one thread).
// Complex f64 forward FFT, power-of-2 sizes. Contenders on IDENTICAL data, each with its plan built once:
//   Cerid crd::hesap::fft::FftPlan  vs  FFTW3 (FFTW_MEASURE)  vs  pocketfft (the scipy/numpy backend, its peer
//   library at the host's ISA: examples/peers/pocketfft_peer.hpp)  vs  Intel MKL DFTI.
// Reports best-of-reps wall (ms; the in-place contenders' input copy is untimed) + GFLOPS (5·n·log2 n / t, the
// standard FFT flop count) + max |Cerid-FFTW| and |pocketfft-FFTW| rel error, and fails above 32·eps·log2 n (a fast
// wrong answer is not a win). The beat-MKL verdict on the AVX2-level i9-14900K (no AVX-512 ⇒ MKL runs AVX2 here ⇒
// level fight). Cerid's Complex<f64> shares the {re,im} layout of fftw_complex and of the peer's re/im pairs.
// From 2^10 to 2^22 every forward the planner offers at the size (fft::detail::forward_builds: the one this level
// runs first, fft::detail::level_forward, then the others) runs in this one binary too, interleaved every rep and
// gated against FFTW the same way; a level's winners become its f64 plans (scripts/gen_fft_level_plans.py reads the
// boards' logs under the "fft kernel level" this program prints, the unit FftPlan keys on).

#include "pocketfft_peer.hpp"

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
#include <limits>
#include <mkl_dfti.h>

using crd::f64;
using crd::usize;
namespace fft = crd::hesap::fft;
namespace cont = crd::containers;
namespace pocket = crd_peer::pocketfft;
using crd::hesap::Complex;

namespace
{
// Best of `reps` timed runs of `fn`, each after an untimed `prepare` (the in-place contenders' input copy).
template <typename Prepare, typename Fn> double time_best_ms(int reps, const Prepare& prepare, const Fn& fn)
{
    double best = 1e300;
    for (int r = 0; r < reps; ++r)
    {
        prepare();
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        best = ms < best ? ms : best;
    }
    return best;
}

double gflops(usize n, double ms)
{
    const double flops = 5.0 * static_cast<double>(n) * std::log2(static_cast<double>(n));
    return flops / (ms * 1e6); // ms→s (1e-3) and flops→Gflops (1e-9) ⇒ /1e6
}

// max |x - ref| / (1 + max |ref|) over n values.
double max_rel(const Complex<f64>* x, const Complex<f64>* ref, usize n)
{
    double maxmag = 0.0;
    double maxdiff = 0.0;
    for (usize i = 0; i < n; ++i)
    {
        maxmag = std::max(maxmag, std::hypot(ref[i].re, ref[i].im));
        maxdiff = std::max(maxdiff, std::hypot(x[i].re - ref[i].re, x[i].im - ref[i].im));
    }
    return maxdiff / (1.0 + maxmag);
}

constexpr int kArmsMaxLog2 = 22;
constexpr usize kMaxArms = sizeof(fft::detail::kFftAlgorithms) / sizeof(fft::detail::kFftAlgorithms[0]);
constexpr const char* kPlanOutOfMemory = "bench_fft_vs_refs: out of memory for a plan";

// Every forward the planner offers at n, the level's own first, each timed best of `reps` with the arms interleaved
// per rep and the input copied in untimed, and gated against FFTW's output. Returns false when an error is above
// `bound`.
bool forward_arms(crd::memory::IAllocator* alloc, const Complex<f64>* base, const Complex<f64>* fftw_out, usize n,
                  int reps, double bound)
{
    fft::FftPlan<f64>* plans[kMaxArms] = {};
    usize count = 0;
    const fft::FftAlgorithm planned =
        fft::detail::level_forward<f64>(n, fft::fft_kernels().level, crd::math::kernel_determinism());
    const auto add = [&](fft::FftAlgorithm forward)
    {
        plans[count] = crd::memory::construct_or_fatal<fft::FftPlan<f64>>(*alloc, kPlanOutOfMemory, alloc, n, forward);
        ++count;
    };
    add(planned);
    for (const fft::FftAlgorithm forward : fft::detail::kFftAlgorithms)
    {
        if (forward != planned && fft::detail::forward_builds<f64>(n, forward))
        {
            add(forward);
        }
    }

    cont::Array<Complex<f64>> work(alloc);
    work.resize(n);
    const auto run = [&](usize a)
    {
        plans[a]->execute(cont::Span<Complex<f64>>(work.data(), n), fft::FftDirection::Forward);
    };
    double err[kMaxArms] = {};
    for (usize a = 0; a < count; ++a)
    {
        std::memcpy(work.data(), base, n * sizeof(Complex<f64>));
        run(a);
        err[a] = max_rel(work.data(), fftw_out, n);
    }
    double best[kMaxArms] = {};
    std::fill(best, best + count, 1e300);
    for (int r = 0; r < reps; ++r)
    {
        for (usize a = 0; a < count; ++a)
        {
            std::memcpy(work.data(), base, n * sizeof(Complex<f64>));
            const auto t0 = std::chrono::steady_clock::now();
            run(a);
            const auto t1 = std::chrono::steady_clock::now();
            best[a] = std::min(best[a], std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
    }

    bool ok = true;
    usize winner = 0;
    for (usize a = 0; a < count; ++a)
    {
        std::printf("%-9zu | %-15s | %9.3f | %.1e\n", n, fft::fft_algorithm_name(plans[a]->state().forward), best[a],
                    err[a]);
        ok = ok && err[a] <= bound;
        winner = best[a] < best[winner] ? a : winner;
    }
    std::printf("n=%zu winner: %s\n", n, fft::fft_algorithm_name(plans[winner]->state().forward));
    for (usize a = count; a-- > 0;)
    {
        crd::memory::destroy(*alloc, plans[a]);
    }
    return ok;
}
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(4ULL << 30);
    std::printf("pocketfft peer: %s\n", pocket::isa());
    std::printf("fft kernel level: %s\n", crd::cpu_level_name(fft::fft_kernels().level)); // the level's own plans
    std::printf("=== complex-f64 forward FFT, power-of-2 (best of reps). GFLOPS = 5 n log2(n)/t ===\n");
    std::printf("%-9s | %-22s | %-20s | %-20s | %-20s | Cerid err | pocket err\n", "n", "Cerid (ms / GFLOPS)",
                "FFTW (ms/GFLOPS)", "pocketfft (ms/GFLOPS)", "MKL (ms/GFLOPS)");

    bool wrong = false;
    for (int lg = 10; lg <= 23; ++lg)
    {
        const usize n = usize{1} << lg;
        int reps = 8;
        if (n <= (1U << 16))
        {
            reps = 200;
        }
        else if (n <= (1U << 20))
        {
            reps = 30;
        }

        // Reference input (deterministic).
        cont::Array<Complex<f64>> base(&alloc);
        base.resize(n);
        crd::u64 s = 0x12345 ^ n;
        for (usize i = 0; i < n; ++i)
        {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            const double a = static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53) * 2.0 - 1.0;
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            const double b = static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53) * 2.0 - 1.0;
            base[i] = Complex<f64>{a, b};
        }

        cont::Array<Complex<f64>> work(&alloc);
        work.resize(n);
        auto reset = [&]
        {
            std::memcpy(work.data(), base.data(), n * sizeof(Complex<f64>));
        };

        // ---- Cerid ----
        const fft::FftPlan<f64> plan(&alloc, n);
        const double cms = time_best_ms(
            reps, reset, [&] { plan.execute(cont::Span<Complex<f64>>(work.data(), n), fft::FftDirection::Forward); });
        cont::Array<Complex<f64>> cerid_out(&alloc);
        cerid_out.resize(n);
        std::memcpy(cerid_out.data(), work.data(), n * sizeof(Complex<f64>));

        // ---- FFTW (MEASURE) ----
        auto* fin = reinterpret_cast<fftw_complex*>(work.data());
        fftw_plan fp = fftw_plan_dft_1d(static_cast<int>(n), fin, fin, FFTW_FORWARD, FFTW_MEASURE);
        const double fms = time_best_ms(reps, reset, [&] { fftw_execute(fp); });
        cont::Array<Complex<f64>> fftw_out(&alloc);
        fftw_out.resize(n);
        std::memcpy(fftw_out.data(), work.data(), n * sizeof(Complex<f64>));
        fftw_destroy_plan(fp);

        // ---- pocketfft (out of place from the reference input, as its c2c) ----
        cont::Array<Complex<f64>> pout(&alloc);
        pout.resize(n);
        const pocket::Owned pplan{pocket::c2c_plan(static_cast<pocket::Index>(n))};
        const auto* pin = reinterpret_cast<const f64*>(base.data());
        auto* pdst = reinterpret_cast<f64*>(pout.data());
        const double pms = time_best_ms(reps, [] {}, [&] { pocket::forward(pplan.get(), pin, pdst); });

        // ---- MKL DFTI ----
        DFTI_DESCRIPTOR_HANDLE h = nullptr;
        DftiCreateDescriptor(&h, DFTI_DOUBLE, DFTI_COMPLEX, 1, static_cast<MKL_LONG>(n));
        DftiSetValue(h, DFTI_PLACEMENT, DFTI_INPLACE);
        DftiCommitDescriptor(h);
        const double mms = time_best_ms(reps, reset, [&] { DftiComputeForward(h, work.data()); });
        DftiFreeDescriptor(&h);

        // Correctness cross-check against FFTW.
        const double cerid_err = max_rel(cerid_out.data(), fftw_out.data(), n);
        const double pocket_err = max_rel(pout.data(), fftw_out.data(), n);
        const double bound = 32.0 * std::numeric_limits<double>::epsilon() * static_cast<double>(lg);

        std::printf("%-9zu | %8.3f / %7.2f      | %7.3f / %7.2f    | %7.3f / %7.2f    | %7.3f / %7.2f    | %.1e   | "
                    "%.1e\n",
                    n, cms, gflops(n, cms), fms, gflops(n, fms), pms, gflops(n, pms), mms, gflops(n, mms), cerid_err,
                    pocket_err);
        if (!(cerid_err <= bound) || !(pocket_err <= bound))
        {
            std::fprintf(stderr, "n=%zu: an error against FFTW is above the bound %.1e\n", n, bound);
            wrong = true;
        }
        if (lg <= kArmsMaxLog2 && !forward_arms(&alloc, base.data(), fftw_out.data(), n, reps, bound))
        {
            std::fprintf(stderr, "n=%zu: a forward arm's error against FFTW is above the bound %.1e\n", n, bound);
            wrong = true;
        }
    }
    return wrong ? 1 : 0;
}
