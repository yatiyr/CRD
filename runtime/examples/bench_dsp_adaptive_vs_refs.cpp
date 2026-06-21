// v11-t: adaptive-filter (NLMS / LMS) per-sample throughput, m=32 taps, N=1M samples. The streaming hot loop
// (system-ID / echo-cancel / equalization). Companion liquid (eqlms, f32) + MATLAB (dsp.LMSFilter) scripts.
//
// MEASURED (WSL, 1 thread, AVX2):
//   CERID LMS 22.63 ms · liquid eqlms-f32 42.34 (1.87x) · MATLAB dsp.LMSFilter 26.80 (1.18x) · CERID NLMS 26.79 ms
// Cerid LMS BEATS both liquid (1.87x, while f64 vs liquid's f32) AND MATLAB (1.18x), plus the run-twice determinism
// moat neither has. (The end-state chk differs across the three — each library's LMS uses a different step-size
// convention; correctness is gated separately by known-plant recovery in test_adaptive.cpp, not by chk-match.)
#include <chrono>
#include <cmath>
#include <cstdio>
#include <crd/hesap/dsp/adaptive.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;

template <class F> static double timed(F&& f, int reps)
{
    f(); // warm
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int r = 0; r < reps; ++r)
    {
        f();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / reps;
}

int main()
{
    crd::memory::TlsfAllocator a(crd::usize{1} << 31);
    const crd::usize n = 1000000, m = 32;
    cont::Array<double> x(&a), d(&a);
    x.resize(n);
    d.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i] = std::sin(0.017 * static_cast<double>(i)) + 0.3 * std::cos(0.05 * static_cast<double>(i));
        d[i] = x[i];
    }
    double chk = 0;
    const double t_nlms = timed(
        [&]
        {
            dsp::NlmsFilter<double> f(&a, m, 0.5);
            double y = 0;
            for (crd::usize i = 0; i < n; ++i)
            {
                y = f.step(x[i], d[i]);
            }
            chk += y;
        },
        20);
    const double t_lms = timed(
        [&]
        {
            dsp::LmsFilter<double> f(&a, m, 0.01);
            double y = 0;
            for (crd::usize i = 0; i < n; ++i)
            {
                y = f.step(x[i], d[i]);
            }
            chk += y;
        },
        20);
    std::printf("CERID NLMS m=%zu N=%zu  %.4f ms/call\n", m, n, t_nlms);
    std::printf("CERID LMS  m=%zu N=%zu  %.4f ms/call (chk=%.4f)\n", m, n, t_lms, chk);
    return 0;
}
