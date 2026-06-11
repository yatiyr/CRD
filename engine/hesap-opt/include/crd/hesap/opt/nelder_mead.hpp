#pragma once

// nelder_mead.hpp — Phase 3.1.6 v7-p-1: NELDER-MEAD (1965) — the downhill-simplex direct search, faithful to
// scipy's `_minimize_neldermead` semantics so the v7-z eval-parity comparison is same-algorithm:
//   • initial simplex: x0 + per-coordinate 5% perturbation (0.00025 absolute where x0_i == 0) — scipy's rule;
//   • the reflect (ρ) / expand (χ) / contract (ψ) / shrink (σ) cycle with scipy's exact accept conditions;
//   • optional ADAPTIVE parameters (Gao-Han 2012: χ = 1 + 2/n, ψ = 3/4 − 1/(2n), σ = 1 − 1/n) for large n;
//   • termination: simplex spread ≤ xatol AND f-spread ≤ fatol (both, like scipy).
// Value-only — no gradients anywhere (the derivative-free family's contract). The stable simplex ordering is an
// index-tie-broken insertion sort (deterministic; the repo bans std::sort). [gold: scipy 'Nelder-Mead', NLopt
// NELDERMEAD — the v7-z scoreboard]. ADR-0090.
//
// DETERMINISM: RNG-free, serial, fixed evaluation order ⇒ bit-identical runs by construction.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::opt
{

template <typename T> struct NelderMeadOptions
{
    T xatol = static_cast<T>(1e-8); // simplex-spread tolerance (scipy default 1e-4; ours tighter)
    T fatol = static_cast<T>(1e-8); // f-spread tolerance
    bool adaptive = false;          // Gao-Han dimension-adaptive parameters
    crd::usize max_fun = 0;         // function-evaluation cap; 0 ⇒ 200·n (scipy's rule)
};

template <typename T>
[[nodiscard]] OptResult<T> minimize_nelder_mead(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                                const OptOptions<T>& opts, crd::memory::IAllocator* alloc,
                                                const NelderMeadOptions<T>& nm = {})
{
    const crd::usize n = obj.n();
    CRD_ASSERT_MSG(x0.size() == n, "minimize_nelder_mead: x0 size mismatch");

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

    // scipy parameters (rho, chi, psi, sigma); adaptive = Gao-Han.
    const T nn = static_cast<T>(n);
    const T rho = static_cast<T>(1);
    const T chi = nm.adaptive ? static_cast<T>(1) + static_cast<T>(2) / nn : static_cast<T>(2);
    const T psi =
        nm.adaptive ? static_cast<T>(0.75) - static_cast<T>(1) / (static_cast<T>(2) * nn) : static_cast<T>(0.5);
    const T sigma = nm.adaptive ? static_cast<T>(1) - static_cast<T>(1) / nn : static_cast<T>(0.5);

    const crd::usize npts = n + 1;
    crd::containers::Array<T> sim(alloc);  // (n+1) × n simplex, row-major
    crd::containers::Array<T> fsim(alloc); // f per vertex
    crd::containers::Array<crd::u32> order(alloc);
    crd::containers::Array<T> xbar(alloc);
    crd::containers::Array<T> xtrial(alloc);
    sim.resize(npts * n);
    fsim.resize(npts);
    order.resize(npts);
    xbar.resize(n);
    xtrial.resize(n);

    const crd::usize max_fun = nm.max_fun > 0 ? nm.max_fun : 200 * n;
    auto feval = [&](const T* x) -> T
    {
        ++result.fn_evals;
        return obj.value({x, n});
    };

    // Initial simplex (scipy's nonzdelt/zdelt rule).
    for (crd::usize j = 0; j < n; ++j)
    {
        sim[j] = x0[j];
    }
    for (crd::usize k = 0; k < n; ++k)
    {
        T* row = sim.data() + (k + 1) * n;
        for (crd::usize j = 0; j < n; ++j)
        {
            row[j] = x0[j];
        }
        row[k] = row[k] != static_cast<T>(0) ? row[k] * static_cast<T>(1.05) : static_cast<T>(0.00025);
    }
    for (crd::usize i = 0; i < npts; ++i)
    {
        fsim[i] = feval(sim.data() + i * n);
        order[i] = static_cast<crd::u32>(i);
    }

    // Stable insertion sort of `order` by fsim (index tie-break — deterministic; no std::sort by repo rule).
    auto sort_order = [&]()
    {
        for (crd::usize i = 1; i < npts; ++i)
        {
            const crd::u32 key = order[i];
            crd::usize k = i;
            while (k > 0 && fsim[order[k - 1]] > fsim[key])
            {
                order[k] = order[k - 1];
                --k;
            }
            order[k] = key;
        }
    };
    sort_order();

    OptStatus status = OptStatus::MaxIterations;
    crd::usize it = 0;
    for (; it < opts.max_iters && result.fn_evals < max_fun; ++it)
    {
        const T* best = sim.data() + order[0] * n;
        if (opts.record_history)
        {
            result.history.push_back(fsim[order[0]]);
        }
        // Convergence: BOTH spreads small (scipy).
        T xspread = static_cast<T>(0);
        T fspread = static_cast<T>(0);
        for (crd::usize i = 1; i < npts; ++i)
        {
            const T* row = sim.data() + order[i] * n;
            for (crd::usize j = 0; j < n; ++j)
            {
                const T d = std::fabs(row[j] - best[j]);
                xspread = d > xspread ? d : xspread;
            }
            const T df = std::fabs(fsim[order[i]] - fsim[order[0]]);
            fspread = df > fspread ? df : fspread;
        }
        if (xspread <= nm.xatol && fspread <= nm.fatol)
        {
            status = OptStatus::Success;
            break;
        }

        const crd::u32 worst = order[npts - 1];
        T* xw = sim.data() + worst * n;
        for (crd::usize j = 0; j < n; ++j) // centroid of all but the worst
        {
            T acc = static_cast<T>(0);
            for (crd::usize i = 0; i + 1 < npts; ++i)
            {
                acc += sim[order[i] * n + j];
            }
            xbar[j] = acc / static_cast<T>(n);
        }

        // Reflection.
        for (crd::usize j = 0; j < n; ++j)
        {
            xtrial[j] = (static_cast<T>(1) + rho) * xbar[j] - rho * xw[j];
        }
        const T fr = feval(xtrial.data());
        bool shrink = false;
        if (fr < fsim[order[0]])
        {
            // Expansion.
            crd::containers::Array<T> xe(alloc);
            xe.resize(n);
            for (crd::usize j = 0; j < n; ++j)
            {
                xe[j] = (static_cast<T>(1) + rho * chi) * xbar[j] - rho * chi * xw[j];
            }
            const T fe = feval(xe.data());
            const T* take = fe < fr ? xe.data() : xtrial.data();
            const T ftake = fe < fr ? fe : fr;
            for (crd::usize j = 0; j < n; ++j)
            {
                xw[j] = take[j];
            }
            fsim[worst] = ftake;
        }
        else if (fr < fsim[order[npts - 2]]) // better than the second-worst: accept the reflection
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                xw[j] = xtrial[j];
            }
            fsim[worst] = fr;
        }
        else if (fr < fsim[worst]) // outside contraction
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                xtrial[j] = (static_cast<T>(1) + psi * rho) * xbar[j] - psi * rho * xw[j];
            }
            const T fc = feval(xtrial.data());
            if (fc <= fr)
            {
                for (crd::usize j = 0; j < n; ++j)
                {
                    xw[j] = xtrial[j];
                }
                fsim[worst] = fc;
            }
            else
            {
                shrink = true;
            }
        }
        else // inside contraction
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                xtrial[j] = (static_cast<T>(1) - psi) * xbar[j] + psi * xw[j];
            }
            const T fcc = feval(xtrial.data());
            if (fcc < fsim[worst])
            {
                for (crd::usize j = 0; j < n; ++j)
                {
                    xw[j] = xtrial[j];
                }
                fsim[worst] = fcc;
            }
            else
            {
                shrink = true;
            }
        }
        if (shrink)
        {
            const T* xb = sim.data() + order[0] * n;
            for (crd::usize i = 1; i < npts; ++i)
            {
                T* row = sim.data() + order[i] * n;
                for (crd::usize j = 0; j < n; ++j)
                {
                    row[j] = xb[j] + sigma * (row[j] - xb[j]);
                }
                fsim[order[i]] = feval(row);
            }
        }
        sort_order();
    }

    const T* xbest = sim.data() + order[0] * n;
    for (crd::usize j = 0; j < n; ++j)
    {
        result.x[j] = xbest[j];
    }
    result.fx = fsim[order[0]];
    result.status = status;
    result.converged = status == OptStatus::Success;
    result.iterations = it;
    return result;
}

} // namespace crd::hesap::opt
