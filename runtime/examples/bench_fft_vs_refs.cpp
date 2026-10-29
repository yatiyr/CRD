// bench_fft_vs_refs — v10 FFT throughput shootout vs the gold standards (scripts/run_bench_fft.sh, one thread).
// Complex f64 forward FFT, power-of-2 sizes. Contenders on IDENTICAL data, each with its plan built once:
//   Cerid crd::hesap::fft::FftPlan  vs  FFTW3 (FFTW_MEASURE)  vs  pocketfft (the scipy/numpy backend, its peer
//   library at the host's ISA: examples/peers/pocketfft_peer.hpp)  vs  Intel MKL DFTI.
// Reports best-of-reps wall (ms) + GFLOPS (5·n·log2 n / t, the standard FFT flop count) + max |Cerid-FFTW| and
// |pocketfft-FFTW| rel error, and fails above 32·eps·log2 n (a fast wrong answer is not a win). The beat-MKL verdict
// on the AVX2-level i9-14900K (no AVX-512 ⇒ MKL runs AVX2 here ⇒ level fight). Cerid's Complex<f64> shares the
// {re,im} layout of fftw_complex and of the peer's re/im pairs.

#include "pocketfft_peer.hpp"

#include <crd/containers/array.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

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
template <typename Fn> double time_best_ms(int reps, const Fn& fn)
{
    double best = 1e300;
    for (int r = 0; r < reps; ++r)
    {
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
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(2ULL << 30);
    std::printf("pocketfft peer: %s\n", pocket::isa());
    std::printf("=== complex-f64 forward FFT, power-of-2 (best of reps). GFLOPS = 5 n log2(n)/t ===\n");
    std::printf("%-9s | %-22s | %-20s | %-20s | %-20s | Cerid err | pocket err\n", "n", "Cerid (ms / GFLOPS)",
                "FFTW (ms/GFLOPS)", "pocketfft (ms/GFLOPS)", "MKL (ms/GFLOPS)");

    bool wrong = false;
    for (int lg = 10; lg <= 23; ++lg)
    {
        const usize n = usize{1} << lg;
        const int reps = n <= (1U << 16) ? 200 : (n <= (1U << 20) ? 30 : 8);

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
        const double cms =
            time_best_ms(reps,
                         [&]
                         {
                             reset();
                             plan.execute(cont::Span<Complex<f64>>(work.data(), n), fft::FftDirection::Forward);
                         });
        cont::Array<Complex<f64>> cerid_out(&alloc);
        cerid_out.resize(n);
        std::memcpy(cerid_out.data(), work.data(), n * sizeof(Complex<f64>));

        // ---- FFTW (MEASURE) ----
        auto* fin = reinterpret_cast<fftw_complex*>(work.data());
        fftw_plan fp = fftw_plan_dft_1d(static_cast<int>(n), fin, fin, FFTW_FORWARD, FFTW_MEASURE);
        const double fms = time_best_ms(reps,
                                        [&]
                                        {
                                            reset();
                                            fftw_execute(fp);
                                        });
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
        const double pms = time_best_ms(reps, [&] { pocket::forward(pplan.get(), pin, pdst); });

        // ---- MKL DFTI ----
        DFTI_DESCRIPTOR_HANDLE h = nullptr;
        DftiCreateDescriptor(&h, DFTI_DOUBLE, DFTI_COMPLEX, 1, static_cast<MKL_LONG>(n));
        DftiSetValue(h, DFTI_PLACEMENT, DFTI_INPLACE);
        DftiCommitDescriptor(h);
        const double mms = time_best_ms(reps,
                                        [&]
                                        {
                                            reset();
                                            DftiComputeForward(h, work.data());
                                        });
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
    }
    return wrong ? 1 : 0;
}
