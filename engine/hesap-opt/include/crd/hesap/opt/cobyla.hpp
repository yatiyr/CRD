#pragma once

// cobyla.hpp — Phase 3.1.6 v7-p-2: COBYLA (Powell 1992) — Constrained Optimization BY Linear Approximations:
// derivative-free minimization of f(x) s.t. c_k(x) ≥ 0 and lb ≤ x ≤ ub, by linear interpolation models over a
// simplex of N+1 points, the trust-region LP subproblem `trstlp` (two stages: feasibility then objective, via
// Gram-Schmidt/Givens active-set updates), and the ρ (trust radius) + PARMU (merit penalty) schedule.
//
// ⚠ FAITHFUL PORT of the NLopt C reference (`nlopt/src/algs/cobyla/cobyla.c`, MIT; itself Jean-Sebastien Roy's
// C translation of Powell's Fortran COBYLA2 with Steven G. Johnson's documented modifications: explicit bound
// handling [ENFORCE_BOUNDS], the deterministic-LCG simplex perturbation, the SAS-suggested ρ-increase rule, and
// the nlopt_stopping convergence plumbing). Ported line-for-line in the f2c idiom — 1-BASED pointer
// adjustments, the original goto control flow, the EXACT float-literal artifacts (`.1f`/`.2f`/`1e-6f` keep
// their float-then-promote values) — per the L-BFGS-B playbook: re-deriving is the bug farm; the differential
// harness vs the compiled reference adjudicates (runtime/examples/cobyla_difftest.cpp, CRD_BUILD_HESAP_VS_COBYLA).
// Stop semantics mirrored from nlopt stop.c (relstop with the inf guard); force-stop and wall-clock stops are
// NOT carried (no async stop in Cerid; named delta). iprint output stripped (no numerics in it). ADR-0090.
//
// DETERMINISM: serial, fixed evaluation order; the SGJ simplex perturbation uses the reference's own LCG seeded
// with (n + m) — deterministic by construction ⇒ bit-identical runs.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/constraints.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::opt
{

template <typename T> struct CobylaOptions
{
    T rhobeg = static_cast<T>(1);    // initial trust-region radius (initial variable changes)
    T rhoend = static_cast<T>(1e-8); // final radius = the x-accuracy target
    T ftol_rel = static_cast<T>(0);  // relative f-convergence (0 = off; nlopt semantics)
    T ftol_abs = static_cast<T>(0);  // absolute f-convergence (0 = off)
    crd::usize max_evals = 0;        // function-evaluation cap; 0 ⇒ 1000·(n+1)
};

namespace detail::cobyla_impl
{

// Mirrors the nlopt_result values this code path can produce.
enum class Rc : crd::i8
{
    RoundoffLimited = -4, // NLOPT_ROUNDOFF_LIMITED (value matched to nlopt.h for the diff harness)
    Success = 1,           // NLOPT_SUCCESS
    MinfMaxReached = 2,    // NLOPT_MINF_MAX_REACHED
    FtolReached = 3,       // NLOPT_FTOL_REACHED
    XtolReached = 4,       // NLOPT_XTOL_REACHED
    MaxevalReached = 5,    // NLOPT_MAXEVAL_REACHED
};

// The nlopt_stopping subset cobylb consults (no force-stop / wall-clock — named in the header note).
template <typename T> struct Stop
{
    int nevals = 0;
    int maxeval = 0;
    T minf_max = -std::numeric_limits<T>::infinity();
    T ftol_rel = static_cast<T>(0);
    T ftol_abs = static_cast<T>(0);
};

// nlopt stop.c `relstop` verbatim (incl. the isinf guard and the vnew==vold catch).
template <typename T> [[nodiscard]] inline bool relstop(T vold, T vnew, T reltol, T abstol) noexcept
{
    if (std::isinf(vold))
    {
        return false;
    }
    return std::fabs(vnew - vold) < abstol ||
           std::fabs(vnew - vold) < reltol * (std::fabs(vnew) + std::fabs(vold)) * static_cast<T>(0.5) ||
           (reltol > static_cast<T>(0) && vnew == vold);
}

template <typename T> [[nodiscard]] inline bool stop_ftol(const Stop<T>& s, T f, T oldf) noexcept
{
    return relstop<T>(oldf, f, s.ftol_rel, s.ftol_abs);
}

template <typename T> [[nodiscard]] inline bool stop_evals(const Stop<T>& s) noexcept
{
    return s.maxeval > 0 && s.nevals >= s.maxeval;
}

// The reference's deterministic LCG (SGJ simplex perturbation).
inline crd::u32 lcg_rand(crd::u32* seed) noexcept
{
    return (*seed = *seed * 1103515245U + 12345U);
}

template <typename T> [[nodiscard]] inline T lcg_urand(crd::u32* seed, T a, T b) noexcept
{
    return a + static_cast<T>(lcg_rand(seed)) * (b - a) / static_cast<T>(static_cast<crd::u32>(-1));
}

// ----------------------------------------------------------------------------------------------- trstlp
// Stage 1: DX = the shortest vector minimizing the greatest violation of A(:,k)ᵀDX ≥ B(k) within ‖DX‖ ≤ ρ;
// stage 2 (mcon > m): spend any remaining freedom minimizing −A(:,m+1)ᵀDX without increasing the violation.
// Active set via an orthogonal Z (Gram-Schmidt/Givens); IFULL=0 iff degeneracy stopped DX short of ρ.
// Signature and 1-based adjustments mirror the reference exactly (the per-routine diff-harness target).
template <typename T>
[[nodiscard]] inline Rc trstlp(const int* n, const int* m, const T* a, const T* b, const T* rho, T* dx, int* ifull,
                               int* iact, T* z__, T* zdota, T* vmultc, T* sdirn, T* dxnew, T* vmultd)
{
    /* System generated locals */
    int a_dim1, a_offset, z_dim1, z_offset, i__1, i__2;
    T d__1, d__2;

    /* Local variables */
    T alpha, tempa;
    T beta;
    T optnew, stpful, sum, tot, acca, accb;
    T ratio, vsave, zdotv, zdotw, dd;
    T sd;
    T sp, ss, resold = static_cast<T>(0), zdvabs, zdwabs, sumabs, resmax, optold;
    T spabs;
    T temp, step;
    int icount;
    int i__, j, k;
    int isave;
    int kk;
    int kl, kp, kw;
    int nact, icon = 0, mcon;
    int nactx = 0;

    /* Parameter adjustments */
    z_dim1 = *n;
    z_offset = 1 + z_dim1 * 1;
    z__ -= z_offset;
    a_dim1 = *n;
    a_offset = 1 + a_dim1 * 1;
    a -= a_offset;
    --b;
    --dx;
    --iact;
    --zdota;
    --vmultc;
    --sdirn;
    --dxnew;
    --vmultd;

    /* Function Body */
    *ifull = 1;
    mcon = *m;
    nact = 0;
    resmax = static_cast<T>(0);
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
        {
            z__[i__ + j * z_dim1] = static_cast<T>(0);
        }
        z__[i__ + i__ * z_dim1] = static_cast<T>(1);
        dx[i__] = static_cast<T>(0);
    }
    if (*m >= 1)
    {
        i__1 = *m;
        for (k = 1; k <= i__1; ++k)
        {
            if (b[k] > resmax)
            {
                resmax = b[k];
                icon = k;
            }
        }
        i__1 = *m;
        for (k = 1; k <= i__1; ++k)
        {
            iact[k] = k;
            vmultc[k] = resmax - b[k];
        }
    }
    if (resmax == static_cast<T>(0))
    {
        goto L480;
    }
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        sdirn[i__] = static_cast<T>(0);
    }

/* End the current stage if 3 consecutive iterations have neither reduced the best objective value nor grown
   the active set (the reference's anti-cycling rule). */
L60:
    optold = static_cast<T>(0);
    icount = 0;
L70:
    if (mcon == *m)
    {
        optnew = resmax;
    }
    else
    {
        optnew = static_cast<T>(0);
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            optnew -= dx[i__] * a[i__ + mcon * a_dim1];
        }
    }
    if (icount == 0 || optnew < optold)
    {
        optold = optnew;
        nactx = nact;
        icount = 3;
    }
    else if (nact > nactx)
    {
        nactx = nact;
        icount = 3;
    }
    else
    {
        --icount;
        if (icount == 0)
        {
            goto L490;
        }
    }

    /* Add constraint iact[icon] to the active set when icon > nact (Givens rotations keep the trailing
       columns of Z orthogonal to the new gradient; rounding-suspect scalar products forced to zero). */
    if (icon <= nact)
    {
        goto L260;
    }
    kk = iact[icon];
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        dxnew[i__] = a[i__ + kk * a_dim1];
    }
    tot = static_cast<T>(0);
    k = *n;
L100:
    if (k > nact)
    {
        sp = static_cast<T>(0);
        spabs = static_cast<T>(0);
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            temp = z__[i__ + k * z_dim1] * dxnew[i__];
            sp += temp;
            spabs += std::fabs(temp);
        }
        acca = spabs + std::fabs(sp) * static_cast<T>(.1);
        accb = spabs + std::fabs(sp) * static_cast<T>(.2);
        if (spabs >= acca || acca >= accb)
        {
            sp = static_cast<T>(0);
        }
        if (tot == static_cast<T>(0))
        {
            tot = sp;
        }
        else
        {
            kp = k + 1;
            temp = std::sqrt(sp * sp + tot * tot);
            alpha = sp / temp;
            beta = tot / temp;
            tot = temp;
            i__1 = *n;
            for (i__ = 1; i__ <= i__1; ++i__)
            {
                temp = alpha * z__[i__ + k * z_dim1] + beta * z__[i__ + kp * z_dim1];
                z__[i__ + kp * z_dim1] = alpha * z__[i__ + kp * z_dim1] - beta * z__[i__ + k * z_dim1];
                z__[i__ + k * z_dim1] = temp;
            }
        }
        --k;
        goto L100;
    }

    /* Add the new constraint if no deletion is needed. */
    if (tot != static_cast<T>(0))
    {
        ++nact;
        zdota[nact] = tot;
        vmultc[icon] = vmultc[nact];
        vmultc[nact] = static_cast<T>(0);
        goto L210;
    }

    /* The new gradient is a linear combination of the active ones: find the multiplier-ratio constraint to
       delete (branch out if none qualifies). */
    ratio = static_cast<T>(-1);
    k = nact;
L130:
    zdotv = static_cast<T>(0);
    zdvabs = static_cast<T>(0);
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        temp = z__[i__ + k * z_dim1] * dxnew[i__];
        zdotv += temp;
        zdvabs += std::fabs(temp);
    }
    acca = zdvabs + std::fabs(zdotv) * static_cast<T>(.1);
    accb = zdvabs + std::fabs(zdotv) * static_cast<T>(.2);
    if (zdvabs < acca && acca < accb)
    {
        temp = zdotv / zdota[k];
        if (temp > static_cast<T>(0) && iact[k] <= *m)
        {
            tempa = vmultc[k] / temp;
            if (ratio < static_cast<T>(0) || tempa < ratio)
            {
                ratio = tempa;
            }
        }
        if (k >= 2)
        {
            kw = iact[k];
            i__1 = *n;
            for (i__ = 1; i__ <= i__1; ++i__)
            {
                dxnew[i__] -= temp * a[i__ + kw * a_dim1];
            }
        }
        vmultd[k] = temp;
    }
    else
    {
        vmultd[k] = static_cast<T>(0);
    }
    --k;
    if (k > 0)
    {
        goto L130;
    }
    if (ratio < static_cast<T>(0))
    {
        goto L490;
    }

    /* Revise the multipliers; rotate the constraint to be replaced to the end of the active list. */
    i__1 = nact;
    for (k = 1; k <= i__1; ++k)
    {
        d__1 = static_cast<T>(0);
        d__2 = vmultc[k] - ratio * vmultd[k];
        vmultc[k] = d__1 >= d__2 ? d__1 : d__2;
    }
    if (icon < nact)
    {
        isave = iact[icon];
        vsave = vmultc[icon];
        k = icon;
    L170:
        kp = k + 1;
        kw = iact[kp];
        sp = static_cast<T>(0);
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            sp += z__[i__ + k * z_dim1] * a[i__ + kw * a_dim1];
        }
        d__1 = zdota[kp];
        temp = std::sqrt(sp * sp + d__1 * d__1);
        alpha = zdota[kp] / temp;
        beta = sp / temp;
        zdota[kp] = alpha * zdota[k];
        zdota[k] = temp;
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            temp = alpha * z__[i__ + kp * z_dim1] + beta * z__[i__ + k * z_dim1];
            z__[i__ + kp * z_dim1] = alpha * z__[i__ + k * z_dim1] - beta * z__[i__ + kp * z_dim1];
            z__[i__ + k * z_dim1] = temp;
        }
        iact[k] = kw;
        vmultc[k] = vmultc[kp];
        k = kp;
        if (k < nact)
        {
            goto L170;
        }
        iact[k] = isave;
        vmultc[k] = vsave;
    }
    temp = static_cast<T>(0);
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        temp += z__[i__ + nact * z_dim1] * a[i__ + kk * a_dim1];
    }
    if (temp == static_cast<T>(0))
    {
        goto L490;
    }
    zdota[nact] = temp;
    vmultc[icon] = static_cast<T>(0);
    vmultc[nact] = ratio;

/* Keep the objective as the LAST active constraint when mcon > m. */
L210:
    iact[icon] = iact[nact];
    iact[nact] = kk;
    if (mcon > *m && kk != mcon)
    {
        k = nact - 1;
        sp = static_cast<T>(0);
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            sp += z__[i__ + k * z_dim1] * a[i__ + kk * a_dim1];
        }
        d__1 = zdota[nact];
        temp = std::sqrt(sp * sp + d__1 * d__1);
        alpha = zdota[nact] / temp;
        beta = sp / temp;
        zdota[nact] = alpha * zdota[k];
        zdota[k] = temp;
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            temp = alpha * z__[i__ + nact * z_dim1] + beta * z__[i__ + k * z_dim1];
            z__[i__ + nact * z_dim1] = alpha * z__[i__ + k * z_dim1] - beta * z__[i__ + nact * z_dim1];
            z__[i__ + k * z_dim1] = temp;
        }
        iact[nact] = iact[k];
        iact[k] = kk;
        temp = vmultc[k];
        vmultc[k] = vmultc[nact];
        vmultc[nact] = temp;
    }

    /* Stage 1: sdirn = the direction reducing all active violations by one simultaneously. */
    if (mcon > *m)
    {
        goto L320;
    }
    kk = iact[nact];
    temp = static_cast<T>(0);
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        temp += sdirn[i__] * a[i__ + kk * a_dim1];
    }
    temp += static_cast<T>(-1);
    temp /= zdota[nact];
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        sdirn[i__] -= temp * z__[i__ + nact * z_dim1];
    }
    goto L340;

/* Delete constraint iact[icon] from the active set. */
L260:
    if (icon < nact)
    {
        isave = iact[icon];
        vsave = vmultc[icon];
        k = icon;
    L270:
        kp = k + 1;
        kk = iact[kp];
        sp = static_cast<T>(0);
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            sp += z__[i__ + k * z_dim1] * a[i__ + kk * a_dim1];
        }
        d__1 = zdota[kp];
        temp = std::sqrt(sp * sp + d__1 * d__1);
        alpha = zdota[kp] / temp;
        beta = sp / temp;
        zdota[kp] = alpha * zdota[k];
        zdota[k] = temp;
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            temp = alpha * z__[i__ + kp * z_dim1] + beta * z__[i__ + k * z_dim1];
            z__[i__ + kp * z_dim1] = alpha * z__[i__ + k * z_dim1] - beta * z__[i__ + kp * z_dim1];
            z__[i__ + k * z_dim1] = temp;
        }
        iact[k] = kk;
        vmultc[k] = vmultc[kp];
        k = kp;
        if (k < nact)
        {
            goto L270;
        }
        iact[k] = isave;
        vmultc[k] = vsave;
    }
    --nact;
    if (mcon > *m)
    {
        goto L320;
    }
    temp = static_cast<T>(0);
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        temp += sdirn[i__] * z__[i__ + (nact + 1) * z_dim1];
    }
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        sdirn[i__] -= temp * z__[i__ + (nact + 1) * z_dim1];
    }
    goto L340;

/* Stage 2 search direction. */
L320:
    temp = static_cast<T>(1) / zdota[nact];
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        sdirn[i__] = temp * z__[i__ + nact * z_dim1];
    }

/* Step to the trust-region boundary, or the step zeroing RESMAX. The 1e-6 factors guard harmless underflows
   (reference comment); float-literal artifacts kept VERBATIM for the bit-exact diff vs the oracle. */
L340:
    dd = *rho * *rho;
    sd = static_cast<T>(0);
    ss = static_cast<T>(0);
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        d__1 = dx[i__];
        if (std::fabs(d__1) >= *rho * static_cast<T>(1e-6F))
        {
            d__2 = dx[i__];
            dd -= d__2 * d__2;
        }
        sd += dx[i__] * sdirn[i__];
        d__1 = sdirn[i__];
        ss += d__1 * d__1;
    }
    if (dd <= static_cast<T>(0))
    {
        goto L490;
    }
    temp = std::sqrt(ss * dd);
    if (std::fabs(sd) >= temp * static_cast<T>(1e-6F))
    {
        temp = std::sqrt(ss * dd + sd * sd);
    }
    stpful = dd / (temp + sd);
    step = stpful;
    if (mcon == *m)
    {
        acca = step + resmax * static_cast<T>(.1);
        accb = step + resmax * static_cast<T>(.2);
        if (step >= acca || acca >= accb)
        {
            goto L480;
        }
        step = step <= resmax ? step : resmax;
    }
    if (std::isinf(step))
    {
        return Rc::RoundoffLimited; // SGJ 2010 error check
    }

    /* DXNEW = trial variables; stage 1 also tightens RESMAX to the active-set residual max. */
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        dxnew[i__] = dx[i__] + step * sdirn[i__];
    }
    if (mcon == *m)
    {
        resold = resmax;
        resmax = static_cast<T>(0);
        i__1 = nact;
        for (k = 1; k <= i__1; ++k)
        {
            kk = iact[k];
            temp = b[kk];
            i__2 = *n;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                temp -= a[i__ + kk * a_dim1] * dxnew[i__];
            }
            resmax = resmax >= temp ? resmax : temp;
        }
    }

    /* VMULTD = the multipliers VMULTC would take at DXNEW (rounding-suspect values forced to zero). */
    k = nact;
L390:
    zdotw = static_cast<T>(0);
    zdwabs = static_cast<T>(0);
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        temp = z__[i__ + k * z_dim1] * dxnew[i__];
        zdotw += temp;
        zdwabs += std::fabs(temp);
    }
    acca = zdwabs + std::fabs(zdotw) * static_cast<T>(.1);
    accb = zdwabs + std::fabs(zdotw) * static_cast<T>(.2);
    if (zdwabs >= acca || acca >= accb)
    {
        zdotw = static_cast<T>(0);
    }
    vmultd[k] = zdotw / zdota[k];
    if (k >= 2)
    {
        kk = iact[k];
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            dxnew[i__] -= vmultd[k] * a[i__ + kk * a_dim1];
        }
        --k;
        goto L390;
    }
    if (mcon > *m)
    {
        d__1 = static_cast<T>(0);
        d__2 = vmultd[nact];
        vmultd[nact] = d__1 >= d__2 ? d__1 : d__2;
    }

    /* Complete VMULTD with the new INACTIVE-constraint residuals. */
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        dxnew[i__] = dx[i__] + step * sdirn[i__];
    }
    if (mcon > nact)
    {
        kl = nact + 1;
        i__1 = mcon;
        for (k = kl; k <= i__1; ++k)
        {
            kk = iact[k];
            sum = resmax - b[kk];
            d__1 = b[kk];
            sumabs = resmax + std::fabs(d__1);
            i__2 = *n;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                temp = a[i__ + kk * a_dim1] * dxnew[i__];
                sum += temp;
                sumabs += std::fabs(temp);
            }
            acca = sumabs + std::fabs(sum) * static_cast<T>(.1F);
            accb = sumabs + std::fabs(sum) * static_cast<T>(.2F);
            if (sumabs >= acca || acca >= accb)
            {
                sum = static_cast<T>(0);
            }
            vmultd[k] = sum;
        }
    }

    /* The fraction of the DX → DXNEW step that keeps every multiplier nonnegative. */
    ratio = static_cast<T>(1);
    icon = 0;
    i__1 = mcon;
    for (k = 1; k <= i__1; ++k)
    {
        if (vmultd[k] < static_cast<T>(0))
        {
            temp = vmultc[k] / (vmultc[k] - vmultd[k]);
            if (temp < ratio)
            {
                ratio = temp;
                icon = k;
            }
        }
    }

    /* Update DX, VMULTC, RESMAX. */
    temp = static_cast<T>(1) - ratio;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        dx[i__] = temp * dx[i__] + ratio * dxnew[i__];
    }
    i__1 = mcon;
    for (k = 1; k <= i__1; ++k)
    {
        d__1 = static_cast<T>(0);
        d__2 = temp * vmultc[k] + ratio * vmultd[k];
        vmultc[k] = d__1 >= d__2 ? d__1 : d__2;
    }
    if (mcon == *m)
    {
        resmax = resold + ratio * (resmax - resold);
    }

    /* Partial step ⇒ iterate; full step ⇒ stage 2 or done. */
    if (icon > 0)
    {
        goto L70;
    }
    if (step == stpful)
    {
        goto L500;
    }
L480:
    mcon = *m + 1;
    icon = mcon;
    iact[mcon] = mcon;
    vmultc[mcon] = static_cast<T>(0);
    goto L60;

L490:
    if (mcon == *m)
    {
        goto L480;
    }
    *ifull = 0;
L500:
    return Rc::Success;
} /* trstlp */

// ----------------------------------------------------------------------------------------------- cobylb
// The main COBYLA driver, ported with the original goto control flow (restructuring the Fortran-66 spaghetti
// IS the bug farm — reference comment included). `Calcfc` is `bool calcfc(int n, int m, const T* x, T& f,
// T* con)` over 0-based arrays, returning true to request a stop (never, in Cerid).
template <typename T, typename Calcfc>
[[nodiscard]] inline Rc cobylb(const int* n, const int* m, const int* mpp, T* x, T* minf, const T* rhobeg, T rhoend,
                               Stop<T>* stop, const T* lb, const T* ub, T* con, T* sim, T* simi, T* datmat, T* a,
                               T* vsig, T* veta, T* sigbar, T* dx, T* w, int* iact, const T* con_tol, Calcfc&& calcfc)
{
    /* System generated locals */
    int sim_dim1, sim_offset, simi_dim1, simi_offset, datmat_dim1, datmat_offset, a_dim1, a_offset, i__1, i__2, i__3;
    T d__1, d__2;

    /* Local variables */
    T alpha, delta, denom, tempa, barmu;
    T beta, cmin = static_cast<T>(0), cmax = static_cast<T>(0);
    T cvmaxm, dxsign, prerem = static_cast<T>(0);
    T edgmax, pareta, prerec = static_cast<T>(0), phimin, parsig = static_cast<T>(0);
    T gamma_;
    T phi, rho, sum = static_cast<T>(0);
    T ratio, vmold, parmu, error, vmnew;
    T resmax, cvmaxp;
    T resnew, trured;
    T temp, wsig, f;
    T weta;
    int i__, j, k, l;
    int idxnew;
    int iflag = 0;
    int isdirn, izdota;
    int ivmc;
    int ivmd;
    int mp, np, iz, ibrnch;
    int nbest, ifull = 0, jdrop;
    Rc rc = Rc::Success;
    crd::u32 seed = static_cast<crd::u32>(*n + *m); /* arbitrary deterministic LCG seed */
    int feasible;

    /* SGJ, 2008: track the minimum feasible function value. */
    *minf = std::numeric_limits<T>::infinity();

    /* Parameter adjustments (SIM's last column = the optimal vertex; the first N columns = displacements to
       the other vertices; SIMI = the inverse of SIM's leading N×N block). */
    a_dim1 = *n;
    a_offset = 1 + a_dim1 * 1;
    a -= a_offset;
    simi_dim1 = *n;
    simi_offset = 1 + simi_dim1 * 1;
    simi -= simi_offset;
    sim_dim1 = *n;
    sim_offset = 1 + sim_dim1 * 1;
    sim -= sim_offset;
    datmat_dim1 = *mpp;
    datmat_offset = 1 + datmat_dim1 * 1;
    datmat -= datmat_offset;
    --x;
    --con;
    --vsig;
    --veta;
    --sigbar;
    --dx;
    --w;
    --iact;
    --lb;
    --ub;

    /* Function Body */
    np = *n + 1;
    mp = *m + 1;
    alpha = static_cast<T>(.25);
    beta = static_cast<T>(2.1);
    gamma_ = static_cast<T>(.5);
    delta = static_cast<T>(1.1);
    rho = *rhobeg;
    parmu = static_cast<T>(0);
    temp = static_cast<T>(1) / rho;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        T rhocur;
        sim[i__ + np * sim_dim1] = x[i__];
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
        {
            sim[i__ + j * sim_dim1] = static_cast<T>(0);
            simi[i__ + j * simi_dim1] = static_cast<T>(0);
        }
        rhocur = rho;
        /* ENFORCE_BOUNDS: keep the step rhocur inside [lb, ub]. */
        if (x[i__] + rhocur > ub[i__])
        {
            if (x[i__] - rhocur >= lb[i__])
            {
                rhocur = -rhocur;
            }
            else if (ub[i__] - x[i__] > x[i__] - lb[i__])
            {
                rhocur = static_cast<T>(0.5) * (ub[i__] - x[i__]);
            }
            else
            {
                rhocur = static_cast<T>(0.5) * (x[i__] - lb[i__]);
            }
        }
        sim[i__ + i__ * sim_dim1] = rhocur;
        simi[i__ + i__ * simi_dim1] = static_cast<T>(1.0) / rhocur;
    }
    jdrop = np;
    ibrnch = 0;

/* Call CALCFC (these instructions also serve the in-iteration calls). */
L40:
    if (stop->nevals > 0)
    {
        if (stop_evals(*stop))
        {
            rc = Rc::MaxevalReached;
        }
    }
    if (rc != Rc::Success)
    {
        goto L600;
    }

    ++stop->nevals;
    if (calcfc(*n, *m, &x[1], f, &con[1]))
    {
        rc = Rc::MaxevalReached; // user-requested stop (unused in Cerid; mapped like a cap)
        goto L600;
    }

    resmax = static_cast<T>(0);
    feasible = 1; /* SGJ, 2010 */
    if (*m > 0)
    {
        i__1 = *m;
        for (k = 1; k <= i__1; ++k)
        {
            d__1 = resmax;
            d__2 = -con[k];
            resmax = d__1 >= d__2 ? d__1 : d__2;
            if (d__2 > con_tol[k - 1])
            {
                feasible = 0; /* SGJ, 2010 */
            }
        }
    }

    /* SGJ, 2008: minf_max reached by a feasible point. */
    if (f < stop->minf_max && feasible)
    {
        rc = Rc::MinfMaxReached;
        goto L620; /* not L600: use the current x, f, resmax */
    }

    con[mp] = f;
    con[*mpp] = resmax;
    if (ibrnch == 1)
    {
        goto L440;
    }

    /* DATMAT column jdrop ← the values at the new vertex (constraints, then f, then max violation). */
    i__1 = *mpp;
    for (k = 1; k <= i__1; ++k)
    {
        datmat[k + jdrop * datmat_dim1] = con[k];
    }
    if (stop->nevals > np)
    {
        goto L130;
    }

    /* Building the initial simplex: swap the new vertex into pole position if it improved, then evaluate the
       next vertex. */
    if (jdrop <= *n)
    {
        if (datmat[mp + np * datmat_dim1] <= f)
        {
            x[jdrop] = sim[jdrop + np * sim_dim1];
        }
        else
        { /* improvement in function val */
            T rhocur = x[jdrop] - sim[jdrop + np * sim_dim1];
            /* SGJ: rhocur (not rho) keeps simplex points within [lb, ub]. */
            sim[jdrop + np * sim_dim1] = x[jdrop];
            i__1 = *mpp;
            for (k = 1; k <= i__1; ++k)
            {
                datmat[k + jdrop * datmat_dim1] = datmat[k + np * datmat_dim1];
                datmat[k + np * datmat_dim1] = con[k];
            }
            i__1 = jdrop;
            for (k = 1; k <= i__1; ++k)
            {
                sim[jdrop + k * sim_dim1] = -rhocur;
                temp = static_cast<T>(0.F);
                i__2 = jdrop;
                for (i__ = k; i__ <= i__2; ++i__)
                {
                    temp -= simi[i__ + k * simi_dim1];
                }
                simi[jdrop + k * simi_dim1] = temp;
            }
        }
    }
    if (stop->nevals <= *n)
    { /* evaluating the initial simplex */
        jdrop = stop->nevals;
        /* SGJ: sim[jdrop, jdrop] (not rho) for [lb, ub] consistency. */
        x[jdrop] += sim[jdrop + jdrop * sim_dim1];
        goto L40;
    }
L130:
    ibrnch = 1;

/* Identify the optimal vertex of the current simplex. */
L140:
    phimin = datmat[mp + np * datmat_dim1] + parmu * datmat[*mpp + np * datmat_dim1];
    nbest = np;
    i__1 = *n;
    for (j = 1; j <= i__1; ++j)
    {
        temp = datmat[mp + j * datmat_dim1] + parmu * datmat[*mpp + j * datmat_dim1];
        if (temp < phimin)
        {
            nbest = j;
            phimin = temp;
        }
        else if (temp == phimin && parmu == static_cast<T>(0))
        {
            if (datmat[*mpp + j * datmat_dim1] < datmat[*mpp + nbest * datmat_dim1])
            {
                nbest = j;
            }
        }
    }

    /* Switch the best vertex into pole position; update SIM, SIMI, DATMAT. */
    if (nbest <= *n)
    {
        i__1 = *mpp;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            temp = datmat[i__ + np * datmat_dim1];
            datmat[i__ + np * datmat_dim1] = datmat[i__ + nbest * datmat_dim1];
            datmat[i__ + nbest * datmat_dim1] = temp;
        }
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            temp = sim[i__ + nbest * sim_dim1];
            sim[i__ + nbest * sim_dim1] = static_cast<T>(0);
            sim[i__ + np * sim_dim1] += temp;
            tempa = static_cast<T>(0);
            i__2 = *n;
            for (k = 1; k <= i__2; ++k)
            {
                sim[i__ + k * sim_dim1] -= temp;
                tempa -= simi[k + i__ * simi_dim1];
            }
            simi[nbest + i__ * simi_dim1] = tempa;
        }
    }

    /* Error return if SIMI is a poor inverse of SIM's leading block. */
    error = static_cast<T>(0);
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
        {
            temp = static_cast<T>(0);
            if (i__ == j)
            {
                temp += static_cast<T>(-1);
            }
            i__3 = *n;
            for (k = 1; k <= i__3; ++k)
            {
                if (sim[k + j * sim_dim1] != static_cast<T>(0))
                {
                    temp += simi[i__ + k * simi_dim1] * sim[k + j * sim_dim1];
                }
            }
            d__1 = error;
            d__2 = std::fabs(temp);
            error = d__1 >= d__2 ? d__1 : d__2;
        }
    }
    if (error > static_cast<T>(.1))
    {
        rc = Rc::RoundoffLimited;
        goto L600;
    }

    /* Linear-model coefficients: constraint gradients, then MINUS the objective gradient, in A. */
    i__2 = mp;
    for (k = 1; k <= i__2; ++k)
    {
        con[k] = -datmat[k + np * datmat_dim1];
        i__1 = *n;
        for (j = 1; j <= i__1; ++j)
        {
            w[j] = datmat[k + j * datmat_dim1] + con[k];
        }
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            temp = static_cast<T>(0);
            i__3 = *n;
            for (j = 1; j <= i__3; ++j)
            {
                temp += w[j] * simi[j + i__ * simi_dim1];
            }
            if (k == mp)
            {
                temp = -temp;
            }
            a[i__ + k * a_dim1] = temp;
        }
    }

    /* Acceptability of the simplex (sigma/eta); IFLAG=0 ⇒ not acceptable. */
    iflag = 1;
    parsig = alpha * rho;
    pareta = beta * rho;
    i__1 = *n;
    for (j = 1; j <= i__1; ++j)
    {
        wsig = static_cast<T>(0);
        weta = static_cast<T>(0);
        i__2 = *n;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            d__1 = simi[j + i__ * simi_dim1];
            wsig += d__1 * d__1;
            d__1 = sim[i__ + j * sim_dim1];
            weta += d__1 * d__1;
        }
        vsig[j] = static_cast<T>(1) / std::sqrt(wsig);
        veta[j] = std::sqrt(weta);
        if (vsig[j] < parsig || veta[j] > pareta)
        {
            iflag = 0;
        }
    }

    /* If a new vertex is needed for acceptability, pick the one to drop. */
    if (ibrnch == 1 || iflag == 1)
    {
        goto L370;
    }
    jdrop = 0;
    temp = pareta;
    i__1 = *n;
    for (j = 1; j <= i__1; ++j)
    {
        if (veta[j] > temp)
        {
            jdrop = j;
            temp = veta[j];
        }
    }
    if (jdrop == 0)
    {
        i__1 = *n;
        for (j = 1; j <= i__1; ++j)
        {
            if (vsig[j] < temp)
            {
                jdrop = j;
                temp = vsig[j];
            }
        }
    }

    /* The step to the new vertex and its sign. */
    temp = gamma_ * rho * vsig[jdrop];
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        dx[i__] = temp * simi[jdrop + i__ * simi_dim1];
    }
    cvmaxp = static_cast<T>(0);
    cvmaxm = static_cast<T>(0);
    i__1 = mp;
    for (k = 1; k <= i__1; ++k)
    {
        sum = static_cast<T>(0);
        i__2 = *n;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            sum += a[i__ + k * a_dim1] * dx[i__];
        }
        if (k < mp)
        {
            temp = datmat[k + np * datmat_dim1];
            d__1 = cvmaxp;
            d__2 = -sum - temp;
            cvmaxp = d__1 >= d__2 ? d__1 : d__2;
            d__1 = cvmaxm;
            d__2 = sum - temp;
            cvmaxm = d__1 >= d__2 ? d__1 : d__2;
        }
    }
    dxsign = static_cast<T>(1);
    if (parmu * (cvmaxp - cvmaxm) > sum + sum)
    {
        dxsign = static_cast<T>(-1);
    }

    /* Update SIM and SIMI; set the next X. */
    temp = static_cast<T>(0);
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        /* SGJ, 2010: pseudo-randomized simplex steps (deterministic LCG). */
        dx[i__] = dxsign * dx[i__] * lcg_urand<T>(&seed, static_cast<T>(0.01), static_cast<T>(1));
        /* ENFORCE_BOUNDS: keep the dx step within [lb, ub]. */
        {
            T xi = sim[i__ + np * sim_dim1];
        fixdx:
            if (xi + dx[i__] > ub[i__])
            {
                dx[i__] = -dx[i__];
            }
            if (xi + dx[i__] < lb[i__])
            {
                if (xi - dx[i__] <= ub[i__])
                {
                    dx[i__] = -dx[i__];
                }
                else
                { /* halve and retry */
                    dx[i__] *= static_cast<T>(0.5);
                    goto fixdx;
                }
            }
        }
        sim[i__ + jdrop * sim_dim1] = dx[i__];
        temp += simi[jdrop + i__ * simi_dim1] * dx[i__];
    }
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        simi[jdrop + i__ * simi_dim1] /= temp;
    }
    i__1 = *n;
    for (j = 1; j <= i__1; ++j)
    {
        if (j != jdrop)
        {
            temp = static_cast<T>(0);
            i__2 = *n;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                temp += simi[j + i__ * simi_dim1] * dx[i__];
            }
            i__2 = *n;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                simi[j + i__ * simi_dim1] -= temp * simi[jdrop + i__ * simi_dim1];
            }
        }
        x[j] = sim[j + np * sim_dim1] + dx[j];
    }
    goto L40;

/* DX = x(*) − x(0) via trstlp; branch if ‖DX‖ < ρ/2. */
L370:
    iz = 1;
    izdota = iz + *n * *n;
    ivmc = izdota + *n;
    isdirn = ivmc + mp;
    idxnew = isdirn + *n;
    ivmd = idxnew + *n;
    rc = trstlp<T>(n, m, &a[a_offset], &con[1], &rho, &dx[1], &ifull, &iact[1], &w[iz], &w[izdota], &w[ivmc],
                   &w[isdirn], &w[idxnew], &w[ivmd]);
    if (rc != Rc::Success)
    {
        goto L600;
    }
    /* ENFORCE_BOUNDS paranoia: clamp dx into [lb, ub] (bounds are linear; should already hold). */
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        T xi = sim[i__ + np * sim_dim1];
        if (xi + dx[i__] > ub[i__])
        {
            dx[i__] = ub[i__] - xi;
        }
        if (xi + dx[i__] < lb[i__])
        {
            dx[i__] = xi - lb[i__];
        }
    }
    if (ifull == 0)
    {
        temp = static_cast<T>(0);
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            d__1 = dx[i__];
            temp += d__1 * d__1;
        }
        if (temp < rho * static_cast<T>(.25) * rho)
        {
            ibrnch = 1;
            goto L550;
        }
    }

    /* Predicted change to F and to the maximum violation under the linear models. */
    resnew = static_cast<T>(0);
    con[mp] = static_cast<T>(0);
    i__1 = mp;
    for (k = 1; k <= i__1; ++k)
    {
        sum = con[k];
        i__2 = *n;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            sum -= a[i__ + k * a_dim1] * dx[i__];
        }
        if (k < mp)
        {
            resnew = resnew >= sum ? resnew : sum;
        }
    }

    /* Raise PARMU if needed (branch back if that moves the optimal vertex); set PREREM/PREREC. */
    barmu = static_cast<T>(0);
    prerec = datmat[*mpp + np * datmat_dim1] - resnew;
    if (prerec > static_cast<T>(0))
    {
        barmu = sum / prerec;
    }
    if (parmu < barmu * static_cast<T>(1.5))
    {
        parmu = barmu * static_cast<T>(2);
        phi = datmat[mp + np * datmat_dim1] + parmu * datmat[*mpp + np * datmat_dim1];
        i__1 = *n;
        for (j = 1; j <= i__1; ++j)
        {
            temp = datmat[mp + j * datmat_dim1] + parmu * datmat[*mpp + j * datmat_dim1];
            if (temp < phi)
            {
                goto L140;
            }
            if (temp == phi && parmu == static_cast<T>(0.F))
            {
                if (datmat[*mpp + j * datmat_dim1] < datmat[*mpp + np * datmat_dim1])
                {
                    goto L140;
                }
            }
        }
    }
    prerem = parmu * prerec - sum;

    /* Evaluate at x(*); then the actual merit reduction. */
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        x[i__] = sim[i__ + np * sim_dim1] + dx[i__];
    }
    ibrnch = 1;
    goto L40;
L440:
    vmold = datmat[mp + np * datmat_dim1] + parmu * datmat[*mpp + np * datmat_dim1];
    vmnew = f + parmu * resmax;
    trured = vmold - vmnew;
    if (parmu == static_cast<T>(0) && f == datmat[mp + np * datmat_dim1])
    {
        prerem = prerec;
        trured = datmat[*mpp + np * datmat_dim1] - resmax;
    }

    /* Decide whether x(*) replaces a vertex (mandatory when TRURED > 0); JDROP = the vertex to replace. */
    ratio = static_cast<T>(0);
    if (trured <= static_cast<T>(0.F))
    {
        ratio = static_cast<T>(1.F);
    }
    jdrop = 0;
    i__1 = *n;
    for (j = 1; j <= i__1; ++j)
    {
        temp = static_cast<T>(0);
        i__2 = *n;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            temp += simi[j + i__ * simi_dim1] * dx[i__];
        }
        temp = std::fabs(temp);
        if (temp > ratio)
        {
            jdrop = j;
            ratio = temp;
        }
        sigbar[j] = temp * vsig[j];
    }

    /* The value of ell. */
    edgmax = delta * rho;
    l = 0;
    i__1 = *n;
    for (j = 1; j <= i__1; ++j)
    {
        if (sigbar[j] >= parsig || sigbar[j] >= vsig[j])
        {
            temp = veta[j];
            if (trured > static_cast<T>(0))
            {
                temp = static_cast<T>(0);
                i__2 = *n;
                for (i__ = 1; i__ <= i__2; ++i__)
                {
                    d__1 = dx[i__] - sim[i__ + j * sim_dim1];
                    temp += d__1 * d__1;
                }
                temp = std::sqrt(temp);
            }
            if (temp > edgmax)
            {
                l = j;
                edgmax = temp;
            }
        }
    }
    if (l > 0)
    {
        jdrop = l;
    }
    if (jdrop == 0)
    {
        goto L550;
    }

    /* Revise the simplex (SIM, SIMI, DATMAT). */
    temp = static_cast<T>(0);
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        sim[i__ + jdrop * sim_dim1] = dx[i__];
        temp += simi[jdrop + i__ * simi_dim1] * dx[i__];
    }
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        simi[jdrop + i__ * simi_dim1] /= temp;
    }
    i__1 = *n;
    for (j = 1; j <= i__1; ++j)
    {
        if (j != jdrop)
        {
            temp = static_cast<T>(0);
            i__2 = *n;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                temp += simi[j + i__ * simi_dim1] * dx[i__];
            }
            i__2 = *n;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                simi[j + i__ * simi_dim1] -= temp * simi[jdrop + i__ * simi_dim1];
            }
        }
    }
    i__1 = *mpp;
    for (k = 1; k <= i__1; ++k)
    {
        datmat[k + jdrop * datmat_dim1] = con[k];
    }

    /* Iterate at the current RHO; SGJ/SAS rule: double rho when the prediction was accurate (and the simplex
       acceptable) to avoid tiny-step stalls. */
    if (trured > static_cast<T>(0) && trured >= prerem * static_cast<T>(.1))
    {
        if (trured >= prerem * static_cast<T>(0.9) && trured <= prerem * static_cast<T>(1.1) && iflag)
        {
            rho *= static_cast<T>(2.0);
        }
        goto L140;
    }
L550:
    if (iflag == 0)
    {
        ibrnch = 0;
        goto L140;
    }

    /* SGJ, 2008: f-convergence tests (best val lives in datmat[mp + np·] or f when ifull == 1). */
    {
        T fbest = ifull == 1 ? f : datmat[mp + np * datmat_dim1];
        if (fbest < *minf && stop_ftol(*stop, fbest, *minf))
        {
            rc = Rc::FtolReached;
            goto L600;
        }
        *minf = fbest;
    }

    /* Reduce RHO (resetting PARMU) if not at its floor. */
    if (rho > rhoend)
    {
        rho *= static_cast<T>(.5);
        if (rho <= rhoend * static_cast<T>(1.5))
        {
            rho = rhoend;
        }
        if (parmu > static_cast<T>(0))
        {
            denom = static_cast<T>(0);
            i__1 = mp;
            for (k = 1; k <= i__1; ++k)
            {
                cmin = datmat[k + np * datmat_dim1];
                cmax = cmin;
                i__2 = *n;
                for (i__ = 1; i__ <= i__2; ++i__)
                {
                    d__1 = cmin;
                    d__2 = datmat[k + i__ * datmat_dim1];
                    cmin = d__1 <= d__2 ? d__1 : d__2;
                    d__1 = cmax;
                    d__2 = datmat[k + i__ * datmat_dim1];
                    cmax = d__1 >= d__2 ? d__1 : d__2;
                }
                if (k <= *m && cmin < cmax * static_cast<T>(.5))
                {
                    temp = (cmax >= static_cast<T>(0) ? cmax : static_cast<T>(0)) - cmin;
                    if (denom <= static_cast<T>(0))
                    {
                        denom = temp;
                    }
                    else
                    {
                        denom = denom <= temp ? denom : temp;
                    }
                }
            }
            if (denom == static_cast<T>(0))
            {
                parmu = static_cast<T>(0);
            }
            else if (cmax - cmin < parmu * denom)
            {
                parmu = (cmax - cmin) / denom;
            }
        }
        goto L140;
    }
    else /* rho <= rhoend */
    {
        rc = rhoend > static_cast<T>(0) ? Rc::XtolReached : Rc::RoundoffLimited;
    }

    /* Return the best calculated values. */
    if (ifull == 1)
    {
        goto L620;
    }
L600:
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        x[i__] = sim[i__ + np * sim_dim1];
    }
    f = datmat[mp + np * datmat_dim1];
L620:
    *minf = f;
    return rc;
} /* cobylb */

} // namespace detail::cobyla_impl

// ------------------------------------------------------------------------------------------ public driver
// min f(x) s.t. c_I(x) ≥ 0 (the pinned v7-j convention — COBYLA's own), lb ≤ x ≤ ub. `cons` may be nullptr
// (unconstrained-with-bounds); equalities are NOT supported by COBYLA (num_eq() must be 0 — model h(x)=0 as
// the ±pair h ≥ 0 ∧ −h ≥ 0 if needed). `lower`/`upper` may be empty (⇒ unbounded). Value-only: no gradients.
template <typename T>
[[nodiscard]] OptResult<T> minimize_cobyla(const Objective<T>& obj, const Constraints<T>* cons,
                                           crd::containers::ConstSpan<T> x0, crd::containers::ConstSpan<T> lower,
                                           crd::containers::ConstSpan<T> upper, crd::memory::IAllocator* alloc,
                                           const CobylaOptions<T>& co = {})
{
    namespace ci = detail::cobyla_impl;
    const crd::usize nn = obj.n();
    CRD_ASSERT_MSG(x0.size() == nn, "minimize_cobyla: x0 size mismatch");
    CRD_ASSERT_MSG(cons == nullptr || cons->num_eq() == 0,
                   "minimize_cobyla: COBYLA handles inequalities only (model equalities as +/- pairs)");
    CRD_ASSERT_MSG(cons == nullptr || cons->n() == nn, "minimize_cobyla: constraints dimension mismatch");
    CRD_ASSERT_MSG(lower.size() == nn || lower.size() == 0, "minimize_cobyla: lower size mismatch");
    CRD_ASSERT_MSG(upper.size() == nn || upper.size() == 0, "minimize_cobyla: upper size mismatch");

    OptResult<T> result(alloc);
    result.x.resize(nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        result.x[i] = x0[i];
    }
    if (nn == 0)
    {
        result.status = OptStatus::Success;
        result.converged = true;
        return result;
    }

    const int n = static_cast<int>(nn);
    const int m = cons != nullptr ? static_cast<int>(cons->num_ineq()) : 0;
    const int mpp = m + 2;
    const T inf = std::numeric_limits<T>::infinity();

    // Bounds (±inf when absent).
    crd::containers::Array<T> lb(alloc);
    crd::containers::Array<T> ub(alloc);
    lb.resize(nn);
    ub.resize(nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        lb[i] = lower.size() == nn ? lower[i] : -inf;
        ub[i] = upper.size() == nn ? upper[i] : inf;
        CRD_ASSERT_MSG(lb[i] <= ub[i], "minimize_cobyla: lower > upper");
    }

    // Workspace (the reference partition: w of n*(3n+2m+11)+4m+6, iact of m+1) + con_tol zeros.
    crd::containers::Array<T> w(alloc);
    crd::containers::Array<int> iact(alloc);
    crd::containers::Array<T> con_tol(alloc);
    crd::containers::Array<T> ce_dummy(alloc); // num_eq == 0; eval still wants the span
    w.resize(static_cast<crd::usize>(n) * (3 * static_cast<crd::usize>(n) + 2 * static_cast<crd::usize>(m) + 11) +
             4 * static_cast<crd::usize>(m) + 6);
    iact.resize(static_cast<crd::usize>(m) + 1);
    con_tol.resize(static_cast<crd::usize>(m) + 1); // zeros: strict feasibility bookkeeping
    for (crd::usize i = 0; i < con_tol.size(); ++i)
    {
        con_tol[i] = static_cast<T>(0);
    }

    ci::Stop<T> stop;
    stop.maxeval = co.max_evals > 0 ? static_cast<int>(co.max_evals) : 1000 * (n + 1);
    stop.ftol_rel = co.ftol_rel;
    stop.ftol_abs = co.ftol_abs;

    crd::usize fn_evals = 0;
    auto calcfc = [&](int cn, int cm, const T* xx, T& f, T* con) -> bool
    {
        ++fn_evals;
        f = obj.value({xx, static_cast<crd::usize>(cn)});
        if (cm > 0)
        {
            cons->eval({xx, static_cast<crd::usize>(cn)}, {ce_dummy.data(), 0}, {con, static_cast<crd::usize>(cm)});
        }
        return false;
    };

    // The reference w-partition from cobyla().
    T* wp = w.data() - 1; // 1-based view
    const int icon = 1;
    const int isim = icon + mpp;
    const int isimi = isim + n * n + n;
    const int idatm = isimi + n * n;
    const int ia = idatm + n * mpp + mpp;
    const int ivsig = ia + m * n + n;
    const int iveta = ivsig + n;
    const int isigb = iveta + n;
    const int idx = isigb + n;
    const int iwork = idx + n;

    T minf = std::numeric_limits<T>::infinity();
    const T rhobeg = co.rhobeg;
    const ci::Rc rc = ci::cobylb<T>(&n, &m, &mpp, result.x.data(), &minf, &rhobeg, co.rhoend, &stop, lb.data(),
                                    ub.data(), &wp[icon], &wp[isim], &wp[isimi], &wp[idatm], &wp[ia], &wp[ivsig],
                                    &wp[iveta], &wp[isigb], &wp[idx], &wp[iwork], iact.data(), con_tol.data(), calcfc);

    result.fx = minf;
    result.fn_evals = fn_evals;
    result.iterations = static_cast<crd::usize>(stop.nevals); // COBYLA counts evaluations
    switch (rc)
    {
        case ci::Rc::XtolReached:
        case ci::Rc::FtolReached:
        case ci::Rc::MinfMaxReached:
        case ci::Rc::Success:
            result.status = OptStatus::Success;
            break;
        case ci::Rc::MaxevalReached:
            result.status = OptStatus::MaxIterations;
            break;
        case ci::Rc::RoundoffLimited:
        default:
            result.status = OptStatus::SmallStep; // rounding-limited stall (documented mapping)
            break;
    }
    result.converged = result.status == OptStatus::Success;
    return result;
}

} // namespace crd::hesap::opt
