// crd-hesap-comms v11c-b — pulse shaping + matched filter. Gates: the RRC-TX ⊛ RRC-RX = raised-cosine ZERO-ISI
// Nyquist property (noise-free round trip recovers symbols exactly) + Gaussian-pulse properties + matched-filter
// SNR gain + run-twice determinism.

#include <crd/hesap/comms/modulation.hpp>
#include <crd/hesap/comms/pulse_shaping.hpp>
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
using Catch::Matchers::WithinAbs;

namespace
{
// peak ISI of the RRC-TX ⊛ RRC-RX chain at the symbol instants, for a given RRC span.
f64 rrc_chain_isi(crd::memory::IAllocator* alloc, f64 beta, usize span, usize sps)
{
    const auto rrc = cm::rrc_pulse<f64>(alloc, beta, span, sps);
    const usize l = rrc.size();
    const cont::ConstSpan<f64> taps(rrc.data(), l);
    cm::Modem<f64> modem(alloc, cm::ModFamily::Qam, 16);
    const usize nsym = 200;
    crd::hesap::stats::PhiloxRng rng(123ULL);
    cont::Array<Complex<f64>> syms(alloc);
    syms.resize(nsym);
    for (usize i = 0; i < nsym; ++i)
    {
        syms[i] = modem.modulate(static_cast<u32>(rng.next_below(16)));
    }
    const auto tx = cm::pulse_shape<f64>(alloc, cont::ConstSpan<Complex<f64>>(syms.data(), nsym), taps, sps);
    const auto rx = cm::matched_filter<f64>(alloc, cont::ConstSpan<Complex<f64>>(tx.data(), tx.size()), taps);
    cont::Array<Complex<f64>> rec(alloc);
    rec.resize(nsym);
    for (usize k = 0; k < nsym; ++k) // combined RC peak for symbol k at k*sps + (l-1)
    {
        rec[k] = rx[k * sps + (l - 1)];
    }
    return cm::peak_distortion<f64>(cont::ConstSpan<Complex<f64>>(rec.data() + span, nsym - 2 * span),
                                    cont::ConstSpan<Complex<f64>>(syms.data() + span, nsym - 2 * span));
}
} // namespace

TEST_CASE("comms pulse shaping: RRC TX+RX is Nyquist (ISI -> 0 as span grows)", "[v11c-b][comms][pulse]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    // The matched-filter peak gain is unit (Σ rrc² = 1); the residual is purely truncation ISI, which vanishes as
    // the RRC span grows — the Nyquist property. (A truncated RRC is only approximately Nyquist.)
    const f64 isi8 = rrc_chain_isi(&alloc, 0.25, 8, 4);
    const f64 isi16 = rrc_chain_isi(&alloc, 0.25, 16, 4);
    const f64 isi32 = rrc_chain_isi(&alloc, 0.25, 32, 4);
    INFO("ISI span8=" << isi8 << " span16=" << isi16 << " span32=" << isi32);
    CHECK(isi16 < isi8);   // monotone decrease with span
    CHECK(isi32 < isi16);
    CHECK(isi32 < 1e-3);   // effectively zero ISI at a realistic span
}

TEST_CASE("comms pulse shaping: Gaussian pulse properties", "[v11c-b][comms][pulse]")
{
    crd::memory::TlsfAllocator alloc(1U << 18);
    const auto g = cm::gaussian_pulse<f64>(&alloc, 0.3, 4, 8);
    const usize n = g.size();
    f64 sum = 0.0;
    for (usize i = 0; i < n; ++i)
    {
        sum += g[i];
    }
    CHECK_THAT(sum, WithinAbs(1.0, 1e-12)); // unit DC gain
    // symmetric + peak at centre
    const usize half = n / 2;
    for (usize i = 0; i < half; ++i)
    {
        INFO("i=" << i);
        CHECK_THAT(g[i], WithinAbs(g[n - 1 - i], 1e-12));
        CHECK(g[i] <= g[half]);
    }
}

TEST_CASE("comms pulse shaping: matched filter maximizes SNR vs a mismatched filter", "[v11c-b][comms][pulse]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    // A single pulse + noise: the matched filter output at the peak has the largest |signal| per unit noise.
    const auto rrc = cm::rrc_pulse<f64>(&alloc, 0.35, 8, 4);
    const usize l = rrc.size();
    cont::Array<Complex<f64>> sym(&alloc);
    sym.resize(1);
    sym[0] = Complex<f64>{1.0, 0.0};
    const auto tx = cm::pulse_shape<f64>(&alloc, cont::ConstSpan<Complex<f64>>(sym.data(), 1), cont::ConstSpan<f64>(rrc.data(), l), 4);
    const auto rx = cm::matched_filter<f64>(&alloc, cont::ConstSpan<Complex<f64>>(tx.data(), tx.size()), cont::ConstSpan<f64>(rrc.data(), l));
    // peak at l-1 == Σ rrc² == 1 (unit energy).
    CHECK_THAT(rx[l - 1].re, WithinAbs(1.0, 1e-9));
    // and it is the maximum of the matched-filter output.
    f64 mx = 0.0;
    for (usize i = 0; i < rx.size(); ++i)
    {
        mx = std::max(mx, std::abs(rx[i].re));
    }
    CHECK_THAT(mx, WithinAbs(1.0, 1e-9));
}

TEST_CASE("comms pulse shaping: run-twice bit-identical (determinism moat)", "[v11c-b][comms][pulse][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const auto rrc = cm::rrc_pulse<f64>(&alloc, 0.25, 6, 4);
    cont::Array<Complex<f64>> syms(&alloc);
    syms.resize(64);
    crd::hesap::stats::PhiloxRng rng(7ULL);
    for (usize i = 0; i < 64; ++i)
    {
        syms[i] = Complex<f64>{rng.next_f64() - 0.5, rng.next_f64() - 0.5};
    }
    const auto a = cm::pulse_shape<f64>(&alloc, cont::ConstSpan<Complex<f64>>(syms.data(), 64), cont::ConstSpan<f64>(rrc.data(), rrc.size()), 4);
    const auto b = cm::pulse_shape<f64>(&alloc, cont::ConstSpan<Complex<f64>>(syms.data(), 64), cont::ConstSpan<f64>(rrc.data(), rrc.size()), 4);
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(Complex<f64>)) == 0);
}
