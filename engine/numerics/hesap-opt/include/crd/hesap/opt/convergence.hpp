#pragma once

// convergence.hpp — Phase 3.1.6 v7-a: the shared first-order stopping test. ADR-0090. Pure scalar comparisons
// ⇒ deterministic / thread-count independent.

#include <crd/hesap/opt/opt_types.hpp>

#include <crd/math/cmath.hpp>
#include <optional>

namespace crd::hesap::opt
{

// Decide whether to stop after a step. Returns the terminating OptStatus, or nullopt to continue.
//   grad_norm = ‖∇f‖∞ at the new point · step_norm = ‖Δx‖ · df = |f_new − f_old| · x_norm = ‖x_new‖ · fx = f_new.
template <typename T>
[[nodiscard]] inline std::optional<OptStatus> check_convergence(T grad_norm, T step_norm, T df, T x_norm, T fx,
                                                                const OptOptions<T>& opts) noexcept
{
    if (!std::isfinite(grad_norm) || !std::isfinite(fx))
    {
        return OptStatus::NotFinite;
    }
    if (grad_norm <= opts.grad_tol) // the primary first-order optimality test
    {
        return OptStatus::Success;
    }
    if (opts.func_tol > static_cast<T>(0) && df <= opts.func_tol * (static_cast<T>(1) + crd::math::fabs(fx)))
    {
        return OptStatus::Success; // objective flattened out
    }
    if (opts.step_tol > static_cast<T>(0) && step_norm <= opts.step_tol * (static_cast<T>(1) + x_norm))
    {
        return OptStatus::SmallStep; // step stalled without meeting the gradient test
    }
    return std::nullopt;
}

} // namespace crd::hesap::opt
