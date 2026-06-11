#pragma once

// newton_sparse.hpp — Phase 3.1.6 v7-g: SPARSE-Hessian full/modified Newton — the structured-second-order path.
// The Hessian comes in as CSR (Objective::sparse_hessian) and (∇²f + τ·I)·p = −∇f is factored with the
// moat-proven hesap-direct SUPERNODAL CHOLESKY (the v5a kernel that beat CHOLMOD on hood/ldoor), so the Newton
// step inherits the cross-thread bit-determinism moat. τ by N&W Algorithm 3.4 (escalate on a failed factor —
// info() != 0 detects the indefinite case). SYMBOLIC-ONCE GATE (same as v7-e-2 sparse-LM): for a fixed-pattern
// objective the Hessian sparsity is constant across iterations, so the symbolic analysis (AMD + etree +
// amalgamation) is paid ONCE and every later iteration/τ-retry calls refactorize() (numeric only).
// FIXED-SPARSITY CONTRACT: sparse_hessian must return the SAME nonzero pattern every call (values change) —
// the same assumption Ceres makes caching its symbolic factorization. ADR-0090; the hesap-opt→hesap-direct
// edge (named in ADR-0090). NOT in the opt.hpp umbrella — include explicitly + link crd-hesap-direct
// (keeps the dense opt free of the direct-solver dependency).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/supernodal_cholesky.hpp>
#include <crd/hesap/opt/convergence.hpp>
#include <crd/hesap/opt/line_search.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/hesap/opt/wolfe_line_search.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::opt
{

// Minimize `obj` from `x0` by sparse (modified) Newton. Requires has_gradient() + has_sparse_hessian().
// `num_workers` ≥ 2 runs the tree-parallel supernodal factor (caller must have crd::jobs::init()'d); the result
// is bit-identical to serial (the v5a moat). `OptResult::hess_evals` counts sparse_hessian() evaluations.
template <typename T>
[[nodiscard]] OptResult<T> minimize_newton_sparse(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                                  const OptOptions<T>& opts, crd::memory::IAllocator* alloc,
                                                  const LineSearch<T>* line_search = nullptr, crd::u32 num_workers = 1)
{
    namespace sp = crd::hesap::sparse;
    namespace dir = crd::hesap::direct;
    CRD_ASSERT_MSG(obj.has_gradient(), "minimize_newton_sparse needs an analytic gradient");
    CRD_ASSERT_MSG(obj.has_sparse_hessian(), "minimize_newton_sparse needs a sparse Hessian (CSR)");
    const crd::usize n = obj.n();

    OptResult<T> result(alloc);
    result.x.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        result.x[i] = x0[i];
    }
    if (n == 0)
    {
        result.status = OptStatus::Success;
        result.converged = true;
        return result;
    }

    auto inf_nrm = [](crd::containers::ConstSpan<T> w) -> T
    {
        T mx = static_cast<T>(0);
        for (crd::usize i = 0; i < w.size(); ++i)
        {
            const T a = std::fabs(w[i]);
            mx = a > mx ? a : mx;
        }
        return mx;
    };

    crd::containers::Array<T> g(alloc);
    crd::containers::Array<T> g_new(alloc);
    crd::containers::Array<T> p(alloc);
    crd::containers::Array<T> x_new(alloc);
    crd::containers::Array<T> work(alloc);        // (H + τ·I) values for the factor
    crd::containers::Array<crd::u32> dpos(alloc); // CSR slot of each diagonal entry of H
    g.resize(n);
    g_new.resize(n);
    p.resize(n);
    x_new.resize(n);
    dpos.resize(n);

    sp::SparseMatrix<T, sp::SparseFormat::Csr> hmat(alloc); // H (CSR n×n), caller-owned + reused per the contract

    const WolfeLineSearch<T> default_ls; // strong Wolfe c1=1e-4, c2=0.9; α₀ = 1 (Newton steps self-scale)
    const LineSearch<T>& ls = line_search != nullptr ? *line_search : default_ls;

    // GATE (v7-e-2 pattern): the supernodal symbolic is analyzed ONCE (fixed Hessian sparsity across the solve);
    // every later iteration and every τ-retry is refactorize() — numeric only.
    dir::SupernodalCholesky<T> chol(alloc);
    bool analyzed = false;

    T* x = result.x.data();
    T fx = obj.value({x, n});
    ++result.fn_evals;
    (void)obj.gradient({x, n}, {g.data(), n});
    ++result.grad_evals;
    T grad_norm = inf_nrm({g.data(), n});

    OptStatus status = OptStatus::MaxIterations;
    crd::usize it = 0;
    for (; it < opts.max_iters; ++it)
    {
        if (opts.record_history)
        {
            result.history.push_back(fx);
        }
        if (grad_norm <= opts.grad_tol)
        {
            status = OptStatus::Success;
            break;
        }

        (void)obj.sparse_hessian({x, n}, hmat);
        ++result.hess_evals;
        const sp::SparsePattern& hp = hmat.pattern();
        const T* hv = hmat.values().values.data();
        const crd::usize nnz = hp.nnz();
        work.resize(nnz);

        // Diagonal CSR slots + the N&W Alg 3.4 τ start (0 when all diagonal entries positive).
        T mindiag = std::numeric_limits<T>::max();
        T maxabsdiag = static_cast<T>(0);
        for (crd::u32 i = 0; i < n; ++i)
        {
            dpos[i] = hp.outer_ptr[i + 1]; // sentinel — a structurally-missing diagonal stays τ-less (asserted PD)
            for (crd::u32 k = hp.outer_ptr[i]; k < hp.outer_ptr[i + 1]; ++k)
            {
                if (hp.inner_idx[k] == i)
                {
                    dpos[i] = k;
                    const T d = hv[k];
                    mindiag = d < mindiag ? d : mindiag;
                    const T a = std::fabs(d);
                    maxabsdiag = a > maxabsdiag ? a : maxabsdiag;
                    break;
                }
            }
            CRD_ASSERT_MSG(dpos[i] < hp.outer_ptr[i + 1],
                           "minimize_newton_sparse: sparse_hessian must store every diagonal entry (τ·I lands there)");
        }
        const T beta = static_cast<T>(1e-3) * (maxabsdiag > static_cast<T>(0) ? maxabsdiag : static_cast<T>(1));
        T tau = mindiag > static_cast<T>(0) ? static_cast<T>(0) : (-mindiag + beta);

        bool solved = false;
        for (int attempt = 0; attempt < 60 && !solved; ++attempt)
        {
            for (crd::usize k = 0; k < nnz; ++k)
            {
                work[k] = hv[k];
            }
            for (crd::usize i = 0; i < n; ++i)
            {
                work[dpos[i]] += tau;
            }
            if (!analyzed)
            {
                chol.factorize(hp, {work.data(), nnz}, dir::kSupernodeRelax, num_workers);
                analyzed = true; // m_sym is computed before the numeric phase ⇒ valid even if this try is non-PD
            }
            else
            {
                chol.refactorize(hp, {work.data(), nnz}, num_workers); // reuse symbolic — the gate
            }
            if (chol.info() != 0)
            {
                const T doubled = static_cast<T>(2) * tau;
                tau = doubled > beta ? doubled : beta; // N&W 3.4: τ ← max(2τ, β) — max, NOT a stuck-at-β ladder
                continue;
            }
            for (crd::usize i = 0; i < n; ++i)
            {
                p[i] = -g[i];
            }
            solved = chol.solve({p.data(), n}, 1);
            if (!solved)
            {
                const T doubled = static_cast<T>(2) * tau;
                tau = doubled > beta ? doubled : beta; // τ ← max(2τ, β)
            }
        }
        if (!solved)
        {
            status = OptStatus::LineSearchFailed; // could not form a descent direction
            break;
        }

        const auto lr = ls.search(obj, {x, n}, fx, {g.data(), n}, {p.data(), n}, static_cast<T>(1), {x_new.data(), n},
                                  {g_new.data(), n});
        result.fn_evals += lr.evals;
        result.grad_evals += lr.grad_evals;
        if (!lr.ok)
        {
            status = OptStatus::LineSearchFailed;
            break;
        }
        if (!lr.grad_at_new_valid)
        {
            (void)obj.gradient({x_new.data(), n}, {g_new.data(), n});
            ++result.grad_evals;
        }

        T step_norm_sq = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T dx = x_new[i] - x[i];
            step_norm_sq += dx * dx;
            x[i] = x_new[i];
            g[i] = g_new[i];
        }
        const T df = std::fabs(lr.fx_new - fx);
        fx = lr.fx_new;
        grad_norm = inf_nrm({g.data(), n});

        const auto stop = check_convergence<T>(grad_norm, std::sqrt(step_norm_sq), df, inf_nrm({x, n}), fx, opts);
        if (stop.has_value())
        {
            status = *stop;
            break;
        }
    }

    result.fx = fx;
    result.grad_norm = grad_norm;
    result.iterations = it;
    result.status = status;
    result.converged = (status == OptStatus::Success);
    return result;
}

} // namespace crd::hesap::opt
