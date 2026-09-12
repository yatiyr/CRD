#pragma once

// crd-hesap-quadrature v13-h — the adaptive integrator (QUADPACK QAG): globally-adaptive bisection driven by the
// Gauss-Kronrod local error estimate, via an ITERATIVE bounded-depth WORK-STACK — NOT recursion. This is the
// certification differentiator (ADR-0095 pillar 2): GSL's qag recurses (unbounded stack ⇒ MISRA 17.2 / WCET
// violation); ours maintains an explicit subinterval list of fixed capacity = max_subiv (the hard WCET knob). Each
// iteration bisects the subinterval with the LARGEST error estimate and re-evaluates GK21 on the two halves, until
// the summed error ≤ max(epsabs, epsrel·|result|) or the subdivision budget is exhausted (→ status MaxSubdivisions,
// not a spin or a trap).
//
// ⚠ HONESTY (pillar 3): error_estimate is the summed GK Tier-1 ESTIMATE, never a guaranteed bound — a narrow peak
// hidden between the Kronrod nodes fools it (the Lyness-Kaganove failure). The result carries it as an estimate; the
// caller must not treat it as an enclosure.

#include <crd/hesap/quadrature/gauss_kronrod.hpp>
#include <crd/hesap/quadrature/integrate.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::quadrature
{

// Globally-adaptive integral of f over [a,b] to absolute tolerance epsabs OR relative epsrel, using GK21 panels and
// at most max_subiv subintervals. Allocates the work-stack ONCE (size max_subiv); the panel evaluations allocate
// nothing. f: callable T→T.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_adaptive(crd::memory::IAllocator* alloc, F&& f, T a, T b, T epsabs, T epsrel,
                                               int max_subiv = 50)
{
    if (max_subiv < 1 || !detail::quad_finite(a) || !detail::quad_finite(b)
        || (epsabs <= T{0} && epsrel <= T{0}))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }

    const crd::usize          cap = static_cast<crd::usize>(max_subiv);
    crd::containers::Array<T>  alist(alloc);
    crd::containers::Array<T>  blist(alloc);
    crd::containers::Array<T>  rlist(alloc);
    crd::containers::Array<T>  elist(alloc);
    alist.resize(cap);
    blist.resize(cap);
    rlist.resize(cap);
    elist.resize(cap);

    const GkResult<T> g0 = gauss_kronrod_21<T>(std::forward<F>(f), a, b);
    alist[0]      = a;
    blist[0]      = b;
    rlist[0]      = g0.value;
    elist[0]      = g0.abserr;
    T        result = g0.value;
    T        errsum = g0.abserr;
    crd::u32 nsub   = 1;
    crd::u32 neval  = 21;
    T        errbnd = detail::qmax<T>(epsabs, epsrel * crd::math::fabs(result));

    while (nsub < static_cast<crd::u32>(max_subiv) && errsum > errbnd)
    {
        // bisect the subinterval with the largest local error estimate (deterministic: first max wins ties).
        crd::u32 imax = 0;
        T        emax = elist[0];
        for (crd::u32 i = 1; i < nsub; ++i)
        {
            if (elist[i] > emax)
            {
                emax = elist[i];
                imax = i;
            }
        }
        const T a1 = alist[imax];
        const T b1 = (alist[imax] + blist[imax]) * static_cast<T>(0.5);
        const T b2 = blist[imax];
        const GkResult<T> g1 = gauss_kronrod_21<T>(f, a1, b1);
        const GkResult<T> g2 = gauss_kronrod_21<T>(f, b1, b2);
        neval += 42;
        result += (g1.value + g2.value) - rlist[imax];
        errsum += (g1.abserr + g2.abserr) - elist[imax];
        alist[imax] = a1;
        blist[imax] = b1;
        rlist[imax] = g1.value;
        elist[imax] = g1.abserr;
        alist[nsub] = b1;
        blist[nsub] = b2;
        rlist[nsub] = g2.value;
        elist[nsub] = g2.abserr;
        ++nsub;
        errbnd = detail::qmax<T>(epsabs, epsrel * crd::math::fabs(result));
    }

    QuadResult<T> r;
    r.value          = result;
    r.error_estimate = errsum;
    r.subdiv_count   = nsub;
    r.eval_count     = neval;
    r.tolerance_met  = errsum <= errbnd;
    r.status         = r.tolerance_met ? QuadStatus::Ok : QuadStatus::MaxSubdivisions;
    return r;
}

} // namespace crd::hesap::quadrature
