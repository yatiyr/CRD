#pragma once

#include <crd/hesap/iterative/iterative_result.hpp>

namespace crd::hesap::iterative
{
// Deterministic convergence test: ‖r‖ ≤ max(rel_tol·‖r₀‖, abs_tol).
// Pure comparison on real scalars -- no reduction, thread-count independent.
template <typename R>
[[nodiscard]] inline bool is_converged(R res, R res0, const IterativeOptions<R>& opts) noexcept
{
    const R rel_thresh = opts.rel_tol * res0;
    const R thresh     = (rel_thresh > opts.abs_tol) ? rel_thresh : opts.abs_tol;
    return res <= thresh;
}

} // namespace crd::hesap::iterative
