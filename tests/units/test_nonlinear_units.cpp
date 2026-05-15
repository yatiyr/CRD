// crd-units v0a-3 -- Layer 3 NonLinearUnit + dB family + musical pitch.

#include <crd/units/units_nonlinear.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace
{
using namespace crd::units;
using crd::f64;
} // namespace

// ---------------------------------------------------------------------------
// (1) dB SPL: 0 dB SPL = 20 micro-Pa reference
// ---------------------------------------------------------------------------

TEST_CASE("DecibelSPL: 0 dB SPL = 20e-6 Pa",
          "[v0a-3][nonlinear][dB]")
{
    auto p = quantity_from_nonlinear<DecibelSPL>(0.0);
    CHECK(std::abs(p.value - 20.0e-6) < 1e-15);
}

TEST_CASE("DecibelSPL: 94 dB SPL = 1.0 Pa (the 'pistonphone' reference)",
          "[v0a-3][nonlinear][dB]")
{
    auto p = quantity_from_nonlinear<DecibelSPL>(94.0);
    // 20e-6 * 10^(94/20) = 20e-6 * 10^4.7 = 20e-6 * 50118.7... ~ 1.00237 Pa
    // The exact "pistonphone" reference is 94 dB SPL == 1 Pa approximately.
    CHECK(std::abs(p.value - 1.0) < 0.01);
}

TEST_CASE("DecibelSPL round-trip: dB -> Pa -> dB is identity within 1e-12",
          "[v0a-3][nonlinear][dB][round-trip]")
{
    auto p = quantity_from_nonlinear<DecibelSPL>(80.0);
    f64 back = value_in_nonlinear<DecibelSPL>(p);
    CHECK(std::abs(back - 80.0) < 1e-12);
}

// ---------------------------------------------------------------------------
// (2) Linear arithmetic in the SI domain, then convert back to dB
// ---------------------------------------------------------------------------

TEST_CASE("DecibelSPL: two identical sources sum coherently to +6 dB",
          "[v0a-3][nonlinear][dB][arith]")
{
    auto p1 = quantity_from_nonlinear<DecibelSPL>(80.0);
    auto p2 = quantity_from_nonlinear<DecibelSPL>(80.0);
    auto sum = p1 + p2;  // Pressure addition in SI domain
    f64 sum_db = value_in_nonlinear<DecibelSPL>(sum);
    CHECK(std::abs(sum_db - 86.0206) < 1e-3);  // 20*log10(2) = 6.0206 dB
}

// ---------------------------------------------------------------------------
// (3) dB V: voltage decibel
// ---------------------------------------------------------------------------

TEST_CASE("DecibelV: 0 dBV = 1 V",
          "[v0a-3][nonlinear][dB][voltage]")
{
    auto v = quantity_from_nonlinear<DecibelV>(0.0);
    CHECK(std::abs(v.value - 1.0) < 1e-15);
}

TEST_CASE("DecibelV: 20 dBV = 10 V",
          "[v0a-3][nonlinear][dB][voltage]")
{
    auto v = quantity_from_nonlinear<DecibelV>(20.0);
    CHECK(std::abs(v.value - 10.0) < 1e-12);
}

// ---------------------------------------------------------------------------
// (4) dB W: power decibel
// ---------------------------------------------------------------------------

TEST_CASE("DecibelW: 0 dBW = 1 W",
          "[v0a-3][nonlinear][dB][power]")
{
    auto w = quantity_from_nonlinear<DecibelW>(0.0);
    CHECK(std::abs(w.value - 1.0) < 1e-15);
}

TEST_CASE("DecibelW: 30 dBW = 1000 W",
          "[v0a-3][nonlinear][dB][power]")
{
    auto w = quantity_from_nonlinear<DecibelW>(30.0);
    CHECK(std::abs(w.value - 1000.0) < 1e-10);
}

// ---------------------------------------------------------------------------
// (5) dBm: power decibel referenced to milliwatt (common in RF)
// ---------------------------------------------------------------------------

TEST_CASE("DecibelMilliwatt: 0 dBm = 1 mW = 0.001 W",
          "[v0a-3][nonlinear][dBm][rf]")
{
    auto w = quantity_from_nonlinear<DecibelMilliwatt>(0.0);
    CHECK(std::abs(w.value - 1.0e-3) < 1e-15);
}

TEST_CASE("DecibelMilliwatt: 20 dBm = 100 mW = 0.1 W",
          "[v0a-3][nonlinear][dBm][rf]")
{
    auto w = quantity_from_nonlinear<DecibelMilliwatt>(20.0);
    CHECK(std::abs(w.value - 0.1) < 1e-12);
}

// ---------------------------------------------------------------------------
// (6) Cents / Semitones (musical pitch)
// ---------------------------------------------------------------------------

TEST_CASE("Cents: 0 cents = 440 Hz (concert A reference)",
          "[v0a-3][nonlinear][cents]")
{
    auto f = quantity_from_nonlinear<Cents>(0.0);
    CHECK(std::abs(f.value - 440.0) < 1e-10);
}

TEST_CASE("Cents: 1200 cents = 880 Hz (one octave above A4)",
          "[v0a-3][nonlinear][cents]")
{
    auto f = quantity_from_nonlinear<Cents>(1200.0);
    CHECK(std::abs(f.value - 880.0) < 1e-10);
}

TEST_CASE("Semitones: 12 semitones = 880 Hz (one octave)",
          "[v0a-3][nonlinear][semitones]")
{
    auto f = quantity_from_nonlinear<Semitones>(12.0);
    CHECK(std::abs(f.value - 880.0) < 1e-10);
}

TEST_CASE("Semitones: 7 semitones above A4 = ~659.255 Hz (E5)",
          "[v0a-3][nonlinear][semitones]")
{
    auto f = quantity_from_nonlinear<Semitones>(7.0);
    // E5 = 440 * 2^(7/12) ~ 659.2551...
    f64 expected = 440.0 * std::pow(2.0, 7.0 / 12.0);
    CHECK(std::abs(f.value - expected) < 1e-10);
}

// ---------------------------------------------------------------------------
// (7) Compile-time dimension validation
// ---------------------------------------------------------------------------

TEST_CASE("NonLinearUnit: dimension tag matches expected",
          "[v0a-3][nonlinear][dimension]")
{
    STATIC_REQUIRE(dim_equal_v<DecibelSPL::dimension, dim::Pressure>);
    STATIC_REQUIRE(dim_equal_v<DecibelV::dimension,   dim::Voltage>);
    STATIC_REQUIRE(dim_equal_v<DecibelW::dimension,   dim::Power>);
    STATIC_REQUIRE(dim_equal_v<DecibelMilliwatt::dimension, dim::Power>);
    STATIC_REQUIRE(dim_equal_v<Cents::dimension,      dim::Frequency>);
    STATIC_REQUIRE(dim_equal_v<Semitones::dimension,  dim::Frequency>);
    STATIC_REQUIRE(DecibelSPL::is_nonlinear);
    STATIC_REQUIRE(Cents::is_nonlinear);
}
