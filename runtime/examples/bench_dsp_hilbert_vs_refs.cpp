// v11-l: Hilbert / analytic-signal throughput. Cerid hilbert() (one-shot — builds an FFT plan per call, like
// scipy.signal.hilbert / MATLAB hilbert) + the plan-cached HilbertTransformer (the realistic streaming hot path).
// The pow-2 fast path uses the direct FftPlan (Bluestein would pad to 2n). Companion scipy + MATLAB scripts.
//
// MEASURED (WSL, 1 thread, AVX2, chk identical across all three ⇒ correct):
//   N=65536:   CERID cached 0.741 ms · scipy 0.967 (1.31x) · MATLAB-1T 1.000 (1.35x)
//   N=1048576: CERID cached 17.46 ms · scipy 25.34 (1.45x) · MATLAB-1T 31.83 (1.81x)
// The plan-cached transformer BEATS scipy + MATLAB at both sizes (Cerid FFT execute > PocketFFT). The one-shot
// hilbert() LOSES to both (full twiddle-table rebuild per call) — the v11-j FftConvolver lesson; the cached path
// is the fix. MATLAB forced to 1 thread (its FFT is multi-threaded MKL by default) for the fair comparison.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <crd/hesap/dsp/hilbert.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;

static void run(crd::memory::IAllocator* a, crd::usize n, int reps)
{
    cont::Array<double> x(a);
    x.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i] = std::sin(0.01 * static_cast<double>(i)) + 0.3 * std::cos(0.023 * static_cast<double>(i));
    }
    const cont::ConstSpan<double> xs(x.data(), n);
    auto warm = dsp::hilbert<double>(a, xs); // warm pages
    double chk = 0;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int r = 0; r < reps; ++r)
    {
        auto h = dsp::hilbert<double>(a, xs);
        chk += h[n / 2].im;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    std::printf("CERID hilbert        N=%-8zu  %.4f ms/call (chk=%.4f)\n", n,
                std::chrono::duration<double, std::milli>(t1 - t0).count() / reps, chk);
    (void)warm;

    // plan-cached transformer (the realistic streaming / repeated-block hot path).
    dsp::HilbertTransformer<double> tr(a, n);
    cont::Array<crd::hesap::Complex<double>> out(a);
    out.resize(n);
    const cont::Span<crd::hesap::Complex<double>> os(out.data(), n);
    tr.transform(xs, os);
    double chk2 = 0;
    auto t2 = std::chrono::high_resolution_clock::now();
    for (int r = 0; r < reps; ++r)
    {
        tr.transform(xs, os);
        chk2 += out[n / 2].im;
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    std::printf("CERID HilbertTransfmr N=%-8zu  %.4f ms/call (chk=%.4f)\n", n,
                std::chrono::duration<double, std::milli>(t3 - t2).count() / reps, chk2);
}

int main()
{
    crd::memory::TlsfAllocator a(crd::usize{1} << 31);
    run(&a, 1U << 16, 500);
    run(&a, 1U << 20, 50);
    return 0;
}
