// v12-b parallel Bessel/Airy batch vs MATLAB-MT (auto-multithreaded besselj over a 1M array). Cerid batch runs the
// full job pool; ns/element. Plain C arrays + crd containers (no STL). MATLAB-MT numbers from bench_bessel_matlab.m
// run WITHOUT maxNumCompThreads(1) (all cores).

#include <crd/hesap/special/special.hpp>

#include <crd/containers/array.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>

namespace sf = crd::hesap::special;
using crd::f64;
using crd::usize;

namespace
{
template <class F>
double per_elem(F&& f, usize n, int reps)
{
    f(); // warm
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; ++r)
    {
        f();
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / (static_cast<double>(reps) * n);
}
} // namespace

int main()
{
    crd::jobs::Config cfg; // full pool (hardware_concurrency)
    crd::jobs::init(cfg);
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 28);
    const usize n = static_cast<usize>(1) << 20; // 1M
    crd::containers::Array<f64> in(&alloc);
    crd::containers::Array<f64> out(&alloc);
    in.resize(n);
    out.resize(n);
    crd::u64 s = 0xBEEF1234ULL;
    for (usize i = 0; i < n; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        in[i] = 0.5 + (static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0)) * 39.5; // x ∈ [0.5, 40]
    }

    std::printf("# Cerid parallel Bessel/Airy batch (%u workers) vs MATLAB-MT (all cores), ns/elem\n",
                crd::jobs::num_workers());
    std::printf("function     Cerid(ns)   MATLAB-MT(ns)   speedup\n");
    const int reps = 40;
    auto row = [&](const char* name, auto call, double matlab)
    {
        const double t = per_elem(call, n, reps);
        std::printf("%-11s  %8.3f   %8.2f       %.2fx %s\n", name, t, matlab, matlab / t, (matlab > t ? "WIN" : "lose"));
    };
    row("cyl_J", [&] { sf::cyl_bessel_j_batch<f64>(out.data(), in.data(), n, 2.5); }, 27.66);
    row("cyl_Y", [&] { sf::cyl_neumann_batch<f64>(out.data(), in.data(), n, 2.5); }, 44.41);
    row("cyl_I", [&] { sf::cyl_bessel_i_batch<f64>(out.data(), in.data(), n, 2.5); }, 15.18);
    row("cyl_K", [&] { sf::cyl_bessel_k_batch<f64>(out.data(), in.data(), n, 2.5); }, 11.49);
    row("airy_Ai", [&] { sf::airy_ai_batch<f64>(out.data(), in.data(), n); }, 35.46);
    row("airy_Bi", [&] { sf::airy_bi_batch<f64>(out.data(), in.data(), n); }, 45.63);
    crd::jobs::shutdown();
    return 0;
}
