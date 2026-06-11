#pragma once

// newuoa.hpp — Phase 3.1.6 v7-p-3: NEWUOA (Powell 2004) — unconstrained derivative-free minimization by
// QUADRATIC interpolation models over NPT points (n+2 ≤ NPT ≤ (n+1)(n+2)/2, default 2n+1), the truncated-CG
// trust-region subproblem (`trsapp_`), the model-improvement movers (`biglag_`/`bigden_` maximizing the
// denominator of the rank-2 H-update), and the `update_` of the inverse-KKT factorization (BMAT/ZMAT, Powell's
// Ω = ZDZᵀ form with the IDZ sign partition).
//
// ⚠ FAITHFUL PORT of the NLopt C reference (`nlopt/src/algs/newuoa/newuoa.c`, MIT) — the f2c idiom kept
// (1-based pointer adjustments, original goto flow, exact literal artifacts) per the L-BFGS-B/COBYLA playbook;
// the differential harness vs the compiled oracle adjudicates (runtime/examples/newuoa_difftest.cpp).
// SCOPE PINNED (phase doc): **Powell's CLASSIC UNCONSTRAINED NEWUOA** — the reference's NEWUOA_BOUND variant
// (`if (lb && ub)` blocks nesting an NLopt MMA solve inside trsapp_/biglag_) is NOT ported (bounds are
// BOBYQA's job — Powell's own position); the port takes the NULL-bounds paths verbatim, and the e2e diff
// passes NULL bounds so the comparison stays apples-to-apples. Stop semantics shared with the COBYLA port
// (detail::cobyla_impl — relstop/Stop verbatim from nlopt stop.c). ADR-0090.
//
// DETERMINISM: serial, fixed evaluation order, RNG-free ⇒ bit-identical runs by construction.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/cobyla.hpp> // detail::cobyla_impl::{Rc, Stop, relstop, stop_ftol, stop_evals}
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::opt
{

template <typename T> struct NewuoaOptions
{
    T rhobeg = static_cast<T>(1);    // initial trust-region radius
    T rhoend = static_cast<T>(1e-8); // final radius = the x-accuracy target
    T ftol_rel = static_cast<T>(0);  // relative f-convergence (0 = off; nlopt semantics)
    T ftol_abs = static_cast<T>(0);  // absolute f-convergence (0 = off)
    crd::usize npt = 0;              // interpolation points; 0 ⇒ 2n+1 (Powell's recommendation)
    crd::usize max_evals = 0;        // function-evaluation cap; 0 ⇒ 1000·(n+1)
};

namespace detail::newuoa_impl
{

using cobyla_impl::Rc;
using cobyla_impl::Stop;
using cobyla_impl::stop_evals;
using cobyla_impl::stop_ftol;

// --------------------------------------------------------------------------------------------- trsapp_
// The trust-region subproblem on the quadratic model Q (gradient GQ, Hessian = HQ packed + the PQ implicit
// part over XPT): truncated CG, then 2-D angle refinements on the boundary. CRVMIN = the least curvature seen
// (0 if STEP reaches the boundary). The L170 block is the reference's inline "subroutine" computing HD = H·D,
// dispatched by ITERC. The reference's NEWUOA_BOUND branch is NOT ported (scope pinned in the header).
template <typename T>
[[nodiscard]] inline Rc trsapp(const int* n, const int* npt, T* xopt, T* xpt, T* gq, T* hq, T* pq, const T* delta,
                               T* step, T* d__, T* g, T* hd, T* hs, T* crvmin)
{
    /* System generated locals */
    int xpt_dim1, xpt_offset, i__1, i__2;
    T d__1, d__2;

    /* Local variables */
    int i__, j, k;
    T dd = static_cast<T>(0), cf, dg, gg = static_cast<T>(0);
    int ih;
    T ds, sg = static_cast<T>(0);
    int iu;
    T ss, dhd, dhs, cth, sgk, shs = static_cast<T>(0), sth, qadd, half, qbeg, qred = static_cast<T>(0), qmin, temp,
                              qsav, qnew, zero, ggbeg = static_cast<T>(0), alpha, angle, reduc;
    int iterc;
    T ggsav, delsq, tempa = static_cast<T>(0), tempb = static_cast<T>(0);
    int isave;
    T bstep = static_cast<T>(0), ratio, twopi;
    int itersw;
    T angtest;
    int itermax;

    /* Parameter adjustments */
    xpt_dim1 = *npt;
    xpt_offset = 1 + xpt_dim1;
    xpt -= xpt_offset;
    --xopt;
    --gq;
    --hq;
    --pq;
    --step;
    --d__;
    --g;
    --hd;
    --hs;

    /* Function Body */
    half = static_cast<T>(.5);
    zero = static_cast<T>(0.);
    twopi = std::atan(static_cast<T>(1.)) * static_cast<T>(8.);
    delsq = *delta * *delta;
    iterc = 0;
    itermax = *n;
    itersw = itermax;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        d__[i__] = xopt[i__];
    }
    goto L170;

/* Prepare for the first line search. */
L20:
    qred = zero;
    dd = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        step[i__] = zero;
        hs[i__] = zero;
        g[i__] = gq[i__] + hd[i__];
        d__[i__] = -g[i__];
        d__1 = d__[i__];
        dd += d__1 * d__1;
    }
    *crvmin = zero;
    if (dd == zero)
    {
        goto L160;
    }
    ds = zero;
    ss = zero;
    gg = dd;
    ggbeg = gg;

/* The step to the trust-region boundary and the product HD. */
L40:
    ++iterc;
    temp = delsq - ss;
    bstep = temp / (ds + std::sqrt(ds * ds + dd * temp));
    goto L170;
L50:
    dhd = zero;
    i__1 = *n;
    for (j = 1; j <= i__1; ++j)
    {
        dhd += d__[j] * hd[j];
    }

    /* Update CRVMIN and set ALPHA. */
    alpha = bstep;
    if (dhd > zero)
    {
        temp = dhd / dd;
        if (iterc == 1)
        {
            *crvmin = temp;
        }
        *crvmin = *crvmin <= temp ? *crvmin : temp;
        d__1 = alpha;
        d__2 = gg / dhd;
        alpha = d__1 <= d__2 ? d__1 : d__2;
    }
    qadd = alpha * (gg - half * alpha * dhd);
    qred += qadd;

    /* Update STEP and HS. */
    ggsav = gg;
    gg = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        step[i__] += alpha * d__[i__];
        hs[i__] += alpha * hd[i__];
        d__1 = g[i__] + hs[i__];
        gg += d__1 * d__1;
    }

    /* Another conjugate-direction iteration if required. */
    if (alpha < bstep)
    {
        if (qadd <= qred * static_cast<T>(.01))
        {
            goto L160;
        }
        if (gg <= ggbeg * static_cast<T>(1e-4))
        {
            goto L160;
        }
        if (iterc == itermax)
        {
            goto L160;
        }
        temp = gg / ggsav;
        dd = zero;
        ds = zero;
        ss = zero;
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            d__[i__] = temp * d__[i__] - g[i__] - hs[i__];
            d__1 = d__[i__];
            dd += d__1 * d__1;
            ds += d__[i__] * step[i__];
            d__1 = step[i__];
            ss += d__1 * d__1;
        }
        if (ds <= zero)
        {
            goto L160;
        }
        if (ss < delsq)
        {
            goto L40;
        }
    }
    *crvmin = zero;
    itersw = iterc;

/* Test whether an alternative iteration is required. */
L90:
    if (gg <= ggbeg * static_cast<T>(1e-4))
    {
        goto L160;
    }
    sg = zero;
    shs = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        sg += step[i__] * g[i__];
        shs += step[i__] * hs[i__];
    }
    sgk = sg + shs;
    angtest = sgk / std::sqrt(gg * delsq);
    if (angtest <= static_cast<T>(-.99))
    {
        goto L160;
    }

    /* The alternative iteration: D, HD and scalar products. */
    ++iterc;
    temp = std::sqrt(delsq * gg - sgk * sgk);
    tempa = delsq / temp;
    tempb = sgk / temp;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        d__[i__] = tempa * (g[i__] + hs[i__]) - tempb * step[i__];
    }
    goto L170;
L120:
    dg = zero;
    dhd = zero;
    dhs = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        dg += d__[i__] * g[i__];
        dhd += hd[i__] * d__[i__];
        dhs += hd[i__] * step[i__];
    }

    /* The angle minimizing Q over the 49-point grid + parabolic refinement. */
    cf = half * (shs - dhd);
    qbeg = sg + cf;
    qsav = qbeg;
    qmin = qbeg;
    isave = 0;
    iu = 49;
    temp = twopi / static_cast<T>(iu + 1);
    qnew = zero;
    i__1 = iu;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        angle = static_cast<T>(i__) * temp;
        cth = std::cos(angle);
        sth = std::sin(angle);
        qnew = (sg + cf * cth) * cth + (dg + dhs * cth) * sth;
        if (qnew < qmin)
        {
            qmin = qnew;
            isave = i__;
            tempa = qsav;
        }
        else if (i__ == isave + 1)
        {
            tempb = qnew;
        }
        qsav = qnew;
    }
    if (static_cast<T>(isave) == zero)
    {
        tempa = qnew;
    }
    if (isave == iu)
    {
        tempb = qbeg;
    }
    angle = zero;
    if (tempa != tempb)
    {
        tempa -= qmin;
        tempb -= qmin;
        angle = half * (tempa - tempb) / (tempa + tempb);
    }
    angle = temp * (static_cast<T>(isave) + angle);

    /* New STEP and HS; convergence test. */
    cth = std::cos(angle);
    sth = std::sin(angle);
    reduc = qbeg - (sg + cf * cth) * cth - (dg + dhs * cth) * sth;
    gg = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        step[i__] = cth * step[i__] + sth * d__[i__];
        hs[i__] = cth * hs[i__] + sth * hd[i__];
        d__1 = g[i__] + hs[i__];
        gg += d__1 * d__1;
    }
    qred += reduc;
    ratio = reduc / qred;
    if (iterc < itermax && ratio > static_cast<T>(.01))
    {
        goto L90;
    }
L160:
    return Rc::Success;

/* The inline "subroutine" HD = H·D (implicit PQ part over XPT + the packed HQ part), dispatched by ITERC. */
L170:
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        hd[i__] = zero;
    }
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        temp = zero;
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
        {
            temp += xpt[k + j * xpt_dim1] * d__[j];
        }
        temp *= pq[k];
        i__2 = *n;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            hd[i__] += temp * xpt[k + i__ * xpt_dim1];
        }
    }
    ih = 0;
    i__2 = *n;
    for (j = 1; j <= i__2; ++j)
    {
        i__1 = j;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            ++ih;
            if (i__ < j)
            {
                hd[j] += hq[ih] * d__[i__];
            }
            hd[i__] += hq[ih] * d__[j];
        }
    }
    if (iterc == 0)
    {
        goto L20;
    }
    if (iterc <= itersw)
    {
        goto L50;
    }
    goto L120;
} /* trsapp */

// --------------------------------------------------------------------------------------------- update_
// Shift interpolation point KNEW: rank-2 update of BMAT and the ZMAT factor of Powell's Ω = Z·D·Zᵀ (IDZ
// partitions D's signs). VLAG carries Θ·Wcheck + e_b of formula (6.11); BETA its parameter; W is workspace.
template <typename T>
inline void update(const int* n, const int* npt, T* bmat, T* zmat, int* idz, const int* ndim, T* vlag, const T* beta,
                   const int* knew, T* w)
{
    /* System generated locals */
    int bmat_dim1, bmat_offset, zmat_dim1, zmat_offset, i__1, i__2;
    T d__1, d__2;

    /* Local variables */
    int i__, j, ja, jb, jl, jp;
    T one, tau, temp;
    int nptm;
    T zero;
    int iflag;
    T scala, scalb_, alpha, denom, tempa, tempb = static_cast<T>(0), tausq;

    /* Parameter adjustments */
    zmat_dim1 = *npt;
    zmat_offset = 1 + zmat_dim1;
    zmat -= zmat_offset;
    bmat_dim1 = *ndim;
    bmat_offset = 1 + bmat_dim1;
    bmat -= bmat_offset;
    --vlag;
    --w;

    /* Function Body */
    one = static_cast<T>(1.);
    zero = static_cast<T>(0.);
    nptm = *npt - *n - 1;

    /* Rotations zeroing the KNEW-th row of ZMAT. */
    jl = 1;
    i__1 = nptm;
    for (j = 2; j <= i__1; ++j)
    {
        if (j == *idz)
        {
            jl = *idz;
        }
        else if (zmat[*knew + j * zmat_dim1] != zero)
        {
            d__1 = zmat[*knew + jl * zmat_dim1];
            d__2 = zmat[*knew + j * zmat_dim1];
            temp = std::sqrt(d__1 * d__1 + d__2 * d__2);
            tempa = zmat[*knew + jl * zmat_dim1] / temp;
            tempb = zmat[*knew + j * zmat_dim1] / temp;
            i__2 = *npt;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                temp = tempa * zmat[i__ + jl * zmat_dim1] + tempb * zmat[i__ + j * zmat_dim1];
                zmat[i__ + j * zmat_dim1] = tempa * zmat[i__ + j * zmat_dim1] - tempb * zmat[i__ + jl * zmat_dim1];
                zmat[i__ + jl * zmat_dim1] = temp;
            }
            zmat[*knew + j * zmat_dim1] = zero;
        }
    }

    /* W ← the first NPT components of HLAG's KNEW-th column; the updating-formula parameters. */
    tempa = zmat[*knew + zmat_dim1];
    if (*idz >= 2)
    {
        tempa = -tempa;
    }
    if (jl > 1)
    {
        tempb = zmat[*knew + jl * zmat_dim1];
    }
    i__1 = *npt;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        w[i__] = tempa * zmat[i__ + zmat_dim1];
        if (jl > 1)
        {
            w[i__] += tempb * zmat[i__ + jl * zmat_dim1];
        }
    }
    alpha = w[*knew];
    tau = vlag[*knew];
    tausq = tau * tau;
    denom = alpha * *beta + tausq;
    vlag[*knew] -= one;

    /* ZMAT completion, single-nonzero case (IFLAG defers a column exchange). */
    iflag = 0;
    if (jl == 1)
    {
        temp = std::sqrt(std::fabs(denom));
        tempb = tempa / temp;
        tempa = tau / temp;
        i__1 = *npt;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            zmat[i__ + zmat_dim1] = tempa * zmat[i__ + zmat_dim1] - tempb * vlag[i__];
        }
        /* Reference artifact kept VERBATIM: temp = sqrt(|denom|) >= 0, so `temp < zero` never fires here
           (Powell's Fortran tested DENOM); the oracle diff is the contract — do not "fix". */
        if (*idz == 1 && temp < zero)
        {
            *idz = 2;
        }
        if (*idz >= 2 && temp >= zero)
        {
            iflag = 1;
        }
    }
    else
    {
        /* The alternative two-column case. */
        ja = 1;
        if (*beta >= zero)
        {
            ja = jl;
        }
        jb = jl + 1 - ja;
        temp = zmat[*knew + jb * zmat_dim1] / denom;
        tempa = temp * *beta;
        tempb = temp * tau;
        temp = zmat[*knew + ja * zmat_dim1];
        scala = one / std::sqrt(std::fabs(*beta) * temp * temp + tausq);
        scalb_ = scala * std::sqrt(std::fabs(denom));
        i__1 = *npt;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            zmat[i__ + ja * zmat_dim1] = scala * (tau * zmat[i__ + ja * zmat_dim1] - temp * vlag[i__]);
            zmat[i__ + jb * zmat_dim1] = scalb_ * (zmat[i__ + jb * zmat_dim1] - tempa * w[i__] - tempb * vlag[i__]);
        }
        if (denom <= zero)
        {
            if (*beta < zero)
            {
                ++(*idz);
            }
            if (*beta >= zero)
            {
                iflag = 1;
            }
        }
    }

    /* IDZ reduction (usually exchanging ZMAT's first column with a later one). */
    if (iflag == 1)
    {
        --(*idz);
        i__1 = *npt;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            temp = zmat[i__ + zmat_dim1];
            zmat[i__ + zmat_dim1] = zmat[i__ + *idz * zmat_dim1];
            zmat[i__ + *idz * zmat_dim1] = temp;
        }
    }

    /* Update BMAT. */
    i__1 = *n;
    for (j = 1; j <= i__1; ++j)
    {
        jp = *npt + j;
        w[jp] = bmat[*knew + j * bmat_dim1];
        tempa = (alpha * vlag[jp] - tau * w[jp]) / denom;
        tempb = (-(*beta) * w[jp] - tau * vlag[jp]) / denom;
        i__2 = jp;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            bmat[i__ + j * bmat_dim1] = bmat[i__ + j * bmat_dim1] + tempa * vlag[i__] + tempb * w[i__];
            if (i__ > *npt)
            {
                bmat[jp + (i__ - *npt) * bmat_dim1] = bmat[i__ + j * bmat_dim1];
            }
        }
    }
} /* update */

// --------------------------------------------------------------------------------------------- biglag_
// D ≈ argmax |LFUNC(XOPT + D)| s.t. ‖D‖ ≤ DELTA where LFUNC is the KNEW-th Lagrange function: a 2-D
// angle-sweep iteration in span{D, S}. ALPHA ← H's KNEW-th diagonal. The reference's `if (lb && ub)` MMA
// branch is NOT ported (scope pinned in the header). SGJ's isinf guard on HCOL kept.
template <typename T>
[[nodiscard]] inline Rc biglag(const int* n, const int* npt, T* xopt, T* xpt, T* bmat, T* zmat, const int* idz,
                               const int* ndim, const int* knew, const T* delta, T* d__, T* alpha, T* hcol, T* gc,
                               T* gd, T* s, T* w)
{
    /* System generated locals */
    int xpt_dim1, xpt_offset, bmat_dim1, bmat_offset, zmat_dim1, zmat_offset, i__1, i__2;
    T d__1;

    /* Local variables */
    int i__, j, k;
    T dd, gg;
    int iu;
    T sp, ss, cf1, cf2, cf3, cf4, cf5, dhd, cth, one, tau, sth, sum, half, temp, step;
    int nptm;
    T zero, angle, scale, denom;
    int iterc, isave;
    T delsq, tempa = static_cast<T>(0), tempb = static_cast<T>(0), twopi, taubeg, tauold, taumax;

    /* Parameter adjustments */
    zmat_dim1 = *npt;
    zmat_offset = 1 + zmat_dim1;
    zmat -= zmat_offset;
    xpt_dim1 = *npt;
    xpt_offset = 1 + xpt_dim1;
    xpt -= xpt_offset;
    --xopt;
    bmat_dim1 = *ndim;
    bmat_offset = 1 + bmat_dim1;
    bmat -= bmat_offset;
    --d__;
    --hcol;
    --gc;
    --gd;
    --s;
    --w;

    /* Function Body */
    half = static_cast<T>(.5);
    one = static_cast<T>(1.);
    zero = static_cast<T>(0.);
    twopi = std::atan(one) * static_cast<T>(8.);
    delsq = *delta * *delta;
    nptm = *npt - *n - 1;

    /* HCOL ← the leading elements of H's KNEW-th column. */
    iterc = 0;
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        hcol[k] = zero;
    }
    i__1 = nptm;
    for (j = 1; j <= i__1; ++j)
    {
        temp = zmat[*knew + j * zmat_dim1];
        if (j < *idz)
        {
            temp = -temp;
        }
        i__2 = *npt;
        for (k = 1; k <= i__2; ++k)
        {
            hcol[k] += temp * zmat[k + j * zmat_dim1];
            if (std::isinf(hcol[k]))
            {
                return Rc::RoundoffLimited;
            }
        }
    }
    *alpha = hcol[*knew];

    /* Unscaled initial D; the LFUNC gradient at XOPT; D times LFUNC's second-derivative matrix. */
    dd = zero;
    i__2 = *n;
    for (i__ = 1; i__ <= i__2; ++i__)
    {
        d__[i__] = xpt[*knew + i__ * xpt_dim1] - xopt[i__];
        gc[i__] = bmat[*knew + i__ * bmat_dim1];
        gd[i__] = zero;
        d__1 = d__[i__];
        dd += d__1 * d__1;
    }
    i__2 = *npt;
    for (k = 1; k <= i__2; ++k)
    {
        temp = zero;
        sum = zero;
        i__1 = *n;
        for (j = 1; j <= i__1; ++j)
        {
            temp += xpt[k + j * xpt_dim1] * xopt[j];
            sum += xpt[k + j * xpt_dim1] * d__[j];
        }
        temp = hcol[k] * temp;
        sum = hcol[k] * sum;
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            gc[i__] += temp * xpt[k + i__ * xpt_dim1];
            gd[i__] += sum * xpt[k + i__ * xpt_dim1];
        }
    }

    /* Scale D and GD (sign change if required); S ← another vector in the initial 2-D subspace. */
    gg = zero;
    sp = zero;
    dhd = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        d__1 = gc[i__];
        gg += d__1 * d__1;
        sp += d__[i__] * gc[i__];
        dhd += d__[i__] * gd[i__];
    }
    scale = *delta / std::sqrt(dd);
    if (sp * dhd < zero)
    {
        scale = -scale;
    }
    temp = zero;
    if (sp * sp > dd * static_cast<T>(.99) * gg)
    {
        temp = one;
    }
    tau = scale * (std::fabs(sp) + half * scale * std::fabs(dhd));
    if (gg * delsq < tau * static_cast<T>(.01) * tau)
    {
        temp = one;
    }
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        d__[i__] = scale * d__[i__];
        gd[i__] = scale * gd[i__];
        s[i__] = gc[i__] + temp * gd[i__];
    }

/* The iteration: S ← the required length/direction (terminate if D and S are nearly parallel). */
L80:
    ++iterc;
    dd = zero;
    sp = zero;
    ss = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        d__1 = d__[i__];
        dd += d__1 * d__1;
        sp += d__[i__] * s[i__];
        d__1 = s[i__];
        ss += d__1 * d__1;
    }
    temp = dd * ss - sp * sp;
    if (temp <= dd * static_cast<T>(1e-8) * ss)
    {
        goto L160;
    }
    denom = std::sqrt(temp);
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        s[i__] = (dd * s[i__] - sp * d__[i__]) / denom;
        w[i__] = zero;
    }

    /* The objective's coefficients on the circle (S times the second-derivative matrix first). */
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        sum = zero;
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
        {
            sum += xpt[k + j * xpt_dim1] * s[j];
        }
        sum = hcol[k] * sum;
        i__2 = *n;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            w[i__] += sum * xpt[k + i__ * xpt_dim1];
        }
    }
    cf1 = zero;
    cf2 = zero;
    cf3 = zero;
    cf4 = zero;
    cf5 = zero;
    i__2 = *n;
    for (i__ = 1; i__ <= i__2; ++i__)
    {
        cf1 += s[i__] * w[i__];
        cf2 += d__[i__] * gc[i__];
        cf3 += s[i__] * gc[i__];
        cf4 += d__[i__] * gd[i__];
        cf5 += s[i__] * gd[i__];
    }
    cf1 = half * cf1;
    cf4 = half * cf4 - cf1;

    /* The angle maximizing |TAU| over the 49-point grid + parabolic refinement. */
    taubeg = cf1 + cf2 + cf4;
    taumax = taubeg;
    tauold = taubeg;
    isave = 0;
    iu = 49;
    temp = twopi / static_cast<T>(iu + 1);
    tau = zero;
    i__2 = iu;
    for (i__ = 1; i__ <= i__2; ++i__)
    {
        angle = static_cast<T>(i__) * temp;
        cth = std::cos(angle);
        sth = std::sin(angle);
        tau = cf1 + (cf2 + cf4 * cth) * cth + (cf3 + cf5 * cth) * sth;
        if (std::fabs(tau) > std::fabs(taumax))
        {
            taumax = tau;
            isave = i__;
            tempa = tauold;
        }
        else if (i__ == isave + 1)
        {
            tempb = tau;
        }
        tauold = tau;
    }
    if (isave == 0)
    {
        tempa = tau;
    }
    if (isave == iu)
    {
        tempb = taubeg;
    }
    step = zero;
    if (tempa != tempb)
    {
        tempa -= taumax;
        tempb -= taumax;
        step = half * (tempa - tempb) / (tempa + tempb);
    }
    angle = temp * (static_cast<T>(isave) + step);

    /* New D and GD; convergence test. */
    cth = std::cos(angle);
    sth = std::sin(angle);
    tau = cf1 + (cf2 + cf4 * cth) * cth + (cf3 + cf5 * cth) * sth;
    i__2 = *n;
    for (i__ = 1; i__ <= i__2; ++i__)
    {
        d__[i__] = cth * d__[i__] + sth * s[i__];
        gd[i__] = cth * gd[i__] + sth * w[i__];
        s[i__] = gc[i__] + gd[i__];
    }
    if (std::fabs(tau) <= std::fabs(taubeg) * static_cast<T>(1.1))
    {
        goto L160;
    }
    if (iterc < *n)
    {
        goto L80;
    }
L160:
    return Rc::Success;
} /* biglag */

// --------------------------------------------------------------------------------------------- bigden_
// D ← a step (seeded by BIGLAG's D) giving a LARGE-modulus denominator in the rank-2 updating formula when
// point KNEW moves to XOPT + D: a 5-harmonic angle sweep over span{D, S} with DEN/DENEX/PAR Fourier
// coefficients. W ← Wcheck, VLAG ← Θ·Wcheck + e_b, BETA ← the formula parameter. SGJ's isinf guards kept;
// the `if (lb && ub)` truncation hack is NOT ported (scope pinned).
template <typename T>
[[nodiscard]] inline Rc bigden(const int* n, const int* npt, T* xopt, T* xpt, T* bmat, T* zmat, const int* idz,
                               const int* ndim, const int* kopt, const int* knew, T* d__, T* w, T* vlag, T* beta, T* s,
                               T* wvec, T* prod)
{
    /* System generated locals */
    int xpt_dim1, xpt_offset, bmat_dim1, bmat_offset, zmat_dim1, zmat_offset, wvec_dim1, wvec_offset, prod_dim1,
        prod_offset, i__1, i__2;
    T d__1;

    /* Local variables */
    int i__, j, k;
    T dd;
    int jc;
    T ds;
    int ip, iu, nw;
    T ss, den[9], one, par[9], tau, sum, two, diff, half, temp;
    int ksav;
    T step;
    int nptm;
    T zero, alpha, angle, denex[9];
    int iterc;
    T tempa, tempb, tempc;
    int isave;
    T ssden, dtest, quart, xoptd, twopi, xopts, denold, denmax, densav, dstemp, sumold, sstemp, xoptsq;

    /* Parameter adjustments */
    zmat_dim1 = *npt;
    zmat_offset = 1 + zmat_dim1;
    zmat -= zmat_offset;
    xpt_dim1 = *npt;
    xpt_offset = 1 + xpt_dim1;
    xpt -= xpt_offset;
    --xopt;
    prod_dim1 = *ndim;
    prod_offset = 1 + prod_dim1;
    prod -= prod_offset;
    wvec_dim1 = *ndim;
    wvec_offset = 1 + wvec_dim1;
    wvec -= wvec_offset;
    bmat_dim1 = *ndim;
    bmat_offset = 1 + bmat_dim1;
    bmat -= bmat_offset;
    --d__;
    --w;
    --vlag;
    --s;

    /* Function Body */
    half = static_cast<T>(.5);
    one = static_cast<T>(1.);
    quart = static_cast<T>(.25);
    two = static_cast<T>(2.);
    zero = static_cast<T>(0.);
    twopi = std::atan(one) * static_cast<T>(8.);
    nptm = *npt - *n - 1;
    ksav = 0;
    tempa = zero;
    tempb = zero;

    /* W(N+1..N+NPT) ← the first NPT elements of H's KNEW-th column. */
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        w[*n + k] = zero;
    }
    i__1 = nptm;
    for (j = 1; j <= i__1; ++j)
    {
        temp = zmat[*knew + j * zmat_dim1];
        if (j < *idz)
        {
            temp = -temp;
        }
        i__2 = *npt;
        for (k = 1; k <= i__2; ++k)
        {
            w[*n + k] += temp * zmat[k + j * zmat_dim1];
        }
    }
    alpha = w[*n + *knew];

    /* Initial D from BIGLAG; initial S usually XOPT→X_KNEW (or another point to avoid near-parallel D, S). */
    dd = zero;
    ds = zero;
    ss = zero;
    xoptsq = zero;
    i__2 = *n;
    for (i__ = 1; i__ <= i__2; ++i__)
    {
        d__1 = d__[i__];
        dd += d__1 * d__1;
        s[i__] = xpt[*knew + i__ * xpt_dim1] - xopt[i__];
        ds += d__[i__] * s[i__];
        d__1 = s[i__];
        ss += d__1 * d__1;
        d__1 = xopt[i__];
        xoptsq += d__1 * d__1;
    }
    if (ds * ds > dd * static_cast<T>(.99) * ss)
    {
        ksav = *knew;
        dtest = ds * ds / ss;
        i__2 = *npt;
        for (k = 1; k <= i__2; ++k)
        {
            if (k != *kopt)
            {
                dstemp = zero;
                sstemp = zero;
                i__1 = *n;
                for (i__ = 1; i__ <= i__1; ++i__)
                {
                    diff = xpt[k + i__ * xpt_dim1] - xopt[i__];
                    dstemp += d__[i__] * diff;
                    sstemp += diff * diff;
                }
                if (sstemp == static_cast<T>(0))
                {
                    return Rc::RoundoffLimited;
                }
                if (dstemp * dstemp / sstemp < dtest)
                {
                    ksav = k;
                    dtest = dstemp * dstemp / sstemp;
                    ds = dstemp;
                    ss = sstemp;
                }
            }
        }
        i__2 = *n;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            s[i__] = xpt[ksav + i__ * xpt_dim1] - xopt[i__];
        }
    }
    ssden = dd * ss - ds * ds;
    iterc = 0;
    densav = zero;

/* The iteration: S ← the required length/direction. */
L70:
    ++iterc;
    if (ssden < static_cast<T>(0))
    {
        return Rc::RoundoffLimited;
    }
    temp = one / std::sqrt(ssden);
    xoptd = zero;
    xopts = zero;
    i__2 = *n;
    for (i__ = 1; i__ <= i__2; ++i__)
    {
        s[i__] = temp * (dd * s[i__] - ds * d__[i__]);
        xoptd += xopt[i__] * d__[i__];
        if (std::isinf(s[i__]))
        {
            return Rc::RoundoffLimited;
        }
        xopts += xopt[i__] * s[i__];
    }

    /* The first two BETA terms. */
    tempa = half * xoptd * xoptd;
    tempb = half * xopts * xopts;
    den[0] = dd * (xoptsq + half * dd) + tempa + tempb;
    den[1] = two * xoptd * dd;
    den[2] = two * xopts * dd;
    den[3] = tempa - tempb;
    den[4] = xoptd * xopts;
    for (i__ = 6; i__ <= 9; ++i__)
    {
        den[i__ - 1] = zero;
    }

    /* Wcheck's coefficients into WVEC. */
    i__2 = *npt;
    for (k = 1; k <= i__2; ++k)
    {
        tempa = zero;
        tempb = zero;
        tempc = zero;
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            tempa += xpt[k + i__ * xpt_dim1] * d__[i__];
            tempb += xpt[k + i__ * xpt_dim1] * s[i__];
            tempc += xpt[k + i__ * xpt_dim1] * xopt[i__];
        }
        wvec[k + wvec_dim1] = quart * (tempa * tempa + tempb * tempb);
        wvec[k + (wvec_dim1 << 1)] = tempa * tempc;
        wvec[k + wvec_dim1 * 3] = tempb * tempc;
        wvec[k + (wvec_dim1 << 2)] = quart * (tempa * tempa - tempb * tempb);
        wvec[k + wvec_dim1 * 5] = half * tempa * tempb;
    }
    i__2 = *n;
    for (i__ = 1; i__ <= i__2; ++i__)
    {
        ip = i__ + *npt;
        wvec[ip + wvec_dim1] = zero;
        wvec[ip + (wvec_dim1 << 1)] = d__[i__];
        wvec[ip + wvec_dim1 * 3] = s[i__];
        wvec[ip + (wvec_dim1 << 2)] = zero;
        wvec[ip + wvec_dim1 * 5] = zero;
    }

    /* Θ·Wcheck's coefficients into PROD. */
    for (jc = 1; jc <= 5; ++jc)
    {
        nw = *npt;
        if (jc == 2 || jc == 3)
        {
            nw = *ndim;
        }
        i__2 = *npt;
        for (k = 1; k <= i__2; ++k)
        {
            prod[k + jc * prod_dim1] = zero;
        }
        i__2 = nptm;
        for (j = 1; j <= i__2; ++j)
        {
            sum = zero;
            i__1 = *npt;
            for (k = 1; k <= i__1; ++k)
            {
                sum += zmat[k + j * zmat_dim1] * wvec[k + jc * wvec_dim1];
            }
            if (j < *idz)
            {
                sum = -sum;
            }
            i__1 = *npt;
            for (k = 1; k <= i__1; ++k)
            {
                prod[k + jc * prod_dim1] += sum * zmat[k + j * zmat_dim1];
            }
        }
        if (nw == *ndim)
        {
            i__1 = *npt;
            for (k = 1; k <= i__1; ++k)
            {
                sum = zero;
                i__2 = *n;
                for (j = 1; j <= i__2; ++j)
                {
                    sum += bmat[k + j * bmat_dim1] * wvec[*npt + j + jc * wvec_dim1];
                }
                prod[k + jc * prod_dim1] += sum;
            }
        }
        i__1 = *n;
        for (j = 1; j <= i__1; ++j)
        {
            sum = zero;
            i__2 = nw;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                sum += bmat[i__ + j * bmat_dim1] * wvec[i__ + jc * wvec_dim1];
            }
            prod[*npt + j + jc * prod_dim1] = sum;
        }
    }

    /* The Θ-dependent part of BETA into DEN. */
    i__1 = *ndim;
    for (k = 1; k <= i__1; ++k)
    {
        sum = zero;
        for (i__ = 1; i__ <= 5; ++i__)
        {
            par[i__ - 1] = half * prod[k + i__ * prod_dim1] * wvec[k + i__ * wvec_dim1];
            sum += par[i__ - 1];
        }
        den[0] = den[0] - par[0] - sum;
        tempa = prod[k + prod_dim1] * wvec[k + (wvec_dim1 << 1)] + prod[k + (prod_dim1 << 1)] * wvec[k + wvec_dim1];
        tempb = prod[k + (prod_dim1 << 1)] * wvec[k + (wvec_dim1 << 2)] +
                prod[k + (prod_dim1 << 2)] * wvec[k + (wvec_dim1 << 1)];
        tempc = prod[k + prod_dim1 * 3] * wvec[k + wvec_dim1 * 5] + prod[k + prod_dim1 * 5] * wvec[k + wvec_dim1 * 3];
        den[1] = den[1] - tempa - half * (tempb + tempc);
        den[5] -= half * (tempb - tempc);
        tempa = prod[k + prod_dim1] * wvec[k + wvec_dim1 * 3] + prod[k + prod_dim1 * 3] * wvec[k + wvec_dim1];
        tempb =
            prod[k + (prod_dim1 << 1)] * wvec[k + wvec_dim1 * 5] + prod[k + prod_dim1 * 5] * wvec[k + (wvec_dim1 << 1)];
        tempc =
            prod[k + prod_dim1 * 3] * wvec[k + (wvec_dim1 << 2)] + prod[k + (prod_dim1 << 2)] * wvec[k + wvec_dim1 * 3];
        den[2] = den[2] - tempa - half * (tempb - tempc);
        den[6] -= half * (tempb + tempc);
        tempa = prod[k + prod_dim1] * wvec[k + (wvec_dim1 << 2)] + prod[k + (prod_dim1 << 2)] * wvec[k + wvec_dim1];
        den[3] = den[3] - tempa - par[1] + par[2];
        tempa = prod[k + prod_dim1] * wvec[k + wvec_dim1 * 5] + prod[k + prod_dim1 * 5] * wvec[k + wvec_dim1];
        tempb =
            prod[k + (prod_dim1 << 1)] * wvec[k + wvec_dim1 * 3] + prod[k + prod_dim1 * 3] * wvec[k + (wvec_dim1 << 1)];
        den[4] = den[4] - tempa - half * tempb;
        den[7] = den[7] - par[3] + par[4];
        tempa =
            prod[k + (prod_dim1 << 2)] * wvec[k + wvec_dim1 * 5] + prod[k + prod_dim1 * 5] * wvec[k + (wvec_dim1 << 2)];
        den[8] -= half * tempa;
    }

    /* Extend DEN to all the DENOM coefficients. */
    sum = zero;
    for (i__ = 1; i__ <= 5; ++i__)
    {
        d__1 = prod[*knew + i__ * prod_dim1];
        par[i__ - 1] = half * (d__1 * d__1);
        sum += par[i__ - 1];
    }
    denex[0] = alpha * den[0] + par[0] + sum;
    tempa = two * prod[*knew + prod_dim1] * prod[*knew + (prod_dim1 << 1)];
    tempb = prod[*knew + (prod_dim1 << 1)] * prod[*knew + (prod_dim1 << 2)];
    tempc = prod[*knew + prod_dim1 * 3] * prod[*knew + prod_dim1 * 5];
    denex[1] = alpha * den[1] + tempa + tempb + tempc;
    denex[5] = alpha * den[5] + tempb - tempc;
    tempa = two * prod[*knew + prod_dim1] * prod[*knew + prod_dim1 * 3];
    tempb = prod[*knew + (prod_dim1 << 1)] * prod[*knew + prod_dim1 * 5];
    tempc = prod[*knew + prod_dim1 * 3] * prod[*knew + (prod_dim1 << 2)];
    denex[2] = alpha * den[2] + tempa + tempb - tempc;
    denex[6] = alpha * den[6] + tempb + tempc;
    tempa = two * prod[*knew + prod_dim1] * prod[*knew + (prod_dim1 << 2)];
    denex[3] = alpha * den[3] + tempa + par[1] - par[2];
    tempa = two * prod[*knew + prod_dim1] * prod[*knew + prod_dim1 * 5];
    denex[4] = alpha * den[4] + tempa + prod[*knew + (prod_dim1 << 1)] * prod[*knew + prod_dim1 * 3];
    denex[7] = alpha * den[7] + par[3] - par[4];
    denex[8] = alpha * den[8] + prod[*knew + (prod_dim1 << 2)] * prod[*knew + prod_dim1 * 5];

    /* The angle maximizing |DENOM|. */
    sum = denex[0] + denex[1] + denex[3] + denex[5] + denex[7];
    denold = sum;
    denmax = sum;
    isave = 0;
    iu = 49;
    temp = twopi / static_cast<T>(iu + 1);
    par[0] = one;
    i__1 = iu;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        angle = static_cast<T>(i__) * temp;
        par[1] = std::cos(angle);
        par[2] = std::sin(angle);
        for (j = 4; j <= 8; j += 2)
        {
            par[j - 1] = par[1] * par[j - 3] - par[2] * par[j - 2];
            par[j] = par[1] * par[j - 2] + par[2] * par[j - 3];
        }
        sumold = sum;
        sum = zero;
        for (j = 1; j <= 9; ++j)
        {
            sum += denex[j - 1] * par[j - 1];
        }
        if (std::fabs(sum) > std::fabs(denmax))
        {
            denmax = sum;
            isave = i__;
            tempa = sumold;
        }
        else if (i__ == isave + 1)
        {
            tempb = sum;
        }
    }
    if (isave == 0)
    {
        tempa = sum;
    }
    if (isave == iu)
    {
        tempb = denold;
    }
    step = zero;
    if (tempa != tempb)
    {
        tempa -= denmax;
        tempb -= denmax;
        step = half * (tempa - tempb) / (tempa + tempb);
    }
    angle = temp * (static_cast<T>(isave) + step);

    /* New denominator parameters, VLAG and D; convergence test. */
    par[1] = std::cos(angle);
    par[2] = std::sin(angle);
    for (j = 4; j <= 8; j += 2)
    {
        par[j - 1] = par[1] * par[j - 3] - par[2] * par[j - 2];
        par[j] = par[1] * par[j - 2] + par[2] * par[j - 3];
    }
    *beta = zero;
    denmax = zero;
    for (j = 1; j <= 9; ++j)
    {
        *beta += den[j - 1] * par[j - 1];
        denmax += denex[j - 1] * par[j - 1];
    }
    i__1 = *ndim;
    for (k = 1; k <= i__1; ++k)
    {
        vlag[k] = zero;
        for (j = 1; j <= 5; ++j)
        {
            vlag[k] += prod[k + j * prod_dim1] * par[j - 1];
        }
    }
    tau = vlag[*knew];
    dd = zero;
    tempa = zero;
    tempb = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        d__[i__] = par[1] * d__[i__] + par[2] * s[i__];
        w[i__] = xopt[i__] + d__[i__];
        d__1 = d__[i__];
        dd += d__1 * d__1;
        tempa += d__[i__] * w[i__];
        tempb += w[i__] * w[i__];
    }
    if (iterc >= *n)
    {
        goto L340;
    }
    if (iterc > 1)
    {
        densav = densav >= denold ? densav : denold;
    }
    if (std::fabs(denmax) <= std::fabs(densav) * static_cast<T>(1.1))
    {
        goto L340;
    }
    densav = denmax;

    /* S ← half the denominator's gradient w.r.t. D; next iteration. */
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        temp = tempa * xopt[i__] + tempb * d__[i__] - vlag[*npt + i__];
        s[i__] = tau * bmat[*knew + i__ * bmat_dim1] + alpha * temp;
    }
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        sum = zero;
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
        {
            sum += xpt[k + j * xpt_dim1] * w[j];
        }
        if (std::isinf(tau * w[*n + k]) || std::isinf(alpha * vlag[k]))
        {
            return Rc::RoundoffLimited;
        }
        temp = (tau * w[*n + k] - alpha * vlag[k]) * sum;
        i__2 = *n;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            s[i__] += temp * xpt[k + i__ * xpt_dim1];
        }
    }
    ss = zero;
    ds = zero;
    i__2 = *n;
    for (i__ = 1; i__ <= i__2; ++i__)
    {
        d__1 = s[i__];
        ss += d__1 * d__1;
        ds += d__[i__] * s[i__];
    }
    ssden = dd * ss - ds * ds;
    if (ssden >= dd * static_cast<T>(1e-8) * ss)
    {
        goto L70;
    }

/* W before the return. (The reference's lb/ub truncation hack is NOT ported — scope pinned.) */
L340:
    i__2 = *ndim;
    for (k = 1; k <= i__2; ++k)
    {
        w[k] = zero;
        for (j = 1; j <= 5; ++j)
        {
            w[k] += wvec[k + j * wvec_dim1] * par[j - 1];
        }
    }
    vlag[*kopt] += one;
    return Rc::Success;
} /* bigden */

// --------------------------------------------------------------------------------------------- newuob_
// The main NEWUOA driver: builds the initial 2n+1/NPT interpolation set, then iterates trust-region steps
// (trsapp) and model steps (biglag, with the bigden fallback when DENOM cancels), maintaining the quadratic
// model (GQ/HQ/PQ) and the inverse factorization (BMAT/ZMAT/IDZ) via update, under Powell's ρ schedule with
// the XBASE shift. `Calfun` is `T calfun(int n, const T* x)` over the 0-based x. rhoend is computed from
// xtol_rel·rhobeg EXACTLY like the reference (the e2e diff passes the same xtol_rel both sides). The
// reference's `if (lb && ub)` clamps are NOT ported (scope pinned); force/time stops not carried.
template <typename T, typename Calfun>
[[nodiscard]] inline Rc newuob(const int* n, const int* npt, T* x, const T* rhobeg, T xtol_rel, Stop<T>* stop, T* minf,
                               Calfun&& calfun, T* xbase, T* xopt, T* xnew, T* xpt, T* fval, T* gq, T* hq, T* pq,
                               T* bmat, T* zmat, const int* ndim, T* d__, T* vlag, T* w)
{
    /* System generated locals */
    int xpt_dim1, xpt_offset, bmat_dim1, bmat_offset, zmat_dim1, zmat_offset, i__1, i__2, i__3;
    T d__1, d__2, d__3;

    /* Local variables */
    T f = static_cast<T>(0);
    int i__, j, k, ih, nf, nh, ip, jp;
    T dx;
    int np, nfm;
    T one;
    int idz = 1;
    T dsq, rho = static_cast<T>(0);
    int ipt = 0, jpt = 0;
    T sum, fbeg = static_cast<T>(0), diff, half, beta = static_cast<T>(0);
    int nfmm;
    T gisq;
    int knew = 0;
    T temp, suma, sumb, fopt = std::numeric_limits<T>::infinity(), bsum, gqsq;
    int kopt = 1, nptm;
    T zero, xipt = static_cast<T>(0), xjpt = static_cast<T>(0), sumz, diffa = static_cast<T>(0),
            diffb = static_cast<T>(0), diffc = static_cast<T>(0), hdiag, alpha = static_cast<T>(0),
            delta = static_cast<T>(0), recip, reciq, fsave;
    int ksave, nfsav = 0, itemp;
    T dnorm = static_cast<T>(0), ratio = static_cast<T>(0), dstep, tenth, vquad;
    int ktemp;
    T tempq;
    int itest = 0;
    T rhosq;
    T detrat, crvmin = static_cast<T>(0);
    T distsq;
    T xoptsq = static_cast<T>(0);
    T rhoend;
    Rc rc = Rc::Success, rc2;

    /* SGJ, 2008: rhoend from the stop info (here: xtol_rel only — no xtol_abs vector in Cerid). */
    rhoend = xtol_rel * (*rhobeg);

    /* Parameter adjustments */
    zmat_dim1 = *npt;
    zmat_offset = 1 + zmat_dim1;
    zmat -= zmat_offset;
    xpt_dim1 = *npt;
    xpt_offset = 1 + xpt_dim1;
    xpt -= xpt_offset;
    --x;
    --xbase;
    --xopt;
    --xnew;
    --fval;
    --gq;
    --hq;
    --pq;
    bmat_dim1 = *ndim;
    bmat_offset = 1 + bmat_dim1;
    bmat -= bmat_offset;
    --d__;
    --vlag;
    --w;

    /* Function Body */
    half = static_cast<T>(.5);
    one = static_cast<T>(1.);
    tenth = static_cast<T>(.1);
    zero = static_cast<T>(0.);
    np = *n + 1;
    nh = *n * np / 2;
    nptm = *npt - np;
    dstep = zero;
    ksave = 0;

    /* Zero XPT, BMAT, HQ, PQ, ZMAT. */
    i__1 = *n;
    for (j = 1; j <= i__1; ++j)
    {
        xbase[j] = x[j];
        i__2 = *npt;
        for (k = 1; k <= i__2; ++k)
        {
            xpt[k + j * xpt_dim1] = zero;
        }
        i__2 = *ndim;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            bmat[i__ + j * bmat_dim1] = zero;
        }
    }
    i__2 = nh;
    for (ih = 1; ih <= i__2; ++ih)
    {
        hq[ih] = zero;
    }
    i__2 = *npt;
    for (k = 1; k <= i__2; ++k)
    {
        pq[k] = zero;
        i__1 = nptm;
        for (j = 1; j <= i__1; ++j)
        {
            zmat[k + j * zmat_dim1] = zero;
        }
    }

    /* The initialization procedure: the next initial point's displacement goes into XPT(NF, ·). */
    rhosq = *rhobeg * *rhobeg;
    recip = one / rhosq;
    reciq = std::sqrt(half) / rhosq;
    nf = 0;
L50:
    nfm = nf;
    nfmm = nf - *n;
    ++nf;
    if (nfm <= *n << 1)
    {
        if (nfm >= 1 && nfm <= *n)
        {
            xpt[nf + nfm * xpt_dim1] = *rhobeg;
        }
        else if (nfm > *n)
        {
            xpt[nf + nfmm * xpt_dim1] = -(*rhobeg);
        }
    }
    else
    {
        itemp = (nfmm - 1) / *n;
        jpt = nfm - itemp * *n - *n;
        ipt = jpt + itemp;
        if (ipt > *n)
        {
            itemp = jpt;
            jpt = ipt - *n;
            ipt = itemp;
        }
        xipt = *rhobeg;
        if (fval[ipt + np] < fval[ipt + 1])
        {
            xipt = -xipt;
        }
        xjpt = *rhobeg;
        if (fval[jpt + np] < fval[jpt + 1])
        {
            xjpt = -xjpt;
        }
        xpt[nf + ipt * xpt_dim1] = xipt;
        xpt[nf + jpt * xpt_dim1] = xjpt;
    }

    /* The next F (label 70 follows immediately). */
    i__1 = *n;
    for (j = 1; j <= i__1; ++j)
    {
        x[j] = xpt[nf + j * xpt_dim1] + xbase[j];
    }
    goto L310;
L70:
    fval[nf] = f;
    if (nf == 1)
    {
        fbeg = f;
        fopt = f;
        kopt = 1;
    }
    else if (f < fopt)
    {
        fopt = f;
        kopt = nf;
    }

    /* Nonzero initial BMAT + model elements for NF ≤ 2N+1; else the off-diagonal Lagrange second
       derivatives. */
    if (nfm <= *n << 1)
    {
        if (nfm >= 1 && nfm <= *n)
        {
            gq[nfm] = (f - fbeg) / *rhobeg;
            if (*npt < nf + *n)
            {
                bmat[nfm * bmat_dim1 + 1] = -one / *rhobeg;
                bmat[nf + nfm * bmat_dim1] = one / *rhobeg;
                bmat[*npt + nfm + nfm * bmat_dim1] = -half * rhosq;
            }
        }
        else if (nfm > *n)
        {
            bmat[nf - *n + nfmm * bmat_dim1] = half / *rhobeg;
            bmat[nf + nfmm * bmat_dim1] = -half / *rhobeg;
            zmat[nfmm * zmat_dim1 + 1] = -reciq - reciq;
            zmat[nf - *n + nfmm * zmat_dim1] = reciq;
            zmat[nf + nfmm * zmat_dim1] = reciq;
            ih = nfmm * (nfmm + 1) / 2;
            temp = (fbeg - f) / *rhobeg;
            hq[ih] = (gq[nfmm] - temp) / *rhobeg;
            gq[nfmm] = half * (gq[nfmm] + temp);
        }
    }
    else
    {
        ih = ipt * (ipt - 1) / 2 + jpt;
        if (xipt < zero)
        {
            ipt += *n;
        }
        if (xjpt < zero)
        {
            jpt += *n;
        }
        zmat[nfmm * zmat_dim1 + 1] = recip;
        zmat[nf + nfmm * zmat_dim1] = recip;
        zmat[ipt + 1 + nfmm * zmat_dim1] = -recip;
        zmat[jpt + 1 + nfmm * zmat_dim1] = -recip;
        hq[ih] = (fbeg - fval[ipt + 1] - fval[jpt + 1] + f) / (xipt * xjpt);
    }
    if (nf < *npt)
    {
        goto L50;
    }

    /* The initial model is complete: begin iterating. */
    rho = *rhobeg;
    delta = rho;
    idz = 1;
    diffa = zero;
    diffb = zero;
    itest = 0;
    xoptsq = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        xopt[i__] = xpt[kopt + i__ * xpt_dim1];
        d__1 = xopt[i__];
        xoptsq += d__1 * d__1;
    }
L90:
    nfsav = nf;

/* The next trust-region step (KNEW = −1 ⇒ the next F improves the model). */
L100:
    knew = 0;
    rc2 = trsapp<T>(n, npt, &xopt[1], &xpt[xpt_offset], &gq[1], &hq[1], &pq[1], &delta, &d__[1], &w[1], &w[np],
                    &w[np + *n], &w[np + (*n << 1)], &crvmin);
    if (static_cast<int>(rc2) < 0)
    {
        rc = rc2;
        goto L530;
    }
    dsq = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        d__1 = d__[i__];
        dsq += d__1 * d__1;
    }
    d__1 = delta;
    d__2 = std::sqrt(dsq);
    dnorm = d__1 <= d__2 ? d__1 : d__2;
    if (dnorm < half * rho)
    {
        knew = -1;
        delta = tenth * delta;
        ratio = static_cast<T>(-1.);
        if (delta <= rho * static_cast<T>(1.5))
        {
            delta = rho;
        }
        if (nf <= nfsav + 2)
        {
            goto L460;
        }
        temp = crvmin * static_cast<T>(.125) * rho * rho;
        d__1 = diffa >= diffb ? diffa : diffb;
        if (temp <= (d__1 >= diffc ? d__1 : diffc))
        {
            goto L460;
        }
        goto L490;
    }

/* Shift XBASE if XOPT strayed too far (BMAT changes independent of ZMAT first). */
L120:
    if (dsq <= xoptsq * static_cast<T>(.001))
    {
        tempq = xoptsq * static_cast<T>(.25);
        i__1 = *npt;
        for (k = 1; k <= i__1; ++k)
        {
            sum = zero;
            i__2 = *n;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                sum += xpt[k + i__ * xpt_dim1] * xopt[i__];
            }
            temp = pq[k] * sum;
            sum -= half * xoptsq;
            w[*npt + k] = sum;
            i__2 = *n;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                gq[i__] += temp * xpt[k + i__ * xpt_dim1];
                xpt[k + i__ * xpt_dim1] -= half * xopt[i__];
                vlag[i__] = bmat[k + i__ * bmat_dim1];
                w[i__] = sum * xpt[k + i__ * xpt_dim1] + tempq * xopt[i__];
                ip = *npt + i__;
                i__3 = i__;
                for (j = 1; j <= i__3; ++j)
                {
                    bmat[ip + j * bmat_dim1] = bmat[ip + j * bmat_dim1] + vlag[i__] * w[j] + w[i__] * vlag[j];
                }
            }
        }

        /* The ZMAT-dependent BMAT revisions. */
        i__3 = nptm;
        for (k = 1; k <= i__3; ++k)
        {
            sumz = zero;
            i__2 = *npt;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                sumz += zmat[i__ + k * zmat_dim1];
                w[i__] = w[*npt + i__] * zmat[i__ + k * zmat_dim1];
            }
            i__2 = *n;
            for (j = 1; j <= i__2; ++j)
            {
                sum = tempq * sumz * xopt[j];
                i__1 = *npt;
                for (i__ = 1; i__ <= i__1; ++i__)
                {
                    sum += w[i__] * xpt[i__ + j * xpt_dim1];
                }
                vlag[j] = sum;
                if (k < idz)
                {
                    sum = -sum;
                }
                i__1 = *npt;
                for (i__ = 1; i__ <= i__1; ++i__)
                {
                    bmat[i__ + j * bmat_dim1] += sum * zmat[i__ + k * zmat_dim1];
                }
            }
            i__1 = *n;
            for (i__ = 1; i__ <= i__1; ++i__)
            {
                ip = i__ + *npt;
                temp = vlag[i__];
                if (k < idz)
                {
                    temp = -temp;
                }
                i__2 = i__;
                for (j = 1; j <= i__2; ++j)
                {
                    bmat[ip + j * bmat_dim1] += temp * vlag[j];
                }
            }
        }

        /* Complete the shift (model parameters included). */
        ih = 0;
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
        {
            w[j] = zero;
            i__1 = *npt;
            for (k = 1; k <= i__1; ++k)
            {
                w[j] += pq[k] * xpt[k + j * xpt_dim1];
                xpt[k + j * xpt_dim1] -= half * xopt[j];
            }
            i__1 = j;
            for (i__ = 1; i__ <= i__1; ++i__)
            {
                ++ih;
                if (i__ < j)
                {
                    gq[j] += hq[ih] * xopt[i__];
                }
                gq[i__] += hq[ih] * xopt[j];
                hq[ih] = hq[ih] + w[i__] * xopt[j] + xopt[i__] * w[j];
                bmat[*npt + i__ + j * bmat_dim1] = bmat[*npt + j + i__ * bmat_dim1];
            }
        }
        i__1 = *n;
        for (j = 1; j <= i__1; ++j)
        {
            xbase[j] += xopt[j];
            xopt[j] = zero;
        }
        xoptsq = zero;
    }

    /* The model step when KNEW > 0 (BIGDEN may replace it if DENOM cancels). */
    if (knew > 0)
    {
        rc2 = biglag<T>(n, npt, &xopt[1], &xpt[xpt_offset], &bmat[bmat_offset], &zmat[zmat_offset], &idz, ndim, &knew,
                        &dstep, &d__[1], &alpha, &vlag[1], &vlag[*npt + 1], &w[1], &w[np], &w[np + *n]);
        if (static_cast<int>(rc2) < 0)
        {
            rc = rc2;
            goto L530;
        }
    }

    /* VLAG and BETA for the current D (Wcheck's first NPT components in W). */
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        suma = zero;
        sumb = zero;
        sum = zero;
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
        {
            suma += xpt[k + j * xpt_dim1] * d__[j];
            sumb += xpt[k + j * xpt_dim1] * xopt[j];
            sum += bmat[k + j * bmat_dim1] * d__[j];
        }
        w[k] = suma * (half * suma + sumb);
        vlag[k] = sum;
    }
    beta = zero;
    i__1 = nptm;
    for (k = 1; k <= i__1; ++k)
    {
        sum = zero;
        i__2 = *npt;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            sum += zmat[i__ + k * zmat_dim1] * w[i__];
        }
        if (k < idz)
        {
            beta += sum * sum;
            sum = -sum;
        }
        else
        {
            beta -= sum * sum;
        }
        i__2 = *npt;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            vlag[i__] += sum * zmat[i__ + k * zmat_dim1];
        }
    }
    bsum = zero;
    dx = zero;
    i__2 = *n;
    for (j = 1; j <= i__2; ++j)
    {
        sum = zero;
        i__1 = *npt;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            sum += w[i__] * bmat[i__ + j * bmat_dim1];
        }
        bsum += sum * d__[j];
        jp = *npt + j;
        i__1 = *n;
        for (k = 1; k <= i__1; ++k)
        {
            sum += bmat[jp + k * bmat_dim1] * d__[k];
        }
        vlag[jp] = sum;
        bsum += sum * d__[j];
        dx += d__[j] * xopt[j];
    }
    beta = dx * dx + dsq * (xoptsq + dx + dx + half * dsq) + beta - bsum;
    vlag[kopt] += one;

    /* BIGDEN replaces D when the DENOM cancellation is unacceptable (XNEW = workspace). */
    if (knew > 0)
    {
        d__1 = vlag[knew];
        if (d__1 == static_cast<T>(0))
        {
            rc = Rc::RoundoffLimited;
            goto L530;
        }
        temp = one + alpha * beta / (d__1 * d__1);
        if (std::fabs(temp) <= static_cast<T>(.8))
        {
            rc2 = bigden<T>(n, npt, &xopt[1], &xpt[xpt_offset], &bmat[bmat_offset], &zmat[zmat_offset], &idz, ndim,
                            &kopt, &knew, &d__[1], &w[1], &vlag[1], &beta, &xnew[1], &w[*ndim + 1], &w[*ndim * 6 + 1]);
            if (static_cast<int>(rc2) < 0)
            {
                rc = rc2;
                goto L530;
            }
        }
    }

/* The next objective value. */
L290:
    i__2 = *n;
    for (i__ = 1; i__ <= i__2; ++i__)
    {
        xnew[i__] = xopt[i__] + d__[i__];
        x[i__] = xbase[i__] + xnew[i__];
    }
    ++nf;
L310:
    if (stop->nevals > 0)
    {
        if (stop_evals(*stop))
        {
            rc = Rc::MaxevalReached;
        }
    }
    if (rc != Rc::Success)
    {
        goto L530;
    }

    ++stop->nevals;
    f = calfun(*n, &x[1]);
    if (f < stop->minf_max)
    {
        rc = Rc::MinfMaxReached;
        goto L530;
    }

    if (nf <= *npt)
    {
        goto L70;
    }
    if (knew == -1)
    {
        goto L530;
    }

    /* The model's prediction of the F-change for step D; DIFF = its error. */
    vquad = zero;
    ih = 0;
    i__2 = *n;
    for (j = 1; j <= i__2; ++j)
    {
        vquad += d__[j] * gq[j];
        i__1 = j;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            ++ih;
            temp = d__[i__] * xnew[j] + d__[j] * xopt[i__];
            if (i__ == j)
            {
                temp = half * temp;
            }
            vquad += temp * hq[ih];
        }
    }
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        vquad += pq[k] * w[k];
    }
    diff = f - fopt - vquad;
    diffc = diffb;
    diffb = diffa;
    diffa = std::fabs(diff);
    if (dnorm > rho)
    {
        nfsav = nf;
    }

    /* FOPT/XOPT update; KNEW > 0 = a model (not trust-region) step. */
    fsave = fopt;
    if (f < fopt)
    {
        fopt = f;
        xoptsq = zero;
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            xopt[i__] = xnew[i__];
            d__1 = xopt[i__];
            xoptsq += d__1 * d__1;
        }
        if (stop_ftol(*stop, fopt, fsave))
        {
            rc = Rc::FtolReached;
            goto L530;
        }
    }
    ksave = knew;
    if (knew > 0)
    {
        goto L410;
    }

    /* DELTA after a trust-region step. */
    if (vquad >= zero)
    {
        goto L530;
    }
    ratio = (f - fsave) / vquad;
    if (ratio <= tenth)
    {
        delta = half * dnorm;
    }
    else if (ratio <= static_cast<T>(.7))
    {
        d__1 = half * delta;
        delta = d__1 >= dnorm ? d__1 : dnorm;
    }
    else
    {
        d__1 = half * delta;
        d__2 = dnorm + dnorm;
        delta = d__1 >= d__2 ? d__1 : d__2;
    }
    if (delta <= rho * static_cast<T>(1.5))
    {
        delta = rho;
    }

    /* KNEW ← the interpolation point to delete. */
    d__2 = tenth * delta;
    d__1 = d__2 >= rho ? d__2 : rho;
    rhosq = d__1 * d__1;
    ktemp = 0;
    detrat = zero;
    if (f >= fsave)
    {
        ktemp = kopt;
        detrat = one;
    }
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        hdiag = zero;
        i__2 = nptm;
        for (j = 1; j <= i__2; ++j)
        {
            temp = one;
            if (j < idz)
            {
                temp = -one;
            }
            d__1 = zmat[k + j * zmat_dim1];
            hdiag += temp * (d__1 * d__1);
        }
        d__2 = vlag[k];
        d__1 = beta * hdiag + d__2 * d__2;
        temp = std::fabs(d__1);
        distsq = zero;
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
        {
            d__1 = xpt[k + j * xpt_dim1] - xopt[j];
            distsq += d__1 * d__1;
        }
        if (distsq > rhosq)
        {
            d__1 = distsq / rhosq;
            temp *= d__1 * (d__1 * d__1);
        }
        if (temp > detrat && k != ktemp)
        {
            detrat = temp;
            knew = k;
        }
    }
    if (knew == 0)
    {
        goto L460;
    }

/* Update BMAT/ZMAT/IDZ to move point KNEW; start the model update (explicit second derivatives). */
L410:
    update<T>(n, npt, &bmat[bmat_offset], &zmat[zmat_offset], &idz, ndim, &vlag[1], &beta, &knew, &w[1]);
    fval[knew] = f;
    ih = 0;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        temp = pq[knew] * xpt[knew + i__ * xpt_dim1];
        i__2 = i__;
        for (j = 1; j <= i__2; ++j)
        {
            ++ih;
            hq[ih] += temp * xpt[knew + j * xpt_dim1];
        }
    }
    pq[knew] = zero;

    /* The other second-derivative parameters, the gradient, and the new point itself. */
    i__2 = nptm;
    for (j = 1; j <= i__2; ++j)
    {
        temp = diff * zmat[knew + j * zmat_dim1];
        if (j < idz)
        {
            temp = -temp;
        }
        i__1 = *npt;
        for (k = 1; k <= i__1; ++k)
        {
            pq[k] += temp * zmat[k + j * zmat_dim1];
        }
    }
    gqsq = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        gq[i__] += diff * bmat[knew + i__ * bmat_dim1];
        d__1 = gq[i__];
        gqsq += d__1 * d__1;
        xpt[knew + i__ * xpt_dim1] = xnew[i__];
    }

    /* Possibly replace the model by the least-Frobenius-norm interpolant (the ITEST mechanism). */
    if (ksave == 0 && delta == rho)
    {
        if (std::fabs(ratio) > static_cast<T>(.01))
        {
            itest = 0;
        }
        else
        {
            i__1 = *npt;
            for (k = 1; k <= i__1; ++k)
            {
                vlag[k] = fval[k] - fval[kopt];
            }
            gisq = zero;
            i__1 = *n;
            for (i__ = 1; i__ <= i__1; ++i__)
            {
                sum = zero;
                i__2 = *npt;
                for (k = 1; k <= i__2; ++k)
                {
                    sum += bmat[k + i__ * bmat_dim1] * vlag[k];
                }
                gisq += sum * sum;
                w[i__] = sum;
            }
            ++itest;
            if (gqsq < gisq * static_cast<T>(100.))
            {
                itest = 0;
            }
            if (itest >= 3)
            {
                i__1 = *n;
                for (i__ = 1; i__ <= i__1; ++i__)
                {
                    gq[i__] = w[i__];
                }
                i__1 = nh;
                for (ih = 1; ih <= i__1; ++ih)
                {
                    hq[ih] = zero;
                }
                i__1 = nptm;
                for (j = 1; j <= i__1; ++j)
                {
                    w[j] = zero;
                    i__2 = *npt;
                    for (k = 1; k <= i__2; ++k)
                    {
                        w[j] += vlag[k] * zmat[k + j * zmat_dim1];
                    }
                    if (j < idz)
                    {
                        w[j] = -w[j];
                    }
                }
                i__1 = *npt;
                for (k = 1; k <= i__1; ++k)
                {
                    pq[k] = zero;
                    i__2 = nptm;
                    for (j = 1; j <= i__2; ++j)
                    {
                        pq[k] += zmat[k + j * zmat_dim1] * w[j];
                    }
                }
                itest = 0;
            }
        }
    }
    if (f < fsave)
    {
        kopt = knew;
    }

    /* Sufficient decrease ⇒ another trust-region step; KSAVE > 0 = the value came from a model step. */
    if (f <= fsave + tenth * vquad)
    {
        goto L100;
    }
    if (ksave > 0)
    {
        goto L100;
    }

    /* Are the interpolation points close enough to the best point? */
    knew = 0;
L460:
    distsq = delta * static_cast<T>(4.) * delta;
    i__2 = *npt;
    for (k = 1; k <= i__2; ++k)
    {
        sum = zero;
        i__1 = *n;
        for (j = 1; j <= i__1; ++j)
        {
            d__1 = xpt[k + j * xpt_dim1] - xopt[j];
            sum += d__1 * d__1;
        }
        if (sum > distsq)
        {
            knew = k;
            distsq = sum;
        }
    }

    /* KNEW > 0 ⇒ set DSTEP and go generate a "model step". */
    if (knew > 0)
    {
        d__2 = tenth * std::sqrt(distsq);
        d__3 = half * delta;
        d__1 = d__2 <= d__3 ? d__2 : d__3;
        dstep = d__1 >= rho ? d__1 : rho;
        dsq = dstep * dstep;
        goto L120;
    }
    if (ratio > zero)
    {
        goto L100;
    }
    if ((delta >= dnorm ? delta : dnorm) > rho)
    {
        goto L100;
    }

/* The work at the current RHO is done: the next RHO and DELTA. */
L490:
    if (rho > rhoend)
    {
        delta = half * rho;
        ratio = rho / rhoend;
        if (ratio <= static_cast<T>(16.))
        {
            rho = rhoend;
        }
        else if (ratio <= static_cast<T>(250.))
        {
            rho = std::sqrt(ratio) * rhoend;
        }
        else
        {
            rho = tenth * rho;
        }
        delta = delta >= rho ? delta : rho;
        goto L90;
    }

    /* Return — after one more Newton-Raphson step if it was too short to have been tried. */
    if (knew == -1)
    {
        goto L290;
    }
    rc = Rc::XtolReached;
L530:
    if (fopt <= f)
    {
        i__2 = *n;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            x[i__] = xbase[i__] + xopt[i__];
        }
        f = fopt;
    }
    *minf = f;
    return rc;
} /* newuob */

} // namespace detail::newuoa_impl

// ------------------------------------------------------------------------------------------ public driver
// min f(x), UNCONSTRAINED (the classic NEWUOA; bounds are BOBYQA's job). Value-only. n must be ≥ 2 (the
// reference's requirement; n = 0 trivially succeeds, n = 1 is rejected by assert — use Brent/NM).
template <typename T>
[[nodiscard]] OptResult<T> minimize_newuoa(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                           crd::memory::IAllocator* alloc, const NewuoaOptions<T>& no = {})
{
    namespace ni = detail::newuoa_impl;
    const crd::usize nn = obj.n();
    CRD_ASSERT_MSG(x0.size() == nn, "minimize_newuoa: x0 size mismatch");

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
    CRD_ASSERT_MSG(nn >= 2, "minimize_newuoa: n must be >= 2 (the reference's requirement)");

    const int n = static_cast<int>(nn);
    int npt = no.npt > 0 ? static_cast<int>(no.npt) : 2 * n + 1;
    const int npt_max = (n + 2) * (n + 1) / 2;
    npt = npt < n + 2 ? n + 2 : (npt > npt_max ? npt_max : npt);
    const int np = n + 1;
    const int nptm = npt - np;
    const int ndim = npt + n;

    // The reference w-partition from newuoa().
    crd::containers::Array<T> w(alloc);
    w.resize(static_cast<crd::usize>((npt + 13) * (npt + n) + 3 * (n * (n + 3)) / 2));
    T* wp = w.data() - 1; // 1-based view
    const int ixb = 1;
    const int ixo = ixb + n;
    const int ixn = ixo + n;
    const int ixp = ixn + n;
    const int ifv = ixp + n * npt;
    const int igq = ifv + npt;
    const int ihq = igq + n;
    const int ipq = ihq + n * np / 2;
    const int ibmat = ipq + npt;
    const int izmat = ibmat + ndim * n;
    const int id = izmat + npt * nptm;
    const int ivl = id + n;
    const int iw = ivl + ndim;

    ni::Stop<T> stop;
    stop.maxeval = no.max_evals > 0 ? static_cast<int>(no.max_evals) : 1000 * (n + 1);
    stop.ftol_rel = no.ftol_rel;
    stop.ftol_abs = no.ftol_abs;

    crd::usize fn_evals = 0;
    auto calfun = [&](int cn, const T* xx) -> T
    {
        ++fn_evals;
        return obj.value({xx, static_cast<crd::usize>(cn)});
    };

    T minf = std::numeric_limits<T>::infinity();
    const T rhobeg = no.rhobeg;
    const T xtol_rel = no.rhoend / no.rhobeg; // the e2e shim passes the same value (bit-identical rhoend)
    const ni::Rc rc = ni::newuob<T>(&n, &npt, result.x.data(), &rhobeg, xtol_rel, &stop, &minf, calfun, &wp[ixb],
                                    &wp[ixo], &wp[ixn], &wp[ixp], &wp[ifv], &wp[igq], &wp[ihq], &wp[ipq], &wp[ibmat],
                                    &wp[izmat], &ndim, &wp[id], &wp[ivl], &wp[iw]);

    result.fx = minf;
    result.fn_evals = fn_evals;
    result.iterations = static_cast<crd::usize>(stop.nevals); // NEWUOA counts evaluations
    switch (rc)
    {
        case ni::Rc::XtolReached:
        case ni::Rc::FtolReached:
        case ni::Rc::MinfMaxReached:
        case ni::Rc::Success:
            result.status = OptStatus::Success;
            break;
        case ni::Rc::MaxevalReached:
            result.status = OptStatus::MaxIterations;
            break;
        case ni::Rc::RoundoffLimited:
        default:
            result.status = OptStatus::SmallStep; // rounding-limited stall (documented mapping)
            break;
    }
    result.converged = result.status == OptStatus::Success;
    return result;
}

} // namespace crd::hesap::opt
