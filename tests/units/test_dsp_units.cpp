// crd-units v11-a -- DSP units: ratio-dB (amplitude/power) + NormalizedFrequency.

#include <crd/units/literals.hpp>
#include <crd/units/normalized_frequency.hpp>
#include <crd/units/units_nonlinear.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <numbers>

namespace
{
using namespace crd::units;
using namespace crd::units::literals;
using crd::f64;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
} // namespace

// ---------------------------------------------------------------------------
// Ratio dB -- amplitude (20*log10) convention
// ---------------------------------------------------------------------------
TEST_CASE("DecibelRatio: amplitude convention 10^(dB/20)", "[v11-a][dsp][dB]")
{
    // 0 dB == ratio 1.
    CHECK_THAT(DecibelRatio::to_si(0.0), WithinAbs(1.0, 1e-15));
    // -6.0206 dB ~ half amplitude.
    CHECK_THAT(DecibelRatio::to_si(-20.0 * std::log10(2.0)), WithinRel(0.5, 1e-12));
    // -60 dB == 1e-3 amplitude.
    CHECK_THAT(DecibelRatio::to_si(-60.0), WithinRel(1e-3, 1e-12));
    // round-trip.
    CHECK_THAT(DecibelRatio::from_si(DecibelRatio::to_si(-37.5)), WithinAbs(-37.5, 1e-12));
    // NEGATIVE dB (attenuation, the DSP-dominant case) must go through the conversion FUNCTION, not a literal:
    // `-60.0_dB_amp` would parse as -(60.0_dB_amp) and negate the LINEAR ratio (sign escapes the log). The
    // function form is the disciplined path the pin prescribes.
    const auto g = quantity_from_nonlinear<DecibelRatio>(-60.0);
    CHECK_THAT(g.value, WithinRel(1e-3, 1e-12));
    // the literal is valid for POSITIVE specs (e.g. a 20 dB gain == 10x amplitude).
    const auto gain = 20.0_dB_amp;
    CHECK_THAT(gain.value, WithinRel(10.0, 1e-12));
}

// ---------------------------------------------------------------------------
// Ratio dB -- power (10*log10) convention (the scipy/MATLAB *ord form)
// ---------------------------------------------------------------------------
TEST_CASE("DecibelPower: power convention 10^(dB/10)", "[v11-a][dsp][dB]")
{
    CHECK_THAT(DecibelPower::to_si(0.0), WithinAbs(1.0, 1e-15));
    // 3.0103 dB ~ double power.
    CHECK_THAT(DecibelPower::to_si(10.0 * std::log10(2.0)), WithinRel(2.0, 1e-12));
    // -40 dB power == 1e-4 (== amplitude 1e-2).
    CHECK_THAT(DecibelPower::to_si(-40.0), WithinRel(1e-4, 1e-12));
    CHECK_THAT(DecibelPower::from_si(DecibelPower::to_si(12.34)), WithinAbs(12.34, 1e-12));
    // amplitude vs power: the SAME dB is a DIFFERENT linear ratio (the footgun the two types kill).
    CHECK(DecibelRatio::to_si(-40.0) != DecibelPower::to_si(-40.0));
    CHECK_THAT(DecibelRatio::to_si(-40.0), WithinRel(1e-2, 1e-12)); // amplitude
    CHECK_THAT(DecibelPower::to_si(-40.0), WithinRel(1e-4, 1e-12)); // power

    const auto p = 3.0102999566398116_dB_pow; // a UDL suffixes the literal token directly (no parens)
    CHECK_THAT(p.value, WithinRel(2.0, 1e-12));
}

// ---------------------------------------------------------------------------
// NormalizedFrequency -- the three conventions agree through one canonical store
// ---------------------------------------------------------------------------
TEST_CASE("NormalizedFrequency: convention conversions are consistent", "[v11-a][dsp][normfreq]")
{
    using NF = NormalizedFrequency<f64>;
    constexpr f64 pi = std::numbers::pi_v<f64>;

    // 1 kHz against 8 kHz fs => nu = 0.125 cyc/sample.
    const NF a = NF::from_hz(1000.0, 8000.0);
    CHECK_THAT(a.cycles_per_sample(), WithinRel(0.125, 1e-15));
    CHECK_THAT(a.rad_per_sample(), WithinRel(0.25 * pi, 1e-15));   // 2*pi*0.125
    CHECK_THAT(a.nyquist_fraction(), WithinRel(0.25, 1e-15));      // 2*0.125
    CHECK_THAT(a.hz(8000.0), WithinRel(1000.0, 1e-12));

    // all four constructors land on the same canonical nu.
    CHECK_THAT(NF::from_cycles_per_sample(0.125).cycles_per_sample(), WithinAbs(0.125, 1e-15));
    CHECK_THAT(NF::from_rad_per_sample(0.25 * pi).cycles_per_sample(), WithinRel(0.125, 1e-15));
    CHECK_THAT(NF::from_nyquist_fraction(0.25).cycles_per_sample(), WithinRel(0.125, 1e-15));

    // Nyquist: Wn = 1 <=> nu = 0.5 <=> w = pi.
    const NF nyq = NF::from_nyquist_fraction(1.0);
    CHECK_THAT(nyq.cycles_per_sample(), WithinAbs(0.5, 1e-15));
    CHECK_THAT(nyq.rad_per_sample(), WithinRel(pi, 1e-15));

    // typed Hz path (Frequency<f64>).
    const auto f = Quantity<dim::Frequency, f64>{1000.0};
    const auto fs = Quantity<dim::Frequency, f64>{8000.0};
    CHECK_THAT(NF::from_hz(f, fs).nyquist_fraction(), WithinRel(0.25, 1e-15));
    CHECK_THAT(NF::from_cycles_per_sample(0.125).hz(fs).value, WithinRel(1000.0, 1e-12));
}
