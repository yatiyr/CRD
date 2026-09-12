// v10-a gates: complex FFT substrate. THE correctness gate is the brute-force O(N^2) DFT (computed in f64
// as the truth) — NOT the round-trip IFFT(FFT(x))==x, which can cancel a twiddle-sign/normalization error
// and pass while the forward transform is wrong (the FFT edition of the odeint-d4 trap). Plus: round-trip
// (secondary), Parseval, inverse-vs-naive-IDFT, run-twice determinism (bit-identical), and plan reuse.

#include <crd/containers/array.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/bluestein.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/hesap/fft/nd_fft.hpp>
#include <crd/hesap/fft/real_fft.hpp>
#include <crd/hesap/fft/sparse_fft.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>

using crd::f32;
using crd::f64;
using crd::usize;
namespace fft = crd::hesap::fft;
namespace cont = crd::containers;
using crd::hesap::Complex;

namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;

// Deterministic LCG fill in [-1, 1] for both components.
template <typename T> void fill_lcg(cont::Array<Complex<T>>& x, usize n, crd::u64 seed)
{
    x.resize(n);
    crd::u64 s = seed;
    auto next = [&s]() -> double
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return (static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53)) * 2.0 - 1.0;
    };
    for (usize i = 0; i < n; ++i)
    {
        x[i] = Complex<T>{static_cast<T>(next()), static_cast<T>(next())};
    }
}

// Brute-force DFT in f64 — the ground truth. (j*k)%n keeps the angle small for reference accuracy.
void naive_dft(cont::ConstSpan<Complex<f64>> x, cont::Span<Complex<f64>> out, bool inverse)
{
    const usize n = x.size();
    const double sgn = inverse ? 1.0 : -1.0;
    for (usize k = 0; k < n; ++k)
    {
        double re = 0.0;
        double im = 0.0;
        for (usize j = 0; j < n; ++j)
        {
            const double ang = sgn * kTwoPi * static_cast<double>((j * k) % n) / static_cast<double>(n);
            const double c = std::cos(ang);
            const double sn = std::sin(ang);
            re += x[j].re * c - x[j].im * sn;
            im += x[j].re * sn + x[j].im * c;
        }
        out[k] = Complex<f64>{re, im};
    }
}

// Forward FFT(x) vs the naive DFT — the gate. Returns max |fft - dft| / (1 + max|dft|).
template <typename T> double forward_vs_naive(usize n, crd::memory::IAllocator* alloc)
{
    cont::Array<Complex<T>> x(alloc);
    fill_lcg(x, n, 0x9E3779B97F4A7C15ULL ^ n);

    cont::Array<Complex<f64>> xd(alloc);
    cont::Array<Complex<f64>> ref(alloc);
    xd.resize(n);
    ref.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        xd[i] = Complex<f64>{static_cast<f64>(x[i].re), static_cast<f64>(x[i].im)};
    }
    naive_dft(cont::ConstSpan<Complex<f64>>(xd.data(), n), cont::Span<Complex<f64>>(ref.data(), n), false);

    const fft::FftPlan<T> plan(alloc, n);
    plan.execute(cont::Span<Complex<T>>(x.data(), n), fft::FftDirection::Forward);

    double maxref = 0.0;
    double maxerr = 0.0;
    for (usize k = 0; k < n; ++k)
    {
        maxref = std::max(maxref, std::hypot(ref[k].re, ref[k].im));
    }
    for (usize k = 0; k < n; ++k)
    {
        const double dr = static_cast<f64>(x[k].re) - ref[k].re;
        const double di = static_cast<f64>(x[k].im) - ref[k].im;
        maxerr = std::max(maxerr, std::hypot(dr, di));
    }
    return maxerr / (1.0 + maxref);
}

} // namespace

TEST_CASE("fft: forward matches the brute-force DFT (f64) across sizes", "[fft]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    for (usize n : {1U, 2U, 4U, 8U, 16U, 32U, 64U, 256U, 1024U})
    {
        const double rel = forward_vs_naive<f64>(n, &alloc);
        INFO("n=" << n << " rel=" << rel);
        CHECK(rel < 1e-12);
    }
}

TEST_CASE("fft: forward matches the brute-force DFT (f32) across sizes", "[fft]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    for (usize n : {2U, 8U, 64U, 256U, 1024U})
    {
        const double rel = forward_vs_naive<f32>(n, &alloc);
        INFO("n=" << n << " rel=" << rel);
        CHECK(rel < 2e-4);
    }
}

TEST_CASE("fft: inverse matches the brute-force IDFT (unnormalized) across sizes", "[fft]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    // Sizes 8/16/32 exercise the AoS lane-trick small-N codelets (conj-trick inverse) — the forward sizes test
    // alone leaves their inverse path ungated. The rest cover the Stockham/four-step inverse.
    for (usize n : {8U, 16U, 32U, 64U, 256U, 1024U})
    {
        cont::Array<Complex<f64>> x(&alloc);
        fill_lcg(x, n, 12345ULL + n);
        cont::Array<Complex<f64>> ref(&alloc);
        ref.resize(n);
        naive_dft(cont::ConstSpan<Complex<f64>>(x.data(), n), cont::Span<Complex<f64>>(ref.data(), n), true);

        const fft::FftPlan<f64> plan(&alloc, n);
        plan.execute(cont::Span<Complex<f64>>(x.data(), n), fft::FftDirection::Inverse);

        double maxerr = 0.0;
        double maxref = 0.0;
        for (usize k = 0; k < n; ++k)
        {
            maxerr = std::max(maxerr, std::hypot(x[k].re - ref[k].re, x[k].im - ref[k].im));
            maxref = std::max(maxref, std::hypot(ref[k].re, ref[k].im));
        }
        INFO("n=" << n << " inverse-vs-naive maxerr=" << maxerr);
        CHECK(maxerr < 1e-10 * (1.0 + maxref));
    }
}

TEST_CASE("fft: round-trip ifft_normalized(fft(x)) == x", "[fft]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    for (usize n : {4U, 64U, 1024U})
    {
        cont::Array<Complex<f64>> x(&alloc);
        fill_lcg(x, n, 777ULL + n);
        cont::Array<Complex<f64>> x0(&alloc);
        x0.resize(n);
        std::memcpy(x0.data(), x.data(), n * sizeof(Complex<f64>));

        fft::fft<f64>(&alloc, cont::Span<Complex<f64>>(x.data(), n), fft::FftDirection::Forward);
        fft::ifft_normalized<f64>(&alloc, cont::Span<Complex<f64>>(x.data(), n));

        double maxerr = 0.0;
        for (usize k = 0; k < n; ++k)
        {
            maxerr = std::max(maxerr, std::hypot(x[k].re - x0[k].re, x[k].im - x0[k].im));
        }
        INFO("n=" << n << " round-trip maxerr=" << maxerr);
        CHECK(maxerr < 1e-13);
    }
}

TEST_CASE("fft: Parseval's theorem", "[fft]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize n = 512;
    cont::Array<Complex<f64>> x(&alloc);
    fill_lcg(x, n, 2024ULL);
    double e_time = 0.0;
    for (usize i = 0; i < n; ++i)
    {
        e_time += x[i].re * x[i].re + x[i].im * x[i].im;
    }
    fft::fft<f64>(&alloc, cont::Span<Complex<f64>>(x.data(), n), fft::FftDirection::Forward);
    double e_freq = 0.0;
    for (usize i = 0; i < n; ++i)
    {
        e_freq += x[i].re * x[i].re + x[i].im * x[i].im;
    }
    e_freq /= static_cast<double>(n); // Σ|X|² = n·Σ|x|²
    INFO("Parseval time=" << e_time << " freq/n=" << e_freq);
    CHECK(std::abs(e_time - e_freq) / e_time < 1e-12);
}

TEST_CASE("fft: run-twice bit identity (deterministic plan)", "[fft][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize n = 1024;
    auto run = [&](cont::Array<Complex<f64>>& out)
    {
        fill_lcg(out, n, 55ULL);
        fft::fft<f64>(&alloc, cont::Span<Complex<f64>>(out.data(), n), fft::FftDirection::Forward);
    };
    cont::Array<Complex<f64>> a(&alloc);
    cont::Array<Complex<f64>> b(&alloc);
    run(a);
    run(b);
    CHECK(std::memcmp(a.data(), b.data(), n * sizeof(Complex<f64>)) == 0);
}

TEST_CASE("fft: four-step path (large n) matches the radix-2 reference oracle", "[fft]")
{
    crd::memory::TlsfAllocator alloc(1ULL << 31);
    // n >= kFourStepMin (2^19) triggers the four-step path; the naive O(N^2) DFT is too slow here, so cross-check
    // against execute_reference (the radix-2 oracle, validated against the naive DFT at small n, O(n log n)). Gate
    // the WHOLE crossover band (2^19..2^23), not just 2^22 — the four-step runs at every size in it, and the
    // block-width/partial-block boundaries differ across n1/n2 splits (e.g. odd m_log2 ⇒ n1≠n2). 2^21/2^22 exercise
    // the default 2048=64×32 hierarchical sub-FFT (n1=2048); 2^23 the 4096=64×64 one too (n1=4096, n2=2048).
    // 2^17/2^18: the FFT-CRUSH 2026-07-03 f64 mid-band four-step opt-ins (128K = 1024x128, 256K square).
    for (usize n : {1U << 17, 1U << 18, 1U << 19, 1U << 20, 1U << 21, 1U << 22, 1U << 23})
    {
        cont::Array<Complex<f64>> x(&alloc);
        fill_lcg(x, n, 31337ULL + n);
        cont::Array<Complex<f64>> a(&alloc);
        cont::Array<Complex<f64>> b(&alloc);
        a.resize(n);
        b.resize(n);
        std::memcpy(a.data(), x.data(), n * sizeof(Complex<f64>));
        std::memcpy(b.data(), x.data(), n * sizeof(Complex<f64>));

        const fft::FftPlan<f64> plan(&alloc, n);
        plan.execute(cont::Span<Complex<f64>>(a.data(), n), fft::FftDirection::Forward);           // four-step
        plan.execute_reference(cont::Span<Complex<f64>>(b.data(), n), fft::FftDirection::Forward); // oracle

        double maxref = 0.0;
        double maxerr = 0.0;
        for (usize k = 0; k < n; ++k)
        {
            maxref = std::max(maxref, std::hypot(b[k].re, b[k].im));
            maxerr = std::max(maxerr, std::hypot(a[k].re - b[k].re, a[k].im - b[k].im));
        }
        const double rel = maxerr / (1.0 + maxref);
        INFO("n=" << n << " four-step vs oracle rel=" << rel);
        CHECK(rel < 1e-12);

        // round-trip through the four-step path.
        plan.execute(cont::Span<Complex<f64>>(a.data(), n), fft::FftDirection::Inverse);
        const double inv = 1.0 / static_cast<double>(n);
        double rtmax = 0.0;
        for (usize k = 0; k < n; ++k)
        {
            rtmax = std::max(rtmax, std::hypot(a[k].re * inv - x[k].re, a[k].im * inv - x[k].im));
        }
        INFO("n=" << n << " four-step round-trip max=" << rtmax);
        CHECK(rtmax < 1e-12);
    }
}

TEST_CASE("fft: four-step path (large n, f32) matches the radix-2 reference oracle", "[fft]")
{
    crd::memory::TlsfAllocator alloc(1ULL << 30);
    // M7 Phase 5: f32 FORWARD now uses the Vec8f hierarchical sub-FFTs by default (1024=32×32, 2048=64×32,
    // 4096=64×64). Gate the crossover band 2^19..2^23 against the f32 radix-2 oracle in the f32 tolerance class
    // (~1e-3 — the hier and the oracle are different f32 summation orders; both ≈f32-eps vs the exact DFT). 2^20
    // exercises 1024 (both sub-FFTs), 2^21/2^22 the 2048, 2^23 the 4096. Disable with -DCRD_FFT_DISABLE_F32_HIER.
    for (usize n : {1U << 19, 1U << 20, 1U << 21, 1U << 22, 1U << 23})
    {
        cont::Array<Complex<f32>> x(&alloc);
        fill_lcg(x, n, 31337ULL + n);
        cont::Array<Complex<f32>> a(&alloc);
        cont::Array<Complex<f32>> b(&alloc);
        a.resize(n);
        b.resize(n);
        std::memcpy(a.data(), x.data(), n * sizeof(Complex<f32>));
        std::memcpy(b.data(), x.data(), n * sizeof(Complex<f32>));

        const fft::FftPlan<f32> plan(&alloc, n);
        plan.execute(cont::Span<Complex<f32>>(a.data(), n), fft::FftDirection::Forward);           // hier four-step
        plan.execute_reference(cont::Span<Complex<f32>>(b.data(), n), fft::FftDirection::Forward); // oracle

        double maxref = 0.0;
        double maxerr = 0.0;
        for (usize k = 0; k < n; ++k)
        {
            maxref = std::max(maxref, std::hypot(static_cast<double>(b[k].re), static_cast<double>(b[k].im)));
            maxerr = std::max(maxerr, std::hypot(static_cast<double>(a[k].re) - static_cast<double>(b[k].re),
                                                 static_cast<double>(a[k].im) - static_cast<double>(b[k].im)));
        }
        const double rel = maxerr / (1.0 + maxref);
        INFO("n=" << n << " f32 four-step vs oracle rel=" << rel);
        CHECK(rel < 1e-3); // f32 class

        // round-trip: forward (hier) + inverse (radix-8 fallback) recovers x in f32 class.
        plan.execute(cont::Span<Complex<f32>>(a.data(), n), fft::FftDirection::Inverse);
        const double inv = 1.0 / static_cast<double>(n);
        double rtmax = 0.0;
        for (usize k = 0; k < n; ++k)
        {
            rtmax = std::max(rtmax, std::hypot(static_cast<double>(a[k].re) * inv - static_cast<double>(x[k].re),
                                               static_cast<double>(a[k].im) * inv - static_cast<double>(x[k].im)));
        }
        INFO("n=" << n << " f32 four-step round-trip max=" << rtmax);
        CHECK(rtmax < 1e-3);
    }
}

TEST_CASE("fft: standalone-hier band (f32) matches the radix-2 reference oracle", "[fft]")
{
    crd::memory::TlsfAllocator alloc(1ULL << 26);
    // FFT-CRUSH 2026-07-03: f32 1024..65536 forward runs the standalone-hier 2-pass (Vec8f
    // codelet{n1}_stage1_fused_sh -> codelet{n2}_batched, natural order) and 128K/256K the deep-split
    // 3-pass (S1 fused_sh -> S2 fused_notr -> S3 batched_strided) — gate EVERY split of both bands
    // against the f32 radix-2 oracle (different f32 summation orders ⇒ the f32 tolerance class), plus the
    // inverse round-trip (inverse = Stockham / four-step fallback).
    for (usize n : {1024U, 2048U, 4096U, 8192U, 16384U, 32768U, 65536U, 131072U, 262144U})
    {
        cont::Array<Complex<f32>> x(&alloc);
        fill_lcg(x, n, 424242ULL + n);
        cont::Array<Complex<f32>> a(&alloc);
        cont::Array<Complex<f32>> b(&alloc);
        a.resize(n);
        b.resize(n);
        std::memcpy(a.data(), x.data(), n * sizeof(Complex<f32>));
        std::memcpy(b.data(), x.data(), n * sizeof(Complex<f32>));

        const fft::FftPlan<f32> plan(&alloc, n);
        plan.execute(cont::Span<Complex<f32>>(a.data(), n), fft::FftDirection::Forward);           // standalone-hier
        plan.execute_reference(cont::Span<Complex<f32>>(b.data(), n), fft::FftDirection::Forward); // oracle

        double maxref = 0.0;
        double maxerr = 0.0;
        for (usize k = 0; k < n; ++k)
        {
            maxref = std::max(maxref, std::hypot(static_cast<double>(b[k].re), static_cast<double>(b[k].im)));
            maxerr = std::max(maxerr, std::hypot(static_cast<double>(a[k].re) - static_cast<double>(b[k].re),
                                                 static_cast<double>(a[k].im) - static_cast<double>(b[k].im)));
        }
        const double rel = maxerr / (1.0 + maxref);
        INFO("n=" << n << " f32 standalone-hier vs oracle rel=" << rel);
        CHECK(rel < 2e-4); // f32 class (mid-band error growth is small)

        // round-trip: forward (standalone-hier) + inverse (Stockham fallback) recovers x in f32 class.
        plan.execute(cont::Span<Complex<f32>>(a.data(), n), fft::FftDirection::Inverse);
        const double inv = 1.0 / static_cast<double>(n);
        double rtmax = 0.0;
        for (usize k = 0; k < n; ++k)
        {
            rtmax = std::max(rtmax, std::hypot(static_cast<double>(a[k].re) * inv - static_cast<double>(x[k].re),
                                               static_cast<double>(a[k].im) * inv - static_cast<double>(x[k].im)));
        }
        INFO("n=" << n << " f32 standalone-hier round-trip max=" << rtmax);
        CHECK(rtmax < 2e-4);
    }
}

TEST_CASE("fft: scheduled radix-8/16 combine passes match the radix-2 oracle", "[fft]")
{
    crd::memory::TlsfAllocator alloc(1ULL << 27);
    // Cover the size-aware combine planner: radix-16 band (m_log2 in [12,17] ⇒ 4096..131072) AND the
    // radix-8 band (>=262144 and the small <4096). The naive O(N^2) DFT is too slow here, so cross-check
    // against execute_reference (the radix-2 oracle, itself validated against the naive DFT at small n).
    for (usize n : {2048U, 4096U, 16384U, 65536U, 131072U, 262144U})
    {
        cont::Array<Complex<f64>> x(&alloc);
        fill_lcg(x, n, 0xABCDEFULL + n);
        cont::Array<Complex<f64>> a(&alloc);
        cont::Array<Complex<f64>> b(&alloc);
        a.resize(n);
        b.resize(n);
        std::memcpy(a.data(), x.data(), n * sizeof(Complex<f64>));
        std::memcpy(b.data(), x.data(), n * sizeof(Complex<f64>));

        const fft::FftPlan<f64> plan(&alloc, n);
        plan.execute(cont::Span<Complex<f64>>(a.data(), n), fft::FftDirection::Forward);           // radix-8/16
        plan.execute_reference(cont::Span<Complex<f64>>(b.data(), n), fft::FftDirection::Forward); // oracle

        double maxref = 0.0;
        double maxerr = 0.0;
        for (usize k = 0; k < n; ++k)
        {
            maxref = std::max(maxref, std::hypot(b[k].re, b[k].im));
            maxerr = std::max(maxerr, std::hypot(a[k].re - b[k].re, a[k].im - b[k].im));
        }
        const double rel = maxerr / (1.0 + maxref);
        INFO("n=" << n << " radix-8/16 vs oracle rel=" << rel);
        CHECK(rel < 1e-12);

        // inverse round-trip through the same path.
        plan.execute(cont::Span<Complex<f64>>(a.data(), n), fft::FftDirection::Inverse);
        const double inv = 1.0 / static_cast<double>(n);
        double rtmax = 0.0;
        for (usize k = 0; k < n; ++k)
        {
            rtmax = std::max(rtmax, std::hypot(a[k].re * inv - x[k].re, a[k].im * inv - x[k].im));
        }
        INFO("n=" << n << " round-trip max=" << rtmax);
        CHECK(rtmax < 1e-12);
    }
}

TEST_CASE("fft: batched FFT matches the per-transform oracle", "[fft]")
{
    crd::memory::TlsfAllocator alloc(1ULL << 24);
    // B independent transforms of size m, ELEMENT-MAJOR (element i of transform t at data[i*B + t]).
    for (usize m : {8U, 64U, 256U, 1024U})
    {
        const usize batch = 7; // not a multiple of 4 ⇒ exercises the SIMD body + scalar tail
        const fft::FftPlan<f64> plan(&alloc, m);
        cont::Array<Complex<f64>> in(&alloc);
        in.resize(m * batch);
        fill_lcg(in, m * batch, 0x6789ULL + m);
        cont::Array<Complex<f64>> work(&alloc);
        work.resize(m * batch);
        std::memcpy(work.data(), in.data(), m * batch * sizeof(Complex<f64>));

        plan.execute_batched(cont::Span<Complex<f64>>(work.data(), m * batch), batch, fft::FftDirection::Forward);

        double maxrel = 0.0;
        for (usize t = 0; t < batch; ++t)
        {
            cont::Array<Complex<f64>> single(&alloc);
            single.resize(m);
            for (usize i = 0; i < m; ++i)
            {
                single[i] = in[i * batch + t];
            }
            plan.execute_reference(cont::Span<Complex<f64>>(single.data(), m), fft::FftDirection::Forward);
            double maxref = 0.0;
            double maxerr = 0.0;
            for (usize i = 0; i < m; ++i)
            {
                maxref = std::max(maxref, std::hypot(single[i].re, single[i].im));
                maxerr = std::max(
                    maxerr, std::hypot(work[i * batch + t].re - single[i].re, work[i * batch + t].im - single[i].im));
            }
            maxrel = std::max(maxrel, maxerr / (1.0 + maxref));
        }
        INFO("m=" << m << " batched-vs-oracle maxrel=" << maxrel);
        CHECK(maxrel < 1e-12);
    }
}

TEST_CASE("fft: batched FFT even-batch (AoS over-2 fast path) matches the oracle, both directions", "[fft]")
{
    crd::memory::TlsfAllocator alloc(1ULL << 24);
    // batch even ⇒ engages the N=8 and N=16 AoS over-2 fast paths (m=16 batch=8 is cache-resident → atom);
    // m=64 stays on the SoA path (even-batch coverage, both directions).
    for (usize m : {8U, 16U, 64U})
    {
        const usize batch = 8;
        for (auto dir : {fft::FftDirection::Forward, fft::FftDirection::Inverse})
        {
            const fft::FftPlan<f64> plan(&alloc, m);
            cont::Array<Complex<f64>> in(&alloc);
            in.resize(m * batch);
            fill_lcg(in, m * batch, 0xBEEFULL + m + (dir == fft::FftDirection::Inverse ? 1U : 0U));
            cont::Array<Complex<f64>> work(&alloc);
            work.resize(m * batch);
            std::memcpy(work.data(), in.data(), m * batch * sizeof(Complex<f64>));

            plan.execute_batched(cont::Span<Complex<f64>>(work.data(), m * batch), batch, dir);

            double maxrel = 0.0;
            for (usize t = 0; t < batch; ++t)
            {
                cont::Array<Complex<f64>> single(&alloc);
                single.resize(m);
                for (usize i = 0; i < m; ++i)
                {
                    single[i] = in[i * batch + t];
                }
                plan.execute_reference(cont::Span<Complex<f64>>(single.data(), m), dir);
                double maxref = 0.0;
                double maxerr = 0.0;
                for (usize i = 0; i < m; ++i)
                {
                    maxref = std::max(maxref, std::hypot(single[i].re, single[i].im));
                    maxerr = std::max(maxerr, std::hypot(work[i * batch + t].re - single[i].re,
                                                         work[i * batch + t].im - single[i].im));
                }
                maxrel = std::max(maxrel, maxerr / (1.0 + maxref));
            }
            INFO("m=" << m << " dir=" << (dir == fft::FftDirection::Forward ? "fwd" : "inv")
                      << " even-batch maxrel=" << maxrel);
            CHECK(maxrel < 1e-12);
        }
    }
}

TEST_CASE("rfft: forward matches the brute-force real DFT + round-trip", "[fft][rfft]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    for (usize n : {4U, 8U, 16U, 64U, 256U, 1024U})
    {
        cont::Array<Complex<f64>> xc(&alloc); // reuse the complex LCG fill, take .re as the real input
        fill_lcg(xc, n, 0x5EED1234ULL ^ n);
        cont::Array<f64> x(&alloc);
        x.resize(n);
        for (usize i = 0; i < n; ++i)
        {
            x[i] = xc[i].re;
        }
        // brute-force real DFT (f64 truth) for k = 0..n/2
        const usize h = n / 2;
        cont::Array<Complex<f64>> ref(&alloc);
        ref.resize(h + 1);
        for (usize k = 0; k <= h; ++k)
        {
            double re = 0.0;
            double im = 0.0;
            for (usize j = 0; j < n; ++j)
            {
                const double ang = -kTwoPi * static_cast<double>((j * k) % n) / static_cast<double>(n);
                re += x[j] * std::cos(ang);
                im += x[j] * std::sin(ang);
            }
            ref[k] = Complex<f64>{re, im};
        }
        const fft::RealFftPlan<f64> plan(&alloc, n);
        cont::Array<Complex<f64>> out(&alloc);
        out.resize(h + 1);
        plan.rfft(cont::ConstSpan<f64>(x.data(), n), cont::Span<Complex<f64>>(out.data(), h + 1));
        double maxref = 0.0;
        double maxerr = 0.0;
        for (usize k = 0; k <= h; ++k)
        {
            maxref = std::max(maxref, std::hypot(ref[k].re, ref[k].im));
            maxerr = std::max(maxerr, std::hypot(out[k].re - ref[k].re, out[k].im - ref[k].im));
        }
        INFO("n=" << n << " rfft-vs-naive rel=" << maxerr / (1.0 + maxref));
        CHECK(maxerr / (1.0 + maxref) < 1e-12);

        // round-trip irfft(rfft(x)) == x
        cont::Array<f64> back(&alloc);
        back.resize(n);
        plan.irfft(cont::ConstSpan<Complex<f64>>(out.data(), h + 1), cont::Span<f64>(back.data(), n));
        double rt = 0.0;
        for (usize i = 0; i < n; ++i)
        {
            rt = std::max(rt, std::abs(back[i] - x[i]));
        }
        INFO("n=" << n << " rfft round-trip max=" << rt);
        CHECK(rt < 1e-12);
    }
}

TEST_CASE("fft: a plan is reusable across inputs", "[fft]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize n = 128;
    const fft::FftPlan<f64> plan(&alloc, n);
    for (crd::u64 seed : {1ULL, 2ULL, 3ULL})
    {
        cont::Array<Complex<f64>> x(&alloc);
        fill_lcg(x, n, seed);
        cont::Array<Complex<f64>> xd(&alloc);
        cont::Array<Complex<f64>> ref(&alloc);
        xd.resize(n);
        ref.resize(n);
        for (usize i = 0; i < n; ++i)
        {
            xd[i] = x[i];
        }
        naive_dft(cont::ConstSpan<Complex<f64>>(xd.data(), n), cont::Span<Complex<f64>>(ref.data(), n), false);
        plan.execute(cont::Span<Complex<f64>>(x.data(), n), fft::FftDirection::Forward);
        double maxerr = 0.0;
        for (usize k = 0; k < n; ++k)
        {
            maxerr = std::max(maxerr, std::hypot(x[k].re - ref[k].re, x[k].im - ref[k].im));
        }
        INFO("seed=" << seed << " maxerr=" << maxerr);
        CHECK(maxerr < 1e-11);
    }
}

// v10-c — Bluestein (chirp-z) arbitrary-size FFT. Gate = the brute-force DFT (NOT round-trip) over prime AND
// composite non-power-of-two sizes, plus the round-trip. Proves any-size O(n log n) over the pow-2 engine.
TEST_CASE("fft: Bluestein arbitrary-size (primes + composites, f64) vs brute-force DFT + round-trip", "[fft][bluestein]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    for (usize n : {1U, 2U, 3U, 5U, 7U, 17U, 100U, 360U, 1009U, 1031U, 4000U})
    {
        cont::Array<Complex<f64>> x(&alloc);
        cont::Array<Complex<f64>> y(&alloc);
        cont::Array<Complex<f64>> ref(&alloc);
        fill_lcg(x, n, 0xB5297A4DULL ^ n);
        y.resize(n);
        ref.resize(n);
        for (usize i = 0; i < n; ++i)
        {
            y[i] = x[i];
        }
        naive_dft(cont::ConstSpan<Complex<f64>>(x.data(), n), cont::Span<Complex<f64>>(ref.data(), n), false);

        const fft::BluesteinPlan<f64> plan(&alloc, n);
        plan.execute(cont::Span<Complex<f64>>(y.data(), n), fft::FftDirection::Forward);

        double maxref = 0.0;
        double maxerr = 0.0;
        for (usize k = 0; k < n; ++k)
        {
            maxref = std::max(maxref, std::hypot(ref[k].re, ref[k].im));
            maxerr = std::max(maxerr, std::hypot(y[k].re - ref[k].re, y[k].im - ref[k].im));
        }
        INFO("n=" << n << " fwd-vs-DFT rel=" << (maxerr / (1.0 + maxref)));
        CHECK(maxerr / (1.0 + maxref) < 1e-12);

        plan.execute(cont::Span<Complex<f64>>(y.data(), n), fft::FftDirection::Inverse);
        double rt = 0.0;
        for (usize k = 0; k < n; ++k)
        {
            rt = std::max(rt, std::hypot(y[k].re - x[k].re, y[k].im - x[k].im));
        }
        INFO("n=" << n << " roundtrip=" << rt);
        CHECK(rt < 1e-12);
    }
}

// v10-e — N-dimensional FFT. Gate = a direct full N-D DFT (O(total²), small shapes) — independent of the
// row-column structure — over 2D/3D/4D shapes INCLUDING non-power-of-two axes (routed through Bluestein), plus
// the round-trip. The result is thread-count-independent by construction (each 1D line is an independent transform).
namespace
{
void naive_ndft(const Complex<f64>* x, Complex<f64>* out, const usize* dims, usize ndim, usize total, bool inv)
{
    usize st[8];
    usize s = 1;
    for (usize i = ndim; i-- > 0;)
    {
        st[i] = s;
        s *= dims[i];
    }
    const double sgn = inv ? 1.0 : -1.0;
    for (usize kk = 0; kk < total; ++kk) // output flat multi-index
    {
        double re = 0.0;
        double im = 0.0;
        for (usize nn = 0; nn < total; ++nn) // input flat multi-index
        {
            double frac = 0.0;
            for (usize ax = 0; ax < ndim; ++ax)
            {
                const usize kax = (kk / st[ax]) % dims[ax];
                const usize nax = (nn / st[ax]) % dims[ax];
                frac += static_cast<double>((kax * nax) % dims[ax]) / static_cast<double>(dims[ax]);
            }
            const double th = sgn * kTwoPi * frac;
            re += x[nn].re * std::cos(th) - x[nn].im * std::sin(th);
            im += x[nn].re * std::sin(th) + x[nn].im * std::cos(th);
        }
        out[kk] = Complex<f64>{re, im};
    }
}
} // namespace

TEST_CASE("fft: N-D (2D/3D/4D, pow-2 + non-pow-2 axes) vs full N-D DFT + round-trip", "[fft][ndfft]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize shapes[][4] = {{4, 4, 0, 0}, {8, 6, 0, 0}, {16, 3, 0, 0}, {4, 4, 4, 0}, {8, 4, 2, 0}, {6, 5, 2, 0}};
    const usize ndims[] = {2, 2, 2, 3, 3, 3};
    for (usize si = 0; si < 6; ++si)
    {
        const usize nd = ndims[si];
        usize total = 1;
        for (usize i = 0; i < nd; ++i)
        {
            total *= shapes[si][i];
        }
        cont::Array<Complex<f64>> x(&alloc);
        cont::Array<Complex<f64>> y(&alloc);
        cont::Array<Complex<f64>> ref(&alloc);
        fill_lcg(x, total, 0xC0FFEEULL ^ total);
        y.resize(total);
        ref.resize(total);
        for (usize i = 0; i < total; ++i)
        {
            y[i] = x[i];
        }
        cont::Array<usize> dims(&alloc);
        dims.resize(nd);
        for (usize i = 0; i < nd; ++i)
        {
            dims[i] = shapes[si][i];
        }
        const fft::NdFftPlan<f64> plan(&alloc, cont::ConstSpan<usize>(dims.data(), nd));
        plan.execute(cont::Span<Complex<f64>>(y.data(), total), fft::FftDirection::Forward);
        naive_ndft(x.data(), ref.data(), dims.data(), nd, total, false);

        double maxref = 0.0;
        double maxerr = 0.0;
        for (usize k = 0; k < total; ++k)
        {
            maxref = std::max(maxref, std::hypot(ref[k].re, ref[k].im));
            maxerr = std::max(maxerr, std::hypot(y[k].re - ref[k].re, y[k].im - ref[k].im));
        }
        INFO("shape#" << si << " nd=" << nd << " fwd-vs-DFT=" << (maxerr / (1.0 + maxref)));
        CHECK(maxerr / (1.0 + maxref) < 1e-12);

        plan.execute(cont::Span<Complex<f64>>(y.data(), total), fft::FftDirection::Inverse);
        double rt = 0.0;
        for (usize k = 0; k < total; ++k)
        {
            rt = std::max(rt, std::hypot(y[k].re - x[k].re, y[k].im - x[k].im));
        }
        CHECK(rt < 1e-12);
    }
}

// v10-h — Sparse FFT (HIKP 2012), END-TO-END SUB-LINEAR + NOISE-ROBUST. Multi-scale binary location (f bit-by-bit
// from O(log n) offset phases) + voting + median bucket estimation; no O(n) step. Gate: EXACT k-sparse (frequencies
// exact + coeffs to filter accuracy) AND NOISY k-sparse (k dominant tones + Gaussian noise → all frequencies
// recovered, coeffs to the √(n/B)·σ/√R floor). Tones planted, x = (1/n)Σ cⱼ e^{2πi fⱼ·/n}.
TEST_CASE("fft: Sparse FFT (HIKP) sub-linear + noise-robust recovery", "[fft][sparsefft]")
{
    crd::memory::TlsfAllocator alloc(1U << 28);
    const usize ns[] = {1024U, 4096U, 16384U, 4096U, 16384U, 65536U};
    const usize ks[] = {3U, 8U, 6U, 5U, 8U, 6U};
    const double noises[] = {0.0, 0.0, 0.0, 0.006, 0.004, 0.0015};
    for (usize ci = 0; ci < 6; ++ci)
    {
        const usize n = ns[ci];
        const usize k = ks[ci];
        const double noise = noises[ci];
        cont::Array<usize> pf(&alloc);
        cont::Array<Complex<f64>> pc(&alloc);
        pf.resize(k);
        pc.resize(k);
        crd::u64 s = (0x51ED270BULL ^ (n + k)) + static_cast<crd::u64>(noise * 1e6);
        auto nextd = [&s]()
        {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            return (static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53)) * 2.0 - 1.0;
        };
        double cscale = 0.0;
        for (usize j = 0; j < k; ++j)
        {
            usize f = 0;
            bool dup = true;
            while (dup)
            {
                s = s * 6364136223846793005ULL + 1ULL;
                f = (s >> 20) % n;
                dup = false;
                for (usize m = 0; m < j; ++m)
                {
                    if (pf[m] == f)
                    {
                        dup = true;
                    }
                }
            }
            pf[j] = f;
            pc[j] = Complex<f64>{nextd(), nextd()};
            cscale = std::max(cscale, std::hypot(pc[j].re, pc[j].im));
        }
        cont::Array<Complex<f64>> x(&alloc);
        x.resize(n);
        for (usize i = 0; i < n; ++i)
        {
            double xr = 0.0;
            double xi = 0.0;
            for (usize j = 0; j < k; ++j)
            {
                const double a = kTwoPi * static_cast<double>((pf[j] * i) % n) / static_cast<double>(n);
                xr += pc[j].re * std::cos(a) - pc[j].im * std::sin(a);
                xi += pc[j].re * std::sin(a) + pc[j].im * std::cos(a);
            }
            x[i] = Complex<f64>{xr / static_cast<double>(n), xi / static_cast<double>(n)};
        }
        if (noise > 0.0) // complex Gaussian noise (Box-Muller), std `noise` per frequency bin
        {
            for (usize i = 0; i < n; ++i)
            {
                s = s * 6364136223846793005ULL + 1ULL;
                const double u1 = static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53);
                s = s * 6364136223846793005ULL + 1ULL;
                const double u2 = static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53);
                const double r = std::sqrt(-2.0 * std::log(u1 + 1e-300)) * noise / std::sqrt(static_cast<double>(n));
                x[i].re += r * std::cos(kTwoPi * u2);
                x[i].im += r * std::sin(kTwoPi * u2);
            }
        }
        const fft::SparseFftPlan<f64> plan(&alloc, n, k, 20);
        cont::Array<usize> rf(&alloc);
        cont::Array<Complex<f64>> rc(&alloc);
        rf.resize(k);
        rc.resize(k);
        const usize got =
            plan.recover(cont::ConstSpan<Complex<f64>>(x.data(), n), cont::Span<usize>(rf.data(), k),
                         cont::Span<Complex<f64>>(rc.data(), k));
        usize matched = 0;
        double maxerr = 0.0;
        for (usize j = 0; j < k; ++j)
        {
            for (usize m = 0; m < got; ++m)
            {
                if (rf[m] == pf[j])
                {
                    matched++;
                    maxerr = std::max(maxerr, std::hypot(rc[m].re - pc[j].re, rc[m].im - pc[j].im));
                    break;
                }
            }
        }
        INFO("n=" << n << " k=" << k << " noise=" << noise << " matched=" << matched << "/" << k
                  << " coeff-relerr=" << (maxerr / cscale));
        CHECK(matched == k);                                       // all frequencies recovered (robust location)
        CHECK(maxerr / cscale < (noise > 0.0 ? 0.12 : 1e-4));      // exact ⇒ filter accuracy; noisy ⇒ √(n/B)σ/√R floor
    }
}
