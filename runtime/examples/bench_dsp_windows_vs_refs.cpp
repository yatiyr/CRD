// v11-b: window-generation throughput shootout — Cerid vs scipy (numpy-C) vs MATLAB.
//
// HONEST FRAMING: window generation is one-time setup, NOT a streaming hot path. At REALISTIC sizes (256-8192)
// scipy is Python-call-overhead-bound, so beating it there is meaningless. This benchmark runs at N=2^20 where
// BOTH scipy (numpy-vectorized C) and Cerid are COMPUTE-bound — a fair AVX2-compiled-vs-AVX2-compiled fight.
// The scipy + MATLAB numbers are produced by the companion bench_dsp_windows_refs.py / .m at the same N.
//
// Cerid's edge is algorithmic, not micro-optimization: the cosine-sum windows use ONE std::cos per sample via the
// Chebyshev recurrence cos(k x) = 2 cos(x) cos((k-1)x) - cos((k-2)x) (vs scipy's K full-array numpy passes), and
// every symmetric window exploits w[i] = w[N-1-i] to compute only half. Correctness is bit-gated to scipy (1e-12)
// by the test suite — speed never trades accuracy.

#include <chrono>
#include <cstdio>

#include <crd/hesap/dsp/windows.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

namespace dsp = crd::hesap::dsp;

int main()
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 31);
    const crd::usize N = crd::usize{1} << 20; // compute-bound regime (fair vs numpy-C / MATLAB)
    const int reps = 50;

    auto bench = [&](const char* name, auto fn)
    {
        {
            auto w = fn();
            (void)w;
        }
        const auto t0 = std::chrono::high_resolution_clock::now();
        double checksum = 0.0;
        for (int r = 0; r < reps; ++r)
        {
            auto w = fn();
            checksum += static_cast<double>(w[N / 2]);
        }
        const auto t1 = std::chrono::high_resolution_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / reps;
        std::printf("%-16s %7.3f ms/call   (N=%zu, chk=%.1f)\n", name, ms, static_cast<size_t>(N), checksum);
    };

    std::printf("=== CERID window generation (N=2^20, AVX2, f64) ===\n");
    bench("hann", [&] { return dsp::hann<double>(&alloc, N); });
    bench("hamming", [&] { return dsp::hamming<double>(&alloc, N); });
    bench("blackmanharris", [&] { return dsp::blackmanharris<double>(&alloc, N); });
    bench("kaiser_b14", [&] { return dsp::kaiser<double>(&alloc, N, 14.0); });
    bench("gaussian", [&] { return dsp::gaussian<double>(&alloc, N, static_cast<double>(N) / 6.0); });
    return 0;
}
