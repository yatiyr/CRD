#pragma once

// opt_types.hpp — Phase 3.1.6 v7-a: shared optimization result/options/status types. ADR-0090.
// T is real (f32/f64) — the optimization domain has no complex scalars (unlike eig). Tolerances are T.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::opt
{

enum class OptStatus : crd::u8
{
    Success = 0,          // a convergence criterion was met
    MaxIterations = 1,    // hit the iteration cap before converging
    LineSearchFailed = 2, // the line search could not find a sufficient-decrease step
    NotFinite = 3,        // f or ∇f became NaN/Inf
    SmallStep = 4,        // the step shrank below step_tol without the gradient criterion (stall)
};

template <typename T> struct OptOptions
{
    crd::usize max_iters = 1000;
    // Stop when ‖∇f(x)‖∞ ≤ grad_tol (the primary first-order optimality test). Default ≈ √eps.
    T grad_tol = std::sqrt(std::numeric_limits<T>::epsilon());
    // Stop when ‖Δx‖ ≤ step_tol·(1 + ‖x‖) (stall guard). 0 ⇒ disabled.
    T step_tol = static_cast<T>(0);
    // Stop when |Δf| ≤ func_tol·(1 + |f|) (flat-objective guard). 0 ⇒ disabled.
    T func_tol = static_cast<T>(0);
    bool record_history = false; // populate OptResult::history (f at each iteration)
};

template <typename T> struct OptResult
{
    crd::containers::Array<T> x;     // the minimizer
    T fx = static_cast<T>(0);        // f(x*)
    T grad_norm = static_cast<T>(0); // ‖∇f(x*)‖∞
    crd::usize iterations = 0;
    crd::usize fn_evals = 0;   // objective value() calls (≈ points evaluated — the L-BFGS verdict
                               // metric vs liblbfgs; wall-clock for quasi-Newton is ~iters×evals)
    crd::usize grad_evals = 0; // objective gradient() calls
    crd::usize hess_evals = 0; // v7-g: hessian()/sparse_hessian() calls (full/modified/sparse Newton)
                               // — for Newton-CG it counts hessian_vector() products instead
    OptStatus status = OptStatus::MaxIterations;
    bool converged = false;            // status == Success
    crd::containers::Array<T> history; // f per iteration, iff record_history
    // ---- v7-j constrained fields (empty / 0 for unconstrained methods) ----
    crd::containers::Array<T> multipliers; // [λ (equality) ; μ (inequality)] at the returned x
    T kkt_residual = static_cast<T>(0);    // max of the 4-part KKT certificate (kkt.hpp)

    explicit OptResult(crd::memory::IAllocator* alloc) noexcept : x(alloc), history(alloc), multipliers(alloc) {}
};

} // namespace crd::hesap::opt
