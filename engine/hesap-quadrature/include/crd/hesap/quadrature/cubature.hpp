#pragma once

// crd-hesap-quadrature v13-k — MULTI-DIMENSIONAL cubature (integration over boxes/spheres/simplices):
//   tensor-product Gauss      — ∫_[a,b]^d f, the dense d≲4 baseline (reuses gauss_legendre).
//   ★Genz-Malik adaptive      — degree-7 fully-symmetric rule + embedded degree-5 error, globally-adaptive box
//                               subdivision (split the worst box along its largest 4th-difference axis), d≈2–7.
//   (★Lebedev sphere + ★Smolyak sparse grid + Dunavant simplex land in lebedev.hpp / smolyak.hpp / simplex.hpp.)
//
// The Genz-Malik rule + adaptive driver are reconstructed-and-verified bit-exact in python vs scipy's
// GenzMalikCubature (the rule) and scipy.integrate.cubature (the adaptive result) BEFORE this port. Peers:
// scipy.integrate.cubature/nquad (no clean MATLAB/Boost/GSL multi-D-adaptive peer — stated).
//
// Moat (ADR-0095): determinism (crd::math, fixed FP order, deterministic worst-box selection) + allocation-free
// bounded-depth adaptation (the region work-stack is sized once to max_subdiv = the WCET knob, NOT recursion) +
// the error-tier QuadResult (the |higher−lower| Tier-1 estimate, never a bound).

#include <crd/hesap/quadrature/gauss.hpp>
#include <crd/hesap/quadrature/integrate.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::quadrature
{

constexpr int kCubMaxDim = 12; // max spatial dimension for the box cubature (2^d corner cost bounds practical use)

namespace detail
{

// Evaluate the Genz-Malik degree-7 rule + embedded degree-5 rule on the box [a,b]^d. Returns the degree-7 value `ih`,
// the error estimate |ih−il|, and per-axis 4th-difference split measures `diffs`. Allocation-free (stack scratch).
template <typename T, typename F>
void gm_estimate(F&& f, const T* a, const T* b, int d, T& ih, T& err, T* diffs, crd::u32& neval)
{
    const T l2 = crd::math::sqrt(static_cast<T>(9) / static_cast<T>(70));
    const T l3 = crd::math::sqrt(static_cast<T>(9) / static_cast<T>(10));
    const T l4 = l3;
    const T l5 = crd::math::sqrt(static_cast<T>(9) / static_cast<T>(19));
    const T dd = static_cast<T>(d);
    const T two_d = crd::math::pow(static_cast<T>(2), dd);
    // weights (degree 7, the 2^d factor cancels the vol/2^d below — kept as scipy defines for bit-agreement)
    const T w1h = two_d * (static_cast<T>(12824) - static_cast<T>(9120) * dd + static_cast<T>(400) * dd * dd)
                  / static_cast<T>(19683);
    const T w2h = two_d * static_cast<T>(980) / static_cast<T>(6561);
    const T w3h = two_d * (static_cast<T>(1820) - static_cast<T>(400) * dd) / static_cast<T>(19683);
    const T w4h = two_d * static_cast<T>(200) / static_cast<T>(19683);
    const T w5h = static_cast<T>(6859) / static_cast<T>(19683);
    const T w1l = two_d * (static_cast<T>(729) - static_cast<T>(950) * dd + static_cast<T>(50) * dd * dd)
                  / static_cast<T>(729);
    const T w2l = two_d * static_cast<T>(245) / static_cast<T>(486);
    const T w3l = two_d * (static_cast<T>(265) - static_cast<T>(100) * dd) / static_cast<T>(1458);
    const T w4l = two_d * static_cast<T>(25) / static_cast<T>(729);

    T mid[kCubMaxDim];
    T half[kCubMaxDim];
    T pt[kCubMaxDim];
    T vol = T{1};
    for (int i = 0; i < d; ++i)
    {
        mid[i]  = (a[i] + b[i]) / T{2};
        half[i] = (b[i] - a[i]) / T{2};
        vol *= (b[i] - a[i]);
        pt[i] = mid[i];
    }
    const T fc = f(pt);
    T       sh = w1h * fc;
    T       sl = w1l * fc;
    crd::u32 ne = 1;
    const T ratio = (static_cast<T>(9) / static_cast<T>(70)) / (static_cast<T>(9) / static_cast<T>(10)); // l2²/l3²
    for (int i = 0; i < d; ++i)
    {
        pt[i]      = mid[i] + half[i] * l2;
        const T f2p = f(pt);
        pt[i]      = mid[i] - half[i] * l2;
        const T f2m = f(pt);
        pt[i]      = mid[i] + half[i] * l3;
        const T f3p = f(pt);
        pt[i]      = mid[i] - half[i] * l3;
        const T f3m = f(pt);
        pt[i]      = mid[i];
        ne += 4;
        sh += w2h * (f2p + f2m) + w3h * (f3p + f3m);
        sl += w2l * (f2p + f2m) + w3l * (f3p + f3m);
        diffs[i] = crd::math::fabs((f3p + f3m - T{2} * fc) - ratio * (f2p + f2m - T{2} * fc));
    }
    for (int i = 0; i < d; ++i) // l4 diagonal points, 4 per unordered axis pair
    {
        for (int j = i + 1; j < d; ++j)
        {
            for (int s = 0; s < 4; ++s)
            {
                pt[i]      = mid[i] + ((s & 1) ? -half[i] * l4 : half[i] * l4);
                pt[j]      = mid[j] + ((s & 2) ? -half[j] * l4 : half[j] * l4);
                const T f4 = f(pt);
                sh += w4h * f4;
                sl += w4l * f4;
                ++ne;
            }
            pt[i] = mid[i];
            pt[j] = mid[j];
        }
    }
    const int corners = 1 << d; // ±l5 in every axis (degree-7 only)
    for (int k = 0; k < corners; ++k)
    {
        for (int i = 0; i < d; ++i)
        {
            pt[i] = mid[i] + ((k >> i) & 1 ? -half[i] * l5 : half[i] * l5);
        }
        sh += w5h * f(pt);
        ++ne;
    }
    for (int i = 0; i < d; ++i)
    {
        pt[i] = mid[i];
    }
    const T scale = vol / two_d;
    ih            = sh * scale;
    const T il    = sl * scale;
    err           = crd::math::fabs(ih - il);
    neval         = ne;
}

// gm_estimate on region slot `slot` (reads ra/rb[slot*d..], writes rval/rerr[slot] + rdiffs[slot*d..]).
template <typename T, typename F>
void gm_estimate_impl(F&& f, crd::u32 slot, int d, const crd::containers::Array<T>& ra,
                      const crd::containers::Array<T>& rb, crd::containers::Array<T>& rval,
                      crd::containers::Array<T>& rerr, crd::containers::Array<T>& rdiffs, crd::u32& neval)
{
    const crd::usize off = static_cast<crd::usize>(slot) * static_cast<crd::usize>(d);
    T ih, err;
    T diffs[kCubMaxDim];
    gm_estimate<T>(std::forward<F>(f), ra.data() + off, rb.data() + off, d, ih, err, diffs, neval);
    rval[slot] = ih;
    rerr[slot] = err;
    for (int i = 0; i < d; ++i)
    {
        rdiffs[off + static_cast<crd::usize>(i)] = diffs[i];
    }
}
} // namespace detail

// Tensor-product Gauss-Legendre over the box [a,b]^d: n_per_dim points per axis ⇒ n^d evaluations. Exact for
// per-axis polynomial degree ≤ 2·n_per_dim − 1. Best for low d (≲4) and smooth integrands. f: callable (const T*)→T.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_tensor_gauss(crd::memory::IAllocator* alloc, F&& f, crd::containers::ConstSpan<T> a,
                                                   crd::containers::ConstSpan<T> b, int n_per_dim)
{
    const int d = static_cast<int>(a.size());
    if (d < 1 || d > kCubMaxDim || b.size() != a.size() || n_per_dim < 1)
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> w(alloc);
    x.resize(static_cast<crd::usize>(n_per_dim));
    w.resize(static_cast<crd::usize>(n_per_dim));
    gauss_legendre<T>(alloc, n_per_dim, x.data(), w.data());
    T mid[kCubMaxDim];
    T half[kCubMaxDim];
    T vol = T{1};
    for (int i = 0; i < d; ++i)
    {
        mid[i]  = (a[i] + b[i]) / T{2};
        half[i] = (b[i] - a[i]) / T{2};
        vol *= half[i];
    }
    int      idx[kCubMaxDim] = {};
    T        pt[kCubMaxDim];
    T        acc   = T{0};
    crd::u64 total = 1;
    for (int i = 0; i < d; ++i)
    {
        total *= static_cast<crd::u64>(n_per_dim);
    }
    for (crd::u64 c = 0; c < total; ++c)
    {
        T wprod = T{1};
        for (int i = 0; i < d; ++i)
        {
            pt[i] = mid[i] + half[i] * x[static_cast<crd::usize>(idx[i])];
            wprod *= w[static_cast<crd::usize>(idx[i])];
        }
        acc += wprod * f(pt);
        for (int i = 0; i < d; ++i) // increment the mixed-radix counter
        {
            if (++idx[i] < n_per_dim)
            {
                break;
            }
            idx[i] = 0;
        }
    }
    QuadResult<T> r;
    r.value      = acc * vol;
    r.eval_count = static_cast<crd::u32>(total);
    return r;
}

// Genz-Malik globally-adaptive cubature over the box [a,b]^d (d ≥ 2). Subdivides the worst box along its largest
// 4th-difference axis until the summed error ≤ max(epsabs, epsrel·|result|) or the box budget max_subdiv is hit
// (→ MaxSubdivisions). The region list is the bounded work-stack (allocated once = the WCET knob; NOT recursion).
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_cubature(crd::memory::IAllocator* alloc, F&& f, crd::containers::ConstSpan<T> a,
                                               crd::containers::ConstSpan<T> b, T epsabs, T epsrel, int max_subdiv = 1000)
{
    const int d = static_cast<int>(a.size());
    if (d < 2 || d > kCubMaxDim || b.size() != a.size() || max_subdiv < 1
        || (epsabs <= T{0} && epsrel <= T{0}))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    const crd::usize cap = static_cast<crd::usize>(max_subdiv);
    const crd::usize dd  = static_cast<crd::usize>(d);
    crd::containers::Array<T> ra(alloc), rb(alloc), rdiffs(alloc), rval(alloc), rerr(alloc);
    ra.resize(cap * dd);
    rb.resize(cap * dd);
    rdiffs.resize(cap * dd);
    rval.resize(cap);
    rerr.resize(cap);

    for (int i = 0; i < d; ++i)
    {
        ra[static_cast<crd::usize>(i)] = a[static_cast<crd::usize>(i)];
        rb[static_cast<crd::usize>(i)] = b[static_cast<crd::usize>(i)];
    }
    crd::u32 ne = 0;
    detail::gm_estimate_impl(f, 0u, d, ra, rb, rval, rerr, rdiffs, ne);
    crd::u32 nreg  = 1;
    crd::u32 neval = ne;
    T        total = rval[0];
    T        toterr = rerr[0];
    T        errbnd = detail::qmax<T>(epsabs, epsrel * crd::math::fabs(total));

    while (toterr > errbnd && nreg < static_cast<crd::u32>(max_subdiv))
    {
        // worst region (largest error; first max wins — deterministic)
        crd::u32 wi   = 0;
        T        emax = rerr[0];
        for (crd::u32 r = 1; r < nreg; ++r)
        {
            if (rerr[r] > emax)
            {
                emax = rerr[r];
                wi   = r;
            }
        }
        // split axis = largest 4th-difference
        int axis  = 0;
        T   dmax  = rdiffs[static_cast<crd::usize>(wi) * dd];
        for (int i = 1; i < d; ++i)
        {
            const T v = rdiffs[static_cast<crd::usize>(wi) * dd + static_cast<crd::usize>(i)];
            if (v > dmax)
            {
                dmax = v;
                axis = i;
            }
        }
        const crd::usize wb   = static_cast<crd::usize>(wi) * dd;
        const T          amid = (ra[wb + static_cast<crd::usize>(axis)] + rb[wb + static_cast<crd::usize>(axis)]) / T{2};
        const crd::usize nb   = static_cast<crd::usize>(nreg) * dd; // new region slot
        // child 2 = [amid, hi] in `axis`; child 1 overwrites the parent as [lo, amid].
        for (int i = 0; i < d; ++i)
        {
            ra[nb + static_cast<crd::usize>(i)] = ra[wb + static_cast<crd::usize>(i)];
            rb[nb + static_cast<crd::usize>(i)] = rb[wb + static_cast<crd::usize>(i)];
        }
        ra[nb + static_cast<crd::usize>(axis)] = amid;            // child 2 lower bound
        rb[wb + static_cast<crd::usize>(axis)] = amid;            // child 1 upper bound
        total -= rval[wi];
        toterr -= rerr[wi];
        crd::u32 ne1 = 0, ne2 = 0;
        detail::gm_estimate_impl(f, wi, d, ra, rb, rval, rerr, rdiffs, ne1);   // child 1 in slot wi
        detail::gm_estimate_impl(f, nreg, d, ra, rb, rval, rerr, rdiffs, ne2); // child 2 in slot nreg
        total += rval[wi] + rval[nreg];
        toterr += rerr[wi] + rerr[nreg];
        neval += ne1 + ne2;
        ++nreg;
        errbnd = detail::qmax<T>(epsabs, epsrel * crd::math::fabs(total));
    }

    QuadResult<T> r;
    r.value          = total;
    r.error_estimate = toterr;
    r.eval_count     = neval;
    r.subdiv_count   = nreg;
    r.tolerance_met  = toterr <= errbnd;
    r.status         = r.tolerance_met ? QuadStatus::Ok : QuadStatus::MaxSubdivisions;
    return r;
}

} // namespace crd::hesap::quadrature
