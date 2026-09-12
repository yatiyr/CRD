#pragma once

// ---------------------------------------------------------------------------
// crd-time -- Duration = crd-units::Time<f64> alias (Detour D-006).
//
// A Duration is a relative time interval in SI seconds, stored as f64 inside
// a `Quantity<dim::Time, f64>` so dimensional safety holds at every API
// surface. Construction via crd-units UDLs: `1.5_s`, `16.667_ms`, `60.0_Hz`
// (the inverse — `Frequency`), etc.
//
// Why f64 (not f32):
//   - Game runtime deltas are in the 1e-3 to 1e-1 s range; f32 has plenty of
//     precision there. BUT: `Instant` arithmetic accumulates over hours of
//     simulation/replay/profiling — f32 loses precision at minute-scale.
//   - GPU timestamps come back as `u64` nanoseconds; converting to f64
//     seconds preserves nanosecond resolution for ~10^7 seconds (months).
//   - f64 matches `std::chrono::steady_clock`'s internal duration type on
//     most platforms.
//
// Consumers wanting f32 deltas can downcast at the API surface via
// `Duration::value_in<Second>()` → f64, then static_cast<f32>.
// ---------------------------------------------------------------------------

#include <crd/units/dim_aliases.hpp>
#include <crd/units/quantity.hpp>

namespace crd::time
{

using Duration = crd::units::Quantity<crd::units::dim::Time, crd::f64>;

// Convenience zero-duration constant (constexpr).
inline constexpr Duration kZeroDuration{};

} // namespace crd::time
