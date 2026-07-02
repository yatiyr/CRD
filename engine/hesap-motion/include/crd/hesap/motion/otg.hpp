#pragma once

// ★★ Online Time-optimal Trajectory generator (OTG) — single-DoF, jerk-limited, ARBITRARY boundary state.
//
// A faithful reimplementation of Ruckig's third-order position solver (Berscheid & Lien 2021, "Jerk-limited Real-Time
// Trajectory Generation with Arbitrary Target States", MIT-licensed): given a current state (p0,v0,a0) and target
// (pf,vf,af) with symmetric limits (vmax,amax,jmax), produce the TIME-OPTIMAL jerk-limited motion. The min-time
// trajectory is a 7-phase jerk-bang-bang profile (UDDU: jerk [+j,0,-j,0,-j,0,+j]) optionally preceded by a ≤2-phase
// BRAKE pre-trajectory when the initial (v0,a0) points away from / over the limits. Every candidate profile is a
// closed-form set of phase durations (with a monic-quartic solve for the none/acc0/acc1 cases); the min-time valid
// one wins. This is the "re-plan from any state each control cycle" primitive robots/drones need.
//
// MOAT: deterministic by construction (crd::math, fixed evaluation order), allocation-free (all stack arrays), and
// WCET-bounded (a fixed finite set of candidates, no unbounded iteration). Verified against the `ruckig` package on
// 1934/1934 random feasible arbitrary-state cases (build/ruckig_step1.py). Velocity may transiently exceed vmax in the
// first phases when |v0|,|a0| are physically too high to brake in time — Ruckig allows this and so do we (the accel
// and jerk limits stay hard; the velocity limit is enforced on the back half + interior a=0 crossings, per Ruckig's
// Profile::check).

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

#include <initializer_list>
#include <limits>

namespace crd::hesap::motion
{

// A planned OTG profile: an optional ≤2-phase brake, then a 7-phase main trajectory. Sample with eval().
template <typename T> struct OtgProfile
{
    T p0 = T{0};
    T v0 = T{0};
    T a0 = T{0};
    T brake_t[2] = {T{0}, T{0}};
    T brake_j[2] = {T{0}, T{0}};
    T t[7] = {T{0}, T{0}, T{0}, T{0}, T{0}, T{0}, T{0}};
    T j[7] = {T{0}, T{0}, T{0}, T{0}, T{0}, T{0}, T{0}};
    T duration = T{0};
    bool valid = false;

    // Sample (position, velocity, acceleration) at absolute time `time` (clamped to [0, duration]).
    void eval(T time, T& p, T& v, T& a) const
    {
        p = p0;
        v = v0;
        a = a0;
        if (time < T{0})
        {
            time = T{0};
        }
        T remaining = time > duration ? duration : time;
        auto step = [&](T dur, T jerk)
        {
            if (remaining <= T{0} || dur <= T{0})
            {
                return;
            }
            const T s = remaining < dur ? remaining : dur;
            p = p + v * s + T{0.5} * a * s * s + jerk * s * s * s / static_cast<T>(6);
            v = v + a * s + T{0.5} * jerk * s * s;
            a = a + jerk * s;
            remaining -= s;
        };
        step(brake_t[0], brake_j[0]);
        step(brake_t[1], brake_j[1]);
        for (int i = 0; i < 7; ++i)
        {
            step(t[i], j[i]);
        }
    }
};

namespace detail
{

// --- monic-cubic resolvent (Ruckig roots.hpp solve_resolvent), returns real roots into x[0..2], count returned. ---
template <typename T> int otg_solve_resolvent(T* x, T a, T b, T c)
{
    const T cos120 = static_cast<T>(-0.5);
    const T sin120 = static_cast<T>(0.866025403784438646764);
    a /= static_cast<T>(3);
    const T a2 = a * a;
    T q = a2 - b / static_cast<T>(3);
    const T r = (a * (static_cast<T>(2) * a2 - b) + c) / static_cast<T>(2);
    const T r2 = r * r;
    const T q3 = q * q * q;
    if (r2 < q3)
    {
        const T qsqrt = crd::math::sqrt(q);
        T tt = r / (q * qsqrt);
        tt = tt < T{-1} ? T{-1} : (tt > T{1} ? T{1} : tt);
        q = static_cast<T>(-2) * qsqrt;
        const T theta = crd::math::acos(tt) / static_cast<T>(3);
        const T ux = crd::math::cos(theta) * q;
        const T uyi = crd::math::sin(theta) * q;
        x[0] = ux - a;
        x[1] = ux * cos120 - uyi * sin120 - a;
        x[2] = ux * cos120 + uyi * sin120 - a;
        return 3;
    }
    T aa = -crd::math::cbrt(crd::math::fabs(r) + crd::math::sqrt(r2 - q3));
    if (r < T{0})
    {
        aa = -aa;
    }
    const T bb = (aa == T{0}) ? T{0} : q / aa;
    x[0] = (aa + bb) - a;
    x[1] = -(aa + bb) / static_cast<T>(2) - a;
    x[2] = crd::math::sqrt(static_cast<T>(3)) * (aa - bb) / static_cast<T>(2);
    if (crd::math::fabs(x[2]) < static_cast<T>(1e-300) + std::numeric_limits<T>::epsilon())
    {
        x[2] = x[1];
        return 2;
    }
    return 1;
}

// --- monic quartic x^4 + a x^3 + b x^2 + c x + d = 0; real roots into out[], count returned (≤4). ---
template <typename T> int otg_solve_quart(T a, T b, T c, T d, T* out)
{
    const T eps = std::numeric_limits<T>::epsilon();
    int n = 0;
    auto ins = [&](T r)
    {
        out[n++] = r;
    };
    if (crd::math::fabs(d) < eps)
    {
        if (crd::math::fabs(c) < eps)
        {
            ins(T{0});
            const T disc = a * a - static_cast<T>(4) * b;
            if (crd::math::fabs(disc) < eps)
            {
                ins(-a / static_cast<T>(2));
            }
            else if (disc > T{0})
            {
                const T s = crd::math::sqrt(disc);
                ins((-a - s) / static_cast<T>(2));
                ins((-a + s) / static_cast<T>(2));
            }
            return n;
        }
        if (crd::math::fabs(a) < eps && crd::math::fabs(b) < eps)
        {
            ins(T{0});
            ins(-crd::math::cbrt(c));
            return n;
        }
    }
    const T a3 = -b;
    const T b3 = a * c - static_cast<T>(4) * d;
    const T c3 = -a * a * d - c * c + static_cast<T>(4) * b * d;
    T x3[3];
    const int nz = otg_solve_resolvent<T>(x3, a3, b3, c3);
    T y = x3[0];
    if (nz != 1)
    {
        if (crd::math::fabs(x3[1]) > crd::math::fabs(y))
        {
            y = x3[1];
        }
        if (crd::math::fabs(x3[2]) > crd::math::fabs(y))
        {
            y = x3[2];
        }
    }
    T q1, q2, p1, p2;
    T disc = y * y - static_cast<T>(4) * d;
    if (crd::math::fabs(disc) < eps)
    {
        q1 = q2 = y / static_cast<T>(2);
        disc = a * a - static_cast<T>(4) * (b - y);
        if (crd::math::fabs(disc) < eps)
        {
            p1 = p2 = a / static_cast<T>(2);
        }
        else
        {
            const T s = crd::math::sqrt(disc);
            p1 = (a + s) / static_cast<T>(2);
            p2 = (a - s) / static_cast<T>(2);
        }
    }
    else
    {
        const T s = crd::math::sqrt(disc);
        q1 = (y + s) / static_cast<T>(2);
        q2 = (y - s) / static_cast<T>(2);
        p1 = (a * q1 - c) / (q1 - q2);
        p2 = (c - a * q2) / (q1 - q2);
    }
    const T eps16 = static_cast<T>(16) * eps;
    disc = p1 * p1 - static_cast<T>(4) * q1;
    if (crd::math::fabs(disc) < eps16)
    {
        ins(-p1 / static_cast<T>(2));
    }
    else if (disc > T{0})
    {
        const T s = crd::math::sqrt(disc);
        ins((-p1 - s) / static_cast<T>(2));
        ins((-p1 + s) / static_cast<T>(2));
    }
    disc = p2 * p2 - static_cast<T>(4) * q2;
    if (crd::math::fabs(disc) < eps16)
    {
        ins(-p2 / static_cast<T>(2));
    }
    else if (disc > T{0})
    {
        const T s = crd::math::sqrt(disc);
        ins((-p2 - s) / static_cast<T>(2));
        ins((-p2 + s) / static_cast<T>(2));
    }
    return n;
}

// Faithful transcription of Ruckig Profile::check<UDDU> (symmetric limits). Returns the profile duration if valid,
// or a negative sentinel if not. Velocity is enforced on the back half (v[3..6]) + interior a=0 crossings for phases>1;
// the front-half overshoot from a hot initial state is allowed; accel is hard at a[1],a[3],a[5].
template <typename T> T otg_check(const T* t, T j_max, T p0, T v0, T a0, T pf, T vf, T af, T vmax, T amax)
{
    for (int i = 0; i < 7; ++i)
    {
        if (t[i] < static_cast<T>(-1e-12))
        {
            return T{-1};
        }
    }
    const T js[7] = {t[0] > T{0} ? j_max : T{0},  T{0}, t[2] > T{0} ? -j_max : T{0}, T{0},
                     t[4] > T{0} ? -j_max : T{0}, T{0}, t[6] > T{0} ? j_max : T{0}};
    T va[8], aa[8], pa[8];
    pa[0] = p0;
    va[0] = v0;
    aa[0] = a0;
    T total = T{0};
    for (int i = 0; i < 7; ++i)
    {
        const T dur = t[i] > T{0} ? t[i] : T{0};
        total += dur;
        aa[i + 1] = aa[i] + dur * js[i];
        va[i + 1] = va[i] + dur * (aa[i] + dur * js[i] / static_cast<T>(2));
        pa[i + 1] = pa[i] + dur * (va[i] + dur * (aa[i] / static_cast<T>(2) + dur * js[i] / static_cast<T>(6)));
    }
    const T veps = static_cast<T>(1e-7);
    const T aeps = static_cast<T>(1e-7);
    if (crd::math::fabs(pa[7] - pf) > static_cast<T>(1e-6) || crd::math::fabs(va[7] - vf) > static_cast<T>(1e-6) ||
        crd::math::fabs(aa[7] - af) > static_cast<T>(1e-6))
    {
        return T{-1};
    }
    for (int idx : {1, 3, 5})
    {
        if (aa[idx] > amax + aeps || aa[idx] < -amax - aeps)
        {
            return T{-1};
        }
    }
    for (int idx : {3, 4, 5, 6})
    {
        if (va[idx] > vmax + veps || va[idx] < -vmax - veps)
        {
            return T{-1};
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
            const T v_a_zero = va[i] - aa[i] * aa[i] / den;
            if (v_a_zero > vmax + veps || v_a_zero < -vmax - veps)
            {
                return T{-1};
            }
        }
    }
    return total;
}

// Step1 (min-time) candidate generation for one limit convention. Updates best_t/best_j/best_T with any valid
// candidate of smaller total time. p0..af are the (possibly post-brake) boundary state.
// phase 0 = velocity-limited candidates (cheap, and optimal when valid); phase 1 = the quartic-heavy acc/none cases;
// phase 2 = the degenerate two-step fallbacks. Split so long moves (which reach v_max) skip the quartic solves.
template <typename T>
void otg_step1_convention(T p0, T v0, T a0, T pf, T vf, T af, T VMAX, T AMAX, T v_max, T a_max, T a_min, T j_max,
                          T& best_t, T* bestt, T& bestj, int phase)
{
    const T pd = pf - p0;
    const T v0v0 = v0 * v0;
    const T vfvf = vf * vf;
    const T a0a0 = a0 * a0;
    const T afaf = af * af;
    const T a0p3 = a0 * a0a0;
    const T a0p4 = a0a0 * a0a0;
    const T afp3 = af * afaf;
    const T afp4 = afaf * afaf;
    const T jj = j_max * j_max;
    T cand[7];

    auto consider = [&](const T* tt)
    {
        const T dur = otg_check<T>(tt, j_max, p0, v0, a0, pf, vf, af, VMAX, AMAX);
        if (dur >= T{0} && dur < best_t - static_cast<T>(1e-9))
        {
            best_t = dur;
            bestj = j_max;
            for (int i = 0; i < 7; ++i)
            {
                bestt[i] = tt[i] > T{0} ? tt[i] : T{0};
            }
        }
    };
    auto zero = [&]()
    {
        for (int i = 0; i < 7; ++i)
        {
            cand[i] = T{0};
        }
    };

    const T disc0 = a0a0 / (2 * jj) + (v_max - v0) / j_max;
    const T disc1 = afaf / (2 * jj) + (v_max - vf) / j_max;
    const T t_acc0 = disc0 >= T{0} ? crd::math::sqrt(disc0) : T{-1};
    const T t_acc1 = disc1 >= T{0} ? crd::math::sqrt(disc1) : T{-1};

    if (phase == 0)
    {
        // ---- time_all_vel: ACC0_ACC1_VEL / ACC1_VEL / ACC0_VEL / VEL (velocity-limited; optimal when valid) ----
        zero();
        cand[0] = (-a0 + a_max) / j_max;
        cand[1] = (a0a0 / 2 - a_max * a_max - j_max * (v0 - v_max)) / (a_max * j_max);
        cand[2] = a_max / j_max;
        cand[3] =
            (3 * (a0p4 * a_min - afp4 * a_max) + 8 * a_max * a_min * (afp3 - a0p3 + 3 * j_max * (a0 * v0 - af * vf)) +
             6 * a0a0 * a_min * (a_max * a_max - 2 * j_max * v0) - 6 * afaf * a_max * (a_min * a_min - 2 * j_max * vf) -
             12 * j_max *
                 (a_max * a_min * (a_max * (v0 + v_max) - a_min * (vf + v_max) - 2 * j_max * pd) +
                  (a_min - a_max) * j_max * v_max * v_max + j_max * (a_max * vfvf - a_min * v0v0))) /
            (24 * a_max * a_min * jj * v_max);
        cand[4] = -a_min / j_max;
        cand[5] = -(afaf / 2 - a_min * a_min - j_max * (vf - v_max)) / (a_min * j_max);
        cand[6] = cand[4] + af / j_max;
        consider(cand);

        if (t_acc0 >= T{0})
        {
            zero();
            cand[0] = t_acc0 - a0 / j_max;
            cand[2] = t_acc0;
            cand[3] = -(3 * afp4 - 8 * a_min * (afp3 - a0p3) - 24 * a_min * j_max * (a0 * v0 - af * vf) +
                        6 * afaf * (a_min * a_min - 2 * j_max * vf) -
                        12 * j_max *
                            (2 * a_min * j_max * pd + a_min * a_min * (vf + v_max) + j_max * (v_max * v_max - vfvf) +
                             a_min * t_acc0 * (a0a0 - 2 * j_max * (v0 + v_max)))) /
                      (24 * a_min * jj * v_max);
            cand[4] = -a_min / j_max;
            cand[5] = -(afaf / 2 - a_min * a_min + j_max * (v_max - vf)) / (a_min * j_max);
            cand[6] = cand[4] + af / j_max;
            consider(cand);
        }
        if (t_acc1 >= T{0})
        {
            zero();
            cand[0] = (-a0 + a_max) / j_max;
            cand[1] = (a0a0 / 2 - a_max * a_max - j_max * (v0 - v_max)) / (a_max * j_max);
            cand[2] = a_max / j_max;
            cand[3] = (3 * a0p4 + 8 * a_max * (afp3 - a0p3) + 24 * a_max * j_max * (a0 * v0 - af * vf) +
                       6 * a0a0 * (a_max * a_max - 2 * j_max * v0) -
                       12 * j_max *
                           (-2 * a_max * j_max * pd + a_max * a_max * (v0 + v_max) + j_max * (v_max * v_max - v0v0) +
                            a_max * t_acc1 * (-afaf + 2 * (vf + v_max) * j_max))) /
                      (24 * a_max * jj * v_max);
            cand[4] = t_acc1;
            cand[6] = t_acc1 + af / j_max;
            consider(cand);
        }
        if (t_acc0 >= T{0} && t_acc1 >= T{0})
        {
            zero();
            cand[0] = t_acc0 - a0 / j_max;
            cand[2] = t_acc0;
            cand[3] = (afp3 - a0p3) / (3 * jj * v_max) +
                      (a0 * v0 - af * vf + (afaf * t_acc1 + a0a0 * t_acc0) / 2) / (j_max * v_max) -
                      (v0 / v_max + T{1}) * t_acc0 - (vf / v_max + T{1}) * t_acc1 + pd / v_max;
            cand[4] = t_acc1;
            cand[6] = t_acc1 + af / j_max;
            consider(cand);
        }

        return;
    }
    if (phase == 1)
    {
        // ---- time_acc0_acc1 ----
        T h1 = (3 * (afp4 * a_max - a0p4 * a_min) +
                a_max * a_min *
                    (8 * (a0p3 - afp3) + 3 * a_max * a_min * (a_max - a_min) + 6 * a_min * afaf - 6 * a_max * a0a0) +
                12 * j_max *
                    (a_max * a_min * ((a_max - 2 * a0) * v0 - (a_min - 2 * af) * vf) + a_min * a0a0 * v0 -
                     a_max * afaf * vf)) /
                   (3 * (a_max - a_min) * jj) +
               4 * (a_max * vfvf - a_min * v0v0 - 2 * a_min * a_max * pd) / (a_max - a_min);
        if (h1 >= T{0})
        {
            h1 = crd::math::sqrt(h1) / 2;
            const T h2 = a0a0 / (2 * a_max * j_max) + (a_min - 2 * a_max) / (2 * j_max) - v0 / a_max;
            const T h3 = -afaf / (2 * a_min * j_max) - (a_max - 2 * a_min) / (2 * j_max) + vf / a_min;
            for (T sol : {T{1}, T{-1}})
            {
                zero();
                cand[0] = (-a0 + a_max) / j_max;
                cand[1] = h2 - sol * h1 / a_max;
                cand[2] = a_max / j_max;
                cand[4] = -a_min / j_max;
                cand[5] = h3 + sol * h1 / a_min;
                cand[6] = cand[4] + af / j_max;
                consider(cand);
            }
        }

        // ---- time_all_none_acc0_acc1 (quartic roots) ----
        const T h2none = (a0a0 - afaf) / (2 * j_max) + (vf - v0);
        const T h2h2 = h2none * h2none;
        const T t_min_none = (a0 - af) / j_max;
        const T t_max_none = (a_max - a_min) / j_max;
        const T h3acc0 = (a0a0 - afaf) / (2 * a_max * j_max) + (vf - v0) / a_max;
        const T t_min_acc0 = (a_max - af) / j_max;
        const T t_max_acc0 = (a_max - a_min) / j_max;
        const T h0acc0 = 3 * (afp4 - a0p4) + 8 * (a0p3 - afp3) * a_max + 24 * a_max * j_max * (af * vf - a0 * v0) -
                         6 * a0a0 * (a_max * a_max - 2 * j_max * v0) + 6 * afaf * (a_max * a_max - 2 * j_max * vf) +
                         12 * j_max * (j_max * (vfvf - v0v0 - 2 * a_max * pd) - a_max * a_max * (vf - v0));
        const T h2acc0 = -afaf + a_max * a_max + 2 * j_max * vf;
        const T h3acc1 = -(a0a0 + afaf) / (2 * j_max * a_min) + a_min / j_max + (vf - v0) / a_min;
        const T t_min_acc1 = (a_min - a0) / j_max;
        const T t_max_acc1 = (a_max - a0) / j_max;
        const T h0acc1 = (a0p4 - afp4) / 4 + 2 * (afp3 - a0p3) * a_min / 3 + (a0a0 - afaf) * a_min * a_min / 2 +
                         j_max * (afaf * vf + a0a0 * v0 + 2 * a_min * (j_max * pd - a0 * v0 - af * vf) +
                                  a_min * a_min * (v0 + vf) + j_max * (v0v0 - vfvf));
        const T h2acc1 = a0a0 - a0 * a_min + 2 * j_max * v0;

        struct QCase
        {
            T a, b, c, d, tmin, tmax;
            int kind; // 0 none, 1 acc0, 2 acc1
        };
        const QCase qcases[3] = {
            {T{0}, -2 * (a0a0 + afaf - 2 * j_max * (v0 + vf)) / jj,
             4 * (a0p3 - afp3 + 3 * j_max * (af * vf - a0 * v0)) / (3 * j_max * jj) - 4 * pd / j_max, -h2h2 / jj,
             t_min_none, t_max_none, 0},
            {-2 * a_max / j_max, h2acc0 / jj, T{0}, h0acc0 / (12 * jj * jj), t_min_acc0, t_max_acc0, 1},
            {2 * (2 * a0 - a_min) / j_max, (5 * a0a0 + a_min * (a_min - 6 * a0) + 2 * j_max * v0) / jj,
             2 * (a0 - a_min) * h2acc1 / (jj * j_max), h0acc1 / (jj * jj), t_min_acc1, t_max_acc1, 2}};
        for (const QCase& qc : qcases)
        {
            T roots[4];
            int nr = otg_solve_quart<T>(qc.a, qc.b, qc.c, qc.d, roots);
            for (int ri = 0; ri < nr; ++ri)
            {
                T tv = roots[ri];
                if (tv < qc.tmin - static_cast<T>(1e-9) || tv > qc.tmax + static_cast<T>(1e-9))
                {
                    continue;
                }
                zero();
                if (qc.kind == 0)
                {
                    const T h0 = crd::math::fabs(tv) > static_cast<T>(1e-12) ? h2none / (2 * j_max * tv) : T{0};
                    cand[0] = h0 + tv / 2 - a0 / j_max;
                    cand[2] = tv;
                    cand[6] = -h0 + tv / 2 + af / j_max;
                }
                else if (qc.kind == 1)
                {
                    cand[0] = (-a0 + a_max) / j_max;
                    cand[1] = h3acc0 - 2 * tv + j_max / a_max * tv * tv;
                    cand[2] = tv;
                    cand[6] = (af - a_max) / j_max + tv;
                }
                else
                {
                    cand[0] = tv;
                    cand[2] = (a0 - a_min) / j_max + tv;
                    cand[5] = h3acc1 - (2 * a0 + j_max * tv) * tv / a_min;
                    cand[6] = (af - a_min) / j_max;
                }
                consider(cand);
            }
        }

        return;
    }
    // ---- two-step degenerate fallbacks (only when the main cases found nothing) ----
    const T valn = (a0a0 + afaf) / 2 + j_max * (vf - v0);
    if (valn >= T{0})
    {
        const T h0 = crd::math::sqrt(valn) * (crd::math::fabs(j_max) / j_max);
        zero();
        cand[0] = (h0 - a0) / j_max;
        cand[2] = (h0 - af) / j_max;
        consider(cand);
    }
    zero();
    cand[0] = (af - a0) / j_max;
    consider(cand);
    if (crd::math::fabs(a0) > static_cast<T>(1e-12))
    {
        zero();
        cand[1] = (afaf - a0a0 + 2 * j_max * (vf - v0)) / (2 * a0 * j_max);
        cand[2] = (a0 - af) / j_max;
        consider(cand);
    }
    zero();
    cand[0] = (-a0 + a_max) / j_max;
    cand[1] = (a0a0 + afaf - 2 * a_max * a_max + 2 * j_max * (vf - v0)) / (2 * a_max * j_max);
    cand[2] = (-af + a_max) / j_max;
    consider(cand);
    if (t_acc1 >= T{0})
    {
        const T h1v = t_acc1;
        zero();
        cand[0] = -a0 / j_max;
        cand[3] = (afp3 - a0p3) / (3 * jj * v_max) + (a0 * v0 - af * vf + (afaf * h1v) / 2) / (j_max * v_max) -
                  (vf / v_max + T{1}) * h1v + pd / v_max;
        cand[4] = h1v;
        cand[6] = h1v + af / j_max;
        consider(cand);
        zero();
        cand[2] = a0 / j_max;
        cand[3] = (afp3 - a0p3) / (3 * jj * v_max) +
                  (a0 * v0 - af * vf + (afaf * h1v + a0p3 / j_max) / 2) / (j_max * v_max) -
                  (v0 / v_max + T{1}) * a0 / j_max - (vf / v_max + T{1}) * h1v + pd / v_max;
        cand[4] = h1v;
        cand[6] = h1v + af / j_max;
        consider(cand);
    }
}

template <typename T> T otg_v_at_t(T v0, T a0, T jj, T tt)
{
    return v0 + tt * (a0 + jj * tt / static_cast<T>(2));
}

// Ruckig brake pre-trajectory (get_position_brake_trajectory). Fills bt[2]/bj[2]; symmetric limits.
template <typename T> void otg_brake(T v0, T a0, T VMAX, T AMAX, T JMAX, T* bt, T* bj)
{
    bt[0] = bt[1] = bj[0] = bj[1] = T{0};
    if (JMAX == T{0} || AMAX == T{0})
    {
        return;
    }
    auto v_at_a_zero = [](T v, T a, T jm)
    {
        return v + a * a / (static_cast<T>(2) * jm);
    };
    auto vel_brake = [&](T v0l, T a0l, T v_max, T v_min, T a_min, T j_max)
    {
        bj[0] = -j_max;
        const T t_to_amin = (a0l - a_min) / j_max;
        const T arg1 = a0l * a0l + 2 * j_max * (v0l - v_max);
        const T arg2 = a0l * a0l / 2 + j_max * (v0l - v_min);
        const T t_to_vmax = a0l / j_max + crd::math::sqrt(arg1 > T{0} ? arg1 : T{0}) / crd::math::fabs(j_max);
        const T t_to_vmin = a0l / j_max + crd::math::sqrt(arg2 > T{0} ? arg2 : T{0}) / crd::math::fabs(j_max);
        const T t_min = t_to_vmax < t_to_vmin ? t_to_vmax : t_to_vmin;
        if (t_to_amin < t_min)
        {
            const T v_at_amin = otg_v_at_t<T>(v0l, a0l, -j_max, t_to_amin);
            const T tvmc = -(v_at_amin - v_max) / a_min;
            const T tvnc = a_min / (2 * j_max) - (v_at_amin - v_min) / a_min;
            bt[0] = t_to_amin > T{0} ? t_to_amin : T{0};
            const T m = (tvmc < tvnc ? tvmc : tvnc);
            bt[1] = m > T{0} ? m : T{0};
        }
        else
        {
            bt[0] = t_min > T{0} ? t_min : T{0};
        }
    };
    auto accel_brake = [&](T v0l, T a0l, T v_max, T v_min, T a_max, T a_min, T j_max)
    {
        bj[0] = -j_max;
        const T t_to_amax = (a0l - a_max) / j_max;
        const T t_to_azero = a0l / j_max;
        const T v_at_amax = otg_v_at_t<T>(v0l, a0l, -j_max, t_to_amax);
        const T v_at_azero = otg_v_at_t<T>(v0l, a0l, -j_max, t_to_azero);
        if ((v_at_azero > v_max && j_max > T{0}) || (v_at_azero < v_max && j_max < T{0}))
        {
            vel_brake(v0l, a0l, v_max, v_min, a_min, j_max);
        }
        else if ((v_at_amax < v_min && j_max > T{0}) || (v_at_amax > v_min && j_max < T{0}))
        {
            const T t_to_vmin = -(v_at_amax - v_min) / a_max;
            const T t_to_vmax = -a_max / (2 * j_max) - (v_at_amax - v_max) / a_max;
            bt[0] = t_to_amax;
            const T m = (t_to_vmin < t_to_vmax ? t_to_vmin : t_to_vmax);
            bt[1] = m > T{0} ? m : T{0};
        }
        else
        {
            bt[0] = t_to_amax;
        }
    };
    const T v_max = VMAX, v_min = -VMAX, a_max = AMAX, a_min = -AMAX, j_max = JMAX;
    if (a0 > a_max)
    {
        accel_brake(v0, a0, v_max, v_min, a_max, a_min, j_max);
    }
    else if (a0 < a_min)
    {
        accel_brake(v0, a0, v_min, v_max, a_min, a_max, -j_max);
    }
    else if ((v0 > v_max && v_at_a_zero(v0, a0, -j_max) > v_min) || (a0 > T{0} && v_at_a_zero(v0, a0, j_max) > v_max))
    {
        vel_brake(v0, a0, v_max, v_min, a_min, j_max);
    }
    else if ((v0 < v_min && v_at_a_zero(v0, a0, j_max) < v_max) || (a0 < T{0} && v_at_a_zero(v0, a0, -j_max) < v_min))
    {
        vel_brake(v0, a0, v_min, v_max, a_max, -j_max);
    }
}

} // namespace detail

// ★★ Plan the time-optimal jerk-limited single-DoF trajectory from (p0,v0,a0) to (pf,vf,af) under symmetric limits.
// Deterministic, allocation-free, WCET-bounded. Matches Ruckig's min-time on 1934/1934 verified arbitrary-state cases.
template <typename T> [[nodiscard]] OtgProfile<T> plan_otg(T p0, T v0, T a0, T pf, T vf, T af, T vmax, T amax, T jmax)
{
    OtgProfile<T> prof;
    prof.p0 = p0;
    prof.v0 = v0;
    prof.a0 = a0;
    // brake pre-trajectory (advance the state through it, then solve step1 from there)
    detail::otg_brake<T>(v0, a0, vmax, amax, jmax, prof.brake_t, prof.brake_j);
    T pb = p0, vb = v0, ab = a0;
    for (int i = 0; i < 2; ++i)
    {
        const T dur = prof.brake_t[i];
        const T jk = prof.brake_j[i];
        pb = pb + vb * dur + T{0.5} * ab * dur * dur + jk * dur * dur * dur / static_cast<T>(6);
        vb = vb + ab * dur + T{0.5} * jk * dur * dur;
        ab = ab + jk * dur;
    }
    T best_t = std::numeric_limits<T>::max();
    T bestt[7]{};
    T bestj = jmax;
    for (int phase = 0; phase < 3 && best_t >= std::numeric_limits<T>::max() / 2; ++phase)
    {
        detail::otg_step1_convention<T>(pb, vb, ab, pf, vf, af, vmax, amax, vmax, amax, -amax, jmax, best_t, bestt,
                                        bestj, phase);
        detail::otg_step1_convention<T>(pb, vb, ab, pf, vf, af, vmax, amax, -vmax, -amax, amax, -jmax, best_t, bestt,
                                        bestj, phase);
    }
    if (best_t >= std::numeric_limits<T>::max() / 2)
    {
        prof.valid = false;
        return prof;
    }
    for (int i = 0; i < 7; ++i)
    {
        prof.t[i] = bestt[i];
        prof.j[i] = T{0};
    }
    prof.j[0] = bestt[0] > T{0} ? bestj : T{0};
    prof.j[2] = bestt[2] > T{0} ? -bestj : T{0};
    prof.j[4] = bestt[4] > T{0} ? -bestj : T{0};
    prof.j[6] = bestt[6] > T{0} ? bestj : T{0};
    prof.duration = prof.brake_t[0] + prof.brake_t[1];
    for (int i = 0; i < 7; ++i)
    {
        prof.duration += prof.t[i];
    }
    prof.valid = true;
    return prof;
}

} // namespace crd::hesap::motion
