#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-a — lattice (reflection-coefficient) representation.
//
// An all-pole / AR filter 1 / A(z), A(z) = 1 + a1 z^{-1} + ... + an z^{-n}, has
// an equivalent LATTICE form parametrised by reflection coefficients k[1..n]
// (PARCOR). The Levinson-Durbin recursion converts both ways:
//   step-DOWN (tf -> lattice): peel a[m][m] = k[m] off, deflate to order m-1;
//   step-UP   (lattice -> tf): grow a[m] from a[m-1] + k[m]*reverse.
// STABILITY is read off directly: A(z) is minimum-phase (all poles inside the
// unit circle) iff every |k[m]| < 1 — the lattice's defining property and the
// reason AR estimators (Burg, v11-o) and lattice filters use it. f32/f64.
// MATLAB tf2latc / latc2tf (all-pole form).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dsp/filter.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::dsp
{

// Reflection coefficients k[0..n-1] of an order-n all-pole section (k[i] is the
// PARCOR at stage i+1).
template <typename T> struct Lattice
{
    crd::containers::Array<T> k;
    explicit Lattice(crd::memory::IAllocator* alloc) : k(alloc) {}
};

// tf (denominator a, a[0]==1) -> lattice. Levinson step-DOWN. Returns false if a
// degenerate |k| == 1 stage is hit (the recursion divides by 1 - k^2).
template <typename T>
[[nodiscard]] bool denom_to_lattice(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> a, Lattice<T>& out)
{
    const crd::usize n = (a.size() == 0) ? 0 : a.size() - 1; // order
    out.k.resize(n);
    if (n == 0)
    {
        return true;
    }
    crd::containers::Array<T> am(alloc); // current a^(m), indices 0..m (am[0]==1)
    am.resize(n + 1);
    for (crd::usize i = 0; i <= n; ++i)
    {
        am[i] = a[i] / a[0];
    }
    for (crd::usize m = n; m >= 1; --m)
    {
        const T km = am[m];
        out.k[m - 1] = km;
        const T denom = T(1) - km * km;
        if (std::abs(denom) < static_cast<T>(1e-300))
        {
            return false; // |k| == 1 ⇒ pole on the unit circle, recursion singular
        }
        crd::containers::Array<T> prev(alloc);
        prev.resize(m); // a^(m-1), indices 0..m-1
        prev[0] = T(1);
        for (crd::usize i = 1; i < m; ++i)
        {
            prev[i] = (am[i] - km * am[m - i]) / denom;
        }
        am = std::move(prev);
        if (m == 1)
        {
            break; // avoid usize underflow
        }
    }
    return true;
}

// lattice -> tf (denominator a, a[0]==1). Levinson step-UP.
template <typename T>
[[nodiscard]] crd::containers::Array<T> lattice_to_denom(crd::memory::IAllocator* alloc, const Lattice<T>& lat)
{
    const crd::usize n = lat.k.size();
    crd::containers::Array<T> am(alloc);
    am.resize(1);
    am[0] = T(1);
    for (crd::usize m = 1; m <= n; ++m)
    {
        const T km = lat.k[m - 1];
        crd::containers::Array<T> next(alloc);
        next.resize(m + 1);
        next[0] = T(1);
        for (crd::usize i = 1; i < m; ++i)
        {
            next[i] = am[i] + km * am[m - i];
        }
        next[m] = km;
        am = std::move(next);
    }
    return am;
}

// A(z) is minimum-phase (all poles strictly inside the unit circle) iff every
// |k| < 1. The lattice's defining stability test (no root-finding needed).
template <typename T> [[nodiscard]] bool lattice_is_stable(const Lattice<T>& lat) noexcept
{
    for (crd::usize i = 0; i < lat.k.size(); ++i)
    {
        if (std::abs(lat.k[i]) >= T(1))
        {
            return false;
        }
    }
    return true;
}

} // namespace crd::hesap::dsp
