#pragma once

// global_search.hpp — Phase 3.1.6 v7-q: the box-constrained GLOBAL/metaheuristic family over the Philox
// stream (every member bit-identical run-to-run by construction — same (seed, stream) ⇒ same trajectory):
//   • `minimize_differential_evolution` — scipy's best/1/bin semantics: NP = popsize·n population uniform in
//     the box, per-generation DITHERED F ~ U(0.5, 1), binomial crossover CR with a guaranteed dimension,
//     bound clipping, greedy selection; scipy's convergence test (population f-std ≤ atol + tol·|f-mean|).
//   • `minimize_pso` — global-best PSO with Clerc's constriction defaults (w = 0.7298, c1 = c2 = 1.49618),
//     velocity capped at the box range, positions clipped.
//   • `minimize_simulated_annealing` — the CLASSICAL form: geometric cooling T ← γT, box-clipped Gaussian
//     neighborhood scaled by T, Metropolis acceptance, best-ever tracking. (scipy's dual_annealing adds the
//     Tsallis visiting distribution + a local-search phase — NOT shipped; named scope.)
//   • `minimize_basin_hopping` — scipy semantics: a local minimizer per hop (Nelder-Mead value-only, or
//     L-BFGS over 2-point finite differences = scipy basinhopping's DEFAULT local method; eval counts include
//     every FD probe, matching scipy's nfev) + uniform step perturbation + Metropolis acceptance at
//     temperature T over the LOCAL minima.
//   • `minimize_multi_start` — k Philox starts in the box, Nelder-Mead local each, best wins (the honest
//     baseline every metaheuristic must beat).
// All value-only. [gold: scipy (differential_evolution / basinhopping), pycma's baselines — v7-z]. ADR-0090.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/finite_difference.hpp>
#include <crd/hesap/opt/lbfgs.hpp>
#include <crd/hesap/opt/nelder_mead.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/hesap/stats/normal.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::opt
{

template <typename T> struct DeOptions
{
    crd::usize popsize = 15;      // population = popsize·n (scipy's default factor)
    crd::usize max_gens = 1000;   // generation cap (scipy maxiter)
    T cr = static_cast<T>(0.7);   // crossover probability (scipy's recombination)
    T tol = static_cast<T>(0.01); // relative convergence (scipy)
    T atol = static_cast<T>(0);   // absolute convergence (scipy)
    crd::u64 seed = 0xDEULL;
};

template <typename T>
[[nodiscard]] OptResult<T> minimize_differential_evolution(const Objective<T>& obj, crd::containers::ConstSpan<T> lower,
                                                           crd::containers::ConstSpan<T> upper,
                                                           crd::memory::IAllocator* alloc, const DeOptions<T>& de = {})
{
    namespace st = crd::hesap::stats;
    const crd::usize n = obj.n();
    CRD_ASSERT_MSG(lower.size() == n && upper.size() == n, "minimize_differential_evolution: bounds required");

    OptResult<T> result(alloc);
    result.x.resize(n);
    if (n == 0)
    {
        result.status = OptStatus::Success;
        result.converged = true;
        return result;
    }

    const crd::usize np = de.popsize * n < 4 ? 4 : de.popsize * n;
    crd::containers::Array<T> pop(alloc);
    crd::containers::Array<T> fpop(alloc);
    crd::containers::Array<T> trial(alloc);
    pop.resize(np * n);
    fpop.resize(np);
    trial.resize(n);

    st::PhiloxRng rng(de.seed, /*stream=*/0U);
    crd::usize evals = 0;
    crd::usize best = 0;
    for (crd::usize k = 0; k < np; ++k)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            pop[k * n + i] = lower[i] + static_cast<T>(rng.next_f64()) * (upper[i] - lower[i]);
        }
        fpop[k] = obj.value({pop.data() + k * n, n});
        ++evals;
        if (fpop[k] < fpop[best])
        {
            best = k;
        }
    }

    OptStatus status = OptStatus::MaxIterations;
    crd::usize gen = 0;
    for (; gen < de.max_gens; ++gen)
    {
        // Dithered mutation factor, one per generation (scipy's (0.5, 1) default).
        const T f = static_cast<T>(0.5) + static_cast<T>(0.5) * static_cast<T>(rng.next_f64());
        for (crd::usize k = 0; k < np; ++k)
        {
            // best/1: pick r1 != r2 != k.
            crd::usize r1 = k;
            while (r1 == k)
            {
                r1 = static_cast<crd::usize>(rng.next_below(np));
            }
            crd::usize r2 = k;
            while (r2 == k || r2 == r1)
            {
                r2 = static_cast<crd::usize>(rng.next_below(np));
            }
            const crd::usize jrand = static_cast<crd::usize>(rng.next_below(n)); // the guaranteed dimension
            for (crd::usize i = 0; i < n; ++i)
            {
                const bool cross = static_cast<T>(rng.next_f64()) < de.cr || i == jrand;
                T v = cross ? pop[best * n + i] + f * (pop[r1 * n + i] - pop[r2 * n + i]) : pop[k * n + i];
                v = v < lower[i] ? lower[i] : (v > upper[i] ? upper[i] : v); // clip (scipy semantics)
                trial[i] = v;
            }
            const T ft = obj.value({trial.data(), n});
            ++evals;
            if (ft <= fpop[k]) // greedy selection
            {
                for (crd::usize i = 0; i < n; ++i)
                {
                    pop[k * n + i] = trial[i];
                }
                fpop[k] = ft;
                if (ft < fpop[best])
                {
                    best = k;
                }
            }
        }
        // scipy's convergence: std(f) <= atol + tol·|mean(f)|.
        T fmean = static_cast<T>(0);
        for (crd::usize k = 0; k < np; ++k)
        {
            fmean += fpop[k];
        }
        fmean /= static_cast<T>(np);
        T fvar = static_cast<T>(0);
        for (crd::usize k = 0; k < np; ++k)
        {
            const T d = fpop[k] - fmean;
            fvar += d * d;
        }
        const T fstd = std::sqrt(fvar / static_cast<T>(np));
        if (fstd <= de.atol + de.tol * std::fabs(fmean))
        {
            status = OptStatus::Success;
            ++gen;
            break;
        }
    }

    for (crd::usize i = 0; i < n; ++i)
    {
        result.x[i] = pop[best * n + i];
    }
    result.fx = fpop[best];
    result.fn_evals = evals;
    result.iterations = gen;
    result.status = status;
    result.converged = status == OptStatus::Success;
    return result;
}

template <typename T> struct PsoOptions
{
    crd::usize swarm = 0; // 0 ⇒ 10 + 2·√n (a common default)
    crd::usize max_iters = 1000;
    T w = static_cast<T>(0.7298); // Clerc constriction
    T c1 = static_cast<T>(1.49618);
    T c2 = static_cast<T>(1.49618);
    T ftol = static_cast<T>(1e-10); // swarm-best stagnation tolerance
    crd::usize patience = 50;       // stagnant iterations before stopping
    crd::u64 seed = 0x9500ULL;
};

template <typename T>
[[nodiscard]] OptResult<T> minimize_pso(const Objective<T>& obj, crd::containers::ConstSpan<T> lower,
                                        crd::containers::ConstSpan<T> upper, crd::memory::IAllocator* alloc,
                                        const PsoOptions<T>& po = {})
{
    namespace st = crd::hesap::stats;
    const crd::usize n = obj.n();
    CRD_ASSERT_MSG(lower.size() == n && upper.size() == n, "minimize_pso: bounds required");

    OptResult<T> result(alloc);
    result.x.resize(n);
    if (n == 0)
    {
        result.status = OptStatus::Success;
        result.converged = true;
        return result;
    }

    const crd::usize swarm = po.swarm > 0 ? po.swarm : 10 + 2 * static_cast<crd::usize>(std::sqrt(static_cast<T>(n)));
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> v(alloc);
    crd::containers::Array<T> pbest(alloc);
    crd::containers::Array<T> fpbest(alloc);
    x.resize(swarm * n);
    v.resize(swarm * n);
    pbest.resize(swarm * n);
    fpbest.resize(swarm);

    st::PhiloxRng rng(po.seed, /*stream=*/0U);
    crd::usize evals = 0;
    T gbest_f = std::numeric_limits<T>::infinity();
    for (crd::usize k = 0; k < swarm; ++k)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            const T range = upper[i] - lower[i];
            x[k * n + i] = lower[i] + static_cast<T>(rng.next_f64()) * range;
            v[k * n + i] = (static_cast<T>(rng.next_f64()) - static_cast<T>(0.5)) * range;
            pbest[k * n + i] = x[k * n + i];
        }
        fpbest[k] = obj.value({x.data() + k * n, n});
        ++evals;
        if (fpbest[k] < gbest_f)
        {
            gbest_f = fpbest[k];
            for (crd::usize i = 0; i < n; ++i)
            {
                result.x[i] = x[k * n + i];
            }
        }
    }

    OptStatus status = OptStatus::MaxIterations;
    crd::usize it = 0;
    crd::usize stagnant = 0;
    for (; it < po.max_iters; ++it)
    {
        const T gbest_before = gbest_f;
        for (crd::usize k = 0; k < swarm; ++k)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                const T range = upper[i] - lower[i];
                const T r1 = static_cast<T>(rng.next_f64());
                const T r2 = static_cast<T>(rng.next_f64());
                T vi = po.w * v[k * n + i] + po.c1 * r1 * (pbest[k * n + i] - x[k * n + i]) +
                       po.c2 * r2 * (result.x[i] - x[k * n + i]);
                vi = vi < -range ? -range : (vi > range ? range : vi); // velocity cap
                v[k * n + i] = vi;
                T xi = x[k * n + i] + vi;
                xi = xi < lower[i] ? lower[i] : (xi > upper[i] ? upper[i] : xi);
                x[k * n + i] = xi;
            }
            const T fk = obj.value({x.data() + k * n, n});
            ++evals;
            if (fk < fpbest[k])
            {
                fpbest[k] = fk;
                for (crd::usize i = 0; i < n; ++i)
                {
                    pbest[k * n + i] = x[k * n + i];
                }
                if (fk < gbest_f)
                {
                    gbest_f = fk;
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        result.x[i] = x[k * n + i];
                    }
                }
            }
        }
        stagnant = gbest_before - gbest_f < po.ftol ? stagnant + 1 : 0;
        if (stagnant >= po.patience)
        {
            status = OptStatus::Success;
            ++it;
            break;
        }
    }

    result.fx = gbest_f;
    result.fn_evals = evals;
    result.iterations = it;
    result.status = status;
    result.converged = status == OptStatus::Success;
    return result;
}

template <typename T> struct SaOptions
{
    T t0 = static_cast<T>(1);         // initial temperature
    T cooling = static_cast<T>(0.95); // geometric factor
    crd::usize iters_per_temp = 50;
    crd::usize max_temps = 200;
    T step_scale = static_cast<T>(0.1); // neighborhood = N(0, (scale·range·T/T0)²) per coordinate
    crd::u64 seed = 0x5AULL;
};

template <typename T>
[[nodiscard]] OptResult<T> minimize_simulated_annealing(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                                        crd::containers::ConstSpan<T> lower,
                                                        crd::containers::ConstSpan<T> upper,
                                                        crd::memory::IAllocator* alloc, const SaOptions<T>& sa = {})
{
    namespace st = crd::hesap::stats;
    const crd::usize n = obj.n();
    CRD_ASSERT_MSG(x0.size() == n && lower.size() == n && upper.size() == n,
                   "minimize_simulated_annealing: x0 + bounds required");

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

    st::PhiloxRng rng(sa.seed, /*stream=*/0U);
    st::NormalSampler normal(rng);
    crd::containers::Array<T> cur(alloc);
    crd::containers::Array<T> cand(alloc);
    cur.resize(n);
    cand.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        cur[i] = x0[i];
    }
    T fcur = obj.value({cur.data(), n});
    crd::usize evals = 1;
    T fbest = fcur;

    T t = sa.t0;
    crd::usize temps = 0;
    for (; temps < sa.max_temps; ++temps)
    {
        for (crd::usize sweep = 0; sweep < sa.iters_per_temp; ++sweep)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                const T range = upper[i] - lower[i];
                T xi = cur[i] + sa.step_scale * range * (t / sa.t0) * static_cast<T>(normal.next());
                xi = xi < lower[i] ? lower[i] : (xi > upper[i] ? upper[i] : xi);
                cand[i] = xi;
            }
            const T fc = obj.value({cand.data(), n});
            ++evals;
            const bool accept =
                fc <= fcur || static_cast<T>(rng.next_f64()) <
                                  std::exp((fcur - fc) / (t > static_cast<T>(1e-300) ? t : static_cast<T>(1e-300)));
            if (accept)
            {
                for (crd::usize i = 0; i < n; ++i)
                {
                    cur[i] = cand[i];
                }
                fcur = fc;
                if (fc < fbest)
                {
                    fbest = fc;
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        result.x[i] = cand[i];
                    }
                }
            }
        }
        t *= sa.cooling;
    }

    result.fx = fbest;
    result.fn_evals = evals;
    result.iterations = temps;
    result.status = OptStatus::Success; // SA runs its schedule to completion by design
    result.converged = true;
    return result;
}

enum class BasinHoppingLocal : crd::u8
{
    NelderMead = 0, // value-only simplex (self-contained)
    LbfgsFd = 1,    // L-BFGS over 2-point finite differences — scipy basinhopping's DEFAULT local method
};

namespace detail
{

// Counts every inner value() call (so the basin-hopping eval total includes FD probes, like scipy's nfev).
template <typename T> class CountingObjective final : public Objective<T>
{
public:
    CountingObjective(const Objective<T>& inner, crd::usize* counter) noexcept
        : Objective<T>(/*has_gradient=*/false, /*has_hessian_vector=*/false), m_inner(&inner), m_counter(counter)
    {
    }

    [[nodiscard]] T value(crd::containers::ConstSpan<T> x) const override
    {
        ++*m_counter;
        return m_inner->value(x);
    }

    [[nodiscard]] crd::usize n() const noexcept override { return m_inner->n(); }

private:
    const Objective<T>* m_inner;
    crd::usize* m_counter;
};

} // namespace detail

template <typename T> struct BasinHoppingOptions
{
    crd::usize hops = 100;                 // scipy niter
    T temperature = static_cast<T>(1);     // Metropolis T over LOCAL minima (scipy default)
    T step = static_cast<T>(0.5);          // uniform perturbation half-width (scipy stepsize; ADAPTED below)
    crd::usize adapt_interval = 50;        // scipy's AdaptiveStepsize interval
    T adapt_factor = static_cast<T>(0.9);  // scipy factor: rate high ⇒ step grows (÷factor), low ⇒ shrinks
    T target_accept = static_cast<T>(0.5); // scipy target_accept_rate
    crd::u64 seed = 0xBA51ULL;
    BasinHoppingLocal local_minimizer = BasinHoppingLocal::NelderMead;
    NelderMeadOptions<T> local{};               // the NelderMead-mode local minimizer
    crd::usize local_max_iters = 500;           // per-hop iteration cap (either mode)
    T local_grad_tol = static_cast<T>(1e-5);    // LbfgsFd-mode first-order tolerance (scipy L-BFGS-B pgtol)
    T local_func_tol = static_cast<T>(2.22e-9); // LbfgsFd-mode flat-f stall exit (scipy factr=1e7 ⇒ factr·eps)
                                                // — REQUIRED with FD gradients: at the noise floor ‖∇f‖ never
                                                // crosses grad_tol and the line search thrashes without this
    crd::usize local_lbfgs_memory = 10;         // LbfgsFd-mode history pairs (scipy L-BFGS-B m)
};

template <typename T>
[[nodiscard]] OptResult<T> minimize_basin_hopping(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                                  crd::memory::IAllocator* alloc, const BasinHoppingOptions<T>& bh = {})
{
    namespace st = crd::hesap::stats;
    const crd::usize n = obj.n();
    CRD_ASSERT_MSG(x0.size() == n, "minimize_basin_hopping: x0 size mismatch");

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

    st::PhiloxRng rng(bh.seed, /*stream=*/0U);
    OptOptions<T> lopts;
    lopts.max_iters = bh.local_max_iters;
    lopts.grad_tol = bh.local_grad_tol;

    crd::containers::Array<T> cur(alloc);
    crd::containers::Array<T> cand(alloc);
    cur.resize(n);
    cand.resize(n);
    crd::usize evals = 0;

    const detail::CountingObjective<T> counting(obj, &evals); // counts FD probes too (scipy nfev semantics)
    const FiniteDiffObjective<T> fd(counting, alloc, FdMode::Forward);
    auto local_min = [&](crd::containers::ConstSpan<T> start) -> OptResult<T>
    {
        if (bh.local_minimizer == BasinHoppingLocal::LbfgsFd)
        {
            OptOptions<T> gopts = lopts;
            gopts.func_tol = bh.local_func_tol;
            return minimize_lbfgs<T>(fd, start, gopts, alloc, /*line_search=*/nullptr, bh.local_lbfgs_memory);
        }
        OptResult<T> r = minimize_nelder_mead<T>(obj, start, lopts, alloc, bh.local);
        evals += r.fn_evals;
        return r;
    };

    OptResult<T> r0 = local_min(x0);
    for (crd::usize i = 0; i < n; ++i)
    {
        cur[i] = r0.x[i];
        result.x[i] = r0.x[i];
    }
    T fcur = r0.fx;
    T fbest = fcur;

    T step = bh.step;
    crd::usize accepts = 0;
    for (crd::usize hop = 0; hop < bh.hops; ++hop)
    {
        // scipy's AdaptiveStepsize: every interval hops, grow the step on a high accept rate, shrink on low.
        if (hop > 0 && hop % bh.adapt_interval == 0)
        {
            const T rate = static_cast<T>(accepts) / static_cast<T>(hop);
            if (rate > bh.target_accept)
            {
                step /= bh.adapt_factor;
            }
            else
            {
                step *= bh.adapt_factor;
            }
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            cand[i] = cur[i] + step * (static_cast<T>(2) * static_cast<T>(rng.next_f64()) - static_cast<T>(1));
        }
        OptResult<T> rl = local_min({cand.data(), n});
        const T fl = rl.fx;
        const bool accept = fl <= fcur || static_cast<T>(rng.next_f64()) <
                                              std::exp((fcur - fl) / (bh.temperature > static_cast<T>(1e-300)
                                                                          ? bh.temperature
                                                                          : static_cast<T>(1e-300)));
        if (accept)
        {
            ++accepts;
            for (crd::usize i = 0; i < n; ++i)
            {
                cur[i] = rl.x[i];
            }
            fcur = fl;
            if (fl < fbest)
            {
                fbest = fl;
                for (crd::usize i = 0; i < n; ++i)
                {
                    result.x[i] = rl.x[i];
                }
            }
        }
    }

    result.fx = fbest;
    result.fn_evals = evals;
    result.iterations = bh.hops;
    result.status = OptStatus::Success; // the hop budget runs to completion by design
    result.converged = true;
    return result;
}

template <typename T> struct MultiStartOptions
{
    crd::usize starts = 20;
    crd::u64 seed = 0x111ULL;
    NelderMeadOptions<T> local{};
    crd::usize local_max_iters = 500;
};

template <typename T>
[[nodiscard]] OptResult<T> minimize_multi_start(const Objective<T>& obj, crd::containers::ConstSpan<T> lower,
                                                crd::containers::ConstSpan<T> upper, crd::memory::IAllocator* alloc,
                                                const MultiStartOptions<T>& ms = {})
{
    namespace st = crd::hesap::stats;
    const crd::usize n = obj.n();
    CRD_ASSERT_MSG(lower.size() == n && upper.size() == n, "minimize_multi_start: bounds required");

    OptResult<T> result(alloc);
    result.x.resize(n);
    if (n == 0)
    {
        result.status = OptStatus::Success;
        result.converged = true;
        return result;
    }

    st::PhiloxRng rng(ms.seed, /*stream=*/0U);
    OptOptions<T> lopts;
    lopts.max_iters = ms.local_max_iters;
    crd::containers::Array<T> start(alloc);
    start.resize(n);
    T fbest = std::numeric_limits<T>::infinity();
    crd::usize evals = 0;

    for (crd::usize s = 0; s < ms.starts; ++s)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            start[i] = lower[i] + static_cast<T>(rng.next_f64()) * (upper[i] - lower[i]);
        }
        OptResult<T> rl = minimize_nelder_mead<T>(obj, {start.data(), n}, lopts, alloc, ms.local);
        evals += rl.fn_evals;
        if (rl.fx < fbest)
        {
            fbest = rl.fx;
            for (crd::usize i = 0; i < n; ++i)
            {
                result.x[i] = rl.x[i];
            }
        }
    }

    result.fx = fbest;
    result.fn_evals = evals;
    result.iterations = ms.starts;
    result.status = OptStatus::Success;
    result.converged = true;
    return result;
}

} // namespace crd::hesap::opt
