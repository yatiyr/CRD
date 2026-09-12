#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v4a -- iterative-solver result + options.
//
// `R` is the REAL scalar type (RealType<T>): tolerances and residual norms
// are always real, even for complex T. Templating on R (not storing f64)
// keeps an f32 solve's tolerances + history in f32 with no precision-mismatch
// trap (advisor 2026-05-25).
// -----------------------------------------------------------------------

enum class StopReason : crd::u8
{
    Converged     = 0,
    MaxIterations = 1,
    Breakdown     = 2, // a denominator (e.g. pᴴAp or ρ) underflowed to 0
    Stagnation    = 3, // residual stopped decreasing (reserved; v4c+)
};

// Per-type sensible default relative tolerance: f64 → 1e-10, f32 → 1e-5
// (f32 machine epsilon ~1.2e-7, so a tighter target never converges).
template <typename R>
inline constexpr R kDefaultRelTol = static_cast<R>(1e-10);
template <>
inline constexpr float kDefaultRelTol<float> = 1e-5F;

template <typename R>
struct IterativeOptions
{
    R          rel_tol          = kDefaultRelTol<R>; // stop when ‖r‖ ≤ rel_tol·‖r₀‖
    R          abs_tol          = static_cast<R>(0); // ... OR ‖r‖ ≤ abs_tol
    crd::usize max_iter         = 1000;
    bool       record_residuals = false; // populate IterativeResult::residual_history
};

template <typename R>
struct IterativeResult
{
    crd::usize                iterations          = 0;
    R                         final_residual_norm = static_cast<R>(0);
    bool                      converged           = false;
    StopReason                reason              = StopReason::MaxIterations;
    crd::containers::Array<R> residual_history; // ‖r₀‖, ‖r₁‖, …  iff record_residuals

    explicit IterativeResult(crd::memory::IAllocator* alloc) : residual_history(alloc) {}
};

} // namespace crd::hesap::iterative
