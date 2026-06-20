#pragma once

// ---------------------------------------------------------------------------
// crd-units -- NormalizedFrequency<T> (Phase 3.1.6 v11-a).
//
// DSP normalizes frequency against the sample rate fs, and THREE incompatible
// conventions coexist in the wild -- a classic, silent footgun:
//
//   * cycles/sample        nu = f / fs            in [0, 0.5] (Nyquist at 0.5)
//   * radians/sample       w  = 2*pi*nu           in [0, pi]
//   * fraction-of-Nyquist  Wn = f / (fs/2) = 2*nu in [0, 1]   (scipy/MATLAB Wn)
//
// scipy.signal.firwin / iirfilter take Wn (fraction-of-Nyquist); freqz returns
// w (rad/sample); textbooks mix all three. Passing the wrong one is a bug that
// type-checks. NormalizedFrequency makes the convention IMPOSSIBLE to confuse:
// one canonical store (cycles/sample), explicit named constructors, explicit
// named accessors. No implicit conversions, no bare scalar in/out.
//
// This is a strong typedef over a dimensionless ratio (f/fs), NOT a
// Quantity<Dim,T> -- it is a normalization, not an SI dimension. It lives in
// crd-units so the typed DSP surface is consistent and reusable.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/units/dim_aliases.hpp>
#include <crd/units/quantity.hpp>

#include <numbers>

namespace crd::units
{

// A frequency expressed relative to the sample rate. Canonical store: cycles
// per sample (nu = f/fs). Construct + read only through the named members so
// the convention is always explicit.
template <typename T = crd::f64> struct NormalizedFrequency
{
    T cycles{}; // nu = f / fs   (the single canonical representation)

    // --- named constructors (the only ways in) ---
    [[nodiscard]] static constexpr NormalizedFrequency from_cycles_per_sample(T nu) noexcept { return {nu}; }
    [[nodiscard]] static constexpr NormalizedFrequency from_rad_per_sample(T w) noexcept
    {
        return {static_cast<T>(w / (T(2) * std::numbers::pi_v<T>))};
    }
    // Wn in [0,1], the scipy/MATLAB filter-design convention (1 == Nyquist).
    [[nodiscard]] static constexpr NormalizedFrequency from_nyquist_fraction(T wn) noexcept { return {wn / T(2)}; }
    // raw Hz against a raw sample rate (lower-layer friendly).
    [[nodiscard]] static constexpr NormalizedFrequency from_hz(T f_hz, T fs_hz) noexcept { return {f_hz / fs_hz}; }
    // typed Hz against a typed sample rate (the public-surface path).
    [[nodiscard]] static NormalizedFrequency from_hz(Quantity<dim::Frequency, T> f,
                                                     Quantity<dim::Frequency, T> fs) noexcept
    {
        return {f.value / fs.value};
    }

    // --- named accessors (the only ways out) ---
    [[nodiscard]] constexpr T cycles_per_sample() const noexcept { return cycles; }
    [[nodiscard]] constexpr T rad_per_sample() const noexcept
    {
        return static_cast<T>(cycles * T(2) * std::numbers::pi_v<T>);
    }
    [[nodiscard]] constexpr T nyquist_fraction() const noexcept { return cycles * T(2); }
    [[nodiscard]] constexpr T hz(T fs_hz) const noexcept { return cycles * fs_hz; }
    [[nodiscard]] Quantity<dim::Frequency, T> hz(Quantity<dim::Frequency, T> fs) const noexcept
    {
        return Quantity<dim::Frequency, T>{cycles * fs.value};
    }
};

} // namespace crd::units
