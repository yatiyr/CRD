// crd-config v0b-2 -- unit-tagged TOML accessors.

#include <crd/config/config.hpp>
#include <crd/config/unit_accessor.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{

crd::config::Config make_cfg(crd::containers::StringView toml)
{
    crd::config::Config cfg;
    REQUIRE(cfg.load_from_string(toml));
    return cfg;
}

} // namespace

TEST_CASE("get_length parses _m / _mm / _cm / _km", "[config][units][length]")
{
    auto cfg = make_cfg(R"(
        height_m  = 1.7
        height_mm = 1700.0
        height_cm = 170.0
        height_km = 0.0017
    )");
    using crd::config::get_length;
    using crd::units::Length32;
    const auto fallback = Length32{0.0F};
    CHECK(get_length<crd::f32>(cfg, "height_m",  fallback).value == Catch::Approx(1.7F));
    CHECK(get_length<crd::f32>(cfg, "height_mm", fallback).value == Catch::Approx(1.7F));
    CHECK(get_length<crd::f32>(cfg, "height_cm", fallback).value == Catch::Approx(1.7F));
    CHECK(get_length<crd::f32>(cfg, "height_km", fallback).value == Catch::Approx(1.7F));
}

TEST_CASE("get_length parses imperial suffixes", "[config][units][length][imperial]")
{
    auto cfg = make_cfg(R"(
        len_in = 39.3700787  # ~ 1 m
        len_ft = 3.2808399   # ~ 1 m
        len_yd = 1.0936133   # ~ 1 m
        len_mi = 0.000621371 # ~ 1 m
    )");
    using crd::config::get_length;
    using crd::units::Length32;
    const auto fb = Length32{0.0F};
    CHECK(get_length<crd::f32>(cfg, "len_in", fb).value == Catch::Approx(1.0F).margin(1e-4F));
    CHECK(get_length<crd::f32>(cfg, "len_ft", fb).value == Catch::Approx(1.0F).margin(1e-4F));
    CHECK(get_length<crd::f32>(cfg, "len_yd", fb).value == Catch::Approx(1.0F).margin(1e-4F));
    CHECK(get_length<crd::f32>(cfg, "len_mi", fb).value == Catch::Approx(1.0F).margin(1e-3F));
}

TEST_CASE("get_mass parses _kg / _g / _t / _lb_mass", "[config][units][mass]")
{
    auto cfg = make_cfg(R"(
        m_kg      = 5.0
        m_g       = 5000.0
        m_t       = 0.005
        m_lb_mass = 11.0231
    )");
    using crd::config::get_mass;
    using crd::units::Mass32;
    const auto fb = Mass32{0.0F};
    CHECK(get_mass<crd::f32>(cfg, "m_kg",      fb).value == Catch::Approx(5.0F));
    CHECK(get_mass<crd::f32>(cfg, "m_g",       fb).value == Catch::Approx(5.0F));
    CHECK(get_mass<crd::f32>(cfg, "m_t",       fb).value == Catch::Approx(5.0F));
    CHECK(get_mass<crd::f32>(cfg, "m_lb_mass", fb).value == Catch::Approx(5.0F).margin(1e-3F));
}

TEST_CASE("get_angle parses _rad / _deg / _turn", "[config][units][angle]")
{
    auto cfg = make_cfg(R"(
        a_rad  = 3.141592653589793
        a_deg  = 180.0
        a_turn = 0.5
    )");
    using crd::config::get_angle;
    using crd::units::Angle32;
    const auto fb = Angle32{0.0F};
    CHECK(get_angle<crd::f32>(cfg, "a_rad",  fb).value == Catch::Approx(3.14159F).margin(1e-4F));
    CHECK(get_angle<crd::f32>(cfg, "a_deg",  fb).value == Catch::Approx(3.14159F).margin(1e-4F));
    CHECK(get_angle<crd::f32>(cfg, "a_turn", fb).value == Catch::Approx(3.14159F).margin(1e-4F));
}

TEST_CASE("get_time parses _s / _ms / _us / _ns / _min / _hr", "[config][units][time]")
{
    auto cfg = make_cfg(R"(
        t_s   = 1.0
        t_ms  = 1000.0
        t_us  = 1000000.0
        t_min = 0.016666666666666666
        t_hr  = 0.0002777777777777778
    )");
    using crd::config::get_time;
    using crd::units::Time64;
    const auto fb = Time64{0.0};
    CHECK(get_time<crd::f64>(cfg, "t_s",   fb).value == Catch::Approx(1.0));
    CHECK(get_time<crd::f64>(cfg, "t_ms",  fb).value == Catch::Approx(1.0));
    CHECK(get_time<crd::f64>(cfg, "t_us",  fb).value == Catch::Approx(1.0));
    CHECK(get_time<crd::f64>(cfg, "t_min", fb).value == Catch::Approx(1.0).margin(1e-6));
    CHECK(get_time<crd::f64>(cfg, "t_hr",  fb).value == Catch::Approx(1.0).margin(1e-6));
}

TEST_CASE("get_temperature handles affine conversion (celsius/fahrenheit/kelvin)",
          "[config][units][temperature][affine]")
{
    auto cfg = make_cfg(R"(
        t_kelvin     = 298.15
        t_celsius    = 25.0
        t_fahrenheit = 77.0
    )");
    using crd::config::get_temperature;
    using crd::units::Temperature;
    const auto fb = Temperature<crd::f32>{0.0F};
    CHECK(get_temperature<crd::f32>(cfg, "t_kelvin",     fb).value == Catch::Approx(298.15F));
    CHECK(get_temperature<crd::f32>(cfg, "t_celsius",    fb).value == Catch::Approx(298.15F).margin(1e-3F));
    CHECK(get_temperature<crd::f32>(cfg, "t_fahrenheit", fb).value == Catch::Approx(298.15F).margin(1e-2F));
}

TEST_CASE("get_velocity handles _mps / _kmph / _mph / _knots", "[config][units][velocity]")
{
    auto cfg = make_cfg(R"(
        v_mps   = 10.0
        v_kmph  = 36.0
        v_mph   = 22.3694
        v_knots = 19.4385
    )");
    using crd::config::get_velocity;
    using crd::units::Velocity32;
    const auto fb = Velocity32{0.0F};
    CHECK(get_velocity<crd::f32>(cfg, "v_mps",   fb).value == Catch::Approx(10.0F));
    CHECK(get_velocity<crd::f32>(cfg, "v_kmph",  fb).value == Catch::Approx(10.0F).margin(1e-3F));
    CHECK(get_velocity<crd::f32>(cfg, "v_mph",   fb).value == Catch::Approx(10.0F).margin(1e-3F));
    CHECK(get_velocity<crd::f32>(cfg, "v_knots", fb).value == Catch::Approx(10.0F).margin(1e-3F));
}

TEST_CASE("get_force / get_pressure / get_energy / get_power", "[config][units][derived]")
{
    auto cfg = make_cfg(R"(
        f_N    = 100.0
        p_kPa  = 101.325
        e_kJ   = 4.184
        w_kW   = 1.0
    )");
    using namespace crd::config;
    using namespace crd::units;
    CHECK(get_force<crd::f32>(cfg, "f_N",   Force32{0.0F}).value    == Catch::Approx(100.0F));
    CHECK(get_pressure<crd::f32>(cfg, "p_kPa", Pressure32{0.0F}).value
          == Catch::Approx(101325.0F).margin(1.0F));
    CHECK(get_energy<crd::f32>(cfg, "e_kJ",  Energy32{0.0F}).value  == Catch::Approx(4184.0F));
    CHECK(get_power<crd::f32>(cfg, "w_kW",   Power32{0.0F}).value   == Catch::Approx(1000.0F));
}

TEST_CASE("get_voltage / get_current / get_frequency", "[config][units][electrical]")
{
    auto cfg = make_cfg(R"(
        v_V   = 3.3
        v_kV  = 0.0033
        v_mV  = 3300.0
        i_A   = 0.5
        i_mA  = 500.0
        f_Hz  = 60.0
        f_kHz = 0.060
        f_MHz = 6.0e-5
    )");
    using namespace crd::config;
    using namespace crd::units;
    const auto fbv = Voltage32{0.0F};
    const auto fbi = Current32{0.0F};
    const auto fbf = Frequency32{0.0F};
    CHECK(get_voltage<crd::f32>(cfg, "v_V",  fbv).value == Catch::Approx(3.3F));
    CHECK(get_voltage<crd::f32>(cfg, "v_kV", fbv).value == Catch::Approx(3.3F));
    CHECK(get_voltage<crd::f32>(cfg, "v_mV", fbv).value == Catch::Approx(3.3F));
    CHECK(get_current<crd::f32>(cfg, "i_A",  fbi).value == Catch::Approx(0.5F));
    CHECK(get_current<crd::f32>(cfg, "i_mA", fbi).value == Catch::Approx(0.5F));
    CHECK(get_frequency<crd::f32>(cfg, "f_Hz",  fbf).value == Catch::Approx(60.0F));
    CHECK(get_frequency<crd::f32>(cfg, "f_kHz", fbf).value == Catch::Approx(60.0F));
    CHECK(get_frequency<crd::f32>(cfg, "f_MHz", fbf).value == Catch::Approx(60.0F).margin(0.01F));
}

TEST_CASE("missing key returns fallback", "[config][units][fallback]")
{
    auto cfg = make_cfg("# empty");
    using crd::config::get_length;
    using crd::units::Length32;
    const auto fb = Length32{42.5F};
    CHECK(get_length<crd::f32>(cfg, "nonexistent_mm", fb).value == 42.5F);
}

TEST_CASE("unknown suffix returns fallback", "[config][units][fallback]")
{
    auto cfg = make_cfg("len_foo = 100.0");
    using crd::config::get_length;
    using crd::units::Length32;
    const auto fb = Length32{99.0F};
    CHECK(get_length<crd::f32>(cfg, "len_foo", fb).value == 99.0F); // _foo not a length suffix
}

TEST_CASE("key without underscore-suffix returns fallback",
          "[config][units][fallback]")
{
    auto cfg = make_cfg("plain = 5.0");
    using crd::config::get_length;
    using crd::units::Length32;
    const auto fb = Length32{1.5F};
    CHECK(get_length<crd::f32>(cfg, "plain", fb).value == 1.5F);
}

TEST_CASE("f64 instantiations work alongside f32", "[config][units][precision]")
{
    auto cfg = make_cfg("len_mm = 25.4");
    using crd::config::get_length;
    using crd::units::Length;
    CHECK(get_length<crd::f64>(cfg, "len_mm", Length<crd::f64>{0.0}).value
          == Catch::Approx(0.0254));
    CHECK(get_length<crd::f32>(cfg, "len_mm", Length<crd::f32>{0.0F}).value
          == Catch::Approx(0.0254F));
}
