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

// Downhill bracket search: a golden-ratio downhill expansion with parabolic extrapolation, the method of the mnbrak
// routine of Numerical Recipes (Press et al.); the minimiser it feeds, brent_min, is Brent 1973. Starting from two
// abscissae it walks downhill, growing each step by the golden ratio and letting the vertex of the parabola through
// the last three points propose a longer jump (capped at 100 times the current span). On return
// f(mid) < f(lo) and f(mid) < f(hi), so a minimum is enclosed.
template <typename T, typename F> inline void brent_bracket(const F& f, T& lo, T& mid, T& hi, T& fLo, T& fMid, T& fHi)
{
    constexpr T phi = static_cast<T>(1.618034);
    constexpr T maxGrowth = static_cast<T>(100);
    constexpr T denomFloor = static_cast<T>(1e-20);
    // The point one golden step beyond `to`, walking away from `from`.
    const auto golden_past = [](T from, T to) -> T
    {
        return to + phi * (to - from);
    };
    // Move the window of three one place along: `next` becomes the new right end.
    const auto slide = [&](T next, T fNext)
    {
        lo = mid;
        fLo = fMid;
        mid = hi;
        fMid = fHi;
        hi = next;
        fHi = fNext;
    };
    const auto flip = [](T& p, T& q)
    {
        const T keep = p;
        p = q;
        q = keep;
    };

    fLo = f(lo);
    fMid = f(mid);
    if (fMid > fLo)
    {
        flip(lo, mid);
        flip(fLo, fMid);
    }
    hi = golden_past(lo, mid);
    fHi = f(hi);
    while (fMid > fHi)
    {
        // Vertex of the parabola through (lo, mid, hi); its denominator is kept away from zero.
        const T leftBase = mid - lo;
        const T rightBase = mid - hi;
        const T leftTerm = leftBase * (fMid - fHi);
        const T rightTerm = rightBase * (fMid - fLo);
        const T spread = rightTerm - leftTerm;
        const T guard = crd::math::fabs(spread) > denomFloor ? crd::math::fabs(spread) : denomFloor;
        const T twiceDenom = static_cast<T>(2) * (spread >= static_cast<T>(0) ? guard : -guard);
        const T vertex = mid - (rightBase * rightTerm - leftBase * leftTerm) / twiceDenom;
        const T reach = mid + maxGrowth * (hi - mid);
        const T plainStep = golden_past(mid, hi);

        if ((mid - vertex) * (vertex - hi) > static_cast<T>(0))
        {
            // Vertex inside (mid, hi): either it closes the bracket, or the walk continues one golden step past hi.
            const T fVertex = f(vertex);
            if (fVertex < fHi)
            {
                lo = mid;
                fLo = fMid;
                mid = vertex;
                fMid = fVertex;
                return;
            }
            if (fVertex > fMid)
            {
                hi = vertex;
                fHi = fVertex;
                return;
            }
            slide(plainStep, f(plainStep));
        }
        else if ((hi - vertex) * (vertex - reach) > static_cast<T>(0))
        {
            // Vertex beyond hi but within reach: take it, and if it still descends, probe one golden step further.
            // After the slide the window ends (previous hi, vertex), so that step runs on from the vertex along the
            // direction of the walk and the bracket keeps opening while f is still falling.
            const T fVertex = f(vertex);
            const bool descends = fVertex < fHi;
            slide(vertex, fVertex);
            if (descends)
            {
                const T again = golden_past(mid, hi);
                slide(again, f(again));
            }
        }
        else if ((vertex - reach) * (reach - hi) >= static_cast<T>(0))
        {
            slide(reach, f(reach));
        }
        else
        {
            slide(plainStep, f(plainStep));
        }
    }
}

// Brent's 1-D minimizer (Brent 1973, ch. 5 "localmin"): golden-section steps safeguarded by successive parabolic
// interpolation through the three best points, on a bracket. Returns the minimizer; `fmin_out` gets f at it.
template <typename T, typename F>
[[nodiscard]] inline T brent_min(const F& f, T xLeft, T xStart, T xRight, T fStart, T tol, T& fmin_out)
{
    constexpr int maxIter = 100;
    constexpr T goldenFraction = static_cast<T>(0.381966);
    constexpr T absFloor = static_cast<T>(1e-18);
    T lo = xLeft < xRight ? xLeft : xRight;
    T hi = xLeft > xRight ? xLeft : xRight;
    T best = xStart;
    T second = xStart;
    T third = xStart;
    T fBest = fStart;
    T fSecond = fStart;
    T fThird = fStart;
    T step = static_cast<T>(0);
    T lastStep = static_cast<T>(0);
    for (int iter = 0; iter < maxIter; ++iter)
    {
        const T centre = static_cast<T>(0.5) * (lo + hi);
        const T unit = tol * crd::math::fabs(best) + absFloor;
        const T unit2 = static_cast<T>(2) * unit;
        if (crd::math::fabs(best - centre) <= unit2 - static_cast<T>(0.5) * (hi - lo))
        {
            break;
        }
        const auto toward = [unit](T direction) -> T
        {
            return direction >= static_cast<T>(0) ? unit : -unit;
        };
        // Parabola through (best, second, third): accept its vertex only if it lies inside the bracket and moves
        // less than half the step before last. On acceptance `lastStep` takes the previous step and `step` the new.
        const auto try_parabola = [&]() -> bool
        {
            const T crossSecond = (best - second) * (fBest - fThird);
            const T crossThird = (best - third) * (fBest - fSecond);
            T numer = (best - third) * crossThird - (best - second) * crossSecond;
            T denom = static_cast<T>(2) * (crossThird - crossSecond);
            if (denom > static_cast<T>(0))
            {
                numer = -numer;
            }
            denom = crd::math::fabs(denom);
            const bool tooLong = crd::math::fabs(numer) >= crd::math::fabs(static_cast<T>(0.5) * denom * lastStep);
            const bool outside = numer <= denom * (lo - best) || numer >= denom * (hi - best);
            if (tooLong || outside)
            {
                return false;
            }
            lastStep = step;
            step = numer / denom;
            const T trial = best + step;
            if (trial - lo < unit2 || hi - trial < unit2)
            {
                step = toward(centre - best);
            }
            return true;
        };
        if (!(crd::math::fabs(lastStep) > unit && try_parabola()))
        {
            // Golden-section step into the larger of the two sub-intervals.
            lastStep = best >= centre ? lo - best : hi - best;
            step = goldenFraction * lastStep;
        }
        const T point = crd::math::fabs(step) >= unit ? best + step : best + toward(step);
        const T fPoint = f(point);
        if (fPoint <= fBest)
        {
            (point >= best ? lo : hi) = best;
            third = second;
            second = best;
            best = point;
            fThird = fSecond;
            fSecond = fBest;
            fBest = fPoint;
            continue;
        }
        (point < best ? lo : hi) = point;
        if (fPoint <= fSecond || second == best)
        {
            third = second;
            second = point;
            fThird = fSecond;
            fSecond = fPoint;
        }
        else if (fPoint <= fThird || third == best || third == second)
        {
            third = point;
            fThird = fPoint;
        }
    }
    fmin_out = fBest;
    return best;
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
