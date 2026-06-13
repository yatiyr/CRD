// bench_dct_vs_refs — v10-f DCT-II throughput shootout vs the gold standards. Cerid DctPlan.dct2 (Makhoul over
// the v10-b FFT) vs PocketFFT (numpy/scipy.fft.dct backend) vs FFTW (REDFT10 = unnormalized DCT-II). WSL,
// g++ -O3 -mavx2 -mfma, 1 thread. f64, power-of-2 N, EXECUTE-only (plans amortized). Reports best-of-reps ms +
// speedup vs each peer (>1 ⇒ Cerid faster) + Cerid's accuracy vs its OWN direct O(N²) DCT (a fast wrong answer
// is not a win). Since Cerid's complex FFT beats PocketFFT's, the Makhoul DCT built on it should beat the
// PocketFFT-backed scipy. FFTW (its own kernels + a real-DCT codelet path) is the harder peer.

#include <crd/containers/array.hpp>
#include <crd/hesap/fft/dct.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <fftw3.h>

#include <pocketfft_hdronly.h>

#include <chrono>
#include <cmath>
#include <cstdio>

using crd::f64;
using crd::usize;
namespace fft = crd::hesap::fft;
namespace cont = crd::containers;

namespace
{
template <typename Fn> double time_best_ms(int reps, Fn&& fn)
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
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(2ULL << 30);
    std::printf("=== DCT-II, f64, power-of-2, 1 thread, EXECUTE-only. speedup = peer_ms / Cerid_ms (>1 Cerid wins) ===\n");
    std::printf("%-9s | %-14s | %-14s | %-14s | %-8s | %-8s | %-8s\n", "N", "Cerid (ms)", "PocketFFT (ms)",
                "FFTW (ms)", "vsPocket", "vsFFTW", "Cerid err");

    for (int lg = 8; lg <= 20; ++lg)
    {
        const usize n = usize{1} << lg;
        const int reps = n <= (1U << 14) ? 200 : (n <= (1U << 18) ? 40 : 10);

        cont::Array<f64> x(&alloc);
        x.resize(n);
        crd::u64 s = 0x515 ^ n;
        for (usize i = 0; i < n; ++i)
        {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            x[i] = static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53) * 2.0 - 1.0;
        }

        cont::Array<f64> yc(&alloc);
        cont::Array<f64> yref(&alloc);
        cont::Array<f64> yp(&alloc);
        cont::Array<f64> yf(&alloc);
        yc.resize(n);
        yref.resize(n);
        yp.resize(n);
        yf.resize(n);

        // ---- Cerid ----
        const fft::DctPlan<f64> plan(&alloc, n);
        const cont::ConstSpan<f64> xs(x.data(), n);
        const double cms = time_best_ms(reps, [&] { plan.dct2(xs, cont::Span<f64>(yc.data(), n)); });
        double cerr = -1.0; // O(N²) direct check only at small N (it dominates the run otherwise)
        if (n <= (1U << 12))
        {
            plan.direct_dct2(xs, cont::Span<f64>(yref.data(), n));
            double maxr = 0.0;
            double maxd = 0.0;
            for (usize i = 0; i < n; ++i)
            {
                maxr = std::max(maxr, std::abs(yref[i]));
                maxd = std::max(maxd, std::abs(yc[i] - yref[i]));
            }
            cerr = maxd / (1.0 + maxr);
        }

        // ---- PocketFFT (scipy backend) DCT-II ----
        const pocketfft::shape_t shape{n};
        const pocketfft::stride_t stride{static_cast<std::ptrdiff_t>(sizeof(double))};
        const pocketfft::shape_t axes{0};
        const double pms = time_best_ms(reps, [&] {
            pocketfft::dct(shape, stride, stride, axes, 2, x.data(), yp.data(), 1.0, false, 1);
        });

        // ---- FFTW REDFT10 (= DCT-II) ----
        fftw_plan fp = fftw_plan_r2r_1d(static_cast<int>(n), x.data(), yf.data(), FFTW_REDFT10, FFTW_MEASURE);
        const double fms = time_best_ms(reps, [&] { fftw_execute(fp); });
        fftw_destroy_plan(fp);

        std::printf("%-9zu | %12.4f   | %12.4f   | %12.4f   | %6.2fx  | %6.2fx  | %.1e\n", n, cms, pms, fms,
                    pms / cms, fms / cms, cerr);
    }
    return 0;
}
