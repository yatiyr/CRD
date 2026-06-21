// v11-n/o/p/q: throughput for the compute-bound members of the batch. CZT (chirp-z, M=N, over the v10 FFT engine)
// vs scipy.signal.czt; arburg (Burg AR, O(pN)) vs MATLAB arburg. (Multitaper + root-MUSIC are accuracy-gated
// estimation methods — their crush is super-resolution / exact recovery, not throughput; see their tests.)
//
// MEASURED (WSL, 1 thread, AVX2, chk identical ⇒ correct):
//   czt cached N=4096 M=N: CERID 0.0859 ms · scipy-cached 0.0968 (1.13x WIN) · MATLAB 2.14 (~25x WIN)
//   czt one-shot:          CERID 0.393 ms · scipy one-shot 0.329 (0.84x — FftPlan twiddle rebuild, the realistic
//                          path is cached anyway)
//   arburg N=100000 p=20:  CERID 2.35 ms · MATLAB-1T 10.83 (4.6x WIN) · (scipy has no arburg)
// arburg CRUSHES MATLAB 4.6x. The plan-cached CztPlan BEATS scipy's cached CZT 1.13x (Cerid FFT execute > PocketFFT)
// + MATLAB ~25x — the FftConvolver/HilbertTransformer pattern applied (chirp-FFT + plan cached once).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <crd/hesap/dsp/ar.hpp>
#include <crd/hesap/dsp/transforms.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;

int main()
{
    crd::memory::TlsfAllocator a(crd::usize{1} << 31);
    const double two_pi = 6.283185307179586;

    // ---- CZT: full M=N chirp-z (DFT-equivalent contour), N=4096 ----
    const crd::usize nczt = 4096;
    cont::Array<double> xc(&a);
    xc.resize(nczt);
    for (crd::usize i = 0; i < nczt; ++i)
    {
        xc[i] = std::sin(0.013 * static_cast<double>(i)) + 0.4 * std::cos(0.071 * static_cast<double>(i));
    }
    const cont::ConstSpan<double> xcs(xc.data(), nczt);
    // one-shot (transient plan).
    auto warm = dsp::czt<double>(&a, xcs, nczt, -two_pi / static_cast<double>(nczt), 0.0);
    double chk = warm[1].re;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int r = 0; r < 100; ++r)
    {
        auto X = dsp::czt<double>(&a, xcs, nczt, -two_pi / static_cast<double>(nczt), 0.0);
        chk += X[2].im;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    std::printf("CERID czt one-shot  N=%zu M=%zu  %.4f ms/call (chk=%.4f)\n", nczt, nczt,
                std::chrono::duration<double, std::milli>(t1 - t0).count() / 100.0, chk);
    // plan-cached (the realistic repeated-transform hot path — apples-to-apples with scipy's CZT object).
    dsp::CztPlan<double> czt_plan(&a, nczt, nczt, -two_pi / static_cast<double>(nczt), 0.0);
    cont::Array<crd::hesap::Complex<double>> Xc(&a);
    Xc.resize(nczt);
    const cont::Span<crd::hesap::Complex<double>> Xs(Xc.data(), nczt);
    czt_plan.execute(xcs, Xs);
    double chk1 = 0;
    auto t0b = std::chrono::high_resolution_clock::now();
    for (int r = 0; r < 100; ++r)
    {
        czt_plan.execute(xcs, Xs);
        chk1 += Xc[2].im;
    }
    auto t1b = std::chrono::high_resolution_clock::now();
    std::printf("CERID czt cached    N=%zu M=%zu  %.4f ms/call (chk=%.4f)\n", nczt, nczt,
                std::chrono::duration<double, std::milli>(t1b - t0b).count() / 100.0, chk1);

    // ---- arburg: AR order p=20 on N=100000 ----
    const crd::usize nar = 100000, p = 20;
    cont::Array<double> xa(&a);
    xa.resize(nar);
    crd::u64 s = 7ULL;
    double xm1 = 0, xm2 = 0;
    for (crd::usize i = 0; i < nar; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const double e = (static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0)) * 2.0 - 1.0;
        const double xn = 1.27 * xm1 - 0.81 * xm2 + e;
        xa[i] = xn;
        xm2 = xm1;
        xm1 = xn;
    }
    const cont::ConstSpan<double> xas(xa.data(), nar);
    auto wm = dsp::arburg<double>(&a, xas, p);
    double chk2 = wm.a[1];
    auto t2 = std::chrono::high_resolution_clock::now();
    for (int r = 0; r < 50; ++r)
    {
        auto m = dsp::arburg<double>(&a, xas, p);
        chk2 += m.variance;
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    std::printf("CERID arburg N=%zu p=%zu  %.4f ms/call (chk=%.4f)\n", nar, p,
                std::chrono::duration<double, std::milli>(t3 - t2).count() / 50.0, chk2);
    return 0;
}
