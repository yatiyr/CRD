// v11-s: detection/measurement throughput. find_peaks + detrend(linear) on N=1M vs scipy; thd on N=1M (FFT-bound)
// vs MATLAB. Companion bench_dsp_measure_refs.py + bench_measure_matlab.m.
//
// MEASURED (WSL, 1 thread, AVX2):
//   detrend N=1M:    CERID 1.73 ms · scipy 23.0 (13.3x WIN — scipy uses linalg.lstsq; Cerid's closed-form 2x2)
//   thd N=1M:        CERID 19.8 ms · MATLAB-1T 117.1 (5.9x WIN) ; snr vs MATLAB 130.6 (~6.6x WIN)
//   find_peaks N=1M: CERID 2.74 ms · scipy 2.61 (0.95x — parity, both O(n) scans)
// detrend CRUSHES scipy 13.3x; thd/snr CRUSH MATLAB ~6x; find_peaks parity. (thd VALUES differ — MATLAB windows +
// lobe-sums vs Cerid unwindowed single-bin; correctness gated separately by the analytic -40 dB planted-harmonic.)
// ⚠ find_peaks MULTITHREADING MEASURED + REJECTED: a deterministic parallel local_maxima (local_maxima_mt) is
// CORRECT (≡ serial, the {1..16} moat) but SLOWER (6.8 ms @32 workers) — find_peaks is OUTPUT-ASSEMBLY-bound (the
// serial peak-list build), not scan-bound, so the parallel scan loses to the per-call scratch + serial gather.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <crd/hesap/dsp/measurements.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;

int main()
{
    crd::jobs::init(); // activate the multithreaded find_peaks path
    std::printf("workers=%u\n", crd::jobs::num_workers());
    crd::memory::TlsfAllocator a(crd::usize{1} << 31);
    const crd::usize n = 1000000;
    cont::Array<double> x(&a), xt(&a);
    x.resize(n);
    xt.resize(n);
    crd::u64 s = 11ULL;
    for (crd::usize i = 0; i < n; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const double e = (static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0)) * 2.0 - 1.0;
        x[i] = std::sin(0.05 * static_cast<double>(i)) + 0.3 * e;
        xt[i] = 0.5 + 1e-6 * static_cast<double>(i) + std::sin(0.013 * static_cast<double>(i));
    }
    const cont::ConstSpan<double> xs(x.data(), n), xts(xt.data(), n);
    const int reps = 20;

    auto p = dsp::find_peaks<double>(&a, xs);
    auto t0 = std::chrono::high_resolution_clock::now();
    crd::usize chk = 0;
    for (int r = 0; r < reps; ++r)
    {
        chk += dsp::find_peaks<double>(&a, xs).size();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    std::printf("CERID find_peaks N=%zu  %.4f ms/call (npeaks=%zu)\n", n,
                std::chrono::duration<double, std::milli>(t1 - t0).count() / reps, p.size());

    auto d = dsp::detrend<double>(&a, xts, true);
    auto t2 = std::chrono::high_resolution_clock::now();
    double chk2 = 0;
    for (int r = 0; r < reps; ++r)
    {
        chk2 += dsp::detrend<double>(&a, xts, true)[n / 2];
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    std::printf("CERID detrend    N=%zu  %.4f ms/call (chk=%.5f)\n", n,
                std::chrono::duration<double, std::milli>(t3 - t2).count() / reps, chk2);

    auto th = dsp::thd<double>(&a, xs);
    auto t4 = std::chrono::high_resolution_clock::now();
    double chk3 = 0;
    for (int r = 0; r < reps; ++r)
    {
        chk3 += dsp::thd<double>(&a, xs);
    }
    auto t5 = std::chrono::high_resolution_clock::now();
    std::printf("CERID thd        N=%zu  %.4f ms/call (thd=%.3f dB)\n", n,
                std::chrono::duration<double, std::milli>(t5 - t4).count() / reps, th);
    (void)chk;
    (void)d;
    crd::jobs::shutdown();
    return 0;
}
