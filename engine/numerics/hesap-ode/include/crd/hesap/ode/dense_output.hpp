#pragma once

// dense_output.hpp — Phase 3.1.6 v9-a: the CONTINUOUS-OUTPUT contract + the method-agnostic fallback
// interpolant. Pinned at v9-a (the day-1 landmine from the plan): events (v9-c), `t_eval` sampling,
// sensitivity checkpointing (v9-k), and plotting all consume dense output — retrofitting it would refactor
// every method. ADR-0091.
//
// THE CONTRACT every adaptive method (v9-b+) implements:
//   • After an ACCEPTED step [t0, t1], the method can write an interpolation-coefficient block of
//     `dense_width(n)` Ts into caller-owned contiguous storage (no allocation in the method; v9-c's
//     `OdeSolution` owns the storage and lays blocks out contiguously per step).
//   • A static, allocation-free `*_eval(block, t)` reconstructs y(t) for t ∈ [t0, t1] at the method's
//     interpolation order. Evaluation is a pure deterministic function of the block.
//   • Methods with a native interpolant (RK45's quartic, DOP853's 7th-order, Radau's collocation
//     polynomial) provide their own; everything else falls back to the cubic Hermite below (3rd order,
//     C¹ across steps, built from data every one-step method already has: y0, f0, y1, f1).

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

namespace crd::hesap::ode
{

// Cubic Hermite on [t0, t1] from endpoint values and slopes. Exact (to roundoff) on polynomials of degree
// ≤ 3; O(h⁴) local interpolation error on smooth solutions. θ = (t − t0)/(t1 − t0):
//   y(t) = h00·y0 + h10·h·f0 + h01·y1 + h11·h·f1
//   h00 = 2θ³−3θ²+1, h10 = θ³−2θ²+θ, h01 = −2θ³+3θ², h11 = θ³−θ²
// θ = 0 reproduces y0 exactly (h00 = 1, rest 0); θ = 1 reproduces y1 exactly. In-place safe (y_out may
// alias none of the inputs being read at a different index — element i reads only index i).
template <typename T>
void hermite_eval(T t0, T t1, crd::containers::ConstSpan<T> y0, crd::containers::ConstSpan<T> f0,
                  crd::containers::ConstSpan<T> y1, crd::containers::ConstSpan<T> f1, T t,
                  crd::containers::Span<T> y_out)
{
    const crd::usize n = y0.size();
    CRD_ASSERT(f0.size() == n && y1.size() == n && f1.size() == n && y_out.size() == n);
    const T h = t1 - t0;
    CRD_ASSERT(h != static_cast<T>(0));
    const T theta = (t - t0) / h;
    const T t2 = theta * theta;
    const T t3 = t2 * theta;

    const T one = static_cast<T>(1);
    const T two = static_cast<T>(2);
    const T three = static_cast<T>(3);

    const T h00 = two * t3 - three * t2 + one;
    const T h10 = t3 - two * t2 + theta;
    const T h01 = -two * t3 + three * t2;
    const T h11 = t3 - t2;

    for (crd::usize i = 0; i < n; ++i)
    {
        y_out[i] = h00 * y0[i] + h10 * h * f0[i] + h01 * y1[i] + h11 * h * f1[i];
    }
}

} // namespace crd::hesap::ode
