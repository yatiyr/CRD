#pragma once

// ★★★ Multi-DoF TIME-SYNCHRONIZED online trajectory generation from ARBITRARY states — Ruckig's step2 ("reach the
// target in EXACTLY tf") + the synchronizer on top of the single-DoF OTG in otg.hpp.
//
// A faithful reimplementation of Ruckig's PositionThirdOrderStep2 (Berscheid & Lien 2021, MIT). Multi-DoF motion must
// arrive together (all axes finish at the same instant so the tool traces a straight Cartesian path). tsync = the
// slowest DoF's min-time (from plan_otg); every other DoF is then re-planned to reach its target in EXACTLY tsync via
// step2 — a set of closed-form 7-phase candidates (UDDU and UDUD jerk patterns) each solving a quartic/quintic for the
// phase times, validated by a check that also pins the total time to tf. Deterministic (crd::math + a WCET-bounded
// derivative-bracketing polynomial solver — no runtime allocation, no unbounded iteration). Reconstructed +
// verified against the `ruckig` package on 2474/2474 random arbitrary-state sync cases (scripts/ruckig_step2.py).

#include <crd/hesap/motion/otg.hpp>

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::motion
{

namespace detail
{

// Horner evaluation of a polynomial given by DESCENDING coefficients c[0]*x^deg + ... + c[deg].
template <typename T>
T otg_poly_eval(const T* c, int deg, T x)
{
    T r = c[0];
    for (int i = 1; i <= deg; ++i)
    {
        r = r * x + c[i];
    }
    return r;
}

// Refine a single root of p in [lo,hi] where p(lo)*p(hi) <= 0 (Newton-bisection hybrid with early break — like
// Ruckig's shrink_interval; converges fast, deterministic + WCET-bounded).
template <typename T>
T otg_shrink(const T* c, int deg, T lo, T hi)
{
    if (otg_poly_eval<T>(c, deg, lo) > T{0})
    {
        const T tmp = lo;
        lo          = hi;
        hi          = tmp;
    }
    T dc[6];
    for (int i = 0; i < deg; ++i)
    {
        dc[i] = c[i] * static_cast<T>(deg - i);
    }
    T rts   = static_cast<T>(0.5) * (lo + hi);
    T dxold = crd::math::fabs(hi - lo);
    T dx    = dxold;
    T f     = otg_poly_eval<T>(c, deg, rts);
    T df    = otg_poly_eval<T>(dc, deg - 1, rts);
    for (int it = 0; it < 64; ++it)
    {
        if ((((rts - hi) * df - f) * ((rts - lo) * df - f) > T{0})
            || (crd::math::fabs(2 * f) > crd::math::fabs(dxold * df)))
        {
            dxold = dx;
            dx    = static_cast<T>(0.5) * (hi - lo);
            rts   = lo + dx;
            if (lo == rts)
            {
                break;
            }
        }
        else
        {
            dxold           = dx;
            dx              = f / df;
            const T tmp     = rts;
            rts -= dx;
            if (tmp == rts)
            {
                break;
            }
        }
        if (crd::math::fabs(dx) < static_cast<T>(1e-14))
        {
            break;
        }
        f  = otg_poly_eval<T>(c, deg, rts);
        df = otg_poly_eval<T>(dc, deg - 1, rts);
        if (f < T{0})
        {
            lo = rts;
        }
        else
        {
            hi = rts;
        }
    }
    return rts;
}

// All real roots of a MONIC descending-coeff polynomial (c[0]==1, degree ≤ 5) within [lo,hi]. Closed-form for
// deg ≤ 4 (via otg_solve_quart / quadratic); for deg 5 the quartic derivative gives the extrema (closed form) →
// bracket the quintic's roots between them → Newton-shrink. Deterministic + WCET-bounded. Returns the count.
template <typename T>
int otg_solve_poly_interval(const T* c, int deg, T lo, T hi, T* out)
{
    T   raw[6];
    int nraw = 0;
    if (deg == 2)
    {
        const T disc = c[1] * c[1] - static_cast<T>(4) * c[2];
        if (disc >= T{0})
        {
            const T s = crd::math::sqrt(disc);
            raw[nraw++] = (-c[1] - s) / static_cast<T>(2);
            raw[nraw++] = (-c[1] + s) / static_cast<T>(2);
        }
    }
    else if (deg == 3)
    {
        // derivative is quadratic -> extrema closed form -> bracket
        const T dc[3] = {static_cast<T>(3), static_cast<T>(2) * c[1], c[2]};
        T       ex[2];
        int     ne   = 0;
        const T disc = dc[1] * dc[1] - static_cast<T>(4) * dc[0] * dc[2];
        if (disc >= T{0})
        {
            const T s = crd::math::sqrt(disc);
            ex[ne++]  = (-dc[1] - s) / (static_cast<T>(2) * dc[0]);
            ex[ne++]  = (-dc[1] + s) / (static_cast<T>(2) * dc[0]);
        }
        T pts[4];
        int np = 0;
        pts[np++] = lo;
        for (int i = 0; i < ne; ++i)
        {
            if (ex[i] > lo && ex[i] < hi)
            {
                pts[np++] = ex[i];
            }
        }
        pts[np++] = hi;
        for (int i = 1; i < np; ++i)
        {
            const T key = pts[i];
            int     j   = i - 1;
            while (j >= 0 && pts[j] > key)
            {
                pts[j + 1] = pts[j];
                --j;
            }
            pts[j + 1] = key;
        }
        for (int i = 0; i < np - 1; ++i)
        {
            if (otg_poly_eval<T>(c, deg, pts[i]) * otg_poly_eval<T>(c, deg, pts[i + 1]) <= T{0})
            {
                raw[nraw++] = otg_shrink<T>(c, deg, pts[i], pts[i + 1]);
            }
        }
    }
    else if (deg == 4)
    {
        nraw = otg_solve_quart<T>(c[1], c[2], c[3], c[4], raw);
    }
    else // deg == 5
    {
        // quartic derivative (monic): d/dx = 5x^4 + 4c1 x^3 + 3c2 x^2 + 2c3 x + c4 -> /5 monic
        T   crit[4];
        int nc = otg_solve_quart<T>(static_cast<T>(4) * c[1] / 5, static_cast<T>(3) * c[2] / 5,
                                    static_cast<T>(2) * c[3] / 5, c[4] / 5, crit);
        T   pts[8];
        int np    = 0;
        pts[np++] = lo;
        for (int i = 0; i < nc; ++i)
        {
            if (crit[i] > lo && crit[i] < hi)
            {
                pts[np++] = crit[i];
            }
        }
        pts[np++] = hi;
        for (int i = 1; i < np; ++i)
        {
            const T key = pts[i];
            int     j   = i - 1;
            while (j >= 0 && pts[j] > key)
            {
                pts[j + 1] = pts[j];
                --j;
            }
            pts[j + 1] = key;
        }
        for (int i = 0; i < np - 1; ++i)
        {
            if (otg_poly_eval<T>(c, deg, pts[i]) * otg_poly_eval<T>(c, deg, pts[i + 1]) <= T{0})
            {
                raw[nraw++] = otg_shrink<T>(c, deg, pts[i], pts[i + 1]);
            }
        }
    }
    int n = 0;
    for (int i = 0; i < nraw; ++i)
    {
        if (raw[i] >= lo - static_cast<T>(1e-12) && raw[i] <= hi + static_cast<T>(1e-12))
        {
            out[n++] = raw[i];
        }
    }
    return n;
}

// check_with_timing: validate a 7-phase profile (UDDU or UDUD jerk pattern, jerk magnitude jf) reaches (pf,vf,af) in
// EXACTLY tf, respecting symmetric vmax/amax on the back half + interior a=0 crossings (Ruckig's rule). Returns valid.
template <typename T>
bool otg_check_timed(const T* t, bool udud, T jf, T p0, T v0, T a0, T pf, T vf, T af, T vmax, T amax, T tf)
{
    for (int i = 0; i < 7; ++i)
    {
        if (t[i] < static_cast<T>(-1e-10) || t[i] != t[i]) // t[i]!=t[i] catches NaN
        {
            return false;
        }
    }
    T js[7];
    js[1] = js[3] = js[5] = T{0};
    js[0]         = t[0] > T{0} ? jf : T{0};
    js[2]         = t[2] > T{0} ? -jf : T{0};
    js[4]         = t[4] > T{0} ? (udud ? jf : -jf) : T{0};
    js[6]         = t[6] > T{0} ? (udud ? -jf : jf) : T{0};
    T va[8], aa[8], pa[8];
    pa[0]   = p0;
    va[0]   = v0;
    aa[0]   = a0;
    T total = T{0};
    for (int i = 0; i < 7; ++i)
    {
        const T d = t[i] > T{0} ? t[i] : T{0};
        total += d;
        aa[i + 1] = aa[i] + d * js[i];
        va[i + 1] = va[i] + d * (aa[i] + d * js[i] / static_cast<T>(2));
        pa[i + 1] = pa[i] + d * (va[i] + d * (aa[i] / static_cast<T>(2) + d * js[i] / static_cast<T>(6)));
    }
    if (crd::math::fabs(total - tf) > static_cast<T>(1e-6))
    {
        return false;
    }
    if (crd::math::fabs(pa[7] - pf) > static_cast<T>(1e-6) || crd::math::fabs(va[7] - vf) > static_cast<T>(1e-6)
        || crd::math::fabs(aa[7] - af) > static_cast<T>(1e-6))
    {
        return false;
    }
    const T ve = static_cast<T>(1e-6);
    const T ae = static_cast<T>(1e-6);
    for (int idx : {1, 3, 5})
    {
        if (aa[idx] > amax + ae || aa[idx] < -amax - ae)
        {
            return false;
        }
    }
    for (int idx : {3, 4, 5, 6})
    {
        if (va[idx] > vmax + ve || va[idx] < -vmax - ve)
        {
            return false;
        }
    }
    for (int i = 2; i < 7; ++i)
    {
        // den != 0 is already implied by the fabs guard; stated explicitly so the LTCG optimizer can prove the
        // division safe (win-shipping /GL fired C4723 potential-div-by-zero without it). Bit-identical semantics.
        const T den = static_cast<T>(2) * js[i];
        if (aa[i + 1] * aa[i] < static_cast<T>(-1e-16) && crd::math::fabs(js[i]) > static_cast<T>(1e-15) &&
            den != T{0})
        {
            const T vaz = va[i] - aa[i] * aa[i] / den;
            if (vaz > vmax + ve || vaz < -vmax - ve)
            {
                return false;
            }
        }
    }
    return true;
}

// Step2 for one limit convention. Tries the case candidates in Ruckig's order; on the FIRST profile that reaches the
// target in exactly tf, fills out_t[7]/out_j (magnitude)/out_udud and returns true.
template <typename T>
bool otg_step2_convention(T p0, T v0, T a0, T pf, T vf, T af, T VMAX, T AMAX, T aMax, T aMin, T jMax, T tf, T* out_t,
                          T& out_jf, bool& out_udud)
{
    const T pd = pf - p0;
    const T vd = vf - v0;
    const T vd_vd = vd * vd;
    const T v0_v0 = v0 * v0;
    const T vf_vf = vf * vf;
    const T ad    = af - a0;
    const T ad_ad = ad * ad;
    const T a0_a0 = a0 * a0;
    const T af_af = af * af;
    const T a0_p3 = a0 * a0_a0;
    const T a0_p4 = a0_a0 * a0_a0;
    const T a0_p5 = a0_p3 * a0_a0;
    const T a0_p6 = a0_p4 * a0_a0;
    const T af_p3 = af * af_af;
    const T af_p4 = af_af * af_af;
    const T af_p5 = af_p3 * af_af;
    const T af_p6 = af_p4 * af_af;
    const T tf_tf = tf * tf;
    const T tf_p3 = tf_tf * tf;
    const T tf_p4 = tf_tf * tf_tf;
    const T jj    = jMax * jMax;
    const T g1    = -pd + tf * v0;
    const T g2    = -2 * pd + tf * (v0 + vf);
    const T eps   = static_cast<T>(2.220446049250313e-16);

    T   t[7];
    bool found = false;
    auto try_it = [&](bool udud, T jf) -> bool {
        if (otg_check_timed<T>(t, udud, jf, p0, v0, a0, pf, vf, af, VMAX, AMAX, tf))
        {
            for (int i = 0; i < 7; ++i)
            {
                out_t[i] = t[i] > T{0} ? t[i] : T{0};
            }
            out_jf   = jf;
            out_udud = udud;
            found    = true;
            return true;
        }
        return false;
    };
    auto zero = [&]() {
        for (int i = 0; i < 7; ++i)
        {
            t[i] = T{0};
        }
    };
    auto sq = [](T x) { return x >= T{0} ? crd::math::sqrt(x) : T{-1}; };

    // ===== acc0_acc1_vel =====
    if ((2 * (aMax - aMin) + ad) / jMax < tf)
    {
        const T inner = (a0_p4 + af_p4 - 4 * a0_p3 * (2 * aMax + aMin) / 3 - 4 * af_p3 * (aMax + 2 * aMin) / 3
                         + 2 * (a0_a0 - af_af) * aMax * aMax
                         + (4 * a0 * aMax - 2 * a0_a0)
                               * (af_af - 2 * af * aMin + (aMin - aMax) * aMin + 2 * jMax * (aMin * tf - vd))
                         + 2 * af_af * (aMin * aMin + 2 * jMax * (aMax * tf - vd))
                         + 4 * jMax
                               * (2 * aMin * (af * vd + jMax * g1) + (aMax * aMax - aMin * aMin) * vd + jMax * vd_vd)
                         + 8 * aMax * jj * (pd - tf * vf))
                            / (aMax * aMin)
                        + 4 * af_af + 2 * a0_a0 + (4 * af + aMax - aMin) * (aMax - aMin)
                        + 4 * jMax * (aMin - aMax + jMax * tf - 2 * af) * tf;
        if (inner >= T{0})
        {
            const T h1 = crd::math::sqrt(inner) * (crd::math::fabs(jMax) / jMax);
            zero();
            t[0] = (-a0 + aMax) / jMax;
            t[1] = (-(af_af - a0_a0 + 2 * aMax * aMax + aMin * (aMin - 2 * ad - 3 * aMax) + 2 * jMax * (aMin * tf - vd))
                    + aMin * h1)
                   / (2 * (aMax - aMin) * jMax);
            t[2] = aMax / jMax;
            t[3] = (aMin - aMax + h1) / (2 * jMax);
            t[4] = -aMin / jMax;
            t[5] = tf - (t[0] + t[1] + t[2] + t[3] + 2 * t[4] + af / jMax);
            t[6] = t[4] + af / jMax;
            if (try_it(false, jMax))
            {
                return true;
            }
        }
    }
    if ((-a0 + 4 * aMax - af) / jMax < tf)
    {
        const T den = (a0_a0 + af_af - 2 * (a0 + af) * aMax + 2 * (aMax * aMax - aMax * jMax * tf + jMax * vd));
        if (crd::math::fabs(den) > static_cast<T>(1e-14))
        {
            zero();
            t[0] = (-a0 + aMax) / jMax;
            t[1] = (3 * (a0_p4 + af_p4) - 4 * (a0_p3 + af_p3) * aMax - 4 * af_p3 * aMax
                    + 24 * (a0 + af) * aMax * aMax * aMax - 6 * (af_af + a0_a0) * (aMax * aMax - 2 * jMax * vd)
                    + 6 * a0_a0 * (af_af - 2 * af * aMax - 2 * aMax * jMax * tf)
                    - 12 * aMax * aMax * (2 * aMax * aMax - 2 * aMax * jMax * tf + jMax * vd) - 24 * af * aMax * jMax * vd
                    + 12 * jj * (2 * aMax * g1 + vd_vd))
                   / (12 * aMax * jMax * den);
            t[2] = aMax / jMax;
            t[3] = (-a0_a0 - af_af + 2 * aMax * (a0 + af - 2 * aMax) - 2 * jMax * vd) / (2 * aMax * jMax) + tf;
            t[4] = t[2];
            t[5] = tf - (t[0] + t[1] + t[2] + t[3] + 2 * t[4] - af / jMax);
            t[6] = t[4] - af / jMax;
            if (try_it(true, jMax))
            {
                return true;
            }
        }
    }

    // ===== acc1_vel (UDDU + UDUD), quartic =====
    {
        const T ph1 = a0_a0 + af_af - aMin * (a0 + 2 * af - aMin) - 2 * jMax * (vd - aMin * tf);
        const T ph2 = 2 * aMin * (jMax * g1 + af * vd) - aMin * aMin * vd + jMax * vd_vd;
        const T ph3 = af_af + aMin * (aMin - 2 * af) - 2 * jMax * (vd - aMin * tf);
        const T poly[5] = {T{1}, (2 * (2 * a0 - aMin)) / jMax, (4 * a0_a0 + ph1 - 3 * a0 * aMin) / jj,
                           (2 * a0 * ph1) / (jj * jMax),
                           (3 * (a0_p4 + af_p4) - 4 * (a0_p3 + 2 * af_p3) * aMin + 6 * af_af * (aMin * aMin - 2 * jMax * vd)
                            + 12 * jMax * ph2 + 6 * a0_a0 * ph3)
                               / (12 * jj * jj)};
        T   roots[6];
        int nr = otg_solve_poly_interval<T>(poly, 4, T{0}, tf, roots);
        for (int ri = 0; ri < nr; ++ri)
        {
            const T t0 = roots[ri];
            const T h1 = -((a0_a0 + af_af) / 2 + jMax * (-vd + 2 * a0 * t0 + jMax * t0 * t0)) / aMin;
            zero();
            t[0] = t0;
            t[2] = a0 / jMax + t0;
            t[3] = tf - (h1 - aMin + a0 + af) / jMax - 2 * t0;
            t[4] = -aMin / jMax;
            t[5] = (h1 + aMin) / jMax;
            t[6] = t[4] + af / jMax;
            if (try_it(false, jMax))
            {
                return true;
            }
        }
    }
    {
        const T ph1 = a0_a0 - af_af + (2 * af - a0) * aMax - aMax * aMax - 2 * jMax * (vd - aMax * tf);
        const T ph2 = aMax * aMax + 2 * jMax * vd;
        const T ph3 = af_af + ph2 - 2 * aMax * (af + jMax * tf);
        const T ph4 = 2 * aMax * jMax * g1 + aMax * aMax * vd + jMax * vd_vd;
        const T poly[5] = {T{1}, (4 * a0 - 2 * aMax) / jMax, (4 * a0_a0 - 3 * a0 * aMax + ph1) / jj,
                           (2 * a0 * ph1) / (jj * jMax),
                           (3 * (a0_p4 + af_p4) - 4 * (a0_p3 + 2 * af_p3) * aMax - 24 * af * aMax * jMax * vd
                            + 12 * jMax * ph4 - 6 * a0_a0 * ph3 + 6 * af_af * ph2)
                               / (12 * jj * jj)};
        T   roots[6];
        int nr = otg_solve_poly_interval<T>(poly, 4, T{0}, tf, roots);
        for (int ri = 0; ri < nr; ++ri)
        {
            const T t0 = roots[ri];
            const T h1 = ((a0_a0 - af_af) / 2 + jj * t0 * t0 - jMax * (vd - 2 * a0 * t0)) / aMax;
            zero();
            t[0] = t0;
            t[2] = t0 + a0 / jMax;
            t[3] = tf + (h1 + ad - aMax) / jMax - 2 * t0;
            t[4] = aMax / jMax;
            t[5] = -(h1 + aMax) / jMax;
            t[6] = t[4] - af / jMax;
            if (try_it(true, jMax))
            {
                return true;
            }
        }
    }

    // ===== acc0_vel (UDDU + UDUD), quartic =====
    {
        const T ph1x = 12 * jMax * (-aMax * aMax * vd - jMax * vd_vd + 2 * aMax * jMax * (-pd + tf * vf));
        const T poly[5] = {T{1}, (2 * aMax) / jMax,
                           (a0_a0 - af_af + 2 * ad * aMax + aMax * aMax + 2 * jMax * (vd - aMax * tf)) / jj, T{0},
                           -(-3 * (a0_p4 + af_p4) + 4 * (af_p3 + 2 * a0_p3) * aMax - 12 * a0 * aMax * (af_af - 2 * jMax * vd)
                             + 6 * a0_a0 * (af_af - aMax * aMax - 2 * jMax * vd)
                             + 6 * af_af * (aMax * aMax - 2 * aMax * jMax * tf + 2 * jMax * vd) + ph1x)
                               / (12 * jj * jj)};
        T   roots[6];
        int nr = otg_solve_poly_interval<T>(poly, 4, T{0}, tf, roots);
        for (int ri = 0; ri < nr; ++ri)
        {
            const T t0 = roots[ri];
            const T h1 = ((a0_a0 - af_af) / 2 + jMax * (jMax * t0 * t0 + vd)) / aMax;
            zero();
            t[0] = (-a0 + aMax) / jMax;
            t[1] = (h1 - aMax) / jMax;
            t[2] = aMax / jMax;
            t[3] = tf - (h1 + ad + aMax) / jMax - 2 * t0;
            t[4] = t0;
            t[6] = af / jMax + t0;
            if (try_it(false, jMax))
            {
                return true;
            }
        }
        const T poly2[5] = {T{1}, (-2 * aMax) / jMax,
                            -(a0_a0 + af_af - 2 * (a0 + af) * aMax + aMax * aMax + 2 * jMax * (vd - aMax * tf)) / jj, T{0},
                            (3 * (a0_p4 + af_p4) - 4 * (af_p3 + 2 * a0_p3) * aMax
                             + 6 * a0_a0 * (af_af + aMax * aMax + 2 * jMax * vd) - 12 * a0 * aMax * (af_af + 2 * jMax * vd)
                             + 6 * af_af * (aMax * aMax - 2 * aMax * jMax * tf + 2 * jMax * vd) - ph1x)
                                / (12 * jj * jj)};
        nr = otg_solve_poly_interval<T>(poly2, 4, T{0}, tf, roots);
        for (int ri = 0; ri < nr; ++ri)
        {
            const T t0 = roots[ri];
            const T h1 = ((a0_a0 + af_af) / 2 + jMax * (vd - jMax * t0 * t0)) / aMax;
            zero();
            t[0] = (-a0 + aMax) / jMax;
            t[1] = (h1 - aMax) / jMax;
            t[2] = aMax / jMax;
            t[3] = tf - (h1 - a0 - af + aMax) / jMax - 2 * t0;
            t[4] = t0;
            t[6] = -(af / jMax) + t0;
            if (try_it(true, jMax))
            {
                return true;
            }
        }
    }

    // ===== vel (UDDU): zero-state cubic, else quintic =====
    if (crd::math::fabs(v0) < eps && crd::math::fabs(a0) < eps && crd::math::fabs(vf) < eps
        && crd::math::fabs(af) < eps)
    {
        const T poly[4] = {T{1}, -tf / 2, T{0}, pd / (2 * jMax)};
        T       roots[4];
        int     nr = otg_solve_poly_interval<T>(poly, 3, T{0}, tf, roots);
        for (int ri = 0; ri < nr; ++ri)
        {
            const T t0 = roots[ri];
            zero();
            t[0] = t0;
            t[2] = t0;
            t[3] = tf - 4 * t0;
            t[4] = t0;
            t[6] = t0;
            if (try_it(false, jMax))
            {
                return true;
            }
        }
    }
    else
    {
        const T p1  = af_af - 2 * jMax * (-2 * af * tf + jMax * tf_tf + 3 * vd);
        const T ph1 = af_p3 - 3 * jj * g1 - 3 * af * jMax * vd;
        const T ph2 = af_p4 + 8 * af_p3 * jMax * tf
                      + 12 * jMax * (3 * jMax * vd_vd - af_af * vd + 2 * af * jMax * (g1 - tf * vd) - 2 * jj * tf * g1);
        const T ph3 = a0 * (af - jMax * tf);
        const T ph4 = jMax * (-ad + jMax * tf);
        if (crd::math::fabs(ph4) > static_cast<T>(1e-14))
        {
            const T poly[6] = {
                T{1},
                (15 * a0_a0 + af_af + 4 * af * jMax * tf - 16 * ph3 - 2 * jMax * (jMax * tf_tf + 3 * vd)) / (4 * ph4),
                (29 * a0_p3 - 2 * af_p3 - 33 * a0 * ph3 + 6 * jj * g1 + 6 * af * jMax * vd + 6 * a0 * p1) / (6 * jMax * ph4),
                (61 * a0_p4 - 76 * a0_a0 * ph3 - 16 * a0 * ph1 + 30 * a0_a0 * p1 + ph2) / (24 * jj * ph4),
                (a0 * (7 * a0_p4 - 10 * a0_a0 * ph3 - 4 * a0 * ph1 + 6 * a0_a0 * p1 + ph2)) / (12 * jj * jMax * ph4),
                (7 * a0_p6 + af_p6 - 12 * a0_p4 * ph3 + 48 * af_p3 * jj * g1 - 8 * a0_p3 * ph1
                 - 72 * jj * jMax * (jMax * g1 * g1 + vd_vd * vd + 2 * af * g1 * vd) - 6 * af_p4 * jMax * vd
                 + 36 * af_af * jj * vd_vd + 9 * a0_p4 * p1 + 3 * a0_a0 * ph2)
                    / (144 * jj * jj * ph4)};
            T   roots[6];
            int nr = otg_solve_poly_interval<T>(poly, 5, T{0}, tf, roots);
            for (int ri = 0; ri < nr; ++ri)
            {
                const T tt   = roots[ri];
                const T disc = (a0_a0 + af_af) / (2 * jj) + (tt * (2 * a0 + jMax * tt) - vd) / jMax;
                if (disc < T{0})
                {
                    continue;
                }
                const T h1 = crd::math::sqrt(disc);
                zero();
                t[0] = tt;
                t[2] = tt + a0 / jMax;
                t[3] = tf - 2 * (tt + h1) - (a0 + af) / jMax;
                t[4] = h1;
                t[6] = h1 + af / jMax;
                if (try_it(false, jMax))
                {
                    return true;
                }
            }
        }
    }

    // ===== acc0_acc1 =====
    if (crd::math::fabs(a0) < eps && crd::math::fabs(af) < eps)
    {
        const T h1 = 2 * aMin * g1 + vd_vd + aMax * (2 * pd + aMin * tf_tf - 2 * tf * vf);
        const T h2 = ((aMax - aMin) * (-aMin * vd + aMax * (aMin * tf - vd)));
        if (crd::math::fabs(h1) > static_cast<T>(1e-14))
        {
            const T jf = h2 / h1;
            if (crd::math::fabs(jf) > static_cast<T>(1e-14))
            {
                zero();
                t[0] = aMax / jf;
                t[1] = (-2 * aMax * h1 + aMin * aMin * g2) / h2;
                t[2] = t[0];
                t[4] = -aMin / jf;
                t[5] = tf - (2 * t[0] + t[1] + 2 * t[4]);
                t[6] = t[4];
                if (try_it(false, jf))
                {
                    return true;
                }
            }
        }
    }
    else
    {
        const T base = 2 * aMin * g1 + vd_vd + aMax * (2 * pd + aMin * tf_tf - 2 * tf * vf);
        if (crd::math::fabs(base) > static_cast<T>(1e-14))
        {
            const T k = (aMax - aMin) * (-aMin * vd + aMax * (aMin * tf - vd)) - af_af * (aMax * tf - vd)
                        + 2 * af * aMin * (aMax * tf - vd) + a0_a0 * (aMin * tf + v0 - vf)
                        - 2 * a0 * aMax * (aMin * tf - vd);
            const T inner = 144 * k * k
                            + 48 * ad
                                  * (3 * a0_p3 - 3 * af_p3 + 12 * aMax * aMin * (-aMax + aMin)
                                     + 4 * af_af * (aMax + 2 * aMin)
                                     + a0 * (-3 * af_af + 8 * af * (aMin - aMax) + 6 * (aMax * aMax + 2 * aMax * aMin - aMin * aMin))
                                     + 6 * af * (aMax * aMax - 2 * aMax * aMin - aMin * aMin)
                                     + a0_a0 * (3 * af - 4 * (2 * aMax + aMin)))
                                  * base;
            if (inner >= T{0})
            {
                const T h1 = crd::math::sqrt(inner);
                const T jf = -(3 * af_af * aMax * tf - 3 * a0_a0 * aMin * tf - 6 * ad * aMax * aMin * tf
                               + 3 * aMax * aMin * (aMin - aMax) * tf + 3 * (a0_a0 - af_af) * vd
                               + 6 * vd * (af * aMin - a0 * aMax) + 3 * (aMax * aMax - aMin * aMin) * vd + h1 / 4)
                             / (6 * base);
                if (crd::math::fabs(jf) > static_cast<T>(1e-14))
                {
                    zero();
                    t[0] = (aMax - a0) / jf;
                    t[1] = (a0_a0 - af_af + 2 * ad * aMin
                            - 2 * (aMax * aMax - 2 * aMax * aMin + aMin * aMin + aMin * jf * tf - jf * vd))
                           / (2 * (aMax - aMin) * jf);
                    t[2] = aMax / jf;
                    t[4] = -aMin / jf;
                    t[5] = tf - (t[0] + t[1] + t[2] + 2 * t[4] + af / jf);
                    t[6] = t[4] + af / jf;
                    if (try_it(false, jf))
                    {
                        return true;
                    }
                }
            }
        }
    }

    // ===== acc1 (4 solutions) =====
    {
        const T inr = jj
                      * (a0_p4 + af_p4 - 4 * af_p3 * jMax * tf + 6 * af_af * jj * tf_tf - 4 * a0_p3 * (af - jMax * tf)
                         + 6 * a0_a0 * (af - jMax * tf) * (af - jMax * tf) + 24 * af * jj * g1
                         - 4 * a0 * (af_p3 - 3 * af_af * jMax * tf + 6 * jj * (-pd + tf * vf))
                         - 12 * jj * (-vd_vd + jMax * tf * g2))
                      / 3;
        const T h0 = sq(inr);
        if (h0 >= T{0})
        {
            const T h0v = h0 / jMax;
            const T d2  = (a0_a0 + af_af - 2 * a0 * af - 2 * ad * jMax * tf + 2 * h0v) / jj + tf_tf;
            const T h1  = sq(d2);
            if (h1 >= T{0} && crd::math::fabs(-ad + jMax * tf) > static_cast<T>(1e-14))
            {
                zero();
                t[0] = -(a0_a0 + af_af + 2 * a0 * (jMax * tf - af) - 2 * jMax * vd + h0v) / (2 * jMax * (-ad + jMax * tf));
                t[2] = (tf - h1) / 2 - ad / (2 * jMax);
                t[5] = h1;
                t[6] = tf - (t[0] + t[2] + t[5]);
                if (try_it(false, jMax))
                {
                    return true;
                }
            }
        }
    }
    {
        const T inr = jj
                      * (a0_p4 + af_p4 + 4 * (af_p3 - a0_p3) * jMax * tf + 6 * af_af * jj * tf_tf
                         + 6 * a0_a0 * (af + jMax * tf) * (af + jMax * tf) + 24 * af * jj * g1
                         - 4 * a0 * (a0_a0 * af + af_p3 + 3 * af_af * jMax * tf + 6 * jj * (-pd + tf * vf))
                         + 12 * jj * (vd_vd + jMax * tf * g2))
                      / 3;
        const T h0 = sq(inr);
        if (h0 >= T{0})
        {
            const T h0v = h0 / jMax;
            const T d2  = (a0_a0 + af_af - 2 * a0 * af + 2 * ad * jMax * tf + 2 * h0v) / jj + tf_tf;
            const T h1  = sq(d2);
            if (h1 >= T{0} && crd::math::fabs(ad + jMax * tf) > static_cast<T>(1e-14))
            {
                zero();
                t[2] = -(a0_a0 + af_af - 2 * a0 * af + 2 * jMax * (vd - a0 * tf) + h0v) / (2 * jMax * (ad + jMax * tf));
                t[4] = ad / (2 * jMax) + (tf - h1) / 2;
                t[5] = h1;
                t[6] = tf - (t[5] + t[4] + t[2]);
                if (try_it(true, jMax))
                {
                    return true;
                }
            }
        }
    }
    {
        const T h0a = a0_p3 - af_p3 - 3 * a0_a0 * aMin + 3 * aMin * aMin * (a0 + jMax * tf)
                      + 3 * af * aMin * (-aMin - 2 * jMax * tf) - 3 * af_af * (-aMin - jMax * tf)
                      - 3 * jj * (-2 * pd - aMin * tf_tf + 2 * tf * vf);
        const T h0b = a0_a0 + af_af - 2 * (a0 + af) * aMin + 2 * (aMin * aMin - jMax * (-aMin * tf + vd));
        const T h0c = a0_p4 + 3 * af_p4 - 4 * (a0_p3 + 2 * af_p3) * aMin + 6 * a0_a0 * aMin * aMin
                      + 6 * af_af * (aMin * aMin - 2 * jMax * vd)
                      + 12 * jMax * (2 * aMin * jMax * g1 - aMin * aMin * vd + jMax * vd_vd) + 24 * af * aMin * jMax * vd
                      - 4 * a0
                            * (af_p3 - 3 * af * aMin * (-aMin - 2 * jMax * tf) + 3 * af_af * (-aMin - jMax * tf)
                               + 3 * jMax * (-aMin * aMin * tf + jMax * (-2 * pd - aMin * tf_tf + 2 * tf * vf)));
        const T rad = 4 * h0a * h0a - 6 * h0b * h0c;
        if (rad >= T{0} && crd::math::fabs(h0b) > static_cast<T>(1e-14))
        {
            const T h1 = (crd::math::fabs(jMax) / jMax) * crd::math::sqrt(rad);
            const T h2 = 6 * jMax * h0b;
            zero();
            t[2] = (2 * h0a + h1) / h2;
            t[3] = -(a0_a0 + af_af - 2 * (a0 + af) * aMin + 2 * (aMin * aMin + aMin * jMax * tf - jMax * vd))
                   / (2 * jMax * (a0 - aMin - jMax * t[2]));
            t[4] = (a0 - aMin) / jMax - t[2];
            t[5] = tf - (t[2] + t[3] + t[4] + (af - aMin) / jMax);
            t[6] = (af - aMin) / jMax;
            if (try_it(false, jMax))
            {
                return true;
            }
        }
    }
    {
        const T h0a = -a0_p3 + af_p3 + 3 * (a0_a0 - af_af) * aMax - 3 * ad * aMax * aMax - 6 * af * aMax * jMax * tf
                      + 3 * af_af * jMax * tf + 3 * jMax * (aMax * aMax * tf + jMax * (-2 * pd - aMax * tf_tf + 2 * tf * vf));
        const T h0b = a0_a0 - af_af + 2 * ad * aMax + 2 * jMax * (aMax * tf - vd);
        const T h0c = a0_p4 + 3 * af_p4 - 4 * (a0_p3 + 2 * af_p3) * aMax + 6 * a0_a0 * aMax * aMax
                      - 24 * af * aMax * jMax * vd + 12 * jMax * (2 * aMax * jMax * g1 + jMax * vd_vd + aMax * aMax * vd)
                      + 6 * af_af * (aMax * aMax + 2 * jMax * vd)
                      - 4 * a0
                            * (af_p3 + 3 * af * aMax * (aMax - 2 * jMax * tf) - 3 * af_af * (aMax - jMax * tf)
                               + 3 * jMax * (aMax * aMax * tf + jMax * (-2 * pd - aMax * tf_tf + 2 * tf * vf)));
        const T rad = 4 * h0a * h0a - 6 * h0b * h0c;
        if (rad >= T{0} && crd::math::fabs(h0b) > static_cast<T>(1e-14))
        {
            const T h1 = (crd::math::fabs(jMax) / jMax) * crd::math::sqrt(rad);
            const T h2 = 6 * jMax * h0b;
            zero();
            t[2] = -(2 * h0a + h1) / h2;
            t[3] = 2 * h1 / h2;
            t[4] = (aMax - a0) / jMax + t[2];
            t[5] = tf - (t[2] + t[3] + t[4] + (-af + aMax) / jMax);
            t[6] = (-af + aMax) / jMax;
            if (try_it(true, jMax))
            {
                return true;
            }
        }
    }

    // ===== acc0 (UDUD + UDDU) =====
    {
        const T d2 = ad_ad / (2 * jj) - ad * (aMax - a0) / jj + (aMax * tf - vd) / jMax;
        if (d2 >= T{0})
        {
            const T h1 = crd::math::sqrt(d2);
            zero();
            t[0] = (aMax - a0) / jMax;
            t[1] = tf - ad / jMax - 2 * h1;
            t[2] = h1;
            t[4] = (af - aMax) / jMax + h1;
            if (try_it(true, jMax))
            {
                return true;
            }
        }
    }
    {
        const T h0a = a0_p3 + 2 * af_p3 - 6 * (af_af + aMax * aMax) * aMax - 6 * (a0 + af) * aMax * jMax * tf
                      + 9 * aMax * aMax * (af + jMax * tf) + 3 * a0 * aMax * (-2 * af + 3 * aMax)
                      + 3 * a0_a0 * (af - 2 * aMax + jMax * tf) - 6 * jj * g1 + 6 * (af - aMax) * jMax * vd
                      - 3 * aMax * jj * tf_tf;
        const T h0b = a0_a0 + af_af + 2 * (aMax * aMax - (a0 + af) * aMax + jMax * (vd - aMax * tf));
        const T rad = 4 * h0a * h0a - 18 * h0b * h0b * h0b;
        if (rad >= T{0} && crd::math::fabs(h0b) > static_cast<T>(1e-14))
        {
            const T h1 = (crd::math::fabs(jMax) / jMax) * crd::math::sqrt(rad);
            const T h2 = 6 * jMax * h0b;
            zero();
            t[0] = (-a0 + aMax) / jMax;
            t[1] = ad / jMax - 2 * t[0] - (2 * h0a - h1) / h2 + tf;
            t[2] = -(2 * h0a + h1) / h2;
            t[3] = (2 * h0a - h1) / h2;
            t[4] = tf - (t[0] + t[1] + t[2] + t[3]);
            if (try_it(false, jMax))
            {
                return true;
            }
        }
    }

    // ===== none: a0=af=0 sqrt, 3-step forms, and general sub-profiles =====
    if (crd::math::fabs(v0) < eps && crd::math::fabs(a0) < eps && crd::math::fabs(af) < eps)
    {
        const T d2 = tf_tf * vf_vf + (4 * pd - tf * vf) * (4 * pd - tf * vf);
        if (d2 >= T{0} && crd::math::fabs(tf_p3) > static_cast<T>(1e-14))
        {
            const T h1 = crd::math::sqrt(d2);
            const T jf = 4 * (4 * pd - 2 * tf * vf + h1) / tf_p3;
            zero();
            t[0] = tf / 4;
            t[2] = 2 * t[0];
            t[6] = t[0];
            if (crd::math::fabs(jf) > static_cast<T>(1e-14) && try_it(false, jf))
            {
                return true;
            }
        }
    }
    {
        const T d2 = -ad_ad + jMax * (2 * (a0 + af) * tf - 4 * vd + jMax * tf_tf);
        if (d2 >= T{0})
        {
            const T h1 = crd::math::sqrt(d2) / crd::math::fabs(jMax);
            zero();
            t[0] = (tf - h1 + ad / jMax) / 2;
            t[1] = h1;
            t[2] = (tf - h1 - ad / jMax) / 2;
            if (try_it(false, jMax))
            {
                return true;
            }
        }
    }
    {
        const T poly[4] = {ad_ad, ad_ad * tf,
                           (a0_a0 + af_af + 10 * a0 * af) * tf_tf + 24 * (tf * (af * v0 - a0 * vf) - pd * ad) + 12 * vd_vd,
                           -3 * tf * ((a0_a0 + af_af + 2 * a0 * af) * tf_tf - 4 * vd * (a0 + af) * tf + 4 * vd_vd)};
        T       roots[4];
        int     nr = otg_solve_poly_interval<T>(poly, 3, T{0}, tf, roots);
        for (int ri = 0; ri < nr; ++ri)
        {
            const T tt = roots[ri];
            if (crd::math::fabs(tf - tt) < static_cast<T>(1e-14))
            {
                continue;
            }
            const T jf = ad / (tf - tt);
            if (crd::math::fabs(jf) < static_cast<T>(1e-14))
            {
                continue;
            }
            zero();
            t[0] = (2 * (vd - a0 * tf) + ad * (tt - tf)) / (2 * jf * tt);
            t[1] = tt;
            t[6] = tf - (t[0] + t[1]);
            if (try_it(false, jf))
            {
                return true;
            }
        }
    }
    {
        zero();
        t[0] = (ad_ad / jMax + 2 * (a0 + af) * tf - jMax * tf_tf - 4 * vd) / (4 * (ad - jMax * tf));
        t[2] = -ad / (2 * jMax) + tf / 2;
        t[6] = tf - (t[0] + t[2]);
        if (try_it(false, jMax))
        {
            return true;
        }
    }
    // UDDU "first acc then constant" (T024) quartic
    {
        const T poly[5] = {T{1}, -2 * tf, 2 * vd / jMax + tf_tf, 4 * (pd - tf * vf) / jMax, (vd_vd + jMax * tf * g2) / jj};
        T       roots[6];
        int     nr = otg_solve_poly_interval<T>(poly, 4, T{0}, tf, roots);
        for (int ri = 0; ri < nr; ++ri)
        {
            const T tt = roots[ri];
            if (crd::math::fabs(jMax * (2 * tt - tf)) < static_cast<T>(1e-14))
            {
                continue;
            }
            zero();
            t[0] = tt;
            t[2] = (jMax * tt * (tt - tf) + vd) / (jMax * (2 * tt - tf));
            t[3] = tf - 2 * tt;
            t[4] = tt - t[2];
            if (try_it(false, jMax))
            {
                return true;
            }
        }
    }
    // UDDU T0234 quartic
    {
        const T ph1 = af + jMax * tf;
        const T poly[5] = {T{1}, -2 * (ad + jMax * tf) / jMax,
                           2 * (a0_a0 + af_af + jMax * (af * tf + vd) - 2 * a0 * ph1) / jj + tf_tf,
                           2 * (a0_p3 - af_p3 - 3 * af_af * jMax * tf + 3 * a0 * ph1 * (ph1 - a0) - 6 * jj * (-pd + tf * vf))
                               / (3 * jj * jMax),
                           (a0_p4 + af_p4 + 4 * af_p3 * jMax * tf - 4 * a0_p3 * ph1 + 6 * a0_a0 * ph1 * ph1
                            + 24 * jj * af * g1 - 4 * a0 * (af_p3 + 3 * af_af * jMax * tf + 6 * jj * (-pd + tf * vf))
                            + 6 * jj * af_af * tf_tf + 12 * jj * (vd_vd + jMax * tf * g2))
                               / (12 * jj * jj)};
        T   roots[6];
        int nr = otg_solve_poly_interval<T>(poly, 4, T{0}, tf, roots);
        for (int ri = 0; ri < nr; ++ri)
        {
            const T tt  = roots[ri];
            const T den = (-ad + jMax * (2 * tt - tf));
            if (crd::math::fabs(den) < static_cast<T>(1e-14))
            {
                continue;
            }
            zero();
            t[0] = tt;
            t[2] = (ad_ad + 2 * jMax * (-a0 * tf - ad * tt + jMax * tt * (tt - tf) + vd)) / (2 * jMax * den);
            t[3] = ad / jMax + tf - 2 * tt;
            t[4] = tf - (tt + t[2] + t[3]);
            if (try_it(false, jMax))
            {
                return true;
            }
        }
    }
    // UDDU T3456 (direct)
    {
        const T h1n = 3 * jMax * (ad_ad + 2 * jMax * (a0 * tf - vd));
        const T h2n = ad_ad + 2 * jMax * (a0 * tf - vd);
        const T rad = 4
                          * (2 * (a0_p3 - af_p3) - 6 * a0_a0 * (af - jMax * tf) + 6 * jj * g1
                             + 3 * a0 * (2 * af_af - 2 * jMax * af * tf + jj * tf_tf) + 6 * ad * jMax * vd)
                          * (2 * (a0_p3 - af_p3) - 6 * a0_a0 * (af - jMax * tf) + 6 * jj * g1
                             + 3 * a0 * (2 * af_af - 2 * jMax * af * tf + jj * tf_tf) + 6 * ad * jMax * vd)
                      - 18 * h2n * h2n * h2n;
        if (rad >= T{0} && crd::math::fabs(h1n) > static_cast<T>(1e-14))
        {
            const T h0 = crd::math::sqrt(rad) / h1n * (crd::math::fabs(jMax) / jMax);
            zero();
            t[3] = (af_p3 - a0_p3 + 3 * (af_af - a0_a0) * jMax * tf - 3 * ad * (a0 * af + 2 * jMax * vd) - 6 * jj * g2) / h1n;
            t[4] = (tf - t[3] - h0) / 2 - ad / (2 * jMax);
            t[5] = h0;
            t[6] = (tf - t[3] + ad / jMax - h0) / 2;
            if (try_it(false, jMax))
            {
                return true;
            }
        }
    }
    // UDDU T2346 quartic
    {
        const T ph1 = ad_ad + 2 * (af + a0) * jMax * tf - jMax * (jMax * tf_tf + 4 * vd);
        const T ph2 = jMax * tf_tf * g1 - vd * (-2 * pd - tf * v0 + 3 * tf * vf);
        const T ph3 = 5 * af_af - 8 * af * jMax * tf + 2 * jMax * (2 * jMax * tf_tf - vd);
        const T ph4 = jj * tf_p4 - 2 * vd_vd + 8 * jMax * tf * (-pd + tf * vf);
        const T ph5 = (5 * af_p4 - 8 * af_p3 * jMax * tf - 12 * af_af * jMax * (jMax * tf_tf + vd)
                       + 24 * af * jj * (-2 * pd + jMax * tf_p3 + 2 * tf * vf) - 6 * jj * ph4);
        const T ph6 = -vd_vd + jMax * tf * (-2 * pd + 3 * tf * v0 - tf * vf) - af * g2;
        if (crd::math::fabs(ph1) > static_cast<T>(1e-14))
        {
            const T poly[5] = {
                T{1},
                -(4 * (a0_p3 - af_p3) - 12 * a0_a0 * (af - jMax * tf)
                  + 6 * a0 * (2 * af_af - 2 * af * jMax * tf + jMax * (jMax * tf_tf - 2 * vd))
                  + 6 * af * jMax * (3 * jMax * tf_tf + 2 * vd)
                  - 6 * jj * (-4 * pd + jMax * tf_p3 - 2 * tf * v0 + 6 * tf * vf))
                    / (3 * jMax * ph1),
                -(-a0_p4 - af_p4 + 4 * a0_p3 * (af - jMax * tf)
                  + a0_a0 * (-6 * af_af + 8 * af * jMax * tf - 4 * jMax * (jMax * tf_tf - vd))
                  + 2 * af_af * jMax * (jMax * tf_tf + 2 * vd) - 4 * af * jj * (-3 * pd + jMax * tf_p3 + 2 * tf * v0 + tf * vf)
                  + jj * (jj * tf_p4 - 8 * vd_vd + 4 * jMax * tf * (-3 * pd + tf * v0 + 2 * tf * vf))
                  + 2 * a0 * (2 * af_p3 - 2 * af_af * jMax * tf + af * jMax * (-3 * jMax * tf_tf - 4 * vd)
                              + jj * (-6 * pd + jMax * tf_p3 - 4 * tf * v0 + 10 * tf * vf)))
                    / (jj * ph1),
                -(a0_p5 - af_p5 + af_p4 * jMax * tf - 5 * a0_p4 * (af - jMax * tf) + 2 * a0_p3 * ph3
                  + 4 * af_p3 * jMax * (jMax * tf_tf + vd) + 12 * jj * af * ph6
                  - 2 * a0_a0 * (5 * af_p3 - 9 * af_af * jMax * tf - 6 * af * jMax * vd + 6 * jj * (-2 * pd - tf * v0 + 3 * tf * vf))
                  - 12 * jj * jMax * ph2 + a0 * ph5)
                    / (3 * jj * jMax * ph1),
                -(-a0_p6 - af_p6 + 6 * a0_p5 * (af - jMax * tf) - 48 * af_p3 * jj * g1
                  + 72 * jj * jMax * (jMax * g1 * g1 + vd_vd * vd + 2 * af * g1 * vd) - 3 * a0_p4 * ph3
                  - 36 * af_af * jj * vd_vd + 6 * af_p4 * jMax * vd
                  + 4 * a0_p3 * (5 * af_p3 - 9 * af_af * jMax * tf - 6 * af * jMax * vd + 6 * jj * (-2 * pd - tf * v0 + 3 * tf * vf))
                  - 3 * a0_a0 * ph5
                  + 6 * a0
                        * (af_p5 - af_p4 * jMax * tf - 4 * af_p3 * jMax * (jMax * tf_tf + vd)
                           + 12 * jj * (-af * ph6 + jMax * ph2)))
                    / (18 * jj * jj * ph1)};
            T   roots[6];
            int nr = otg_solve_poly_interval<T>(poly, 4, T{0}, tf, roots);
            for (int ri = 0; ri < nr; ++ri)
            {
                const T tt    = roots[ri];
                const T disc2 = 2 * ad_ad + 4 * jMax * (ad * tt + a0 * tf + jMax * tt * (tt - tf) - vd);
                if (disc2 < T{0})
                {
                    continue;
                }
                const T h1 = crd::math::sqrt(disc2) / crd::math::fabs(jMax);
                zero();
                t[2] = tt;
                t[3] = tf - 2 * tt - ad / jMax - h1;
                t[4] = h1 / 2;
                t[6] = tf - (tt + t[3] + t[4]);
                if (try_it(false, jMax))
                {
                    return true;
                }
            }
        }
    }
    (void)a0_p5;
    (void)af_p5;
    (void)v0_v0;
    return found;
}

} // namespace detail

// ★★★ Plan a single-DoF profile reaching (pf,vf,af) in EXACTLY tf (tf ≥ the min-time from plan_otg). The
// synchronization primitive: bring every DoF to a common arrival time. Returns an invalid profile if tf is
// infeasible (< min-time). Deterministic, allocation-free, WCET-bounded.
template <typename T>
[[nodiscard]] OtgProfile<T> plan_otg_timed(T p0, T v0, T a0, T pf, T vf, T af, T vmax, T amax, T jmax, T tf)
{
    OtgProfile<T> prof;
    prof.p0 = p0;
    prof.v0 = v0;
    prof.a0 = a0;
    detail::otg_brake<T>(v0, a0, vmax, amax, jmax, prof.brake_t, prof.brake_j);
    T pb = p0, vb = v0, ab = a0, brake_dur = T{0};
    for (int i = 0; i < 2; ++i)
    {
        const T d = prof.brake_t[i];
        const T j = prof.brake_j[i];
        pb        = pb + vb * d + T{0.5} * ab * d * d + j * d * d * d / static_cast<T>(6);
        vb        = vb + ab * d + T{0.5} * j * d * d;
        ab        = ab + j * d;
        brake_dur += d;
    }
    const T tf_rem = tf - brake_dur;
    T       jf     = jmax;
    bool    udud   = false;
    // up_first (Ruckig): the likely direction — try that convention first so the valid case is usually found without
    // evaluating the opposite direction at all.
    const bool up_first = ((pf - pb) > tf_rem * vb);
    auto conv_up = [&]() {
        return detail::otg_step2_convention<T>(pb, vb, ab, pf, vf, af, vmax, amax, amax, -amax, jmax, tf_rem, prof.t, jf,
                                               udud);
    };
    auto conv_dn = [&]() {
        return detail::otg_step2_convention<T>(pb, vb, ab, pf, vf, af, vmax, amax, -amax, amax, -jmax, tf_rem, prof.t,
                                               jf, udud);
    };
    const bool ok = up_first ? (conv_up() || conv_dn()) : (conv_dn() || conv_up());
    if (!ok)
    {
        prof.valid = false;
        return prof;
    }
    prof.j[1] = prof.j[3] = prof.j[5] = T{0};
    prof.j[0]                         = prof.t[0] > T{0} ? jf : T{0};
    prof.j[2]                         = prof.t[2] > T{0} ? -jf : T{0};
    prof.j[4]                         = prof.t[4] > T{0} ? (udud ? jf : -jf) : T{0};
    prof.j[6]                         = prof.t[6] > T{0} ? (udud ? -jf : jf) : T{0};
    prof.duration                     = tf;
    prof.valid                        = true;
    return prof;
}

// ★★★ Multi-DoF TIME-SYNCHRONIZED OTG from ARBITRARY states: plan every DoF to arrive at the SAME instant
// tsync = max_d(min-time). The critical DoF uses its time-optimal profile (plan_otg); every other DoF is re-planned
// to reach its target in exactly tsync via step2 (plan_otg_timed). Fills out[0..ndof-1]; returns tsync. This is the
// full Ruckig-class multi-axis online generator (works mid-motion from any current velocity/acceleration).
template <typename T>
[[nodiscard]] T plan_synchronized_otg(int ndof, const T* p0, const T* v0, const T* a0, const T* pf, const T* vf,
                                      const T* af, const T* vmax, const T* amax, const T* jmax, OtgProfile<T>* out)
{
    T tsync = T{0};
    for (int d = 0; d < ndof; ++d)
    {
        const OtgProfile<T> p = plan_otg<T>(p0[d], v0[d], a0[d], pf[d], vf[d], af[d], vmax[d], amax[d], jmax[d]);
        out[d]                = p;
        if (p.valid && p.duration > tsync)
        {
            tsync = p.duration;
        }
    }
    for (int d = 0; d < ndof; ++d)
    {
        if (out[d].valid && crd::math::fabs(out[d].duration - tsync) < static_cast<T>(1e-9))
        {
            continue; // critical (or already-tsync) DoF keeps its time-optimal profile
        }
        const OtgProfile<T> s = plan_otg_timed<T>(p0[d], v0[d], a0[d], pf[d], vf[d], af[d], vmax[d], amax[d], jmax[d],
                                                  tsync);
        if (s.valid)
        {
            out[d] = s;
        }
    }
    return tsync;
}

} // namespace crd::hesap::motion
