// hesap-special batch boards, ns/element. Plain C arrays + crd containers (no STL).
//
// 1. The Lanczos lgamma and tgamma batches on one thread at the hesap-special kernel level the process selected
//    (CRD_CPU_LEVEL forces one): 4 Mi arguments in [0.1, 50] (the bench_special_matlab.m / bench_special_refs.py
//    input, CRD_MATLAB_1T=1 for MATLAB's single-thread peer row) and 8 Ki in the L2 cache. Each batch must give the
//    portable level's bits over the 4 Mi arguments (both kernels are bit-exact on every level), or the run exits 1.
// 2. The parallel Bessel/Airy and gamma batches on the full job pool against MATLAB-MT (auto-multithreaded besselj,
//    gammaln and gamma over the same arrays), from bench_bessel_matlab.m and bench_special_matlab.m run WITHOUT
//    maxNumCompThreads(1). The argument 1 stops after the one-thread rows (for a run pinned to one core).

#include <crd/containers/array.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/special/special.hpp>
#include <crd/hesap/special/special_dispatch.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <bit>
#include <chrono>
#include <cstdio>

namespace sf = crd::hesap::special;
using crd::f64;
using crd::usize;

namespace
{
template <class F> double per_elem(const F& f, usize n, int reps)
{
    f(); // warm
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; ++r)
    {
        f();
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() /
           (static_cast<double>(reps) * static_cast<double>(n));
}

// The fastest of `rounds` timings of f over n elements.
template <class F> double best_per_elem(const F& f, usize n, int rounds)
{
    double best = 1e300;
    for (int r = 0; r < rounds; ++r)
    {
        const double t = per_elem(f, n, 1);
        best = t < best ? t : best;
    }
    return best;
}

void fill_uniform(crd::containers::Array<f64>& a, usize n, crd::u64 seed, f64 lo, f64 hi)
{
    a.resize(n);
    crd::u64 s = seed;
    for (usize i = 0; i < n; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        a[i] = lo + (static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0)) * (hi - lo);
    }
}
} // namespace

int main(int argc, char** argv)
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 28);
    crd::containers::Array<f64> in(&alloc);
    crd::containers::Array<f64> out(&alloc);
    crd::containers::Array<f64> portable_out(&alloc);
    f64 checksum = 0.0;
    bool ok = true;

    // ---- 1. one thread (jobs not initialised: the batches run their serial path) ----
    const usize big = static_cast<usize>(4) << 20;
    const usize l2 = static_cast<usize>(8) << 10;
    fill_uniform(in, big, 0x12345ULL, 0.1, 50.0);
    out.resize(big);
    portable_out.resize(big);
    const sf::SpecialKernels& portable = *sf::special_kernel_levels()[0].table;
    const crd::CpuSelection& cpu = crd::cpu_selection();
    std::printf("# Lanczos gamma batches, 1 thread, hesap-special kernel level %s (selected %s), ns/elem\n",
                crd::cpu_level_name(sf::special_kernels().level), crd::cpu_level_name(cpu.selected));
    std::printf("function     4Mi [0.1,50]   8Ki in L2\n");
    using Kernel = void (*)(f64*, const f64*, usize) noexcept;
    auto single = [&](const char* name, Kernel batch, Kernel portable_kernel)
    {
        const double t_big = best_per_elem([&] { batch(out.data(), in.data(), big); }, big, 7);
        checksum += out[0] + out[big / 2U];
        portable_kernel(portable_out.data(), in.data(), big);
        usize misses = 0;
        for (usize i = 0; i < big; ++i)
        {
            misses += std::bit_cast<crd::u64>(out[i]) == std::bit_cast<crd::u64>(portable_out[i]) ? 0U : 1U;
        }
        if (misses != 0U)
        {
            std::printf("!MISMATCH! %s: %zu of the 4Mi values are not the portable level's bits\n", name, misses);
            ok = false;
        }
        const double t_l2 = best_per_elem(
            [&]
            {
                for (int r = 0; r < 64; ++r)
                {
                    batch(out.data(), in.data(), l2);
                }
            },
            64U * l2, 7);
        checksum += out[l2 / 2U];
        std::printf("%-11s  %8.3f       %8.3f\n", name, t_big, t_l2);
    };
    single("lgamma", &sf::lgamma_batch<f64>, portable.lgamma_lanczos);
    single("tgamma", &sf::gamma_batch<f64>, portable.tgamma_lanczos);
    if (argc > 1 && argv[1][0] == '1')
    {
        std::printf("# checksum %.6e\n", checksum);
        return ok ? 0 : 1; // "1": the one-thread rows only (a pinned run)
    }

    // ---- 2. the full job pool against MATLAB-MT ----
    crd::jobs::Config cfg; // full pool (hardware_concurrency)
    crd::jobs::init(cfg);
    const usize n = static_cast<usize>(1) << 20;   // 1M
    fill_uniform(in, n, 0xBEEF1234ULL, 0.5, 40.0); // x in [0.5, 40]
    std::printf("\n# Cerid parallel special batches (%u workers) vs MATLAB-MT (all cores), ns/elem\n",
                crd::jobs::num_workers());
    std::printf("function     Cerid(ns)   MATLAB-MT(ns)   speedup\n");
    const int reps = 40;
    auto row = [&](const char* name, usize count, auto call, double matlab)
    {
        const double t = per_elem(call, count, reps);
        checksum += out[count / 2U];
        std::printf("%-11s  %8.3f   %8.2f       %.2fx %s\n", name, t, matlab, matlab / t,
                    (matlab > t ? "WIN" : "lose"));
    };
    row("cyl_J", n, [&] { sf::cyl_bessel_j_batch<f64>(out.data(), in.data(), n, 2.5); }, 27.66);
    row("cyl_Y", n, [&] { sf::cyl_neumann_batch<f64>(out.data(), in.data(), n, 2.5); }, 44.41);
    row("cyl_I", n, [&] { sf::cyl_bessel_i_batch<f64>(out.data(), in.data(), n, 2.5); }, 15.18);
    row("cyl_K", n, [&] { sf::cyl_bessel_k_batch<f64>(out.data(), in.data(), n, 2.5); }, 11.49);
    row("airy_Ai", n, [&] { sf::airy_ai_batch<f64>(out.data(), in.data(), n); }, 35.46);
    row("airy_Bi", n, [&] { sf::airy_bi_batch<f64>(out.data(), in.data(), n); }, 45.63);
    fill_uniform(in, big, 0x12345ULL, 0.1, 50.0);
    row("lgamma 4Mi", big, [&] { sf::lgamma_batch<f64>(out.data(), in.data(), big); }, 0.65);
    row("tgamma 4Mi", big, [&] { sf::gamma_batch<f64>(out.data(), in.data(), big); }, 1.16);
    crd::jobs::shutdown();
    std::printf("# checksum %.6e\n", checksum);
    return ok ? 0 : 1;
}
