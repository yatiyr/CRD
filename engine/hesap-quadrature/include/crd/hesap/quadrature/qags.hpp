#pragma once

// crd-hesap-quadrature v13-h — QAGS: the adaptive integrator with Wynn-ε EXTRAPOLATION (de Doncker 1978), the QUADPACK
// workhorse and scipy.integrate.quad's default. Where plain QAG bisects and sums (slow when the integrand has an
// endpoint singularity — the local error decays like a power law), QAGS detects the slow-converging regime, switches
// to "extrapolation mode", and applies the epsilon algorithm (Wynn 1956) to the sequence of partial integrals to
// leap to the limit. It is THE routine that integrates 1/√x, ln x, etc. to full precision in a handful of panels.
//
// This is a faithful, goto-preserving transliteration of scipy/QUADPACK dqagse + dqelg + dqpsrt (the v7 NLopt-port
// discipline) — verified bit-close to scipy.integrate.quad on singular integrands. The GK21 panels reuse the
// engine's verified gauss_kronrod_21. Determinism by construction (fixed FP order, deterministic interval selection);
// the work-stack is allocated once (size limit = the WCET bound). ⚠ HONESTY (pillar 3): error_estimate is the
// extrapolated Tier-1 estimate, never a guaranteed bound.

#include <crd/hesap/quadrature/gauss_kronrod.hpp>
#include <crd/hesap/quadrature/integrate.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>

namespace crd::hesap::quadrature
{

// Reusable work-stack for the adaptive QAGS/QAGI drivers: the subinterval lists (a/b/result/error) + the error-order
// index, sized once to `limit`. Allocate ONCE, integrate many — the allocation-free hot path (ADR-0095 pillar 2). The
// per-call-allocating convenience overloads create one of these internally; pass your own to amortize across calls.
template <typename T>
struct AdaptiveWorkspace
{
    crd::containers::Array<T>   alist;
    crd::containers::Array<T>   blist;
    crd::containers::Array<T>   rlist;
    crd::containers::Array<T>   elist;
    crd::containers::Array<int> iord;
    int                         limit;

    AdaptiveWorkspace(crd::memory::IAllocator* alloc, int subdivision_limit)
        : alist(alloc), blist(alloc), rlist(alloc), elist(alloc), iord(alloc), limit(subdivision_limit)
    {
        const crd::usize cap = static_cast<crd::usize>(subdivision_limit < 1 ? 1 : subdivision_limit);
        alist.resize(cap);
        blist.resize(cap);
        rlist.resize(cap);
        elist.resize(cap);
        iord.resize(cap);
    }
};

namespace detail
{

// QUADPACK dqpsrt — maintain the descending ordering of the local error estimates (top-down insert of the new
// largest, bottom-up of the smallest). 0-based, faithful port.
template <typename T>
inline void qpsrt(int limit, int last, int* maxerr, T* ermax, const T* elist, int* iord, int* nrmax)
{
    if (last <= 2)
    {
        iord[0] = 0;
        iord[1] = 1;
        *maxerr = iord[*nrmax];
        *ermax  = elist[*maxerr];
        return;
    }
    T errmax = elist[*maxerr];
    while ((*nrmax > 0) && (errmax > elist[iord[*nrmax - 1]]))
    {
        iord[*nrmax] = iord[*nrmax - 1];
        *nrmax -= 1;
    }
    const int jupbn  = (last > limit / 2 + 2) ? (limit - last + 2) : (last - 1);
    const T   errmin = elist[last - 1];
    const int jbnd   = jupbn - 1;
    const int ibeg   = *nrmax + 1;
    int       i      = ibeg;
    bool      to60   = false;
    if (ibeg <= jbnd)
    {
        for (i = ibeg; i <= jbnd; ++i)
        {
            if (errmax >= elist[iord[i]])
            {
                to60 = true;
                break;
            }
            iord[i - 1] = iord[i];
        }
    }
    if (!to60)
    {
        iord[jbnd]  = *maxerr;
        iord[jupbn] = last - 1;
        *maxerr     = iord[*nrmax];
        *ermax      = elist[*maxerr];
        return;
    }
    // LINE60 — insert errmin bottom-up
    iord[i - 1] = *maxerr;
    int k       = jbnd;
    for (int j = i; j <= jbnd; ++j)
    {
        if (errmin < elist[iord[k]])
        {
            break;
        }
        iord[k + 1] = iord[k];
        --k;
    }
    if (errmin < elist[iord[k]])
    {
        iord[k + 1] = last - 1;
    }
    else
    {
        iord[i] = last - 1;
    }
    *maxerr = iord[*nrmax];
    *ermax  = elist[*maxerr];
}

// QUADPACK dqelg — the epsilon algorithm (Wynn). epstab (size ≥ 52) holds the two lower diagonals of the ε-table;
// res3la holds the last 3 results. Faithful port.
template <typename T>
inline void qelg(int* n, T* epstab, T* result, T* abserr, T* res3la, int* nres)
{
    const T   epmach = std::numeric_limits<T>::epsilon();
    const T   oflow  = std::numeric_limits<T>::max();
    int       i, indx, k1, k2, k3, newelm, num;
    T         delta1, delta2, delta3, epsinf, error, err1, err2, err3;
    T         e0, e1, e1abs, e2, e3, res, ss, tol1, tol2, tol3;

    num     = *n;
    *nres += 1;
    *abserr = oflow;
    *result = epstab[*n];
    if (*n < 2)
    {
        *abserr = detail::qmax<T>(*abserr, static_cast<T>(5) * epmach * crd::math::fabs(*result));
        return;
    }
    const int limexp = 49;
    epstab[*n + 2]   = epstab[*n];
    newelm           = *n / 2;
    epstab[*n]       = oflow;
    k1               = *n;
    for (i = 0; i < newelm; ++i)
    {
        k2     = k1 - 1;
        k3     = k1 - 2;
        res    = epstab[k1 + 2];
        e0     = epstab[k3];
        e1     = epstab[k2];
        e2     = res;
        e1abs  = crd::math::fabs(e1);
        delta2 = e2 - e1;
        err2   = crd::math::fabs(delta2);
        tol2   = detail::qmax<T>(crd::math::fabs(e2), e1abs) * epmach;
        delta3 = e1 - e0;
        err3   = crd::math::fabs(delta3);
        tol3   = detail::qmax<T>(e1abs, crd::math::fabs(e0)) * epmach;
        if (!((err2 > tol2) || (err3 > tol3)))
        {
            *result = res;
            *abserr = err2 + err3;
            *abserr = detail::qmax<T>(*abserr, static_cast<T>(5) * epmach * crd::math::fabs(*result));
            return;
        }
        e3         = epstab[k1];
        epstab[k1] = e1;
        delta1     = e1 - e3;
        err1       = crd::math::fabs(delta1);
        tol1       = detail::qmax<T>(e1abs, crd::math::fabs(e3)) * epmach;
        if ((err1 <= tol1) || (err2 <= tol2) || (err3 <= tol3))
        {
            *n = i + i;
            break;
        }
        ss     = static_cast<T>(1) / delta1 + static_cast<T>(1) / delta2 - static_cast<T>(1) / delta3;
        epsinf = crd::math::fabs(ss * e1);
        if (!(epsinf > static_cast<T>(1e-4)))
        {
            *n = i + i;
            break;
        }
        res        = e1 + static_cast<T>(1) / ss;
        epstab[k1] = res;
        k1 -= 2;
        error = err2 + crd::math::fabs(res - e2) + err3;
        if (!(error > *abserr))
        {
            *abserr = error;
            *result = res;
        }
    }
    if (*n == limexp)
    {
        *n = 2 * (limexp / 2);
    }
    const int n2_rem = num % 2;
    for (i = 0; i <= newelm; ++i)
    {
        epstab[2 * i + n2_rem] = epstab[2 * i + 2 + n2_rem];
    }
    if (*n != num)
    {
        indx = num - *n;
        for (i = 0; i <= *n; ++i)
        {
            epstab[i] = epstab[indx];
            ++indx;
        }
    }
    if (*nres < 4)
    {
        res3la[*nres - 1] = *result;
        *abserr           = oflow;
    }
    else
    {
        *abserr   = crd::math::fabs(*result - res3la[2]) + crd::math::fabs(*result - res3la[1])
                  + crd::math::fabs(*result - res3la[0]);
        res3la[0] = res3la[1];
        res3la[1] = res3la[2];
        res3la[2] = *result;
    }
    *abserr = detail::qmax<T>(*abserr, static_cast<T>(5) * epmach * crd::math::fabs(*result));
}

// QUADPACK dqk15i — the 15-point Gauss-Kronrod rule on a SEMI/DOUBLY-INFINITE range mapped onto (0,1) via
// x = boun + dinf·(1−t)/t (Jacobian 1/t²). inf = +1 → (boun,+∞), −1 → (−∞,boun), +2 → (−∞,+∞) (the two halves are
// summed). [a,b] ⊆ (0,1) is the current panel in t-space. Faithful port.
template <typename T, typename F>
[[nodiscard]] GkResult<T> gk15i(F&& f, T boun, int inf, T a, T b)
{
    static constexpr T wg[8]  = {static_cast<T>(0),
                                 static_cast<T>(0.129484966168869693270611432679082),
                                 static_cast<T>(0),
                                 static_cast<T>(0.279705391489276667901467771423780),
                                 static_cast<T>(0),
                                 static_cast<T>(0.381830050505118944950369775488975),
                                 static_cast<T>(0),
                                 static_cast<T>(0.417959183673469387755102040816327)};
    static constexpr T xgk[8] = {static_cast<T>(0.991455371120812639206854697526329),
                                 static_cast<T>(0.949107912342758524526189684047851),
                                 static_cast<T>(0.864864423359769072789712788640926),
                                 static_cast<T>(0.741531185599394439863864773280788),
                                 static_cast<T>(0.586087235467691130294144838258730),
                                 static_cast<T>(0.405845151377397166906606412076961),
                                 static_cast<T>(0.207784955007898467600689403773245),
                                 static_cast<T>(0)};
    static constexpr T wgk[8] = {static_cast<T>(0.022935322010529224963732008058970),
                                 static_cast<T>(0.063092092629978553290700663189204),
                                 static_cast<T>(0.104790010322250183839876322541518),
                                 static_cast<T>(0.140653259715525918745189590510238),
                                 static_cast<T>(0.169004726639267902826583426598550),
                                 static_cast<T>(0.190350578064785409913256402421014),
                                 static_cast<T>(0.204432940075298892414161999234649),
                                 static_cast<T>(0.209482141084727828012999174891714)};
    const T dinf  = (inf > 1) ? T{1} : static_cast<T>(inf);
    const T centr = static_cast<T>(0.5) * (a + b);
    const T hlgth = static_cast<T>(0.5) * (b - a);
    T       tabsc1 = boun + dinf * (T{1} - centr) / centr;
    T       fval1  = f(tabsc1);
    if (inf == 2)
    {
        tabsc1 = -tabsc1;
        fval1  = fval1 + f(tabsc1);
    }
    const T fc     = (fval1 / centr) / centr;
    T       resg   = fc * wg[7];
    T       resk   = fc * wgk[7];
    T       resabs = crd::math::fabs(resk);
    T       fv1[7];
    T       fv2[7];
    for (int j = 0; j < 7; ++j)
    {
        const T absc  = hlgth * xgk[j];
        const T absc1 = centr - absc;
        const T absc2 = centr + absc;
        T       t1    = boun + dinf * (T{1} - absc1) / absc1;
        T       t2    = boun + dinf * (T{1} - absc2) / absc2;
        T       f1    = f(t1);
        T       f2    = f(t2);
        if (inf == 2)
        {
            t1 = -t1;
            t2 = -t2;
            f1 = f1 + f(t1);
            f2 = f2 + f(t2);
        }
        f1     = (f1 / absc1) / absc1;
        f2     = (f2 / absc2) / absc2;
        fv1[j] = f1;
        fv2[j] = f2;
        const T fsum = f1 + f2;
        resg += wg[j] * fsum;
        resk += wgk[j] * fsum;
        resabs += wgk[j] * (crd::math::fabs(f1) + crd::math::fabs(f2));
    }
    const T reskh  = resk * static_cast<T>(0.5);
    T       resasc = wgk[7] * crd::math::fabs(fc - reskh);
    for (int j = 0; j < 7; ++j)
    {
        resasc += wgk[j] * (crd::math::fabs(fv1[j] - reskh) + crd::math::fabs(fv2[j] - reskh));
    }
    GkResult<T> r;
    r.value      = resk * hlgth;
    r.resabs     = resabs * hlgth;
    r.resasc     = resasc * hlgth;
    T abserr     = crd::math::fabs((resk - resg) * hlgth);
    if (r.resasc != T{0} && abserr != T{0})
    {
        const T rat = static_cast<T>(200) * abserr / r.resasc; // x^1.5 = x·√x — avoid the heavy double-double pow
        abserr      = r.resasc * qmin<T>(T{1}, rat * crd::math::sqrt(rat));
    }
    const T uflow  = std::numeric_limits<T>::min();
    const T epmach = std::numeric_limits<T>::epsilon();
    if (r.resabs > uflow / (static_cast<T>(50) * epmach))
    {
        abserr = qmax<T>(epmach * static_cast<T>(50) * r.resabs, abserr);
    }
    r.abserr = abserr;
    return r;
}

// The shared QAGS/QAGI extrapolation driver (QUADPACK dqagse ≡ dqagie body): adaptive bisection over [a0,b0] with a
// pluggable panel evaluator `panel(a,b) → GkResult` (GK21 for QAGS, the transformed GK15i for QAGI), Wynn-ε
// extrapolation via qelg, error ordering via qpsrt. evals_per_panel converts the panel count to eval_count.
template <typename T, typename Panel>
[[nodiscard]] QuadResult<T> qags_driver(AdaptiveWorkspace<T>& ws, Panel&& panel, T a0, T b0, T epsabs, T epsrel,
                                        int evals_per_panel)
{
    const T   epmach = std::numeric_limits<T>::epsilon();
    const T   uflow  = std::numeric_limits<T>::min();
    const T   oflow  = std::numeric_limits<T>::max();
    const int limit  = ws.limit;
    if (limit < 1
        || ((epsabs <= T{0}) && (epsrel < qmax<T>(static_cast<T>(50) * epmach, static_cast<T>(0.5e-28)))))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }

    auto& alist = ws.alist; // reused caller workspace — no per-call allocation (the QAGS/QAGI hot path)
    auto& blist = ws.blist;
    auto& rlist = ws.rlist;
    auto& elist = ws.elist;
    auto& iord  = ws.iord;
    T     rlist2[52] = {};
    T     res3la[3]  = {};

    // declare everything up front (the gotos forbid jumping over initializers)
    int  ier = 0, ierror = 0, last = 1, maxerr = 0, nrmax = 0, nres = 0, numrl2 = 0, ktmin = 0;
    int  extrap = 0, noext = 0, iroff1 = 0, iroff2 = 0, iroff3 = 0, ksgn = 0, jupbnd = 0, k = 0, L = 0;
    T    result = T{0}, abserr = T{0}, defabs = T{0}, resabs = T{0}, dres = T{0}, errbnd = T{0};
    T    erlarg = T{0}, ertest = T{0}, correc = T{0}, errmax = T{0}, area = T{0}, errsum = T{0};
    T    small = T{0}, a1, b1, a2, b2, area1, area2, area12, error1, error2, error12, defab1, defab2, erlast;
    T    reseps = T{0}, abseps = T{0};

    alist[0] = a0;
    blist[0] = b0;

    const GkResult<T> g0 = panel(a0, b0);
    result = g0.value;
    abserr = g0.abserr;
    defabs = g0.resabs;
    resabs = g0.resasc;

    dres     = crd::math::fabs(result);
    errbnd   = detail::qmax<T>(epsabs, epsrel * dres);
    last     = 1;
    rlist[0] = result;
    elist[0] = abserr;
    iord[0]  = 0;
    if ((abserr <= static_cast<T>(100) * epmach * defabs) && (abserr > errbnd))
    {
        ier = 2;
    }
    if (limit == 1)
    {
        ier = 1;
    }
    if ((ier != 0) || ((abserr <= errbnd) && (abserr != resabs)) || (abserr == T{0}))
    {
        goto LINE140;
    }

    rlist2[0] = result;
    errmax    = abserr;
    maxerr    = 0;
    area      = result;
    errsum    = abserr;
    abserr    = oflow;
    nrmax     = 0;
    nres      = 0;
    numrl2    = 1;
    ktmin     = 0;
    extrap    = 0;
    noext     = 0;
    iroff1 = iroff2 = iroff3 = 0;
    ksgn = (dres >= (static_cast<T>(1) - static_cast<T>(50) * epmach) * defabs) ? 1 : -1;

    for (L = 1; L < limit; ++L)
    {
        last = L + 1;
        a1   = alist[maxerr];
        b1   = static_cast<T>(0.5) * (alist[maxerr] + blist[maxerr]);
        a2   = b1;
        b2   = blist[maxerr];
        erlast = errmax;
        {
            const GkResult<T> r1 = panel(a1, b1);
            const GkResult<T> r2 = panel(a2, b2);
            area1  = r1.value;
            error1 = r1.abserr;
            defab1 = r1.resasc;
            area2  = r2.value;
            error2 = r2.abserr;
            defab2 = r2.resasc;
        }
        area12  = area1 + area2;
        error12 = error1 + error2;
        errsum  = errsum + error12 - errmax;
        area    = area + area12 - rlist[maxerr];
        if ((defab1 != error1) && (defab2 != error2))
        {
            if (!((crd::math::fabs(rlist[maxerr] - area12) > static_cast<T>(1e-5) * crd::math::fabs(area12))
                  || (error12 < static_cast<T>(0.99) * errmax)))
            {
                if (extrap)
                {
                    ++iroff2;
                }
                else
                {
                    ++iroff1;
                }
            }
            if ((L > 9) && (error12 > errmax))
            {
                ++iroff3;
            }
        }
        rlist[maxerr] = area1;
        rlist[L]      = area2;
        errbnd        = detail::qmax<T>(epsabs, epsrel * crd::math::fabs(area));
        if (((iroff1 + iroff2) >= 10) || (iroff3 >= 20))
        {
            ier = 2;
        }
        if (iroff2 >= 5)
        {
            ierror = 3;
        }
        if (last == limit)
        {
            ier = 1;
        }
        if (detail::qmax<T>(crd::math::fabs(a1), crd::math::fabs(b2))
            <= (static_cast<T>(1) + static_cast<T>(100) * epmach) * (crd::math::fabs(a2) + static_cast<T>(1000) * uflow))
        {
            ier = 4;
        }
        if (!(error2 > error1))
        {
            alist[L]      = a2;
            blist[maxerr] = b1;
            blist[L]      = b2;
            elist[maxerr] = error1;
            elist[L]      = error2;
        }
        else
        {
            alist[maxerr] = a2;
            alist[L]      = a1;
            blist[L]      = b1;
            rlist[maxerr] = area2;
            rlist[L]      = area1;
            elist[maxerr] = error2;
            elist[L]      = error1;
        }
        detail::qpsrt<T>(limit, last, &maxerr, &errmax, elist.data(), iord.data(), &nrmax);
        if (errsum <= errbnd)
        {
            goto LINE115;
        }
        if (ier != 0)
        {
            break;
        }
        if (L == 1)
        {
            goto LINE80;
        }
        if (noext)
        {
            continue;
        }
        erlarg = erlarg - erlast;
        if (crd::math::fabs(b1 - a1) > small)
        {
            erlarg = erlarg + error12;
        }
        if (!extrap)
        {
            if (crd::math::fabs(blist[maxerr] - alist[maxerr]) > small)
            {
                continue;
            }
            extrap = 1;
            nrmax  = 1;
        }
        if ((ierror == 3) || (erlarg <= ertest))
        {
            goto LINE60;
        }
        jupbnd = (last > 2 + (limit / 2)) ? (limit + 3 - last) : last;
        {
            bool to90 = false;
            for (k = nrmax; k < jupbnd; ++k)
            {
                maxerr = iord[nrmax];
                errmax = elist[maxerr];
                if (crd::math::fabs(blist[maxerr] - alist[maxerr]) > small)
                {
                    to90 = true;
                    break;
                }
                ++nrmax;
            }
            if (to90)
            {
                continue; // LINE90 is a no-op at the bottom of the loop
            }
        }
    LINE60:
        ++numrl2;
        rlist2[numrl2] = area;
        detail::qelg<T>(&numrl2, rlist2, &reseps, &abseps, res3la, &nres);
        ktmin += 1;
        if ((ktmin > 5) && (abserr < static_cast<T>(1e-3) * errsum))
        {
            ier = 5;
        }
        if (!(abseps >= abserr))
        {
            ktmin  = 0;
            abserr = abseps;
            result = reseps;
            correc = erlarg;
            ertest = detail::qmax<T>(epsabs, epsrel * crd::math::fabs(reseps));
            if (abserr <= ertest)
            {
                break;
            }
        }
        if (numrl2 == 0)
        {
            noext = 1;
        }
        if (ier == 5)
        {
            break;
        }
        maxerr = iord[0];
        errmax = elist[maxerr];
        nrmax  = 0;
        extrap = 0;
        small  = small * static_cast<T>(0.5);
        erlarg = errsum;
        continue;
    LINE80:
        small     = crd::math::fabs(b0 - a0) * static_cast<T>(0.375);
        erlarg    = errsum;
        ertest    = errbnd;
        rlist2[1] = area;
    }

    // Set final result and error estimate.
    if (abserr == oflow)
    {
        goto LINE115;
    }
    if ((ier + ierror) == 0)
    {
        goto LINE110;
    }
    if (ierror == 3)
    {
        abserr = abserr + correc;
    }
    if (ier == 0)
    {
        ier = 3;
    }
    if ((result != T{0}) && (area != T{0}))
    {
        if (abserr / crd::math::fabs(result) > errsum / crd::math::fabs(area))
        {
            goto LINE115;
        }
        goto LINE110;
    }
    if (abserr > errsum)
    {
        goto LINE115;
    }
    if (area == T{0})
    {
        goto LINE130;
    }
LINE110:
    if ((ksgn == -1) && (detail::qmax<T>(crd::math::fabs(result), crd::math::fabs(area)) <= defabs * static_cast<T>(0.01)))
    {
        goto LINE130;
    }
    if ((static_cast<T>(0.01) > (result / area)) || ((result / area) > static_cast<T>(100))
        || (errsum > crd::math::fabs(area)))
    {
        ier = 6;
    }
    goto LINE130;
LINE115:
    result = T{0};
    for (k = 0; k <= L; ++k)
    {
        result = result + rlist[k];
    }
    abserr = errsum;
LINE130:
    if (ier > 2)
    {
        ier -= 1;
    }
LINE140:
    QuadResult<T> out;
    out.value          = result;
    out.error_estimate = abserr;
    out.subdiv_count   = static_cast<crd::u32>(last);
    out.eval_count     = static_cast<crd::u32>(evals_per_panel * (2 * last - 1));
    out.tolerance_met  = (ier == 0);
    out.status         = (ier == 0)   ? QuadStatus::Ok
                         : (ier == 1) ? QuadStatus::MaxSubdivisions
                                      : QuadStatus::RoundoffError;
    return out;
}

} // namespace detail

// Adaptive integration of f over [a,b] with Wynn-ε extrapolation (QUADPACK QAGS). Handles endpoint singularities that
// stall plain QAG. limit = the subdivision budget (WCET bound).
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_qags(AdaptiveWorkspace<T>& ws, F&& f, T a, T b, T epsabs, T epsrel)
{
    if (!detail::quad_finite(a) || !detail::quad_finite(b))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    return detail::qags_driver<T>(
        ws, [&](T pa, T pb) { return gauss_kronrod_21<T>(f, pa, pb); }, a, b, epsabs, epsrel, 21);
}

// Convenience overload — allocates the workspace once per call (pass an AdaptiveWorkspace to amortize across calls).
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_qags(crd::memory::IAllocator* alloc, F&& f, T a, T b, T epsabs, T epsrel,
                                           int limit = 50)
{
    AdaptiveWorkspace<T> ws(alloc, limit);
    return integrate_qags<T>(ws, static_cast<F&&>(f), a, b, epsabs, epsrel);
}

// Adaptive integration over a SEMI/DOUBLY-INFINITE range via the QUADPACK transform (QAGI). inf = +1 → ∫_bound^∞,
// inf = −1 → ∫_{−∞}^bound, inf = +2 → ∫_{−∞}^∞. Maps the range onto (0,1) and runs QAGS on the transformed GK15
// panels — the routine behind scipy.integrate.quad(f, a, ±inf). bound is ignored when inf = 2.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_qagi(AdaptiveWorkspace<T>& ws, F&& f, T bound, int inf, T epsabs, T epsrel)
{
    if (!detail::quad_finite(bound) || (inf != 1 && inf != -1 && inf != 2))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    const T   boun = (inf == 2) ? T{0} : bound;
    const int epp  = (inf == 2) ? 30 : 15;
    return detail::qags_driver<T>(
        ws, [&](T pa, T pb) { return detail::gk15i<T>(f, boun, inf, pa, pb); }, T{0}, T{1}, epsabs, epsrel, epp);
}

// Convenience overload — allocates the workspace once per call.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_qagi(crd::memory::IAllocator* alloc, F&& f, T bound, int inf, T epsabs, T epsrel,
                                           int limit = 50)
{
    AdaptiveWorkspace<T> ws(alloc, limit);
    return integrate_qagi<T>(ws, static_cast<F&&>(f), bound, inf, epsabs, epsrel);
}

// QAGP — adaptive integration over [a,b] with KNOWN singularity/discontinuity locations `points` (each in (a,b)).
// Splits at the (sorted) break-points and runs QAGS on each sub-interval, so every break-point becomes a panel
// ENDPOINT where the Wynn-ε extrapolation resolves the local singularity; the pieces are summed (the abs tolerance is
// split across them). Up to 32 break-points.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_qagp(AdaptiveWorkspace<T>& ws, F&& f, T a, T b,
                                           crd::containers::ConstSpan<T> points, T epsabs, T epsrel)
{
    if (!detail::quad_finite(a) || !detail::quad_finite(b))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    const T sign = (a <= b) ? T{1} : T{-1};
    const T lo   = a < b ? a : b;
    const T hi   = a < b ? b : a;
    T       pts[34];
    int     n  = 0;
    pts[n++]   = lo;
    for (crd::usize i = 0; i < points.size() && n < 33; ++i)
    {
        if (detail::quad_finite(points[i]) && points[i] > lo && points[i] < hi)
        {
            pts[n++] = points[i];
        }
    }
    pts[n++] = hi;
    for (int i = 1; i < n; ++i) // insertion-sort the break-points
    {
        const T v = pts[i];
        int     j = i - 1;
        while (j >= 0 && pts[j] > v)
        {
            pts[j + 1] = pts[j];
            --j;
        }
        pts[j + 1] = v;
    }
    const int     npieces   = n - 1;
    const T       eps_piece = epsabs / static_cast<T>(npieces);
    QuadResult<T> total;
    total.status        = QuadStatus::Ok;
    total.tolerance_met = true;
    for (int i = 0; i < npieces; ++i)
    {
        const QuadResult<T> r = integrate_qags<T>(ws, f, pts[i], pts[i + 1], eps_piece, epsrel);
        total.value += r.value;
        total.error_estimate += r.error_estimate;
        total.eval_count += r.eval_count;
        total.subdiv_count += r.subdiv_count;
        if (!r.tolerance_met)
        {
            total.tolerance_met = false;
        }
        if (r.status != QuadStatus::Ok && total.status == QuadStatus::Ok)
        {
            total.status = r.status;
        }
    }
    total.value *= sign;
    if (!total.tolerance_met && total.status == QuadStatus::Ok)
    {
        total.status = QuadStatus::MaxSubdivisions;
    }
    return total;
}

// Convenience overload — allocates the workspace once per call.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_qagp(crd::memory::IAllocator* alloc, F&& f, T a, T b,
                                           crd::containers::ConstSpan<T> points, T epsabs, T epsrel, int limit = 50)
{
    AdaptiveWorkspace<T> ws(alloc, limit);
    return integrate_qagp<T>(ws, static_cast<F&&>(f), a, b, points, epsabs, epsrel);
}

} // namespace crd::hesap::quadrature
