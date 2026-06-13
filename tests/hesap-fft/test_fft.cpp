// v10-a gates: complex FFT substrate. THE correctness gate is the brute-force O(N^2) DFT (computed in f64
// as the truth) — NOT the round-trip IFFT(FFT(x))==x, which can cancel a twiddle-sign/normalization error
// and pass while the forward transform is wrong (the FFT edition of the odeint-d4 trap). Plus: round-trip
// (secondary), Parseval, inverse-vs-naive-IDFT, run-twice determinism (bit-identical), and plan reuse.

#include <crd/containers/array.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/hesap/fft/real_fft.hpp>
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

TEST_CASE("fft: inverse matches the brute-force IDFT (unnormalized)", "[fft]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize n = 256;
    cont::Array<Complex<f64>> x(&alloc);
    fill_lcg(x, n, 12345ULL);
    cont::Array<Complex<f64>> ref(&alloc);
    ref.resize(n);
    naive_dft(cont::ConstSpan<Complex<f64>>(x.data(), n), cont::Span<Complex<f64>>(ref.data(), n), true);

    const fft::FftPlan<f64> plan(&alloc, n);
    plan.execute(cont::Span<Complex<f64>>(x.data(), n), fft::FftDirection::Inverse);

    double maxerr = 0.0;
    for (usize k = 0; k < n; ++k)
    {
        maxerr = std::max(maxerr, std::hypot(x[k].re - ref[k].re, x[k].im - ref[k].im));
    }
    INFO("inverse-vs-naive maxerr=" << maxerr);
    CHECK(maxerr < 1e-10);
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
    crd::memory::TlsfAllocator alloc(1ULL << 30);
    // n >= 2^22 triggers the four-step (six-step) path; the naive O(N^2) DFT is too slow here, so cross-check
    // against execute_reference (the radix-2 oracle, validated against the naive DFT at small n, O(n log n)).
    for (usize n : {1U << 22})
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
        plan.execute(cont::Span<Complex<f64>>(a.data(), n), fft::FftDirection::Forward); // four-step
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
        plan.execute(cont::Span<Complex<f64>>(a.data(), n), fft::FftDirection::Forward);          // radix-8/16
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
                maxerr = std::max(maxerr, std::hypot(work[i * batch + t].re - single[i].re,
                                                     work[i * batch + t].im - single[i].im));
            }
            maxrel = std::max(maxrel, maxerr / (1.0 + maxref));
        }
        INFO("m=" << m << " batched-vs-oracle maxrel=" << maxrel);
        CHECK(maxrel < 1e-12);
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
