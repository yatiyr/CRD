#pragma once

// integrate.hpp — Phase 3.1.6 v9-a: the fixed-step driver — the wiring proof that the two API layers
// compose (a thin counting loop over the steppers.hpp kernels through the OdeFunction contract). The
// adaptive drivers (v9-b explicit, v9-d stiff) follow this same shape. ADR-0091.
//
// DETERMINISM: step times are RECOMPUTED as t_i = t0 + i·h (never accumulated — no drift, fixed FP ops);
// the final step lands on t1 exactly (h_last = t1 − t_{N−1}). Bit-identical run-to-run by construction.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/ode/ode_function.hpp>
#include <crd/hesap/ode/ode_types.hpp>
#include <crd/hesap/ode/steppers.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::ode
{

enum class FixedMethod : crd::u8
{
    Euler = 0,    // order 1, 1 fev/step
    Midpoint = 1, // order 2, 2 fev/step
    Rk4 = 2,      // order 4, 4 fev/step
};

// Advance y from t0 to t1 in `nsteps` equal steps with the chosen kernel. `y` is in-out (y(t0) on entry,
// y(t1) on success). The driver allocates the kernel scratch once from `alloc`; the per-step state check
// (NotFinite) lives HERE, not in the kernels — the hot-loop layer stays check-free.
template <typename T>
[[nodiscard]] OdeResult<T> integrate_fixed(const OdeFunction<T>& f, T t0, T t1, crd::usize nsteps,
                                           crd::containers::Span<T> y, crd::memory::IAllocator* alloc,
                                           FixedMethod method = FixedMethod::Rk4)
{
    const crd::usize n = f.dim();
    CRD_ASSERT(y.size() == n);
    CRD_ASSERT(alloc != nullptr);

    OdeResult<T> result;
    result.t = t0;

    if (!std::isfinite(t0) || !std::isfinite(t1) || (nsteps == 0 && t1 != t0))
    {
        result.status = OdeStatus::InvalidInput;
        return result;
    }
    if (t1 == t0 || n == 0)
    {
        result.status = OdeStatus::Success;
        result.success = true;
        result.t = t1;
        return result;
    }

    crd::containers::Array<T> scratch(alloc);
    scratch.resize(rk4_scratch(n)); // the max of the three; one allocation regardless of method

    // The kernel-facing RHS adapter: counts evaluations (the driver owns ALL work counting).
    auto rhs = [&f, &result](T t, crd::containers::ConstSpan<T> yy, crd::containers::Span<T> dydt)
    {
        f.rhs(t, yy, dydt);
        ++result.work.nfev;
    };

    const T h = (t1 - t0) / static_cast<T>(nsteps);
    const crd::containers::ConstSpan<T> y_in(y.data(), n);
    const crd::containers::Span<T> sc(scratch.data(), scratch.size());

    for (crd::usize i = 0; i < nsteps; ++i)
    {
        const T t = t0 + static_cast<T>(i) * h;
        // Land the final step on t1 EXACTLY (recomputed-t determinism; no accumulation drift).
        const T hi = (i + 1 == nsteps) ? (t1 - t) : h;

        switch (method)
        {
            case FixedMethod::Euler:
                step_euler(rhs, t, y_in, hi, y, sc);
                break;
            case FixedMethod::Midpoint:
                step_midpoint(rhs, t, y_in, hi, y, sc);
                break;
            case FixedMethod::Rk4:
                step_rk4(rhs, t, y_in, hi, y, sc);
                break;
        }
        ++result.work.nsteps;
        ++result.work.naccept;

        for (crd::usize j = 0; j < n; ++j)
        {
            if (!std::isfinite(y[j]))
            {
                result.status = OdeStatus::NotFinite;
                result.t = t0 + static_cast<T>(i + 1) * h;
                return result;
            }
        }
    }

    result.status = OdeStatus::Success;
    result.success = true;
    result.t = t1;
    return result;
}

} // namespace crd::hesap::ode
