// v11-k: resample_poly throughput (the polyphase hot loop). Cerid resample_poly(N=1M) one-shot (filter design +
// upfirdn, like scipy.signal.resample_poly / MATLAB resample). Companion scipy/MATLAB/liquid scripts.
//
// MEASURED (WSL, 1 thread, AVX2, chk identical vs scipy + MATLAB ⇒ correct):
//   up=3/down=2: CERID 10.76 ms · scipy 12.60 (1.17x) · MATLAB-1T 23.49 (2.18x) · liquid-f32 12.36 (1.15x)
//   up=2/down=3: CERID  6.39 ms · scipy  6.87 (1.08x) · MATLAB-1T 12.73 (1.99x) · liquid-f32  6.35 (parity, f64 vs f32)
// Cerid BEATS scipy + MATLAB on both, beats/parities liquid while being f64 (liquid rresamp is f32). EARNED via the
// REVERSED polyphase tap bank (branch-free contiguous dot — the first cut, with per-tap bounds checks, LOST 0.84x/0.69x).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <crd/hesap/dsp/multirate.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;

static void run(crd::memory::IAllocator* a, crd::usize n, crd::usize up, crd::usize down, int reps)
{
    cont::Array<double> x(a);
    x.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i] = std::sin(2 * 3.14159265358979 * 0.05 * static_cast<double>(i)) +
               0.5 * std::sin(2 * 3.14159265358979 * 0.13 * static_cast<double>(i));
    }
    const cont::ConstSpan<double> xs(x.data(), n);
    auto warm = dsp::resample_poly<double>(a, xs, up, down);
    double chk = 0;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int r = 0; r < reps; ++r)
    {
        auto y = dsp::resample_poly<double>(a, xs, up, down);
        chk += y[y.size() / 2];
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    std::printf("CERID resample_poly N=%zu up=%zu down=%zu  %.4f ms/call (out=%zu chk=%.5f)\n", n, up, down,
                std::chrono::duration<double, std::milli>(t1 - t0).count() / reps, warm.size(), chk);
}

int main()
{
    crd::memory::TlsfAllocator a(crd::usize{1} << 31);
    run(&a, 1000000, 3, 2, 30);
    run(&a, 1000000, 2, 3, 30);
    return 0;
}
