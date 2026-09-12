#pragma once

// line_search.hpp — Phase 3.1.6 v7-a: the LineSearch<T> interface + a minimal backtracking-Armijo. ADR-0090.
// The elite line searches (Wolfe / strong-Wolfe / More-Thuente) land in v7-c on this interface.
//
// COST CONTRACT (advisor-pinned): a line search MAY evaluate the gradient at trial points (Wolfe needs gᵀp there)
// and return it via `g_out` with `grad_at_new_valid=true`; a value-only search (Armijo) leaves `g_out` untouched
// and sets `grad_at_new_valid=false`, and the OPTIMIZER then evaluates ∇f once at the accepted point. This keeps
// the per-iteration gradient count explicit for L-BFGS / CG (which care).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/opt/objective.hpp>

#include <cmath>

namespace crd::hesap::opt
{

template <typename T>
struct LineSearchResult
{
    T          alpha = static_cast<T>(0);   // the accepted step length
    T          fx_new = static_cast<T>(0);  // f(x + alpha·p)
    bool       ok = false;                  // a sufficient-decrease step was found
    bool       grad_at_new_valid = false;   // true iff g_out holds ∇f(x + alpha·p)
    crd::usize evals = 0;                    // objective value() calls consumed
    crd::usize grad_evals = 0;               // objective gradient() calls consumed
};

template <typename T>
class LineSearch
{
public:
    virtual ~LineSearch() = default;
    // Find alpha along the descent direction p from x (with value fx and gradient g). Writes the accepted point
    // x + alpha·p into x_out (length n); may write ∇f there into g_out (see the cost contract above).
    [[nodiscard]] virtual LineSearchResult<T> search(const Objective<T>& obj, crd::containers::ConstSpan<T> x, T fx,
                                                     crd::containers::ConstSpan<T> g, crd::containers::ConstSpan<T> p,
                                                     T alpha0, crd::containers::Span<T> x_out,
                                                     crd::containers::Span<T> g_out) const = 0;
};

// Backtracking line search enforcing only the Armijo sufficient-decrease condition
//   f(x + alpha·p) ≤ f(x) + c1·alpha·(∇f·p),   halving alpha until it holds (or max_iters).
// Value-only ⇒ leaves g_out untouched (the optimizer evaluates ∇f at the accepted point). Deterministic.
template <typename T>
class BacktrackingArmijo final : public LineSearch<T>
{
public:
    BacktrackingArmijo() = default;
    BacktrackingArmijo(T c1, T rho, crd::usize max_iters) noexcept : m_c1(c1), m_rho(rho), m_max_iters(max_iters) {}

    [[nodiscard]] LineSearchResult<T> search(const Objective<T>& obj, crd::containers::ConstSpan<T> x, T fx,
                                             crd::containers::ConstSpan<T> g, crd::containers::ConstSpan<T> p,
                                             T alpha0, crd::containers::Span<T> x_out,
                                             crd::containers::Span<T> g_out) const override
    {
        namespace dn = crd::hesap::dense;
        (void)g_out;
        const crd::usize n = x.size();
        const T dg = dn::dot<T>(g, p); // directional derivative ∇f·p (must be < 0 for a descent direction)
        LineSearchResult<T> r;
        T alpha = alpha0;
        for (crd::usize it = 0; it < m_max_iters; ++it)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                x_out[i] = x[i] + alpha * p[i];
            }
            const T fnew = obj.value(x_out);
            ++r.evals;
            if (std::isfinite(fnew) && fnew <= fx + m_c1 * alpha * dg)
            {
                r.alpha = alpha;
                r.fx_new = fnew;
                r.ok = true;
                return r;
            }
            alpha *= m_rho;
        }
        r.alpha = alpha;
        r.fx_new = fx;
        r.ok = false; // no sufficient-decrease step found
        return r;
    }

private:
    T          m_c1 = static_cast<T>(1e-4);
    T          m_rho = static_cast<T>(0.5);
    crd::usize m_max_iters = 50;
};

} // namespace crd::hesap::opt
