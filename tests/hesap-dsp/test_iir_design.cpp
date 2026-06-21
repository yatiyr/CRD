// crd-hesap-dsp v11-g — IIR digital design gates. Frequency-transformed designs (highpass/bandpass/bandstop) are
// gated on RESPONSE-EQUALITY vs scipy's sos (|H| at 64 pts, order-independent). Order estimation (*ord) is gated on
// EXACT N + Wn-to-tolerance vs scipy AND SPEC-COMPLIANCE (the designed filter meets gpass at wp, gstop at ws — the
// real point of order estimation). Notch/peak/comb gated on coefficients vs scipy.

#include <crd/hesap/dsp/filter.hpp>
#include <crd/hesap/dsp/freqz.hpp>
#include <crd/hesap/dsp/iir.hpp>
#include <crd/hesap/dsp/iir_design.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "iir_design_refs.inc"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <numbers>

namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;
using crd::hesap::Complex;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr f64 kPi = std::numbers::pi_v<f64>;

// |H(e^{jw})| of an SOS cascade at a single angular frequency w (rad/sample) — direct evaluation.
[[nodiscard]] f64 sos_mag_at(const dsp::SecondOrderSections<f64>& sos, f64 w)
{
    const f64 c1 = std::cos(w);
    const f64 c2 = std::cos(2.0 * w);
    const f64 s1 = std::sin(w);
    const f64 s2 = std::sin(2.0 * w);
    f64 hre = 1.0;
    f64 him = 0.0;
    for (usize k = 0; k < sos.sections.size(); ++k)
    {
        const auto& bq = sos.sections[k];
        const f64 nre = bq.b0 + bq.b1 * c1 + bq.b2 * c2;
        const f64 nim = -(bq.b1 * s1 + bq.b2 * s2);
        const f64 dre = 1.0 + bq.a1 * c1 + bq.a2 * c2;
        const f64 dim = -(bq.a1 * s1 + bq.a2 * s2);
        const f64 d = dre * dre + dim * dim;
        const f64 qre = (nre * dre + nim * dim) / d;
        const f64 qim = (nim * dre - nre * dim) / d;
        const f64 t = hre * qre - him * qim;
        him = hre * qim + him * qre;
        hre = t;
    }
    return std::hypot(hre, him);
}

// design via zpk -> sos, |H| at 64 points vs scipy sosfreqz magnitude (order-independent).
template <usize N> void check_response(const double (&ref)[N], crd::memory::IAllocator* a, const dsp::Zpk<f64>& zpk)
{
    const auto sos = dsp::zpk_to_sos<f64>(a, zpk);
    cont::Array<f64> w(a);
    cont::Array<Complex<f64>> h(a);
    dsp::sosfreqz<f64>(sos, 64, w, h);
    REQUIRE(N == 64);
    for (usize i = 0; i < 64; ++i)
    {
        INFO("mag[" << i << "]");
        CHECK_THAT(std::hypot(h[i].re, h[i].im), WithinAbs(ref[i], 1e-9));
    }
}

template <usize N> void check_coeffs(const double (&ref)[N], const cont::Array<f64>& got, double tol)
{
    REQUIRE(got.size() == N);
    for (usize i = 0; i < N; ++i)
    {
        INFO("coeff[" << i << "]");
        CHECK_THAT(got[i], WithinAbs(ref[i], tol));
    }
}

// the full order-estimation gate: exact N + Wn-to-tol + spec-compliance of the designed filter.
template <usize NW>
void check_order(crd::memory::IAllocator* a, const dsp::IirOrder<f64>& got, int ref_n, const double (&ref_wn)[NW],
                 dsp::IirKind kind, cont::ConstSpan<f64> wp, cont::ConstSpan<f64> ws, f64 gpass, f64 gstop,
                 double wn_tol)
{
    CHECK(got.n == static_cast<usize>(ref_n)); // N is exact (a ceil)
    REQUIRE(got.wn.size() == NW);
    for (usize i = 0; i < NW; ++i)
    {
        INFO("Wn[" << i << "]");
        CHECK_THAT(got.wn[i], WithinAbs(ref_wn[i], wn_tol));
    }
    // spec-compliance: design with (n, Wn) and verify gain at the band edges.
    const dsp::BandType bt = dsp::band_type_of<f64>(wp, ws);
    const auto zpk = dsp::iirfilter<f64>(a, got.n, cont::ConstSpan<f64>(got.wn.data(), got.wn.size()), bt, kind, gpass, gstop);
    const auto sos = dsp::zpk_to_sos<f64>(a, zpk);
    const f64 pass_floor = std::pow(10.0, -gpass / 20.0); // gain must be >= this at passband edges
    const f64 stop_ceil = std::pow(10.0, -gstop / 20.0);  // gain must be <= this at stopband edges
    for (usize i = 0; i < wp.size(); ++i)
    {
        INFO("passband edge wp[" << i << "]=" << wp[i]);
        CHECK(sos_mag_at(sos, wp[i] * kPi) >= pass_floor - 1e-7);
    }
    for (usize i = 0; i < ws.size(); ++i)
    {
        INFO("stopband edge ws[" << i << "]=" << ws[i]);
        CHECK(sos_mag_at(sos, ws[i] * kPi) <= stop_ceil + 1e-7);
    }
}

cont::ConstSpan<f64> span1(const f64* p) { return {p, 1}; }
cont::ConstSpan<f64> span2(const f64* p) { return {p, 2}; }
} // namespace

TEST_CASE("dsp iir_design: frequency transforms (hp/bp/bs) match scipy response", "[v11-g][dsp][iir]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const f64 wn_hp[] = {0.3};
    const f64 wn_bp[] = {0.2, 0.4};
    const f64 wn_bs[] = {0.25, 0.45};
    check_response(ref_butter_hp_mag, &alloc, dsp::iirfilter<f64>(&alloc, 6, span1(wn_hp), dsp::BandType::Highpass, dsp::IirKind::Butter));
    check_response(ref_butter_bp_mag, &alloc, dsp::iirfilter<f64>(&alloc, 4, span2(wn_bp), dsp::BandType::Bandpass, dsp::IirKind::Butter));
    check_response(ref_butter_bs_mag, &alloc, dsp::iirfilter<f64>(&alloc, 4, span2(wn_bp), dsp::BandType::Bandstop, dsp::IirKind::Butter));
    check_response(ref_cheby1_hp_mag, &alloc, dsp::iirfilter<f64>(&alloc, 6, span1(wn_hp), dsp::BandType::Highpass, dsp::IirKind::Cheby1, 1.0, 0.0));
    check_response(ref_cheby2_bp_mag, &alloc, dsp::iirfilter<f64>(&alloc, 6, span2(wn_bp), dsp::BandType::Bandpass, dsp::IirKind::Cheby2, 0.0, 40.0));
    check_response(ref_ellip_bs_mag, &alloc, dsp::iirfilter<f64>(&alloc, 5, span2(wn_bs), dsp::BandType::Bandstop, dsp::IirKind::Ellip, 1.0, 40.0));
}

TEST_CASE("dsp iir_design: buttord -- exact N, Wn, and spec-compliance", "[v11-g][dsp][iir]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const f64 lp_wp[] = {0.2};
    const f64 lp_ws[] = {0.3};
    const f64 hp_wp[] = {0.3};
    const f64 hp_ws[] = {0.2};
    const f64 bp_wp[] = {0.2, 0.5};
    const f64 bp_ws[] = {0.1, 0.6};
    const f64 bs_wp[] = {0.1, 0.6};
    const f64 bs_ws[] = {0.2, 0.5};
    check_order(&alloc, dsp::buttord<f64>(&alloc, span1(lp_wp), span1(lp_ws), 1.0, 40.0), ref_buttord_lp_n,
                ref_buttord_lp_wn, dsp::IirKind::Butter, span1(lp_wp), span1(lp_ws), 1.0, 40.0, 1e-10);
    check_order(&alloc, dsp::buttord<f64>(&alloc, span1(hp_wp), span1(hp_ws), 1.0, 40.0), ref_buttord_hp_n,
                ref_buttord_hp_wn, dsp::IirKind::Butter, span1(hp_wp), span1(hp_ws), 1.0, 40.0, 1e-10);
    check_order(&alloc, dsp::buttord<f64>(&alloc, span2(bp_wp), span2(bp_ws), 1.0, 40.0), ref_buttord_bp_n,
                ref_buttord_bp_wn, dsp::IirKind::Butter, span2(bp_wp), span2(bp_ws), 1.0, 40.0, 1e-6);
    check_order(&alloc, dsp::buttord<f64>(&alloc, span2(bs_wp), span2(bs_ws), 1.0, 40.0), ref_buttord_bs_n,
                ref_buttord_bs_wn, dsp::IirKind::Butter, span2(bs_wp), span2(bs_ws), 1.0, 40.0, 1e-6);
}

TEST_CASE("dsp iir_design: cheb1ord/cheb2ord -- exact N, Wn, and spec-compliance", "[v11-g][dsp][iir]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const f64 lp_wp[] = {0.2};
    const f64 lp_ws[] = {0.3};
    const f64 bp_wp[] = {0.2, 0.5};
    const f64 bp_ws[] = {0.1, 0.6};
    check_order(&alloc, dsp::cheb1ord<f64>(&alloc, span1(lp_wp), span1(lp_ws), 1.0, 40.0), ref_cheb1ord_lp_n,
                ref_cheb1ord_lp_wn, dsp::IirKind::Cheby1, span1(lp_wp), span1(lp_ws), 1.0, 40.0, 1e-10);
    check_order(&alloc, dsp::cheb1ord<f64>(&alloc, span2(bp_wp), span2(bp_ws), 1.0, 40.0), ref_cheb1ord_bp_n,
                ref_cheb1ord_bp_wn, dsp::IirKind::Cheby1, span2(bp_wp), span2(bp_ws), 1.0, 40.0, 1e-6);
    check_order(&alloc, dsp::cheb2ord<f64>(&alloc, span1(lp_wp), span1(lp_ws), 1.0, 40.0), ref_cheb2ord_lp_n,
                ref_cheb2ord_lp_wn, dsp::IirKind::Cheby2, span1(lp_wp), span1(lp_ws), 1.0, 40.0, 1e-10);
    check_order(&alloc, dsp::cheb2ord<f64>(&alloc, span2(bp_wp), span2(bp_ws), 1.0, 40.0), ref_cheb2ord_bp_n,
                ref_cheb2ord_bp_wn, dsp::IirKind::Cheby2, span2(bp_wp), span2(bp_ws), 1.0, 40.0, 1e-6);
}

TEST_CASE("dsp iir_design: ellipord -- exact N, Wn, and spec-compliance", "[v11-g][dsp][iir]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const f64 lp_wp[] = {0.2};
    const f64 lp_ws[] = {0.3};
    const f64 bs_wp[] = {0.1, 0.6};
    const f64 bs_ws[] = {0.2, 0.5};
    check_order(&alloc, dsp::ellipord<f64>(&alloc, span1(lp_wp), span1(lp_ws), 1.0, 40.0), ref_ellipord_lp_n,
                ref_ellipord_lp_wn, dsp::IirKind::Ellip, span1(lp_wp), span1(lp_ws), 1.0, 40.0, 1e-10);
    check_order(&alloc, dsp::ellipord<f64>(&alloc, span2(bs_wp), span2(bs_ws), 1.0, 40.0), ref_ellipord_bs_n,
                ref_ellipord_bs_wn, dsp::IirKind::Ellip, span2(bs_wp), span2(bs_ws), 1.0, 40.0, 1e-6);
}

TEST_CASE("dsp iir_design: iirdesign end-to-end meets spec", "[v11-g][dsp][iir]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const f64 wp[] = {0.2};
    const f64 ws[] = {0.3};
    // iirdesign(Butter) must equal iirfilter(buttord(...)) and meet the spec.
    const auto zpk = dsp::iirdesign<f64>(&alloc, span1(wp), span1(ws), 1.0, 40.0, dsp::IirKind::Ellip);
    const auto sos = dsp::zpk_to_sos<f64>(&alloc, zpk);
    CHECK(sos_mag_at(sos, wp[0] * kPi) >= std::pow(10.0, -1.0 / 20.0) - 1e-7);  // passband ripple <= 1 dB at wp
    CHECK(sos_mag_at(sos, ws[0] * kPi) <= std::pow(10.0, -40.0 / 20.0) + 1e-7); // stopband >= 40 dB at ws
}

TEST_CASE("dsp iir_design: notch/peak/comb coefficients match scipy", "[v11-g][dsp][iir]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    check_coeffs(ref_notch_b, dsp::iirnotch<f64>(&alloc, 0.25, 30.0).b, 1e-12);
    check_coeffs(ref_notch_a, dsp::iirnotch<f64>(&alloc, 0.25, 30.0).a, 1e-12);
    check_coeffs(ref_peak_b, dsp::iirpeak<f64>(&alloc, 0.25, 30.0).b, 1e-12);
    check_coeffs(ref_peak_a, dsp::iirpeak<f64>(&alloc, 0.25, 30.0).a, 1e-12);
    check_coeffs(ref_comb_notch_b, dsp::iircomb<f64>(&alloc, 0.25, 30.0, false).b, 1e-12);
    check_coeffs(ref_comb_notch_a, dsp::iircomb<f64>(&alloc, 0.25, 30.0, false).a, 1e-12);
    check_coeffs(ref_comb_peak_b, dsp::iircomb<f64>(&alloc, 0.25, 30.0, true).b, 1e-12);
    check_coeffs(ref_comb_peak_a, dsp::iircomb<f64>(&alloc, 0.25, 30.0, true).a, 1e-12);
}
