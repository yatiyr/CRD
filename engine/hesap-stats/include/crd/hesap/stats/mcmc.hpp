#pragma once

// v12-q — MCMC samplers (crd-hesap-stats). Random-walk Metropolis-Hastings · adaptive (Haario) · Gibbs · HMC · NUTS ·
// slice sampling · SMC. Targets are user log-density (+ gradient for HMC/NUTS) functors over a ConstSpan<T> state. The
// counter-based Threefry stream makes a fixed seed bit-reproducible (the determinism moat). Validated by recovering known
// targets (mean/variance + R-hat via mcmc_diagnostics) and by a bit-exact leapfrog trajectory. Gold: Stan · PyMC.

#include <crd/hesap/stats/descriptive.hpp> // Real
#include <crd/hesap/special/erf.hpp>       // ndtri (normal draws via inverse-CDF — deterministic)
#include <crd/hesap/stats/threefry.hpp>    // ThreefryRng (counter-based → determinism moat)

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::stats
{

namespace detail
{
template <Real T> [[nodiscard]] inline T mcmc_uniform(ThreefryRng& rng) noexcept
{
    return static_cast<T>(rng.next_u64() >> 11) * static_cast<T>(1.0 / 9007199254740992.0); // [0,1)
}
template <Real T> [[nodiscard]] inline T mcmc_normal(ThreefryRng& rng)
{
    // open-interval uniform → finite inverse-CDF
    const T u = (static_cast<T>(rng.next_u64() >> 11) + static_cast<T>(0.5)) * static_cast<T>(1.0 / 9007199254740992.0);
    return special::ndtri(u);
}
template <Real T>
[[nodiscard]] inline crd::containers::Array<T> copy_vec(crd::containers::ConstSpan<T> x, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> a(alloc);
    a.resize(x.size());
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        a[i] = x[i];
    }
    return a;
}
} // namespace detail

// Random-walk Metropolis-Hastings. Proposal x' = x + step * N(0, I). Returns the n_samples x d chain (row-major).
template <Real T, typename LogP>
[[nodiscard]] crd::containers::Array<T> metropolis(LogP logp, crd::containers::ConstSpan<T> x0, crd::usize n_samples,
                                                   T step, crd::u64 seed, crd::memory::IAllocator* alloc)
{
    const crd::usize d = x0.size();
    crd::containers::Array<T> chain(alloc);
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> prop(alloc);
    chain.resize(n_samples * d);
    x.resize(d);
    prop.resize(d);
    for (crd::usize i = 0; i < d; ++i)
    {
        x[i] = x0[i];
    }
    ThreefryRng rng(seed, 0);
    T lp = logp(crd::containers::ConstSpan<T>{x.data(), d});
    for (crd::usize s = 0; s < n_samples; ++s)
    {
        for (crd::usize i = 0; i < d; ++i)
        {
            prop[i] = x[i] + step * detail::mcmc_normal<T>(rng);
        }
        const T lp_prop = logp(crd::containers::ConstSpan<T>{prop.data(), d});
        if (detail::mcmc_uniform<T>(rng) < crd::math::exp(lp_prop - lp))
        {
            for (crd::usize i = 0; i < d; ++i)
            {
                x[i] = prop[i];
            }
            lp = lp_prop;
        }
        for (crd::usize i = 0; i < d; ++i)
        {
            chain[s * d + i] = x[i];
        }
    }
    return chain;
}

// Coordinate-wise slice sampler (Neal 2003: stepping-out + shrinkage). w is the initial step width.
template <Real T, typename LogP>
[[nodiscard]] crd::containers::Array<T> slice_sample(LogP logp, crd::containers::ConstSpan<T> x0, crd::usize n_samples,
                                                     T w, crd::u64 seed, crd::memory::IAllocator* alloc)
{
    const crd::usize d = x0.size();
    crd::containers::Array<T> chain(alloc);
    crd::containers::Array<T> x(alloc);
    chain.resize(n_samples * d);
    x.resize(d);
    for (crd::usize i = 0; i < d; ++i)
    {
        x[i] = x0[i];
    }
    ThreefryRng rng(seed, 0);
    for (crd::usize s = 0; s < n_samples; ++s)
    {
        for (crd::usize i = 0; i < d; ++i)
        {
            const auto lp_at = [&](T v) {
                x[i] = v;
                return logp(crd::containers::ConstSpan<T>{x.data(), d});
            };
            const T xi = x[i];
            const T y = lp_at(xi) + crd::math::log(detail::mcmc_uniform<T>(rng)); // log slice level
            T lo = xi - w * detail::mcmc_uniform<T>(rng);
            T hi = lo + w;
            for (int k = 0; k < 50 && lp_at(lo) > y; ++k)
            {
                lo -= w;
            }
            for (int k = 0; k < 50 && lp_at(hi) > y; ++k)
            {
                hi += w;
            }
            for (;;)
            {
                const T xn = lo + detail::mcmc_uniform<T>(rng) * (hi - lo);
                if (lp_at(xn) >= y)
                {
                    x[i] = xn;
                    break;
                }
                if (xn < xi)
                {
                    lo = xn;
                }
                else
                {
                    hi = xn;
                }
            }
        }
        for (crd::usize i = 0; i < d; ++i)
        {
            chain[s * d + i] = x[i];
        }
    }
    return chain;
}

// One leapfrog integration of n_steps from (x, p) for a target with gradient `grad` (gradient of log-density). x and p
// are updated in place. Deterministic — gated bit-for-bit.
template <Real T, typename Grad>
void leapfrog(Grad grad, crd::containers::Span<T> x, crd::containers::Span<T> p, T step, crd::usize n_steps,
              crd::memory::IAllocator* alloc)
{
    const crd::usize d = x.size();
    crd::containers::Array<T> g(alloc);
    g.resize(d);
    grad(crd::containers::ConstSpan<T>{x.data(), d}, crd::containers::Span<T>{g.data(), d});
    for (crd::usize l = 0; l < n_steps; ++l)
    {
        for (crd::usize i = 0; i < d; ++i)
        {
            p[i] += static_cast<T>(0.5) * step * g[i]; // half momentum step (grad of log-density = -dU/dx)
        }
        for (crd::usize i = 0; i < d; ++i)
        {
            x[i] += step * p[i];
        }
        grad(crd::containers::ConstSpan<T>{x.data(), d}, crd::containers::Span<T>{g.data(), d});
        for (crd::usize i = 0; i < d; ++i)
        {
            p[i] += static_cast<T>(0.5) * step * g[i];
        }
    }
}

// Hamiltonian Monte Carlo with a fixed step size and trajectory length. logp + grad over the state.
template <Real T, typename LogP, typename Grad>
[[nodiscard]] crd::containers::Array<T> hmc(LogP logp, Grad grad, crd::containers::ConstSpan<T> x0,
                                            crd::usize n_samples, T step, crd::usize n_leapfrog, crd::u64 seed,
                                            crd::memory::IAllocator* alloc)
{
    const crd::usize d = x0.size();
    crd::containers::Array<T> chain(alloc);
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> xn(alloc);
    crd::containers::Array<T> p(alloc);
    chain.resize(n_samples * d);
    x.resize(d);
    xn.resize(d);
    p.resize(d);
    for (crd::usize i = 0; i < d; ++i)
    {
        x[i] = x0[i];
    }
    ThreefryRng rng(seed, 0);
    for (crd::usize s = 0; s < n_samples; ++s)
    {
        for (crd::usize i = 0; i < d; ++i)
        {
            p[i] = detail::mcmc_normal<T>(rng);
            xn[i] = x[i];
        }
        const T lp = logp(crd::containers::ConstSpan<T>{x.data(), d});
        T k0 = static_cast<T>(0);
        for (crd::usize i = 0; i < d; ++i)
        {
            k0 += p[i] * p[i];
        }
        const T h0 = -lp + static_cast<T>(0.5) * k0;
        leapfrog(grad, crd::containers::Span<T>{xn.data(), d}, crd::containers::Span<T>{p.data(), d}, step, n_leapfrog,
                 alloc);
        const T lpn = logp(crd::containers::ConstSpan<T>{xn.data(), d});
        T kn = static_cast<T>(0);
        for (crd::usize i = 0; i < d; ++i)
        {
            kn += p[i] * p[i];
        }
        const T hn = -lpn + static_cast<T>(0.5) * kn;
        if (detail::mcmc_uniform<T>(rng) < crd::math::exp(h0 - hn))
        {
            for (crd::usize i = 0; i < d; ++i)
            {
                x[i] = xn[i];
            }
        }
        for (crd::usize i = 0; i < d; ++i)
        {
            chain[s * d + i] = x[i];
        }
    }
    return chain;
}

// Adaptive (Haario) Metropolis: the per-component proposal scale adapts to the running variance of the chain
// (sd = sqrt(2.4^2/d * var)) after `adapt_start` iterations; before that, init_step. (Component-wise adaptation; for a
// 1-D target this is exactly Haario's AM.)
template <Real T, typename LogP>
[[nodiscard]] crd::containers::Array<T> adaptive_metropolis(LogP logp, crd::containers::ConstSpan<T> x0,
                                                            crd::usize n_samples, T init_step, crd::usize adapt_start,
                                                            crd::u64 seed, crd::memory::IAllocator* alloc)
{
    const crd::usize d = x0.size();
    crd::containers::Array<T> chain(alloc);
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> prop(alloc);
    crd::containers::Array<T> rmean(alloc);
    crd::containers::Array<T> rm2(alloc);
    chain.resize(n_samples * d);
    x.resize(d);
    prop.resize(d);
    rmean.resize(d);
    rm2.resize(d);
    for (crd::usize i = 0; i < d; ++i)
    {
        x[i] = x0[i];
        rmean[i] = x0[i];
        rm2[i] = static_cast<T>(0);
    }
    ThreefryRng rng(seed, 0);
    crd::usize count = 1;
    T lp = logp(crd::containers::ConstSpan<T>{x.data(), d});
    const T sd_scale = static_cast<T>(2.4) * static_cast<T>(2.4) / static_cast<T>(d);
    for (crd::usize s = 0; s < n_samples; ++s)
    {
        for (crd::usize i = 0; i < d; ++i)
        {
            T sd = init_step;
            if (s >= adapt_start && count > 1)
            {
                sd = crd::math::sqrt(sd_scale * rm2[i] / static_cast<T>(count - 1)) + static_cast<T>(1e-8);
            }
            prop[i] = x[i] + sd * detail::mcmc_normal<T>(rng);
        }
        const T lpp = logp(crd::containers::ConstSpan<T>{prop.data(), d});
        if (detail::mcmc_uniform<T>(rng) < crd::math::exp(lpp - lp))
        {
            for (crd::usize i = 0; i < d; ++i)
            {
                x[i] = prop[i];
            }
            lp = lpp;
        }
        ++count;
        for (crd::usize i = 0; i < d; ++i)
        {
            const T del = x[i] - rmean[i];
            rmean[i] += del / static_cast<T>(count);
            rm2[i] += del * (x[i] - rmean[i]);
            chain[s * d + i] = x[i];
        }
    }
    return chain;
}

template <Real T> struct NutsTree
{
    crd::containers::Array<T> x_minus;
    crd::containers::Array<T> p_minus;
    crd::containers::Array<T> x_plus;
    crd::containers::Array<T> p_plus;
    crd::containers::Array<T> x_prime;
    crd::usize n_prime;
    int s_prime;
    T alpha;            // sum of Metropolis accept probabilities over the tree leaves (for dual averaging)
    crd::usize n_alpha; // number of leaves
};

// NUTS BuildTree (Hoffman-Gelman 2014, Algorithm 6, slice-based + acceptance stats for dual averaging). Recursively
// doubles the trajectory in direction v. joint0 is the initial Hamiltonian (for the acceptance statistic).
template <Real T, typename LogP, typename Grad>
[[nodiscard]] NutsTree<T> build_tree(LogP logp, Grad grad, crd::containers::ConstSpan<T> x,
                                     crd::containers::ConstSpan<T> p, T log_u, T joint0, int v, int j, T eps,
                                     ThreefryRng& rng, crd::memory::IAllocator* alloc)
{
    const crd::usize d = x.size();
    if (j == 0)
    {
        auto xx = detail::copy_vec(x, alloc);
        auto pp = detail::copy_vec(p, alloc);
        leapfrog(grad, crd::containers::Span<T>{xx.data(), d}, crd::containers::Span<T>{pp.data(), d},
                 static_cast<T>(v) * eps, 1, alloc);
        const T lp = logp(crd::containers::ConstSpan<T>{xx.data(), d});
        T kk = static_cast<T>(0);
        for (crd::usize i = 0; i < d; ++i)
        {
            kk += pp[i] * pp[i];
        }
        const T joint = lp - static_cast<T>(0.5) * kk;
        NutsTree<T> t;
        t.x_minus = detail::copy_vec(crd::containers::ConstSpan<T>{xx.data(), d}, alloc);
        t.p_minus = detail::copy_vec(crd::containers::ConstSpan<T>{pp.data(), d}, alloc);
        t.x_plus = detail::copy_vec(crd::containers::ConstSpan<T>{xx.data(), d}, alloc);
        t.p_plus = static_cast<crd::containers::Array<T>&&>(pp);
        t.x_prime = static_cast<crd::containers::Array<T>&&>(xx);
        t.n_prime = (log_u <= joint) ? 1U : 0U;
        t.s_prime = (log_u < joint + static_cast<T>(1000)) ? 1 : 0;
        const T da = joint - joint0;
        t.alpha = (da > static_cast<T>(0)) ? static_cast<T>(1) : crd::math::exp(da);
        t.n_alpha = 1;
        return t;
    }
    NutsTree<T> t = build_tree(logp, grad, x, p, log_u, joint0, v, j - 1, eps, rng, alloc);
    if (t.s_prime == 1)
    {
        NutsTree<T> t2;
        if (v == -1)
        {
            t2 = build_tree(logp, grad, crd::containers::ConstSpan<T>{t.x_minus.data(), d},
                            crd::containers::ConstSpan<T>{t.p_minus.data(), d}, log_u, joint0, v, j - 1, eps, rng, alloc);
            t.x_minus = static_cast<crd::containers::Array<T>&&>(t2.x_minus);
            t.p_minus = static_cast<crd::containers::Array<T>&&>(t2.p_minus);
        }
        else
        {
            t2 = build_tree(logp, grad, crd::containers::ConstSpan<T>{t.x_plus.data(), d},
                            crd::containers::ConstSpan<T>{t.p_plus.data(), d}, log_u, joint0, v, j - 1, eps, rng, alloc);
            t.x_plus = static_cast<crd::containers::Array<T>&&>(t2.x_plus);
            t.p_plus = static_cast<crd::containers::Array<T>&&>(t2.p_plus);
        }
        const crd::usize ntot = t.n_prime + t2.n_prime;
        if (ntot > 0 &&
            detail::mcmc_uniform<T>(rng) < static_cast<T>(t2.n_prime) / static_cast<T>(ntot))
        {
            t.x_prime = static_cast<crd::containers::Array<T>&&>(t2.x_prime);
        }
        T dot1 = static_cast<T>(0);
        T dot2 = static_cast<T>(0);
        for (crd::usize i = 0; i < d; ++i)
        {
            const T dx = t.x_plus[i] - t.x_minus[i];
            dot1 += dx * t.p_minus[i];
            dot2 += dx * t.p_plus[i];
        }
        t.s_prime = t2.s_prime * ((dot1 >= static_cast<T>(0)) ? 1 : 0) * ((dot2 >= static_cast<T>(0)) ? 1 : 0);
        t.n_prime = ntot;
        t.alpha += t2.alpha;
        t.n_alpha += t2.n_alpha;
    }
    return t;
}

// No-U-Turn Sampler (fixed step size). logp + grad over the state; the trajectory length is chosen automatically by the
// U-turn criterion. Stan's core algorithm.
template <Real T, typename LogP, typename Grad>
[[nodiscard]] crd::containers::Array<T> nuts(LogP logp, Grad grad, crd::containers::ConstSpan<T> x0,
                                             crd::usize n_samples, T eps, crd::u64 seed, crd::memory::IAllocator* alloc,
                                             int max_depth = 10)
{
    const crd::usize d = x0.size();
    crd::containers::Array<T> chain(alloc);
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> p(alloc);
    crd::containers::Array<T> xm(alloc);
    crd::containers::Array<T> pm(alloc);
    crd::containers::Array<T> xp(alloc);
    crd::containers::Array<T> pp(alloc);
    crd::containers::Array<T> xs(alloc);
    chain.resize(n_samples * d);
    x.resize(d);
    p.resize(d);
    xm.resize(d);
    pm.resize(d);
    xp.resize(d);
    pp.resize(d);
    xs.resize(d);
    for (crd::usize i = 0; i < d; ++i)
    {
        x[i] = x0[i];
    }
    ThreefryRng rng(seed, 0);
    for (crd::usize s = 0; s < n_samples; ++s)
    {
        T k0 = static_cast<T>(0);
        for (crd::usize i = 0; i < d; ++i)
        {
            p[i] = detail::mcmc_normal<T>(rng);
            k0 += p[i] * p[i];
        }
        const T joint0 = logp(crd::containers::ConstSpan<T>{x.data(), d}) - static_cast<T>(0.5) * k0;
        const T log_u = joint0 + crd::math::log(detail::mcmc_uniform<T>(rng));
        for (crd::usize i = 0; i < d; ++i)
        {
            xm[i] = x[i];
            pm[i] = p[i];
            xp[i] = x[i];
            pp[i] = p[i];
            xs[i] = x[i];
        }
        crd::usize n = 1;
        int sflag = 1;
        int j = 0;
        while (sflag == 1 && j < max_depth)
        {
            const int v = (detail::mcmc_uniform<T>(rng) < static_cast<T>(0.5)) ? -1 : 1;
            NutsTree<T> tree;
            if (v == -1)
            {
                tree = build_tree(logp, grad, crd::containers::ConstSpan<T>{xm.data(), d},
                                  crd::containers::ConstSpan<T>{pm.data(), d}, log_u, joint0, v, j, eps, rng, alloc);
                for (crd::usize i = 0; i < d; ++i)
                {
                    xm[i] = tree.x_minus[i];
                    pm[i] = tree.p_minus[i];
                }
            }
            else
            {
                tree = build_tree(logp, grad, crd::containers::ConstSpan<T>{xp.data(), d},
                                  crd::containers::ConstSpan<T>{pp.data(), d}, log_u, joint0, v, j, eps, rng, alloc);
                for (crd::usize i = 0; i < d; ++i)
                {
                    xp[i] = tree.x_plus[i];
                    pp[i] = tree.p_plus[i];
                }
            }
            if (tree.s_prime == 1 && detail::mcmc_uniform<T>(rng) < static_cast<T>(tree.n_prime) / static_cast<T>(n))
            {
                for (crd::usize i = 0; i < d; ++i)
                {
                    xs[i] = tree.x_prime[i];
                }
            }
            n += tree.n_prime;
            T dot1 = static_cast<T>(0);
            T dot2 = static_cast<T>(0);
            for (crd::usize i = 0; i < d; ++i)
            {
                const T dx = xp[i] - xm[i];
                dot1 += dx * pm[i];
                dot2 += dx * pp[i];
            }
            sflag = tree.s_prime * ((dot1 >= static_cast<T>(0)) ? 1 : 0) * ((dot2 >= static_cast<T>(0)) ? 1 : 0);
            ++j;
        }
        for (crd::usize i = 0; i < d; ++i)
        {
            x[i] = xs[i];
            chain[s * d + i] = x[i];
        }
    }
    return chain;
}

// NUTS with dual-averaging step-size adaptation (Hoffman-Gelman Algorithm 6 — the full Stan algorithm). The step size is
// tuned over `n_warmup` iterations to a target acceptance of 0.8, then frozen for `n_samples` draws.
template <Real T, typename LogP, typename Grad>
[[nodiscard]] crd::containers::Array<T> nuts_adapt(LogP logp, Grad grad, crd::containers::ConstSpan<T> x0,
                                                   crd::usize n_warmup, crd::usize n_samples, T eps0, crd::u64 seed,
                                                   crd::memory::IAllocator* alloc, int max_depth = 10)
{
    const crd::usize d = x0.size();
    crd::containers::Array<T> chain(alloc);
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> p(alloc);
    crd::containers::Array<T> xm(alloc);
    crd::containers::Array<T> pm(alloc);
    crd::containers::Array<T> xp(alloc);
    crd::containers::Array<T> pp(alloc);
    crd::containers::Array<T> xs(alloc);
    chain.resize(n_samples * d);
    x.resize(d);
    p.resize(d);
    xm.resize(d);
    pm.resize(d);
    xp.resize(d);
    pp.resize(d);
    xs.resize(d);
    for (crd::usize i = 0; i < d; ++i)
    {
        x[i] = x0[i];
    }
    ThreefryRng rng(seed, 0);
    const T delta = static_cast<T>(0.8);
    const T gamma = static_cast<T>(0.05);
    const T kappa = static_cast<T>(0.75);
    const T t0 = static_cast<T>(10);
    const T mu = crd::math::log(static_cast<T>(10) * eps0);
    T log_eps = crd::math::log(eps0);
    T log_eps_bar = static_cast<T>(0);
    T h_bar = static_cast<T>(0);
    T eps = eps0;
    const crd::usize total = n_warmup + n_samples;
    for (crd::usize m = 1; m <= total; ++m)
    {
        T k0 = static_cast<T>(0);
        for (crd::usize i = 0; i < d; ++i)
        {
            p[i] = detail::mcmc_normal<T>(rng);
            k0 += p[i] * p[i];
        }
        const T joint0 = logp(crd::containers::ConstSpan<T>{x.data(), d}) - static_cast<T>(0.5) * k0;
        const T log_u = joint0 + crd::math::log(detail::mcmc_uniform<T>(rng));
        for (crd::usize i = 0; i < d; ++i)
        {
            xm[i] = x[i];
            pm[i] = p[i];
            xp[i] = x[i];
            pp[i] = p[i];
            xs[i] = x[i];
        }
        crd::usize n = 1;
        int sflag = 1;
        int j = 0;
        T alpha = static_cast<T>(0);
        crd::usize n_alpha = 1;
        while (sflag == 1 && j < max_depth)
        {
            const int v = (detail::mcmc_uniform<T>(rng) < static_cast<T>(0.5)) ? -1 : 1;
            NutsTree<T> tree;
            if (v == -1)
            {
                tree = build_tree(logp, grad, crd::containers::ConstSpan<T>{xm.data(), d},
                                  crd::containers::ConstSpan<T>{pm.data(), d}, log_u, joint0, v, j, eps, rng, alloc);
                for (crd::usize i = 0; i < d; ++i)
                {
                    xm[i] = tree.x_minus[i];
                    pm[i] = tree.p_minus[i];
                }
            }
            else
            {
                tree = build_tree(logp, grad, crd::containers::ConstSpan<T>{xp.data(), d},
                                  crd::containers::ConstSpan<T>{pp.data(), d}, log_u, joint0, v, j, eps, rng, alloc);
                for (crd::usize i = 0; i < d; ++i)
                {
                    xp[i] = tree.x_plus[i];
                    pp[i] = tree.p_plus[i];
                }
            }
            if (tree.s_prime == 1 && detail::mcmc_uniform<T>(rng) < static_cast<T>(tree.n_prime) / static_cast<T>(n))
            {
                for (crd::usize i = 0; i < d; ++i)
                {
                    xs[i] = tree.x_prime[i];
                }
            }
            n += tree.n_prime;
            alpha = tree.alpha;
            n_alpha = tree.n_alpha;
            T dot1 = static_cast<T>(0);
            T dot2 = static_cast<T>(0);
            for (crd::usize i = 0; i < d; ++i)
            {
                const T dx = xp[i] - xm[i];
                dot1 += dx * pm[i];
                dot2 += dx * pp[i];
            }
            sflag = tree.s_prime * ((dot1 >= static_cast<T>(0)) ? 1 : 0) * ((dot2 >= static_cast<T>(0)) ? 1 : 0);
            ++j;
        }
        for (crd::usize i = 0; i < d; ++i)
        {
            x[i] = xs[i];
        }
        if (m <= n_warmup)
        {
            const T mt = static_cast<T>(m);
            const T accept = alpha / static_cast<T>(n_alpha);
            h_bar = (static_cast<T>(1) - static_cast<T>(1) / (mt + t0)) * h_bar +
                    (static_cast<T>(1) / (mt + t0)) * (delta - accept);
            log_eps = mu - crd::math::sqrt(mt) / gamma * h_bar;
            const T eta = crd::math::pow(mt, -kappa);
            log_eps_bar = eta * log_eps + (static_cast<T>(1) - eta) * log_eps_bar;
            eps = crd::math::exp(log_eps);
        }
        else
        {
            eps = crd::math::exp(log_eps_bar);
            const crd::usize si = m - n_warmup - 1;
            for (crd::usize i = 0; i < d; ++i)
            {
                chain[si * d + i] = x[i];
            }
        }
    }
    return chain;
}

// Gibbs sampler: updates each coordinate from its full conditional. cond(i, x, rng) draws x_i | x_{-i}.
template <Real T, typename Cond>
[[nodiscard]] crd::containers::Array<T> gibbs(Cond cond, crd::containers::ConstSpan<T> x0, crd::usize n_samples,
                                              crd::u64 seed, crd::memory::IAllocator* alloc)
{
    const crd::usize d = x0.size();
    crd::containers::Array<T> chain(alloc);
    crd::containers::Array<T> x(alloc);
    chain.resize(n_samples * d);
    x.resize(d);
    for (crd::usize i = 0; i < d; ++i)
    {
        x[i] = x0[i];
    }
    ThreefryRng rng(seed, 0);
    for (crd::usize s = 0; s < n_samples; ++s)
    {
        for (crd::usize i = 0; i < d; ++i)
        {
            x[i] = cond(i, crd::containers::ConstSpan<T>{x.data(), d}, rng);
        }
        for (crd::usize i = 0; i < d; ++i)
        {
            chain[s * d + i] = x[i];
        }
    }
    return chain;
}

template <Real T> struct SmcResult
{
    crd::containers::Array<T> particles; // n_particles x dim row-major
    crd::containers::Array<T> weights;   // normalized
    crd::usize n_particles;
    crd::usize dim;
};

// Sequential Monte Carlo sampler with geometric tempering from prior to posterior (pi_beta ∝ prior * lik^beta). Each
// temperature: reweight by exp(dbeta*loglik), systematic-resample when ESS < N/2, then MCMC-move each particle. prior_
// sample(out, rng) draws from the prior. R/Stan particle-filter style; gated by recovering a known Gaussian posterior.
template <Real T, typename LogPrior, typename LogLik, typename PriorSample>
[[nodiscard]] SmcResult<T> smc(LogPrior log_prior, LogLik log_lik, PriorSample prior_sample, crd::usize n_particles,
                               crd::usize d, crd::usize n_temps, crd::usize n_mcmc, T mcmc_step, crd::u64 seed,
                               crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> part(alloc);
    crd::containers::Array<T> w(alloc);
    crd::containers::Array<T> llik(alloc);
    crd::containers::Array<T> prop(alloc);
    part.resize(n_particles * d);
    w.resize(n_particles);
    llik.resize(n_particles);
    prop.resize(d);
    ThreefryRng rng(seed, 0);
    for (crd::usize i = 0; i < n_particles; ++i)
    {
        prior_sample(crd::containers::Span<T>{part.data() + i * d, d}, rng);
        w[i] = static_cast<T>(1) / static_cast<T>(n_particles);
        llik[i] = log_lik(crd::containers::ConstSpan<T>{part.data() + i * d, d});
    }
    T beta_prev = static_cast<T>(0);
    crd::containers::Array<T> npart(alloc);
    crd::containers::Array<T> nllik(alloc);
    npart.resize(n_particles * d);
    nllik.resize(n_particles);
    for (crd::usize temp = 1; temp <= n_temps; ++temp)
    {
        const T beta = static_cast<T>(temp) / static_cast<T>(n_temps);
        const T dbeta = beta - beta_prev;
        T wsum = static_cast<T>(0);
        for (crd::usize i = 0; i < n_particles; ++i)
        {
            w[i] *= crd::math::exp(dbeta * llik[i]);
            wsum += w[i];
        }
        for (crd::usize i = 0; i < n_particles; ++i)
        {
            w[i] /= wsum;
        }
        T ess_inv = static_cast<T>(0);
        for (crd::usize i = 0; i < n_particles; ++i)
        {
            ess_inv += w[i] * w[i];
        }
        if (static_cast<T>(1) / ess_inv < static_cast<T>(n_particles) / static_cast<T>(2)) // systematic resample
        {
            const T u0 = detail::mcmc_uniform<T>(rng) / static_cast<T>(n_particles);
            T c = w[0];
            crd::usize src = 0;
            for (crd::usize k = 0; k < n_particles; ++k)
            {
                const T u = u0 + static_cast<T>(k) / static_cast<T>(n_particles);
                while (u > c && src + 1 < n_particles)
                {
                    ++src;
                    c += w[src];
                }
                for (crd::usize j = 0; j < d; ++j)
                {
                    npart[k * d + j] = part[src * d + j];
                }
                nllik[k] = llik[src];
            }
            for (crd::usize i = 0; i < n_particles * d; ++i)
            {
                part[i] = npart[i];
            }
            for (crd::usize i = 0; i < n_particles; ++i)
            {
                llik[i] = nllik[i];
                w[i] = static_cast<T>(1) / static_cast<T>(n_particles);
            }
        }
        for (crd::usize i = 0; i < n_particles; ++i) // MCMC move targeting pi_beta
        {
            for (crd::usize mv = 0; mv < n_mcmc; ++mv)
            {
                const T cur = log_prior(crd::containers::ConstSpan<T>{part.data() + i * d, d}) + beta * llik[i];
                for (crd::usize j = 0; j < d; ++j)
                {
                    prop[j] = part[i * d + j] + mcmc_step * detail::mcmc_normal<T>(rng);
                }
                const T pll = log_lik(crd::containers::ConstSpan<T>{prop.data(), d});
                const T pr = log_prior(crd::containers::ConstSpan<T>{prop.data(), d}) + beta * pll;
                if (detail::mcmc_uniform<T>(rng) < crd::math::exp(pr - cur))
                {
                    for (crd::usize j = 0; j < d; ++j)
                    {
                        part[i * d + j] = prop[j];
                    }
                    llik[i] = pll;
                }
            }
        }
        beta_prev = beta;
    }
    return {static_cast<crd::containers::Array<T>&&>(part), static_cast<crd::containers::Array<T>&&>(w), n_particles, d};
}

} // namespace crd::hesap::stats
