#pragma once

// finite_difference.hpp — Phase 3.1.6 v7-b: finite-difference gradient over the value-only Objective<T>. This is
// the fallback that lets EVERY method from v7-c on (L-BFGS, CG, LM, …) run on a user objective that supplies only
// value() — no analytic gradient, no scalar-generic functor. ADR-0090.
//
// STEP SELECTION (advisor-pinned — a FIXED absolute step is silently wrong on badly-scaled variables): the step is
// RELATIVE to each component's magnitude. Forward difference h_i = √ε·max(|x_i|,1) (error O(h) ~ √ε); central
// difference h_i = ε^(1/3)·max(|x_i|,1) (error O(h²) ~ ε^(2/3) ≈ 1e-10 for f64). We also recover the TRUE step the
// FP grid actually used (xph − x_i) so the divisor matches the perturbation exactly.
//
// DETERMINISM: this is serial scalar arithmetic over value() calls. If value() is bit-exact across worker counts
// (e.g. a ParallelSparseLinearOp objective), the FD gradient is too — the composition v7-d's L-BFGS moat rests on.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::opt
{

enum class FdMode : crd::u8
{
    Forward, // n+1 evals, O(h) accuracy
    Central, // 2n  evals, O(h²) accuracy (the default — worth the extra n evals)
};

namespace detail
{
template <typename T>
[[nodiscard]] inline T fd_step(T xi, T base) noexcept
{
    const T scale = std::fabs(xi) > static_cast<T>(1) ? std::fabs(xi) : static_cast<T>(1);
    return base * scale;
}
} // namespace detail

// ∇f(x) ≈ finite differences of `obj.value` → g (g.size() == x.size() == obj.n()). Allocates an x-copy from
// `alloc`. Works on any Objective<T> (gradient capability NOT required — that's the point).
template <typename T>
inline void finite_difference_gradient(const Objective<T>& obj, crd::containers::ConstSpan<T> x,
                                       crd::containers::Span<T> g, FdMode mode, crd::memory::IAllocator* alloc)
{
    const crd::usize n = obj.n();
    crd::containers::Array<T> xp(alloc);
    xp.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        xp[i] = x[i];
    }

    const T eps = std::numeric_limits<T>::epsilon();
    if (mode == FdMode::Forward)
    {
        const T base = std::sqrt(eps);
        const T f0 = obj.value({xp.data(), n});
        for (crd::usize i = 0; i < n; ++i)
        {
            const T xi = xp[i];
            const T h = detail::fd_step<T>(xi, base);
            xp[i] = xi + h;
            const T h_true = xp[i] - xi; // the step the FP grid actually realized
            const T fp = obj.value({xp.data(), n});
            g[i] = (fp - f0) / h_true;
            xp[i] = xi;
        }
    }
    else
    {
        const T base = std::cbrt(eps);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T xi = xp[i];
            const T h = detail::fd_step<T>(xi, base);
            xp[i] = xi + h;
            const T xph = xp[i];
            xp[i] = xi - h;
            const T xmh = xp[i];
            const T fm = obj.value({xp.data(), n});
            xp[i] = xph;
            const T fp = obj.value({xp.data(), n});
            g[i] = (fp - fm) / (xph - xmh);
            xp[i] = xi;
        }
    }
}

// FiniteDiffObjective<T> — wraps a value-only Objective<T> and exposes gradient() via finite differences, so it
// drops into any gradient-based optimizer transparently. The inner objective must outlive this wrapper.
template <typename T>
class FiniteDiffObjective final : public Objective<T>
{
public:
    FiniteDiffObjective(const Objective<T>& inner, crd::memory::IAllocator* alloc, FdMode mode = FdMode::Central)
        : Objective<T>(/*has_gradient=*/true, /*has_hessian_vector=*/false)
        , m_inner(&inner)
        , m_alloc(alloc)
        , m_mode(mode)
    {
    }

    [[nodiscard]] T value(crd::containers::ConstSpan<T> x) const override { return m_inner->value(x); }

    [[nodiscard]] crd::usize n() const noexcept override { return m_inner->n(); }

    [[nodiscard]] bool gradient(crd::containers::ConstSpan<T> x, crd::containers::Span<T> g) const override
    {
        finite_difference_gradient<T>(*m_inner, x, g, m_mode, m_alloc);
        return true;
    }

private:
    const Objective<T>*       m_inner;
    crd::memory::IAllocator*  m_alloc;
    FdMode                    m_mode;
};

} // namespace crd::hesap::opt
