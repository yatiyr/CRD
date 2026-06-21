// crd-hesap-comms v11c-f — channels + AGC + framing + FEC. Gates: AWGN measured SNR matches; Rayleigh/Rician
// average power = 1 + Rician K-factor; AGC converges to the target power; preamble correlation peaks at the true
// offset; Hamming(7,4) corrects any single-bit error; reproducible (run-twice).

#include <crd/hesap/comms/channel.hpp>
#include <crd/hesap/comms/framing.hpp>
#include <crd/hesap/comms/modulation.hpp>
#include <crd/hesap/stats/normal.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

namespace cm = crd::hesap::comms;
namespace cont = crd::containers;
using crd::f64;
using crd::u32;
using crd::u8;
using crd::usize;
using crd::hesap::Complex;
using C = Complex<f64>;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("comms channel: AWGN measured SNR matches the target", "[v11c-f][comms][channel]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const usize n = 200000;
    cont::Array<C> x(&alloc);
    x.resize(n);
    crd::hesap::stats::PhiloxRng rng(1ULL);
    cm::Modem<f64> qpsk(&alloc, cm::ModFamily::Psk, 4);
    for (usize i = 0; i < n; ++i)
    {
        x[i] = qpsk.modulate(static_cast<u32>(rng.next_below(4)));
    }
    const f64 sp = cm::signal_power<f64>(cont::ConstSpan<C>(x.data(), n));
    CHECK_THAT(sp, WithinAbs(1.0, 1e-9)); // unit-energy QPSK
    const f64 snr_db = 10.0;
    const f64 sigma = cm::noise_sigma_for_snr<f64>(sp, snr_db);
    cont::Array<C> noisy(&alloc);
    noisy.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        noisy[i] = x[i];
    }
    crd::hesap::stats::PhiloxRng nrng(99ULL);
    crd::hesap::stats::NormalSampler ns(nrng);
    cm::awgn_add<f64>(cont::Span<C>(noisy.data(), n), sigma, ns);
    f64 np = 0.0;
    for (usize i = 0; i < n; ++i)
    {
        const f64 dr = noisy[i].re - x[i].re;
        const f64 di = noisy[i].im - x[i].im;
        np += dr * dr + di * di;
    }
    np /= static_cast<f64>(n);
    const f64 meas_snr_db = 10.0 * std::log10(sp / np);
    INFO("measured SNR " << meas_snr_db);
    CHECK_THAT(meas_snr_db, WithinAbs(snr_db, 0.1));
}

TEST_CASE("comms channel: Rayleigh + Rician fading statistics", "[v11c-f][comms][channel]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const usize n = 200000;
    cont::Array<C> x(&alloc);
    x.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        x[i] = C{1.0, 0.0};
    }
    crd::hesap::stats::PhiloxRng rng(7ULL);
    crd::hesap::stats::NormalSampler g(rng);
    cm::rayleigh_flat<f64>(cont::Span<C>(x.data(), n), g);
    f64 p = cm::signal_power<f64>(cont::ConstSpan<C>(x.data(), n));
    INFO("Rayleigh mean power " << p);
    CHECK_THAT(p, WithinRel(1.0, 0.02)); // E[|g|²] = 1

    cont::Array<C> y(&alloc);
    y.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        y[i] = C{1.0, 0.0};
    }
    const f64 k = 4.0;
    cm::rician_flat<f64>(cont::Span<C>(y.data(), n), k, g);
    p = cm::signal_power<f64>(cont::ConstSpan<C>(y.data(), n));
    CHECK_THAT(p, WithinRel(1.0, 0.02)); // unit average power
    // mean of the real part ≈ the LOS component √(K/(K+1)).
    f64 mr = 0.0;
    for (usize i = 0; i < n; ++i)
    {
        mr += y[i].re;
    }
    mr /= static_cast<f64>(n);
    CHECK_THAT(mr, WithinAbs(std::sqrt(k / (k + 1.0)), 0.02));
}

TEST_CASE("comms channel: AGC converges to the target power", "[v11c-f][comms][channel]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const usize n = 20000;
    cm::Agc<f64> agc(1.0, 0.01); // target power 1
    crd::hesap::stats::PhiloxRng rng(3ULL);
    f64 pout = 0.0;
    usize cnt = 0;
    for (usize i = 0; i < n; ++i)
    {
        // input with a large constant gain (0.1 amplitude ⇒ power 0.01) — AGC should boost ~100x.
        const C in{0.1 * (rng.next_f64() * 2.0 - 1.0), 0.1 * (rng.next_f64() * 2.0 - 1.0)};
        const C y = agc.process(in);
        if (i > n - 4000) // steady state
        {
            pout += y.re * y.re + y.im * y.im;
            ++cnt;
        }
    }
    pout /= static_cast<f64>(cnt);
    INFO("AGC steady power " << pout);
    CHECK_THAT(pout, WithinRel(1.0, 0.25)); // within 25% of the target (1-pole AGC ripple)
}

TEST_CASE("comms framing: preamble correlation peaks at the true offset", "[v11c-f][comms][framing]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    cm::Modem<f64> bpsk(&alloc, cm::ModFamily::Psk, 2);
    crd::hesap::stats::PhiloxRng rng(5ULL);
    const usize plen = 31;
    cont::Array<C> pre(&alloc);
    pre.resize(plen);
    for (usize i = 0; i < plen; ++i)
    {
        pre[i] = bpsk.modulate(static_cast<u32>(rng.next_below(2)));
    }
    const usize n = 500;
    const usize off_true = 137;
    cont::Array<C> rx(&alloc);
    rx.resize(n);
    crd::hesap::stats::PhiloxRng nrng(8ULL);
    crd::hesap::stats::NormalSampler ns(nrng);
    for (usize i = 0; i < n; ++i)
    {
        rx[i] = C{0.3 * ns.next(), 0.3 * ns.next()}; // noise floor
    }
    for (usize i = 0; i < plen; ++i)
    {
        rx[off_true + i].re += pre[i].re;
        rx[off_true + i].im += pre[i].im;
    }
    f64 metric = 0.0;
    const usize off = cm::find_preamble<f64>(cont::ConstSpan<C>(rx.data(), n), cont::ConstSpan<C>(pre.data(), plen), metric);
    INFO("detected offset " << off << " metric " << metric);
    CHECK(off == off_true);
    CHECK(metric > 0.5);
}

TEST_CASE("comms FEC: Hamming(7,4) corrects any single-bit error", "[v11c-f][comms][fec]")
{
    for (u32 data = 0; data < 16; ++data)
    {
        u8 d4[4] = {static_cast<u8>(data & 1U), static_cast<u8>((data >> 1) & 1U), static_cast<u8>((data >> 2) & 1U),
                    static_cast<u8>((data >> 3) & 1U)};
        u8 c7[7];
        cm::hamming74_encode(d4, c7);
        for (int flip = 0; flip < 7; ++flip)
        {
            u8 r[7];
            for (int i = 0; i < 7; ++i)
            {
                r[i] = c7[i];
            }
            r[flip] ^= 1U;
            u8 dec[4];
            const int corr = cm::hamming74_decode(r, dec);
            INFO("data " << data << " flip " << flip << " corr " << corr);
            CHECK(corr == flip);
            for (int i = 0; i < 4; ++i)
            {
                CHECK(dec[i] == d4[i]);
            }
        }
        // no error ⇒ no correction
        u8 dec0[4];
        CHECK(cm::hamming74_decode(c7, dec0) == -1);
    }
}
