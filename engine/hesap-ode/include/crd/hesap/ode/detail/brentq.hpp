#pragma once

// brentq.hpp — Phase 3.1.6 v9-c: Brent's root-finder (the zeroin algorithm: bisection + secant + inverse
// quadratic with Brent's acceptance rules) on a bracketing interval. Deterministic: pure FP function of
// the inputs. Used by event detection (scipy's solve_ivp refines event roots with brentq at xtol = rtol =
// 4·eps over the dense output). ADR-0091.

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <cmath>

namespace crd::hesap::ode::detail
{

// Find x in [a, b] with g(x) = 0, given g(a)·g(b) <= 0. Returns the root estimate; converges when the
// bracket shrinks below xtol + rtol·|x|. F is any callable T(T).
template <typename T, typename F> [[nodiscard]] T brentq(F&& g, T a, T b, T xtol, T rtol, crd::u32 max_iter = 100)
{
    T fa = g(a);
    T fb = g(b);
    CRD_ASSERT(!(fa > static_cast<T>(0) && fb > static_cast<T>(0)) &&
               !(fa < static_cast<T>(0) && fb < static_cast<T>(0)));

    if (fa == static_cast<T>(0))
    {
        return a;
    }
    if (fb == static_cast<T>(0))
    {
        return b;
    }

    T c = a;
    T fc = fa;
    T d = b - a;
    T e = d;

    for (crd::u32 iter = 0; iter < max_iter; ++iter)
    {
        if (std::abs(fc) < std::abs(fb))
        {
            a = b;
            b = c;
            c = a;
            fa = fb;
            fb = fc;
            fc = fa;
        }
        const T tol = static_cast<T>(2) * rtol * std::abs(b) + static_cast<T>(0.5) * xtol;
        const T m = static_cast<T>(0.5) * (c - b);
        if (std::abs(m) <= tol || fb == static_cast<T>(0))
        {
            return b;
        }
        if (std::abs(e) < tol || std::abs(fa) <= std::abs(fb))
        {
            d = m; // bisection
            e = m;
        }
        else
        {
            T s = fb / fa;
            T p;
            T q;
            if (a == c)
            {
                p = static_cast<T>(2) * m * s; // secant
                q = static_cast<T>(1) - s;
            }
            else
            {
                const T qq = fa / fc; // inverse quadratic
                const T r = fb / fc;
                p = s * (static_cast<T>(2) * m * qq * (qq - r) - (b - a) * (r - static_cast<T>(1)));
                q = (qq - static_cast<T>(1)) * (r - static_cast<T>(1)) * (s - static_cast<T>(1));
            }
            if (p > static_cast<T>(0))
            {
                q = -q;
            }
            else
            {
                p = -p;
            }
            const T min1 = static_cast<T>(3) * m * q - std::abs(tol * q);
            const T min2 = std::abs(e * q);
            if (static_cast<T>(2) * p < (min1 < min2 ? min1 : min2))
            {
                e = d; // accept interpolation
                d = p / q;
            }
            else
            {
                d = m; // fall back to bisection
                e = m;
            }
        }
        a = b;
        fa = fb;
        if (std::abs(d) > tol)
        {
            b += d;
        }
        else
        {
            b += (m > static_cast<T>(0) ? tol : -tol);
        }
        fb = g(b);
        if ((fb > static_cast<T>(0)) == (fc > static_cast<T>(0)))
        {
            c = a;
            fc = fa;
            d = b - a;
            e = d;
        }
    }
    return b;
}

} // namespace crd::hesap::ode::detail
