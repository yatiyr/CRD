#pragma once

// levenberg_marquardt_sparse.hpp — Phase 3.1.6 v7-e-2: SPARSE-Jacobian Levenberg-Marquardt — THE CRUSH VEHICLE.
// A sparse Jacobian J makes JᵀJ sparse; we form it (transpose + spgemm) and factor (JᵀJ + λ·diag) with the
// moat-proven hesap-direct SUPERNODAL CHOLESKY — the same kernel that BEAT CHOLMOD (v5a hood/ldoor 1.28-1.33×),
// and Ceres's SPARSE_NORMAL_CHOLESKY *is* CHOLMOD. So on the factorization-dominated regime Cerid sparse-LM can
// beat Ceres-sparse, and it carries the cross-thread determinism moat Ceres lacks (the supernodal factor is
// bit-identical across worker counts). Same Madsen-Nielsen-Tingleff ν-update as the dense path. ADR-0090; the
// hesap-opt→hesap-direct edge (named in ADR-0090). NOT in the opt.hpp umbrella — include explicitly + link
// crd-hesap-direct (keeps the dense opt free of the direct-solver dependency).
//
// ⚠ HONEST (advisor): the crush claim is matched-iterations + per-iteration FACTOR faster + the moat — NOT "beat
// Ceres at NLS generally" (Ceres wins Jacobian-eval, Schur-complement for bundle adjustment, threading we don't
// replicate here). The symbolic analysis (AMD + etree + supernode amalgamation — the v5a CHOLMOD-gap cost) is the
// SupernodalCholesky symbolic-once / numeric-per-trial split: we analyze ONCE (the JᵀJ nonzero structure is
// constant across the whole solve, the same fixed-sparsity assumption Ceres makes when it caches its symbolic
// factorization) and refactorize() every λ-trial — so the per-iteration cost is numeric-only, matching Ceres.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/supernodal_cholesky.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/hesap/opt/residual_function.hpp>
#include <crd/hesap/sparse/spgemm.hpp>
#include <crd/hesap/sparse/structural.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::opt
{

// Minimize ½‖r(x)‖² from x0 with a SPARSE Jacobian. `num_workers` ≥ 2 runs the tree-parallel supernodal factor
// (caller must have crd::jobs::init()'d); the result is bit-identical to serial (the v5a moat).
template <typename T>
[[nodiscard]] OptResult<T> minimize_levenberg_marquardt_sparse(const ResidualFunction<T>& res,
                                                               crd::containers::ConstSpan<T> x0,
                                                               const OptOptions<T>& opts,
                                                               crd::memory::IAllocator* alloc,
                                                               T tau = static_cast<T>(1e-3), crd::u32 num_workers = 1)
{
    namespace sp = crd::hesap::sparse;
    namespace dir = crd::hesap::direct;
    CRD_ASSERT_MSG(res.has_sparse_jacobian(), "minimize_levenberg_marquardt_sparse needs a sparse Jacobian");
    const crd::usize n = res.n();
    const crd::usize m = res.num_residuals();

    OptResult<T> result(alloc);
    result.x.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        result.x[i] = x0[i];
    }
    if (n == 0 || m == 0)
    {
        result.status = OptStatus::Success;
        result.converged = true;
        return result;
    }

    crd::containers::Array<T> r(alloc);
    crd::containers::Array<T> g(alloc);
    crd::containers::Array<T> delta(alloc);
    crd::containers::Array<T> x_new(alloc);
    crd::containers::Array<T> r_new(alloc);
    crd::containers::Array<T> work(alloc);     // (JᵀJ + λ·diag) values for the factor
    crd::containers::Array<T> diag(alloc);     // diag(JᵀJ)
    crd::containers::Array<crd::u32> dpos(alloc); // CSR slot of each diagonal entry in JᵀJ
    r.resize(m);
    g.resize(n);
    delta.resize(n);
    x_new.resize(n);
    r_new.resize(m);
    diag.resize(n);
    dpos.resize(n);

    sp::SparseMatrix<T, sp::SparseFormat::Csr> jmat(alloc); // J (CSR m×n), reused

    T* x = result.x.data();

    auto eval_cost = [&](const T* xv) -> T
    {
        res.residuals({xv, n}, {r.data(), m});
        T c = static_cast<T>(0);
        for (crd::usize i = 0; i < m; ++i)
        {
            c += r[i] * r[i];
        }
        return static_cast<T>(0.5) * c;
    };
    auto inf_norm = [](const T* v, crd::usize len) -> T
    {
        T mx = static_cast<T>(0);
        for (crd::usize i = 0; i < len; ++i)
        {
            const T a = crd::math::fabs(v[i]);
            mx = a > mx ? a : mx;
        }
        return mx;
    };

    // Form JᵀJ (sparse n×n) + g = Jᵀr at the current x; returns the JᵀJ matrix. Also fills g, diag, dpos.
    auto form_normal = [&]() -> sp::SparseMatrix<T, sp::SparseFormat::Csr>
    {
        (void)res.sparse_jacobian({x, n}, jmat);
        sp::SparseMatrix<T, sp::SparseFormat::Csr> jt = sp::transpose(jmat, alloc);
        sp::SparseMatrix<T, sp::SparseFormat::Csr> jtj = sp::spgemm(jt, jmat, alloc); // Jᵀ·J
        // g = Jᵀ·r: iterate J's CSR (row = residual i), scatter into g by column.
        for (crd::usize j = 0; j < n; ++j)
        {
            g[j] = static_cast<T>(0);
        }
        const sp::SparsePattern& jp = jmat.pattern();
        const T*                 jv = jmat.values().values.data();
        for (crd::usize i = 0; i < m; ++i)
        {
            const crd::u32 lo = jp.outer_ptr[i];
            const crd::u32 hi = jp.outer_ptr[i + 1];
            const T        ri = r[i];
            for (crd::u32 k = lo; k < hi; ++k)
            {
                g[jp.inner_idx[k]] += jv[k] * ri;
            }
        }
        // diag(JᵀJ) + the CSR slot of each diagonal entry.
        const sp::SparsePattern& gp = jtj.pattern();
        const T*                 gv = jtj.values().values.data();
        for (crd::u32 i = 0; i < n; ++i)
        {
            diag[i] = static_cast<T>(0);
            dpos[i] = gp.outer_ptr[i + 1]; // sentinel = no diagonal (shouldn't happen for JᵀJ)
            for (crd::u32 k = gp.outer_ptr[i]; k < gp.outer_ptr[i + 1]; ++k)
            {
                if (gp.inner_idx[k] == i)
                {
                    diag[i] = gv[k];
                    dpos[i] = k;
                    break;
                }
            }
        }
        return jtj;
    };

    T cost = eval_cost(x);
    sp::SparseMatrix<T, sp::SparseFormat::Csr> jtj = form_normal();
    T grad_norm = inf_norm(g.data(), n);

    T maxdiag = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        maxdiag = diag[i] > maxdiag ? diag[i] : maxdiag;
    }
    T lambda = tau * maxdiag;
    T nu = static_cast<T>(2);

    const crd::usize nnz = jtj.pattern().nnz();
    work.resize(nnz);

    // GATE (v7-e-2): analyze the supernodal symbolic ONCE — the JᵀJ nonzero structure is constant across the whole
    // solve (fixed-sparsity NLS, the same assumption Ceres makes caching its symbolic). Every λ-trial and every
    // iteration after the first calls refactorize() (numeric only) so the symbolic phase (AMD + etree + supernode
    // amalgamation — the v5a CHOLMOD-gap cost) is paid a single time — the lever for the Ceres wall-clock crush.
    dir::SupernodalCholesky<T> chol(alloc);
    bool                       analyzed = false;

    OptStatus  status = OptStatus::MaxIterations;
    crd::usize it = 0;
    for (; it < opts.max_iters; ++it)
    {
        if (opts.record_history)
        {
            result.history.push_back(cost);
        }
        if (grad_norm <= opts.grad_tol)
        {
            status = OptStatus::Success;
            break;
        }

        // Build (JᵀJ + λ·diag) values + factor + solve (raise λ on non-PD).
        bool solved = false;
        for (int attempt = 0; attempt < 30 && !solved; ++attempt)
        {
            const T* gv = jtj.values().values.data();
            for (crd::usize k = 0; k < nnz; ++k)
            {
                work[k] = gv[k];
            }
            for (crd::usize i = 0; i < n; ++i)
            {
                work[dpos[i]] += lambda * diag[i];
            }
            if (!analyzed)
            {
                chol.factorize(jtj.pattern(), {work.data(), nnz}, dir::kSupernodeRelax, num_workers);
                analyzed = true; // m_sym is computed before the numeric phase ⇒ valid even if this trial is non-PD
            }
            else
            {
                chol.refactorize(jtj.pattern(), {work.data(), nnz}, num_workers); // reuse symbolic — the gate
            }
            if (chol.info() != 0)
            {
                lambda = lambda > static_cast<T>(0) ? lambda * nu : static_cast<T>(1);
                nu *= static_cast<T>(2);
                continue;
            }
            for (crd::usize i = 0; i < n; ++i)
            {
                delta[i] = -g[i];
            }
            solved = chol.solve({delta.data(), n}, 1);
            if (!solved)
            {
                lambda = lambda > static_cast<T>(0) ? lambda * nu : static_cast<T>(1);
                nu *= static_cast<T>(2);
            }
        }
        if (!solved)
        {
            status = OptStatus::LineSearchFailed;
            break;
        }

        T step_norm_sq = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            x_new[i] = x[i] + delta[i];
            step_norm_sq += delta[i] * delta[i];
        }
        res.residuals({x_new.data(), n}, {r_new.data(), m});
        T new_cost = static_cast<T>(0);
        for (crd::usize i = 0; i < m; ++i)
        {
            new_cost += r_new[i] * r_new[i];
        }
        new_cost *= static_cast<T>(0.5);

        T pred = static_cast<T>(0); // ½·δᵀ(λ·diag·δ − g)  (Madsen)
        for (crd::usize i = 0; i < n; ++i)
        {
            pred += delta[i] * (lambda * diag[i] * delta[i] - g[i]);
        }
        pred *= static_cast<T>(0.5);
        const T rho = pred > static_cast<T>(0) ? (cost - new_cost) / pred : (cost - new_cost);

        if (rho > static_cast<T>(0) && std::isfinite(new_cost))
        {
            const T df = cost - new_cost;
            for (crd::usize i = 0; i < n; ++i)
            {
                x[i] = x_new[i];
            }
            cost = eval_cost(x);
            jtj = form_normal();
            grad_norm = inf_norm(g.data(), n);
            const T two_rho_m1 = static_cast<T>(2) * rho - static_cast<T>(1);
            const T factor = static_cast<T>(1) - two_rho_m1 * two_rho_m1 * two_rho_m1;
            const T lo = static_cast<T>(1) / static_cast<T>(3);
            lambda *= factor > lo ? factor : lo;
            nu = static_cast<T>(2);
            const T x_norm = inf_norm(x, n);
            const auto stop = check_convergence<T>(grad_norm, crd::math::sqrt(step_norm_sq), crd::math::fabs(df), x_norm, cost,
                                                   opts);
            if (stop.has_value())
            {
                status = *stop;
                break;
            }
        }
        else
        {
            lambda *= nu;
            nu *= static_cast<T>(2);
        }
    }

    result.fx = cost;
    result.grad_norm = grad_norm;
    result.iterations = it;
    result.status = status;
    result.converged = (status == OptStatus::Success);
    return result;
}

} // namespace crd::hesap::opt
