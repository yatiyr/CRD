#pragma once

// ---------------------------------------------------------------------------
// crd-units — boundary egress accessor (Phase 3.1.7.5 v0a-2).
//
// value_in<TargetUnit>(quantity) returns the scalar value of the quantity
// expressed in TargetUnit. Compile-time dimension validation: the call is
// rejected at compile time if TargetUnit's dimension does not match the
// quantity's dimension.
//
// **Performance.** Layer 1 / Layer 4 conversion = one FP multiply at the
// boundary. The factor is a compile-time `static constexpr f64` so the
// multiplication happens directly with that constant.
//
// **Member form** lives on Quantity (added in this header via free-function
// + a member wrapper added below — Quantity stays in v0a-1 and this header
// extends with adapter functions).
//
// **Round-trip exactness.** For SI-prefix and standardized-imperial unit
// pairs, the conversion factor is an exact rational — `value_in<Foot>(q).value_in<Inch>()`
// round-trip is bit-exact in f64 because `std::ratio_divide<Foot::factor_ratio,
// Inch::factor_ratio>` reduces to `std::ratio<12, 1>`. For irrational-
// factor units (Degree, Grad, BTU, etc.), the f64 evaluation rounds at the
// last step — 1 ULP tolerance documented in units_si.hpp.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/units/dim.hpp>
#include <crd/units/quantity.hpp>
#include <crd/units/units_si.hpp>

namespace crd::units
{

// ===========================================================================
// Free-function form: value_in<TargetUnit>(q)
// ===========================================================================

template <typename TargetUnit, typename D, typename T>
[[nodiscard]] constexpr T value_in(Quantity<D, T> q) noexcept
{
    static_assert(dim_equal_v<typename TargetUnit::dimension, D>,
                  "value_in<TargetUnit>(q): dimension mismatch — TargetUnit's dimension "
                  "does not match Quantity's dimension.");
    return q.value / static_cast<T>(TargetUnit::factor);
}

// ===========================================================================
// Ingress: construct a Quantity FROM a value in a given unit
// ===========================================================================
//
// quantity_from<SourceUnit>(scalar_value) returns the Quantity<D, T>
// representing `scalar_value` in `SourceUnit`. Used at config / file /
// network ingress points: `length_mm = 25.4` becomes
// `quantity_from<Millimeter>(25.4) → Length<f64>{0.0254}` internally.

template <typename SourceUnit, typename T>
[[nodiscard]] constexpr Quantity<typename SourceUnit::dimension, T>
quantity_from(T value_in_unit) noexcept
{
    return Quantity<typename SourceUnit::dimension, T>{value_in_unit *
                                                        static_cast<T>(SourceUnit::factor)};
}

} // namespace crd::units
