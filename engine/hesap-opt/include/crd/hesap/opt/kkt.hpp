#pragma once

// kkt.hpp — Phase 3.1.6 v7-j: the KKT machinery every constrained solver (v7-k QP, v7-n NLP) shares:
//
//   • KktResidual — the FOUR-PART first-order certificate at (x, λ, μ) for min f s.t. c_E = 0, c_I ≥ 0:
//       stationarity     ‖∇f − J_Eᵀλ − J_Iᵀμ‖∞      (the Lagrangian gradient)
//       primal           max(‖c_E‖∞, ‖[−c_I]₊‖∞)     (constraint violation)
//       dual             ‖[−μ]₊‖∞                     (μ ≥ 0)
//       complementarity  ‖μ ∘ c_I‖∞
//     `max()` of the four is the scalar stopping quantity solvers report in OptResult::kkt_residual — the
//     same certificate IPOPT/OSQP print. (Convention: L = f − λᵀc_E − μᵀc_I; constraints.hpp.)
//
//   • solve_kkt_dense — ONE equality-constrained-QP / Newton-SQP step: the symmetric saddle system
//       [ W   J_Eᵀ ] [ p ]   [ −g ]
//       [ J_E  0   ] [ z ] = [ −c ]        (λ⁺ = −z under the L = f − λᵀc convention)
//     factored with the v0e dense **Bunch-Kaufman LDLᵀ** (the same device IPOPT uses via MA57), with the
//     **INERTIA TEST** read off D's 1×1/2×2 blocks — a valid EQP minimizer needs inertia (n+, m−, 0) — and the
//     IPOPT-style ladder when it fails: δ·I on the W block (×100 per retry), −γ·I on the constraint block when
//     the factor is singular (rank-deficient J_E). ADR-0090; N&W §16.1-16.2, §18.1; Wächter-Biegler (ICM).
//
//   • estimate_eq_multipliers — the least-squares multipliers λ̂ = argmin ‖∇f − J_Eᵀλ‖₂ (QR-backed dense::lstsq)
//     for initializing/reporting multipliers when only x is known.
//
// DETERMINISM: Bunch-Kaufman, lstsq, and every loop here are serial fixed-order arithmetic ⇒ moat-safe.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/ldlt.hpp>
#include <crd/hesap/dense/lstsq.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/opt/constraints.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::opt
{

template <typename T> struct KktResidual
{
    T stationarity = static_cast<T>(0);    // ‖∇f − J_Eᵀλ − J_Iᵀμ‖∞
    T primal = static_cast<T>(0);          // max(‖c_E‖∞, ‖[−c_I]₊‖∞)
    T dual = static_cast<T>(0);            // ‖[−μ]₊‖∞
    T complementarity = static_cast<T>(0); // ‖μ ∘ c_I‖∞

    [[nodiscard]] T max() const noexcept
    {
        T m = stationarity > primal ? stationarity : primal;
        m = m > dual ? m : dual;
        return m > complementarity ? m : complementarity;
    }
};

// The four-part KKT residual at (x, λ, μ). Needs obj.has_gradient() + cons.has_jacobians(). Scratch from alloc.
template <typename T>
[[nodiscard]] KktResidual<T> compute_kkt_residual(const Objective<T>& obj, const Constraints<T>& cons,
                                                  crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> lambda,
                                                  crd::containers::ConstSpan<T> mu, crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(obj.has_gradient() && cons.has_jacobians(),
                   "compute_kkt_residual needs the gradient + constraint Jacobians");
    const crd::usize n = obj.n();
    const crd::usize me = cons.num_eq();
    const crd::usize mi = cons.num_ineq();
    CRD_ASSERT_MSG(lambda.size() == me && mu.size() == mi, "compute_kkt_residual: multiplier size mismatch");

    crd::containers::Array<T> g(alloc);
    crd::containers::Array<T> ce(alloc);
    crd::containers::Array<T> ci(alloc);
    crd::containers::Array<T> je(alloc);
    crd::containers::Array<T> ji(alloc);
    g.resize(n);
    ce.resize(me);
    ci.resize(mi);
    je.resize(me * n);
    ji.resize(mi * n);
    (void)obj.gradient(x, {g.data(), n});
    cons.eval(x, {ce.data(), me}, {ci.data(), mi});
    (void)cons.jacobians(x, {je.data(), me * n}, {ji.data(), mi * n});

    KktResidual<T> r;
    for (crd::usize j = 0; j < n; ++j) // ∇L_j = g_j − Σλ_i J_E[i,j] − Σμ_i J_I[i,j]
    {
        T acc = g[j];
        for (crd::usize i = 0; i < me; ++i)
        {
            acc -= lambda[i] * je[i * n + j];
        }
        for (crd::usize i = 0; i < mi; ++i)
        {
            acc -= mu[i] * ji[i * n + j];
        }
        const T a = crd::math::fabs(acc);
        r.stationarity = a > r.stationarity ? a : r.stationarity;
    }
    for (crd::usize i = 0; i < me; ++i)
    {
        const T a = crd::math::fabs(ce[i]);
        r.primal = a > r.primal ? a : r.primal;
    }
    for (crd::usize i = 0; i < mi; ++i)
    {
        const T viol = ci[i] < static_cast<T>(0) ? -ci[i] : static_cast<T>(0);
        r.primal = viol > r.primal ? viol : r.primal;
        const T dviol = mu[i] < static_cast<T>(0) ? -mu[i] : static_cast<T>(0);
        r.dual = dviol > r.dual ? dviol : r.dual;
        const T comp = crd::math::fabs(mu[i] * ci[i]);
        r.complementarity = comp > r.complementarity ? comp : r.complementarity;
    }
    return r;
}

// Result of one dense KKT solve: whether the factor succeeded, the regularizations applied, and whether the
// inertia had to be corrected (δ > 0 ⇒ the reduced Hessian was not positive definite at this point).
template <typename T> struct KktSolveInfo
{
    bool solved = false;
    T delta = static_cast<T>(0); // δ added to the W block
    T gamma = static_cast<T>(0); // γ added (negated) to the constraint block
    bool inertia_corrected = false;
};

namespace detail
{
// Inertia (n+, n−, n0) of D from a Bunch-Kaufman factor: 1×1 block → sign of d; 2×2 block [a b; b c] →
// det < 0 ⇒ one of each; det > 0 ⇒ two of sign(a) (a ≠ 0 in a B-K 2×2 pivot); det = 0 ⇒ a zero eigenvalue.
template <typename T>
inline void ldlt_inertia(const crd::hesap::dense::LDLT<T>& f, crd::usize& pos, crd::usize& neg,
                         crd::usize& zero) noexcept
{
    pos = 0;
    neg = 0;
    zero = 0;
    const crd::usize n = f.n();
    for (crd::usize k = 0; k < n; ++k)
    {
        const crd::u8 kind = f.block_kind(k);
        if (kind == 1U)
        {
            const T d = f.packed().at(k, k);
            if (d > static_cast<T>(0))
            {
                ++pos;
            }
            else if (d < static_cast<T>(0))
            {
                ++neg;
            }
            else
            {
                ++zero;
            }
        }
        else if (kind == 2U)
        {
            const T a = f.packed().at(k, k);
            const T b = f.packed().at(k + 1, k);
            const T c = f.packed().at(k + 1, k + 1);
            const T det = a * c - b * b;
            if (det < static_cast<T>(0))
            {
                ++pos;
                ++neg;
            }
            else if (det > static_cast<T>(0))
            {
                if (a > static_cast<T>(0))
                {
                    pos += 2;
                }
                else
                {
                    neg += 2;
                }
            }
            else
            {
                ++zero;
                if (a + c > static_cast<T>(0))
                {
                    ++pos;
                }
                else if (a + c < static_cast<T>(0))
                {
                    ++neg;
                }
                else
                {
                    ++zero;
                }
            }
        }
    }
}
} // namespace detail

// Solve ONE dense saddle KKT system  [W J_Eᵀ; J_E 0]·[p; z] = [−g; −c]  and return p + the NEW multipliers
// λ⁺ = −z. `w` is n×n row-major (the Lagrangian Hessian at x), `je` is m×n row-major, `g` length n, `c` length
// m. Inertia-corrected per the header note; `info.delta > 0` means the step was computed on a δ-regularized
// W (the SQP driver may treat it like the modified-Newton τ path).
template <typename T>
[[nodiscard]] KktSolveInfo<T> solve_kkt_dense(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> w,
                                              crd::containers::ConstSpan<T> je, crd::containers::ConstSpan<T> g,
                                              crd::containers::ConstSpan<T> c, crd::containers::Span<T> p,
                                              crd::containers::Span<T> lambda_new)
{
    namespace dn = crd::hesap::dense;
    const crd::usize n = g.size();
    const crd::usize m = c.size();
    CRD_ASSERT_MSG(w.size() == n * n && je.size() == m * n && p.size() == n && lambda_new.size() == m,
                   "solve_kkt_dense: span size mismatch");
    const crd::usize nk = n + m;

    KktSolveInfo<T> info;
    if (nk == 0)
    {
        info.solved = true;
        return info;
    }

    // Scale anchor for the regularization ladders.
    T wscale = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T a = crd::math::fabs(w[i * n + i]);
        wscale = a > wscale ? a : wscale;
    }
    wscale = wscale > static_cast<T>(1) ? wscale : static_cast<T>(1);
    const T delta0 = static_cast<T>(1e-8) * wscale;
    const T gamma0 = crd::math::sqrt(std::numeric_limits<T>::epsilon()) * wscale;

    crd::containers::Array<T> rhs(alloc);
    rhs.resize(nk);

    T delta = static_cast<T>(0);
    T gamma = static_cast<T>(0);
    for (int attempt = 0; attempt < 12; ++attempt)
    {
        dn::Symmetric<T> kmat(alloc, nk);
        for (crd::usize i = 0; i < n; ++i) // W + δI block (lower triangle)
        {
            for (crd::usize j = 0; j <= i; ++j)
            {
                kmat.at(i, j) = w[i * n + j];
            }
            kmat.at(i, i) += delta;
        }
        for (crd::usize i = 0; i < m; ++i) // J_E block + −γI block
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                kmat.at(n + i, j) = je[i * n + j];
            }
            kmat.at(n + i, n + i) = -gamma;
        }

        dn::LDLT<T> f(alloc, nk);
        dn::factor_ldlt<T, dn::Layout::RowMajor>(f, kmat);
        crd::usize pos = 0;
        crd::usize neg = 0;
        crd::usize zero = 0;
        bool ok = f.info() == 0;
        if (ok)
        {
            detail::ldlt_inertia<T>(f, pos, neg, zero);
            ok = pos == n && neg == m && zero == 0; // the EQP-minimizer inertia condition
        }
        if (!ok)
        {
            if (f.info() != 0 || zero > 0)
            {
                gamma = gamma > static_cast<T>(0) ? gamma * static_cast<T>(10) : gamma0; // singular ⇒ J_E reg
            }
            delta = delta > static_cast<T>(0) ? delta * static_cast<T>(100) : delta0; // wrong inertia ⇒ W reg
            info.inertia_corrected = true;
            continue;
        }

        for (crd::usize i = 0; i < n; ++i)
        {
            rhs[i] = -g[i];
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            rhs[n + i] = -c[i];
        }
        dn::solve_ldlt<T, dn::Layout::RowMajor>(f, {rhs.data(), nk});
        for (crd::usize i = 0; i < n; ++i)
        {
            p[i] = rhs[i];
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            lambda_new[i] = -rhs[n + i]; // λ⁺ = −z (the L = f − λᵀc convention)
        }
        info.solved = true;
        info.delta = delta;
        info.gamma = gamma;
        return info;
    }
    info.delta = delta;
    info.gamma = gamma;
    return info; // solved == false: the regularization ladder ran out (pathological scaling)
}

// Least-squares equality multipliers λ̂ = argmin ‖∇f(x) − J_E(x)ᵀλ‖₂ (the standard multiplier estimate when
// only x is known — solver init + reporting). Needs obj.has_gradient() + cons.has_jacobians().
template <typename T>
void estimate_eq_multipliers(const Objective<T>& obj, const Constraints<T>& cons, crd::containers::ConstSpan<T> x,
                             crd::containers::Span<T> lambda, crd::memory::IAllocator* alloc)
{
    namespace dn = crd::hesap::dense;
    CRD_ASSERT_MSG(obj.has_gradient() && cons.has_jacobians(),
                   "estimate_eq_multipliers needs the gradient + constraint Jacobians");
    const crd::usize n = obj.n();
    const crd::usize me = cons.num_eq();
    CRD_ASSERT_MSG(lambda.size() == me, "estimate_eq_multipliers: lambda size mismatch");
    if (me == 0)
    {
        return;
    }

    crd::containers::Array<T> g(alloc);
    crd::containers::Array<T> je(alloc);
    crd::containers::Array<T> ci(alloc);
    g.resize(n);
    je.resize(me * n);
    ci.resize(cons.num_ineq());
    crd::containers::Array<T> ce(alloc);
    ce.resize(me);
    (void)obj.gradient(x, {g.data(), n});
    crd::containers::Array<T> ji(alloc);
    ji.resize(cons.num_ineq() * n);
    (void)cons.jacobians(x, {je.data(), me * n}, {ji.data(), ji.size()});

    dn::Matrix<T> a(alloc, n, me); // J_Eᵀ (n × me)
    dn::Vector<T> b(alloc, n);
    for (crd::usize i = 0; i < me; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            a.at(j, i) = je[i * n + j];
        }
    }
    for (crd::usize j = 0; j < n; ++j)
    {
        b.data()[j] = g[j];
    }
    const dn::LstSq<T> sol = dn::lstsq<T>(alloc, a, b, dn::LstSqMethod::Auto, static_cast<T>(-1),
                                          /*with_residual=*/false);
    for (crd::usize i = 0; i < me; ++i)
    {
        lambda[i] = sol.x.at(i, 0);
    }
}

} // namespace crd::hesap::opt
