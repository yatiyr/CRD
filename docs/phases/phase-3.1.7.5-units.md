# Phase 3.1.7.5 — `crd-units`: compile-time-dimensional units substrate

**Status:** ✅ **CLOSED 2026-05-15** — all 4 arcs shipped (v0a substrate + v0b/c/d adoption); ADR-0078; `crd-no-untagged-physical-numeric` CI guard live. Originally locked 2026-05-14 as the architectural answer to
the units / scale / precision question surfaced in the Phase 3.1.7
renewed-scope review. Sequenced **between Phase 3.1.7 `crd-geometry`
close and Phase 3.1 eylem v1c resume** so eylem v1c+ and every
downstream Cerid module consumes dimensional types **from day 1**.

**Strategic mandate (user 2026-05-14):** "Every physical and scientific
stuff always having units no matter what." This is a Cerid project-wide
architectural commitment, pinned in `docs/PRINCIPLES.md`. There is no
opt-out path. Bare `f32` / `f64` for a physical quantity is a
code-review block, a CI-guard violation, and a memory feedback
trigger for future agents.

**ADR:** ADR-0078 candidate (mint at v0a close, per the established
ADR-at-slice-time pattern; ADR-0076 §15/§18 precedents).

**Cornerstones (will be added to `docs/PRINCIPLES.md` § Architectural
Cornerstones once ADR-0078 lands):**
- All physical quantities carry compile-time dimension tags via `Quantity<D, T>` wrappers.
- Internal canonical unit = SI base (m / kg / s / rad / K / A / cd / mol).
- Precision tier (f32 / f64) is orthogonal to dimensional type.
- Asset / file / UI boundaries normalize to SI at load; runtime never sees non-SI.
- SIMD / GPU hot paths decay to raw scalar via `.value` (zero-overhead).
- No untagged physical numeric crosses a public module boundary.

## Why this exists

Cerid serves **eight equal-class domains**: games, simulation (incl.
robotics), medical visualization, DAW-class creative tools, offline
cinematic pipelines, manufacturing/CAD, CFD/FEA, aerospace/mechanics.
Each domain has historically used a different unit convention:

| Domain | Conventional unit |
|---|---|
| Games (Unity) | 1 unit = 1 m, bare `float` |
| Games (Unreal) | 1 unit = 1 cm, bare `double`/`float` |
| Robotics (ROS) | SI meters / radians (REP 103) |
| CAD (Solidworks, Inventor) | mm (or in per user pref), bare `double` |
| CAD (Fusion 360, Onshape) | cm internally, mm displayed |
| PCB (KiCad, Altium) | mm and mil interchangeably |
| Aerospace large-world | km or AU; f64 absolutely required |
| 3D printing (slicers) | mm + mm/s + °C |
| CFD / FEA | SI; some legacy code in CGS or imperial |
| Audio / DAWs | seconds + Hz |
| Medical (DICOM) | mm + ms |

Without a unified type system, every cross-domain integration is a
unit-conversion bug waiting to happen. The Mars Climate Orbiter ($327M
loss, 1999) was exactly this class of bug — pound·force·seconds vs
Newton·seconds across a module boundary. NASA's response was a units
policy that every spacecraft simulator now bakes in.

The single elegant move that solves ALL of this: **make units a
compile-time type at the API surface, with zero runtime cost**. The
substrate is small (~3 KLOC); the safety dividend is huge; the
adoption pattern propagates uniformly to every future domain module.

## Why NOT bloat `crd-math`

Same reasoning that motivated `crd-sdf` (ADR-0064 §2), `crd-hesap`
(ADR-0065 §2), and `crd-geometry` (ADR-0076 §2):

1. **Different abstraction tier.** `crd-math` is the SIMD-friendly raw-float linear-algebra substrate (Vec / Mat / Quat / Transform / SIMD primitives / determinism wrappers). `crd-units` is the dimensional-type system that *consumes* `crd-math` (a `Vec3<Length<f32>>` is layout-equal to `Vec3<f32>` but carries dimensional semantics). Bloating `crd-math` with dimensional types fuses two abstraction tiers and forces the SIMD kernels to see template noise.
2. **Independent evolution.** Future PCB / EDA needs `Voltage` / `Resistance` / `Capacitance` / `MagneticFlux`; future photometric rendering needs `LuminousFlux` / `Illuminance` / `Luminance`; future chemistry needs `AmountOfSubstance`. `crd-math` should not grow with each new domain's dimensions.
3. **Leaf-substrate discipline.** `crd-units` is a leaf module (deps: `crd-core` only). Putting it inside `crd-math` would inflate `crd-math`'s public surface for every consumer that doesn't need dimensional safety.
4. **CI guard scoping.** The `no-untagged-physical-numeric` CI guard
   (v0a) scopes to `engine/` *except* `crd-math`'s SIMD kernels and
   `crd-rhi-vulkan`'s raw-buffer-upload paths. A separate module gives
   the guard a clean opt-out boundary.

## Core type — `Quantity<D, T>`

```cpp
namespace crd::units
{

template <typename D, typename T = f32>
struct Quantity
{
    T value;  // ALWAYS in SI base unit

    constexpr Quantity() = default;
    explicit constexpr Quantity(T v) : value(v) {}

    // Same-dimension arithmetic
    constexpr Quantity operator+(Quantity rhs) const { return Quantity{value + rhs.value}; }
    constexpr Quantity operator-(Quantity rhs) const { return Quantity{value - rhs.value}; }
    constexpr Quantity operator-() const { return Quantity{-value}; }

    // Scalar arithmetic (dimensionless multiplier)
    constexpr Quantity operator*(T s) const { return Quantity{value * s}; }
    constexpr Quantity operator/(T s) const { return Quantity{value / s}; }

    // Comparison
    constexpr bool operator==(Quantity rhs) const = default;
    constexpr auto operator<=>(Quantity rhs) const = default;

    // Boundary egress (compile-time validated against D)
    template <typename TargetUnit>
    constexpr T value_in() const;
};

// Cross-dimension product / quotient
template <typename Da, typename Db, typename T>
constexpr Quantity<DimMul<Da, Db>, T>
operator*(Quantity<Da, T> a, Quantity<Db, T> b) { return Quantity<DimMul<Da, Db>, T>{a.value * b.value}; }

template <typename Da, typename Db, typename T>
constexpr Quantity<DimDiv<Da, Db>, T>
operator/(Quantity<Da, T> a, Quantity<Db, T> b) { return Quantity<DimDiv<Da, Db>, T>{a.value / b.value}; }

}  // namespace crd::units
```

**Dimension representation.** A `Dim<L, M, T, I, Theta, N, J, A>`
template carries eight signed-integer compile-time exponents (the 7 SI
base + Angle as a tagged-dimensionless distinct base, since strict-SI
radians = m/m is silently dimensionless, breaking the type-safety we
want for angles):

```cpp
template <int L, int M, int T, int I, int Th, int N, int J, int A>
struct Dim {};

// L = Length, M = Mass, T = Time, I = Current,
// Th = Thermodynamic temperature, N = Amount of substance,
// J = Luminous intensity, A = Angle (tagged-dimensionless)

namespace dim
{
    using Dimensionless = Dim< 0, 0, 0, 0, 0, 0, 0, 0>;
    using Length        = Dim< 1, 0, 0, 0, 0, 0, 0, 0>;
    using Mass          = Dim< 0, 1, 0, 0, 0, 0, 0, 0>;
    using Time          = Dim< 0, 0, 1, 0, 0, 0, 0, 0>;
    using Current       = Dim< 0, 0, 0, 1, 0, 0, 0, 0>;
    using Temperature   = Dim< 0, 0, 0, 0, 1, 0, 0, 0>;
    using Amount        = Dim< 0, 0, 0, 0, 0, 1, 0, 0>;
    using LuminousI     = Dim< 0, 0, 0, 0, 0, 0, 1, 0>;
    using Angle         = Dim< 0, 0, 0, 0, 0, 0, 0, 1>;
}

// DimMul / DimDiv add / subtract exponents component-wise.
```

Aliases for the common quantities (all zero-overhead, all type-distinct):

```cpp
template <typename T = f32> using Length            = Quantity<dim::Length, T>;
template <typename T = f32> using Mass              = Quantity<dim::Mass, T>;
template <typename T = f32> using Time              = Quantity<dim::Time, T>;
template <typename T = f32> using Angle             = Quantity<dim::Angle, T>;
template <typename T = f32> using Temperature       = Quantity<dim::Temperature, T>;
template <typename T = f32> using Current           = Quantity<dim::Current, T>;

template <typename T = f32> using Area              = Quantity<DimMul<dim::Length, dim::Length>, T>;
template <typename T = f32> using Volume            = Quantity<DimMul<DimMul<dim::Length, dim::Length>, dim::Length>, T>;
template <typename T = f32> using Velocity          = Quantity<DimDiv<dim::Length, dim::Time>, T>;
template <typename T = f32> using Acceleration      = Quantity<DimDiv<Velocity<T>::dim, dim::Time>, T>;
template <typename T = f32> using Force             = Quantity<DimMul<dim::Mass, Acceleration<T>::dim>, T>;
template <typename T = f32> using Pressure          = Quantity<DimDiv<Force<T>::dim, Area<T>::dim>, T>;
template <typename T = f32> using Energy            = Quantity<DimMul<Force<T>::dim, dim::Length>, T>;
template <typename T = f32> using Power             = Quantity<DimDiv<Energy<T>::dim, dim::Time>, T>;
template <typename T = f32> using Torque            = Quantity<DimMul<Force<T>::dim, dim::Length>, T>;
template <typename T = f32> using AngularVelocity   = Quantity<DimDiv<dim::Angle, dim::Time>, T>;
template <typename T = f32> using AngularAccel      = Quantity<DimDiv<AngularVelocity<T>::dim, dim::Time>, T>;
template <typename T = f32> using MomentOfInertia   = Quantity<DimMul<dim::Mass, Area<T>::dim>, T>;
template <typename T = f32> using Momentum          = Quantity<DimMul<dim::Mass, Velocity<T>::dim>, T>;
template <typename T = f32> using AngularMomentum   = Quantity<DimMul<MomentOfInertia<T>::dim, AngularVelocity<T>::dim>, T>;
template <typename T = f32> using Frequency         = Quantity<DimInv<dim::Time>, T>;
template <typename T = f32> using Density           = Quantity<DimDiv<dim::Mass, Volume<T>::dim>, T>;

// Electrical (consumed by future Phase 3.1.17 crd-eda)
template <typename T = f32> using Voltage           = Quantity<..., T>;
template <typename T = f32> using Resistance        = Quantity<..., T>;
template <typename T = f32> using Capacitance       = Quantity<..., T>;
template <typename T = f32> using Inductance        = Quantity<..., T>;
template <typename T = f32> using Charge            = Quantity<..., T>;
template <typename T = f32> using MagneticFlux      = Quantity<..., T>;
template <typename T = f32> using MagneticField     = Quantity<..., T>;

// Photometric (consumed by future Phase 3.5+ rendering area lights)
template <typename T = f32> using LuminousFlux      = Quantity<..., T>;
template <typename T = f32> using Illuminance       = Quantity<..., T>;
template <typename T = f32> using Luminance         = Quantity<..., T>;

// Thermodynamic (consumed by future Phase 3.1.10 crd-cfd, Phase 3.1.13 crd-cam 3D printing)
template <typename T = f32> using HeatCapacity      = Quantity<..., T>;
template <typename T = f32> using ThermalConductivity = Quantity<..., T>;
template <typename T = f32> using HeatFlux          = Quantity<..., T>;

// (Full list: ~50 named derived quantities in v0a.)
```

## User-defined literals — ergonomic ingress

```cpp
using namespace crd::units::literals;

auto box_size       = 25.4_mm;       // → Length<f32>{0.0254}
auto bolt_length    = 1.5_in;        // → Length<f32>{0.0381}
auto gravity        = 9.81_m_per_s2; // → Acceleration<f32>
auto bolt_torque    = 50.0_N_m;      // → Torque<f32>
auto spindle_speed  = 12000.0_rpm;   // → AngularVelocity<f32>
auto print_speed    = 60.0_mm_per_s; // → Velocity<f32>
auto print_temp     = 215.0_celsius; // → Temperature<f32>{488.15}  (Kelvin internally)
auto chip_voltage   = 3.3_V;         // → Voltage<f32>
auto trace_width    = 0.15_mm;       // → Length<f32>
auto coil_inductance = 4.7_uH;       // → Inductance<f32>
auto fps_target     = 60.0_Hz;       // → Frequency<f32>
auto camera_fov     = 45.0_deg;      // → Angle<f32>{π/4}            (radians internally)
auto orbital_period = 90.0_min;      // → Time<f32>{5400.0}          (seconds internally)
auto orbital_radius = 6800.0_km;     // → Length<f32>{6.8e6}
auto au_distance    = 1.0_au;        // → Length<f64>{1.496e11}      (auto-promotes to f64)
auto bullet_mass    = 9.5_g;         // → Mass<f32>{0.0095}
auto load_weight    = 50.0_lb;       // → Mass<f32>{22.6796}
```

Full UDL set in v0a: **~120 literals** covering length (m / mm / cm / km / μm / nm / in / ft / yd / mi / nmi / au / ly), mass (kg / g / mg / ton / lb / oz / slug), time (s / ms / μs / ns / min / h / day / year), angle (rad / deg / grad / rev / arcmin / arcsec), force (N / kN / lbf / kgf), pressure (Pa / kPa / MPa / GPa / psi / bar / atm / mmHg / torr), energy (J / kJ / MJ / cal / kcal / eV / Btu / Wh / kWh), power (W / kW / MW / hp / Btu_per_hr), velocity (m_per_s / km_per_h / mph / knot / ft_per_s / mm_per_s), acceleration (m_per_s2 / G / standard_g), torque (N_m / lbf_ft / lbf_in / kgf_cm), frequency (Hz / kHz / MHz / GHz / rpm), voltage (V / mV / kV), current (A / mA / μA), resistance (Ohm / kOhm / MOhm), capacitance (F / mF / μF / nF / pF), inductance (H / mH / μH / nH), temperature (K / celsius / fahrenheit / rankine), luminous flux (lm), illuminance (lx), luminance (cd_per_m2 / nit).

## Boundary egress — `.value_in<TargetUnit>()`

```cpp
struct Millimeter { /* tag type carrying conversion factor */ };
struct Inch       { /* tag type carrying conversion factor */ };
struct Degree     { /* tag type carrying conversion factor */ };
struct Pound      { /* tag type for force, not mass — pound-force */ };

f64 mm   = box_size.value_in<Millimeter>();      // 25.4
f64 in   = box_size.value_in<Inch>();            // 1.0
f64 deg  = camera_fov.value_in<Degree>();        // 45.0
f64 rpm  = spindle_speed.value_in<Rpm>();        // 12000.0

// Dimension-mismatched call is a compile error:
f64 mass_in_meters = box_size.value_in<Kilogram>();  // ERROR: Length has dimension [L], Kilogram has [M]
```

The unit-tag types live in `crd-units` and are also templated on the
target dimension — `value_in<Millimeter>` compile-checks that the
caller's `Quantity::dim` is `Length`.

## Vec / Mat / Quat dimensional wrappers

```cpp
template <typename Q> struct Vec2;  // where Q is a Quantity<...>
template <typename Q> struct Vec3;
template <typename Q> struct Vec4;
template <typename Q> struct Mat3;
template <typename Q> struct Mat4;

using Position3   = Vec3<Length<f32>>;
using Position3d  = Vec3<Length<f64>>;
using Velocity3   = Vec3<Velocity<f32>>;
using Force3      = Vec3<Force<f32>>;
using Torque3     = Vec3<Torque<f32>>;
using AngVel3     = Vec3<AngularVelocity<f32>>;
using AngMom3     = Vec3<AngularMomentum<f32>>;

// Quat stays dimensionless (unit quaternion); orientation NOT carried as
// a dimensional type (a quaternion's components are dimensionless even
// though the rotation it represents is "an angle"). Conversion to/from
// axis-angle uses Angle for the angle and Vec3<f32> for the axis.
```

**Layout pin (CI-enforced):** `Vec3<Length<f32>>` is layout-equal to
`Vec3<f32>` — same size, same alignment, same field order, no extra
bytes. The SIMD-friendly SoA storage in `crd-scene`'s archetype
chunks (3.0 v1f shipped) reinterprets `f32[N]` columns as
`Length<f32>[N]` at the access point with zero conversion. The wrapper
is `std::is_standard_layout_v` + `std::is_trivially_copyable_v` —
safe to memcpy and to upload as a GPU uniform.

## Unit conversion system (the boundary surface in detail)

The single most-touched surface of `crd-units` is converting between
unit choices — at file load, at UI display, at cross-domain
integration, at cross-engine asset import. **Conversion is not a
runtime cost to optimize away; it is a boundary discipline that must
be ergonomic, exact-where-possible, performant-everywhere, and
trivially extensible.** Cerid will widen this surface continuously
as new domains land (PCB needs `Mil`, aerospace needs `AU` / `ly`,
audio needs `dB` family, CAM needs `RPM` and `IPM`, …), so the
framework is designed to absorb new units / dimensions / conversion
classes **without touching `crd-units` core**.

The system has **6 layers**, each addressing a distinct conversion
class. Layers compose freely: a domain-extended non-linear logarithmic
unit composed with a SI-prefixed base composed into a compound derived
unit all flow through the same compile-time pathway.

### Layer 1 — Linear units with compile-time rational factors

Most conversions are pure scale factors. **Encode as `std::ratio` not
`f64`** so conversions are exact integer arithmetic at compile time.
SI-prefix conversions (`m ↔ mm` / `kg ↔ g`) become bit-exact
round-trips; standardised imperial conversions (1 in = 0.0254 m,
1 mile = 1609344 mm, 1 lb = 0.45359237 kg) are likewise exact rationals.

```cpp
template <typename Dim, typename FactorRatio>
struct LinearUnit
{
    using dimension    = Dim;
    using factor_ratio = FactorRatio;
    static constexpr f64 factor = static_cast<f64>(FactorRatio::num) / FactorRatio::den;
};

// SI base
using Meter      = LinearUnit<dim::Length, std::ratio<1>>;
using Kilogram   = LinearUnit<dim::Mass,   std::ratio<1>>;
using Second     = LinearUnit<dim::Time,   std::ratio<1>>;
using Radian     = LinearUnit<dim::Angle,  std::ratio<1>>;
using Kelvin     = LinearUnit<dim::Temperature, std::ratio<1>>;
using Ampere     = LinearUnit<dim::Current, std::ratio<1>>;
using Candela    = LinearUnit<dim::LuminousI, std::ratio<1>>;
using Mole       = LinearUnit<dim::Amount, std::ratio<1>>;

// SI prefixes (EXACT integer ratios)
using Kilometer  = LinearUnit<dim::Length, std::ratio<1000>>;
using Centimeter = LinearUnit<dim::Length, std::ratio<1, 100>>;
using Millimeter = LinearUnit<dim::Length, std::ratio<1, 1000>>;
using Micrometer = LinearUnit<dim::Length, std::ratio<1, 1'000'000>>;
using Nanometer  = LinearUnit<dim::Length, std::ratio<1, 1'000'000'000>>;
using Angstrom   = LinearUnit<dim::Length, std::ratio<1, 10'000'000'000>>;  // chemistry / future material

// Imperial (EXACT per 1959 international agreement)
using Inch         = LinearUnit<dim::Length, std::ratio<254, 10000>>;          // 0.0254 m exact
using Foot         = LinearUnit<dim::Length, std::ratio<3048, 10000>>;         // 0.3048 m exact
using Yard         = LinearUnit<dim::Length, std::ratio<9144, 10000>>;         // 0.9144 m exact
using Mile         = LinearUnit<dim::Length, std::ratio<1609344, 1000>>;       // 1609.344 m exact
using NauticalMile = LinearUnit<dim::Length, std::ratio<1852>>;                // 1852 m exact
using Mil          = LinearUnit<dim::Length, std::ratio<254, 10'000'000>>;     // 25.4 μm exact (PCB consumer)
using Pound        = LinearUnit<dim::Mass,   std::ratio<45359237, 100'000'000>>;  // 0.45359237 kg exact
using Ounce        = LinearUnit<dim::Mass,   std::ratio<45359237, 1'600'000'000>>;  // 1/16 lb exact

// Time
using Minute     = LinearUnit<dim::Time, std::ratio<60>>;
using Hour       = LinearUnit<dim::Time, std::ratio<3600>>;
using Day        = LinearUnit<dim::Time, std::ratio<86400>>;   // solar day
using Week       = LinearUnit<dim::Time, std::ratio<604800>>;

// Angle
using Degree     = LinearUnit<dim::Angle, /* π/180 — irrational, stored as constexpr f64 with f64-best literal */>;
using Grad       = LinearUnit<dim::Angle, /* π/200 */>;
using Revolution = LinearUnit<dim::Angle, /* 2π */>;
using ArcMinute  = LinearUnit<dim::Angle, /* π/10800 */>;
using ArcSecond  = LinearUnit<dim::Angle, /* π/648000 */>;
```

**Layer-1 conversion** = one FP multiply at the boundary. SI-prefix
round-trips are bit-exact because the multiplier is an exact integer
power-of-10. Imperial round-trips through `f64` are exact at the
rational arithmetic level; `f32` round-trips are within 1 ULP.

### Layer 2 — Affine units (the absolute-vs-delta trap)

Temperature has both scale and offset (K = C + 273.15). Naive libraries
treat affine units as linear, then `C_a − C_b` returns the wrong
K-delta. Cerid nails this at compile time with **two distinct types**:

```cpp
template <typename Dim, typename ScaleRatio, typename OffsetRatio>
struct AffineUnit
{
    using dimension = Dim;
    static constexpr f64 scale  = ratio_to_f64<ScaleRatio>();
    static constexpr f64 offset = ratio_to_f64<OffsetRatio>();
    static constexpr bool is_affine = true;
};

using Celsius    = AffineUnit<dim::Temperature, std::ratio<1>,    std::ratio<27315, 100>>;
using Fahrenheit = AffineUnit<dim::Temperature, std::ratio<5, 9>, std::ratio<45967, 180>>;
using Rankine    = AffineUnit<dim::Temperature, std::ratio<5, 9>, std::ratio<0>>;

// Two distinct types — the elite move:
template <typename T> using Temperature      = Quantity<dim::Temperature,       T>;  // absolute (always K internally)
template <typename T> using TemperatureDelta = Quantity<dim::Temperature_Delta, T>;  // relative
// (dim::Temperature_Delta is the same exponent vector but carries a "delta" marker so
//  arithmetic rules distinguish absolute vs relative — same SI base, different operator set.)

// Compile-checked arithmetic:
auto a    = 100.0_celsius;        // Temperature{373.15 K}
auto b    = 25.0_celsius;         // Temperature{298.15 K}
auto diff = a - b;                 // TemperatureDelta{75.0}     ← subtraction strips offset
auto c    = b + diff;              // Temperature{373.15 K}      ← abs + delta = abs
auto d    = diff + diff;           // TemperatureDelta{150.0}    ← delta + delta = delta
auto e    = diff * 2.0f;           // TemperatureDelta{150.0}    ← scalar mul on delta OK
auto in_F = a.value_in<Fahrenheit>();  // 212.0   ← absolute-to-absolute uses offset
auto dF   = diff.value_in<Fahrenheit_Delta>();  // 135.0  ← delta-to-delta does NOT use offset

// Compile errors:
auto bad1 = a + b;            // ERROR: cannot add two absolute temperatures (no meaning)
auto bad2 = a + 5.0f;         // ERROR: cannot add bare scalar to temperature (use _kelvin literal)
auto bad3 = a * 2.0f;         // ERROR: cannot scale an absolute temperature (only delta)
auto bad4 = -a;               // ERROR: cannot negate an absolute temperature
```

The same `absolute vs delta` pattern is *reserved* for:
- **Datetime vs Duration** — if/when Cerid grows a calendar/datetime module (currently `Time` is pure duration).
- **Gauge vs Absolute Pressure** — for weather / aerospace cabin / tire / CFD inlet. `Pressure` (absolute) vs `PressureDelta` (gauge). Opted in when a consumer needs it.
- **Voltage vs Voltage-Difference** — usually not needed (potential differences are what we measure), pinned reserved.

### Layer 3 — Non-linear units (logarithmic, perceptual)

dB / cents / semitones / stellar magnitude / Richter / pH can't be
represented as scale + offset. They need explicit functions:

```cpp
template <typename Dim, auto ToSi, auto FromSi>
struct NonLinearUnit
{
    using dimension = Dim;
    static constexpr bool is_nonlinear = true;
    template <typename T> static constexpr T to_si(T u)   { return ToSi(u); }
    template <typename T> static constexpr T from_si(T s) { return FromSi(s); }
};

// Audio family (consumer: Phase 3.4 audio + cinematic)
using DecibelSPL = NonLinearUnit<dim::Pressure,
    [](f64 db) { return 20e-6 * std::pow(10.0, db / 20.0); },
    [](f64 pa) { return 20.0 * std::log10(pa / 20e-6); }>;
using DecibelV   = NonLinearUnit<dim::Voltage, [](f64 db){ return std::pow(10.0, db/20.0); }, /*…*/>;
using DecibelW   = NonLinearUnit<dim::Power,   [](f64 db){ return std::pow(10.0, db/10.0); }, /*…*/>;
using DecibelA   = NonLinearUnit<dim::Pressure, /* A-weighted SPL — same conversion as dB SPL; the weighting is a separate signal-processing concern */>;
using Dbu        = NonLinearUnit<dim::Voltage, /* 0 dBu = 0.7746 V */>;

// Musical pitch (consumer: cinematic + audio)
using Cents      = NonLinearUnit<dim::Frequency, /* 1200·log2(f₁/f₀) */>;
using Semitones  = NonLinearUnit<dim::Frequency, /* 12·log2(f₁/f₀) */>;

// Astronomical (consumer: future eylem-aero + sciviz)
using StellarMagnitude = NonLinearUnit<dim::Luminance, /* m = -2.5·log10(F/F₀) */>;

// RF / electrical (consumer: future crd-eda)
using DbMilliWatt = NonLinearUnit<dim::Power, /* 0 dBm = 1 mW */>;

// Material / chemistry (future cfd combustion / fea material)
using PH         = NonLinearUnit<dim::Concentration, /* -log10([H⁺]) */>;
```

**Critical pin: non-linear units do NOT support direct arithmetic.**
`DecibelSPL(20) + DecibelSPL(20) ≠ DecibelSPL(40)` (it's actually +3 dB
for incoherent summation, +6 dB for coherent — power doubling).
Compile-time rejection: a `Quantity` constructed via non-linear ingress
carries a runtime-checkable marker, and `operator+` on two such
quantities is disabled. Callers must convert to linear, compute, convert back:

```cpp
auto p1 = 80.0_dB_spl;    // → Pressure{0.2 Pa} internally
auto p2 = 80.0_dB_spl;    // → Pressure{0.2 Pa} internally
auto sum = p1 + p2;        // ALLOWED — Pressure addition in linear (SI) domain
f64 sum_db = sum.value_in<DecibelSPL>();  // 86.02 dB (correct — power doubled)
```

For Cerid v0a ships **dB-family** (SPL / V / W / A-weighted / u / m)
and **cents / semitones** (audio + cinematic — both first-class
domains). Stellar magnitude / pH / Richter land via the same framework
when a consumer asks (eylem-aero / future chemistry / future seismology).

### Layer 4 — Compound derived units (auto-derive from dimension)

The most elegant part. Once `Mile` and `Hour` exist, `MilePerHour`
falls out — dimension from the dimensional decomposition, factor from
`std::ratio` arithmetic, all at compile time:

```cpp
template <typename Num, typename Den>
struct UnitDiv
{
    using dimension    = DimDiv<typename Num::dimension, typename Den::dimension>;
    using factor_ratio = std::ratio_divide<typename Num::factor_ratio, typename Den::factor_ratio>;
    static constexpr f64 factor = ratio_to_f64<factor_ratio>();
};

template <typename A, typename B>
struct UnitMul
{
    using dimension    = DimMul<typename A::dimension, typename B::dimension>;
    using factor_ratio = std::ratio_multiply<typename A::factor_ratio, typename B::factor_ratio>;
    static constexpr f64 factor = ratio_to_f64<factor_ratio>();
};

// Velocity (auto-derived; factor = std::ratio arithmetic at compile time)
using MeterPerSecond     = UnitDiv<Meter, Second>;
using KilometerPerHour   = UnitDiv<Kilometer, Hour>;
using MilePerHour        = UnitDiv<Mile, Hour>;
using FootPerSecond      = UnitDiv<Foot, Second>;
using Knot               = UnitDiv<NauticalMile, Hour>;
using MillimeterPerSec   = UnitDiv<Millimeter, Second>;

// Acceleration
using MeterPerSecondSq   = UnitDiv<MeterPerSecond, Second>;
using StandardG          = LinearUnit<dim::Acceleration, std::ratio<980665, 100000>>;  // 9.80665 EXACT

// Force, pressure, torque, density, energy, power — all auto-derive
using Newton                = UnitMul<Kilogram, MeterPerSecondSq>;
using NewtonMeter           = UnitMul<Newton, Meter>;
using Pascal                = UnitDiv<Newton, UnitMul<Meter, Meter>>;
using PoundForcePerSqInch   = UnitDiv<PoundForce, UnitMul<Inch, Inch>>;     // psi — exact rational
using KilogramPerCubicMeter = UnitDiv<Kilogram, UnitMul<UnitMul<Meter, Meter>, Meter>>;
using Joule                 = UnitMul<Newton, Meter>;
using Watt                  = UnitDiv<Joule, Second>;
using Volt                  = UnitDiv<Watt, Ampere>;
using Ohm                   = UnitDiv<Volt, Ampere>;
using Farad                 = UnitDiv<Coulomb, Volt>;
using Henry                 = UnitDiv<UnitMul<Volt, Second>, Ampere>;
using Hertz                 = LinearUnit<dim::Frequency, std::ratio<1>>;
using RevolutionsPerMinute  = UnitDiv<Revolution, Minute>;

// Auto-derived conversion just works:
auto v = 60.0_mph;
f64 in_kmh   = v.value_in<KilometerPerHour>();    // 96.5606  — exact rational
f64 in_knots = v.value_in<Knot>();                 // 52.137...
f64 in_fps   = v.value_in<FootPerSecond>();        // 88.0     — exact integer
```

**The user never enumerates "all velocity units" or "all pressure
units".** Any (length, time) pair gives a velocity unit; any (force,
area) gives a pressure unit. Compile-time `std::ratio` arithmetic
produces exact factors. **Adding one new base unit unlocks N new
compound units automatically** — this is the extensibility multiplier
that makes the framework scale to PCB / aerospace / CAM / chemistry /
audio / cinematic / etc.

### Layer 5 — Domain extensibility (federated registration)

Domain modules register their own units in their own namespace
**without touching `crd-units` core**. This is the keystone of the
"easy to extend" requirement — every future module brings its unit
catalogue.

```cpp
// crd-eylem-aero (ADR-0073 reserved)
namespace crd::eylem_aero::units
{
    using AstronomicalUnit = LinearUnit<dim::Length, /* 1.49597870700e11 m — exact per IAU 2012 */>;
    using LightYear        = LinearUnit<dim::Length, std::ratio<9460730472580800, 1>>;  // EXACT (defined)
    using Parsec           = LinearUnit<dim::Length, /* ~3.0857e16 m */>;
    using EarthRadius      = LinearUnit<dim::Length, std::ratio<6378137>>;              // WGS84 equatorial EXACT
    using SolarRadius      = LinearUnit<dim::Length, /* 6.957e8 m */>;
    using SolarMass        = LinearUnit<dim::Mass,   /* 1.98892e30 kg */>;
    using EarthMass        = LinearUnit<dim::Mass,   /* 5.972e24 kg */>;
    using JulianYear       = LinearUnit<dim::Time,   std::ratio<31557600>>;             // 365.25 d EXACT
    using SiderealDay      = LinearUnit<dim::Time,   /* 86164.0905 s */>;
    using TropicalYear     = LinearUnit<dim::Time,   /* 31556925.216 s */>;
    using StandardG        = LinearUnit<dim::Acceleration, std::ratio<980665, 100000>>; // EXACT
    using SpeedOfLight     = LinearUnit<dim::Velocity, std::ratio<299792458>>;          // EXACT (defining constant)
    using AU_per_day       = UnitDiv<AstronomicalUnit, Day>;                            // ephemeris velocity

    inline namespace literals
    {
        constexpr auto operator""_au(long double v) { return Length<f64>{v * AstronomicalUnit::factor}; }
        constexpr auto operator""_ly(long double v) { return Length<f64>{v * LightYear::factor}; }
        constexpr auto operator""_pc(long double v) { return Length<f64>{v * Parsec::factor}; }
        constexpr auto operator""_G(long double v)  { return Acceleration<f32>{f32(v * StandardG::factor)}; }
    }
}

// crd-eda (Phase 3.1.17 reserved)
namespace crd::eda::units
{
    using Mil              = LinearUnit<dim::Length, std::ratio<254, 10'000'000>>;     // 25.4 μm EXACT
    using OhmCm            = UnitMul<Ohm, Centimeter>;                                 // resistivity
    using SiemensPerMeter  = UnitDiv<Siemens, Meter>;                                  // conductivity
    using AmpHour          = UnitMul<Ampere, Hour>;                                    // battery capacity
    using MilliAmpHour     = UnitMul<MilliAmpere, Hour>;
    using WattHour         = UnitMul<Watt, Hour>;                                      // energy storage
    using DbMilliWatt      = NonLinearUnit<dim::Power, /* dBm — 0 dBm = 1 mW */>;       // RF power
    using DbMicroVolt      = NonLinearUnit<dim::Voltage, /* dBμV */>;                   // RF reception
}

// crd-cam (Phase 3.1.13 reserved)
namespace crd::cam::units
{
    using InchPerMinute    = UnitDiv<Inch, Minute>;          // CNC feed rate (US convention)
    using MillimeterPerMin = UnitDiv<Millimeter, Minute>;    // CNC feed rate (metric convention)
    using RPM              = UnitDiv<Revolution, Minute>;
    using InchPerRev       = UnitDiv<Inch, Revolution>;      // chip load
    using SurfaceFootPerMin = UnitDiv<Foot, Minute>;          // SFM (US cutting speed)
    using MeterPerMin      = UnitDiv<Meter, Minute>;
    using CubicInchPerMin  = UnitDiv<UnitMul<UnitMul<Inch,Inch>,Inch>, Minute>;  // MRR (material removal rate)
}

// crd-eylem-cine (ADR-0074 reserved)
namespace crd::eylem_cine::units
{
    using Frame_24fps      = LinearUnit<dim::Time, std::ratio<1, 24>>;
    using Frame_25fps      = LinearUnit<dim::Time, std::ratio<1, 25>>;
    using Frame_30fps      = LinearUnit<dim::Time, std::ratio<1, 30>>;
    using Frame_48fps      = LinearUnit<dim::Time, std::ratio<1, 48>>;
    using Frame_60fps      = LinearUnit<dim::Time, std::ratio<1, 60>>;
    using Frame_120fps     = LinearUnit<dim::Time, std::ratio<1, 120>>;
    // SMPTE timecode hh:mm:ss:ff is a format/parse concern (Layer 6), not a unit per se.
}

// Future crd-material (when chemistry / fea-material lands)
namespace crd::material::units
{
    using Pascal_Second    = UnitMul<Pascal, Second>;          // dynamic viscosity
    using StokesCgs        = LinearUnit<dim::KinematicVisc, /* 1e-4 m²/s — CGS */>;
    using Centipoise       = LinearUnit<dim::DynamicVisc, /* 1e-3 Pa·s */>;
    using JoulePerKgKelvin = UnitDiv<UnitDiv<Joule, Kilogram>, Kelvin>;     // specific heat capacity
    using WattPerMeterKelvin = UnitDiv<UnitDiv<Watt, Meter>, Kelvin>;       // thermal conductivity
    using YoungsModulus    = Pascal;                                        // (already exists as derived)
}
```

**`crd-units` core ships the framework + ~120 common units** (SI base,
SI prefixed, imperial, time, angle, force, pressure, energy, power,
electrical, photometric, thermodynamic, audio). Each domain module
adds its own ~10–30 specialised units. **No central registry, no
plugin system, no runtime dispatch.** Pure C++ namespace + ADL +
`std::ratio` compile-time arithmetic.

**The cost of adding a new unit:** one `LinearUnit<>` / `AffineUnit<>` /
`NonLinearUnit<>` declaration + optionally one UDL operator. Maybe 5
lines. Compound derivations come free.

### Layer 6 — Format + parse + user preference (UI / I/O surface)

```cpp
// Format — every Quantity has a runtime-tag overload
String format_quantity(Length<f32> l, UnitTag unit, FormatOptions opts = {});
//   format_quantity(box_size, Millimeter::tag, {.precision = 3, .mode = Compact})
//   → "25.400 mm"
//   format_quantity(orbit_radius, AstronomicalUnit::tag, {.precision = 2, .mode = Scientific})
//   → "1.50e+00 AU"
//   format_quantity(temperature, Fahrenheit::tag, {.precision = 1})
//   → "212.0 °F"

// Parse — error-as-value (Result<T>); never throws
Result<Length<f32>> parse_length(StringView s);
//   parse_length("25.4 mm")     → Length<f32>{0.0254}
//   parse_length("1.0 in")      → Length<f32>{0.0254}
//   parse_length("3'6\"")       → Length<f32>{1.0668}    (imperial mixed)
//   parse_length("2.5e-3 m")    → Length<f32>{0.0025}
//   parse_length("invalid")     → Result::error
Result<Pressure<f32>>    parse_pressure(StringView s);     // "1 atm" / "101325 Pa" / "14.7 psi"
Result<Angle<f32>>       parse_angle(StringView s);        // "45 deg" / "π/4 rad" / "1.57 rad"
Result<Temperature<f32>> parse_temperature(StringView s);  // "25 °C" / "298.15 K" / "77 °F"

// User-preference table — per-document, per-discipline preset.
// Stored runtime tags (so UI can switch at runtime); static template form
// (.value_in<Millimeter>()) remains available where target is compile-time-known.
struct UnitPreferences
{
    UnitTag length;      // mm / m / km / in / ft / μm / nm / AU
    UnitTag angle;       // deg / rad / grad
    UnitTag mass;        // kg / g / mg / lb / oz
    UnitTag force;       // N / kN / lbf / kgf
    UnitTag pressure;    // Pa / kPa / MPa / psi / bar / atm / mmHg
    UnitTag temperature; // K / °C / °F
    UnitTag time;        // s / ms / μs / min / h
    UnitTag frequency;   // Hz / kHz / MHz / rpm
    UnitTag velocity;    // m/s / km/h / mph / knot / ft/s
    UnitTag voltage;     // V / mV / kV
    UnitTag current;     // A / mA / μA
    UnitTag energy;      // J / kJ / kWh / Btu / cal
    UnitTag power;       // W / kW / hp
    // ...
};

// Discipline presets shipped with crd-units (extensible by domain modules):
constexpr UnitPreferences k_game_default      = { Meter,      Degree, Kilogram, Newton, Pascal,     Celsius,    Second,      Hertz,      MeterPerSecond,    Volt, Ampere,     Joule,    Watt };
constexpr UnitPreferences k_cad_default       = { Millimeter, Degree, Kilogram, Newton, MegaPascal, Celsius,    Second,      Hertz,      MeterPerSecond,    Volt, Ampere,     Joule,    Watt };
constexpr UnitPreferences k_robotics_default  = { Meter,      Radian, Kilogram, Newton, Pascal,     Celsius,    Second,      Hertz,      MeterPerSecond,    Volt, Ampere,     Joule,    Watt };
constexpr UnitPreferences k_aerospace_default = { Kilometer,  Degree, Kilogram, Newton, Pascal,     Kelvin,     Second,      Hertz,      MeterPerSecond,    Volt, Ampere,     Joule,    Watt };
constexpr UnitPreferences k_pcb_default       = { Millimeter, Degree, Gram,     Newton, Pascal,     Celsius,    Second,      MegaHertz,  MeterPerSecond,    Volt, MilliAmpere,Joule,    Watt };
constexpr UnitPreferences k_audio_default     = { Meter,      Degree, Kilogram, Newton, DecibelSPL, Celsius,    Millisecond, Hertz,      MeterPerSecond,    Volt, MilliAmpere,Joule,    Watt };
constexpr UnitPreferences k_3d_print_default  = { Millimeter, Degree, Gram,     Newton, Pascal,     Celsius,    Second,      Hertz,      MillimeterPerSec,  Volt, Ampere,     Joule,    Watt };
constexpr UnitPreferences k_cam_default       = { Inch,       Degree, Pound,    PoundForce, PSI,    Fahrenheit, Second,      RPM,        InchPerMinute,     Volt, Ampere,     Joule,    Horsepower };
constexpr UnitPreferences k_cinematic_default = { Meter,      Degree, Kilogram, Newton, Pascal,     Celsius,    Frame_24fps, Hertz,      MeterPerSecond,    Volt, Ampere,     Joule,    Watt };
constexpr UnitPreferences k_imperial_default  = { Inch,       Degree, Pound,    PoundForce, PSI,    Fahrenheit, Second,      Hertz,      MilePerHour,       Volt, Ampere,     Btu,      Horsepower };
constexpr UnitPreferences k_si_strict_default = { Meter,      Radian, Kilogram, Newton, Pascal,     Kelvin,     Second,      Hertz,      MeterPerSecond,    Volt, Ampere,     Joule,    Watt };
constexpr UnitPreferences k_scientific_default = { Meter,     Radian, Kilogram, Newton, Pascal,     Kelvin,     Second,      Hertz,      MeterPerSecond,    Volt, Ampere,     Joule,    Watt };
```

**ImGui inspector** reads via `.value_in(prefs.length)` — the runtime
tag form (a value, not a template parameter) because the user picks at
runtime. The compile-time form `.value_in<Millimeter>()` stays
available for callers that know the target at compile time
(performance-sensitive read-back loops). Both go through the same
conversion math; the runtime form is a small dispatch table.

**CRDR scene format** carries the user's preference plus the raw SI
value. Opening the same scene in a different discipline preset
auto-displays in the new unit. Internal storage NEVER changes —
switching discipline is a UI re-format, not a data conversion.

**File format readers** carry source-format unit tags (glTF
`KHR_unit`, STEP `SI_UNIT`, IGES `GLOBAL` section, FBX `UnitScaleFactor`,
IFC `IFCSIUNIT`, Gerber `%MOIN*%` / `%MOMM*%`) and emit Cerid
quantities via the standard conversion path. **The asset cooker
normalises to SI at cook time, never at load.**

### Performance characteristics

| Layer | Conversion cost | Notes |
|---|---|---|
| 1 Linear | 1 FP multiply at the boundary | Rational factor → `static constexpr f64`; SI-prefix + standardised-imperial round-trips bit-exact |
| 2 Affine | 1 multiply + 1 add at the boundary | `value_in` inlines |
| 3 Non-linear | 1 `pow` or `log10` at the boundary | Boundary-only — never on a hot path; ~30 ns per call |
| 4 Compound | 0 — decays to Layer 1 | `std::ratio_multiply` / `std::ratio_divide` at *compile time* → single rational factor; identical to a hand-written Linear unit |
| 5 Domain-extended | 0 — decays to whichever layer the unit declares | Same pathway as built-in units |
| 6 Format / parse | ~10–50 ns format, ~50–200 ns parse | One per UI interaction — never hot |

**`Quantity` arithmetic is identical to bare-scalar in codegen:**
`(a + b).value` for `Quantity<D, f32>` produces the same MSVC / GCC /
clang output as `a + b` for `f32`. The wrapper is bit-equal layout —
single member, zero-cost, `is_standard_layout_v` + `is_trivially_copyable_v`
pinned. Verified by codegen-equivalence test in v0a (compare
`objdump -d` of two TUs: one with `Quantity`, one with bare scalar).

**`Vec3<Quantity>` is bit-equal to `Vec3<f32>`** — SIMD kernels reinterpret
the column at the access boundary; no scalar conversion, no copy.

### Extensibility — what makes the design grow gracefully

1. **Add a new base unit** — one `LinearUnit<dim, ratio>` declaration. Everything Layer 4 derives unlocks N new compound units automatically. *Cost: ~1 line.*
2. **Add a new dimension** — extend `Dim<L,M,T,I,Th,N,J,A>` to add the new exponent; all existing code recompiles unchanged (added exponent defaults to 0). E.g., if Cerid grows a "radioactivity" or "data-storage" dimension, add it as the 9th exponent without breaking anything. *Cost: ~3 lines in the core `Dim` type + 1 new dimension alias.*
3. **Add a new conversion class** — new `*Unit` template alongside `LinearUnit` / `AffineUnit` / `NonLinearUnit`. Cerid's example: future `DiscreteUnit` for inherently quantized things (frame counts at fixed fps, sample counts at fixed sample rate). *Cost: ~50 lines + tests; the framework's pluggability survives.*
4. **Add a domain pack** — new namespace `crd::your_module::units` with unit aliases + UDLs. No central registration; ADL handles lookup. *Cost: a header file with ~20–30 lines per ~10 units, no `crd-units` changes.*
5. **Add a user preference profile** — one `constexpr UnitPreferences` constant. Becomes a `crd-config` discipline selector. *Cost: ~15 lines.*
6. **Cross-engine import** — new file format reader reads the source's unit tag, picks the matching `LinearUnit` / `AffineUnit`, calls `Quantity::from(value, source_unit)`. Asset cooker normalises to SI at cook time. *Cost: one converter table per format (~30 lines).*

### Frame transforms are NOT unit conversions (critical pin)

Coordinate-system / reference-frame transforms (ENU vs NED vs ECEF in
geodetic / robotics; body vs inertial frame in physics; world vs local
vs view vs clip in rendering; cartesian vs polar vs spherical) are
**geometric transforms**, not unit conversions. The dimension stays
`Length`; only the basis changes.

These belong to `crd-math::Transform` + `crd-geometry-primitives::transform_aabb`
(Phase 3.1.7 v11), NOT to `crd-units`. Naive libraries fuse the two
(`Position<ENU, Length>` template explosions, ambiguous semantics).

**Cerid keeps them strictly orthogonal:**
- `crd-units` handles units (Length-in-meters ↔ Length-in-millimeters).
- `crd-math` / `crd-geometry` handle frames (Length-in-frame-A ↔ Length-in-frame-B).

A future `crd-coordinates` substrate may eventually own the frame
graph (robotics `tf2`-equivalent, aerospace SOFA frames, rendering
view-stack), but that's a separate substrate decision — explicitly
NOT in `crd-units` scope.

## Slice list — 4 slices over ~4.5 weeks

| Slice | Scope | LOC | Calendar |
|---|---|---|---|
| **v0a** | **`crd-units` substrate + full 6-layer conversion system.** New module `engine/units/` (target `crd-units`, ns `crd::units`, deps `crd-core` only). `Quantity<D, T>` zero-overhead wrapper + `Dim<L, M, T, I, Th, N, J, A>` 8-exponent compile-time tag + `DimMul`/`DimDiv`/`DimInv`/`DimPow`/`DimRoot` template arithmetic. **Layer 1: `LinearUnit<Dim, std::ratio>`** + ~120 named units (SI base + SI prefixes + imperial + time + angle + force + pressure + energy + power + electrical + photometric + thermodynamic + audio). **Layer 2: `AffineUnit<Dim, Scale, Offset>`** + distinct `Temperature` / `TemperatureDelta` types with absolute-vs-delta operator rules (compile-time `a + b` rejection for absolute pairs). **Layer 3: `NonLinearUnit<Dim, ToSi, FromSi>`** + dB family (SPL / V / W / A / u / m) + cents / semitones + arithmetic-disabled marker (compile-time block on `dB + dB`). **Layer 4: `UnitMul<A,B>` / `UnitDiv<Num,Den>`** compound auto-derivation via `std::ratio_multiply` / `std::ratio_divide` (factor + dimension both fall out from decomposition — adding one base unit unlocks N compound units automatically; this is the extensibility multiplier). **Layer 5: federated domain registration** — namespace `crd::your_module::units` adds aliases + UDLs via ADL, no central registry; v0a ships the framework + ~120 common units, domain modules add their own ~10–30 each (`crd-eylem-aero` AU/ly/parsec/SolarMass/EarthRadius/JulianYear, `crd-eda` Mil/OhmCm/AmpHour, `crd-cam` RPM/IPM/SFM, `crd-eylem-cine` Frame_24/25/30/48/60/120fps, etc.). **Layer 6: format + parse + UnitPreferences** ships in v0d (UI / I/O surface). ~120 UDLs across length / mass / time / angle / force / pressure / energy / power / velocity / acceleration / torque / frequency / voltage / current / resistance / capacitance / inductance / temperature / photometric. `.value_in<TargetUnit>()` (compile-time) + `.value_in(UnitTag)` (runtime — dispatch table, for UI). `Vec2/Vec3/Vec4/Mat3/Mat4<Quantity>` wrappers (layout-equal to `Vec/Mat<T>` — CI-pinned by `static_assert(sizeof(Vec3<Length<f32>>) == sizeof(Vec3<f32>))`; SIMD-reinterpretable). New CI guard `crd-no-untagged-physical-numeric` (grep-based, scoped to `engine/` minus `crd-math/src/simd/` + `crd-rhi-vulkan/`, flags bare-`f32`/`f64` field names matching `length` / `position` / `velocity` / `mass` / `force` / `torque` / `pressure` / `energy` / `power` / `temperature` / `voltage` / `current` / `frequency` / `angle` / `duration` / etc.). Tests: compile-time dimension-mismatch rejection (negative-compile suite via Catch2 `STATIC_REQUIRE` + `consteval`-error harness), runtime numeric round-trip across precision (`f32` ↔ `f64` explicit), UDL conversion exactness, dimensional arithmetic correctness, Vec/Mat compose preserves dimension, **rational round-trip property tests** (every SI-prefix + standardised-imperial conversion is `static_assert`-exact), `TemperatureDelta` semantics (subtraction strips offset; add-delta-to-absolute preserves; abs+abs rejected), non-linear arithmetic rejection (compile-time block on `dB + dB`), compound auto-derive equivalence (`UnitDiv<Mile, Hour>` factor == hand-written `LinearUnit<Velocity, mile_per_hour_ratio>` factor), layout-equality, `std::is_standard_layout_v` + `std::is_trivially_copyable_v` pins, **codegen-equivalence** (compare `objdump -d` of a TU using `Quantity<L, f32>` vs bare `f32` arithmetic — must be identical). System doc `docs/systems/units.md` ships at v0a close. ADR-0078 minted. | ~1.6 KLOC + ~1.0 KLOC tests | ~7 days |
| **v0b** | **Adoption pass A — foundation modules.** `crd-config` TOML reader gains unit-tagged-key support: `position_m = [0, 1, 0]` / `length_mm = 25.4` / `angle_deg = 90.0` / `mass_kg = 5.0` / `force_N = 100.0` / `temperature_celsius = 25.0` / `voltage_V = 3.3` — reader produces the dimensional type, untagged numeric for a physical-quantity field is an error (config-time, with file:line). Bare numbers stay legal for genuinely-dimensionless fields (`restitution = 0.8`, `friction = 0.5`, RGBA colour components, integer counts). `crd-scene Transform` API change: `Vec3<Length<f32>>` position + `Quat<f32>` rotation + `Vec3<f32>` scale (scale stays dimensionless). The transform-propagation kernel (v1m, bit-exact deterministic) stays bit-identical at the math level — only the access API surface gains typing. glTF cooker normalizes lengths to SI at cook time (glTF spec: meters by default — confirm + assert per-asset). `crd-scene` SceneResource / Öbek / Preset / Profile cookers add unit-tag pass at cook time + a `units_normalized: true` flag in CRDR headers. CRDR chunk format gains an optional `'UNIT'` chunk for legacy assets (warn if missing — every new asset is SI-tagged). New `tests/units/test_config_io.cpp` + `tests/scene/test_transform_units.cpp` (existing transform-propagation tests pass unchanged after the API migration). | ~600 LOC + ~400 tests | ~5 days |
| **v0c** | **Adoption pass B — `crd-eylem`.** `RigidBody`: `Vec3<Length<f32>> position` + `Quat<f32> orientation` + `Vec3<Velocity<f32>> linear_velocity` + `Vec3<AngularVelocity<f32>> angular_velocity` + `Mass<f32> mass` + `Mat3<MomentOfInertia<f32>> inertia_tensor` + `Vec3<Force<f32>> accumulated_force` + `Vec3<Torque<f32>> accumulated_torque` + `f32 restitution` (dimensionless) + `f32 friction` (dimensionless). Integrator (semi-implicit Euler + RK4 sketch + the eylem deterministic step) becomes self-verifying via the dimensional arithmetic — `Velocity += Acceleration * Time`, `Length += Velocity * Time`, `Force = Mass * Acceleration`, `Torque = r × Force`, `AngularMomentum = MomentOfInertia · AngularVelocity` — every line type-checks. The 9 force-field formulas (v1b force-field substrate) audited + ported to dimensional types (gravity → `Vec3<Acceleration>`, spring → `Force = -k·Length`, damper → `Force = -c·Velocity`, drag → `Force ~ Velocity²`, magnetic → `Force = q·v×B`, etc.). Contact: penetration `Length`, normal `Vec3<f32>` (dimensionless unit-vector), impulse `Vec3<Quantity<DimMul<Force, Time>>>`. Constraint solver: Jacobian rows carry mixed dimensions — that's where the compile-time check pays the highest dividend (Featherstone v6 articulations will *not* compile with a Jacobian-row dimension mismatch). Eylem v1c broadphase consumes `crd-geometry`'s `Vec3<Length>` AABBs from day 1 (which means `crd-geometry-primitives`' AABB3 / OBB / Sphere / Triangle3 / etc. become `Vec3<Length>`-parametrised at the API surface — that's a Phase 3.1.7-side adoption inside v0c, scope-pinned: only the public-API surface changes, internals stay raw `f32`). Tests: deterministic-replay across 9-config CI matrix preserved (the dimensional types add zero runtime variability). | ~800 LOC + ~500 tests | ~5 days |
| **v0d** | **Adoption pass C — `crd-renderer`, `crd-resources` cookers, ImGui, Layer-6 format+parse+preferences, CI sweep close.** `crd-renderer`: uniform-upload boundary identified — Mat4 stays raw `Mat4f` at the GPU upload point (`vkCmdPushConstants` / uniform-buffer write); everything upstream (Camera position, light positions, frustum corners, mesh AABBs) is typed. `crd-resources` cookers (texture / mesh / material / scene / öbek / preset / profile): every loader's POST-load pass normalizes to SI; existing CRDR header gets a `units_normalized: true` flag. **Layer 6 ships here**: `format_quantity(q, UnitTag, FormatOptions)` for every base + derived dimension; `parse_*(StringView) → Result<Quantity>` family (length / mass / pressure / angle / temperature / time / frequency / velocity / voltage / current) with imperial-mixed support (`3'6"` → Length), scientific notation, π-literal parsing in angles, error-as-value never-throws contract; `UnitPreferences` struct + 11 discipline presets (`k_game_default` / `k_cad_default` / `k_robotics_default` / `k_aerospace_default` / `k_pcb_default` / `k_audio_default` / `k_3d_print_default` / `k_cam_default` / `k_cinematic_default` / `k_imperial_default` / `k_si_strict_default`); `crd-config` gains a per-document `unit_preferences` key with discipline-name resolution. `crd-imgui` debug inspector: every read-back uses `.value_in(prefs.length)` (runtime form) — user-preferred unit is per-document + per-discipline, stored in `crd-config`. CRDR scene format carries the preference + the raw SI value (opening a scene in a different discipline preset re-formats on the fly; bytes-on-disk unchanged). Cross-engine file-format readers (glTF `KHR_unit`, STEP `SI_UNIT`, IGES `GLOBAL`, FBX `UnitScaleFactor`, IFC `IFCSIUNIT`, Gerber `%MOIN*%`/`%MOMM*%`) gain unit-tag plumbing in their respective cookers (those that exist today; future readers follow the pattern). `crd-no-untagged-physical-numeric` CI guard scope finalised across the codebase. **Full 17-config `scripts/full-sweep.ps1` PASS.** ADR-0078 minted at v0a close; v0d lands the PRINCIPLES.md cornerstone migration (Principles → Architectural Cornerstones with ADR-0078 reference). System doc `docs/systems/units.md` finalised with the full type catalogue + UDL list + per-discipline preset list + adoption checklist for future modules. Phase 3.1.7.5 CLOSED. | ~700 LOC + ~550 tests | ~6 days |

**Total:** ~3.5 KLOC engine + ~2.4 KLOC tests + 1 ADR + system doc, **~4.5 weeks calendar** (v0a expanded 2026-05-14 from 1.0 → 1.6 KLOC engine / 5 → 7 days to absorb the full 6-layer conversion system; v0d expanded 500 → 700 LOC / 5 → 6 days to absorb Layer 6 format/parse/UnitPreferences + cross-engine format readers).

## Consumer mapping

Every Cerid module that touches a physical quantity consumes
`crd-units`. The adoption pass is one-shot at v0b/c/d; future modules
adopt by default.

| Module | Dimensional types consumed | Adoption slice |
|---|---|---|
| `crd-config` | unit-tagged TOML keys (`length_mm`, `mass_kg`, …) | v0b |
| `crd-scene` (Transform, SceneResource, Öbek/Preset/Profile cookers) | `Position3` + `Vec3<f32>` scale + Angle for rotation-via-axis-angle | v0b |
| `crd-resources` (texture/mesh/material/scene/öbek cookers) | unit normalization at cook time | v0d |
| `crd-eylem` (RigidBody + integrator + force fields + contact + constraints + broadphase + narrowphase) | full dimensional throughout (Length/Mass/Time/Velocity/Acceleration/Force/Torque/AngularVelocity/MomentOfInertia/Pressure/Energy/Power) | v0c |
| `crd-geometry-primitives` (AABB3 / OBB / Sphere / Triangle3 / Plane / Capsule / Cylinder / Tetrahedron / Frustum / ConvexHullView) | `Vec3<Length>` at API surface; raw `f32` in SIMD kernels | v0c (geometry-side adoption pinned in v0c per the eylem-broadphase consumer) |
| `crd-geometry-bvh` (BvhTree / Bvh4Tree / DynamicBvh / queries) | dimensional AABBs; result-types `RayHit` / `ClosestPointResult` carry `Length`-typed t / distance | v0c |
| `crd-geometry-convex` (GJK / EPA / SAT / Quickhull / ConvexHullView) | `Vec3<Length>` shapes; distance / penetration as `Length` | v0c |
| `crd-geometry-curves` (Bezier / Hermite / Catmull-Rom / B-spline / arcs) | `Vec3<Length>` control points; arc-length table = `Length`; tangent = `Velocity-like` (parameter rate) | v0c (lands inside the curves slice if curves ship after units) |
| `crd-renderer` (Camera, lights, FramePlan, frustum, draw list) | `Position3` for camera / light position; `Angle` for FOV; `Length` for near/far; raw `Mat4f` at GPU upload | v0d |
| `crd-rhi` (low-level) | raw scalars only (GPU buffer / pipeline / shader); typed surfaces upstream | v0d (validation only — RHI stays raw) |
| `crd-jobs`, `crd-memory`, `crd-containers`, `crd-log`, `crd-platform` | not affected (no physical quantities) | — |
| `crd-imgui` debug | `.value_in<UserPreferredUnit>()` in every inspector read | v0d |
| `crd-sandbox` | adoption demo: scene-selector + every showcase scene typed | v0d |
| Future `crd-sdf` (3.1.5) | `Length`-typed sample points, gradient = `Vec3<f32>` (unit-vector), SDF value = `Length` | v0c if sdf ships after units; otherwise sdf adopts at sdf's v0 |
| Future `crd-hesap` (3.1.6) | `Quantity`-templated tensors; autodiff respects dimensional arithmetic | hesap v0 |
| Future `crd-eylem-aero` (ADR-0073) | Pressure / Density / Velocity / Force / Acceleration — full aerospace dimensional throughout | aero v0 (consumes `crd-units` from day 1, no adoption pass needed) |
| Future `crd-eylem-cine` (ADR-0074) | `Position3` / `Velocity3` / `AngularVelocity3` for animated mesh queries | cine v0 |
| Future `crd-brep` (3.1.8) | `Length` for CAD; `Angle` for sketch constraints; STEP/IGES/Parasolid unit tag at import | brep v0 |
| Future `crd-cad-feature` (3.1.9) | `Length` + `Angle` + dimensional constraints (parametric expressions are dimensionally typed) | cad-feature v0 |
| Future `crd-cfd` (3.1.10) | full thermofluid: `Velocity` / `Pressure` / `Density` / `Temperature` / `Viscosity` / `ThermalConductivity` / `HeatFlux` / `SpecificEnergy` | cfd v0 |
| Future `crd-estimation` + `crd-control` (3.1.11) | `Length` + `Angle` + `Velocity` + `AngularVelocity` for state; `Force` + `Torque` for control signals | est+ctrl v0 |
| Future `crd-fea` (3.1.12) | `Length` + `Force` + `Pressure` + `Stress` (= `Pressure`) + `Strain` (dimensionless) + `YoungsModulus` (= `Pressure`) | fea v0 |
| Future `crd-cam` (3.1.13) | milling: `Velocity` (feed) + `AngularVelocity` (spindle) + `Length` (depth/radius) + `Power`; printing: `Temperature` + `Velocity` + `Length` (layer height) + `Volume` (flow) | cam v0 |
| Future `crd-eda` (3.1.17) | `Length` (traces/clearances) + `Voltage` + `Current` + `Resistance` + `Capacitance` + `Inductance` + `Frequency` + `Charge` + `MagneticFlux` | eda v0 |
| Future `crd-procgen` (3.1.15) | `Length` + `Angle` for L-systems / WFC / terrain primitives | procgen v0 |
| Future `crd-sciviz` (3.1.16) | typed-quantity-aware colour-map domain ranges + typed axis ticks + typed measurement annotations | sciviz v0 |
| Future audio (3.4) | `Frequency` + `Time` + `Pressure` (sound pressure level via reference 20 μPa) | audio v0 |
| Future animation (3.2) | `Time` + `Angle` (joint angle) + `AngularVelocity` (joint velocity) | animation v0 |

## Architectural pins

1. **SI is the only canonical internal unit.** No exceptions. `Length`'s `.value` is always meters; `Mass`'s is always kg; `Time`'s is always seconds; `Angle`'s is always radians; `Temperature`'s is always kelvin; etc.
2. **Precision tier is orthogonal.** `Length<f32>` for games / runtime, `Length<f64>` for aerospace large-world / CAD micrometer / scientific. Same dimensional type system, different scalar precision. Explicit conversion `Length<f64>{l32.value_in<Meter>()}` (round-trip-safe).
3. **Boundary discipline.** Asset / file / UI / network layers carry unit tags and convert at the boundary; runtime never sees non-SI. No "implicit conversion" path — every conversion is explicit at a single boundary point.
4. **Zero overhead.** `Quantity<D, T>` is bit-equal to `T` for SIMD / GPU upload. `Vec3<Length<f32>>` is bit-equal to `Vec3<f32>`. Compile-time `static_assert`s pin layout / size / alignment / trivially-copyable.
5. **No untagged-physical numeric crosses a module boundary.** `crd-no-untagged-physical-numeric` CI guard enforces. Dimensionless quantities (restitution, friction, indices, counts, RGBA components) are allowed bare; physical quantities (length, mass, time, force, …) are typed.
6. **`crd-math` stays raw.** SIMD kernels (Vec/Mat ops, the SIMD substrate from ADR-0033 + ADR-0033 amendments, `crd-simd-emission-check`) operate on raw `f32` / `f64`. The dimensional layer is *around* `crd-math`, not inside it.
7. **GPU / RHI stays raw.** `vkCmdPushConstants` / uniform buffer / SSBO writes consume raw `f32` / `f64`. Conversion happens once at the upload-call site (`.value` accessor); shaders see bare floats.
8. **Angle is a distinct base dimension**, not strict-SI dimensionless. Strict SI: radians = m/m = dimensionless. Cerid: `Angle` is its own type (tagged as the 8th compile-time exponent in `Dim<L,M,T,I,Th,N,J,A>`) so `Angle + Length` is a compile error. This is the mp-units P1935 + Boost.Units pragmatic choice; we follow it.
9. **Determinism is preserved.** The dimensional wrappers do not change FP semantics. ADR-0063 contract intact across `crd-units` adoption.
10. **No runtime cost.** `static_assert(sizeof(Length<f32>) == sizeof(f32))`. The wrapper compiles away.
11. **Frame transforms are NOT unit conversions.** Coordinate-system / reference-frame transforms (ENU / NED / ECEF in geodetic + robotics; body vs inertial frame in physics; world vs local vs view vs clip in rendering; cartesian vs polar vs spherical) are *geometric* transforms — dimension stays `Length`, only the basis changes. They live in `crd-math::Transform` + `crd-geometry-primitives::transform_aabb` (Phase 3.1.7 v11), **not** in `crd-units`. A future `crd-coordinates` substrate may own the frame graph (robotics `tf2`-equivalent, aerospace SOFA frames, rendering view-stack) — explicitly out of `crd-units` scope.
12. **Conversion is a boundary discipline, not a hot-path cost.** Layer 1 (linear) + Layer 4 (compound auto-derived) are one FP multiply. Layer 2 (affine) is one multiply + one add. Layer 3 (non-linear: dB / cents / magnitude / Richter / pH) is one `pow` / `log10` — boundary-only, never on a hot loop. Layer 6 format/parse is per-UI-interaction, never hot. `Quantity` arithmetic itself produces identical codegen to bare-scalar arithmetic (`objdump`-verified in v0a).
13. **Extensibility is namespace-scoped, not central-registry-scoped.** Domain modules add their own units (`crd-eylem-aero` → AU/ly/parsec/SolarMass, `crd-eda` → Mil/OhmCm/AmpHour, `crd-cam` → RPM/IPM/SFM, `crd-eylem-cine` → Frame_24/30/60fps, future `crd-material` → centipoise/pascal-second/specific-heat-capacity) in their own `units` sub-namespace with their own UDLs. ADL handles lookup. **`crd-units` core never grows when a domain ships.** Adding one new base unit unlocks N new compound units automatically (Layer 4 multiplier).
14. **Standardised-rational exactness wherever possible.** SI prefix conversions (`m ↔ mm`), standardised imperial (`m ↔ inch` per 1959 international agreement), defined astronomical constants (`m ↔ ly` per IAU 2012, `m ↔ AU` per IAU 2012, `m ↔ EarthRadius` per WGS84 equatorial), exact SI defining constants (`m/s ↔ SpeedOfLight`) — all encoded as `std::ratio`, all bit-exact round-trips in `f64`, within 1 ULP in `f32`. The Mars-Climate-Orbiter class of drift becomes impossible at the rational-arithmetic layer.

## Open questions (resolved, pinned for the record)

| Q | Resolution |
|---|---|
| Quantity strong-typing or just a "stated convention"? | **Strong-typed.** Conventions get violated; types do not. The user's "always having units no matter what" requirement forces this. |
| Boost.Units / mp-units / hand-rolled? | **Hand-rolled.** Boost.Units is C++03-era + heavy compile-time cost; mp-units (P1935) targets C++26 + still in flux. A focused Cerid-owned implementation (~1 KLOC) is leaner + matches our "no vendor wraps for core simulation surfaces" principle. |
| Angle as base dimension or dimensionless? | **Base dimension.** Strict SI radians is dimensionless, which silently lets `Angle + Length` compile. We tag Angle as the 8th exponent. mp-units does the same. |
| `Vec3<Quantity>` or `Quantity-Vec3`? | `Vec3<Quantity>` — the dimensional type is the *element*, the vector is the container. Layout-equal to `Vec3<T>` so SIMD storage is unaffected. |
| Adoption inside `crd-math` (Vec/Mat themselves)? | **No.** `crd-math` stays raw-scalar — it's the SIMD substrate. `crd-units` wraps it. |
| Quaternion as dimensional? | **No.** Unit-quaternions have dimensionless components even though the rotation they represent has angular semantics. Axis-angle decomposition exposes `Angle`. |
| Energy and Torque have the same SI base (kg·m²·s⁻²) — distinct? | **Distinct via stronger tagging.** mp-units / Boost both special-case this — torque is a *pseudo-vector* (axis-orientation matters), energy is a scalar. We tag Torque and Energy with distinct compile-time markers despite the same SI base, so `Energy = Torque` is a compile error. |
| Time vs Frequency arithmetic? | `Frequency = 1 / Time`; `Frequency * Time = Dimensionless` (allowed; pin the result type to `Dimensionless` not `f32`). |
| Pound — mass or force? | **Force (pound-force, lbf).** Ambiguous in everyday usage; SI-pedantic `Mass` literal is `_kg`, force literal is `_N` or `_lbf`. The literal `_lb` is **disallowed** (compile error at literal-eval; user picks `_lb_mass` or `_lbf` explicitly). |
| Temperature: absolute vs relative? | **Absolute.** `_celsius` literal converts to kelvin by `+273.15` at construction; `_fahrenheit` similarly. Subtractions of two absolute temperatures produce a *temperature difference* tagged `Quantity<Temperature_Diff>` (distinct type) to handle relative-vs-absolute correctly. This is a known mp-units subtlety; we handle it from day 1. |
| f32 / f64 promotion rules? | Explicit conversion only — `Length<f32>(l64.value_in<Meter>())`. No implicit cross-precision arithmetic. |
| Cross-module migration cost? | One-shot adoption at v0b/c/d. ~50 fields across `crd-scene Transform` + `crd-eylem RigidBody` + `crd-config` + `crd-resources` + `crd-renderer Camera`/`Light`. Each touch is mechanical. |
| Editor / file format compatibility? | All existing TOML / glTF / CRDR assets continue to load via the unit-tag fallback (untagged → assumed SI base + warning). Once v0d closes, new assets must be tagged. |
| Conversion factor storage — `f64` or `std::ratio`? | **`std::ratio`** for the source-of-truth; `static constexpr f64 factor` derived. SI prefixes + standardised imperial round-trips become bit-exact; compound `UnitDiv` / `UnitMul` compose via `std::ratio_multiply` / `std::ratio_divide` at compile time. The `f64` form is only used at the actual runtime multiply. |
| Non-linear units — addable? | **No.** `dB(20) + dB(20) ≠ dB(40)`. Compile-time block on `operator+` for non-linear quantities. Callers convert to the underlying linear SI domain, add there, convert back. The same applies to multiplication of magnitudes, multiplication of Richter scales, etc. |
| Compound unit factor derivation — runtime or compile-time? | **Compile-time** via `std::ratio_multiply` / `std::ratio_divide`. `UnitDiv<Mile, Hour>::factor` is a `static constexpr f64` known at the call site — identical performance to a hand-written linear unit. |
| dB-family — which set ships in v0a? | dB SPL (pressure-domain, audio + cinematic) + dB V (voltage) + dB W (power) + dB A (A-weighted SPL) + dB u (audio reference) + dB m / dBm (RF power, future EDA). Stellar magnitude / Richter / pH reserved-via-framework; ship when a consumer asks. |
| Bytes / binary prefixes (KiB vs KB, GiB vs GB)? | Yes — `_KiB` / `_MiB` / `_GiB` / `_TiB` (binary, ×1024) and `_kB` / `_MB` / `_GB` / `_TB` (decimal, ×1000) literals included. Dimensionless category (`dim::Data` is a new tagged dimension). Consumers: `crd-memory` budgets, `crd-resources` file sizes, network bandwidths. |
| Date arithmetic — leap years, leap seconds, timezones? | **OUT OF SCOPE for `crd-units`.** `Time` is pure duration (seconds since an arbitrary epoch). Calendar / datetime / leap-second handling is a separate `crd-datetime` module if a consumer ever needs it (cinematic timecode handling is `eylem-cine`'s problem; aerospace ephemeris timing is `eylem-aero`'s problem; both build on `crd-units`' duration primitives). |
| Gauge vs absolute pressure? | **Reserved.** Default `Pressure` is absolute (Pa). `PressureDelta` (gauge) opted in when a CFD / weather / aerospace consumer needs it — same `absolute vs delta` pattern as `Temperature` / `TemperatureDelta`. |
| Pound — mass or force? | **Force (`PoundForce`, `lbf`).** `_lb` literal **disallowed** (compile error at literal-eval); user picks `_lb_mass` or `_lbf` explicitly. Same for `_oz` (mass-vs-fluid-ounce-vs-troy-ounce): require `_oz_mass`, `_oz_troy`, `_oz_fluid_us`, `_oz_fluid_imp`. The forced explicitness eliminates a class of cross-cultural bugs at the literal site. |
| Hour vs solar-day vs sidereal-day? | Both `Hour` and `Day` (solar, 86400 s EXACT) ship in `crd-units` core. `SiderealDay` (86164.0905 s) and `JulianYear` (31557600 s EXACT) ship in `crd::eylem_aero::units`. |
| Temperature: absolute vs relative? | **Two distinct types: `Temperature` (absolute) and `TemperatureDelta` (relative).** `_celsius` literal applies offset (K = C + 273.15); subtraction of two `Temperature`s yields a `TemperatureDelta`; `TemperatureDelta + Temperature` yields `Temperature`; `Temperature + Temperature` is a compile error. Future-reserved for: `Pressure / PressureDelta` (gauge), `Datetime / Duration`, possibly `Voltage / VoltageDelta`. |
| `f32` / `f64` promotion rules? | **Explicit conversion only.** `Length<f64>{l32.value_in<Meter>()}` round-trips bit-exactly (f32→f64 widening is exact). No implicit cross-precision arithmetic. Mixing `Length<f32>` and `Length<f64>` in an expression is a compile error; the user picks one. |
| Energy and Torque both reduce to kg·m²·s⁻² in strict SI — distinct? | **Distinct via "kind" tagging.** mp-units pattern: `Energy` and `Torque` share the same `Dim<>` exponent vector but carry a `kind` marker so `Energy = Torque` is a compile error (semantically different even though dimensionally identical). Same for `Frequency` vs `AngularVelocity` (both s⁻¹). |
| Cross-engine import — STEP / IGES / FBX / glTF / Gerber unit tags? | Each format's reader maps its native unit tag to a `crd-units` `LinearUnit` / `AffineUnit` table; cooker normalises to SI at cook time. The Cerid asset pipeline owns the conversion table per format. Same pattern for export: cooker reads SI from the engine, applies the target format's preferred unit at write time. |
| Coordinate-system / reference-frame transforms? | **NOT a `crd-units` concern.** Geometric transforms (ENU / NED / ECEF; body vs inertial; cartesian vs polar) live in `crd-math::Transform` + `crd-geometry-primitives::transform_aabb`. Pinned as Architectural Pin #11. A future `crd-coordinates` substrate may eventually own the frame graph — explicitly out of `crd-units` scope. |
| Runtime unit-tag dispatch — how fast? | A `UnitTag` is a `u16` (256 max core units + 256 max per-domain extension headroom). `.value_in(UnitTag)` is a switch / lookup-table dispatch on the tag; ~3–5 ns per call. Used in UI / inspector / format paths; never on a hot path. The compile-time `.value_in<TargetUnit>()` form remains the zero-cost option for caller-knows-target code. |

## Validation + benchmark expectations

- **Compile-time dimension-mismatch rejection** — the negative-compile test suite (Catch2 `STATIC_REQUIRE` + manual `#ifdef` gates) validates that every dimensional invariant rejects bad code at compile time, not at runtime.
- **Numeric round-trip** — `Length<f32>{value_in<Meter>(25.4_mm)} == 25.4_mm` bit-exact; `value_in<Millimeter>()` round-trips through Length within `1 ULP`.
- **Cross-precision conversion** — `Length<f64>{l32.value_in<Meter>()}.value_in<Meter>() == l32.value_in<Meter>()` bit-exact (since f32→f64 is exact).
- **Layout pins** — `sizeof(Length<f32>) == sizeof(f32)` ✓; `alignof(Length<f32>) == alignof(f32)` ✓; `std::is_standard_layout_v<Length<f32>>` ✓; `std::is_trivially_copyable_v<Length<f32>>` ✓; `sizeof(Vec3<Length<f32>>) == sizeof(Vec3<f32>)` ✓; `Vec3<Length<f32>>` memcpy-safe.
- **Zero overhead** — perf bench: `Vec3<Length<f32>> sum(N)` runs at the same throughput as `Vec3<f32> sum(N)` within measurement noise (validates the wrapper compiles away).
- **Determinism preserved** — every existing deterministic-replay test (scene-state replay, eylem-step replay, transform-propagation bit-exact) passes byte-for-byte after the v0b/c/d migration.
- **Eylem integration test** — known-physics-ground-truth cases (free fall: `h = ½·g·t²`, simple pendulum period, projectile range, 2-body orbit) verified to within `1e-6` (f32) / `1e-12` (f64) across all 9 deterministic-replay CI configs.
- **Cross-platform / cross-config** — `scripts/full-sweep.ps1` 17-config sweep at v0d close.

## Slot in the broader Cerid roadmap

```
Phase 3.1 eylem v0 ✅ → v1a-v1b ✅
  ⏸ PAUSE
Phase 3.1.7 crd-geometry  (49 slices, ~7.5–9.5 mo)
  → v3a/b/c ✅ done 2026-05-14, v3d hull simplification NEXT
  → v3-close → v4 → v4-validate → v5 → v6 → v7 → v8 → v9 → v9e → v10 → v11
  → Phase 3.1.7 CLOSE
Phase 3.1.7.5 crd-units  (4 slices, ~4 weeks) ← NEW SLOT
  → v0a substrate
  → v0b adoption A (config + scene + glTF)
  → v0c adoption B (eylem + geometry)
  → v0d adoption C (renderer + cookers + ImGui + sweep close)
  → Phase 3.1.7.5 CLOSE
Phase 3.1 eylem v1c RESUME (broadphase consuming crd-geometry + crd-units from day 1)
  → v1d → v1d-manifold → v1d-mesh → v1e → ... → v9
Phase 3.1.5 crd-sdf  (interleaved between eylem v2 and v3)
Phase 3.1.6 crd-hesap  (after eylem close)
Phase 3.1.8 crd-brep  (after hesap)
...
Phase 3.1.17 crd-eda
```

The slot timing is critical: `crd-units` ships *before* eylem v1c so
eylem's broadphase / narrowphase / contact / constraint solver / force
fields / sensors / aerospace / cinematic / vehicles / CCD / FEM / GPU /
differentiable all consume dimensional types from day 1. Every later
substrate (sdf, hesap, brep, cad-feature, cfd, estimation, control,
fea, cam, ml-inference, procgen, sciviz, eda) consumes dimensional
types from day 1 by virtue of `crd-units` already existing in the
dependency graph.

## Reference reading

- **mp-units** (Mateusz Pusz, P1935R8, target C++26) — the C++ standards proposal we're tracking for future inter-op. Cerid's `crd-units` is API-compatible with mp-units' shape so a future migration is mechanical.
- **Boost.Units** (Matthias Schabel, 2007 — battle-tested for 18 years) — the canonical hand-rolled approach; we follow its compile-time tagging strategy.
- **NASA Mars Climate Orbiter loss report** (1999) — the canonical case study for unit-conversion bugs; $327M loss from a single Newton·seconds vs pound·force·seconds mismatch across a module boundary. Cerid's units architecture is exactly what would have prevented it.
- **ROS REP 103** — Standard Units of Measure and Coordinate Conventions; the robotics community's SI-everywhere policy. Cerid is aligned.
- **IEC 80000-1** — Quantities and units, general principles.
- **ASME Y14.5** — GD&T (relevant for the eventual `crd-cad-feature` 3.1.9 adoption).

## References

- ADR-0063 (determinism contract) — units adoption preserves bit-exact determinism.
- ADR-0076 §15 (geometry pins) — `crd-no-std-math-check` + `crd-simd-emission-check` + `crd-no-non-ascii-test-names` + `crd-no-std-sort-check` precedents for the new `crd-no-untagged-physical-numeric` guard.
- ADR-0077 (multi-domain expansion) — the 8-domain mandate that motivates the units commitment.
- `docs/PRINCIPLES.md` — the units principle pinned 2026-05-14 (will become a Cornerstone with ADR-0078 reference at v0a close).
- `docs/phases/phase-3.1.7-geometry.md` — § Renewed scope coverage / ADR candidates #1 (revised 2026-05-14 to point at this phase doc).
