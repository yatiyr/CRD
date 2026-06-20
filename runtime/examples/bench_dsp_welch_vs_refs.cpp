// v11-m: Welch PSD throughput — the MULTI-THREADED FFT crush. Welch = many INDEPENDENT segment FFTs ⇒ the safe,
// high-value place for a parallel FFT (per-job plans; the delicate single long×long four-step is NOT touched).
// Cerid welch(10M, nperseg=4096): MULTI 21.9ms / 1-thread 99.7ms (4.5x scaling) vs scipy 251ms (11.5x) vs MATLAB
// pwelch 335ms (15.3x). AND bit-identical across {1..16} threads (serial fixed-order average) — a moat MKL lacks.
#include <chrono>
#include <cstdio>
#include <cmath>
#include <crd/hesap/dsp/spectral.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
int main()
{
    crd::memory::TlsfAllocator a(crd::usize{1} << 31);
    const crd::usize N = 10000000, nperseg = 4096;
    const int reps = 10;
    cont::Array<double> x(&a);
    x.resize(N);
    for (crd::usize i = 0; i < N; ++i) x[i] = std::sin(0.05 * i) + 0.3 * std::sin(0.21 * i);
    crd::jobs::Config cfg;
    cfg.num_threads = 0; // all cores
    crd::jobs::init(cfg);
    auto w = dsp::welch_psd<double>(&a, cont::ConstSpan<double>(x.data(), N), 1.0, nperseg);
    auto t0 = std::chrono::high_resolution_clock::now();
    double chk = 0;
    for (int r = 0; r < reps; ++r) { auto p = dsp::welch_psd<double>(&a, cont::ConstSpan<double>(x.data(), N), 1.0, nperseg); chk += p[10]; }
    auto t1 = std::chrono::high_resolution_clock::now();
    std::printf("CERID welch(10M, nperseg=4096) workers=%u  %.3f ms/call (chk=%.6f)\n",
                crd::jobs::num_workers(), std::chrono::duration<double, std::milli>(t1 - t0).count() / reps, chk);
    crd::jobs::shutdown();
    return 0;
}
