// crd-hesap-dsp v11-h — RBJ audio EQ biquads. Gated on the closed-form coefficients (vs an independent cookbook
// transcription, ~1e-12) AND the spec properties (DC / Nyquist / centre-frequency gains — the real design intent).

#include <crd/hesap/dsp/filter.hpp>
#include <crd/hesap/dsp/rbj.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "rbj_refs.inc"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <numbers>

namespace dsp = crd::hesap::dsp;
using crd::f64;
using crd::usize;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr f64 kPi = std::numbers::pi_v<f64>;
constexpr f64 kQ = 0.7071067811865476;

// |H(e^{jw})| of a single biquad at angular frequency w (rad/sample).
[[nodiscard]] f64 biquad_mag(const dsp::Biquad<f64>& bq, f64 w)
{
    const f64 c1 = std::cos(w), c2 = std::cos(2.0 * w), s1 = std::sin(w), s2 = std::sin(2.0 * w);
    const f64 nre = bq.b0 + bq.b1 * c1 + bq.b2 * c2, nim = -(bq.b1 * s1 + bq.b2 * s2);
    const f64 dre = 1.0 + bq.a1 * c1 + bq.a2 * c2, dim = -(bq.a1 * s1 + bq.a2 * s2);
    return std::sqrt((nre * nre + nim * nim) / (dre * dre + dim * dim));
}

void check_coeffs(const double (&ref)[5], const dsp::Biquad<f64>& bq)
{
    CHECK_THAT(bq.b0, WithinAbs(ref[0], 1e-12));
    CHECK_THAT(bq.b1, WithinAbs(ref[1], 1e-12));
    CHECK_THAT(bq.b2, WithinAbs(ref[2], 1e-12));
    CHECK_THAT(bq.a1, WithinAbs(ref[3], 1e-12));
    CHECK_THAT(bq.a2, WithinAbs(ref[4], 1e-12));
}
} // namespace

TEST_CASE("dsp rbj: coefficients match the cookbook transcription", "[v11-h][dsp][rbj]")
{
    check_coeffs(ref_rbj_lowpass, dsp::rbj_lowpass<f64>(0.25, kQ));
    check_coeffs(ref_rbj_highpass, dsp::rbj_highpass<f64>(0.25, kQ));
    check_coeffs(ref_rbj_bandpass, dsp::rbj_bandpass<f64>(0.25, kQ));
    check_coeffs(ref_rbj_notch, dsp::rbj_notch<f64>(0.25, kQ));
    check_coeffs(ref_rbj_allpass, dsp::rbj_allpass<f64>(0.25, kQ));
    check_coeffs(ref_rbj_peaking, dsp::rbj_peaking<f64>(0.25, kQ, 6.0));
    check_coeffs(ref_rbj_lowshelf, dsp::rbj_lowshelf<f64>(0.25, kQ, 6.0));
    check_coeffs(ref_rbj_highshelf, dsp::rbj_highshelf<f64>(0.25, kQ, 6.0));
}

TEST_CASE("dsp rbj: spec properties (DC / Nyquist / centre-frequency gains)", "[v11-h][dsp][rbj]")
{
    const f64 f0 = 0.25, w0 = f0 * kPi;
    // lowpass: |H(0)|=1, |H(Nyquist)|≈0.
    {
        const auto bq = dsp::rbj_lowpass<f64>(f0, kQ);
        CHECK_THAT(biquad_mag(bq, 0.0), WithinAbs(1.0, 1e-9));
        CHECK(biquad_mag(bq, kPi) < 1e-3);
    }
    // highpass: |H(0)|=0, |H(Nyquist)|=1.
    {
        const auto bq = dsp::rbj_highpass<f64>(f0, kQ);
        CHECK(biquad_mag(bq, 0.0) < 1e-12);
        CHECK_THAT(biquad_mag(bq, kPi), WithinAbs(1.0, 1e-9));
    }
    // bandpass (0 dB peak): |H(f0)|=1, |H(0)|=0, |H(Nyq)|=0.
    {
        const auto bq = dsp::rbj_bandpass<f64>(f0, kQ);
        CHECK_THAT(biquad_mag(bq, w0), WithinAbs(1.0, 1e-9));
        CHECK(biquad_mag(bq, 0.0) < 1e-12);
        CHECK(biquad_mag(bq, kPi) < 1e-12);
    }
    // notch: null at f0, unity at DC + Nyquist.
    {
        const auto bq = dsp::rbj_notch<f64>(f0, kQ);
        CHECK(biquad_mag(bq, w0) < 1e-12);
        CHECK_THAT(biquad_mag(bq, 0.0), WithinAbs(1.0, 1e-9));
        CHECK_THAT(biquad_mag(bq, kPi), WithinAbs(1.0, 1e-9));
    }
    // allpass: unity magnitude everywhere.
    {
        const auto bq = dsp::rbj_allpass<f64>(f0, kQ);
        for (f64 w : {0.1, 0.5, w0, 1.5, 3.0})
        {
            CHECK_THAT(biquad_mag(bq, w), WithinAbs(1.0, 1e-9));
        }
    }
    // peaking +6 dB: |H(f0)| = 10^(6/20), unity at DC + Nyquist.
    {
        const auto bq = dsp::rbj_peaking<f64>(f0, kQ, 6.0);
        CHECK_THAT(biquad_mag(bq, w0), WithinAbs(std::pow(10.0, 6.0 / 20.0), 1e-9));
        CHECK_THAT(biquad_mag(bq, 0.0), WithinAbs(1.0, 1e-9));
        CHECK_THAT(biquad_mag(bq, kPi), WithinAbs(1.0, 1e-9));
    }
    // low shelf +6 dB: |H(0)| = 10^(6/20), |H(Nyq)| = 1.
    {
        const auto bq = dsp::rbj_lowshelf<f64>(f0, kQ, 6.0);
        CHECK_THAT(biquad_mag(bq, 0.0), WithinAbs(std::pow(10.0, 6.0 / 20.0), 1e-9));
        CHECK_THAT(biquad_mag(bq, kPi), WithinAbs(1.0, 1e-9));
    }
    // high shelf +6 dB: |H(Nyq)| = 10^(6/20), |H(0)| = 1.
    {
        const auto bq = dsp::rbj_highshelf<f64>(f0, kQ, 6.0);
        CHECK_THAT(biquad_mag(bq, kPi), WithinAbs(std::pow(10.0, 6.0 / 20.0), 1e-9));
        CHECK_THAT(biquad_mag(bq, 0.0), WithinAbs(1.0, 1e-9));
    }
}

TEST_CASE("dsp rbj: parametric EQ chain (cascade) combines gains", "[v11-h][dsp][rbj]")
{
    crd::memory::TlsfAllocator alloc(1U << 18);
    // a 4-band EQ: low-shelf +3 dB, peaking +6 dB @0.2, peaking -4 dB @0.35, high-shelf +2 dB.
    dsp::SecondOrderSections<f64> eq(&alloc);
    eq.sections.push_back(dsp::rbj_lowshelf<f64>(0.05, kQ, 3.0));
    eq.sections.push_back(dsp::rbj_peaking<f64>(0.2, 2.0, 6.0));
    eq.sections.push_back(dsp::rbj_peaking<f64>(0.35, 2.0, -4.0));
    eq.sections.push_back(dsp::rbj_highshelf<f64>(0.7, kQ, 2.0));
    // overall response = product of section responses; check the boost band lifts and the cut band dips.
    auto sos_mag = [&](f64 w)
    {
        f64 m = 1.0;
        for (usize k = 0; k < eq.sections.size(); ++k)
        {
            m *= biquad_mag(eq.sections[k], w);
        }
        return m;
    };
    CHECK(sos_mag(0.2 * kPi) > 1.0);       // +6 dB peak band boosts
    CHECK(sos_mag(0.35 * kPi) < 1.0);      // -4 dB band cuts
    CHECK(sos_mag(0.005 * kPi) > 1.0);     // low shelf +3 dB at very low freq
}
