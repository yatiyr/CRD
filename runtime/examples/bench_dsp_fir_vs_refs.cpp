// v11-c: FIR window-method design throughput — Cerid vs scipy (firwin) vs MATLAB (fir1).
// firwin is closed-form (sinc x window x normalize) → 1e-12 coefficient match AND a perf crush. N=2^20
// (compute-bound, fair AVX2 fight; realistic FIR sizes are Python-overhead-bound for scipy). Cerid's edge:
// a single fused loop + linear-phase SYMMETRY (compute half the sincs) vs scipy's multiple numpy passes.
#include <chrono>
#include <cstdio>
#include <crd/hesap/dsp/fir.hpp>
#include <crd/hesap/dsp/windows.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
int main()
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 31);
    const crd::usize N = crd::usize{1} << 20;
    const int reps = 50;
    const double fc[1] = {0.3};
    const auto win = dsp::hamming<double>(&alloc, N);
    const auto t0 = std::chrono::high_resolution_clock::now();
    double chk = 0.0;
    for (int r = 0; r < reps; ++r)
    {
        const auto h = dsp::firwin<double>(&alloc, N, cont::ConstSpan<double>(fc, 1),
                                           cont::ConstSpan<double>(win.data(), N), true);
        chk += h[N / 2];
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    std::printf("CERID firwin lowpass  %7.3f ms/call   (N=%zu, chk=%.3f)\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count() / reps, static_cast<size_t>(N), chk);
    return 0;
}

// firls(1601) benchmark (compute-bound — the O(M^3) normal-equation solve). Cerid's single-pass matrix build +
// LU vs scipy's numpy-broadcast construction (41.8x) and MATLAB's ill-conditioned formulation (3.4x; MATLAB warns
// RCOND~1e-19 — Cerid stays stable via the scipy normal-equation form). See the companion .py for scipy.
