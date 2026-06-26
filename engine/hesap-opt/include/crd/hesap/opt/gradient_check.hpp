#pragma once

// gradient_check.hpp — Phase 3.1.6 v7-b: the gradient-check harness every elite optimizer ships (Ceres
// `CheckGradients`, scipy `check_grad`, PyTorch `gradcheck`). Compares an objective's ANALYTIC gradient() against
// a central finite difference and reports the worst componentwise error — the standard way to catch a hand-derived
// gradient bug before it silently wrecks a solve. ADR-0090.
//
// THRESHOLD GUIDANCE (advisor-pinned): central-difference accuracy floors at ~ε^(2/3) (≈1e-10 for f64) and the
// relative agreement at ~1e-7; a correct gradient gives max_rel_err ~1e-7, a wrong one ~1e-1. Pass at
// max_rel_err < 1e-5 (well clear of the FD noise floor, well below a real bug). This is a DIAGNOSTIC, not a moat
// gate — FD noise is deterministic but not machine-exact, so do NOT assert bit-equality here.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/finite_difference.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::opt
{

template <typename T>
struct GradCheckResult
{
    T          max_abs_err = static_cast<T>(0); // max_i |g_analytic_i − g_fd_i|
    T          max_rel_err = static_cast<T>(0); // max_i |Δ_i| / max(|g_a_i|, |g_fd_i|, 1)
    crd::usize worst_index = 0;                 // argmax of the relative error
};

// Compare obj.gradient(x) against a central finite difference at x. Requires obj.has_gradient() (it is the
// analytic gradient being checked). Allocates two n-vectors from `alloc`.
template <typename T>
[[nodiscard]] inline GradCheckResult<T> gradient_check(const Objective<T>& obj, crd::containers::ConstSpan<T> x,
                                                       crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(obj.has_gradient(), "gradient_check: objective has no analytic gradient to check");
    const crd::usize n = obj.n();

    crd::containers::Array<T> ga(alloc);
    crd::containers::Array<T> gf(alloc);
    ga.resize(n);
    gf.resize(n);

    [[maybe_unused]] const bool ok = obj.gradient(x, {ga.data(), n});
    CRD_ASSERT_MSG(ok, "gradient_check: gradient() returned false despite has_gradient()==true");
    finite_difference_gradient<T>(obj, x, {gf.data(), n}, FdMode::Central, alloc);

    GradCheckResult<T> res;
    for (crd::usize i = 0; i < n; ++i)
    {
        const T abs_err = crd::math::fabs(ga[i] - gf[i]);
        T denom = crd::math::fabs(ga[i]);
        if (crd::math::fabs(gf[i]) > denom)
        {
            denom = crd::math::fabs(gf[i]);
        }
        if (denom < static_cast<T>(1))
        {
            denom = static_cast<T>(1);
        }
        const T rel_err = abs_err / denom;
        if (abs_err > res.max_abs_err)
        {
            res.max_abs_err = abs_err;
        }
        if (rel_err > res.max_rel_err)
        {
            res.max_rel_err = rel_err;
            res.worst_index = i;
        }
    }
    return res;
}

} // namespace crd::hesap::opt
