// prof_fft_kernel — isolated Cerid FFT kernel for perf profiling. Runs ONE size in a tight loop so `perf stat`
// attributes uops/IPC/port-pressure/cache-misses to the Cerid butterfly alone (no ref libs in the binary).
// argv: log2N iters [inverse] [f32] (the f64 forward by default).  Build: see scripts/prof_fft_kernel.sh.  Use at an
// L1-resident N (e.g. 10 => 1024) to measure the PURE kernel quality (no memory pressure) — that 0.44× vs MKL is the
// real wall. The line ends with the FNV-1a digest of one transform of the input (`bits`), so two builds' boards also
// show they compute the same bits (docs/bench/README.md#fast-ab-boards); the profile tree (CRD_HESAP_FFT_PROFILE) adds
// the phase counters.

#include <crd/containers/array.hpp>
#include <crd/containers/hash.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using crd::f32;
using crd::f64;
using crd::usize;
namespace fft = crd::hesap::fft;
namespace cont = crd::containers;
using crd::hesap::Complex;

namespace
{
template <typename T> void run(int lg, long iters, fft::FftDirection dir)
{
    const usize n = usize{1} << lg;
    crd::memory::TlsfAllocator alloc(1ULL << 30);
    cont::Array<Complex<T>> work(&alloc);
    work.resize(n);
    crd::u64 s = 0x1234;
    for (usize i = 0; i < n; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        work[i] = Complex<T>{static_cast<T>(static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53)), T(0.5)};
    }
    const fft::FftPlan<T> plan(&alloc, n);
    // The digest of one transform of the input, outside the timed loop (which runs on its own output).
    cont::Array<Complex<T>> once(&alloc);
    once.resize(n);
    std::memcpy(once.data(), work.data(), n * sizeof(Complex<T>));
    plan.execute(cont::Span<Complex<T>>(once.data(), n), dir);
    const crd::u64 bits = crd::containers::fnv1a_64(once.data(), n * sizeof(Complex<T>));

    const auto t0 = std::chrono::steady_clock::now();
    for (long it = 0; it < iters; ++it)
    {
        plan.execute(cont::Span<Complex<T>>(work.data(), n), dir);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double per = secs / static_cast<double>(iters);
    const double gf = 5.0 * static_cast<double>(n) * std::log2(static_cast<double>(n)) / (per * 1e9);
    std::printf("N=%zu iters=%ld  %.3f ns/fft  %.2f GFLOPS  (sink=%.3f) %s %s bits=%016llx\n", n, iters, per * 1e9,
                gf, static_cast<double>(work[0].re + work[n / 2].im),
                dir == fft::FftDirection::Inverse ? "inverse" : "forward", sizeof(T) == 4 ? "f32" : "f64",
                static_cast<unsigned long long>(bits));
#ifdef CRD_FFT_PROFILE
    fft::prof::dump();
    fft::prof::dump_four_step();
#endif
}
} // namespace

int main(int argc, char** argv)
{
    const int lg = argc > 1 ? std::atoi(argv[1]) : 10;
    const long iters = argc > 2 ? std::atol(argv[2]) : 2000000;
    fft::FftDirection dir = fft::FftDirection::Forward;
    bool single = false;
    for (int i = 3; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "inverse") == 0)
        {
            dir = fft::FftDirection::Inverse;
        }
        else if (std::strcmp(argv[i], "f32") == 0)
        {
            single = true;
        }
        else
        {
            std::fprintf(stderr, "usage: prof_fft_kernel [log2N] [iters] [inverse] [f32]\n");
            return 2;
        }
    }
    if (single)
    {
        run<f32>(lg, iters, dir);
    }
    else
    {
        run<f64>(lg, iters, dir);
    }
    return 0;
}
