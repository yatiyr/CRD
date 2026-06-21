// crd-hesap-comms v11c-e — equalizers. Gates: LMS-DD + DFE open a multipath ISI channel (BER→0 after training);
// CMA blindly equalizes (BER→0 modulo the PSK phase ambiguity); MLSE == the brute-force ML sequence; determinism.

#include <crd/hesap/comms/equalizer.hpp>
#include <crd/hesap/comms/modulation.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>

namespace cm = crd::hesap::comms;
namespace cont = crd::containers;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::hesap::Complex;
using C = Complex<f64>;

namespace
{
C cmul(C a, C b) { return C{a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; }

// FIR multipath channel.
cont::Array<C> apply_channel(crd::memory::IAllocator* a, const cont::Array<C>& s, const cont::Array<C>& h)
{
    cont::Array<C> y(a);
    y.resize(s.size());
    for (usize n = 0; n < s.size(); ++n)
    {
        C acc{0, 0};
        for (usize l = 0; l < h.size(); ++l)
        {
            if (n >= l)
            {
                acc.re += h[l].re * s[n - l].re - h[l].im * s[n - l].im;
                acc.im += h[l].re * s[n - l].im + h[l].im * s[n - l].re;
            }
        }
        y[n] = acc;
    }
    return y;
}

// best BER over an integer delay window and the 4 QPSK phase rotations.
usize best_ber(const cont::Array<C>& rec, const cont::Array<u32>& tx, const cm::Modem<f64>& m, usize warmup,
               usize maxdelay)
{
    usize best = rec.size();
    for (u32 rot = 0; rot < 4; ++rot)
    {
        const f64 ang = static_cast<f64>(rot) * (std::numbers::pi_v<f64> / 2.0);
        const C w{std::cos(ang), std::sin(ang)};
        for (usize d = 0; d <= maxdelay; ++d)
        {
            usize err = 0;
            usize cnt = 0;
            for (usize k = warmup; k + d < rec.size() && k < tx.size(); ++k)
            {
                const C r = cmul(rec[k + d], w);
                err += (m.demodulate(r) != tx[k]) ? 1U : 0U;
                ++cnt;
            }
            if (cnt > 100 && err < best)
            {
                best = err;
            }
        }
    }
    return best;
}
} // namespace

TEST_CASE("comms equalizer: LMS-DD opens a multipath channel", "[v11c-e][comms][equalizer]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    cm::Modem<f64> qpsk(&alloc, cm::ModFamily::Psk, 4);
    crd::hesap::stats::PhiloxRng rng(11ULL);
    const usize n = 6000;
    cont::Array<u32> tx(&alloc);
    cont::Array<C> s(&alloc);
    tx.resize(n);
    s.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        tx[i] = static_cast<u32>(rng.next_below(4));
        s[i] = qpsk.modulate(tx[i]);
    }
    cont::Array<C> h(&alloc);
    h.resize(3);
    h[0] = C{1.0, 0.0};
    h[1] = C{0.35, 0.15};
    h[2] = C{-0.2, 0.05};
    const auto rx = apply_channel(&alloc, s, h);

    const usize ntaps = 15;
    const usize delay = ntaps / 2;
    cm::LmsEqualizer<f64> eq(&alloc, ntaps, 0.01);
    cont::Array<C> rec(&alloc);
    rec.resize(n);
    const usize train = 3000;
    for (usize k = 0; k < n; ++k)
    {
        const C y = eq.filter(rx[k]);
        rec[k] = y;
        // desired = the symbol that should appear at the equalizer output now (delay D); train then decision-direct.
        C d;
        if (k < train && k >= delay)
        {
            d = s[k - delay];
        }
        else
        {
            d = qpsk.constellation(qpsk.demodulate(y));
        }
        eq.update(y, d);
    }
    // recovered symbol for tx[k] appears at rec[k+D]; gate steady-state BER.
    const usize errs = best_ber(rec, tx, qpsk, train + 200, ntaps);
    INFO("LMS-DD errors " << errs);
    CHECK(errs == 0);
}

TEST_CASE("comms equalizer: CMA blindly equalizes a multipath channel", "[v11c-e][comms][equalizer]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    cm::Modem<f64> qpsk(&alloc, cm::ModFamily::Psk, 4);
    crd::hesap::stats::PhiloxRng rng(13ULL);
    const usize n = 12000;
    cont::Array<u32> tx(&alloc);
    cont::Array<C> s(&alloc);
    tx.resize(n);
    s.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        tx[i] = static_cast<u32>(rng.next_below(4));
        s[i] = qpsk.modulate(tx[i]);
    }
    cont::Array<C> h(&alloc);
    h.resize(3);
    h[0] = C{1.0, 0.0};
    h[1] = C{0.3, 0.1};
    h[2] = C{0.1, -0.05};
    const auto rx = apply_channel(&alloc, s, h);

    const usize ntaps = 15;
    cm::CmaEqualizer<f64> eq(&alloc, ntaps, 0.003, 1.0); // R2 = 1 for unit-energy QPSK
    cont::Array<C> rec(&alloc);
    rec.resize(n);
    for (usize k = 0; k < n; ++k)
    {
        const C y = eq.filter(rx[k]);
        rec[k] = y;
        eq.update(y);
    }
    const usize errs = best_ber(rec, tx, qpsk, n - 2000, ntaps); // last 2000, after blind convergence
    INFO("CMA errors " << errs);
    CHECK(errs == 0);
}

TEST_CASE("comms equalizer: DFE opens a channel with a deep tap", "[v11c-e][comms][equalizer]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    cm::Modem<f64> qpsk(&alloc, cm::ModFamily::Psk, 4);
    crd::hesap::stats::PhiloxRng rng(17ULL);
    const usize n = 8000;
    cont::Array<u32> tx(&alloc);
    cont::Array<C> s(&alloc);
    tx.resize(n);
    s.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        tx[i] = static_cast<u32>(rng.next_below(4));
        s[i] = qpsk.modulate(tx[i]);
    }
    cont::Array<C> h(&alloc);
    h.resize(2);
    h[0] = C{1.0, 0.0};
    h[1] = C{0.6, 0.0}; // a strong post-cursor (DFE territory)
    const auto rx = apply_channel(&alloc, s, h);

    const usize nff = 9;
    const usize nfb = 3;
    const usize delay = nff / 2;
    cm::DfeEqualizer<f64> eq(&alloc, nff, nfb, 0.01);
    cont::Array<C> rec(&alloc);
    rec.resize(n);
    const usize train = 4000;
    for (usize k = 0; k < n; ++k)
    {
        const C y = eq.filter(rx[k]);
        rec[k] = y;
        C d = (k < train && k >= delay) ? s[k - delay] : qpsk.constellation(qpsk.demodulate(y));
        eq.update(y, d);
    }
    const usize errs = best_ber(rec, tx, qpsk, train + 200, nff);
    INFO("DFE errors " << errs);
    CHECK(errs == 0);
}

TEST_CASE("comms equalizer: MLSE == brute-force ML", "[v11c-e][comms][equalizer]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    cm::Modem<f64> qpsk(&alloc, cm::ModFamily::Psk, 4);
    crd::hesap::stats::PhiloxRng rng(23ULL);
    cont::Array<C> h(&alloc);
    h.resize(2);
    h[0] = C{1.0, 0.0};
    h[1] = C{0.5, 0.2};
    const usize n = 7;
    cont::Array<u32> tx(&alloc);
    cont::Array<C> s(&alloc);
    tx.resize(n);
    s.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        tx[i] = static_cast<u32>(rng.next_below(4));
        s[i] = qpsk.modulate(tx[i]);
    }
    cont::Array<C> r(&alloc); // channel output + small noise
    r.resize(n);
    for (usize k = 0; k < n; ++k)
    {
        C acc{0, 0};
        for (usize l = 0; l < h.size(); ++l)
        {
            if (k >= l)
            {
                acc.re += h[l].re * s[k - l].re - h[l].im * s[k - l].im;
                acc.im += h[l].re * s[k - l].im + h[l].im * s[k - l].re;
            }
        }
        r[k] = C{acc.re + 0.02 * (rng.next_f64() - 0.5), acc.im + 0.02 * (rng.next_f64() - 0.5)};
    }
    const auto mlse = cm::mlse_viterbi<f64>(&alloc, cont::ConstSpan<C>(r.data(), n), cont::ConstSpan<C>(h.data(), 2),
                                            qpsk);
    // brute-force ML: enumerate all 4^n sequences, pick the minimum-distance one (history starts all-zero/sym 0).
    cont::Array<u32> bestseq(&alloc);
    bestseq.resize(n);
    f64 bestm = 1e30;
    cont::Array<u32> seq(&alloc);
    seq.resize(n);
    const usize total = static_cast<usize>(std::pow(4.0, static_cast<f64>(n)));
    for (usize code = 0; code < total; ++code)
    {
        usize c = code;
        for (usize i = 0; i < n; ++i)
        {
            seq[i] = static_cast<u32>(c % 4);
            c /= 4;
        }
        f64 metric = 0.0;
        for (usize k = 0; k < n; ++k)
        {
            C pred{0, 0};
            for (usize l = 0; l < h.size(); ++l)
            {
                if (k >= l)
                {
                    const C sl = qpsk.constellation(seq[k - l]);
                    pred.re += h[l].re * sl.re - h[l].im * sl.im;
                    pred.im += h[l].re * sl.im + h[l].im * sl.re;
                }
                // k < l ⇒ history sym 0 (constellation[0]); add its contribution
                else
                {
                    const C sl = qpsk.constellation(0);
                    pred.re += h[l].re * sl.re - h[l].im * sl.im;
                    pred.im += h[l].re * sl.im + h[l].im * sl.re;
                }
            }
            const f64 dr = r[k].re - pred.re;
            const f64 di = r[k].im - pred.im;
            metric += dr * dr + di * di;
        }
        if (metric < bestm)
        {
            bestm = metric;
            for (usize i = 0; i < n; ++i)
            {
                bestseq[i] = seq[i];
            }
        }
    }
    for (usize k = 0; k < n; ++k)
    {
        INFO("k=" << k << " mlse=" << mlse[k] << " brute=" << bestseq[k]);
        CHECK(mlse[k] == bestseq[k]);
    }
}
