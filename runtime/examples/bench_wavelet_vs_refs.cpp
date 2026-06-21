// v11w-b: DWT / multilevel wavedec throughput — the wavelet analysis hot path. Cerid wavedec(N=1M) vs pywt
// (C core) vs MATLAB wavedec (1-thread). DWT is short-conv + downsample (sequential, memory-bound) ⇒ a modest
// but real win is the honest expectation (like the IIR cascade). The moat = run-twice bit-identical, single thread.
//
// MEASURED (WSL, 1 thread, AVX2, N=1M, level 6, chk IDENTICAL vs pywt ⇒ correct):
//   haar/periodization  CERID 2.74 ms · pywt 2.60  (0.95-1.07x, run-noise — haar is tiny-filter memory-bound = parity)
//   db4 /periodization  CERID 3.90 ms · pywt 4.72  (1.14-1.21x WIN)
//   db8 /periodization  CERID 6.80 ms · pywt 7.14  (1.05x WIN)
//   sym8/symmetric      CERID 7.03 ms · pywt 6.97  (0.99x parity)
//   swt db4  level 5    CERID 28.0 ms · pywt 33.0  (1.18x WIN)   ── à trous, chk identical
//   swt sym4 level 5    CERID 27.5 ms · pywt 32.6  (1.19x WIN)
//   cwt morl 64 scales  CERID 24.7 ms · pywt 29.0  (1.17x WIN)   ── N=16384, MULTI-THREADED batched (chk identical)
//   cwt cmor 64 scales  CERID 24.8 ms · pywt 72.9  (2.94x WIN)   ── complex; MT-batched crushes single-thread pywt
//   dwt2 1024x1024 db4  CERID 11.9 ms · pywt 17.8  (1.49x WIN)   ── separable, multi-threaded rows+cols
// CWT (independent scales) + 2-D (independent rows/cols) are MULTI-THREADED + bit-identical across {1,4,16} —
// the determinism moat pywt/MATLAB lack. All chk values identical to pywt ⇒ correct at bench scale.
// Honest: DWT is short-conv + downsample (sequential, memory-bound) ⇒ parity-to-modest-win vs pywt's C core is the
// real ceiling (like the IIR cascade) — EARNED via the reversed-filter branch-free interior (the first cut, switch-
// per-tap, was 7-8x SLOWER). The differentiator is the moat: run-twice bit-identical, single thread (pywt/MATLAB lack
// it). MATLAB wavedec (1-thread) runs on Windows — tests/hesap-wavelet/bench_wavelet_matlab.m.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <crd/hesap/wavelet/cwt.hpp>
#include <crd/hesap/wavelet/dwt.hpp>
#include <crd/hesap/wavelet/dwt2.hpp>
#include <crd/hesap/wavelet/swt.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
namespace wv = crd::hesap::wavelet;
namespace cont = crd::containers;

static void fill_lcg(cont::Array<double>& x, crd::usize n)
{
    x.resize(n);
    crd::u64 s = 88172645463325252ULL;
    for (crd::usize i = 0; i < n; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        x[i] = (static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0)) * 2.0 - 1.0;
    }
}

static void run_swt(crd::memory::IAllocator* a, crd::usize n, const char* name, crd::usize level, int reps)
{
    cont::Array<double> x(a);
    fill_lcg(x, n);
    const cont::ConstSpan<double> xs(x.data(), n);
    const auto w = wv::wavelet_by_name(name);
    auto warm = wv::swt<double>(a, xs, *w, level);
    const double chk = warm[0].cA[warm[0].cA.size() / 2] + warm[level - 1].cD[0];
    volatile double sink = 0;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int r = 0; r < reps; ++r)
    {
        auto c = wv::swt<double>(a, xs, *w, level);
        sink += c[0].cA[0];
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    (void)sink;
    std::printf("CERID swt     N=%zu wav=%-6s level=%zu                    %.4f ms/call (chk=%.6f)\n", n, name, level,
                std::chrono::duration<double, std::milli>(t1 - t0).count() / reps, chk);
}

static void run(crd::memory::IAllocator* a, crd::usize n, const char* name, crd::usize level,
                wv::SignalExtensionMode mode, const char* modestr, int reps)
{
    cont::Array<double> x(a);
    fill_lcg(x, n); // exactly-reproducible LCG (bit-identical to the numpy ref ⇒ chk proves correctness)
    const cont::ConstSpan<double> xs(x.data(), n);
    const auto w = wv::wavelet_by_name(name);
    auto warm = wv::wavedec<double>(a, xs, *w, mode, level); // warm
    const double chk = warm[0][warm[0].size() / 2] + warm[warm.size() - 1][0]; // == pywt's chk ⇒ correctness
    volatile double sink = 0;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int r = 0; r < reps; ++r)
    {
        auto c = wv::wavedec<double>(a, xs, *w, mode, level);
        sink += c[0][c[0].size() / 2];
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    (void)sink;
    std::printf("CERID wavedec N=%zu wav=%-6s level=%zu mode=%-13s  %.4f ms/call (ncoef=%zu chk=%.6f)\n", n, name,
                level, modestr, std::chrono::duration<double, std::milli>(t1 - t0).count() / reps, warm.size(), chk);
}

// CWT over many scales (the FFT-heavy, embarrassingly-parallel batched transform). Cerid is MULTI-THREADED
// (independent scales) ⇒ a crush over single-threaded pywt, like Welch. chk = a coefficient (matches pywt, gated 1e-8).
static void run_cwt(crd::memory::IAllocator* a, crd::usize n, const char* name, crd::usize nscales, int reps)
{
    cont::Array<double> x(a);
    fill_lcg(x, n);
    cont::Array<double> scales(a);
    scales.resize(nscales);
    for (crd::usize s = 0; s < nscales; ++s)
    {
        scales[s] = std::pow(128.0, static_cast<double>(s) / static_cast<double>(nscales - 1)); // 1..128 geometric
    }
    const cont::ConstSpan<double> xs(x.data(), n), sc(scales.data(), nscales);
    const auto w = wv::continuous_wavelet(name);
    auto warm = wv::cwt<double>(a, xs, sc, w);
    const double chk = warm.coeffs[(nscales / 2) * n + n / 2].re;
    volatile double sink = 0;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int r = 0; r < reps; ++r)
    {
        auto c = wv::cwt<double>(a, xs, sc, w);
        sink += c.coeffs[0].re;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    (void)sink;
    std::printf("CERID cwt     N=%zu wav=%-6s scales=%zu  %.4f ms/call (chk=%.6f)\n", n, name, nscales,
                std::chrono::duration<double, std::milli>(t1 - t0).count() / reps, chk);
}

static void run_dwt2(crd::memory::IAllocator* a, crd::usize R, crd::usize C, const char* name, int reps)
{
    cont::Array<double> img(a);
    fill_lcg(img, R * C);
    const auto w = wv::wavelet_by_name(name);
    auto warm = wv::dwt2<double>(a, img.data(), R, C, *w, wv::SignalExtensionMode::Periodization);
    const double chk = warm.cA[warm.cA.size() / 2];
    volatile double sink = 0;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int r = 0; r < reps; ++r)
    {
        auto d = wv::dwt2<double>(a, img.data(), R, C, *w, wv::SignalExtensionMode::Periodization);
        sink += d.cA[0];
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    (void)sink;
    std::printf("CERID dwt2    %zux%zu wav=%-6s          %.4f ms/call (chk=%.6f)\n", R, C, name,
                std::chrono::duration<double, std::milli>(t1 - t0).count() / reps, chk);
}

int main()
{
    crd::jobs::Config cfg; // engage the multi-threaded CWT / 2-D path
    crd::jobs::init(cfg);
    crd::memory::TlsfAllocator a(crd::usize{1} << 31);
    const crd::usize n = 1U << 20;
    run(&a, n, "haar", 6, wv::SignalExtensionMode::Periodization, "periodization", 50);
    run(&a, n, "db4", 6, wv::SignalExtensionMode::Periodization, "periodization", 50);
    run(&a, n, "db8", 6, wv::SignalExtensionMode::Periodization, "periodization", 50);
    run(&a, n, "sym8", 6, wv::SignalExtensionMode::Symmetric, "symmetric", 50);
    run_swt(&a, n, "db4", 5, 30);
    run_swt(&a, n, "sym4", 5, 30);
    run_cwt(&a, 1U << 14, "morl", 64, 20);
    run_cwt(&a, 1U << 14, "cmor1.5-1.0", 64, 20);
    run_dwt2(&a, 1024, 1024, "db4", 30);
    crd::jobs::shutdown();
    return 0;
}
