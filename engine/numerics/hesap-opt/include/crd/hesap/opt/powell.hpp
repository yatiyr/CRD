#pragma once

// powell.hpp — Phase 3.1.6 v7-p-1: POWELL'S CONJUGATE-DIRECTION METHOD (1964) — derivative-free minimization
// by successive 1-D line minimizations, with Powell's direction-replacement rule building conjugacy:
//   • each iteration line-minimizes along every direction in the set (initially the coordinate axes);
//   • the extrapolation test (the f_E quadratic-decrease criterion, NR/scipy form) decides whether the sweep's
//     net displacement replaces the direction of largest single decrease — on an exact quadratic the set turns
//     mutually conjugate and the method finitely terminates;
//   • the 1-D engine is a FAITHFUL BRENT minimizer (golden section + safeguarded successive parabolic
//     interpolation, Brent 1973) over a golden-ratio expansion bracket — the same pair scipy's 'Powell' drives.
// Value-only — no gradients anywhere. [gold: scipy 'Powell' — the v7-z scoreboard]. ADR-0090.
//
// DETERMINISM: RNG-free, serial, fixed sweep order ⇒ bit-identical runs by construction.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::opt
{

template <typename T> struct PowellOptions
{
    T xtol = static_cast<T>(1e-8);  // outer x tolerance; the inner Brent runs at 100·xtol — scipy's exact
                                    // coupling (_linesearch_powell tol = xtol*100). scipy default xtol=1e-4.
    T ftol = static_cast<T>(1e-10); // per-iteration relative f-decrease termination (scipy semantics)
};

namespace detail
{

// Golden-ratio downhill bracket expansion (NR `mnbrak`): returns (ax, bx, cx) with f(bx) < f(ax), f(cx).
template <typename T, typename F> inline void brent_bracket(const F& f, T& ax, T& bx, T& cx, T& fa, T& fb, T& fc)
{
    constexpr T gold = static_cast<T>(1.618034);
    constexpr T glimit = static_cast<T>(100);
    constexpr T tiny = static_cast<T>(1e-20);
    fa = f(ax);
    fb = f(bx);
    if (fb > fa)
    {
        T t = ax;
        ax = bx;
        bx = t;
        t = fa;
        fa = fb;
        fb = t;
    }
    cx = bx + gold * (bx - ax);
    fc = f(cx);
    while (fb > fc)
    {
        const T r = (bx - ax) * (fb - fc);
        const T q = (bx - cx) * (fb - fa);
        T denom = q - r;
        const T mag = crd::math::fabs(denom) > tiny ? crd::math::fabs(denom) : tiny;
        denom = denom >= static_cast<T>(0) ? mag : -mag;
        T u = bx - ((bx - cx) * q - (bx - ax) * r) / (static_cast<T>(2) * denom);
        const T ulim = bx + glimit * (cx - bx);
        T fu;
        if ((bx - u) * (u - cx) > static_cast<T>(0)) // u between b and c
        {
            fu = f(u);
            if (fu < fc)
            {
                ax = bx;
                bx = u;
                fa = fb;
                fb = fu;
                return;
            }
            if (fu > fb)
            {
                cx = u;
                fc = fu;
                return;
            }
            u = cx + gold * (cx - bx);
            fu = f(u);
        }
        else if ((cx - u) * (u - ulim) > static_cast<T>(0)) // u between c and the limit
        {
            fu = f(u);
            if (fu < fc)
            {
                bx = cx;
                cx = u;
                u = u + gold * (u - cx);
                fb = fc;
                fc = fu;
                fu = f(u);
            }
        }
        else if ((u - ulim) * (ulim - cx) >= static_cast<T>(0)) // clamp to the limit
        {
            u = ulim;
            fu = f(u);
        }
        else // reject the parabolic u; plain golden step
        {
            u = cx + gold * (cx - bx);
            fu = f(u);
        }
        ax = bx;
        bx = cx;
        cx = u;
        fa = fb;
        fb = fc;
        fc = fu;
    }
}

// Brent's 1-D minimizer (1973; NR `brent`): golden section + safeguarded parabolic interpolation on a bracket.
// Returns the minimizer; `fmin_out` gets f at it.
template <typename T, typename F>
[[nodiscard]] inline T brent_min(const F& f, T ax, T bx, T cx, T fbx, T tol, T& fmin_out)
{
    constexpr int itmax = 100;
    constexpr T cgold = static_cast<T>(0.381966);
    constexpr T zeps = static_cast<T>(1e-18);
    T a = ax < cx ? ax : cx;
    T b = ax > cx ? ax : cx;
    T x = bx;
    T w = bx;
    T v = bx;
    T fx = fbx;
    T fw = fx;
    T fv = fx;
    T d = static_cast<T>(0);
    T e = static_cast<T>(0);
    for (int iter = 0; iter < itmax; ++iter)
    {
        const T xm = static_cast<T>(0.5) * (a + b);
        const T tol1 = tol * crd::math::fabs(x) + zeps;
        const T tol2 = static_cast<T>(2) * tol1;
        if (crd::math::fabs(x - xm) <= tol2 - static_cast<T>(0.5) * (b - a))
        {
            break;
        }
        bool golden = true;
        if (crd::math::fabs(e) > tol1) // try the parabolic fit through (x, w, v)
        {
            const T r = (x - w) * (fx - fv);
            T q = (x - v) * (fx - fw);
            T p = (x - v) * q - (x - w) * r;
            q = static_cast<T>(2) * (q - r);
            if (q > static_cast<T>(0))
            {
                p = -p;
            }
            q = crd::math::fabs(q);
            const T etemp = e;
            e = d;
            if (!(crd::math::fabs(p) >= crd::math::fabs(static_cast<T>(0.5) * q * etemp) || p <= q * (a - x) || p >= q * (b - x)))
            {
                d = p / q; // parabolic step accepted
                const T u = x + d;
                if (u - a < tol2 || b - u < tol2)
                {
                    d = xm - x >= static_cast<T>(0) ? tol1 : -tol1;
                }
                golden = false;
            }
        }
        if (golden)
        {
            e = x >= xm ? a - x : b - x;
            d = cgold * e;
        }
        const T u = crd::math::fabs(d) >= tol1 ? x + d : x + (d >= static_cast<T>(0) ? tol1 : -tol1);
        const T fu = f(u);
        if (fu <= fx)
        {
            if (u >= x)
            {
                a = x;
            }
            else
            {
                b = x;
            }
            v = w;
            w = x;
            x = u;
            fv = fw;
            fw = fx;
            fx = fu;
        }
        else
        {
            if (u < x)
            {
                a = u;
            }
            else
            {
                b = u;
            }
            if (fu <= fw || w == x)
            {
                v = w;
                w = u;
                fv = fw;
                fw = fu;
            }
            else if (fu <= fv || v == x || v == w)
            {
                v = u;
                fv = fu;
            }
        }
    }
    fmin_out = fx;
    return x;
}

} // namespace detail

template <typename T>
[[nodiscard]] OptResult<T> minimize_powell(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                           const OptOptions<T>& opts, crd::memory::IAllocator* alloc,
                                           const PowellOptions<T>& po = {})
{
    const crd::usize n = obj.n();
    CRD_ASSERT_MSG(x0.size() == n, "minimize_powell: x0 size mismatch");

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

    crd::containers::Array<T> direc(alloc); // n × n direction set, row-major; rows are directions
    crd::containers::Array<T> xiter(alloc); // sweep start
    crd::containers::Array<T> xext(alloc);  // extrapolated point
    crd::containers::Array<T> dnew(alloc);  // net sweep displacement
    direc.resize(n * n);
    xiter.resize(n);
    xext.resize(n);
    dnew.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            direc[i * n + j] = i == j ? static_cast<T>(1) : static_cast<T>(0);
        }
    }

    T* x = result.x.data();
    auto feval = [&](const T* p) -> T
    {
        ++result.fn_evals;
        return obj.value({p, n});
    };
    // Line-minimize f along direction `d` from the CURRENT x (updates x and fx).
    crd::containers::Array<T> ls_buf(alloc);
    ls_buf.resize(n);
    auto line_min = [&](const T* d, T& fx) -> T // returns the f-decrease
    {
        auto f1d = [&](T alpha) -> T
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                ls_buf[j] = x[j] + alpha * d[j];
            }
            return feval(ls_buf.data());
        };
        T ax = static_cast<T>(0);
        T bx = static_cast<T>(1);
        T cx;
        T fa;
        T fb;
        T fc;
        detail::brent_bracket<T>(f1d, ax, bx, cx, fa, fb, fc);
        T fmin;
        const T astar = detail::brent_min<T>(f1d, ax, bx, cx, fb, po.xtol * static_cast<T>(100), fmin);
        const T dec = fx - fmin;
        if (dec > static_cast<T>(0))
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                x[j] += astar * d[j];
            }
            fx = fmin;
            return dec;
        }
        return static_cast<T>(0);
    };

    T fx = feval(x);
    OptStatus status = OptStatus::MaxIterations;
    crd::usize it = 0;
    for (; it < opts.max_iters; ++it)
    {
        if (opts.record_history)
        {
            result.history.push_back(fx);
        }
        const T fx_start = fx;
        for (crd::usize j = 0; j < n; ++j)
        {
            xiter[j] = x[j];
        }
        // Sweep every direction; track the largest single decrease.
        T del = static_cast<T>(0);
        crd::usize ibig = 0;
        for (crd::usize i = 0; i < n; ++i)
        {
            const T dec = line_min(direc.data() + i * n, fx);
            if (dec > del)
            {
                del = dec;
                ibig = i;
            }
        }
        // Termination (scipy/NR relative-decrease form).
        if (static_cast<T>(2) * (fx_start - fx) <=
            po.ftol * (crd::math::fabs(fx_start) + crd::math::fabs(fx)) + static_cast<T>(1e-25))
        {
            status = OptStatus::Success;
            ++it;
            break;
        }
        // Powell's extrapolation test: keep the direction set unless the net displacement earns its place.
        for (crd::usize j = 0; j < n; ++j)
        {
            xext[j] = static_cast<T>(2) * x[j] - xiter[j];
            dnew[j] = x[j] - xiter[j];
        }
        const T fext = feval(xext.data());
        if (fext < fx_start)
        {
            const T a1 = fx_start - fx - del;
            const T a2 = fx_start - fext;
            const T t = static_cast<T>(2) * (fx_start - static_cast<T>(2) * fx + fext) * a1 * a1 - del * a2 * a2;
            if (t < static_cast<T>(0))
            {
                (void)line_min(dnew.data(), fx);
                for (crd::usize j = 0; j < n; ++j) // direc[ibig] <- direc[n-1]; direc[n-1] <- dnew
                {
                    direc[ibig * n + j] = direc[(n - 1) * n + j];
                    direc[(n - 1) * n + j] = dnew[j];
                }
            }
        }
    }

    result.fx = fx;
    result.status = status;
    result.converged = status == OptStatus::Success;
    result.iterations = it;
    return result;
}

} // namespace crd::hesap::opt
