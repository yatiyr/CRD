// crd-units v0a-3 -- UDL tests.
//
// Every named UDL converts to the expected SI value at construction.
// Ambiguous literals (`_lb`, `_oz`) are NOT defined -- compile error at
// use is the intended behaviour.

#include <crd/units/literals.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace
{
using namespace crd::units;
using namespace crd::units::literals;
using crd::f64;
} // namespace

// ---------------------------------------------------------------------------
// (1) Length UDLs
// ---------------------------------------------------------------------------

TEST_CASE("UDL: length literals convert to SI meters",
          "[v0a-3][literals][length]")
{
    CHECK((1.0_m).value     == 1.0);
    CHECK((1.0_km).value    == 1000.0);
    CHECK((1.0_cm).value    == 0.01);
    CHECK((1.0_mm).value    == 0.001);
    CHECK((25.4_mm).value   == 0.0254);
    CHECK((1.0_in).value    == 0.0254);
    CHECK((1.0_ft).value    == 0.3048);
    CHECK((1.0_yd).value    == 0.9144);
    CHECK((1.0_mi).value    == 1609.344);
    CHECK((1.0_nmi).value   == 1852.0);
}

TEST_CASE("UDL: integer length literals work",
          "[v0a-3][literals][length]")
{
    CHECK((100_mm).value   == 0.1);
    CHECK((5280_ft).value  == 1609.344);
}

// ---------------------------------------------------------------------------
// (2) Mass UDLs
// ---------------------------------------------------------------------------

TEST_CASE("UDL: mass literals convert to SI kg",
          "[v0a-3][literals][mass]")
{
    CHECK((1.0_kg).value      == 1.0);
    CHECK((1.0_g).value       == 0.001);
    CHECK((1.0_mg).value      == 1e-6);
    CHECK((1.0_tonne).value   == 1000.0);
    CHECK((1.0_lb_mass).value == 0.45359237);
    CHECK((1.0_oz_mass).value == 0.45359237 / 16.0);
}

// ---------------------------------------------------------------------------
// (3) Time UDLs
// ---------------------------------------------------------------------------

TEST_CASE("UDL: time literals convert to SI seconds",
          "[v0a-3][literals][time]")
{
    CHECK((1.0_s).value     == 1.0);
    CHECK((1.0_ms).value    == 0.001);
    CHECK((1.0_us).value    == 1e-6);
    CHECK((1.0_ns).value    == 1e-9);
    CHECK((1.0_min).value   == 60.0);
    CHECK((1.0_h).value     == 3600.0);
}

// ---------------------------------------------------------------------------
// (4) Force / pressure / energy / power
// ---------------------------------------------------------------------------

TEST_CASE("UDL: force literals",
          "[v0a-3][literals][force]")
{
    CHECK((1.0_N).value   == 1.0);
    CHECK((1.0_kN).value  == 1000.0);
    CHECK(std::abs((1.0_lbf).value - 4.4482216152605) < 1e-10);
    CHECK(std::abs((1.0_kgf).value - 9.80665) < 1e-12);
}

TEST_CASE("UDL: pressure literals",
          "[v0a-3][literals][pressure]")
{
    CHECK((1.0_Pa).value   == 1.0);
    CHECK((1.0_kPa).value  == 1000.0);
    CHECK((1.0_MPa).value  == 1e6);
    CHECK((1.0_bar).value  == 100000.0);
    CHECK((1.0_atm).value  == 101325.0);
}

TEST_CASE("UDL: energy literals",
          "[v0a-3][literals][energy]")
{
    CHECK((1.0_J).value     == 1.0);
    CHECK((1.0_kJ).value    == 1000.0);
    CHECK((1.0_kWh).value   == 3.6e6);
    CHECK((1.0_cal).value   == 4.184);
    CHECK((1.0_kcal).value  == 4184.0);
}

TEST_CASE("UDL: power literals",
          "[v0a-3][literals][power]")
{
    CHECK((1.0_W).value     == 1.0);
    CHECK((1.0_kW).value    == 1000.0);
    CHECK((1.0_MW).value    == 1e6);
}

// ---------------------------------------------------------------------------
// (5) Frequency / electrical
// ---------------------------------------------------------------------------

TEST_CASE("UDL: frequency literals",
          "[v0a-3][literals][frequency]")
{
    CHECK((1.0_Hz).value   == 1.0);
    CHECK((1.0_kHz).value  == 1000.0);
    CHECK((1.0_MHz).value  == 1e6);
    CHECK((1.0_GHz).value  == 1e9);
}

TEST_CASE("UDL: electrical literals",
          "[v0a-3][literals][electrical]")
{
    CHECK((1.0_V).value    == 1.0);
    CHECK((1.0_mV).value   == 0.001);
    CHECK((1.0_kV).value   == 1000.0);
    CHECK((1.0_A).value    == 1.0);
    CHECK((1.0_mA).value   == 0.001);
    CHECK((1.0_uA).value   == 1e-6);
    CHECK((1.0_ohm).value  == 1.0);
    CHECK((1.0_F).value    == 1.0);
    CHECK((1.0_uF).value   == 1e-6);
    CHECK((1.0_nF).value   == 1e-9);
    CHECK((1.0_pF).value   == 1e-12);
    CHECK((1.0_H).value    == 1.0);
    CHECK((1.0_mH).value   == 0.001);
}

// ---------------------------------------------------------------------------
// (6) Temperature (affine) literals
// ---------------------------------------------------------------------------

TEST_CASE("UDL: temperature literals are affine",
          "[v0a-3][literals][temperature]")
{
    auto t1 = 0.0_celsius;
    CHECK(t1.value == 273.15);

    auto t2 = 100.0_celsius;
    CHECK(t2.value == 373.15);

    auto t3 = 32.0_fahrenheit;
    CHECK(std::abs(t3.value - 273.15) < 1e-10);

    auto t4 = 0.0_kelvin;
    CHECK(t4.value == 0.0);

    // Subtraction strips offset.
    auto diff = t2 - t1;
    CHECK(diff.value == 100.0);
}

// ---------------------------------------------------------------------------
// (7) Non-linear (dB / cents / semitones) literals
// ---------------------------------------------------------------------------

TEST_CASE("UDL: dB SPL literal",
          "[v0a-3][literals][nonlinear]")
{
    auto p = 0.0_dB_spl;
    CHECK(std::abs(p.value - 20.0e-6) < 1e-15);
}

TEST_CASE("UDL: dBm literal",
          "[v0a-3][literals][nonlinear]")
{
    auto w = 0.0_dBm;
    CHECK(std::abs(w.value - 1.0e-3) < 1e-15);
}

TEST_CASE("UDL: cents literal",
          "[v0a-3][literals][nonlinear]")
{
    auto f = 0.0_cents;
    CHECK(std::abs(f.value - 440.0) < 1e-10);

    auto f_oct = 1200.0_cents;
    CHECK(std::abs(f_oct.value - 880.0) < 1e-10);
}

// ---------------------------------------------------------------------------
// (8) Angular velocity (compound via RPM)
// ---------------------------------------------------------------------------

TEST_CASE("UDL: rpm literal",
          "[v0a-3][literals][angular]")
{
    auto omega = 60.0_rpm;
    // 60 rpm = 1 rev / s = 2*pi rad/s
    CHECK(std::abs(omega.value - 6.283185307179586) < 1e-12);
}

// ---------------------------------------------------------------------------
// (9) Ambiguous-literal policy (compile-time check via NOT compiling them)
// ---------------------------------------------------------------------------
//
// `_lb` and `_oz` are deliberately NOT defined. Writing `1.0_lb` would
// produce a compile error. We verify the contrapositive: the disambiguated
// forms compile.

TEST_CASE("UDL: disambiguated forms compile",
          "[v0a-3][literals][ambiguous]")
{
    auto m   = 1.0_lb_mass;
    auto f   = 1.0_lbf;
    auto m2  = 1.0_oz_mass;
    (void)m; (void)f; (void)m2;
    CHECK(true);
}
