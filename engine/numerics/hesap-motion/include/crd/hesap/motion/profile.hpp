#pragma once

// crd-hesap-motion v13-q — JERK-LIMITED motion profiles (single DoF, rest-to-rest): the trapezoidal (2nd-order,
// bounded acc/vel) and the ★S-CURVE (3rd-order, bounded JERK/acc/vel) time-optimal profiles. Bounding jerk is what
// removes the vibration / wear / tracking error that a discontinuous acceleration causes — the modern requirement for
// robot arms, CNC, gantries, camera dollies, elevators. This is the CORE of a Ruckig-class online generator: the
// planner computes the 7 phase durations in closed form (no iteration in the common case), then the state (pos, vel,
// acc, jerk) is reconstructed at any t with a bounded-WCET walk over ≤7 phases.
//
// Verified in python: reaches the target exactly and respects the jerk/acc/vel limits across the cruise / triangular /
// long-distance regimes. Gate = limit-conformance + target-reached + a numeric double-integrator cross-check.
// Moat: determinism (crd::math, fixed FP order, no data-dependent unbounded loops) + allocation-free + the WCET bound
// (≤ 7 phases). Full multi-DoF time-SYNCHRONIZATION (all axes finish together — the rest of Ruckig) is a follow-on.

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::motion
{

// A planned rest-to-rest jerk-limited profile: 7 phases [tj, ta, tj, tc, tj, ta, tj] with jerk ±jmax / 0. Build once,
// eval(t) many times (allocation-free).
template <typename T>
struct ScurveProfile
{
    T    p0    = T{0};
    T    sign  = T{1};  // direction (+1 if target ≥ start)
    T    tj    = T{0};  // jerk-phase duration
    T    ta    = T{0};  // constant-accel-phase duration
    T    tc    = T{0};  // cruise duration
    T    jmax  = T{0};  // jerk magnitude used (0 ⇒ trapezoidal / 2nd-order)
    T    amax  = T{0};  // acceleration magnitude used (the constant-accel level for the trapezoidal profile)
    T    vpeak = T{0};  // peak (cruise) velocity reached
    T    total = T{0};  // total motion time
    bool valid = false;

    // State at time t along the profile (clamped to [0, total]). Walks the ≤7 phases (bounded WCET).
    void eval(T t, T& pos, T& vel, T& acc, T& jerk) const noexcept
    {
        if (t < T{0})
        {
            t = T{0};
        }
        if (t > total)
        {
            t = total;
        }
        if (jmax == T{0}) // trapezoidal: 3 constant-accel phases [ta:+amax, tc:0, ta:−amax]
        {
            const T tdurs[3]  = {ta, tc, ta};
            const T tacc[3]   = {amax, T{0}, -amax};
            T       p = T{0};
            T       v = T{0};
            for (int ph = 0; ph < 3; ++ph)
            {
                const T d = tdurs[ph];
                const T a = tacc[ph];
                if (t <= d || ph == 2)
                {
                    const T dt = (t < d) ? t : d;
                    pos        = p0 + sign * (p + v * dt + T{0.5} * a * dt * dt);
                    vel        = sign * (v + a * dt);
                    acc        = sign * a;
                    jerk       = T{0};
                    if (t <= d)
                    {
                        return;
                    }
                }
                p += v * d + T{0.5} * a * d * d;
                v += a * d;
                t -= d;
            }
            return;
        }
        const T durs[7]  = {tj, ta, tj, tc, tj, ta, tj};
        const T jsign[7] = {T{1}, T{0}, T{-1}, T{0}, T{-1}, T{0}, T{1}};
        T       p    = T{0};
        T       v    = T{0};
        T       a    = T{0};
        T       jcur = T{0};
        for (int ph = 0; ph < 7; ++ph)
        {
            const T d = durs[ph];
            const T j = jsign[ph] * jmax;
            if (t <= d || ph == 6)
            {
                const T dt = (t < d) ? t : d;
                jcur       = j;
                pos        = p0 + sign * (p + v * dt + T{0.5} * a * dt * dt + j * dt * dt * dt / static_cast<T>(6));
                vel        = sign * (v + a * dt + T{0.5} * j * dt * dt);
                acc        = sign * (a + j * dt);
                jerk       = sign * jcur;
                if (t <= d)
                {
                    return;
                }
            }
            // advance to the end of this phase
            p += v * d + T{0.5} * a * d * d + j * d * d * d / static_cast<T>(6);
            v += a * d + T{0.5} * j * d * d;
            a += j * d;
            t -= d;
        }
    }
};

// Plan a rest-to-rest S-curve (jerk-limited) profile from p0 to pT with the given limits (all > 0).
template <typename T>
[[nodiscard]] ScurveProfile<T> plan_scurve(T p0, T pT, T vmax, T amax, T jmax)
{
    ScurveProfile<T> pr;
    if (!(vmax > T{0}) || !(amax > T{0}) || !(jmax > T{0}))
    {
        return pr;
    }
    const T dist = crd::math::fabs(pT - p0);
    pr.p0        = p0;
    pr.sign      = (pT >= p0) ? T{1} : T{-1};
    pr.jmax      = jmax;
    if (dist < static_cast<T>(1e-18))
    {
        pr.valid = true;
        return pr;
    }
    T tj = amax / jmax;
    T ta;
    T vpk = vmax;
    if (vmax < amax * tj) // amax never reached (accel is triangular): recompute tj from vmax
    {
        tj = crd::math::sqrt(vmax / jmax);
        ta = T{0};
    }
    else
    {
        ta = vmax / amax - tj;
    }
    // displacement to accelerate 0->vmax over (2tj+ta): avg vel vmax/2 => vmax*(tj + ta/2). Full accel+decel = 2×.
    T d_acc = vpk * (tj + ta / T{2});
    T tc;
    if (dist < T{2} * d_acc) // vmax not reached: bisect for the peak velocity, no cruise
    {
        T lo = T{0};
        T hi = vmax;
        for (int it = 0; it < 100; ++it)
        {
            const T v = T{0.5} * (lo + hi);
            T       tjx;
            T       tax;
            if (v < amax * (amax / jmax))
            {
                tjx = crd::math::sqrt(v / jmax);
                tax = T{0};
            }
            else
            {
                tjx = amax / jmax;
                tax = v / amax - tjx;
            }
            const T dacc = v * (tjx + tax / T{2});
            if (T{2} * dacc < dist)
            {
                lo = v;
            }
            else
            {
                hi = v;
            }
        }
        vpk = lo;
        if (vpk < amax * (amax / jmax))
        {
            tj = crd::math::sqrt(vpk / jmax);
            ta = T{0};
        }
        else
        {
            tj = amax / jmax;
            ta = vpk / amax - tj;
        }
        tc = T{0};
    }
    else
    {
        tc = (dist - T{2} * d_acc) / vpk;
    }
    pr.tj    = tj;
    pr.ta    = ta;
    pr.tc    = tc;
    pr.total = T{4} * tj + T{2} * ta + tc;
    pr.valid = true;
    return pr;
}

// Plan a rest-to-rest trapezoidal (2nd-order, jerk unbounded) profile — the jmax→∞ limit of the S-curve (tj→0). Bounded
// acceleration + velocity only. Faster but with a discontinuous acceleration (the reason the S-curve exists).
template <typename T>
[[nodiscard]] ScurveProfile<T> plan_trapezoidal(T p0, T pT, T vmax, T amax)
{
    ScurveProfile<T> pr;
    if (!(vmax > T{0}) || !(amax > T{0}))
    {
        return pr;
    }
    const T dist = crd::math::fabs(pT - p0);
    pr.p0        = p0;
    pr.sign      = (pT >= p0) ? T{1} : T{-1};
    pr.jmax      = T{0};
    if (dist < static_cast<T>(1e-18))
    {
        pr.valid = true;
        return pr;
    }
    // Model the trapezoid as an S-curve with tj=0: accel phase ta = vmax/amax reaching vmax; d_acc = vmax*ta/2 = 0.5 vmax²/amax.
    T ta  = vmax / amax;
    T vpk = vmax;
    T d_acc = T{0.5} * vpk * ta;
    T tc;
    if (dist < T{2} * d_acc) // triangular (vmax not reached): vpk = sqrt(dist·amax)
    {
        vpk = crd::math::sqrt(dist * amax);
        ta  = vpk / amax;
        tc  = T{0};
    }
    else
    {
        tc = (dist - T{2} * d_acc) / vpk;
    }
    pr.tj    = T{0};
    pr.ta    = ta;
    pr.tc    = tc;
    pr.amax  = amax;
    pr.vpeak = vpk;
    pr.total = T{2} * ta + tc;
    pr.valid = true;
    return pr;
}

// Minimum rest-to-rest jerk-limited duration for one DoF (the time of plan_scurve). 0 for a zero move.
template <typename T>
[[nodiscard]] T scurve_duration(T p0, T pT, T vmax, T amax, T jmax)
{
    const ScurveProfile<T> pr = plan_scurve<T>(p0, pT, vmax, amax, jmax);
    return pr.valid ? pr.total : T{0};
}

// ★★Multi-DoF TIME-SYNCHRONIZED jerk-limited OTG (the Ruckig rest-to-rest core): plan one S-curve per DoF and re-time
// them so ALL axes finish at the SAME instant tsync = max_d(T_min[d]) — the essence of a modern online time-optimal
// generator (robot/drone multi-axis moves must arrive together to trace a straight Cartesian path). The critical DoF
// (longest T_min) keeps its limits; each other DoF's peak velocity is reduced (binary search, a bounded fixed-iteration
// loop — deterministic + WCET) so its duration equals tsync. Matches ruckig's rest-to-rest `.duration` exactly.
// Fills out[0..ndof-1] with the re-timed per-DoF profiles; returns tsync. Rest-to-rest (v0=a0=vf=af=0); arbitrary
// non-zero boundary states are the full-Ruckig extension (7 profile cases) noted for a follow-on.
template <typename T>
[[nodiscard]] T plan_synchronized(int ndof, const T* p0, const T* pT, const T* vmax, const T* amax, const T* jmax,
                                  ScurveProfile<T>* out)
{
    T tsync = T{0};
    for (int d = 0; d < ndof; ++d)
    {
        const T tmin = scurve_duration<T>(p0[d], pT[d], vmax[d], amax[d], jmax[d]);
        if (tmin > tsync)
        {
            tsync = tmin;
        }
    }
    for (int d = 0; d < ndof; ++d)
    {
        if (crd::math::fabs(pT[d] - p0[d]) < static_cast<T>(1e-15) || tsync <= T{0})
        {
            out[d]       = plan_scurve<T>(p0[d], pT[d], vmax[d], amax[d], jmax[d]);
            out[d].total = tsync; // a stationary DoF just idles until tsync
            continue;
        }
        // binary-search the reduced peak velocity that makes this DoF's duration == tsync (duration ↓ as veff ↑).
        T lo = static_cast<T>(1e-9);
        T hi = vmax[d];
        for (int it = 0; it < 200; ++it)
        {
            const T ve = static_cast<T>(0.5) * (lo + hi);
            const T t  = scurve_duration<T>(p0[d], pT[d], ve, amax[d], jmax[d]);
            if (t > tsync)
            {
                lo = ve; // too slow ⇒ raise the velocity cap
            }
            else
            {
                hi = ve;
            }
        }
        out[d]       = plan_scurve<T>(p0[d], pT[d], lo, amax[d], jmax[d]);
        out[d].total = tsync;
    }
    return tsync;
}

} // namespace crd::hesap::motion
