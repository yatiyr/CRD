#pragma once

// bobyqa.hpp — Phase 3.1.6 v7-p-4: BOBYQA (Powell 2009) — Bound Optimization BY Quadratic Approximation:
// derivative-free minimization of f(x) s.t. xl ≤ x ≤ xu by quadratic interpolation models over NPT points,
// the BOUNDED trust-region subproblem `trsbox_` (truncated CG with active-bound fixing), the alternative
// model-step mover `altmov_` (the bounded Λ_knew maximizer with the Cauchy fallback), the `rescue_`
// re-initialization when the denominator degenerates, and the BMAT/ZMAT rank-2 `update_` (NEWUOA's form
// without the IDZ sign partition — BOBYQA keeps DENOM positive).
//
// ⚠ FAITHFUL PORT of the NLopt C reference (`nlopt/src/algs/bobyqa/bobyqa.c`, MIT) — f2c idiom (1-based
// pointer adjustments, original goto flow, exact literal artifacts) per the L-BFGS-B/COBYLA/NEWUOA playbook;
// the differential harness vs the compiled oracle adjudicates (runtime/examples/bobyqa_difftest.cpp).
// Bounds are NATIVE here (no nested-solver variant to exclude — this IS Powell's bounded method, the reason
// the NEWUOA port could pin the classic unconstrained scope). Stop semantics shared with the COBYLA port
// (detail::cobyla_impl — relstop/Stop verbatim from nlopt stop.c); force/time stops not carried; the NLopt
// wrapper's variable RESCALING layer is NOT ported (the shim diffs at the bobyqa() layer beneath it).
// ADR-0090.
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

#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::opt
{

template <typename T> struct BobyqaOptions
{
    T rhobeg = static_cast<T>(1);    // initial trust-region radius
    T rhoend = static_cast<T>(1e-8); // final radius = the x-accuracy target
    T ftol_rel = static_cast<T>(0);  // relative f-convergence (0 = off; nlopt semantics)
    T ftol_abs = static_cast<T>(0);  // absolute f-convergence (0 = off)
    crd::usize npt = 0;              // interpolation points; 0 ⇒ 2n+1 (Powell's recommendation)
    crd::usize max_evals = 0;        // function-evaluation cap; 0 ⇒ 1000·(n+1)
};

namespace detail::bobyqa_impl
{

using cobyla_impl::Rc;
using cobyla_impl::Stop;
using cobyla_impl::stop_evals;
using cobyla_impl::stop_ftol;

// --------------------------------------------------------------------------------------------- update_
// BMAT/ZMAT rank-2 update for moving point KNEW (Powell 2006 eq. 4.11 form; ZMAT entries below ZTEST are
// treated as zero). No IDZ here — BOBYQA's DENOM stays positive.
template <typename T>
inline void update(const int* n, const int* npt, T* bmat, T* zmat, const int* ndim, T* vlag, const T* beta,
                   const T* denom, const int* knew, T* w)
{
    /* System generated locals */
    int bmat_dim1, bmat_offset, zmat_dim1, zmat_offset, i__1, i__2;
    T d__1, d__2, d__3;

    /* Local variables */
    int i__, j, k, jp;
    T one, tau, temp;
    int nptm;
    T zero, alpha, tempa, tempb, ztest;

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
    ztest = zero;
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        i__2 = nptm;
        for (j = 1; j <= i__2; ++j)
        {
            d__1 = zmat[k + j * zmat_dim1];
            d__2 = ztest;
            d__3 = crd::math::fabs(d__1);
            ztest = d__2 >= d__3 ? d__2 : d__3;
        }
    }
    ztest *= static_cast<T>(1e-20);

    /* Rotations zeroing the KNEW-th row of ZMAT. */
    i__2 = nptm;
    for (j = 2; j <= i__2; ++j)
    {
        d__1 = zmat[*knew + j * zmat_dim1];
        if (crd::math::fabs(d__1) > ztest)
        {
            d__1 = zmat[*knew + zmat_dim1];
            d__2 = zmat[*knew + j * zmat_dim1];
            temp = crd::math::sqrt(d__1 * d__1 + d__2 * d__2);
            tempa = zmat[*knew + zmat_dim1] / temp;
            tempb = zmat[*knew + j * zmat_dim1] / temp;
            i__1 = *npt;
            for (i__ = 1; i__ <= i__1; ++i__)
            {
                temp = tempa * zmat[i__ + zmat_dim1] + tempb * zmat[i__ + j * zmat_dim1];
                zmat[i__ + j * zmat_dim1] = tempa * zmat[i__ + j * zmat_dim1] - tempb * zmat[i__ + zmat_dim1];
                zmat[i__ + zmat_dim1] = temp;
            }
        }
        zmat[*knew + j * zmat_dim1] = zero;
    }

    /* W ← HLAG's KNEW-th column head; the updating-formula parameters. */
    i__2 = *npt;
    for (i__ = 1; i__ <= i__2; ++i__)
    {
        w[i__] = zmat[*knew + zmat_dim1] * zmat[i__ + zmat_dim1];
    }
    alpha = w[*knew];
    tau = vlag[*knew];
    vlag[*knew] -= one;

    /* Complete ZMAT. */
    temp = crd::math::sqrt(*denom);
    tempb = zmat[*knew + zmat_dim1] / temp;
    tempa = tau / temp;
    i__2 = *npt;
    for (i__ = 1; i__ <= i__2; ++i__)
    {
        zmat[i__ + zmat_dim1] = tempa * zmat[i__ + zmat_dim1] - tempb * vlag[i__];
    }

    /* Update BMAT. */
    i__2 = *n;
    for (j = 1; j <= i__2; ++j)
    {
        jp = *npt + j;
        w[jp] = bmat[*knew + j * bmat_dim1];
        tempa = (alpha * vlag[jp] - tau * w[jp]) / *denom;
        tempb = (-(*beta) * w[jp] - tau * vlag[jp]) / *denom;
        i__1 = jp;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            bmat[i__ + j * bmat_dim1] = bmat[i__ + j * bmat_dim1] + tempa * vlag[i__] + tempb * w[i__];
            if (i__ > *npt)
            {
                bmat[jp + (i__ - *npt) * bmat_dim1] = bmat[i__ + j * bmat_dim1];
            }
        }
    }
} /* update */

// --------------------------------------------------------------------------------------------- prelim_
// First-iteration setup of XBASE/XPT/FVAL/GOPT/HQ/PQ/BMAT/ZMAT (+ NF, KOPT); the initial points respect the
// shifted bounds SL/SU (steps flip or shrink at active bounds — the bound-aware analog of NEWUOA's init).
// `Calfun` is `T calfun(int n, const T* x)` over the 0-based x.
template <typename T, typename Calfun>
[[nodiscard]] inline Rc prelim(const int* n, const int* npt, T* x, const T* xl, const T* xu, const T* rhobeg,
                               Stop<T>* stop, Calfun&& calfun, T* xbase, T* xpt, T* fval, T* gopt, T* hq, T* pq,
                               T* bmat, T* zmat, const int* ndim, T* sl, T* su, int* kopt)
{
    /* System generated locals */
    int xpt_dim1, xpt_offset, bmat_dim1, bmat_offset, zmat_dim1, zmat_offset, i__1, i__2;
    T d__1, d__2, d__3, d__4;

    /* Local variables */
    T f;
    int i__, j, k, ih, np, nfm;
    T one;
    int nfx, ipt = 0, jpt = 0;
    T two, fbeg = static_cast<T>(0), diff, half, temp, zero, recip, stepa = static_cast<T>(0),
           stepb = static_cast<T>(0);
    int itemp;
    T rhosq;
    int nf;

    /* Parameter adjustments */
    zmat_dim1 = *npt;
    zmat_offset = 1 + zmat_dim1;
    zmat -= zmat_offset;
    xpt_dim1 = *npt;
    xpt_offset = 1 + xpt_dim1;
    xpt -= xpt_offset;
    --x;
    --xl;
    --xu;
    --xbase;
    --fval;
    --gopt;
    --hq;
    --pq;
    bmat_dim1 = *ndim;
    bmat_offset = 1 + bmat_dim1;
    bmat -= bmat_offset;
    --sl;
    --su;

    /* Function Body */
    half = static_cast<T>(.5);
    one = static_cast<T>(1.);
    two = static_cast<T>(2.);
    zero = static_cast<T>(0.);
    rhosq = *rhobeg * *rhobeg;
    recip = one / rhosq;
    np = *n + 1;

    /* XBASE ← x; zero XPT/BMAT/HQ/PQ/ZMAT. */
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
    i__2 = *n * np / 2;
    for (ih = 1; ih <= i__2; ++ih)
    {
        hq[ih] = zero;
    }
    i__2 = *npt;
    for (k = 1; k <= i__2; ++k)
    {
        pq[k] = zero;
        i__1 = *npt - np;
        for (j = 1; j <= i__1; ++j)
        {
            zmat[k + j * zmat_dim1] = zero;
        }
    }

    /* The initialization loop (XPT(NF+1, ·) gets the next displacement). */
    nf = 0;
L50:
    nfm = nf;
    nfx = nf - *n;
    ++nf;
    if (nfm <= *n << 1)
    {
        if (nfm >= 1 && nfm <= *n)
        {
            stepa = *rhobeg;
            if (su[nfm] == zero)
            {
                stepa = -stepa;
            }
            xpt[nf + nfm * xpt_dim1] = stepa;
        }
        else if (nfm > *n)
        {
            stepa = xpt[nf - *n + nfx * xpt_dim1];
            stepb = -(*rhobeg);
            if (sl[nfx] == zero)
            {
                d__1 = two * *rhobeg;
                d__2 = su[nfx];
                stepb = d__1 <= d__2 ? d__1 : d__2;
            }
            if (su[nfx] == zero)
            {
                d__1 = -two * *rhobeg;
                d__2 = sl[nfx];
                stepb = d__1 >= d__2 ? d__1 : d__2;
            }
            xpt[nf + nfx * xpt_dim1] = stepb;
        }
    }
    else
    {
        itemp = (nfm - np) / *n;
        jpt = nfm - itemp * *n - *n;
        ipt = jpt + itemp;
        if (ipt > *n)
        {
            itemp = jpt;
            jpt = ipt - *n;
            ipt = itemp;
        }
        xpt[nf + ipt * xpt_dim1] = xpt[ipt + 1 + ipt * xpt_dim1];
        xpt[nf + jpt * xpt_dim1] = xpt[jpt + 1 + jpt * xpt_dim1];
    }

    /* The next F (bound-clamped x; exact bound landing where the step hits SL/SU). */
    i__1 = *n;
    for (j = 1; j <= i__1; ++j)
    {
        d__3 = xl[j];
        d__4 = xbase[j] + xpt[nf + j * xpt_dim1];
        d__1 = d__3 >= d__4 ? d__3 : d__4;
        d__2 = xu[j];
        x[j] = d__1 <= d__2 ? d__1 : d__2;
        if (xpt[nf + j * xpt_dim1] == sl[j])
        {
            x[j] = xl[j];
        }
        if (xpt[nf + j * xpt_dim1] == su[j])
        {
            x[j] = xu[j];
        }
    }
    ++stop->nevals;
    f = calfun(*n, &x[1]);
    fval[nf] = f;
    if (nf == 1)
    {
        fbeg = f;
        *kopt = 1;
    }
    else if (f < fval[*kopt])
    {
        *kopt = nf;
    }

    /* Initial BMAT + model for NF ≤ 2N+1 (with the stepa/stepb point switch); else the off-diagonal Lagrange
       second derivatives. */
    if (nf <= (*n << 1) + 1)
    {
        if (nf >= 2 && nf <= *n + 1)
        {
            gopt[nfm] = (f - fbeg) / stepa;
            if (*npt < nf + *n)
            {
                bmat[nfm * bmat_dim1 + 1] = -one / stepa;
                bmat[nf + nfm * bmat_dim1] = one / stepa;
                bmat[*npt + nfm + nfm * bmat_dim1] = -half * rhosq;
            }
        }
        else if (nf >= *n + 2)
        {
            ih = nfx * (nfx + 1) / 2;
            temp = (f - fbeg) / stepb;
            diff = stepb - stepa;
            hq[ih] = two * (temp - gopt[nfx]) / diff;
            gopt[nfx] = (gopt[nfx] * stepb - temp * stepa) / diff;
            if (stepa * stepb < zero)
            {
                if (f < fval[nf - *n])
                {
                    fval[nf] = fval[nf - *n];
                    fval[nf - *n] = f;
                    if (*kopt == nf)
                    {
                        *kopt = nf - *n;
                    }
                    xpt[nf - *n + nfx * xpt_dim1] = stepb;
                    xpt[nf + nfx * xpt_dim1] = stepa;
                }
            }
            bmat[nfx * bmat_dim1 + 1] = -(stepa + stepb) / (stepa * stepb);
            bmat[nf + nfx * bmat_dim1] = -half / xpt[nf - *n + nfx * xpt_dim1];
            bmat[nf - *n + nfx * bmat_dim1] = -bmat[nfx * bmat_dim1 + 1] - bmat[nf + nfx * bmat_dim1];
            zmat[nfx * zmat_dim1 + 1] = crd::math::sqrt(two) / (stepa * stepb);
            zmat[nf + nfx * zmat_dim1] = crd::math::sqrt(half) / rhosq;
            zmat[nf - *n + nfx * zmat_dim1] = -zmat[nfx * zmat_dim1 + 1] - zmat[nf + nfx * zmat_dim1];
        }
    }
    else
    {
        ih = ipt * (ipt - 1) / 2 + jpt;
        zmat[nfx * zmat_dim1 + 1] = recip;
        zmat[nf + nfx * zmat_dim1] = recip;
        zmat[ipt + 1 + nfx * zmat_dim1] = -recip;
        zmat[jpt + 1 + nfx * zmat_dim1] = -recip;
        temp = xpt[nf + ipt * xpt_dim1] * xpt[nf + jpt * xpt_dim1];
        hq[ih] = (fbeg - fval[ipt + 1] - fval[jpt + 1] + f) / temp;
    }
    if (f < stop->minf_max)
    {
        return Rc::MinfMaxReached;
    }
    if (stop_evals(*stop))
    {
        return Rc::MaxevalReached;
    }
    if (nf < *npt)
    {
        goto L50;
    }
    return Rc::Success;
} /* prelim */

// --------------------------------------------------------------------------------------------- altmov_
// The model-improvement mover: XNEW maximizes the denominator along the lines through XOPT and each other
// interpolation point (bound-clipped, with exact bound landing via IBDSAV); XALT is the CONSTRAINED CAUCHY
// step of the KNEW-th Lagrange function tried with both gradient signs (the larger CAUCHY wins). ALPHA ←
// H's KNEW-th diagonal.
template <typename T>
inline void altmov(const int* n, const int* npt, T* xpt, T* xopt, T* bmat, T* zmat, const int* ndim, T* sl, T* su,
                   const int* kopt, const int* knew, const T* adelt, T* xnew, T* xalt, T* alpha, T* cauchy, T* glag,
                   T* hcol, T* w)
{
    /* System generated locals */
    int xpt_dim1, xpt_offset, bmat_dim1, bmat_offset, zmat_dim1, zmat_offset, i__1, i__2;
    T d__1, d__2, d__3, d__4;

    /* Local variables */
    int i__, j, k;
    T ha, gw, one, diff, half;
    int ilbd, isbd;
    T slbd;
    int iubd;
    T vlag, subd, temp;
    int ksav = 0;
    T step = static_cast<T>(0), zero, curv;
    int iflag;
    T scale, csave = static_cast<T>(0), tempa, tempb, tempd, const__, sumin, ggfree;
    int ibdsav = 0;
    T dderiv, bigstp, predsq, presav, distsq, stpsav = static_cast<T>(0), wfixsq, wsqsav;

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
    --sl;
    --su;
    --xnew;
    --xalt;
    --glag;
    --hcol;
    --w;

    /* Function Body */
    half = static_cast<T>(.5);
    one = static_cast<T>(1.);
    zero = static_cast<T>(0.);
    const__ = one + crd::math::sqrt(static_cast<T>(2.));
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        hcol[k] = zero;
    }
    i__1 = *npt - *n - 1;
    for (j = 1; j <= i__1; ++j)
    {
        temp = zmat[*knew + j * zmat_dim1];
        i__2 = *npt;
        for (k = 1; k <= i__2; ++k)
        {
            hcol[k] += temp * zmat[k + j * zmat_dim1];
        }
    }
    *alpha = hcol[*knew];
    ha = half * *alpha;

    /* The KNEW-th Lagrange function's gradient at XOPT. */
    i__2 = *n;
    for (i__ = 1; i__ <= i__2; ++i__)
    {
        glag[i__] = bmat[*knew + i__ * bmat_dim1];
    }
    i__2 = *npt;
    for (k = 1; k <= i__2; ++k)
    {
        temp = zero;
        i__1 = *n;
        for (j = 1; j <= i__1; ++j)
        {
            temp += xpt[k + j * xpt_dim1] * xopt[j];
        }
        temp = hcol[k] * temp;
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            glag[i__] += temp * xpt[k + i__ * xpt_dim1];
        }
    }

    /* Line search through XOPT and each other point: SLBD/SUBD step bounds, PREDSQ the predicted denominator
       square, PRESAV its best admissible value. */
    presav = zero;
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        if (k == *kopt)
        {
            goto L80;
        }
        dderiv = zero;
        distsq = zero;
        i__2 = *n;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            temp = xpt[k + i__ * xpt_dim1] - xopt[i__];
            dderiv += glag[i__] * temp;
            distsq += temp * temp;
        }
        subd = *adelt / crd::math::sqrt(distsq);
        slbd = -subd;
        ilbd = 0;
        iubd = 0;
        sumin = one <= subd ? one : subd;

        /* Revise SLBD/SUBD for the SL/SU bounds. */
        i__2 = *n;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            temp = xpt[k + i__ * xpt_dim1] - xopt[i__];
            if (temp > zero)
            {
                if (slbd * temp < sl[i__] - xopt[i__])
                {
                    slbd = (sl[i__] - xopt[i__]) / temp;
                    ilbd = -i__;
                }
                if (subd * temp > su[i__] - xopt[i__])
                {
                    d__1 = sumin;
                    d__2 = (su[i__] - xopt[i__]) / temp;
                    subd = d__1 >= d__2 ? d__1 : d__2;
                    iubd = i__;
                }
            }
            else if (temp < zero)
            {
                if (slbd * temp > su[i__] - xopt[i__])
                {
                    slbd = (su[i__] - xopt[i__]) / temp;
                    ilbd = i__;
                }
                if (subd * temp < sl[i__] - xopt[i__])
                {
                    d__1 = sumin;
                    d__2 = (sl[i__] - xopt[i__]) / temp;
                    subd = d__1 >= d__2 ? d__1 : d__2;
                    iubd = -i__;
                }
            }
        }

        /* Seek a large |Λ_knew| (the k == KNEW line has the quadratic form; other lines the product form). */
        if (k == *knew)
        {
            diff = dderiv - one;
            step = slbd;
            vlag = slbd * (dderiv - slbd * diff);
            isbd = ilbd;
            temp = subd * (dderiv - subd * diff);
            if (crd::math::fabs(temp) > crd::math::fabs(vlag))
            {
                step = subd;
                vlag = temp;
                isbd = iubd;
            }
            tempd = half * dderiv;
            tempa = tempd - diff * slbd;
            tempb = tempd - diff * subd;
            if (tempa * tempb < zero)
            {
                temp = tempd * tempd / diff;
                if (crd::math::fabs(temp) > crd::math::fabs(vlag))
                {
                    step = tempd / diff;
                    vlag = temp;
                    isbd = 0;
                }
            }
        }
        else
        {
            step = slbd;
            vlag = slbd * (one - slbd);
            isbd = ilbd;
            temp = subd * (one - subd);
            if (crd::math::fabs(temp) > crd::math::fabs(vlag))
            {
                step = subd;
                vlag = temp;
                isbd = iubd;
            }
            if (subd > half)
            {
                if (crd::math::fabs(vlag) < static_cast<T>(.25))
                {
                    step = half;
                    vlag = static_cast<T>(.25);
                    isbd = 0;
                }
            }
            vlag *= dderiv;
        }

        /* PREDSQ for this line; maintain PRESAV. */
        temp = step * (one - step) * distsq;
        predsq = vlag * vlag * (vlag * vlag + ha * temp * temp);
        if (predsq > presav)
        {
            presav = predsq;
            ksav = k;
            stpsav = step;
            ibdsav = isbd;
        }
    L80:;
    }

    /* XNEW with the bounds satisfied EXACTLY. */
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        temp = xopt[i__] + stpsav * (xpt[ksav + i__ * xpt_dim1] - xopt[i__]);
        d__3 = su[i__];
        d__2 = d__3 <= temp ? d__3 : temp;
        d__1 = sl[i__];
        xnew[i__] = d__1 >= d__2 ? d__1 : d__2;
    }
    if (ibdsav < 0)
    {
        xnew[-ibdsav] = sl[-ibdsav];
    }
    if (ibdsav > 0)
    {
        xnew[ibdsav] = su[ibdsav];
    }

    /* The constrained Cauchy step: fixed components accumulate in WFIXSQ; free ones marked BIGSTP. */
    bigstp = *adelt + *adelt;
    iflag = 0;
L100:
    wfixsq = zero;
    ggfree = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        w[i__] = zero;
        d__1 = xopt[i__] - sl[i__];
        d__2 = glag[i__];
        tempa = d__1 <= d__2 ? d__1 : d__2;
        d__1 = xopt[i__] - su[i__];
        d__2 = glag[i__];
        tempb = d__1 >= d__2 ? d__1 : d__2;
        if (tempa > zero || tempb < zero)
        {
            w[i__] = bigstp;
            d__1 = glag[i__];
            ggfree += d__1 * d__1;
        }
    }
    if (ggfree == zero)
    {
        *cauchy = zero;
        goto L200;
    }

/* Can more components of W be fixed? */
L120:
    temp = *adelt * *adelt - wfixsq;
    if (temp > zero)
    {
        wsqsav = wfixsq;
        step = crd::math::sqrt(temp / ggfree);
        ggfree = zero;
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            if (w[i__] == bigstp)
            {
                temp = xopt[i__] - step * glag[i__];
                if (temp <= sl[i__])
                {
                    w[i__] = sl[i__] - xopt[i__];
                    d__1 = w[i__];
                    wfixsq += d__1 * d__1;
                }
                else if (temp >= su[i__])
                {
                    w[i__] = su[i__] - xopt[i__];
                    d__1 = w[i__];
                    wfixsq += d__1 * d__1;
                }
                else
                {
                    d__1 = glag[i__];
                    ggfree += d__1 * d__1;
                }
            }
        }
        if (wfixsq > wsqsav && ggfree > zero)
        {
            goto L120;
        }
    }

    /* The remaining free W components and all of XALT (W may be rescaled below). */
    gw = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        if (w[i__] == bigstp)
        {
            w[i__] = -step * glag[i__];
            d__3 = su[i__];
            d__4 = xopt[i__] + w[i__];
            d__2 = d__3 <= d__4 ? d__3 : d__4;
            d__1 = sl[i__];
            xalt[i__] = d__1 >= d__2 ? d__1 : d__2;
        }
        else if (w[i__] == zero)
        {
            xalt[i__] = xopt[i__];
        }
        else if (glag[i__] > zero)
        {
            xalt[i__] = sl[i__];
        }
        else
        {
            xalt[i__] = su[i__];
        }
        gw += glag[i__] * w[i__];
    }

    /* CURV = the Lagrange function's curvature along W; scale W when that shrinks |Λ| at XOPT+W. */
    curv = zero;
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        temp = zero;
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
        {
            temp += xpt[k + j * xpt_dim1] * w[j];
        }
        curv += hcol[k] * temp * temp;
    }
    if (iflag == 1)
    {
        curv = -curv;
    }
    if (curv > -gw && curv < -const__ * gw)
    {
        scale = -gw / curv;
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            temp = xopt[i__] + scale * w[i__];
            d__3 = su[i__];
            d__2 = d__3 <= temp ? d__3 : temp;
            d__1 = sl[i__];
            xalt[i__] = d__1 >= d__2 ? d__1 : d__2;
        }
        d__1 = half * gw * scale;
        *cauchy = d__1 * d__1;
    }
    else
    {
        d__1 = gw + half * curv;
        *cauchy = d__1 * d__1;
    }

    /* Try the reversed-gradient XALT too; keep the larger CAUCHY. */
    if (iflag == 0)
    {
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            glag[i__] = -glag[i__];
            w[*n + i__] = xalt[i__];
        }
        csave = *cauchy;
        iflag = 1;
        goto L100;
    }
    if (csave > *cauchy)
    {
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            xalt[i__] = w[*n + i__];
        }
        *cauchy = csave;
    }
L200:
    return;
} /* altmov */

// --------------------------------------------------------------------------------------------- trsbox_
// The BOUNDED trust-region subproblem: truncated CG with restart-on-new-bound (XBDI marks variables fixed at
// SL/SU), then boundary 2-D angle iterations on the free subspace. CRVMIN: 0 if the boundary was reached,
// −1 if every CG search was constrained, else the least free curvature. The L210 block is the inline
// HS = H·S "subroutine" dispatched by CRVMIN/ITERC.
template <typename T>
inline void trsbox(const int* n, const int* npt, T* xpt, T* xopt, T* gopt, T* hq, T* pq, T* sl, T* su, const T* delta,
                   T* xnew, T* d__, T* gnew, T* xbdi, T* s, T* hs, T* hred, T* dsq, T* crvmin)
{
    /* System generated locals */
    int xpt_dim1, xpt_offset, i__1, i__2;
    T d__1, d__2, d__3, d__4;

    /* Local variables */
    int i__, j, k, ih;
    T ds;
    int iu;
    T dhd, dhs, cth, one, shs, sth, ssq, half, beta, sdec, blen;
    int iact = 0, nact;
    T angt = static_cast<T>(0), qred;
    int isav;
    T temp, zero, xsav = static_cast<T>(0), xsum, angbd = static_cast<T>(0), dredg = static_cast<T>(0),
                  sredg = static_cast<T>(0);
    int iterc;
    T resid, delsq, ggsav = static_cast<T>(0), tempa, tempb, redmax, dredsq = static_cast<T>(0), redsav, onemin,
                    gredsq = static_cast<T>(0), rednew;
    int itcsav = 0;
    T rdprev = static_cast<T>(0), rdnext = static_cast<T>(0), stplen, stepsq;
    int itermax = 0;

    /* Parameter adjustments */
    xpt_dim1 = *npt;
    xpt_offset = 1 + xpt_dim1;
    xpt -= xpt_offset;
    --xopt;
    --gopt;
    --hq;
    --pq;
    --sl;
    --su;
    --xnew;
    --d__;
    --gnew;
    --xbdi;
    --s;
    --hs;
    --hred;

    /* Function Body */
    half = static_cast<T>(.5);
    one = static_cast<T>(1.);
    onemin = static_cast<T>(-1.);
    zero = static_cast<T>(0.);

    /* Initial XBDI/NACT (GOPT's sign decides initial bound fixing); D = 0; GNEW = GOPT. */
    iterc = 0;
    nact = 0;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        xbdi[i__] = zero;
        if (xopt[i__] <= sl[i__])
        {
            if (gopt[i__] >= zero)
            {
                xbdi[i__] = onemin;
            }
        }
        else if (xopt[i__] >= su[i__])
        {
            if (gopt[i__] <= zero)
            {
                xbdi[i__] = one;
            }
        }
        if (xbdi[i__] != zero)
        {
            ++nact;
        }
        d__[i__] = zero;
        gnew[i__] = gopt[i__];
    }
    delsq = *delta * *delta;
    qred = zero;
    *crvmin = onemin;

/* The next CG direction (steepest descent on restarts; fixed components zero). */
L20:
    beta = zero;
L30:
    stepsq = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        if (xbdi[i__] != zero)
        {
            s[i__] = zero;
        }
        else if (beta == zero)
        {
            s[i__] = -gnew[i__];
        }
        else
        {
            s[i__] = beta * s[i__] - gnew[i__];
        }
        d__1 = s[i__];
        stepsq += d__1 * d__1;
    }
    if (stepsq == zero)
    {
        goto L190;
    }
    if (beta == zero)
    {
        gredsq = stepsq;
        itermax = iterc + *n - nact;
    }
    if (gredsq * delsq <= qred * static_cast<T>(1e-4) * qred)
    {
        goto L190;
    }

    /* HS = H·S; BLEN = the step to the TR boundary; STPLEN the steplength ignoring the simple bounds. */
    goto L210;
L50:
    resid = delsq;
    ds = zero;
    shs = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        if (xbdi[i__] == zero)
        {
            d__1 = d__[i__];
            resid -= d__1 * d__1;
            ds += s[i__] * d__[i__];
            shs += s[i__] * hs[i__];
        }
    }
    if (resid <= zero)
    {
        goto L90;
    }
    temp = crd::math::sqrt(stepsq * resid + ds * ds);
    if (ds < zero)
    {
        blen = (temp - ds) / stepsq;
    }
    else
    {
        blen = resid / (temp + ds);
    }
    stplen = blen;
    if (shs > zero)
    {
        d__1 = blen;
        d__2 = gredsq / shs;
        stplen = d__1 <= d__2 ? d__1 : d__2;
    }

    /* Shrink STPLEN for the simple bounds; IACT = the newly constrained variable. */
    iact = 0;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        if (s[i__] != zero)
        {
            xsum = xopt[i__] + d__[i__];
            if (s[i__] > zero)
            {
                temp = (su[i__] - xsum) / s[i__];
            }
            else
            {
                temp = (sl[i__] - xsum) / s[i__];
            }
            if (temp < stplen)
            {
                stplen = temp;
                iact = i__;
            }
        }
    }

    /* Update CRVMIN, GNEW, D; SDEC = the Q-decrease. */
    sdec = zero;
    if (stplen > zero)
    {
        ++iterc;
        temp = shs / stepsq;
        if (iact == 0 && temp > zero)
        {
            *crvmin = *crvmin <= temp ? *crvmin : temp;
            if (*crvmin == onemin)
            {
                *crvmin = temp;
            }
        }
        ggsav = gredsq;
        gredsq = zero;
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            gnew[i__] += stplen * hs[i__];
            if (xbdi[i__] == zero)
            {
                d__1 = gnew[i__];
                gredsq += d__1 * d__1;
            }
            d__[i__] += stplen * s[i__];
        }
        d__1 = stplen * (ggsav - half * stplen * shs);
        sdec = d__1 >= zero ? d__1 : zero;
        qred += sdec;
    }

    /* Restart CG when a new bound was hit. */
    if (iact > 0)
    {
        ++nact;
        xbdi[iact] = one;
        if (s[iact] < zero)
        {
            xbdi[iact] = onemin;
        }
        d__1 = d__[iact];
        delsq -= d__1 * d__1;
        if (delsq <= zero)
        {
            goto L90;
        }
        goto L20;
    }

    /* STPLEN < BLEN: another CG iteration or return. */
    if (stplen < blen)
    {
        if (iterc == itermax)
        {
            goto L190;
        }
        if (sdec <= qred * static_cast<T>(.01))
        {
            goto L190;
        }
        beta = gredsq / ggsav;
        goto L30;
    }
L90:
    *crvmin = zero;

/* The alternative (boundary) iteration: scalars + H times the reduced D. */
L100:
    if (nact >= *n - 1)
    {
        goto L190;
    }
    dredsq = zero;
    dredg = zero;
    gredsq = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        if (xbdi[i__] == zero)
        {
            d__1 = d__[i__];
            dredsq += d__1 * d__1;
            dredg += d__[i__] * gnew[i__];
            d__1 = gnew[i__];
            gredsq += d__1 * d__1;
            s[i__] = d__[i__];
        }
        else
        {
            s[i__] = zero;
        }
    }
    itcsav = iterc;
    goto L210;

/* S ← a combination of reduced D and reduced G orthogonal to reduced D. */
L120:
    ++iterc;
    temp = gredsq * dredsq - dredg * dredg;
    if (temp <= qred * static_cast<T>(1e-4) * qred)
    {
        goto L190;
    }
    temp = crd::math::sqrt(temp);
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        if (xbdi[i__] == zero)
        {
            s[i__] = (dredg * d__[i__] - dredsq * gnew[i__]) / temp;
        }
        else
        {
            s[i__] = zero;
        }
    }
    sredg = -temp;

    /* ANGBD = the bound on tan(half-angle); a free variable already at a bound branches back to L100. */
    angbd = one;
    iact = 0;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        if (xbdi[i__] == zero)
        {
            tempa = xopt[i__] + d__[i__] - sl[i__];
            tempb = su[i__] - xopt[i__] - d__[i__];
            if (tempa <= zero)
            {
                ++nact;
                xbdi[i__] = onemin;
                goto L100;
            }
            else if (tempb <= zero)
            {
                ++nact;
                xbdi[i__] = one;
                goto L100;
            }
            d__1 = d__[i__];
            d__2 = s[i__];
            ssq = d__1 * d__1 + d__2 * d__2;
            d__1 = xopt[i__] - sl[i__];
            temp = ssq - d__1 * d__1;
            if (temp > zero)
            {
                temp = crd::math::sqrt(temp) - s[i__];
                if (angbd * temp > tempa)
                {
                    angbd = tempa / temp;
                    iact = i__;
                    xsav = onemin;
                }
            }
            d__1 = su[i__] - xopt[i__];
            temp = ssq - d__1 * d__1;
            if (temp > zero)
            {
                temp = crd::math::sqrt(temp) + s[i__];
                if (angbd * temp > tempb)
                {
                    angbd = tempb / temp;
                    iact = i__;
                    xsav = one;
                }
            }
        }
    }

    /* HHD + curvatures for the alternative iteration. */
    goto L210;
L150:
    shs = zero;
    dhs = zero;
    dhd = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        if (xbdi[i__] == zero)
        {
            shs += s[i__] * hs[i__];
            dhs += d__[i__] * hs[i__];
            dhd += d__[i__] * hred[i__];
        }
    }

    /* The greatest Q-reduction over equally spaced ANGT in [0, ANGBD]. */
    redmax = zero;
    isav = 0;
    redsav = zero;
    iu = static_cast<int>(angbd * static_cast<T>(17.) + static_cast<T>(3.1));
    i__1 = iu;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        angt = angbd * static_cast<T>(i__) / static_cast<T>(iu);
        sth = (angt + angt) / (one + angt * angt);
        temp = shs + angt * (angt * dhd - dhs - dhs);
        rednew = sth * (angt * dredg - sredg - half * sth * temp);
        if (rednew > redmax)
        {
            redmax = rednew;
            isav = i__;
            rdprev = redsav;
        }
        else if (i__ == isav + 1)
        {
            rdnext = rednew;
        }
        redsav = rednew;
    }

    /* Zero reduction ⇒ return; else the refined angle + SDEC. */
    if (isav == 0)
    {
        goto L190;
    }
    if (isav < iu)
    {
        temp = (rdnext - rdprev) / (redmax + redmax - rdprev - rdnext);
        angt = angbd * (static_cast<T>(isav) + half * temp) / static_cast<T>(iu);
    }
    cth = (one - angt * angt) / (one + angt * angt);
    sth = (angt + angt) / (one + angt * angt);
    temp = shs + angt * (angt * dhd - dhs - dhs);
    sdec = sth * (angt * dredg - sredg - half * sth * temp);
    if (sdec <= zero)
    {
        goto L190;
    }

    /* Update GNEW, D, HRED; a bound-restricted angle fixes that variable. */
    dredg = zero;
    gredsq = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        gnew[i__] = gnew[i__] + (cth - one) * hred[i__] + sth * hs[i__];
        if (xbdi[i__] == zero)
        {
            d__[i__] = cth * d__[i__] + sth * s[i__];
            dredg += d__[i__] * gnew[i__];
            d__1 = gnew[i__];
            gredsq += d__1 * d__1;
        }
        hred[i__] = cth * hred[i__] + sth * hs[i__];
    }
    qred += sdec;
    if (iact > 0 && isav == iu)
    {
        ++nact;
        xbdi[iact] = xsav;
        goto L100;
    }
    if (sdec > qred * static_cast<T>(.01))
    {
        goto L120;
    }

/* XNEW = XOPT + D with careful bound attention; DSQ = ‖D‖². */
L190:
    *dsq = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        d__3 = xopt[i__] + d__[i__];
        d__4 = su[i__];
        d__1 = d__3 <= d__4 ? d__3 : d__4;
        d__2 = sl[i__];
        xnew[i__] = d__1 >= d__2 ? d__1 : d__2;
        if (xbdi[i__] == onemin)
        {
            xnew[i__] = sl[i__];
        }
        if (xbdi[i__] == one)
        {
            xnew[i__] = su[i__];
        }
        d__[i__] = xnew[i__] - xopt[i__];
        d__1 = d__[i__];
        *dsq += d__1 * d__1;
    }
    return;

/* The inline "subroutine" HS = H·S, dispatched by CRVMIN/ITERC. */
L210:
    ih = 0;
    i__1 = *n;
    for (j = 1; j <= i__1; ++j)
    {
        hs[j] = zero;
        i__2 = j;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            ++ih;
            if (i__ < j)
            {
                hs[j] += hq[ih] * s[i__];
            }
            hs[i__] += hq[ih] * s[j];
        }
    }
    i__2 = *npt;
    for (k = 1; k <= i__2; ++k)
    {
        if (pq[k] != zero)
        {
            temp = zero;
            i__1 = *n;
            for (j = 1; j <= i__1; ++j)
            {
                temp += xpt[k + j * xpt_dim1] * s[j];
            }
            temp *= pq[k];
            i__1 = *n;
            for (i__ = 1; i__ <= i__1; ++i__)
            {
                hs[i__] += temp * xpt[k + i__ * xpt_dim1];
            }
        }
    }
    if (*crvmin != zero)
    {
        goto L50;
    }
    if (iterc > itcsav)
    {
        goto L150;
    }
    i__2 = *n;
    for (i__ = 1; i__ <= i__2; ++i__)
    {
        hred[i__] = hs[i__];
    }
    goto L120;
} /* trsbox */

// --------------------------------------------------------------------------------------------- rescue_
// Re-initialization when the denominator degenerates: shifts XBASE to the trust-region centre, builds a
// well-conditioned PROVISIONAL point set (PTSAUX coordinate steps, PTSID identifiers), then reinstates
// original points one by one through `update` (KOLD chosen to keep the denominator large), evaluating F at
// whatever provisional points remain. `Calfun` is `T calfun(int n, const T* x)`.
template <typename T, typename Calfun>
[[nodiscard]] inline Rc rescue(const int* n, const int* npt, const T* xl, const T* xu, Stop<T>* stop, Calfun&& calfun,
                               T* xbase, T* xpt, T* fval, T* xopt, T* gopt, T* hq, T* pq, T* bmat, T* zmat,
                               const int* ndim, T* sl, T* su, const T* delta, int* kopt, T* vlag, T* ptsaux, T* ptsid,
                               T* w)
{
    /* System generated locals */
    int xpt_dim1, xpt_offset, bmat_dim1, bmat_offset, zmat_dim1, zmat_offset, i__1, i__2, i__3;
    T d__1, d__2, d__3, d__4;

    /* Local variables */
    T f;
    int i__, j, k, ih, jp, ip, iq, np, iw;
    T xp = static_cast<T>(0), xq = static_cast<T>(0), den;
    int ihp = 0;
    T one;
    int ihq, jpn, kpt;
    T sum, diff, half, beta;
    int kold;
    T winc;
    int nrem, knew;
    T temp, bsum;
    int nptm;
    T zero, hdiag, fbase, sfrac, denom, vquad, sumpq;
    T dsqmin, distsq, vlmxsq;

    /* Parameter adjustments */
    zmat_dim1 = *npt;
    zmat_offset = 1 + zmat_dim1;
    zmat -= zmat_offset;
    xpt_dim1 = *npt;
    xpt_offset = 1 + xpt_dim1;
    xpt -= xpt_offset;
    --xl;
    --xu;
    --xbase;
    --fval;
    --xopt;
    --gopt;
    --hq;
    --pq;
    bmat_dim1 = *ndim;
    bmat_offset = 1 + bmat_dim1;
    bmat -= bmat_offset;
    --sl;
    --su;
    --vlag;
    ptsaux -= 3;
    --ptsid;
    --w;

    /* Function Body */
    half = static_cast<T>(.5);
    one = static_cast<T>(1.);
    zero = static_cast<T>(0.);
    np = *n + 1;
    sfrac = half / static_cast<T>(np);
    nptm = *npt - np;

    /* Shift points so XOPT is the origin; zero ZMAT; SUMPQ + per-point distances (WINC tracks the max). */
    sumpq = zero;
    winc = zero;
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        distsq = zero;
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
        {
            xpt[k + j * xpt_dim1] -= xopt[j];
            d__1 = xpt[k + j * xpt_dim1];
            distsq += d__1 * d__1;
        }
        sumpq += pq[k];
        w[*ndim + k] = distsq;
        winc = winc >= distsq ? winc : distsq;
        i__2 = nptm;
        for (j = 1; j <= i__2; ++j)
        {
            zmat[k + j * zmat_dim1] = zero;
        }
    }

    /* HQ after the XBASE shift. */
    ih = 0;
    i__2 = *n;
    for (j = 1; j <= i__2; ++j)
    {
        w[j] = half * sumpq * xopt[j];
        i__1 = *npt;
        for (k = 1; k <= i__1; ++k)
        {
            w[j] += pq[k] * xpt[k + j * xpt_dim1];
        }
        i__1 = j;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            ++ih;
            hq[ih] = hq[ih] + w[i__] * xopt[j] + w[j] * xopt[i__];
        }
    }

    /* Shift XBASE/SL/SU/XOPT; zero BMAT; set PTSAUX (bound-clipped ± delta steps, ordered/balanced). */
    i__1 = *n;
    for (j = 1; j <= i__1; ++j)
    {
        xbase[j] += xopt[j];
        sl[j] -= xopt[j];
        su[j] -= xopt[j];
        xopt[j] = zero;
        d__1 = *delta;
        d__2 = su[j];
        ptsaux[(j << 1) + 1] = d__1 <= d__2 ? d__1 : d__2;
        d__1 = -(*delta);
        d__2 = sl[j];
        ptsaux[(j << 1) + 2] = d__1 >= d__2 ? d__1 : d__2;
        if (ptsaux[(j << 1) + 1] + ptsaux[(j << 1) + 2] < zero)
        {
            temp = ptsaux[(j << 1) + 1];
            ptsaux[(j << 1) + 1] = ptsaux[(j << 1) + 2];
            ptsaux[(j << 1) + 2] = temp;
        }
        d__2 = ptsaux[(j << 1) + 2];
        d__1 = ptsaux[(j << 1) + 1];
        if (crd::math::fabs(d__2) < half * crd::math::fabs(d__1))
        {
            ptsaux[(j << 1) + 2] = half * ptsaux[(j << 1) + 1];
        }
        i__2 = *ndim;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            bmat[i__ + j * bmat_dim1] = zero;
        }
    }
    fbase = fval[*kopt];

    /* Identifiers + nonzero BMAT/ZMAT entries of the coordinate-direction provisional points. */
    ptsid[1] = sfrac;
    i__2 = *n;
    for (j = 1; j <= i__2; ++j)
    {
        jp = j + 1;
        jpn = jp + *n;
        ptsid[jp] = static_cast<T>(j) + sfrac;
        if (jpn <= *npt)
        {
            ptsid[jpn] = static_cast<T>(j) / static_cast<T>(np) + sfrac;
            temp = one / (ptsaux[(j << 1) + 1] - ptsaux[(j << 1) + 2]);
            bmat[jp + j * bmat_dim1] = -temp + one / ptsaux[(j << 1) + 1];
            bmat[jpn + j * bmat_dim1] = temp + one / ptsaux[(j << 1) + 2];
            bmat[j * bmat_dim1 + 1] = -bmat[jp + j * bmat_dim1] - bmat[jpn + j * bmat_dim1];
            d__1 = ptsaux[(j << 1) + 1] * ptsaux[(j << 1) + 2];
            zmat[j * zmat_dim1 + 1] = crd::math::sqrt(static_cast<T>(2.)) / crd::math::fabs(d__1);
            zmat[jp + j * zmat_dim1] = zmat[j * zmat_dim1 + 1] * ptsaux[(j << 1) + 2] * temp;
            zmat[jpn + j * zmat_dim1] = -zmat[j * zmat_dim1 + 1] * ptsaux[(j << 1) + 1] * temp;
        }
        else
        {
            bmat[j * bmat_dim1 + 1] = -one / ptsaux[(j << 1) + 1];
            bmat[jp + j * bmat_dim1] = one / ptsaux[(j << 1) + 1];
            d__1 = ptsaux[(j << 1) + 1];
            bmat[j + *npt + j * bmat_dim1] = -half * (d__1 * d__1);
        }
    }

    /* Remaining identifiers + their ZMAT entries. */
    if (*npt >= *n + np)
    {
        i__2 = *npt;
        for (k = np << 1; k <= i__2; ++k)
        {
            iw = static_cast<int>((static_cast<T>(k - np) - half) / static_cast<T>(*n));
            ip = k - np - iw * *n;
            iq = ip + iw;
            if (iq > *n)
            {
                iq -= *n;
            }
            ptsid[k] = static_cast<T>(ip) + static_cast<T>(iq) / static_cast<T>(np) + sfrac;
            temp = one / (ptsaux[(ip << 1) + 1] * ptsaux[(iq << 1) + 1]);
            zmat[(k - np) * zmat_dim1 + 1] = temp;
            zmat[ip + 1 + (k - np) * zmat_dim1] = -temp;
            zmat[iq + 1 + (k - np) * zmat_dim1] = -temp;
            zmat[k + (k - np) * zmat_dim1] = temp;
        }
    }
    nrem = *npt;
    kold = 1;
    knew = *kopt;

/* Exchange PTSID(KOLD) with PTSID(KNEW). */
L80:
    i__2 = *n;
    for (j = 1; j <= i__2; ++j)
    {
        temp = bmat[kold + j * bmat_dim1];
        bmat[kold + j * bmat_dim1] = bmat[knew + j * bmat_dim1];
        bmat[knew + j * bmat_dim1] = temp;
    }
    i__2 = nptm;
    for (j = 1; j <= i__2; ++j)
    {
        temp = zmat[kold + j * zmat_dim1];
        zmat[kold + j * zmat_dim1] = zmat[knew + j * zmat_dim1];
        zmat[knew + j * zmat_dim1] = temp;
    }
    ptsid[kold] = ptsid[knew];
    ptsid[knew] = zero;
    w[*ndim + knew] = zero;
    --nrem;
    if (knew != *kopt)
    {
        temp = vlag[kold];
        vlag[kold] = vlag[knew];
        vlag[knew] = temp;

        /* Flip point KNEW from provisional to original via update (L350 when all originals reinstated). */
        update<T>(n, npt, &bmat[bmat_offset], &zmat[zmat_offset], ndim, &vlag[1], &beta, &denom, &knew, &w[1]);
        if (nrem == 0)
        {
            goto L350;
        }
        i__2 = *npt;
        for (k = 1; k <= i__2; ++k)
        {
            d__1 = w[*ndim + k];
            w[*ndim + k] = crd::math::fabs(d__1);
        }
    }

/* Pick the original point KNEW not yet reinstated (closest to XOPT; retry-bumped by WINC). */
L120:
    dsqmin = zero;
    i__2 = *npt;
    for (k = 1; k <= i__2; ++k)
    {
        if (w[*ndim + k] > zero)
        {
            if (dsqmin == zero || w[*ndim + k] < dsqmin)
            {
                knew = k;
                dsqmin = w[*ndim + k];
            }
        }
    }
    if (dsqmin == zero)
    {
        goto L260;
    }

    /* The W-vector of the chosen original point. */
    i__2 = *n;
    for (j = 1; j <= i__2; ++j)
    {
        w[*npt + j] = xpt[knew + j * xpt_dim1];
    }
    i__2 = *npt;
    for (k = 1; k <= i__2; ++k)
    {
        sum = zero;
        if (k == *kopt)
        {
        }
        else if (ptsid[k] == zero)
        {
            i__1 = *n;
            for (j = 1; j <= i__1; ++j)
            {
                sum += w[*npt + j] * xpt[k + j * xpt_dim1];
            }
        }
        else
        {
            ip = static_cast<int>(ptsid[k]);
            if (ip > 0)
            {
                sum = w[*npt + ip] * ptsaux[(ip << 1) + 1];
            }
            iq = static_cast<int>(static_cast<T>(np) * ptsid[k] - static_cast<T>(ip * np));
            if (iq > 0)
            {
                iw = 1;
                if (ip == 0)
                {
                    iw = 2;
                }
                sum += w[*npt + iq] * ptsaux[iw + (iq << 1)];
            }
        }
        w[k] = half * sum * sum;
    }

    /* VLAG and BETA for reinstating XPT(KNEW, ·). */
    i__2 = *npt;
    for (k = 1; k <= i__2; ++k)
    {
        sum = zero;
        i__1 = *n;
        for (j = 1; j <= i__1; ++j)
        {
            sum += bmat[k + j * bmat_dim1] * w[*npt + j];
        }
        vlag[k] = sum;
    }
    beta = zero;
    i__2 = nptm;
    for (j = 1; j <= i__2; ++j)
    {
        sum = zero;
        i__1 = *npt;
        for (k = 1; k <= i__1; ++k)
        {
            sum += zmat[k + j * zmat_dim1] * w[k];
        }
        beta -= sum * sum;
        i__1 = *npt;
        for (k = 1; k <= i__1; ++k)
        {
            vlag[k] += sum * zmat[k + j * zmat_dim1];
        }
    }
    bsum = zero;
    distsq = zero;
    i__1 = *n;
    for (j = 1; j <= i__1; ++j)
    {
        sum = zero;
        i__2 = *npt;
        for (k = 1; k <= i__2; ++k)
        {
            sum += bmat[k + j * bmat_dim1] * w[k];
        }
        jp = j + *npt;
        bsum += sum * w[jp];
        i__2 = *ndim;
        for (ip = *npt + 1; ip <= i__2; ++ip)
        {
            sum += bmat[ip + j * bmat_dim1] * w[ip];
        }
        bsum += sum * w[jp];
        vlag[jp] = sum;
        d__1 = xpt[knew + j * xpt_dim1];
        distsq += d__1 * d__1;
    }
    beta = half * distsq * distsq + beta - bsum;
    vlag[*kopt] += one;

    /* KOLD = the provisional point to delete (avoid a small update denominator). */
    denom = zero;
    vlmxsq = zero;
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        if (ptsid[k] != zero)
        {
            hdiag = zero;
            i__2 = nptm;
            for (j = 1; j <= i__2; ++j)
            {
                d__1 = zmat[k + j * zmat_dim1];
                hdiag += d__1 * d__1;
            }
            d__1 = vlag[k];
            den = beta * hdiag + d__1 * d__1;
            if (den > denom)
            {
                kold = k;
                denom = den;
            }
        }
        d__3 = vlag[k];
        d__1 = vlmxsq;
        d__2 = d__3 * d__3;
        vlmxsq = d__1 >= d__2 ? d__1 : d__2;
    }
    if (denom <= vlmxsq * static_cast<T>(.01))
    {
        w[*ndim + knew] = -w[*ndim + knew] - winc;
        goto L120;
    }
    goto L80;

/* Final positions chosen; evaluate F at whatever provisional points remain + fold them into the model. */
L260:
    i__1 = *npt;
    for (kpt = 1; kpt <= i__1; ++kpt)
    {
        if (ptsid[kpt] == zero)
        {
            goto L340;
        }
        if (stop_evals(*stop))
        {
            return Rc::MaxevalReached;
        }

        ih = 0;
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
        {
            w[j] = xpt[kpt + j * xpt_dim1];
            xpt[kpt + j * xpt_dim1] = zero;
            temp = pq[kpt] * w[j];
            i__3 = j;
            for (i__ = 1; i__ <= i__3; ++i__)
            {
                ++ih;
                hq[ih] += temp * w[i__];
            }
        }
        pq[kpt] = zero;
        ip = static_cast<int>(ptsid[kpt]);
        iq = static_cast<int>(static_cast<T>(np) * ptsid[kpt] - static_cast<T>(ip * np));
        if (ip > 0)
        {
            xp = ptsaux[(ip << 1) + 1];
            xpt[kpt + ip * xpt_dim1] = xp;
        }
        if (iq > 0)
        {
            xq = ptsaux[(iq << 1) + 1];
            if (ip == 0)
            {
                xq = ptsaux[(iq << 1) + 2];
            }
            xpt[kpt + iq * xpt_dim1] = xq;
        }

        /* VQUAD = the current model at the new point. */
        vquad = fbase;
        if (ip > 0)
        {
            ihp = (ip + ip * ip) / 2;
            vquad += xp * (gopt[ip] + half * xp * hq[ihp]);
        }
        if (iq > 0)
        {
            ihq = (iq + iq * iq) / 2;
            vquad += xq * (gopt[iq] + half * xq * hq[ihq]);
            if (ip > 0)
            {
                const int idiff = ip - iq >= 0 ? ip - iq : iq - ip;
                iw = (ihp >= ihq ? ihp : ihq) - idiff;
                vquad += xp * xq * hq[iw];
            }
        }
        i__3 = *npt;
        for (k = 1; k <= i__3; ++k)
        {
            temp = zero;
            if (ip > 0)
            {
                temp += xp * xpt[k + ip * xpt_dim1];
            }
            if (iq > 0)
            {
                temp += xq * xpt[k + iq * xpt_dim1];
            }
            vquad += half * pq[k] * temp * temp;
        }

        /* F at the new point (bound-clamped, exact bound landing); DIFF for the model update. */
        i__3 = *n;
        for (i__ = 1; i__ <= i__3; ++i__)
        {
            d__3 = xl[i__];
            d__4 = xbase[i__] + xpt[kpt + i__ * xpt_dim1];
            d__1 = d__3 >= d__4 ? d__3 : d__4;
            d__2 = xu[i__];
            w[i__] = d__1 <= d__2 ? d__1 : d__2;
            if (xpt[kpt + i__ * xpt_dim1] == sl[i__])
            {
                w[i__] = xl[i__];
            }
            if (xpt[kpt + i__ * xpt_dim1] == su[i__])
            {
                w[i__] = xu[i__];
            }
        }
        ++stop->nevals;
        f = calfun(*n, &w[1]);
        fval[kpt] = f;
        if (f < fval[*kopt])
        {
            *kopt = kpt;
        }
        if (f < stop->minf_max)
        {
            return Rc::MinfMaxReached;
        }
        if (stop_evals(*stop))
        {
            return Rc::MaxevalReached;
        }
        diff = f - vquad;

        /* Update the model. */
        i__3 = *n;
        for (i__ = 1; i__ <= i__3; ++i__)
        {
            gopt[i__] += diff * bmat[kpt + i__ * bmat_dim1];
        }
        i__3 = *npt;
        for (k = 1; k <= i__3; ++k)
        {
            sum = zero;
            i__2 = nptm;
            for (j = 1; j <= i__2; ++j)
            {
                sum += zmat[k + j * zmat_dim1] * zmat[kpt + j * zmat_dim1];
            }
            temp = diff * sum;
            if (ptsid[k] == zero)
            {
                pq[k] += temp;
            }
            else
            {
                ip = static_cast<int>(ptsid[k]);
                iq = static_cast<int>(static_cast<T>(np) * ptsid[k] - static_cast<T>(ip * np));
                ihq = (iq * iq + iq) / 2;
                if (ip == 0)
                {
                    d__1 = ptsaux[(iq << 1) + 2];
                    hq[ihq] += temp * (d__1 * d__1);
                }
                else
                {
                    ihp = (ip * ip + ip) / 2;
                    d__1 = ptsaux[(ip << 1) + 1];
                    hq[ihp] += temp * (d__1 * d__1);
                    if (iq > 0)
                    {
                        d__1 = ptsaux[(iq << 1) + 1];
                        hq[ihq] += temp * (d__1 * d__1);
                        const int idiff = iq - ip >= 0 ? iq - ip : ip - iq;
                        iw = (ihp >= ihq ? ihp : ihq) - idiff;
                        hq[iw] += temp * ptsaux[(ip << 1) + 1] * ptsaux[(iq << 1) + 1];
                    }
                }
            }
        }
        ptsid[kpt] = zero;
    L340:;
    }
L350:
    return Rc::Success;
} /* rescue */

// --------------------------------------------------------------------------------------------- bobyqb_
// The main BOBYQA driver: prelim init, then trust-region steps (trsbox) and model steps (altmov, with the
// Cauchy alternative when the denominator loses), the XBASE shift, the RESCUE safeguard on denominator
// cancellation, the least-Frobenius-norm ITEST swap, and Powell's ρ schedule. rhoend is computed by the
// CALLER (the public driver passes it like the reference's bobyqa() computes it from xtol_rel·rhobeg).
template <typename T, typename Calfun>
[[nodiscard]] inline Rc bobyqb(const int* n, const int* npt, T* x, const T* xl, const T* xu, const T* rhobeg,
                               const T* rhoend, Stop<T>* stop, Calfun&& calfun, T* minf, T* xbase, T* xpt, T* fval,
                               T* xopt, T* gopt, T* hq, T* pq, T* bmat, T* zmat, const int* ndim, T* sl, T* su, T* xnew,
                               T* xalt, T* d__, T* vlag, T* w)
{
    /* System generated locals */
    int xpt_dim1, xpt_offset, bmat_dim1, bmat_offset, zmat_dim1, zmat_offset, i__1, i__2, i__3;
    T d__1, d__2, d__3, d__4;

    /* Local variables */
    T f = static_cast<T>(0);
    int i__, j, k, ih, jj, nh, ip, jp;
    T dx;
    int np;
    T den, one, ten, dsq, rho, sum, two, diff, half, beta = static_cast<T>(0), gisq;
    int knew = 0;
    T temp, suma, sumb, bsum, fopt;
    int kopt = 1, nptm;
    T zero, curv;
    int ksav;
    T gqsq, dist, sumw, sumz, diffa, diffb, diffc = static_cast<T>(0), hdiag;
    int kbase;
    T alpha = static_cast<T>(0), delta, adelt = static_cast<T>(0), denom = static_cast<T>(0), fsave, bdtol, delsq;
    int nresc, nfsav;
    T ratio = static_cast<T>(0), dnorm, vquad, pqold, tenth;
    int itest;
    T sumpq, scaden;
    T errbig, cauchy = static_cast<T>(0), fracsq, biglsq, densav;
    T bdtest;
    T crvmin, frhosq;
    T distsq;
    int ntrits;
    T xoptsq;
    Rc rc = Rc::Success, rc2;

    /* Parameter adjustments */
    zmat_dim1 = *npt;
    zmat_offset = 1 + zmat_dim1;
    zmat -= zmat_offset;
    xpt_dim1 = *npt;
    xpt_offset = 1 + xpt_dim1;
    xpt -= xpt_offset;
    --x;
    --xl;
    --xu;
    --xbase;
    --fval;
    --xopt;
    --gopt;
    --hq;
    --pq;
    bmat_dim1 = *ndim;
    bmat_offset = 1 + bmat_dim1;
    bmat -= bmat_offset;
    --sl;
    --su;
    --xnew;
    --xalt;
    --d__;
    --vlag;
    --w;

    /* Function Body */
    half = static_cast<T>(.5);
    one = static_cast<T>(1.);
    ten = static_cast<T>(10.);
    tenth = static_cast<T>(.1);
    two = static_cast<T>(2.);
    zero = static_cast<T>(0.);
    np = *n + 1;
    nptm = *npt - np;
    nh = *n * np / 2;

    /* PRELIM sets the first-iteration state; branch to L720 if it stopped early. */
    rc2 = prelim<T>(n, npt, &x[1], &xl[1], &xu[1], rhobeg, stop, calfun, &xbase[1], &xpt[xpt_offset], &fval[1],
                    &gopt[1], &hq[1], &pq[1], &bmat[bmat_offset], &zmat[zmat_offset], ndim, &sl[1], &su[1], &kopt);
    xoptsq = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        xopt[i__] = xpt[kopt + i__ * xpt_dim1];
        d__1 = xopt[i__];
        xoptsq += d__1 * d__1;
    }
    fsave = fval[1];
    if (rc2 != Rc::Success)
    {
        rc = rc2;
        goto L720;
    }
    kbase = 1;

    /* Iterative-procedure settings. */
    rho = *rhobeg;
    delta = rho;
    nresc = stop->nevals;
    ntrits = 0;
    diffa = zero;
    diffb = zero;
    itest = 0;
    nfsav = stop->nevals;

/* Update GOPT before the first iteration and after each F-calling RESCUE. */
L20:
    if (kopt != kbase)
    {
        ih = 0;
        i__1 = *n;
        for (j = 1; j <= i__1; ++j)
        {
            i__2 = j;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                ++ih;
                if (i__ < j)
                {
                    gopt[j] += hq[ih] * xopt[i__];
                }
                gopt[i__] += hq[ih] * xopt[j];
            }
        }
        if (stop->nevals > *npt)
        {
            i__2 = *npt;
            for (k = 1; k <= i__2; ++k)
            {
                temp = zero;
                i__1 = *n;
                for (j = 1; j <= i__1; ++j)
                {
                    temp += xpt[k + j * xpt_dim1] * xopt[j];
                }
                temp = pq[k] * temp;
                i__1 = *n;
                for (i__ = 1; i__ <= i__1; ++i__)
                {
                    gopt[i__] += temp * xpt[k + i__ * xpt_dim1];
                }
            }
        }
    }

/* The next trust-region point; NTRITS = -1 routes short steps to L650/L680 without evaluating F. */
L60:
    trsbox<T>(n, npt, &xpt[xpt_offset], &xopt[1], &gopt[1], &hq[1], &pq[1], &sl[1], &su[1], &delta, &xnew[1], &d__[1],
              &w[1], &w[np], &w[np + *n], &w[np + (*n << 1)], &w[np + *n * 3], &dsq, &crvmin);
    d__1 = delta;
    d__2 = crd::math::sqrt(dsq);
    dnorm = d__1 <= d__2 ? d__1 : d__2;
    if (dnorm < half * rho)
    {
        ntrits = -1;
        d__1 = ten * rho;
        distsq = d__1 * d__1;
        if (stop->nevals <= nfsav + 2)
        {
            goto L650;
        }

        /* L650 vs L680: is the work at the current RHO complete? */
        d__1 = diffa >= diffb ? diffa : diffb;
        errbig = d__1 >= diffc ? d__1 : diffc;
        frhosq = rho * static_cast<T>(.125) * rho;
        if (crvmin > zero && errbig > frhosq * crvmin)
        {
            goto L650;
        }
        bdtol = errbig / rho;
        i__1 = *n;
        for (j = 1; j <= i__1; ++j)
        {
            bdtest = bdtol;
            if (xnew[j] == sl[j])
            {
                bdtest = w[j];
            }
            if (xnew[j] == su[j])
            {
                bdtest = -w[j];
            }
            if (bdtest < bdtol)
            {
                curv = hq[(j + j * j) / 2];
                i__2 = *npt;
                for (k = 1; k <= i__2; ++k)
                {
                    d__1 = xpt[k + j * xpt_dim1];
                    curv += pq[k] * (d__1 * d__1);
                }
                bdtest += half * curv * rho;
                if (bdtest < bdtol)
                {
                    goto L650;
                }
            }
        }
        goto L680;
    }
    ++ntrits;

/* Shift XBASE when XOPT strays (the cheap shift; RESCUE is the expensive one). */
L90:
    if (dsq <= xoptsq * static_cast<T>(.001))
    {
        fracsq = xoptsq * static_cast<T>(.25);
        sumpq = zero;
        i__1 = *npt;
        for (k = 1; k <= i__1; ++k)
        {
            sumpq += pq[k];
            sum = -half * xoptsq;
            i__2 = *n;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                sum += xpt[k + i__ * xpt_dim1] * xopt[i__];
            }
            w[*npt + k] = sum;
            temp = fracsq - half * sum;
            i__2 = *n;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                w[i__] = bmat[k + i__ * bmat_dim1];
                vlag[i__] = sum * xpt[k + i__ * xpt_dim1] + temp * xopt[i__];
                ip = *npt + i__;
                i__3 = i__;
                for (j = 1; j <= i__3; ++j)
                {
                    bmat[ip + j * bmat_dim1] = bmat[ip + j * bmat_dim1] + w[i__] * vlag[j] + vlag[i__] * w[j];
                }
            }
        }

        /* The ZMAT-dependent BMAT revisions. */
        i__3 = nptm;
        for (jj = 1; jj <= i__3; ++jj)
        {
            sumz = zero;
            sumw = zero;
            i__2 = *npt;
            for (k = 1; k <= i__2; ++k)
            {
                sumz += zmat[k + jj * zmat_dim1];
                vlag[k] = w[*npt + k] * zmat[k + jj * zmat_dim1];
                sumw += vlag[k];
            }
            i__2 = *n;
            for (j = 1; j <= i__2; ++j)
            {
                sum = (fracsq * sumz - half * sumw) * xopt[j];
                i__1 = *npt;
                for (k = 1; k <= i__1; ++k)
                {
                    sum += vlag[k] * xpt[k + j * xpt_dim1];
                }
                w[j] = sum;
                i__1 = *npt;
                for (k = 1; k <= i__1; ++k)
                {
                    bmat[k + j * bmat_dim1] += sum * zmat[k + jj * zmat_dim1];
                }
            }
            i__1 = *n;
            for (i__ = 1; i__ <= i__1; ++i__)
            {
                ip = i__ + *npt;
                temp = w[i__];
                i__2 = i__;
                for (j = 1; j <= i__2; ++j)
                {
                    bmat[ip + j * bmat_dim1] += temp * w[j];
                }
            }
        }

        /* Complete the shift (second-derivative parameters included). */
        ih = 0;
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
        {
            w[j] = -half * sumpq * xopt[j];
            i__1 = *npt;
            for (k = 1; k <= i__1; ++k)
            {
                w[j] += pq[k] * xpt[k + j * xpt_dim1];
                xpt[k + j * xpt_dim1] -= xopt[j];
            }
            i__1 = j;
            for (i__ = 1; i__ <= i__1; ++i__)
            {
                ++ih;
                hq[ih] = hq[ih] + w[i__] * xopt[j] + xopt[i__] * w[j];
                bmat[*npt + i__ + j * bmat_dim1] = bmat[*npt + j + i__ * bmat_dim1];
            }
        }
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            xbase[i__] += xopt[i__];
            xnew[i__] -= xopt[i__];
            sl[i__] -= xopt[i__];
            su[i__] -= xopt[i__];
            xopt[i__] = zero;
        }
        xoptsq = zero;
    }
    if (ntrits == 0)
    {
        goto L210;
    }
    goto L230;

/* RESCUE: the expensive from-scratch rebuild (only when the update denominator halves from rounding). */
L190:
    nfsav = stop->nevals;
    kbase = kopt;
    rc2 = rescue<T>(n, npt, &xl[1], &xu[1], stop, calfun, &xbase[1], &xpt[xpt_offset], &fval[1], &xopt[1], &gopt[1],
                    &hq[1], &pq[1], &bmat[bmat_offset], &zmat[zmat_offset], ndim, &sl[1], &su[1], &delta, &kopt,
                    &vlag[1], &w[1], &w[*n + np], &w[*ndim + np]);

    /* XOPT updated now in case of the L720 branch; GOPT updates happen after the L20 branch. */
    xoptsq = zero;
    if (kopt != kbase)
    {
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            xopt[i__] = xpt[kopt + i__ * xpt_dim1];
            d__1 = xopt[i__];
            xoptsq += d__1 * d__1;
        }
    }
    if (rc2 != Rc::Success)
    {
        rc = rc2;
        goto L720;
    }
    nresc = stop->nevals;
    if (nfsav < stop->nevals)
    {
        nfsav = stop->nevals;
        goto L20;
    }
    if (ntrits > 0)
    {
        goto L60;
    }

/* ALTMOV picks XNEW (denominator line search) and XALT (constrained Cauchy). */
L210:
    altmov<T>(n, npt, &xpt[xpt_offset], &xopt[1], &bmat[bmat_offset], &zmat[zmat_offset], ndim, &sl[1], &su[1], &kopt,
              &knew, &adelt, &xnew[1], &xalt[1], &alpha, &cauchy, &w[1], &w[np], &w[*ndim + 1]);
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        d__[i__] = xnew[i__] - xopt[i__];
    }

/* VLAG and BETA for the current D (D·XPT(K,·) kept in W(NPT+K) for VQUAD). */
L230:
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
        w[*npt + k] = suma;
    }
    beta = zero;
    i__1 = nptm;
    for (jj = 1; jj <= i__1; ++jj)
    {
        sum = zero;
        i__2 = *npt;
        for (k = 1; k <= i__2; ++k)
        {
            sum += zmat[k + jj * zmat_dim1] * w[k];
        }
        beta -= sum * sum;
        i__2 = *npt;
        for (k = 1; k <= i__2; ++k)
        {
            vlag[k] += sum * zmat[k + jj * zmat_dim1];
        }
    }
    dsq = zero;
    bsum = zero;
    dx = zero;
    i__2 = *n;
    for (j = 1; j <= i__2; ++j)
    {
        d__1 = d__[j];
        dsq += d__1 * d__1;
        sum = zero;
        i__1 = *npt;
        for (k = 1; k <= i__1; ++k)
        {
            sum += w[k] * bmat[k + j * bmat_dim1];
        }
        bsum += sum * d__[j];
        jp = *npt + j;
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            sum += bmat[jp + i__ * bmat_dim1] * d__[i__];
        }
        vlag[jp] = sum;
        bsum += sum * d__[j];
        dx += d__[j] * xopt[j];
    }
    beta = dx * dx + dsq * (xoptsq + dx + dx + half * dsq) + beta - bsum;
    vlag[kopt] += one;

    /* NTRITS == 0: maybe swap in the Cauchy step; RESCUE/return on damaged denominators. */
    if (ntrits == 0)
    {
        d__1 = vlag[knew];
        denom = d__1 * d__1 + alpha * beta;
        if (denom < cauchy && cauchy > zero)
        {
            i__2 = *n;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                xnew[i__] = xalt[i__];
                d__[i__] = xnew[i__] - xopt[i__];
            }
            cauchy = zero;
            goto L230;
        }
        d__1 = vlag[knew];
        if (denom <= half * (d__1 * d__1))
        {
            if (stop->nevals > nresc)
            {
                goto L190;
            }
            rc = Rc::RoundoffLimited; // much cancellation in a denominator
            goto L720;
        }
    }
    else
    {
        /* NTRITS > 0: KNEW = the point to delete for the trust-region step. */
        delsq = delta * delta;
        scaden = zero;
        biglsq = zero;
        knew = 0;
        i__2 = *npt;
        for (k = 1; k <= i__2; ++k)
        {
            if (k == kopt)
            {
                goto L350;
            }
            hdiag = zero;
            i__1 = nptm;
            for (jj = 1; jj <= i__1; ++jj)
            {
                d__1 = zmat[k + jj * zmat_dim1];
                hdiag += d__1 * d__1;
            }
            d__1 = vlag[k];
            den = beta * hdiag + d__1 * d__1;
            distsq = zero;
            i__1 = *n;
            for (j = 1; j <= i__1; ++j)
            {
                d__1 = xpt[k + j * xpt_dim1] - xopt[j];
                distsq += d__1 * d__1;
            }
            d__3 = distsq / delsq;
            d__2 = d__3 * d__3;
            temp = one >= d__2 ? one : d__2;
            if (temp * den > scaden)
            {
                scaden = temp * den;
                knew = k;
                denom = den;
            }
            d__3 = vlag[k];
            d__1 = biglsq;
            d__2 = temp * (d__3 * d__3);
            biglsq = d__1 >= d__2 ? d__1 : d__2;
        L350:;
        }
        if (scaden <= half * biglsq)
        {
            if (stop->nevals > nresc)
            {
                goto L190;
            }
            rc = Rc::RoundoffLimited; // much cancellation in a denominator
            goto L720;
        }
    }

/* Evaluate F at XBASE+XNEW (bound-adjusted), unless the eval cap stops us. */
L360:
    i__2 = *n;
    for (i__ = 1; i__ <= i__2; ++i__)
    {
        d__3 = xl[i__];
        d__4 = xbase[i__] + xnew[i__];
        d__1 = d__3 >= d__4 ? d__3 : d__4;
        d__2 = xu[i__];
        x[i__] = d__1 <= d__2 ? d__1 : d__2;
        if (xnew[i__] == sl[i__])
        {
            x[i__] = xl[i__];
        }
        if (xnew[i__] == su[i__])
        {
            x[i__] = xu[i__];
        }
    }
    if (stop_evals(*stop))
    {
        rc = Rc::MaxevalReached;
    }
    if (rc != Rc::Success)
    {
        goto L720;
    }
    ++stop->nevals;
    f = calfun(*n, &x[1]);
    if (ntrits == -1)
    {
        fsave = f;
        rc = Rc::XtolReached;
        if (fsave < fval[kopt])
        {
            *minf = f;
            return rc;
        }
        goto L720;
    }
    if (f < stop->minf_max)
    {
        *minf = f;
        return Rc::MinfMaxReached;
    }

    /* The model's predicted change; DIFF = the prediction error. */
    fopt = fval[kopt];
    vquad = zero;
    ih = 0;
    i__2 = *n;
    for (j = 1; j <= i__2; ++j)
    {
        vquad += d__[j] * gopt[j];
        i__1 = j;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            ++ih;
            temp = d__[i__] * d__[j];
            if (i__ == j)
            {
                temp = half * temp;
            }
            vquad += hq[ih] * temp;
        }
    }
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        d__1 = w[*npt + k];
        vquad += half * pq[k] * (d__1 * d__1);
    }
    diff = f - fopt - vquad;
    diffc = diffb;
    diffb = diffa;
    diffa = crd::math::fabs(diff);
    if (dnorm > rho)
    {
        nfsav = stop->nevals;
    }

    /* DELTA after a trust-region step; recompute KNEW/DENOM when F improved. */
    if (ntrits > 0)
    {
        if (vquad >= zero)
        {
            rc = Rc::RoundoffLimited; // a trust-region step failed to reduce Q
            goto L720;
        }
        ratio = (f - fopt) / vquad;
        if (ratio <= tenth)
        {
            d__1 = half * delta;
            delta = d__1 <= dnorm ? d__1 : dnorm;
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
        if (f < fopt)
        {
            ksav = knew;
            densav = denom;
            delsq = delta * delta;
            scaden = zero;
            biglsq = zero;
            knew = 0;
            i__1 = *npt;
            for (k = 1; k <= i__1; ++k)
            {
                hdiag = zero;
                i__2 = nptm;
                for (jj = 1; jj <= i__2; ++jj)
                {
                    d__1 = zmat[k + jj * zmat_dim1];
                    hdiag += d__1 * d__1;
                }
                d__1 = vlag[k];
                den = beta * hdiag + d__1 * d__1;
                distsq = zero;
                i__2 = *n;
                for (j = 1; j <= i__2; ++j)
                {
                    d__1 = xpt[k + j * xpt_dim1] - xnew[j];
                    distsq += d__1 * d__1;
                }
                d__3 = distsq / delsq;
                d__2 = d__3 * d__3;
                temp = one >= d__2 ? one : d__2;
                if (temp * den > scaden)
                {
                    scaden = temp * den;
                    knew = k;
                    denom = den;
                }
                d__3 = vlag[k];
                d__1 = biglsq;
                d__2 = temp * (d__3 * d__3);
                biglsq = d__1 >= d__2 ? d__1 : d__2;
            }
            if (scaden <= half * biglsq)
            {
                knew = ksav;
                denom = densav;
            }
        }
    }

    /* Move point KNEW (update), fold its PQ into HQ, spread DIFF, include the new point. */
    update<T>(n, npt, &bmat[bmat_offset], &zmat[zmat_offset], ndim, &vlag[1], &beta, &denom, &knew, &w[1]);
    ih = 0;
    pqold = pq[knew];
    pq[knew] = zero;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        temp = pqold * xpt[knew + i__ * xpt_dim1];
        i__2 = i__;
        for (j = 1; j <= i__2; ++j)
        {
            ++ih;
            hq[ih] += temp * xpt[knew + j * xpt_dim1];
        }
    }
    i__2 = nptm;
    for (jj = 1; jj <= i__2; ++jj)
    {
        temp = diff * zmat[knew + jj * zmat_dim1];
        i__1 = *npt;
        for (k = 1; k <= i__1; ++k)
        {
            pq[k] += temp * zmat[k + jj * zmat_dim1];
        }
    }
    fval[knew] = f;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__)
    {
        xpt[knew + i__ * xpt_dim1] = xnew[i__];
        w[i__] = bmat[knew + i__ * bmat_dim1];
    }
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        suma = zero;
        i__2 = nptm;
        for (jj = 1; jj <= i__2; ++jj)
        {
            suma += zmat[knew + jj * zmat_dim1] * zmat[k + jj * zmat_dim1];
        }
        if (std::isinf(suma))
        {
            rc = Rc::RoundoffLimited; // SGJ singularity detection
            goto L720;
        }
        sumb = zero;
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
        {
            sumb += xpt[k + j * xpt_dim1] * xopt[j];
        }
        temp = suma * sumb;
        i__2 = *n;
        for (i__ = 1; i__ <= i__2; ++i__)
        {
            w[i__] += temp * xpt[k + i__ * xpt_dim1];
        }
    }
    i__2 = *n;
    for (i__ = 1; i__ <= i__2; ++i__)
    {
        gopt[i__] += diff * w[i__];
    }

    /* XOPT/GOPT/KOPT when F improved. */
    if (f < fopt)
    {
        kopt = knew;
        xoptsq = zero;
        ih = 0;
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
        {
            xopt[j] = xnew[j];
            d__1 = xopt[j];
            xoptsq += d__1 * d__1;
            i__1 = j;
            for (i__ = 1; i__ <= i__1; ++i__)
            {
                ++ih;
                if (i__ < j)
                {
                    gopt[j] += hq[ih] * d__[i__];
                }
                gopt[i__] += hq[ih] * d__[j];
            }
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
            temp = pq[k] * temp;
            i__2 = *n;
            for (i__ = 1; i__ <= i__2; ++i__)
            {
                gopt[i__] += temp * xpt[k + i__ * xpt_dim1];
            }
        }
        if (stop_ftol(*stop, f, fopt))
        {
            rc = Rc::FtolReached;
            goto L720;
        }
    }

    /* The least-Frobenius-norm interpolant test (the ITEST mechanism, bound-aware projected gradients). */
    if (ntrits > 0)
    {
        i__2 = *npt;
        for (k = 1; k <= i__2; ++k)
        {
            vlag[k] = fval[k] - fval[kopt];
            w[k] = zero;
        }
        i__2 = nptm;
        for (j = 1; j <= i__2; ++j)
        {
            sum = zero;
            i__1 = *npt;
            for (k = 1; k <= i__1; ++k)
            {
                sum += zmat[k + j * zmat_dim1] * vlag[k];
            }
            i__1 = *npt;
            for (k = 1; k <= i__1; ++k)
            {
                w[k] += sum * zmat[k + j * zmat_dim1];
            }
        }
        i__1 = *npt;
        for (k = 1; k <= i__1; ++k)
        {
            sum = zero;
            i__2 = *n;
            for (j = 1; j <= i__2; ++j)
            {
                sum += xpt[k + j * xpt_dim1] * xopt[j];
            }
            w[k + *npt] = w[k];
            w[k] = sum * w[k];
        }
        gqsq = zero;
        gisq = zero;
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            sum = zero;
            i__2 = *npt;
            for (k = 1; k <= i__2; ++k)
            {
                sum = sum + bmat[k + i__ * bmat_dim1] * vlag[k] + xpt[k + i__ * xpt_dim1] * w[k];
            }
            if (xopt[i__] == sl[i__])
            {
                d__2 = zero;
                d__3 = gopt[i__];
                d__1 = d__2 <= d__3 ? d__2 : d__3;
                gqsq += d__1 * d__1;
                d__1 = zero <= sum ? zero : sum;
                gisq += d__1 * d__1;
            }
            else if (xopt[i__] == su[i__])
            {
                d__2 = zero;
                d__3 = gopt[i__];
                d__1 = d__2 >= d__3 ? d__2 : d__3;
                gqsq += d__1 * d__1;
                d__1 = zero >= sum ? zero : sum;
                gisq += d__1 * d__1;
            }
            else
            {
                d__1 = gopt[i__];
                gqsq += d__1 * d__1;
                gisq += sum * sum;
            }
            vlag[*npt + i__] = sum;
        }

        ++itest;
        if (gqsq < ten * gisq)
        {
            itest = 0;
        }
        if (itest >= 3)
        {
            i__1 = *npt >= nh ? *npt : nh;
            for (i__ = 1; i__ <= i__1; ++i__)
            {
                if (i__ <= *n)
                {
                    gopt[i__] = vlag[*npt + i__];
                }
                if (i__ <= *npt)
                {
                    pq[i__] = w[*npt + i__];
                }
                if (i__ <= nh)
                {
                    hq[i__] = zero;
                }
                itest = 0;
            }
        }
    }

    /* Sufficient decrease ⇒ another trust-region iteration (NTRITS == 0 = an alternative step). */
    if (ntrits == 0)
    {
        goto L60;
    }
    if (f <= fopt + tenth * vquad)
    {
        goto L60;
    }

    /* Are the points close enough to the best one? */
    d__3 = two * delta;
    d__4 = ten * rho;
    d__1 = d__3 * d__3;
    d__2 = d__4 * d__4;
    distsq = d__1 >= d__2 ? d__1 : d__2;
L650:
    knew = 0;
    i__1 = *npt;
    for (k = 1; k <= i__1; ++k)
    {
        sum = zero;
        i__2 = *n;
        for (j = 1; j <= i__2; ++j)
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

    /* KNEW > 0 ⇒ ALTMOV via L90; else another TR iteration or the RHO schedule. */
    if (knew > 0)
    {
        dist = crd::math::sqrt(distsq);
        if (ntrits == -1)
        {
            d__1 = tenth * delta;
            d__2 = half * dist;
            delta = d__1 <= d__2 ? d__1 : d__2;
            if (delta <= rho * static_cast<T>(1.5))
            {
                delta = rho;
            }
        }
        ntrits = 0;
        d__2 = tenth * dist;
        d__1 = d__2 <= delta ? d__2 : delta;
        adelt = d__1 >= rho ? d__1 : rho;
        dsq = adelt * adelt;
        goto L90;
    }
    if (ntrits == -1)
    {
        goto L680;
    }
    if (ratio > zero)
    {
        goto L60;
    }
    if ((delta >= dnorm ? delta : dnorm) > rho)
    {
        goto L60;
    }

/* The next RHO and DELTA. */
L680:
    if (rho > *rhoend)
    {
        delta = half * rho;
        ratio = rho / *rhoend;
        if (ratio <= static_cast<T>(16.))
        {
            rho = *rhoend;
        }
        else if (ratio <= static_cast<T>(250.))
        {
            rho = crd::math::sqrt(ratio) * *rhoend;
        }
        else
        {
            rho = tenth * rho;
        }
        delta = delta >= rho ? delta : rho;
        ntrits = 0;
        nfsav = stop->nevals;
        goto L60;
    }

    /* Return — after one more Newton-Raphson step if it was too short to have been tried. */
    if (ntrits == -1)
    {
        goto L360;
    }
L720:
    /* SGJ: unconditionally return the best point (safer on sudden stops). */
    {
        i__1 = *n;
        for (i__ = 1; i__ <= i__1; ++i__)
        {
            d__3 = xl[i__];
            d__4 = xbase[i__] + xopt[i__];
            d__1 = d__3 >= d__4 ? d__3 : d__4;
            d__2 = xu[i__];
            x[i__] = d__1 <= d__2 ? d__1 : d__2;
            if (xopt[i__] == sl[i__])
            {
                x[i__] = xl[i__];
            }
            if (xopt[i__] == su[i__])
            {
                x[i__] = xu[i__];
            }
        }
        f = fval[kopt];
    }
    *minf = f;
    return rc;
} /* bobyqb */

} // namespace detail::bobyqa_impl

// ------------------------------------------------------------------------------------------ public driver
// min f(x) s.t. lower ≤ x ≤ upper (REQUIRED, with upper − lower ≥ 2·rhobeg per Powell — asserted). The
// NLopt rescaling layer is NOT ported (the e2e diff calls the oracle with equal dx ⇒ identity scaling);
// the bound-preprocessing block (x adjustment + the SL/SU shifted bounds) IS Powell's own and is ported.
// Value-only; n ≥ 2 (the reference's requirement; n = 0 trivially succeeds).
template <typename T>
[[nodiscard]] OptResult<T> minimize_bobyqa(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                           crd::containers::ConstSpan<T> lower, crd::containers::ConstSpan<T> upper,
                                           crd::memory::IAllocator* alloc, const BobyqaOptions<T>& bo = {})
{
    namespace bi = detail::bobyqa_impl;
    const crd::usize nn = obj.n();
    CRD_ASSERT_MSG(x0.size() == nn, "minimize_bobyqa: x0 size mismatch");
    CRD_ASSERT_MSG(lower.size() == nn && upper.size() == nn, "minimize_bobyqa: bounds required (use NEWUOA otherwise)");

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
    CRD_ASSERT_MSG(nn >= 2, "minimize_bobyqa: n must be >= 2 (the reference's requirement)");

    const int n = static_cast<int>(nn);
    int npt = bo.npt > 0 ? static_cast<int>(bo.npt) : 2 * n + 1;
    const int npt_max = (n + 2) * (n + 1) / 2;
    npt = npt < n + 2 ? n + 2 : (npt > npt_max ? npt_max : npt);
    const int np = n + 1;
    const int ndim = npt + n;
    const T rhobeg = bo.rhobeg;
    const T rhoend = bo.rhoend;

    for (crd::usize j = 0; j < nn; ++j)
    {
        CRD_ASSERT_MSG(upper[j] - lower[j] >= rhobeg + rhobeg,
                       "minimize_bobyqa: a bound gap is below 2*rhobeg (Powell's requirement)");
    }

    // The reference w-partition from bobyqa().
    crd::containers::Array<T> w(alloc);
    w.resize(static_cast<crd::usize>((npt + 5) * (npt + n) + 3 * n * (n + 5) / 2));
    T* wp = w.data() - 1; // 1-based view
    const int ixb = 1;
    const int ixp = ixb + n;
    const int ifv = ixp + n * npt;
    const int ixo = ifv + npt;
    const int igo = ixo + n;
    const int ihq = igo + n;
    const int ipq = ihq + n * np / 2;
    const int ibmat = ipq + npt;
    const int izmat = ibmat + ndim * n;
    const int isl = izmat + npt * (npt - np);
    const int isu = isl + n;
    const int ixn = isu + n;
    const int ixa = ixn + n;
    const int id = ixa + n;
    const int ivl = id + n;
    const int iw = ivl + ndim;

    // Bound-aware adjustment of the initial x + the SL/SU shifted bounds (the reference's L30 loop).
    crd::containers::Array<T> xbuf(alloc);
    xbuf.resize(nn);
    for (crd::usize j = 0; j < nn; ++j)
    {
        xbuf[j] = x0[j];
    }
    {
        const T zero = static_cast<T>(0);
        for (int j = 1; j <= n; ++j)
        {
            const T temp = upper[static_cast<crd::usize>(j - 1)] - lower[static_cast<crd::usize>(j - 1)];
            const int jsl = isl + j - 1;
            const int jsu = jsl + n;
            T& xj = xbuf[static_cast<crd::usize>(j - 1)];
            const T xlj = lower[static_cast<crd::usize>(j - 1)];
            const T xuj = upper[static_cast<crd::usize>(j - 1)];
            wp[jsl] = xlj - xj;
            wp[jsu] = xuj - xj;
            if (wp[jsl] >= -rhobeg)
            {
                if (wp[jsl] >= zero)
                {
                    xj = xlj;
                    wp[jsl] = zero;
                    wp[jsu] = temp;
                }
                else
                {
                    xj = xlj + rhobeg;
                    wp[jsl] = -rhobeg;
                    const T d1 = xuj - xj;
                    wp[jsu] = d1 >= rhobeg ? d1 : rhobeg;
                }
            }
            else if (wp[jsu] <= rhobeg)
            {
                if (wp[jsu] <= zero)
                {
                    xj = xuj;
                    wp[jsl] = -temp;
                    wp[jsu] = zero;
                }
                else
                {
                    xj = xuj - rhobeg;
                    const T d1 = xlj - xj;
                    const T d2 = -rhobeg;
                    wp[jsl] = d1 <= d2 ? d1 : d2;
                    wp[jsu] = rhobeg;
                }
            }
        }
    }

    bi::Stop<T> stop;
    stop.maxeval = bo.max_evals > 0 ? static_cast<int>(bo.max_evals) : 1000 * (n + 1);
    stop.ftol_rel = bo.ftol_rel;
    stop.ftol_abs = bo.ftol_abs;

    crd::usize fn_evals = 0;
    auto calfun = [&](int cn, const T* xx) -> T
    {
        ++fn_evals;
        return obj.value({xx, static_cast<crd::usize>(cn)});
    };

    T minf = std::numeric_limits<T>::infinity();
    const bi::Rc rc =
        bi::bobyqb<T>(&n, &npt, xbuf.data(), lower.data(), upper.data(), &rhobeg, &rhoend, &stop, calfun, &minf,
                      &wp[ixb], &wp[ixp], &wp[ifv], &wp[ixo], &wp[igo], &wp[ihq], &wp[ipq], &wp[ibmat], &wp[izmat],
                      &ndim, &wp[isl], &wp[isu], &wp[ixn], &wp[ixa], &wp[id], &wp[ivl], &wp[iw]);

    for (crd::usize i = 0; i < nn; ++i)
    {
        result.x[i] = xbuf[i];
    }
    result.fx = minf;
    result.fn_evals = fn_evals;
    result.iterations = static_cast<crd::usize>(stop.nevals); // BOBYQA counts evaluations
    switch (rc)
    {
        case bi::Rc::XtolReached:
        case bi::Rc::FtolReached:
        case bi::Rc::MinfMaxReached:
        case bi::Rc::Success:
            result.status = OptStatus::Success;
            break;
        case bi::Rc::MaxevalReached:
            result.status = OptStatus::MaxIterations;
            break;
        case bi::Rc::RoundoffLimited:
        default:
            result.status = OptStatus::SmallStep; // rounding-limited stall (documented mapping)
            break;
    }
    result.converged = result.status == OptStatus::Success;
    return result;
}

} // namespace crd::hesap::opt
