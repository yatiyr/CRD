// v11-j: FFT convolution throughput. ⭐ KEY FINDING (profiled): the one-shot fftconvolve rebuilds the FFT plan
// (~28ms twiddle precompute @2^21) EVERY call — scipy/MATLAB amortize plans via caches. FftConvolver builds the
// plan ONCE (the realistic repeated-convolution hot path) ⇒ Cerid 41.7ms vs scipy 87.6ms = 2.1x. Cerid's RAW FFT
// is fast (12.7ms complex 2^21 1-thread) — NOT an MKL gap. The multi-core crush vs multi-threaded MATLAB needs a
// multi-threaded FFT (lands on many-independent-FFT cases: Welch/STFT/N-D, v11-m/n). Companion scipy/MATLAB scripts.
#include <chrono>
#include <cstdio>
#include <cmath>
#include <crd/hesap/dsp/convolution.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
int main()
{
    crd::memory::TlsfAllocator a(crd::usize{1} << 31);
    const crd::usize N = 1000000;
    const int reps = 10;
    cont::Array<double> x(&a), h(&a);
    x.resize(N); h.resize(N);
    for (crd::usize i = 0; i < N; ++i) { x[i] = std::sin(0.01 * i); h[i] = std::cos(0.02 * i) * std::exp(-1e-6 * i); }
    dsp::FftConvolver<double> conv(&a, N, N); // FFT plan built ONCE (amortized like scipy/MATLAB caches)
    auto w = conv.convolve(cont::ConstSpan<double>(x.data(), N), cont::ConstSpan<double>(h.data(), N));
    auto t0 = std::chrono::high_resolution_clock::now();
    double chk = 0;
    for (int r = 0; r < reps; ++r) {
        auto c = conv.convolve(cont::ConstSpan<double>(x.data(), N), cont::ConstSpan<double>(h.data(), N));
        chk += c[N];
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    std::printf("CERID FftConvolver(1M,1M) plan-cached  %.3f ms/call (chk=%.4f)\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count() / reps, chk);
    return 0;
}
