// bench_dct_vs_refs — v10-f DCT-II throughput shootout vs the gold standards (scripts/run_bench_dct.sh). Cerid
// DctPlan.dct2 (Makhoul over the v10-b FFT) vs pocketfft (the numpy/scipy.fft.dct backend, its peer library at the
// host's ISA: examples/peers/pocketfft_peer.hpp) vs FFTW (REDFT10 = unnormalized DCT-II). 1 thread, f64, power-of-2
// N, EXECUTE-only (every side's plan built once). Reports best-of-reps ms + speedup vs each peer (>1 ⇒ Cerid
// faster) + Cerid's accuracy vs its OWN direct O(N²) DCT and pocketfft's vs FFTW, and fails above 32·eps·log2 N (a
// fast wrong answer is not a win). Since Cerid's complex FFT beats pocketfft's, the Makhoul DCT built on it should
// beat the pocketfft-backed scipy. FFTW (its own kernels + a real-DCT codelet path) is the harder peer.

#include "pocketfft_peer.hpp"

#include <crd/containers/array.hpp>
#include <crd/hesap/fft/dct.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fftw3.h>
#include <limits>

using crd::f64;
using crd::usize;
namespace fft = crd::hesap::fft;
namespace cont = crd::containers;
namespace pocket = crd_peer::pocketfft;

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

// max |x - ref| / (1 + max |ref|) over n values.
double max_rel(const f64* x, const f64* ref, usize n)
{
    double maxr = 0.0;
    double maxd = 0.0;
    for (usize i = 0; i < n; ++i)
    {
        maxr = std::max(maxr, std::abs(ref[i]));
        maxd = std::max(maxd, std::abs(x[i] - ref[i]));
    }
    return maxd / (1.0 + maxr);
}
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(2ULL << 30);
    std::printf("pocketfft peer: %s\n", pocket::isa());
    std::printf("=== DCT-II, f64, power-of-2, 1 thread, EXECUTE-only. speedup = peer_ms / Cerid_ms (>1 Cerid wins) "
                "===\n");
    std::printf("%-9s | %-14s | %-14s | %-14s | %-8s | %-8s | %-9s | %-9s\n", "N", "Cerid (ms)", "pocketfft (ms)",
                "FFTW (ms)", "vsPocket", "vsFFTW", "Cerid err", "pocket err");

    bool wrong = false;
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
        const bool direct = n <= (1U << 12); // the O(N²) direct check only at small N (it dominates the run otherwise)
        double cerr = -1.0;
        if (direct)
        {
            plan.direct_dct2(xs, cont::Span<f64>(yref.data(), n));
            cerr = max_rel(yc.data(), yref.data(), n);
        }

        // ---- pocketfft (scipy backend) DCT-II ----
        const pocket::Owned pplan{pocket::dct2_plan(static_cast<pocket::Index>(n))};
        const double pms = time_best_ms(reps, [&] { pocket::dct2(pplan.get(), x.data(), yp.data()); });

        // ---- FFTW REDFT10 (= DCT-II) ----
        fftw_plan fp = fftw_plan_r2r_1d(static_cast<int>(n), x.data(), yf.data(), FFTW_REDFT10, FFTW_MEASURE);
        const double fms = time_best_ms(reps, [&] { fftw_execute(fp); });
        fftw_destroy_plan(fp);
        const double perr = max_rel(yp.data(), yf.data(), n);
        const double bound = 32.0 * std::numeric_limits<double>::epsilon() * static_cast<double>(lg);

        std::printf("%-9zu | %12.4f   | %12.4f   | %12.4f   | %6.2fx  | %6.2fx  | %.1e   | %.1e\n", n, cms, pms, fms,
                    pms / cms, fms / cms, cerr, perr);
        if ((direct && !(cerr <= bound)) || !(perr <= bound))
        {
            std::fprintf(stderr, "N=%zu: an error is above the bound %.1e\n", n, bound);
            wrong = true;
        }
    }
    return wrong ? 1 : 0;
}
