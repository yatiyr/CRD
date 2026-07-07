#pragma once

// forward_ad.hpp — Phase 3.1.6 v7-b: EXACT gradients by forward-mode automatic differentiation. Given a
// scalar-generic functor f (one templated `operator()(ConstSpan<S>) const` that runs on both T and Dual<T>),
// seeding component i with derivative 1 and evaluating f returns ∂f/∂x_i exactly — no truncation error, no
// step-size tuning (the finite-difference weaknesses). n passes for an R^n→R gradient (one tangent per pass);
// vector-mode (several tangents per pass) is a perf refinement, not v7-b. ADR-0090.
//
// This is the "elite" exact-derivative path. Reverse-mode AD (the BA/ML workhorse, O(1)× cost for R^n→R) is the
// separate ADR-0065 autodiff module and plugs into the SAME Objective gradient interface when it ships — forward
// mode + finite differences are the v7-b providers.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/autodiff/forward.hpp>
#include <crd/hesap/opt/dual.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/memory/allocator.hpp>

#include <concepts>
#include <utility>

namespace crd::hesap::opt
{

// DiffFunctor migrated to its canonical home in crd-hesap-autodiff (ADR-0097 §1); re-exported so `opt::DiffFunctor`
// and the requires-clauses below still name it — zero regressions for the Dual migration.
using autodiff::forward::DiffFunctor;

// Fused value + gradient: returns f(x) and fills g with ∇f(x), both exact. `g.size() == x.size()`. Allocates an
// n-vector of Dual<T> from `alloc`. (This IS the value_and_gradient the Objective vtable reserves — forward AD
// produces both in one sweep.)
template <typename T, typename F>
    requires DiffFunctor<F, T>
inline T forward_ad_gradient(const F& f, crd::containers::ConstSpan<T> x, crd::containers::Span<T> g,
                             crd::memory::IAllocator* alloc)
{
    const crd::usize n = x.size();
    crd::containers::Array<Dual<T>> xd(alloc);
    xd.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        xd[i] = Dual<T>{x[i], static_cast<T>(0)};
    }

    // Explicit span (braced-init can't deduce the functor's ConstSpan<S> parameter).
    const crd::containers::ConstSpan<Dual<T>> xspan{xd.data(), n};
    T value = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        xd[i].d = static_cast<T>(1);                       // seed the i-th tangent
        const Dual<T> r = f(xspan);
        g[i] = r.d;                                        // ∂f/∂x_i
        value = r.v;                                       // f(x) (identical across passes)
        xd[i].d = static_cast<T>(0);                       // un-seed
    }
    if (n == 0)
    {
        value = f(xspan).v;
    }
    return value;
}

// FunctorObjective<F, T> — adapts a scalar-generic functor into the virtual Objective<T> the optimizer consumes:
// value() runs the real path, gradient() runs forward-mode AD (has_gradient() == true). The functor is held BY
// VALUE (copied in). Its `operator()` MUST be const (value()/gradient() are const). ADR-0090 §6: AD plugs into
// the same Objective interface.
template <typename F, typename T>
class FunctorObjective final : public Objective<T>
{
public:
    FunctorObjective(F f, crd::usize n, crd::memory::IAllocator* alloc)
        : Objective<T>(/*has_gradient=*/true, /*has_hessian_vector=*/false)
        , m_f(std::move(f))
        , m_n(n)
        , m_alloc(alloc)
    {
    }

    [[nodiscard]] T value(crd::containers::ConstSpan<T> x) const override { return m_f(x); }

    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }

    [[nodiscard]] bool gradient(crd::containers::ConstSpan<T> x, crd::containers::Span<T> g) const override
    {
        (void)forward_ad_gradient<T>(m_f, x, g, m_alloc);
        return true;
    }

private:
    F                        m_f;
    crd::usize               m_n;
    crd::memory::IAllocator* m_alloc;
};

// Build a FunctorObjective from a scalar-generic functor. `T` is explicit (the scalar to optimize in), `F` deduced.
template <typename T, typename F>
[[nodiscard]] inline FunctorObjective<F, T> make_objective_from_functor(F f, crd::usize n,
                                                                        crd::memory::IAllocator* alloc)
{
    return FunctorObjective<F, T>(std::move(f), n, alloc);
}

} // namespace crd::hesap::opt
