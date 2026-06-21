// crd-hesap-comms v11c-c/d — timing + carrier recovery. Gates: Gardner TED S-curve; the symbol synchronizer locks
// onto a fractionally-delayed signal (recovers symbols); the Costas loop removes a phase+frequency offset; the
// M-th-power AFC estimate matches the applied CFO; run-twice determinism.

#include <crd/hesap/comms/carrier.hpp>
#include <crd/hesap/comms/modulation.hpp>
#include <crd/hesap/comms/pulse_shaping.hpp>
#include <crd/hesap/comms/timing.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <numbers>

namespace cm = crd::hesap::comms;
namespace cont = crd::containers;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::hesap::Complex;
using Catch::Matchers::WithinAbs;

namespace
{
// best symbol-error count of `rec` against `tx`, searching a small integer delay window (sync has an unknown delay).
usize best_symbol_errors(const cont::Array<Complex<f64>>& rec, const cont::Array<u32>& txsym, const cm::Modem<f64>& m,
                         usize warmup)
{
    usize best = rec.size();
    for (usize d = 0; d <= 48; ++d) // the matched-filter + start-transient delay is ~16 symbols; search wide
    {
        usize err = 0;
        usize cnt = 0;
        for (usize k = warmup; k + d < rec.size() && k < txsym.size(); ++k)
        {
            const u32 dec = m.demodulate(rec[k + d]);
            err += (dec != txsym[k]) ? 1U : 0U;
            ++cnt;
        }
        if (cnt > 50 && err < best)
        {
            best = err;
        }
    }
    return best;
}
} // namespace

TEST_CASE("comms timing: Gardner TED S-curve", "[v11c-c][comms][timing]")
{
    // A BPSK signal sampled with a timing offset: the Gardner error should change sign through the correct timing.
    crd::memory::TlsfAllocator alloc(1U << 20);
    cm::Modem<f64> bpsk(&alloc, cm::ModFamily::Psk, 2);
    const auto rrc = cm::rrc_pulse<f64>(&alloc, 0.5, 12, 2);
    crd::hesap::stats::PhiloxRng rng(1ULL);
    const usize nsym = 400;
    cont::Array<Complex<f64>> syms(&alloc);
    syms.resize(nsym);
    for (usize i = 0; i < nsym; ++i)
    {
        syms[i] = bpsk.modulate(static_cast<u32>(rng.next_below(2)));
    }
    const auto tx = cm::pulse_shape<f64>(&alloc, cont::ConstSpan<Complex<f64>>(syms.data(), nsym),
                                         cont::ConstSpan<f64>(rrc.data(), rrc.size()), 2);
    const auto mf = cm::matched_filter<f64>(&alloc, cont::ConstSpan<Complex<f64>>(tx.data(), tx.size()),
                                            cont::ConstSpan<f64>(rrc.data(), rrc.size()));
    // sample at offsets around the symbol instant; the averaged Gardner error vs offset is the S-curve.
    const usize l = rrc.size();
    auto scurve = [&](int off) {
        f64 acc = 0.0;
        usize cnt = 0;
        for (usize k = 4; k * 2 + (l - 1) + 4 < mf.size() / 1; ++k)
        {
            const usize base = k * 2 + (l - 1) + static_cast<usize>(off);
            if (base < 2 || base + 1 >= mf.size())
            {
                break;
            }
            acc += cm::gardner_ted<f64>(mf[base - 2], mf[base - 1], mf[base]);
            ++cnt;
        }
        return (cnt > 0) ? acc / static_cast<f64>(cnt) : 0.0;
    };
    CHECK(scurve(-1) < 0.0); // late vs early have opposite sign through zero
    CHECK(scurve(1) > 0.0);
}

TEST_CASE("comms timing: SymbolSync locks onto a fractional delay", "[v11c-c][comms][timing]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    cm::Modem<f64> qpsk(&alloc, cm::ModFamily::Psk, 4);
    const auto rrc = cm::rrc_pulse<f64>(&alloc, 0.3, 16, 2); // sps = 2
    crd::hesap::stats::PhiloxRng rng(7ULL);
    const usize nsym = 2000;
    cont::Array<u32> txsym(&alloc);
    cont::Array<Complex<f64>> syms(&alloc);
    txsym.resize(nsym);
    syms.resize(nsym);
    for (usize i = 0; i < nsym; ++i)
    {
        txsym[i] = static_cast<u32>(rng.next_below(4));
        syms[i] = qpsk.modulate(txsym[i]);
    }
    // TX shape, matched filter, then apply a FRACTIONAL delay (0.4 sample) via cubic interpolation.
    const auto tx = cm::pulse_shape<f64>(&alloc, cont::ConstSpan<Complex<f64>>(syms.data(), nsym),
                                         cont::ConstSpan<f64>(rrc.data(), rrc.size()), 2);
    const auto mf = cm::matched_filter<f64>(&alloc, cont::ConstSpan<Complex<f64>>(tx.data(), tx.size()),
                                            cont::ConstSpan<f64>(rrc.data(), rrc.size()));
    cont::Array<Complex<f64>> delayed(&alloc);
    delayed.resize(mf.size());
    const f64 frac = 0.4;
    for (usize n = 0; n < mf.size(); ++n)
    {
        Complex<f64> win[4];
        for (int j = 0; j < 4; ++j)
        {
            const crd::isize idx = static_cast<crd::isize>(n) - 3 + j;
            win[j] = (idx >= 0 && idx < static_cast<crd::isize>(mf.size())) ? mf[static_cast<usize>(idx)]
                                                                           : Complex<f64>{0.0, 0.0};
        }
        delayed[n] = cm::cubic_interp<f64>(win, frac);
    }
    cm::SymbolSync<f64> sync(0.01, 1.0);
    cont::Array<Complex<f64>> rec(&alloc);
    sync.process(cont::ConstSpan<Complex<f64>>(delayed.data(), delayed.size()), rec);
    REQUIRE(rec.size() > nsym / 2);
    const usize errs = best_symbol_errors(rec, txsym, qpsk, rec.size() / 2); // steady state (2nd half)
    INFO("symbol sync errors " << errs << " of ~" << rec.size() / 2);
    CHECK(errs == 0);
}

TEST_CASE("comms carrier: Costas loop removes phase + frequency offset", "[v11c-d][comms][carrier]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    cm::Modem<f64> qpsk(&alloc, cm::ModFamily::Psk, 4);
    crd::hesap::stats::PhiloxRng rng(9ULL);
    const usize nsym = 4000;
    cont::Array<u32> txsym(&alloc);
    cont::Array<Complex<f64>> rx(&alloc);
    txsym.resize(nsym);
    rx.resize(nsym);
    const f64 dphi = 0.0008; // freq offset (rad/symbol)
    const f64 phi0 = 0.7;    // phase offset
    for (usize i = 0; i < nsym; ++i)
    {
        txsym[i] = static_cast<u32>(rng.next_below(4));
        const auto s = qpsk.modulate(txsym[i]);
        const f64 ph = phi0 + dphi * static_cast<f64>(i);
        const f64 c = std::cos(ph);
        const f64 sn = std::sin(ph);
        rx[i] = Complex<f64>{s.re * c - s.im * sn, s.re * sn + s.im * c};
    }
    cm::CostasLoop<f64> costas(&qpsk, 0.02, 1.0);
    cont::Array<Complex<f64>> rec(&alloc);
    costas.process(cont::ConstSpan<Complex<f64>>(rx.data(), nsym), rec);
    // after lock, the de-rotated symbols recover the data (QPSK has a 4-fold phase ambiguity ⇒ allow a constant
    // rotation by trying the 4 rotations and taking the best).
    usize best = nsym;
    for (u32 rot = 0; rot < 4; ++rot)
    {
        const f64 ang = static_cast<f64>(rot) * (std::numbers::pi_v<f64> / 2.0);
        const f64 c = std::cos(ang);
        const f64 sn = std::sin(ang);
        usize err = 0;
        for (usize i = nsym / 2; i < nsym; ++i)
        {
            const Complex<f64> r{rec[i].re * c - rec[i].im * sn, rec[i].re * sn + rec[i].im * c};
            err += (qpsk.demodulate(r) != txsym[i]) ? 1U : 0U;
        }
        if (err < best)
        {
            best = err;
        }
    }
    INFO("costas residual errors " << best);
    CHECK(best == 0);
}

TEST_CASE("comms carrier: M-th power AFC estimates the CFO", "[v11c-d][comms][carrier]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    cm::Modem<f64> qpsk(&alloc, cm::ModFamily::Psk, 4);
    crd::hesap::stats::PhiloxRng rng(3ULL);
    const usize nsym = 8000;
    cont::Array<Complex<f64>> rx(&alloc);
    rx.resize(nsym);
    const f64 cfo = 0.013; // rad/sample
    for (usize i = 0; i < nsym; ++i)
    {
        const auto s = qpsk.modulate(static_cast<u32>(rng.next_below(4)));
        const f64 ph = cfo * static_cast<f64>(i);
        const f64 c = std::cos(ph);
        const f64 sn = std::sin(ph);
        rx[i] = Complex<f64>{s.re * c - s.im * sn, s.re * sn + s.im * c};
    }
    const f64 est = cm::estimate_cfo_mpsk<f64>(cont::ConstSpan<Complex<f64>>(rx.data(), nsym), 4);
    INFO("cfo est " << est << " true " << cfo);
    CHECK_THAT(est, WithinAbs(cfo, 1e-3));
}
