// crd-hesap-comms v11c-g — OFDM. Gates: round trip is identity; through a multipath channel (memory ≤ CP) the
// cyclic prefix makes it circular ⇒ 1-tap zero-forcing EQ (known-H and pilot-estimated) recovers the symbols
// (BER→0); run-twice bit-identical.

#include <crd/hesap/comms/modulation.hpp>
#include <crd/hesap/comms/ofdm.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstring>

namespace cm = crd::hesap::comms;
namespace cont = crd::containers;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::hesap::Complex;
using C = Complex<f64>;
using Catch::Matchers::WithinAbs;

TEST_CASE("comms ofdm: round trip is identity", "[v11c-g][comms][ofdm]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const usize nfft = 64;
    const usize cp = 16;
    cm::OfdmModulator<f64> ofdm(&alloc, nfft, cp);
    cm::Modem<f64> qpsk(&alloc, cm::ModFamily::Psk, 4);
    crd::hesap::stats::PhiloxRng rng(1ULL);
    cont::Array<C> x(&alloc);
    cont::Array<C> tx(&alloc);
    cont::Array<C> y(&alloc);
    x.resize(nfft);
    tx.resize(nfft + cp);
    y.resize(nfft);
    for (usize k = 0; k < nfft; ++k)
    {
        x[k] = qpsk.modulate(static_cast<u32>(rng.next_below(4)));
    }
    ofdm.modulate(cont::ConstSpan<C>(x.data(), nfft), cont::Span<C>(tx.data(), nfft + cp));
    ofdm.demodulate(cont::ConstSpan<C>(tx.data(), nfft + cp), cont::Span<C>(y.data(), nfft));
    for (usize k = 0; k < nfft; ++k)
    {
        INFO("k=" << k);
        CHECK_THAT(y[k].re, WithinAbs(x[k].re, 1e-9));
        CHECK_THAT(y[k].im, WithinAbs(x[k].im, 1e-9));
    }
}

TEST_CASE("comms ofdm: multipath through CP + zero-forcing EQ recovers symbols", "[v11c-g][comms][ofdm]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const usize nfft = 128;
    const usize cp = 16;
    cm::OfdmModulator<f64> ofdm(&alloc, nfft, cp);
    cm::Modem<f64> qpsk(&alloc, cm::ModFamily::Psk, 4);
    crd::hesap::stats::PhiloxRng rng(2ULL);
    cont::Array<u32> tsym(&alloc);
    cont::Array<C> x(&alloc);
    cont::Array<C> tx(&alloc);
    tsym.resize(nfft);
    x.resize(nfft);
    tx.resize(nfft + cp);
    for (usize k = 0; k < nfft; ++k)
    {
        tsym[k] = static_cast<u32>(rng.next_below(4));
        x[k] = qpsk.modulate(tsym[k]);
    }
    ofdm.modulate(cont::ConstSpan<C>(x.data(), nfft), cont::Span<C>(tx.data(), nfft + cp));
    // multipath channel (memory < CP). To make the channel circular over the FFT window, prepend the symbol's own
    // tail (CP already does this) — the received block is the linear conv whose first nfft post-CP samples = circular.
    cont::Array<C> h(&alloc);
    h.resize(4);
    h[0] = C{1.0, 0.0};
    h[1] = C{0.4, 0.2};
    h[2] = C{-0.15, 0.1};
    h[3] = C{0.05, -0.05};
    cont::Array<C> rx(&alloc);
    rx.resize(nfft + cp);
    for (usize n = 0; n < nfft + cp; ++n)
    {
        C acc{0, 0};
        for (usize l = 0; l < h.size(); ++l)
        {
            if (n >= l)
            {
                acc.re += h[l].re * tx[n - l].re - h[l].im * tx[n - l].im;
                acc.im += h[l].re * tx[n - l].im + h[l].im * tx[n - l].re;
            }
        }
        rx[n] = acc;
    }
    cont::Array<C> y(&alloc);
    cont::Array<C> hf(&alloc);
    cont::Array<C> xeq(&alloc);
    y.resize(nfft);
    hf.resize(nfft);
    xeq.resize(nfft);
    ofdm.demodulate(cont::ConstSpan<C>(rx.data(), nfft + cp), cont::Span<C>(y.data(), nfft));
    cm::channel_freq_response<f64>(&alloc, cont::ConstSpan<C>(h.data(), h.size()), nfft, cont::Span<C>(hf.data(), nfft));
    cm::zf_equalize<f64>(cont::ConstSpan<C>(y.data(), nfft), cont::ConstSpan<C>(hf.data(), nfft),
                         cont::Span<C>(xeq.data(), nfft));
    usize errs = 0;
    for (usize k = 0; k < nfft; ++k)
    {
        errs += (qpsk.demodulate(xeq[k]) != tsym[k]) ? 1U : 0U;
    }
    INFO("known-H ZF errors " << errs);
    CHECK(errs == 0);
}

TEST_CASE("comms ofdm: pilot-based channel estimation + EQ", "[v11c-g][comms][ofdm]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const usize nfft = 128;
    const usize cp = 16;
    cm::OfdmModulator<f64> ofdm(&alloc, nfft, cp);
    cm::Modem<f64> qpsk(&alloc, cm::ModFamily::Psk, 4);
    crd::hesap::stats::PhiloxRng rng(4ULL);
    // every 8th subcarrier is a pilot (value 1+0j).
    cont::Array<usize> pidx(&alloc);
    cont::Array<C> pval(&alloc);
    for (usize k = 0; k < nfft; k += 8)
    {
        pidx.push_back(k);
        pval.push_back(C{1.0, 0.0});
    }
    cont::Array<u32> tsym(&alloc);
    cont::Array<C> x(&alloc);
    cont::Array<C> tx(&alloc);
    tsym.resize(nfft);
    x.resize(nfft);
    tx.resize(nfft + cp);
    usize pp = 0;
    for (usize k = 0; k < nfft; ++k)
    {
        if (pp < pidx.size() && pidx[pp] == k)
        {
            x[k] = pval[pp];
            tsym[k] = 0xFFFFFFFFU; // pilot marker (skip in BER)
            ++pp;
        }
        else
        {
            tsym[k] = static_cast<u32>(rng.next_below(4));
            x[k] = qpsk.modulate(tsym[k]);
        }
    }
    ofdm.modulate(cont::ConstSpan<C>(x.data(), nfft), cont::Span<C>(tx.data(), nfft + cp));
    cont::Array<C> h(&alloc);
    h.resize(3);
    h[0] = C{1.0, 0.0};
    h[1] = C{0.3, 0.1};
    h[2] = C{0.1, -0.05};
    cont::Array<C> rx(&alloc);
    rx.resize(nfft + cp);
    for (usize n = 0; n < nfft + cp; ++n)
    {
        C acc{0, 0};
        for (usize l = 0; l < h.size(); ++l)
        {
            if (n >= l)
            {
                acc.re += h[l].re * tx[n - l].re - h[l].im * tx[n - l].im;
                acc.im += h[l].re * tx[n - l].im + h[l].im * tx[n - l].re;
            }
        }
        rx[n] = acc;
    }
    cont::Array<C> y(&alloc);
    cont::Array<C> hf(&alloc);
    cont::Array<C> xeq(&alloc);
    y.resize(nfft);
    hf.resize(nfft);
    xeq.resize(nfft);
    ofdm.demodulate(cont::ConstSpan<C>(rx.data(), nfft + cp), cont::Span<C>(y.data(), nfft));
    cm::estimate_channel_pilots<f64>(cont::ConstSpan<C>(y.data(), nfft), cont::ConstSpan<usize>(pidx.data(), pidx.size()),
                                     cont::ConstSpan<C>(pval.data(), pval.size()), cont::Span<C>(hf.data(), nfft));
    cm::zf_equalize<f64>(cont::ConstSpan<C>(y.data(), nfft), cont::ConstSpan<C>(hf.data(), nfft),
                         cont::Span<C>(xeq.data(), nfft));
    usize errs = 0;
    usize cnt = 0;
    for (usize k = 0; k < nfft; ++k)
    {
        if (tsym[k] == 0xFFFFFFFFU)
        {
            continue; // skip pilots
        }
        errs += (qpsk.demodulate(xeq[k]) != tsym[k]) ? 1U : 0U;
        ++cnt;
    }
    INFO("pilot-EQ errors " << errs << " of " << cnt);
    CHECK(errs == 0);
}

TEST_CASE("comms ofdm: run-twice bit-identical (determinism moat)", "[v11c-g][comms][ofdm][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const usize nfft = 64;
    const usize cp = 16;
    cm::OfdmModulator<f64> ofdm(&alloc, nfft, cp);
    cont::Array<C> x(&alloc);
    cont::Array<C> a(&alloc);
    cont::Array<C> b(&alloc);
    x.resize(nfft);
    a.resize(nfft + cp);
    b.resize(nfft + cp);
    crd::hesap::stats::PhiloxRng rng(6ULL);
    for (usize k = 0; k < nfft; ++k)
    {
        x[k] = C{rng.next_f64() - 0.5, rng.next_f64() - 0.5};
    }
    ofdm.modulate(cont::ConstSpan<C>(x.data(), nfft), cont::Span<C>(a.data(), nfft + cp));
    ofdm.modulate(cont::ConstSpan<C>(x.data(), nfft), cont::Span<C>(b.data(), nfft + cp));
    CHECK(std::memcmp(a.data(), b.data(), (nfft + cp) * sizeof(C)) == 0);
}
