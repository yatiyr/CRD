#pragma once

// ---------------------------------------------------------------------------
// crd-units -- Layer 3: NonLinearUnit + dB family + musical pitch
// (Phase 3.1.7.5 v0a-3).
//
// dB / cents / semitones / stellar magnitude / Richter / pH cannot be
// represented as scale + offset. They use explicit conversion functions.
//
// Architectural pin: **non-linear units are an I/O concern only.** dB values
// don't support arithmetic at the type level -- `dB(20) + dB(20) != dB(40)`
// (it's actually +3 dB for incoherent or +6 dB for coherent summation).
// The Cerid pattern: callers convert dB to the underlying linear quantity
// (Pressure / Voltage / Power), do arithmetic in the linear domain, then
// convert back to dB via the boundary accessor.
//
// Concretely:
//   auto p1 = 80.0_dB_spl;  -> Quantity<dim::Pressure, f64>{0.2 Pa}
//   auto p2 = 80.0_dB_spl;  -> Quantity<dim::Pressure, f64>{0.2 Pa}
//   auto sum = p1 + p2;      -> Pressure{0.4} (linear arithmetic OK)
//   f64 sum_dB = value_in_nonlinear<DecibelSPL>(sum);  -> 86.02 dB
//
// The dB literal converts at construction; the conversion function lives in
// the boundary egress (value_in_nonlinear<DecibelSPL>(q)). There is no
// distinct "DbValue" type -- the value lives in the linear Quantity, and
// dB is just an I/O lens on it.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/units/dim.hpp>
#include <crd/units/dim_aliases.hpp>
#include <crd/units/quantity.hpp>

#include <crd/math/cmath.hpp>

namespace crd::units
{

// ===========================================================================
// NonLinearUnit<Dim, ToSiFn, FromSiFn>
// ===========================================================================
//
// ToSiFn and FromSiFn are constexpr function-objects (typically stateless
// lambdas or static-method-wrapper structs) that convert a scalar between
// the non-linear unit and the linear SI base.
//
// We use struct-based dispatch because constexpr lambdas as template
// parameters require C++20 with full constant-evaluable bodies, and we
// also want crd::math::pow / crd::math::log10 which aren't constexpr in C++20.
//
// Per-unit definitions live below as concrete types (DecibelSPL etc.) with
// static `to_si` / `from_si` member functions. The framework supports any
// new non-linear unit by defining another such type.

// ===========================================================================
// Audio dB family
// ===========================================================================
//
// dB SPL (sound pressure level): reference is 20 micropascal.
//   pa = 20e-6 * pow(10, db / 20)
//   db = 20 * log10(pa / 20e-6)
//
// dB V (decibel-volt): reference is 1 volt.
//   v  = pow(10, db / 20)
//   db = 20 * log10(v)
//
// dB W (decibel-watt): reference is 1 watt.
//   w  = pow(10, db / 10)
//   db = 10 * log10(w)
//
// dB m (decibel-milliwatt, dBm): reference is 1 mW.
//   w  = 1e-3 * pow(10, db / 10)
//   db = 10 * log10(w / 1e-3)
//
// dB u (audio reference): reference is 0.7746 V (corresponds to 0 dBm in
// a 600-ohm reference).
//   v  = 0.7746 * pow(10, db / 20)
//   db = 20 * log10(v / 0.7746)

struct DecibelSPL
{
    using dimension = dim::Pressure;
    static constexpr bool is_nonlinear = true;

    [[nodiscard]] static crd::f64 to_si(crd::f64 db) noexcept
    {
        return 20.0e-6 * crd::math::pow(10.0, db / 20.0);
    }
    [[nodiscard]] static crd::f64 from_si(crd::f64 pa) noexcept
    {
        return 20.0 * crd::math::log10(pa / 20.0e-6);
    }
};

struct DecibelV
{
    using dimension = dim::Voltage;
    static constexpr bool is_nonlinear = true;

    [[nodiscard]] static crd::f64 to_si(crd::f64 db) noexcept
    {
        return crd::math::pow(10.0, db / 20.0);
    }
    [[nodiscard]] static crd::f64 from_si(crd::f64 v) noexcept
    {
        return 20.0 * crd::math::log10(v);
    }
};

struct DecibelW
{
    using dimension = dim::Power;
    static constexpr bool is_nonlinear = true;

    [[nodiscard]] static crd::f64 to_si(crd::f64 db) noexcept
    {
        return crd::math::pow(10.0, db / 10.0);
    }
    [[nodiscard]] static crd::f64 from_si(crd::f64 w) noexcept
    {
        return 10.0 * crd::math::log10(w);
    }
};

struct DecibelMilliwatt  // dBm
{
    using dimension = dim::Power;
    static constexpr bool is_nonlinear = true;

    [[nodiscard]] static crd::f64 to_si(crd::f64 dbm) noexcept
    {
        return 1.0e-3 * crd::math::pow(10.0, dbm / 10.0);
    }
    [[nodiscard]] static crd::f64 from_si(crd::f64 w) noexcept
    {
        return 10.0 * crd::math::log10(w / 1.0e-3);
    }
};

// ===========================================================================
// DSP ratio-dB family (Phase 3.1.6 v11-a)
// ===========================================================================
//
// Unlike dB SPL/V/W (referenced to a PHYSICAL unit), DSP works in dB as a
// DIMENSIONLESS RATIO of a transfer magnitude |H| = out/in. Two conventions,
// both shipped explicitly (the type name carries the convention so the
// amplitude-vs-power ambiguity is impossible to hide):
//
//   DecibelRatio  (amplitude / field quantities): the |H| convention.
//     lin = 10^(dB/20)        dB = 20*log10(lin)
//     -- magnitude responses, "Rs dB down" stopband edges (|H| = 10^(-Rs/20)).
//
//   DecibelPower  (power quantities): the |H|^2 convention.
//     lin = 10^(dB/10)        dB = 10*log10(lin)
//     -- the 10^(0.1*Rp) form scipy/MATLAB filter-ORDER routines (cheb1ord,
//        ellipord, ...) use on the squared magnitude.
//
// Both carry dimension = Dimensionless: a ratio has no SI dimension. Per the
// architectural pin at the top of this file, these are I/O lenses -- DSP
// design code converts a dB spec to the linear ratio at the boundary and does
// all arithmetic on the linear ratio.

struct DecibelRatio
{
    using dimension = dim::Dimensionless;
    static constexpr bool is_nonlinear = true;

    [[nodiscard]] static crd::f64 to_si(crd::f64 db) noexcept { return crd::math::pow(10.0, db / 20.0); }
    [[nodiscard]] static crd::f64 from_si(crd::f64 ratio) noexcept { return 20.0 * crd::math::log10(ratio); }
};

struct DecibelPower
{
    using dimension = dim::Dimensionless;
    static constexpr bool is_nonlinear = true;

    [[nodiscard]] static crd::f64 to_si(crd::f64 db) noexcept { return crd::math::pow(10.0, db / 10.0); }
    [[nodiscard]] static crd::f64 from_si(crd::f64 ratio) noexcept { return 10.0 * crd::math::log10(ratio); }
};

// ===========================================================================
// Musical pitch family
// ===========================================================================
//
// Cents: 1200 * log2(f1 / f_ref). Reference frequency is conventionally
// implicit (e.g. A4 = 440 Hz for pitch perception).
//
// Semitones: 12 * log2(f1 / f_ref).
//
// For Cerid, we use 440 Hz as the implicit reference (standard concert A).
// Consumers needing a different reference can write their own non-linear
// unit (federated registration pattern, Layer 5 from the phase doc).

inline constexpr crd::f64 kCentsReferenceFrequencyHz = 440.0;

struct Cents
{
    using dimension = dim::Frequency;
    static constexpr bool is_nonlinear = true;

    [[nodiscard]] static crd::f64 to_si(crd::f64 cents) noexcept
    {
        return kCentsReferenceFrequencyHz * crd::math::pow(2.0, cents / 1200.0);
    }
    [[nodiscard]] static crd::f64 from_si(crd::f64 hz) noexcept
    {
        return 1200.0 * crd::math::log2(hz / kCentsReferenceFrequencyHz);
    }
};

struct Semitones
{
    using dimension = dim::Frequency;
    static constexpr bool is_nonlinear = true;

    [[nodiscard]] static crd::f64 to_si(crd::f64 semitones) noexcept
    {
        return kCentsReferenceFrequencyHz * crd::math::pow(2.0, semitones / 12.0);
    }
    [[nodiscard]] static crd::f64 from_si(crd::f64 hz) noexcept
    {
        return 12.0 * crd::math::log2(hz / kCentsReferenceFrequencyHz);
    }
};

// ===========================================================================
// Ingress + egress
// ===========================================================================

// Construct a Quantity from a scalar in a non-linear unit.
template <typename U, typename T>
[[nodiscard]] Quantity<typename U::dimension, T> quantity_from_nonlinear(T value_in_unit) noexcept
{
    static_assert(U::is_nonlinear, "quantity_from_nonlinear<U>: U must be a NonLinearUnit.");
    return Quantity<typename U::dimension, T>{static_cast<T>(U::to_si(static_cast<crd::f64>(value_in_unit)))};
}

// Convert a Quantity back to a scalar in a non-linear unit.
template <typename U, typename D, typename T>
[[nodiscard]] T value_in_nonlinear(Quantity<D, T> q) noexcept
{
    static_assert(U::is_nonlinear, "value_in_nonlinear<U>: U must be a NonLinearUnit.");
    static_assert(dim_equal_v<typename U::dimension, D>,
                  "value_in_nonlinear<U>: dimension mismatch.");
    return static_cast<T>(U::from_si(static_cast<crd::f64>(q.value)));
}

} // namespace crd::units
