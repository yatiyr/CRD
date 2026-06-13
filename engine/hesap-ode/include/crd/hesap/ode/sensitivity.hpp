#pragma once

// sensitivity.hpp — Phase 3.1.6 v9-k: parameter sensitivities (the control/optimization pull — shooting,
// parameter estimation, optimal control). Two modes, both the CVODES patterns:
//
//   • FORWARD (CVODES simultaneous corrector): integrate the AUGMENTED state Y = [y; S] (S the n×np
//     sensitivity block, S[:,j] = ∂y/∂p_j) with the EXISTING ERK/BDF drivers. The augmented RHS is
//     [f ; J_y·s_j + ∂f/∂p_j]. For the stiff (BDF) path the augmented iteration "Jacobian" is BLOCK-DIAGONAL
//     with every block = J_y — the second-derivative coupling ∂(J_y·s_j)/∂y is intentionally dropped; the
//     RESIDUAL still uses the exact augmented RHS, so the converged solution is exact (the block-diagonal
//     matrix only changes Newton's CONVERGENCE RATE — CVODES does exactly this).
//
//   • ADJOINT (CVODES ASA): forward-solve y storing the dense-output trajectory, then integrate the adjoint
//     Λ = [λ; q] BACKWARD from t1 to t0: λ̇ = −J_yᵀ·λ (λ(t1) = ∂g/∂y(t1)ᵀ) and q̇_j = −λᵀ·∂f/∂p_j (q(t1)=0)
//     ⇒ q_j(t0) = ∫_{t0}^{t1} λᵀ ∂f/∂p_j dt = dg/dp_j (Lagrangian derivation; y0 ⊥ p assumed, the
//     λ(t0)ᵀ∂y0/∂p term is the named follow-on). First cut: dense J_y (so the transpose is free) + the FULL
//     stored forward dense output interpolated on the backward pass (true checkpoint/re-integration is a
//     memory optimization, not correctness — named). MOAT: pure deterministic FP, bit-identical run-twice.
//     ADR-0091.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/ode/bdf.hpp>
#include <crd/hesap/ode/erk.hpp>
#include <crd/hesap/ode/ode_function.hpp>
#include <crd/hesap/ode/ode_types.hpp>
#include <crd/hesap/ode/solution.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::ode
{

// The parameter-dependent RHS y' = f(t, y, p). All capabilities are required (no FD fallback here — the
// sensitivity equations need J_y and ∂f/∂p exactly; FD over those would defeat the purpose).
template <typename T> class ParametricOdeFunction
{
public:
    virtual ~ParametricOdeFunction() = default;
    virtual void rhs(T t, crd::containers::ConstSpan<T> y, crd::containers::ConstSpan<T> p,
                     crd::containers::Span<T> dydt) const = 0;
    // ∂f/∂y (n×n, ROW-MAJOR).
    virtual void jacobian_y(T t, crd::containers::ConstSpan<T> y, crd::containers::ConstSpan<T> p,
                            crd::containers::Span<T> jac) const = 0;
    // ∂f/∂p_j (column j of the parameter Jacobian) → out (size n).
    virtual void dfdp(T t, crd::containers::ConstSpan<T> y, crd::containers::ConstSpan<T> p, crd::usize j,
                      crd::containers::Span<T> out) const = 0;
    [[nodiscard]] virtual crd::usize dim() const noexcept = 0;
    [[nodiscard]] virtual crd::usize n_params() const noexcept = 0;
};

namespace detail
{

// Wraps a ParametricOdeFunction at FIXED p as a plain OdeFunction (the forward state solve in the adjoint
// pass, and the FD oracle). Carries the analytic dense Jacobian.
template <typename T> class ParamFixedFn final : public OdeFunction<T>
{
public:
    ParamFixedFn(const ParametricOdeFunction<T>& pfn, crd::containers::ConstSpan<T> p)
        : OdeFunction<T>(/*jac*/ true), m_pfn(&pfn), m_p(p)
    {
    }
    void rhs(T t, crd::containers::ConstSpan<T> y, crd::containers::Span<T> d) const override
    {
        m_pfn->rhs(t, y, m_p, d);
    }
    [[nodiscard]] bool jacobian(T t, crd::containers::ConstSpan<T> y, crd::containers::Span<T> j) const override
    {
        m_pfn->jacobian_y(t, y, m_p, j);
        return true;
    }
    [[nodiscard]] crd::usize dim() const noexcept override { return m_pfn->dim(); }

private:
    const ParametricOdeFunction<T>* m_pfn;
    crd::containers::ConstSpan<T> m_p;
};

// The augmented forward-sensitivity system over Y = [y; s_0; …; s_{np-1}] (size n·(1+np)).
template <typename T> class AugmentedSensitivityFn final : public OdeFunction<T>
{
public:
    AugmentedSensitivityFn(crd::memory::IAllocator* alloc, const ParametricOdeFunction<T>& pfn,
                           crd::containers::ConstSpan<T> p)
        : OdeFunction<T>(/*jac*/ true), m_pfn(&pfn), m_p(p), m_n(pfn.dim()), m_np(pfn.n_params()), m_jy(alloc),
          m_tmp(alloc)
    {
        m_jy.resize(m_n * m_n);
        m_tmp.resize(m_n);
    }
    void rhs(T t, crd::containers::ConstSpan<T> Y, crd::containers::Span<T> dY) const override
    {
        const crd::containers::ConstSpan<T> y(Y.data(), m_n);
        m_pfn->rhs(t, y, m_p, crd::containers::Span<T>(dY.data(), m_n));
        m_pfn->jacobian_y(t, y, m_p, crd::containers::Span<T>(m_jy.data(), m_n * m_n));
        for (crd::usize j = 0; j < m_np; ++j)
        {
            const T* sj = Y.data() + (1 + j) * m_n;
            T* dsj = dY.data() + (1 + j) * m_n;
            m_pfn->dfdp(t, y, m_p, j, crd::containers::Span<T>(m_tmp.data(), m_n)); // ∂f/∂p_j
            for (crd::usize i = 0; i < m_n; ++i)
            {
                T acc = static_cast<T>(0);
                for (crd::usize l = 0; l < m_n; ++l)
                {
                    acc += m_jy[i * m_n + l] * sj[l]; // (J_y·s_j)_i
                }
                dsj[i] = acc + m_tmp[i];
            }
        }
    }
    // Block-diagonal "Jacobian": every (1+np) diagonal block = J_y (CVODES simultaneous corrector).
    [[nodiscard]] bool jacobian(T t, crd::containers::ConstSpan<T> Y, crd::containers::Span<T> jac) const override
    {
        const crd::usize N = m_n * (1 + m_np);
        for (crd::usize i = 0; i < N * N; ++i)
        {
            jac[i] = static_cast<T>(0);
        }
        const crd::containers::ConstSpan<T> y(Y.data(), m_n);
        m_pfn->jacobian_y(t, y, m_p, crd::containers::Span<T>(m_jy.data(), m_n * m_n));
        for (crd::usize blk = 0; blk <= m_np; ++blk)
        {
            const crd::usize off = blk * m_n;
            for (crd::usize i = 0; i < m_n; ++i)
            {
                for (crd::usize l = 0; l < m_n; ++l)
                {
                    jac[(off + i) * N + (off + l)] = m_jy[i * m_n + l];
                }
            }
        }
        return true;
    }
    [[nodiscard]] crd::usize dim() const noexcept override { return m_n * (1 + m_np); }

private:
    const ParametricOdeFunction<T>* m_pfn;
    crd::containers::ConstSpan<T> m_p;
    crd::usize m_n;
    crd::usize m_np;
    mutable crd::containers::Array<T> m_jy;
    mutable crd::containers::Array<T> m_tmp;
};

// The adjoint backward system over Λ = [λ (n); q (np)]: λ̇ = −J_yᵀ·λ, q̇_j = −λᵀ·∂f/∂p_j. y(t) is
// interpolated from the stored forward dense output.
template <typename T> class AdjointFn final : public OdeFunction<T>
{
public:
    AdjointFn(crd::memory::IAllocator* alloc, const ParametricOdeFunction<T>& pfn, crd::containers::ConstSpan<T> p,
              const OdeSolution<T>& sol)
        : OdeFunction<T>(/*jac*/ true), m_pfn(&pfn), m_p(p), m_sol(&sol), m_n(pfn.dim()), m_np(pfn.n_params()),
          m_y(alloc), m_jy(alloc), m_tmp(alloc)
    {
        m_y.resize(m_n);
        m_jy.resize(m_n * m_n);
        m_tmp.resize(m_n);
    }
    void rhs(T t, crd::containers::ConstSpan<T> L, crd::containers::Span<T> dL) const override
    {
        m_sol->eval(t, crd::containers::Span<T>(m_y.data(), m_n));
        m_pfn->jacobian_y(t, crd::containers::ConstSpan<T>(m_y.data(), m_n), m_p,
                          crd::containers::Span<T>(m_jy.data(), m_n * m_n));
        // dλ/dt = −J_yᵀ·λ  ⇒  dL[i] = −Σ_l J_y[l,i]·λ[l].
        for (crd::usize i = 0; i < m_n; ++i)
        {
            T acc = static_cast<T>(0);
            for (crd::usize l = 0; l < m_n; ++l)
            {
                acc += m_jy[l * m_n + i] * L[l];
            }
            dL[i] = -acc;
        }
        // dq_j/dt = −λᵀ·∂f/∂p_j.
        for (crd::usize j = 0; j < m_np; ++j)
        {
            m_pfn->dfdp(t, crd::containers::ConstSpan<T>(m_y.data(), m_n), m_p, j,
                        crd::containers::Span<T>(m_tmp.data(), m_n));
            T acc = static_cast<T>(0);
            for (crd::usize i = 0; i < m_n; ++i)
            {
                acc += L[i] * m_tmp[i];
            }
            dL[m_n + j] = -acc;
        }
    }
    // Block lower-triangular: ∂(dλ)/∂λ = −J_yᵀ; ∂(dq_j)/∂λ_i = −∂f/∂p_j[i]; ∂(·)/∂q = 0.
    [[nodiscard]] bool jacobian(T t, crd::containers::ConstSpan<T>, crd::containers::Span<T> jac) const override
    {
        const crd::usize N = m_n + m_np;
        for (crd::usize i = 0; i < N * N; ++i)
        {
            jac[i] = static_cast<T>(0);
        }
        m_sol->eval(t, crd::containers::Span<T>(m_y.data(), m_n));
        m_pfn->jacobian_y(t, crd::containers::ConstSpan<T>(m_y.data(), m_n), m_p,
                          crd::containers::Span<T>(m_jy.data(), m_n * m_n));
        for (crd::usize i = 0; i < m_n; ++i)
        {
            for (crd::usize l = 0; l < m_n; ++l)
            {
                jac[i * N + l] = -m_jy[l * m_n + i]; // −J_yᵀ
            }
        }
        for (crd::usize j = 0; j < m_np; ++j)
        {
            m_pfn->dfdp(t, crd::containers::ConstSpan<T>(m_y.data(), m_n), m_p, j,
                        crd::containers::Span<T>(m_tmp.data(), m_n));
            for (crd::usize i = 0; i < m_n; ++i)
            {
                jac[(m_n + j) * N + i] = -m_tmp[i];
            }
        }
        return true;
    }
    [[nodiscard]] crd::usize dim() const noexcept override { return m_n + m_np; }

private:
    const ParametricOdeFunction<T>* m_pfn;
    crd::containers::ConstSpan<T> m_p;
    const OdeSolution<T>* m_sol;
    crd::usize m_n;
    crd::usize m_np;
    mutable crd::containers::Array<T> m_y;
    mutable crd::containers::Array<T> m_jy;
    mutable crd::containers::Array<T> m_tmp;
};

// Linear solver for the augmented sensitivity system: its iteration matrix (I − c·J_aug) is BLOCK-DIAGONAL
// with all (1+np) diagonal blocks IDENTICAL (= I − c·J_y, since J_y depends only on the state block). So
// factor the n×n block ONCE and apply it to each n-subvector of the RHS — the CVODES shared-factorization
// economy: O(n³) factor + (1+np)·O(n²) solves instead of factoring the full (n·(1+np))² matrix. The result
// is BIT-IDENTICAL to a full block-diagonal solve (the matrix IS block-diagonal). v9-k.
template <typename T> class BlockDiagonalOdeLinearSolver final : public OdeLinearSolver<T>
{
public:
    BlockDiagonalOdeLinearSolver(crd::memory::IAllocator* alloc, crd::usize n_base, crd::usize n_blocks)
        : m_nb(n_base), m_blocks(n_blocks), m_m(alloc, n_base, n_base), m_lu(alloc, n_base)
    {
    }
    [[nodiscard]] bool factor_iteration_matrix(T c, crd::containers::ConstSpan<T> jac, crd::usize big_n) override
    {
        CRD_ASSERT(big_n == m_nb * m_blocks && jac.size() == big_n * big_n);
        for (crd::usize i = 0; i < m_nb; ++i)
        {
            for (crd::usize j = 0; j < m_nb; ++j)
            {
                const T iden = (i == j) ? static_cast<T>(1) : static_cast<T>(0);
                m_m.at(i, j) = iden - c * jac[i * big_n + j]; // top-left block (all blocks identical)
            }
        }
        dense::factor_lu(m_lu, m_m);
        return m_lu.info() == 0;
    }
    void solve(crd::containers::Span<T> b) override
    {
        for (crd::usize k = 0; k < m_blocks; ++k)
        {
            dense::solve_lu(m_lu, crd::containers::Span<T>(b.data() + k * m_nb, m_nb));
        }
    }

private:
    crd::usize m_nb;
    crd::usize m_blocks;
    dense::Matrix<T, dense::Layout::RowMajor> m_m;
    dense::LU<T, dense::Layout::RowMajor> m_lu;
};

} // namespace detail

// FORWARD sensitivities. `y` (size n) in-out state; `S` (size n·np, ROW-MAJOR by parameter: S[j·n + i] =
// ∂y_i/∂p_j) in-out sensitivities (pass the initial ∂y0/∂p, usually 0). `stiff` ⇒ BDF, else RK45.
template <typename T>
[[nodiscard]] OdeResult<T> integrate_forward_sensitivities(const ParametricOdeFunction<T>& pfn, T t0, T t1,
                                                           crd::containers::Span<T> y, crd::containers::Span<T> S,
                                                           crd::containers::ConstSpan<T> p,
                                                           const OdeOptions<T>& opts, crd::memory::IAllocator* alloc,
                                                           bool stiff = false)
{
    namespace cont = crd::containers;
    const crd::usize n = pfn.dim();
    const crd::usize np = pfn.n_params();
    const crd::usize N = n * (1 + np);
    CRD_ASSERT(y.size() == n && S.size() == n * np && p.size() == np);

    cont::Array<T> Y(alloc);
    Y.resize(N);
    for (crd::usize i = 0; i < n; ++i)
    {
        Y[i] = y[i];
    }
    for (crd::usize j = 0; j < np; ++j)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            Y[(1 + j) * n + i] = S[j * n + i];
        }
    }

    // CVODES sensErrCon = FALSE (the default): exclude the sensitivity block from step-error control — the
    // STATE controls the step and the sensitivities ride along. Without this the augmented WRMS norm is
    // dominated by S (whose magnitudes can dwarf y), throttling the step to a fraction of the state-only
    // size — the dominant cost. Implemented through the per-component atol: real atol on the state, a huge
    // atol on S so its error contribution vanishes.
    cont::Array<T> atolv(alloc);
    atolv.resize(N);
    const T big = static_cast<T>(1e200);
    for (crd::usize i = 0; i < n; ++i)
    {
        atolv[i] = opts.atol_vec.empty() ? opts.atol : opts.atol_vec[i];
    }
    for (crd::usize k = n; k < N; ++k)
    {
        atolv[k] = big;
    }
    OdeOptions<T> sopts = opts;
    sopts.atol_vec = cont::ConstSpan<T>(atolv.data(), N);

    detail::AugmentedSensitivityFn<T> aug(alloc, pfn, p);
    // Shared-factorization solver (factor the n×n block once, reuse for all 1+np blocks) — the stiff path.
    detail::BlockDiagonalOdeLinearSolver<T> bsolver(alloc, n, 1 + np);
    const OdeResult<T> r = stiff
                               ? integrate_bdf<T>(aug, t0, t1, cont::Span<T>(Y.data(), N), sopts, alloc, &bsolver)
                               : integrate_erk<T>(aug, t0, t1, cont::Span<T>(Y.data(), N), sopts, alloc);

    for (crd::usize i = 0; i < n; ++i)
    {
        y[i] = Y[i];
    }
    for (crd::usize j = 0; j < np; ++j)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            S[j * n + i] = Y[(1 + j) * n + i];
        }
    }
    return r;
}

// ADJOINT sensitivities for a TERMINAL functional g(y(t1)). `dgdy_t1` = ∂g/∂y(t1) (size n). `grad` (size np)
// out = dg/dp. `stiff` ⇒ BDF (both passes), else RK45. Returns the BACKWARD solve's result.
template <typename T>
[[nodiscard]] OdeResult<T> integrate_adjoint_sensitivities(const ParametricOdeFunction<T>& pfn, T t0, T t1,
                                                           crd::containers::ConstSpan<T> y0,
                                                           crd::containers::ConstSpan<T> p,
                                                           crd::containers::ConstSpan<T> dgdy_t1,
                                                           crd::containers::Span<T> grad, const OdeOptions<T>& opts,
                                                           crd::memory::IAllocator* alloc, bool stiff = false)
{
    namespace cont = crd::containers;
    const crd::usize n = pfn.dim();
    const crd::usize np = pfn.n_params();
    CRD_ASSERT(y0.size() == n && p.size() == np && dgdy_t1.size() == n && grad.size() == np);

    // 1. Forward solve y, recording the dense-output trajectory.
    detail::ParamFixedFn<T> fwd(pfn, p);
    OdeSolution<T> sol(alloc);
    cont::Array<T> y(alloc);
    y.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        y[i] = y0[i];
    }
    const OdeResult<T> rf = stiff ? integrate_bdf<T>(fwd, t0, t1, cont::Span<T>(y.data(), n), opts, alloc, nullptr, &sol)
                                  : integrate_erk<T>(fwd, t0, t1, cont::Span<T>(y.data(), n), opts, alloc,
                                                     ErkMethod::Rk45, &sol);
    if (!rf.success)
    {
        return rf;
    }

    // 2. Backward solve Λ = [λ; q] from t1 to t0.
    detail::AdjointFn<T> adj(alloc, pfn, p, sol);
    cont::Array<T> L(alloc);
    L.resize(n + np);
    for (crd::usize i = 0; i < n; ++i)
    {
        L[i] = dgdy_t1[i]; // λ(t1) = ∂g/∂y(t1)
    }
    for (crd::usize j = 0; j < np; ++j)
    {
        L[n + j] = static_cast<T>(0); // q(t1) = 0
    }
    const OdeResult<T> rb = stiff ? integrate_bdf<T>(adj, t1, t0, cont::Span<T>(L.data(), n + np), opts, alloc)
                                  : integrate_erk<T>(adj, t1, t0, cont::Span<T>(L.data(), n + np), opts, alloc);

    // 3. dg/dp_j = q_j(t0).
    for (crd::usize j = 0; j < np; ++j)
    {
        grad[j] = L[n + j];
    }
    return rb;
}

} // namespace crd::hesap::ode
