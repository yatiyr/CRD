#pragma once

// qp.hpp — Phase 3.1.6 v7-k ⭐: QUADRATIC PROGRAMMING — the MPC / contact / SVM workhorse. One canonical
// problem form (the OSQP form — the most general dense convex-QP statement; equality = l_i == u_i, one-sided
// = ±inf):
//
//     min ½xᵀPx + qᵀx   s.t.   l ≤ Ax ≤ u        (P n×n symmetric PSD, A m×n, row-major dense)
//
// and two of the three v7-k solvers (the third, Goldfarb-Idnani dual active-set, is qp_active_set.hpp):
//
//   • solve_qp_admm — the **OSQP-class operator-splitting ADMM** (Stellato et al. 2020): the quasi-definite
//     KKT matrix [P+σI Aᵀ; A −diag(1/ρ)] is factored ONCE per ρ (the v0e Bunch-Kaufman) and every iteration
//     is one solve + a projection — the embedded-MPC profile. Faithful pieces: per-constraint ρ with the
//     equality boost (ρ_eq = 1e3·ρ), over-relaxation α = 1.6, OSQP's primal/dual residual termination,
//     **primal/dual INFEASIBILITY CERTIFICATES** (δy/δx tests, §3.4), a DETERMINISTIC adaptive-ρ rule
//     (residual-ratio, fixed interval — no wall-clock input, so the iteration count is reproducible), and the
//     active-set POLISH step (an equality-KKT solve on the detected active set — recovers high accuracy from
//     a first-order solution). P only needs PSD.
//   • solve_qp_mehrotra — the **predictor-corrector interior-point** method (Mehrotra 1992) on the converted
//     (A_E x = b, G x ≤ h) form: affine predictor → σ = (μ_aff/μ)³ centering-corrector → fraction-to-boundary
//     steps; each Newton solve is the reduced saddle [P + GᵀDG, A_Eᵀ; A_E, 0] through the v7-j
//     inertia-corrected Bunch-Kaufman (`solve_kkt_dense`-class machinery). The accuracy reference.
//
// DUAL CONVENTION (uniform across all three solvers so ONE certificate checks them all — OSQP's sign):
//     stationarity  Px + q + Aᵀy = 0,   y_i ≥ 0 active at u_i,  y_i ≤ 0 active at l_i,  y_i = 0 inactive.
//
// SCOPE (honest): DENSE v7-k. The sparse backend (supernodal/multifrontal LDLᵀ behind the same seam) lands
// with the MPC consumer slice — the ADMM iteration is written against a factor-solve seam on purpose.
// Gold peers: OSQP, qpOASES, quadprog [v7-z scoreboard]. ADR-0090; the moat: every solver is a serial
// fixed-order recurrence ⇒ bit-identical runs (worker-count independence becomes non-vacuous with the
// sparse backend).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/ldlt.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/opt/kkt.hpp> // solve_kkt_dense (the polish + IPM saddle)
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::opt
{

enum class QpStatus : crd::u8
{
    Solved = 0,
    MaxIterations = 1,
    PrimalInfeasible = 2, // certified (ADMM δy test)
    DualInfeasible = 3,   // certified (ADMM δx test)
    NumericalError = 4    // a factorization failed beyond recovery
};

// min ½xᵀPx + qᵀx s.t. l ≤ Ax ≤ u. All spans row-major dense; use ±infinity for absent sides; l_i == u_i is
// an equality row. P must be SYMMETRIC (only the lower triangle is read where it matters) and PSD (PD for GI).
template <typename T> struct QpProblem
{
    crd::containers::ConstSpan<T> p; // n×n
    crd::containers::ConstSpan<T> q; // n
    crd::containers::ConstSpan<T> a; // m×n
    crd::containers::ConstSpan<T> l; // m
    crd::containers::ConstSpan<T> u; // m
    crd::usize n = 0;
    crd::usize m = 0;

    [[nodiscard]] bool valid() const noexcept
    {
        return p.size() == n * n && q.size() == n && a.size() == m * n && l.size() == m && u.size() == m;
    }
};

template <typename T> struct QpResult
{
    crd::containers::Array<T> x;
    crd::containers::Array<T> y; // duals of l ≤ Ax ≤ u (the OSQP sign convention above)
    T obj = static_cast<T>(0);
    QpStatus status = QpStatus::MaxIterations;
    crd::usize iterations = 0;
    T primal_res = static_cast<T>(0); // ‖Ax − Π_[l,u](Ax)‖∞ at exit
    T dual_res = static_cast<T>(0);   // ‖Px + q + Aᵀy‖∞ at exit

    explicit QpResult(crd::memory::IAllocator* alloc) noexcept : x(alloc), y(alloc) {}
};

namespace detail
{

template <typename T>
inline void qp_mat_vec(crd::containers::ConstSpan<T> mat, crd::usize rows, crd::usize cols,
                       crd::containers::ConstSpan<T> v, crd::containers::Span<T> out) noexcept
{
    for (crd::usize i = 0; i < rows; ++i)
    {
        T acc = static_cast<T>(0);
        for (crd::usize j = 0; j < cols; ++j)
        {
            acc += mat[i * cols + j] * v[j];
        }
        out[i] = acc;
    }
}

template <typename T>
inline void qp_mat_tvec(crd::containers::ConstSpan<T> mat, crd::usize rows, crd::usize cols,
                        crd::containers::ConstSpan<T> v, crd::containers::Span<T> out) noexcept
{
    for (crd::usize j = 0; j < cols; ++j)
    {
        out[j] = static_cast<T>(0);
    }
    for (crd::usize i = 0; i < rows; ++i)
    {
        const T vi = v[i];
        for (crd::usize j = 0; j < cols; ++j)
        {
            out[j] += mat[i * cols + j] * vi;
        }
    }
}

template <typename T> [[nodiscard]] inline T qp_inf_norm(crd::containers::ConstSpan<T> v) noexcept
{
    T mx = static_cast<T>(0);
    for (crd::usize i = 0; i < v.size(); ++i)
    {
        const T a = crd::math::fabs(v[i]);
        mx = a > mx ? a : mx;
    }
    return mx;
}

// Objective ½xᵀPx + qᵀx and the dual residual Px + q + Aᵀy (the shared exit bookkeeping).
template <typename T>
inline void qp_finalize(const QpProblem<T>& prob, QpResult<T>& result, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> px(alloc);
    crd::containers::Array<T> aty(alloc);
    crd::containers::Array<T> ax(alloc);
    px.resize(prob.n);
    aty.resize(prob.n);
    ax.resize(prob.m);
    qp_mat_vec<T>(prob.p, prob.n, prob.n, {result.x.data(), prob.n}, {px.data(), prob.n});
    qp_mat_tvec<T>(prob.a, prob.m, prob.n, {result.y.data(), prob.m}, {aty.data(), prob.n});
    qp_mat_vec<T>(prob.a, prob.m, prob.n, {result.x.data(), prob.n}, {ax.data(), prob.m});
    T obj = static_cast<T>(0);
    T dres = static_cast<T>(0);
    bool finite = true; // a NaN never wins a `>` fold — track it explicitly so garbage can't report residual 0
    for (crd::usize j = 0; j < prob.n; ++j)
    {
        obj += static_cast<T>(0.5) * result.x[j] * px[j] + prob.q[j] * result.x[j];
        const T d = crd::math::fabs(px[j] + prob.q[j] + aty[j]);
        finite = finite && std::isfinite(d);
        dres = d > dres ? d : dres;
    }
    T pres = static_cast<T>(0);
    for (crd::usize i = 0; i < prob.m; ++i)
    {
        const T zi = ax[i] < prob.l[i] ? prob.l[i] : (ax[i] > prob.u[i] ? prob.u[i] : ax[i]);
        const T d = crd::math::fabs(ax[i] - zi);
        finite = finite && std::isfinite(d);
        pres = d > pres ? d : pres;
    }
    result.obj = obj;
    result.dual_res = finite ? dres : std::numeric_limits<T>::infinity();
    result.primal_res = finite ? pres : std::numeric_limits<T>::infinity();
}

} // namespace detail

// ------------------------------------------------------------------------------------------- OSQP-class ADMM

template <typename T> struct QpAdmmOptions
{
    T rho = static_cast<T>(0.1);    // base step (equality rows get 1e3·ρ, the OSQP boost)
    T sigma = static_cast<T>(1e-6); // x-regularization
    T alpha = static_cast<T>(1.6);  // over-relaxation
    T eps_abs = static_cast<T>(1e-6);
    T eps_rel = static_cast<T>(1e-6);
    T eps_infeas = static_cast<T>(1e-7); // certificate tolerance
    crd::usize max_iters = 10000;
    bool adaptive_rho = true; // deterministic residual-ratio rule, every adaptive_interval iterations
    crd::usize adaptive_interval = 50;
    bool polish = true;         // active-set equality-KKT refinement on success
    bool scaling = true;        // modified RUIZ equilibration + cost normalization (OSQP §5 — the iteration-count
                                // lever; deterministic fixed-iteration scaling, termination in scaled units =
                                // OSQP's scaled_termination mode; the polish refines in scaled space and the
                                // wrapper re-finalizes unscaled)
    crd::usize ruiz_iters = 10; // OSQP's default equilibration sweep count
};

// The UNSCALED ADMM core (the `scaling=false` path; `solve_qp_admm` below wraps it with Ruiz equilibration).
// P PSD suffices.
template <typename T>
[[nodiscard]] QpResult<T> solve_qp_admm_unscaled(const QpProblem<T>& prob, const QpAdmmOptions<T>& opts,
                                                 crd::memory::IAllocator* alloc)
{
    namespace dn = crd::hesap::dense;
    CRD_ASSERT_MSG(prob.valid(), "solve_qp_admm: inconsistent problem spans");
    const crd::usize n = prob.n;
    const crd::usize m = prob.m;

    QpResult<T> result(alloc);
    result.x.resize(n);
    result.y.resize(m);
    for (crd::usize i = 0; i < n; ++i)
    {
        result.x[i] = static_cast<T>(0);
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        result.y[i] = static_cast<T>(0);
    }
    if (n == 0)
    {
        result.status = QpStatus::Solved;
        return result;
    }

    const crd::usize nk = n + m;
    crd::containers::Array<T> rho(alloc); // per-constraint ρ (equality boost)
    rho.resize(m);
    auto rho_for = [&](crd::usize i, T base) -> T
    {
        return prob.l[i] == prob.u[i] ? static_cast<T>(1e3) * base : base;
    };
    T base_rho = opts.rho;
    for (crd::usize i = 0; i < m; ++i)
    {
        rho[i] = rho_for(i, base_rho);
    }

    // The factor-solve seam: refactor the quasi-definite KKT [P+σI Aᵀ; A −diag(1/ρ)] (once per ρ).
    dn::LDLT<T> kkt(alloc, nk);
    auto refactor = [&]() -> bool
    {
        dn::Symmetric<T> kmat(alloc, nk);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j <= i; ++j)
            {
                kmat.at(i, j) = prob.p[i * n + j];
            }
            kmat.at(i, i) += opts.sigma;
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                kmat.at(n + i, j) = prob.a[i * n + j];
            }
            kmat.at(n + i, n + i) = -static_cast<T>(1) / rho[i];
        }
        dn::factor_ldlt<T, dn::Layout::RowMajor>(kkt, kmat);
        return kkt.info() == 0;
    };
    if (!refactor())
    {
        result.status = QpStatus::NumericalError;
        return result;
    }

    crd::containers::Array<T> z(alloc);
    crd::containers::Array<T> ax(alloc);
    crd::containers::Array<T> rhs(alloc);
    crd::containers::Array<T> ztilde(alloc);
    crd::containers::Array<T> px(alloc);
    crd::containers::Array<T> aty(alloc);
    crd::containers::Array<T> x_prev(alloc);
    crd::containers::Array<T> y_prev(alloc);
    z.resize(m);
    ax.resize(m);
    rhs.resize(nk);
    ztilde.resize(m);
    px.resize(n);
    aty.resize(n);
    x_prev.resize(n);
    y_prev.resize(m);

    detail::qp_mat_vec<T>(prob.a, m, n, {result.x.data(), n}, {ax.data(), m});
    for (crd::usize i = 0; i < m; ++i)
    {
        z[i] = ax[i] < prob.l[i] ? prob.l[i] : (ax[i] > prob.u[i] ? prob.u[i] : ax[i]);
    }

    T* x = result.x.data();
    T* y = result.y.data();

    QpStatus status = QpStatus::MaxIterations;
    crd::usize it = 0;
    for (; it < opts.max_iters; ++it)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            x_prev[i] = x[i];
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            y_prev[i] = y[i];
        }

        // (1) the KKT solve: rhs = [σx − q ; z − y/ρ] → x̃ = rhs[0:n], ν = rhs[n:]; z̃ = z + (ν − y)/ρ.
        for (crd::usize i = 0; i < n; ++i)
        {
            rhs[i] = opts.sigma * x[i] - prob.q[i];
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            rhs[n + i] = z[i] - y[i] / rho[i];
        }
        dn::solve_ldlt<T, dn::Layout::RowMajor>(kkt, {rhs.data(), nk});
        for (crd::usize i = 0; i < m; ++i)
        {
            ztilde[i] = z[i] + (rhs[n + i] - y[i]) / rho[i];
        }

        // (2)-(4) relaxed updates + projection + dual ascent.
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] = opts.alpha * rhs[i] + (static_cast<T>(1) - opts.alpha) * x[i];
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            const T zr = opts.alpha * ztilde[i] + (static_cast<T>(1) - opts.alpha) * z[i];
            T znew = zr + y[i] / rho[i];
            znew = znew < prob.l[i] ? prob.l[i] : (znew > prob.u[i] ? prob.u[i] : znew);
            y[i] = y[i] + rho[i] * (zr - znew);
            z[i] = znew;
        }

        // Residuals (OSQP §3.3) every iteration (cheap at dense-small; keeps termination deterministic).
        detail::qp_mat_vec<T>(prob.p, n, n, {x, n}, {px.data(), n});
        detail::qp_mat_tvec<T>(prob.a, m, n, {y, m}, {aty.data(), n});
        detail::qp_mat_vec<T>(prob.a, m, n, {x, n}, {ax.data(), m});
        T pres = static_cast<T>(0);
        T pscale = static_cast<T>(0);
        for (crd::usize i = 0; i < m; ++i)
        {
            const T d = crd::math::fabs(ax[i] - z[i]);
            pres = d > pres ? d : pres;
            const T s1 = crd::math::fabs(ax[i]);
            const T s2 = crd::math::fabs(z[i]);
            pscale = s1 > pscale ? s1 : pscale;
            pscale = s2 > pscale ? s2 : pscale;
        }
        T dres = static_cast<T>(0);
        T dscale = static_cast<T>(0);
        for (crd::usize j = 0; j < n; ++j)
        {
            const T d = crd::math::fabs(px[j] + prob.q[j] + aty[j]);
            dres = d > dres ? d : dres;
            T s = crd::math::fabs(px[j]);
            s = crd::math::fabs(prob.q[j]) > s ? crd::math::fabs(prob.q[j]) : s;
            s = crd::math::fabs(aty[j]) > s ? crd::math::fabs(aty[j]) : s;
            dscale = s > dscale ? s : dscale;
        }
        if (pres <= opts.eps_abs + opts.eps_rel * pscale && dres <= opts.eps_abs + opts.eps_rel * dscale)
        {
            status = QpStatus::Solved;
            ++it;
            break;
        }

        // Infeasibility certificates (OSQP §3.4) on the iterate DIFFERENCES.
        {
            T dy_norm = static_cast<T>(0);
            for (crd::usize i = 0; i < m; ++i)
            {
                const T d = crd::math::fabs(y[i] - y_prev[i]);
                dy_norm = d > dy_norm ? d : dy_norm;
            }
            if (dy_norm > static_cast<T>(0))
            {
                crd::containers::Array<T> dy(alloc);
                dy.resize(m);
                for (crd::usize i = 0; i < m; ++i)
                {
                    dy[i] = (y[i] - y_prev[i]) / dy_norm;
                }
                detail::qp_mat_tvec<T>(prob.a, m, n, {dy.data(), m}, {aty.data(), n});
                T support = static_cast<T>(0); // uᵀ[δy]₊ + lᵀ[δy]₋
                bool finite_support = true;
                for (crd::usize i = 0; i < m && finite_support; ++i)
                {
                    if (dy[i] > static_cast<T>(0))
                    {
                        finite_support = std::isfinite(prob.u[i]);
                        support += prob.u[i] * dy[i];
                    }
                    else if (dy[i] < static_cast<T>(0))
                    {
                        finite_support = std::isfinite(prob.l[i]);
                        support += prob.l[i] * dy[i];
                    }
                }
                if (finite_support && detail::qp_inf_norm<T>({aty.data(), n}) <= opts.eps_infeas &&
                    support <= -opts.eps_infeas)
                {
                    status = QpStatus::PrimalInfeasible;
                    ++it;
                    break;
                }
            }
            T dx_norm = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                const T d = crd::math::fabs(x[i] - x_prev[i]);
                dx_norm = d > dx_norm ? d : dx_norm;
            }
            if (dx_norm > static_cast<T>(0))
            {
                crd::containers::Array<T> dx(alloc);
                dx.resize(n);
                for (crd::usize i = 0; i < n; ++i)
                {
                    dx[i] = (x[i] - x_prev[i]) / dx_norm;
                }
                detail::qp_mat_vec<T>(prob.p, n, n, {dx.data(), n}, {px.data(), n});
                detail::qp_mat_vec<T>(prob.a, m, n, {dx.data(), n}, {ax.data(), m});
                T qdx = static_cast<T>(0);
                for (crd::usize j = 0; j < n; ++j)
                {
                    qdx += prob.q[j] * dx[j];
                }
                bool dir_ok = true; // (Aδx)_i must be ~0 where both bounds finite, ≥0 where u=∞, ≤0 where l=−∞
                for (crd::usize i = 0; i < m && dir_ok; ++i)
                {
                    const bool lf = std::isfinite(prob.l[i]);
                    const bool uf = std::isfinite(prob.u[i]);
                    const T v = ax[i];
                    if (lf && uf)
                    {
                        dir_ok = crd::math::fabs(v) <= opts.eps_infeas;
                    }
                    else if (uf)
                    {
                        dir_ok = v <= opts.eps_infeas;
                    }
                    else if (lf)
                    {
                        dir_ok = v >= -opts.eps_infeas;
                    }
                }
                if (dir_ok && detail::qp_inf_norm<T>({px.data(), n}) <= opts.eps_infeas && qdx <= -opts.eps_infeas)
                {
                    status = QpStatus::DualInfeasible;
                    ++it;
                    break;
                }
            }
        }

        // Deterministic adaptive ρ (residual-ratio rule on a FIXED interval — no wall clock involved).
        if (opts.adaptive_rho && (it + 1) % opts.adaptive_interval == 0)
        {
            const T pr = pres / (pscale > static_cast<T>(1e-30) ? pscale : static_cast<T>(1));
            const T dr = dres / (dscale > static_cast<T>(1e-30) ? dscale : static_cast<T>(1));
            if (pr > static_cast<T>(0) && dr > static_cast<T>(0))
            {
                const T ratio = crd::math::sqrt(pr / dr);
                if (ratio > static_cast<T>(5) || ratio < static_cast<T>(0.2))
                {
                    T next = base_rho * ratio;
                    next = next < static_cast<T>(1e-6) ? static_cast<T>(1e-6)
                                                       : (next > static_cast<T>(1e6) ? static_cast<T>(1e6) : next);
                    base_rho = next;
                    for (crd::usize i = 0; i < m; ++i)
                    {
                        rho[i] = rho_for(i, base_rho);
                    }
                    if (!refactor())
                    {
                        status = QpStatus::NumericalError;
                        break;
                    }
                }
            }
        }
    }
    result.iterations = it;
    result.status = status;

    // Polish (OSQP §5.2-lite): solve the equality-KKT on the detected active set (an EMPTY set polishes the
    // unconstrained problem to the exact Newton solve); keep it iff the certificate improves.
    if (status == QpStatus::Solved && opts.polish)
    {
        crd::containers::Array<crd::u32> act(alloc);
        crd::containers::Array<T> bact(alloc);
        crd::containers::Array<crd::i8> side(alloc); // −1 lower-active, +1 upper-active, 0 equality (any sign)
        act.resize(m);
        bact.resize(m);
        side.resize(m);
        crd::usize na = 0;
        for (crd::usize i = 0; i < m; ++i)
        {
            if (prob.l[i] == prob.u[i] || y[i] < static_cast<T>(0))
            {
                act[na] = static_cast<crd::u32>(i);
                bact[na] = prob.l[i];
                side[na] = prob.l[i] == prob.u[i] ? static_cast<crd::i8>(0) : static_cast<crd::i8>(-1);
                ++na;
            }
            else if (y[i] > static_cast<T>(0))
            {
                act[na] = static_cast<crd::u32>(i);
                bact[na] = prob.u[i];
                side[na] = static_cast<crd::i8>(1);
                ++na;
            }
        }
        if (na <= n) // an over-determined active set means the y-signs are unreliable — skip the polish
        {
            crd::containers::Array<T> aact(alloc);
            crd::containers::Array<T> cact(alloc);
            crd::containers::Array<T> xp(alloc);
            crd::containers::Array<T> nu(alloc);
            aact.resize(na * n);
            cact.resize(na);
            xp.resize(n);
            nu.resize(na);
            for (crd::usize k = 0; k < na; ++k)
            {
                for (crd::usize j = 0; j < n; ++j)
                {
                    aact[k * n + j] = prob.a[static_cast<crd::usize>(act[k]) * n + j];
                }
                cact[k] = -bact[k]; // solve_kkt_dense expects c with A_E x + c = 0 ⇒ c = −b
            }
            const auto ks = solve_kkt_dense<T>(alloc, prob.p, {aact.data(), na * n}, prob.q, {cact.data(), na},
                                               {xp.data(), n}, {nu.data(), na});
            if (ks.solved)
            {
                // Candidate: x = xp, y = mapped duals (λ⁺ from the L = f − λᵀc convention ⇒ OSQP sign is −λ).
                QpResult<T> cand(alloc);
                cand.x.resize(n);
                cand.y.resize(m);
                for (crd::usize j = 0; j < n; ++j)
                {
                    cand.x[j] = xp[j];
                }
                for (crd::usize i = 0; i < m; ++i)
                {
                    cand.y[i] = static_cast<T>(0);
                }
                for (crd::usize k = 0; k < na; ++k)
                {
                    cand.y[act[k]] = -nu[k];
                }
                detail::qp_finalize<T>(prob, cand, alloc);
                detail::qp_finalize<T>(prob, result, alloc);
                const T cand_max = cand.primal_res > cand.dual_res ? cand.primal_res : cand.dual_res;
                const T cur_max = result.primal_res > result.dual_res ? result.primal_res : result.dual_res;
                // KKT requires the dual SIGNS too: a wrong active set solved exactly still yields stationarity
                // ≈ 0 + primal feasibility, but the forced row's multiplier flips sign (caught on the Ruiz-scaled
                // cross-adjudication family). Reject sign-inconsistent or non-finite candidates.
                bool kkt_signs_ok = true;
                for (crd::usize k = 0; k < na && kkt_signs_ok; ++k)
                {
                    const T yk = cand.y[act[k]];
                    kkt_signs_ok =
                        std::isfinite(yk) && (side[k] == 0 || (side[k] < 0 ? yk <= opts.eps_abs : yk >= -opts.eps_abs));
                }
                for (crd::usize j = 0; j < n && kkt_signs_ok; ++j)
                {
                    kkt_signs_ok = std::isfinite(cand.x[j]);
                }
                if (kkt_signs_ok && cand_max <= cur_max)
                {
                    for (crd::usize j = 0; j < n; ++j)
                    {
                        result.x[j] = cand.x[j];
                    }
                    for (crd::usize i = 0; i < m; ++i)
                    {
                        result.y[i] = cand.y[i];
                    }
                }
            }
        }
    }

    detail::qp_finalize<T>(prob, result, alloc);
    return result;
}

// OSQP-class ADMM with MODIFIED RUIZ EQUILIBRATION + cost normalization (OSQP §5): D/E diagonal scalings
// equilibrate the column/row ∞-norms of [P Aᵀ; A 0] over `ruiz_iters` fixed sweeps (deterministic), the cost
// factor c normalizes the objective scale; the scaled problem P̄=cDPD, q̄=cDq, Ā=EAD, l̄=El, ū=Eu runs through
// the unscaled core, then x = Dx̄, y = Eȳ/c, and the final objective/residuals are re-finalized UNSCALED.
// This is the documented iteration-count lever behind OSQP's convergence speed.
template <typename T>
[[nodiscard]] QpResult<T> solve_qp_admm(const QpProblem<T>& prob, const QpAdmmOptions<T>& opts,
                                        crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(prob.valid(), "solve_qp_admm: inconsistent problem spans");
    const crd::usize n = prob.n;
    const crd::usize m = prob.m;
    if (!opts.scaling || n == 0)
    {
        return solve_qp_admm_unscaled<T>(prob, opts, alloc);
    }

    constexpr T min_scale = static_cast<T>(1e-4); // OSQP's MIN/MAX_SCALING clamps
    constexpr T max_scale = static_cast<T>(1e4);
    constexpr T tiny = static_cast<T>(1e-12);
    auto clamp_scale = [&](T v) -> T
    {
        return v < min_scale ? min_scale : (v > max_scale ? max_scale : v);
    };

    // Live scaled copies + the accumulated D (n), E (m), c.
    crd::containers::Array<T> ps(alloc);
    crd::containers::Array<T> qs(alloc);
    crd::containers::Array<T> as(alloc);
    crd::containers::Array<T> ls(alloc);
    crd::containers::Array<T> us(alloc);
    crd::containers::Array<T> dvec(alloc);
    crd::containers::Array<T> evec(alloc);
    ps.resize(n * n);
    qs.resize(n);
    as.resize(m * n);
    ls.resize(m);
    us.resize(m);
    dvec.resize(n);
    evec.resize(m);
    for (crd::usize k = 0; k < n * n; ++k)
    {
        ps[k] = prob.p[k];
    }
    for (crd::usize j = 0; j < n; ++j)
    {
        qs[j] = prob.q[j];
        dvec[j] = static_cast<T>(1);
    }
    for (crd::usize k = 0; k < m * n; ++k)
    {
        as[k] = prob.a[k];
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        ls[i] = prob.l[i];
        us[i] = prob.u[i];
        evec[i] = static_cast<T>(1);
    }
    T c = static_cast<T>(1);

    for (crd::usize sweep = 0; sweep < opts.ruiz_iters; ++sweep)
    {
        // δ_x[j] = 1/sqrt(∞-norm of column j of the current [P̄; Ā]); δ_e[i] = the same for row i of Ā.
        for (crd::usize j = 0; j < n; ++j)
        {
            T nrm = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                const T v = crd::math::fabs(ps[i * n + j]);
                nrm = v > nrm ? v : nrm;
            }
            for (crd::usize k = 0; k < m; ++k)
            {
                const T v = crd::math::fabs(as[k * n + j]);
                nrm = v > nrm ? v : nrm;
            }
            const T dj = clamp_scale(static_cast<T>(1) / crd::math::sqrt(nrm > tiny ? nrm : static_cast<T>(1)));
            dvec[j] *= dj;
            qs[j] *= dj;
            for (crd::usize i = 0; i < n; ++i) // P̄ ← δ P̄ (column j), the row side comes from the i-sweep
            {
                ps[i * n + j] *= dj;
                ps[j * n + i] *= dj;
            }
            for (crd::usize k = 0; k < m; ++k)
            {
                as[k * n + j] *= dj;
            }
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            T nrm = static_cast<T>(0);
            for (crd::usize j = 0; j < n; ++j)
            {
                const T v = crd::math::fabs(as[i * n + j]);
                nrm = v > nrm ? v : nrm;
            }
            const T ei = clamp_scale(static_cast<T>(1) / crd::math::sqrt(nrm > tiny ? nrm : static_cast<T>(1)));
            evec[i] *= ei;
            ls[i] *= ei;
            us[i] *= ei;
            for (crd::usize j = 0; j < n; ++j)
            {
                as[i * n + j] *= ei;
            }
        }
        // Cost normalization: γ = 1/max(mean column ∞-norm of P̄, ‖q̄‖∞).
        T pmean = static_cast<T>(0);
        for (crd::usize j = 0; j < n; ++j)
        {
            T nrm = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                const T v = crd::math::fabs(ps[i * n + j]);
                nrm = v > nrm ? v : nrm;
            }
            pmean += nrm;
        }
        pmean /= static_cast<T>(n);
        const T qinf = detail::qp_inf_norm<T>({qs.data(), n});
        const T denom = pmean > qinf ? pmean : qinf;
        const T gamma = clamp_scale(static_cast<T>(1) / (denom > tiny ? denom : static_cast<T>(1)));
        for (crd::usize k = 0; k < n * n; ++k)
        {
            ps[k] *= gamma;
        }
        for (crd::usize j = 0; j < n; ++j)
        {
            qs[j] *= gamma;
        }
        c *= gamma;
    }

    const QpProblem<T> scaled{
        {ps.data(), n * n}, {qs.data(), n}, {as.data(), m * n}, {ls.data(), m}, {us.data(), m}, n, m};
    QpResult<T> r = solve_qp_admm_unscaled<T>(scaled, opts, alloc);

    // Unscale: x = D x̄, y = E ȳ / c; the objective/residuals re-finalized on the ORIGINAL problem.
    for (crd::usize j = 0; j < n; ++j)
    {
        r.x[j] *= dvec[j];
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        r.y[i] *= evec[i] / c;
    }
    detail::qp_finalize<T>(prob, r, alloc);
    return r;
}

// --------------------------------------------------------------------------------- Mehrotra interior point

template <typename T> struct QpIpmOptions
{
    crd::usize max_iters = 100;
    T tol = static_cast<T>(1e-9); // on μ and the scaled primal/dual residuals
};

// Mehrotra predictor-corrector on the canonical form (converted internally to A_E x = b, G x ≤ h).
// Requires a strictly feasible-ish convex problem (P PSD); the accuracy reference among the three.
template <typename T>
[[nodiscard]] QpResult<T> solve_qp_mehrotra(const QpProblem<T>& prob, const QpIpmOptions<T>& opts,
                                            crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(prob.valid(), "solve_qp_mehrotra: inconsistent problem spans");
    const crd::usize n = prob.n;

    QpResult<T> result(alloc);
    result.x.resize(n);
    result.y.resize(prob.m);
    for (crd::usize i = 0; i < n; ++i)
    {
        result.x[i] = static_cast<T>(0);
    }
    for (crd::usize i = 0; i < prob.m; ++i)
    {
        result.y[i] = static_cast<T>(0);
    }
    if (n == 0)
    {
        result.status = QpStatus::Solved;
        return result;
    }

    // Convert: l==u rows → equalities (A_E, b). Finite u → G row (+a_i, u_i, source +i). Finite l → G row
    // (−a_i, −l_i, source −i). `gsrc` maps each G row back to the original row + sign for the y-report.
    crd::usize me = 0;
    crd::usize mi = 0;
    for (crd::usize i = 0; i < prob.m; ++i)
    {
        if (prob.l[i] == prob.u[i])
        {
            ++me;
        }
        else
        {
            if (std::isfinite(prob.u[i]))
            {
                ++mi;
            }
            if (std::isfinite(prob.l[i]))
            {
                ++mi;
            }
        }
    }
    crd::containers::Array<T> ae(alloc);
    crd::containers::Array<T> be(alloc);
    crd::containers::Array<T> gmat(alloc);
    crd::containers::Array<T> h(alloc);
    crd::containers::Array<crd::i32> gsrc(alloc); // +(<row>+1) for an upper row, −(<row>+1) for a lower row
    ae.resize(me * n);
    be.resize(me);
    gmat.resize(mi * n);
    h.resize(mi);
    gsrc.resize(mi);
    {
        crd::usize we = 0;
        crd::usize wi = 0;
        for (crd::usize i = 0; i < prob.m; ++i)
        {
            if (prob.l[i] == prob.u[i])
            {
                for (crd::usize j = 0; j < n; ++j)
                {
                    ae[we * n + j] = prob.a[i * n + j];
                }
                be[we] = prob.l[i];
                ++we;
            }
            else
            {
                if (std::isfinite(prob.u[i]))
                {
                    for (crd::usize j = 0; j < n; ++j)
                    {
                        gmat[wi * n + j] = prob.a[i * n + j];
                    }
                    h[wi] = prob.u[i];
                    gsrc[wi] = static_cast<crd::i32>(i) + 1;
                    ++wi;
                }
                if (std::isfinite(prob.l[i]))
                {
                    for (crd::usize j = 0; j < n; ++j)
                    {
                        gmat[wi * n + j] = -prob.a[i * n + j];
                    }
                    h[wi] = -prob.l[i];
                    gsrc[wi] = -(static_cast<crd::i32>(i) + 1);
                    ++wi;
                }
            }
        }
    }

    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> ye(alloc);
    crd::containers::Array<T> s(alloc);
    crd::containers::Array<T> zi(alloc);
    x.resize(n);
    ye.resize(me);
    s.resize(mi);
    zi.resize(mi);
    for (crd::usize j = 0; j < n; ++j)
    {
        x[j] = static_cast<T>(0);
    }
    for (crd::usize i = 0; i < me; ++i)
    {
        ye[i] = static_cast<T>(0);
    }
    crd::containers::Array<T> gx(alloc);
    gx.resize(mi);
    if (mi > 0)
    {
        detail::qp_mat_vec<T>({gmat.data(), mi * n}, mi, n, {x.data(), n}, {gx.data(), mi});
    }
    for (crd::usize i = 0; i < mi; ++i)
    {
        const T gap = h[i] - gx[i];
        s[i] = gap > static_cast<T>(1) ? gap : static_cast<T>(1);
        zi[i] = static_cast<T>(1);
    }

    crd::containers::Array<T> wmat(alloc); // P + GᵀDG
    crd::containers::Array<T> rd(alloc);
    crd::containers::Array<T> rp(alloc);
    crd::containers::Array<T> rg(alloc);
    crd::containers::Array<T> rhs_x(alloc);
    crd::containers::Array<T> dx(alloc);
    crd::containers::Array<T> dye(alloc);
    crd::containers::Array<T> ds(alloc);
    crd::containers::Array<T> dz(alloc);
    crd::containers::Array<T> ds_aff(alloc);
    crd::containers::Array<T> dz_aff(alloc);
    wmat.resize(n * n);
    rd.resize(n);
    rp.resize(me);
    rg.resize(mi);
    rhs_x.resize(n);
    dx.resize(n);
    dye.resize(me);
    ds.resize(mi);
    dz.resize(mi);
    ds_aff.resize(mi);
    dz_aff.resize(mi);

    QpStatus status = QpStatus::MaxIterations;
    crd::usize it = 0;
    for (; it < opts.max_iters; ++it)
    {
        // Residuals: rp = A_E x − b; rd = Px + q + A_Eᵀye + Gᵀz; rg = Gx + s − h.
        for (crd::usize i = 0; i < me; ++i)
        {
            T acc = -be[i];
            for (crd::usize j = 0; j < n; ++j)
            {
                acc += ae[i * n + j] * x[j];
            }
            rp[i] = acc;
        }
        detail::qp_mat_vec<T>(prob.p, n, n, {x.data(), n}, {rd.data(), n});
        for (crd::usize j = 0; j < n; ++j)
        {
            rd[j] += prob.q[j];
        }
        if (me > 0)
        {
            crd::containers::Array<T> tmp(alloc);
            tmp.resize(n);
            detail::qp_mat_tvec<T>({ae.data(), me * n}, me, n, {ye.data(), me}, {tmp.data(), n});
            for (crd::usize j = 0; j < n; ++j)
            {
                rd[j] += tmp[j];
            }
        }
        if (mi > 0)
        {
            crd::containers::Array<T> tmp(alloc);
            tmp.resize(n);
            detail::qp_mat_tvec<T>({gmat.data(), mi * n}, mi, n, {zi.data(), mi}, {tmp.data(), n});
            for (crd::usize j = 0; j < n; ++j)
            {
                rd[j] += tmp[j];
            }
            detail::qp_mat_vec<T>({gmat.data(), mi * n}, mi, n, {x.data(), n}, {gx.data(), mi});
            for (crd::usize i = 0; i < mi; ++i)
            {
                rg[i] = gx[i] + s[i] - h[i];
            }
        }
        T mu = static_cast<T>(0);
        for (crd::usize i = 0; i < mi; ++i)
        {
            mu += s[i] * zi[i];
        }
        mu = mi > 0 ? mu / static_cast<T>(mi) : static_cast<T>(0);

        const T scale = static_cast<T>(1) + detail::qp_inf_norm<T>(prob.q);
        if (mu <= opts.tol && detail::qp_inf_norm<T>({rd.data(), n}) <= opts.tol * scale &&
            detail::qp_inf_norm<T>({rp.data(), me}) <= opts.tol * scale &&
            detail::qp_inf_norm<T>({rg.data(), mi}) <= opts.tol * scale)
        {
            status = QpStatus::Solved;
            break;
        }

        // Divergence guard (infeasible/unbounded problems drive s, z to non-finite values — Bunch-Kaufman on
        // NaN entries is undefined; stop honestly instead).
        bool finite_iterates = true;
        for (crd::usize i = 0; i < mi && finite_iterates; ++i)
        {
            finite_iterates = std::isfinite(s[i]) && std::isfinite(zi[i]) && s[i] > static_cast<T>(0);
        }
        for (crd::usize j = 0; j < n && finite_iterates; ++j)
        {
            finite_iterates = std::isfinite(x[j]);
        }
        if (!finite_iterates)
        {
            status = QpStatus::NumericalError;
            break;
        }

        // One Newton solve of the reduced saddle for a given complementarity rhs rc (rc_i on s_i z_i):
        auto newton_solve = [&](crd::containers::ConstSpan<T> rc, crd::containers::Span<T> out_dx,
                                crd::containers::Span<T> out_dye, crd::containers::Span<T> out_ds,
                                crd::containers::Span<T> out_dz) -> bool
        {
            for (crd::usize k = 0; k < n * n; ++k)
            {
                wmat[k] = prob.p[k];
            }
            for (crd::usize i = 0; i < mi; ++i)
            {
                const T d = zi[i] / s[i]; // D_ii
                for (crd::usize r = 0; r < n; ++r)
                {
                    const T gr = gmat[i * n + r];
                    if (gr != static_cast<T>(0))
                    {
                        for (crd::usize c = 0; c < n; ++c)
                        {
                            wmat[r * n + c] += d * gr * gmat[i * n + c];
                        }
                    }
                }
            }
            // rhs_x = −rd − Gᵀ S⁻¹(−rc + Z·rg)
            for (crd::usize j = 0; j < n; ++j)
            {
                rhs_x[j] = -rd[j];
            }
            for (crd::usize i = 0; i < mi; ++i)
            {
                const T w = (-rc[i] + zi[i] * rg[i]) / s[i];
                for (crd::usize j = 0; j < n; ++j)
                {
                    rhs_x[j] -= gmat[i * n + j] * w;
                }
            }
            // Solve [W A_Eᵀ; A_E 0][dx; dye'] = [rhs_x; −rp] via the v7-j machinery (note its convention:
            // it solves with rhs [−g; −c] ⇒ pass g = −rhs_x, c = rp; returns λ⁺ = −dye' ⇒ dye = −λ⁺).
            crd::containers::Array<T> gneg(alloc);
            gneg.resize(n);
            for (crd::usize j = 0; j < n; ++j)
            {
                gneg[j] = -rhs_x[j];
            }
            crd::containers::Array<T> lam(alloc);
            lam.resize(me);
            const auto ks = solve_kkt_dense<T>(alloc, {wmat.data(), n * n}, {ae.data(), me * n}, {gneg.data(), n},
                                               {rp.data(), me}, out_dx, {lam.data(), me});
            if (!ks.solved)
            {
                return false;
            }
            for (crd::usize i = 0; i < me; ++i)
            {
                out_dye[i] = -lam[i];
            }
            for (crd::usize i = 0; i < mi; ++i)
            {
                T gdx = static_cast<T>(0);
                for (crd::usize j = 0; j < n; ++j)
                {
                    gdx += gmat[i * n + j] * out_dx[j];
                }
                out_ds[i] = -rg[i] - gdx;
                out_dz[i] = (-rc[i] - zi[i] * out_ds[i]) / s[i];
            }
            return true;
        };

        // Predictor (affine): rc = S·Z·e.
        crd::containers::Array<T> rc(alloc);
        rc.resize(mi);
        for (crd::usize i = 0; i < mi; ++i)
        {
            rc[i] = s[i] * zi[i];
        }
        if (!newton_solve({rc.data(), mi}, {dx.data(), n}, {dye.data(), me}, {ds_aff.data(), mi}, {dz_aff.data(), mi}))
        {
            status = QpStatus::NumericalError;
            break;
        }
        auto step_max = [&](crd::containers::ConstSpan<T> v, crd::containers::ConstSpan<T> dv) -> T
        {
            T amax = static_cast<T>(1);
            for (crd::usize i = 0; i < v.size(); ++i)
            {
                if (dv[i] < static_cast<T>(0))
                {
                    const T a = -v[i] / dv[i];
                    amax = a < amax ? a : amax;
                }
            }
            return amax;
        };
        const T a_aff = step_max({s.data(), mi}, {ds_aff.data(), mi}) < step_max({zi.data(), mi}, {dz_aff.data(), mi})
                            ? step_max({s.data(), mi}, {ds_aff.data(), mi})
                            : step_max({zi.data(), mi}, {dz_aff.data(), mi});
        T mu_aff = static_cast<T>(0);
        for (crd::usize i = 0; i < mi; ++i)
        {
            mu_aff += (s[i] + a_aff * ds_aff[i]) * (zi[i] + a_aff * dz_aff[i]);
        }
        mu_aff = mi > 0 ? mu_aff / static_cast<T>(mi) : static_cast<T>(0);
        const T sigma = mu > static_cast<T>(0) ? (mu_aff / mu) * (mu_aff / mu) * (mu_aff / mu) : static_cast<T>(0);

        // Corrector: rc = S·Z·e + ΔS_aff·ΔZ_aff·e − σμ·e.
        for (crd::usize i = 0; i < mi; ++i)
        {
            rc[i] = s[i] * zi[i] + ds_aff[i] * dz_aff[i] - sigma * mu;
        }
        if (!newton_solve({rc.data(), mi}, {dx.data(), n}, {dye.data(), me}, {ds.data(), mi}, {dz.data(), mi}))
        {
            status = QpStatus::NumericalError;
            break;
        }
        const T a_pr = step_max({s.data(), mi}, {ds.data(), mi});
        const T a_du = step_max({zi.data(), mi}, {dz.data(), mi});
        const T eta = static_cast<T>(0.995); // fraction to the boundary
        T a = a_pr < a_du ? a_pr : a_du;
        a = eta * a < static_cast<T>(1) ? eta * a : static_cast<T>(1);

        for (crd::usize j = 0; j < n; ++j)
        {
            x[j] += a * dx[j];
        }
        for (crd::usize i = 0; i < me; ++i)
        {
            ye[i] += a * dye[i];
        }
        for (crd::usize i = 0; i < mi; ++i)
        {
            s[i] += a * ds[i];
            zi[i] += a * dz[i];
        }
    }

    // Report in the canonical form: x; y_i = (eq: ye) | (z_upper − z_lower) per original row (OSQP sign).
    for (crd::usize j = 0; j < n; ++j)
    {
        result.x[j] = x[j];
    }
    {
        crd::usize we = 0;
        for (crd::usize i = 0; i < prob.m; ++i)
        {
            if (prob.l[i] == prob.u[i])
            {
                result.y[i] = ye[we++];
            }
            else
            {
                result.y[i] = static_cast<T>(0);
            }
        }
        for (crd::usize k = 0; k < mi; ++k)
        {
            const crd::usize row = static_cast<crd::usize>(gsrc[k] > 0 ? gsrc[k] - 1 : -gsrc[k] - 1);
            result.y[row] += gsrc[k] > 0 ? zi[k] : -zi[k];
        }
    }
    result.iterations = it;
    result.status = status;
    detail::qp_finalize<T>(prob, result, alloc);
    return result;
}

} // namespace crd::hesap::opt
