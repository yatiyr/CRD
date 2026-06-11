#pragma once

// merit.hpp — Phase 3.1.6 v7-j: the ℓ1 (exact) penalty MERIT FUNCTION — the line-search globalization device
// for constrained steps (SQP accepts a step only if it improves φ, which balances objective decrease against
// constraint violation):
//
//     φ(x; ν) = f(x) + ν·( Σ_i |c_E,i(x)| + Σ_i [−c_I,i(x)]₊ )
//
// EXACT penalty: for ν > ‖λ*‖∞ the minimizers of φ are the solutions of the constrained problem (N&W Thm 17.3),
// which is why the SQP driver keeps ν above the current multiplier estimate. φ is non-smooth — the line search
// uses the DIRECTIONAL derivative D(φ; p) (N&W 18.29), with the standard one-sided rules at kinks:
//     D|c_i| = sign(c_i)·∇c_iᵀp   (|∇c_iᵀp| at c_i = 0);   D[−c_i]₊ = −∇c_iᵀp if c_i < 0
//     ([−∇c_iᵀp]₊ at c_i = 0; 0 if c_i > 0).
// Pure functions over already-evaluated pieces (f, c, J) — eval accounting stays in the driver. ADR-0090.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <cmath>

namespace crd::hesap::opt
{

// φ(x; ν) from the already-evaluated objective value and constraint vectors.
template <typename T>
[[nodiscard]] inline T l1_merit_value(T fx, crd::containers::ConstSpan<T> ce, crd::containers::ConstSpan<T> ci,
                                      T nu) noexcept
{
    T pen = static_cast<T>(0);
    for (crd::usize i = 0; i < ce.size(); ++i)
    {
        pen += std::fabs(ce[i]);
    }
    for (crd::usize i = 0; i < ci.size(); ++i)
    {
        if (ci[i] < static_cast<T>(0))
        {
            pen -= ci[i];
        }
    }
    return fx + nu * pen;
}

// D(φ; p) at x from the already-evaluated gradient, constraints, and ROW-MAJOR Jacobians.
template <typename T>
[[nodiscard]] inline T l1_merit_directional(crd::containers::ConstSpan<T> g, crd::containers::ConstSpan<T> p,
                                            crd::containers::ConstSpan<T> ce, crd::containers::ConstSpan<T> ci,
                                            crd::containers::ConstSpan<T> je, crd::containers::ConstSpan<T> ji,
                                            T nu) noexcept
{
    const crd::usize n = p.size();
    T d = static_cast<T>(0);
    for (crd::usize j = 0; j < n; ++j)
    {
        d += g[j] * p[j];
    }
    for (crd::usize i = 0; i < ce.size(); ++i)
    {
        T jp = static_cast<T>(0);
        for (crd::usize j = 0; j < n; ++j)
        {
            jp += je[i * n + j] * p[j];
        }
        if (ce[i] > static_cast<T>(0))
        {
            d += nu * jp;
        }
        else if (ce[i] < static_cast<T>(0))
        {
            d -= nu * jp;
        }
        else
        {
            d += nu * std::fabs(jp);
        }
    }
    for (crd::usize i = 0; i < ci.size(); ++i)
    {
        T jp = static_cast<T>(0);
        for (crd::usize j = 0; j < n; ++j)
        {
            jp += ji[i * n + j] * p[j];
        }
        if (ci[i] < static_cast<T>(0))
        {
            d -= nu * jp;
        }
        else if (ci[i] == static_cast<T>(0) && jp < static_cast<T>(0))
        {
            d -= nu * jp;
        }
    }
    return d;
}

} // namespace crd::hesap::opt
