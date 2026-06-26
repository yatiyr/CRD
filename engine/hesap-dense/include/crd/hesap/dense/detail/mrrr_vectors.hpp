#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/detail/dqds.hpp>  // Z1
#include <crd/hesap/dense/detail/sturm_count.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3a-3.2 — MRRR eigenvector kernel (dlar1v: twisted factorization).
//
// Given a relatively-robust representation  L D L^T  (D = pivots, L = unit
// lower bidiagonal subdiagonals, LD[i]=L[i]*D[i], LLD[i]=L[i]^2*D[i]) of a
// shifted tridiagonal block [b1..bn], and a shift LAMBDA (an eigenvalue
// relative to the RRR), dlar1v computes the corresponding eigenvector z by the
// TWISTED factorization: a stationary differential transform (dstqds) from b1
// and a progressive one (dqds) from bn meet at the twist index r = argmin of
// |s[i]+p[i]| (minimal |gamma| = best-conditioned pivot); z solves N_r^T z = e_r,
// built outward from r. Orthogonality of well-separated eigenvectors is
// guaranteed by construction (no Gram-Schmidt) — the MRRR O(n^2) win.
//
// Faithful IEEE port of dlar1v.f (1-based Z1 wrappers keep the index arithmetic
// identical; D(dense-eig)-MRRR-Z1base). The slower pivmin-guarded loops are the
// faithful NaN fallback (the fast path can produce NaN at an exact eigenvalue).
// Lower layer: raw f32/f64 (ADR-0078).
//
// work is 1-based length >= 4n: lplus @ [1..n], uminus @ [n+1..2n],
// s @ [2n+1..3n], p @ [3n+1..4n]  (the dlar1v INDLPL/INDUMN/INDS/INDP layout).
// -----------------------------------------------------------------------

// =======================================================================
// dlaneg — Sturm count (number of negative pivots of L D L^T - sigma I, which
// equals the number of eigenvalues of L D L^T below sigma) via a TWISTED
// factorization meeting at twist index r. Works directly on the factors
// (D = pivots length n, LLD = L^2*D length n-1) without forming the matrix.
// Faithful port of dlaneg.f (IEEE Inf/NaN propagation + BLKLEN=128 chunked
// NaN re-detection + pivmin guard in the slow path). Used by dlarrb to refine
// cluster eigenvalues relative to an RRR. Lower layer: raw f32/f64.
// =======================================================================
template <typename R>
[[nodiscard]] inline int dlaneg(int n, const R* d, const R* lld, R sigma, R pivmin, int r) noexcept
{
    constexpr int kBlkLen = 128;
    int negcnt = 0;

    // I) upper part: L D L^T - sigma I = L+ D+ L+^T.
    R t = -sigma;
    for (int bj = 1; bj <= r - 1; bj += kBlkLen)
    {
        int neg1 = 0;
        const R bsav = t;
        const int last = std::min(bj + kBlkLen - 1, r - 1);
        for (int j = bj; j <= last; ++j)
        {
            const R dplus = d[j - 1] + t;
            if (dplus < R{0})
            {
                ++neg1;
            }
            const R tmp = t / dplus;
            t = tmp * lld[j - 1] - sigma;
        }
        if (std::isnan(t))
        {
            neg1 = 0;
            t = bsav;
            for (int j = bj; j <= last; ++j)
            {
                const R dplus = d[j - 1] + t;
                if (dplus < R{0})
                {
                    ++neg1;
                }
                R tmp = t / dplus;
                if (std::isnan(tmp))
                {
                    tmp = R{1};
                }
                t = tmp * lld[j - 1] - sigma;
            }
        }
        negcnt += neg1;
    }

    // II) lower part: L D L^T - sigma I = U- D- U-^T.
    R p = d[n - 1] - sigma;
    for (int bj = n - 1; bj >= r; bj -= kBlkLen)
    {
        int neg2 = 0;
        const R bsav = p;
        const int last = std::max(bj - kBlkLen + 1, r);
        for (int j = bj; j >= last; --j)
        {
            const R dminus = lld[j - 1] + p;
            if (dminus < R{0})
            {
                ++neg2;
            }
            const R tmp = p / dminus;
            p = tmp * d[j - 1] - sigma;
        }
        if (std::isnan(p))
        {
            neg2 = 0;
            p = bsav;
            for (int j = bj; j >= last; --j)
            {
                const R dminus = lld[j - 1] + p;
                if (dminus < R{0})
                {
                    ++neg2;
                }
                R tmp = p / dminus;
                if (std::isnan(tmp))
                {
                    tmp = R{1};
                }
                p = tmp * d[j - 1] - sigma;
            }
        }
        negcnt += neg2;
    }

    // III) twist index.
    const R gamma = (t + sigma) + p;
    if (gamma < R{0})
    {
        ++negcnt;
    }
    (void)pivmin;  // IEEE path uses Inf/NaN propagation (dlaneg.f comment)
    return negcnt;
}

// =======================================================================
// dlarrb_refine — refine the eigenvalues w[ifirst-1 .. ilast-1] (0-based array,
// 1-based indices; relative to the RRR) WITHIN an RRR (D length n, LLD length
// n-1) by dlaneg bisection to relative tol. w/werr updated in place. twist =
// the twist index (<1 or >n -> n). Needed in the cluster loop so dlar1v gets a
// relatively-accurate eigenvalue (the orthogonality prerequisite).
//
// D(dense-eig)-MRRR-dlarrb-per-eigenvalue: faithful to dlarrb.f's math but with
// a per-eigenvalue bisection instead of the linked-list multisection — same
// converged result, simpler control flow (the sharing is a speed-only opt;
// cluster sizes are small so it is invisible). Deterministic (fixed itmax +
// pivmin Sturm guard inside dlaneg).
// =======================================================================
template <typename R>
inline void dlarrb_refine(int n, const R* d, const R* lld, R* w, R* werr, int ifirst, int ilast, R rtol1, R rtol2,
                          R pivmin, R spdiam, int twist) noexcept
{
    const int r = (twist < 1 || twist > n) ? n : twist;
    const R mnwdth = R{2} * pivmin;
    int maxitr = 2;
    {
        const R ratio = (spdiam + pivmin) / pivmin;
        R pw = R{1};
        while (pw < ratio && maxitr < 2000)
        {
            pw *= R{2};
            ++maxitr;
        }
    }

    for (int i = ifirst; i <= ilast; ++i)
    {
        R left = w[i - 1] - werr[i - 1];
        R right = w[i - 1] + werr[i - 1];

        // Widen so dlaneg(left) <= i-1 and dlaneg(right) >= i (bracket eig i).
        R back = werr[i - 1];
        while (dlaneg(n, d, lld, left, pivmin, r) > i - 1)
        {
            left -= back;
            back *= R{2};
        }
        back = werr[i - 1];
        while (dlaneg(n, d, lld, right, pivmin, r) < i)
        {
            right += back;
            back *= R{2};
        }

        // Relative gap to neighbours (for the rtol1*gap convergence term).
        R gap = std::numeric_limits<R>::max();
        if (i > ifirst)
        {
            gap = std::min(gap, w[i - 1] - w[i - 2]);
        }
        if (i < ilast)
        {
            gap = std::min(gap, w[i] - w[i - 1]);
        }
        if (gap == std::numeric_limits<R>::max())
        {
            gap = std::abs(w[i - 1]);
        }

        for (int iter = 0; iter < maxitr; ++iter)
        {
            const R width = R{0.5} * std::abs(right - left);
            const R mag = std::max(std::abs(left), std::abs(right));
            const R cvrgd = std::max(rtol1 * gap, rtol2 * mag);
            if (width <= cvrgd || width <= mnwdth)
            {
                break;
            }
            const R mid = R{0.5} * (left + right);
            if (dlaneg(n, d, lld, mid, pivmin, r) <= i - 1)
            {
                left = mid;
            }
            else
            {
                right = mid;
            }
        }
        w[i - 1] = R{0.5} * (left + right);
        werr[i - 1] = R{0.5} * std::abs(right - left);
    }
}

template <typename R>
struct Lar1vOut
{
    int negcnt;   // negative-pivot count (if wantnc), else -1
    int r;        // chosen twist index (1-based)
    R mingma;     // the minimal |s+p| (1/largest diagonal of the inverse)
    R nrminv;     // 1 / ||z||_2
    R resid;      // residual |mingma| * nrminv
    R rqcorr;     // Rayleigh-quotient correction mingma / ztz
    R ztz;        // z^T z (before normalization)
    int isuppz0;  // support start (1-based)
    int isuppz1;  // support end   (1-based)
};

template <typename R>
inline Lar1vOut<R> dlar1v(int n, int b1, int bn, R lambda, Z1<R> d, Z1<R> l, Z1<R> ld, Z1<R> lld, R pivmin, R gaptol,
                          Z1<R> z, int r_in, bool wantnc, Z1<R> work) noexcept
{
    const R eps = std::numeric_limits<R>::epsilon();

    int r1;
    int r2;
    if (r_in == 0)
    {
        r1 = b1;
        r2 = bn;
    }
    else
    {
        r1 = r_in;
        r2 = r_in;
    }

    const int indlpl = 0;
    const int indumn = n;
    const int inds = 2 * n + 1;
    const int indp = 3 * n + 1;

    if (b1 == 1)
    {
        work[inds] = R{0};
    }
    else
    {
        work[inds + b1 - 1] = lld[b1 - 1];
    }

    // Stationary transform (differential form) up to index r2.
    bool sawnan1 = false;
    int neg1 = 0;
    R s = work[inds + b1 - 1] - lambda;
    for (int i = b1; i <= r1 - 1; ++i)
    {
        const R dplus = d[i] + s;
        work[indlpl + i] = ld[i] / dplus;
        if (dplus < R{0})
        {
            ++neg1;
        }
        work[inds + i] = s * work[indlpl + i] * l[i];
        s = work[inds + i] - lambda;
    }
    sawnan1 = std::isnan(s);
    if (!sawnan1)
    {
        for (int i = r1; i <= r2 - 1; ++i)
        {
            const R dplus = d[i] + s;
            work[indlpl + i] = ld[i] / dplus;
            work[inds + i] = s * work[indlpl + i] * l[i];
            s = work[inds + i] - lambda;
        }
        sawnan1 = std::isnan(s);
    }
    if (sawnan1)
    {
        // Slower pivmin-guarded version (NaN fallback).
        neg1 = 0;
        s = work[inds + b1 - 1] - lambda;
        for (int i = b1; i <= r1 - 1; ++i)
        {
            R dplus = d[i] + s;
            if (std::abs(dplus) < pivmin)
            {
                dplus = -pivmin;
            }
            work[indlpl + i] = ld[i] / dplus;
            if (dplus < R{0})
            {
                ++neg1;
            }
            work[inds + i] = s * work[indlpl + i] * l[i];
            if (work[indlpl + i] == R{0})
            {
                work[inds + i] = lld[i];
            }
            s = work[inds + i] - lambda;
        }
        for (int i = r1; i <= r2 - 1; ++i)
        {
            R dplus = d[i] + s;
            if (std::abs(dplus) < pivmin)
            {
                dplus = -pivmin;
            }
            work[indlpl + i] = ld[i] / dplus;
            work[inds + i] = s * work[indlpl + i] * l[i];
            if (work[indlpl + i] == R{0})
            {
                work[inds + i] = lld[i];
            }
            s = work[inds + i] - lambda;
        }
    }

    // Progressive transform (differential form) down to index r1.
    bool sawnan2 = false;
    int neg2 = 0;
    work[indp + bn - 1] = d[bn] - lambda;
    for (int i = bn - 1; i >= r1; --i)
    {
        const R dminus = lld[i] + work[indp + i];
        const R tmp = d[i] / dminus;
        if (dminus < R{0})
        {
            ++neg2;
        }
        work[indumn + i] = l[i] * tmp;
        work[indp + i - 1] = work[indp + i] * tmp - lambda;
    }
    sawnan2 = std::isnan(work[indp + r1 - 1]);
    if (sawnan2)
    {
        neg2 = 0;
        for (int i = bn - 1; i >= r1; --i)
        {
            R dminus = lld[i] + work[indp + i];
            if (std::abs(dminus) < pivmin)
            {
                dminus = -pivmin;
            }
            const R tmp = d[i] / dminus;
            if (dminus < R{0})
            {
                ++neg2;
            }
            work[indumn + i] = l[i] * tmp;
            work[indp + i - 1] = work[indp + i] * tmp - lambda;
            if (tmp == R{0})
            {
                work[indp + i - 1] = d[i] - lambda;
            }
        }
    }

    // Find the twist index r in [r1,r2]: argmin |s[i]+p[i]| (best pivot).
    R mingma = work[inds + r1 - 1] + work[indp + r1 - 1];
    if (mingma < R{0})
    {
        ++neg1;
    }
    const int negcnt = wantnc ? (neg1 + neg2) : -1;
    if (std::abs(mingma) == R{0})
    {
        mingma = eps * work[inds + r1 - 1];
    }
    int r = r1;
    for (int i = r1; i <= r2 - 1; ++i)
    {
        R tmp = work[inds + i] + work[indp + i];
        if (tmp == R{0})
        {
            tmp = eps * work[inds + i];
        }
        if (std::abs(tmp) <= std::abs(mingma))
        {
            mingma = tmp;
            r = i + 1;
        }
    }

    // Solve N_r^T z = e_r, built outward from r.
    int isuppz0 = b1;
    int isuppz1 = bn;
    z[r] = R{1};
    R ztz = R{1};

    // Upward from r.
    if (!sawnan1 && !sawnan2)
    {
        for (int i = r - 1; i >= b1; --i)
        {
            z[i] = -(work[indlpl + i] * z[i + 1]);
            if ((std::abs(z[i]) + std::abs(z[i + 1])) * std::abs(ld[i]) < gaptol)
            {
                z[i] = R{0};
                isuppz0 = i + 1;
                break;
            }
            ztz = ztz + z[i] * z[i];
        }
    }
    else
    {
        for (int i = r - 1; i >= b1; --i)
        {
            if (z[i + 1] == R{0})
            {
                z[i] = -(ld[i + 1] / ld[i]) * z[i + 2];
            }
            else
            {
                z[i] = -(work[indlpl + i] * z[i + 1]);
            }
            if ((std::abs(z[i]) + std::abs(z[i + 1])) * std::abs(ld[i]) < gaptol)
            {
                z[i] = R{0};
                isuppz0 = i + 1;
                break;
            }
            ztz = ztz + z[i] * z[i];
        }
    }

    // Downward from r.
    if (!sawnan1 && !sawnan2)
    {
        for (int i = r; i <= bn - 1; ++i)
        {
            z[i + 1] = -(work[indumn + i] * z[i]);
            if ((std::abs(z[i]) + std::abs(z[i + 1])) * std::abs(ld[i]) < gaptol)
            {
                z[i + 1] = R{0};
                isuppz1 = i;
                break;
            }
            ztz = ztz + z[i + 1] * z[i + 1];
        }
    }
    else
    {
        for (int i = r; i <= bn - 1; ++i)
        {
            if (z[i] == R{0})
            {
                z[i + 1] = -(ld[i - 1] / ld[i]) * z[i - 1];
            }
            else
            {
                z[i + 1] = -(work[indumn + i] * z[i]);
            }
            if ((std::abs(z[i]) + std::abs(z[i + 1])) * std::abs(ld[i]) < gaptol)
            {
                z[i + 1] = R{0};
                isuppz1 = i;
                break;
            }
            ztz = ztz + z[i + 1] * z[i + 1];
        }
    }

    const R tinv = R{1} / ztz;
    const R nrminv = crd::math::sqrt(tinv);
    const R resid = std::abs(mingma) * nrminv;
    const R rqcorr = mingma * tinv;

    return Lar1vOut<R>{negcnt, r, mingma, nrminv, resid, rqcorr, ztz, isuppz0, isuppz1};
}

// =======================================================================
// dlarrf — form a CHILD RRR for a cluster: find a shift sigma near one end of
// the cluster such that (parent L D L^T) - sigma I = L+ D+ L+^T has bounded
// element growth (=> relatively robust), so the cluster eigenvalues become
// relatively well-separated in the child. Faithful port of dlarrf.f: try left
// (lsigma) and right (rsigma) cluster-end shifts via dstqds, accept if element
// growth <= MAXGROWTH1*spdiam; else a refined-RRR test for isolated clusters;
// else back off (KTRYMAX) and finally force the best shift found.
//
// 0-based: d/l/ld/lld parent factors (length n / n-1); clstrt/clend 0-based
// cluster index bounds into w/werr/wgap; outputs sigma + child dplus (n) /
// lplus (n). work length >= 2n. Returns 0 (ok) / 1 (forced/failed RRR).
// =======================================================================
template <typename R>
inline int dlarrf(int n, const R* d, const R* l, const R* ld, const R* lld, int clstrt, int clend, const R* w,
                  const R* wgap, const R* werr, R spdiam, R clgapl, R clgapr, R pivmin, R& sigma, R* dplus,
                  R* lplus, R* work) noexcept
{
    if (n <= 0)
    {
        return 0;
    }
    (void)lld;  // the dstqds factorization uses d/l/ld; LLD is vestigial in dlarrf's signature
    const R eps = std::numeric_limits<R>::epsilon();
    const R safmin = std::numeric_limits<R>::min();
    constexpr int kKtryMax = 1;
    constexpr int kSLeft = 1;
    constexpr int kSRight = 2;
    const R maxgrowth1 = R{8};
    const R maxgrowth2 = R{8};
    const R quart = R{0.25};
    const R four = R{4};
    const R two = R{2};
    const R one = R{1};
    const R fact = R{2};  // 2^KTRYMAX

    int shift = 0;
    bool forcer = false;
    const bool nofail = false;

    const R clwdth = std::abs(w[clend] - w[clstrt]) + werr[clend] + werr[clstrt];
    const R avgap = clwdth / static_cast<R>(clend - clstrt);
    const R mingap = std::min(clgapl, clgapr);
    R lsigma = std::min(w[clstrt], w[clend]) - werr[clstrt];
    R rsigma = std::max(w[clstrt], w[clend]) + werr[clend];
    lsigma -= std::abs(lsigma) * four * eps;
    rsigma += std::abs(rsigma) * four * eps;
    const R ldmax = quart * mingap + two * pivmin;
    const R rdmax = quart * mingap + two * pivmin;
    R ldelta = std::max(avgap, wgap[clstrt]) / fact;
    R rdelta = std::max(avgap, wgap[clend - 1]) / fact;

    R smlgrowth = one / safmin;
    const R fail = static_cast<R>(n - 1) * mingap / (spdiam * eps);
    const R fail2 = static_cast<R>(n - 1) * mingap / (spdiam * crd::math::sqrt(eps));
    R bestshift = lsigma;
    int ktry = 0;
    const R growthbound = maxgrowth1 * spdiam;
    int indx = 1;

    while (true)
    {
        ldelta = std::min(ldmax, ldelta);
        rdelta = std::min(rdmax, rdelta);

        // Left end: factor (parent - lsigma) -> dplus/lplus.
        bool sawnan1 = false;
        R s = -lsigma;
        dplus[0] = d[0] + s;
        if (std::abs(dplus[0]) < pivmin)
        {
            dplus[0] = -pivmin;
            sawnan1 = true;
        }
        R max1 = std::abs(dplus[0]);
        for (int i = 0; i < n - 1; ++i)
        {
            lplus[i] = ld[i] / dplus[i];
            s = s * lplus[i] * l[i] - lsigma;
            dplus[i + 1] = d[i + 1] + s;
            if (std::abs(dplus[i + 1]) < pivmin)
            {
                dplus[i + 1] = -pivmin;
                sawnan1 = true;
            }
            max1 = std::max(max1, std::abs(dplus[i + 1]));
        }
        sawnan1 = sawnan1 || std::isnan(max1);
        if (forcer || (max1 <= growthbound && !sawnan1))
        {
            sigma = lsigma;
            shift = kSLeft;
            break;
        }

        // Right end: factor (parent - rsigma) -> work[0..n-1] / work[n..2n-2].
        bool sawnan2 = false;
        s = -rsigma;
        work[0] = d[0] + s;
        if (std::abs(work[0]) < pivmin)
        {
            work[0] = -pivmin;
            sawnan2 = true;
        }
        R max2 = std::abs(work[0]);
        for (int i = 0; i < n - 1; ++i)
        {
            work[n + i] = ld[i] / work[i];
            s = s * work[n + i] * l[i] - rsigma;
            work[i + 1] = d[i + 1] + s;
            if (std::abs(work[i + 1]) < pivmin)
            {
                work[i + 1] = -pivmin;
                sawnan2 = true;
            }
            max2 = std::max(max2, std::abs(work[i + 1]));
        }
        sawnan2 = sawnan2 || std::isnan(max2);
        if (forcer || (max2 <= growthbound && !sawnan2))
        {
            sigma = rsigma;
            shift = kSRight;
            break;
        }

        // Both ends grew. Record the better; try the refined RRR test.
        bool both_nan = sawnan1 && sawnan2;
        if (!both_nan)
        {
            if (!sawnan1)
            {
                indx = 1;
                if (max1 <= smlgrowth)
                {
                    smlgrowth = max1;
                    bestshift = lsigma;
                }
            }
            if (!sawnan2)
            {
                if (sawnan1 || max2 <= max1)
                {
                    indx = 2;
                }
                if (max2 <= smlgrowth)
                {
                    smlgrowth = max2;
                    bestshift = rsigma;
                }
            }
            const bool dorrr1 =
                (clwdth < mingap / R{128}) && (std::min(max1, max2) < fail2) && !sawnan1 && !sawnan2;
            if (dorrr1 && indx == 1)
            {
                R tmp = std::abs(dplus[n - 1]);
                R znm2 = one;
                R prod = one;
                R oldp = one;
                for (int i = n - 2; i >= 0; --i)
                {
                    if (prod <= eps)
                    {
                        prod = ((dplus[i + 1] * work[n + i + 1]) / (dplus[i] * work[n + i])) * oldp;
                    }
                    else
                    {
                        prod = prod * std::abs(work[n + i]);
                    }
                    oldp = prod;
                    znm2 += prod * prod;
                    tmp = std::max(tmp, std::abs(dplus[i] * prod));
                }
                if (tmp / (spdiam * crd::math::sqrt(znm2)) <= maxgrowth2)
                {
                    sigma = lsigma;
                    shift = kSLeft;
                    break;
                }
            }
            else if (dorrr1 && indx == 2)
            {
                R tmp = std::abs(work[n - 1]);
                R znm2 = one;
                R prod = one;
                R oldp = one;
                for (int i = n - 2; i >= 0; --i)
                {
                    if (prod <= eps)
                    {
                        prod = ((work[i + 1] * lplus[i + 1]) / (work[i] * lplus[i])) * oldp;
                    }
                    else
                    {
                        prod = prod * std::abs(lplus[i]);
                    }
                    oldp = prod;
                    znm2 += prod * prod;
                    tmp = std::max(tmp, std::abs(work[i] * prod));
                }
                if (tmp / (spdiam * crd::math::sqrt(znm2)) <= maxgrowth2)
                {
                    sigma = rsigma;
                    shift = kSRight;
                    break;
                }
            }
        }

        // Back off to the outside, or force the best shift.
        if (ktry < kKtryMax)
        {
            lsigma = std::max(lsigma - ldelta, lsigma - ldmax);
            rsigma = std::min(rsigma + rdelta, rsigma + rdmax);
            ldelta = two * ldelta;
            rdelta = two * rdelta;
            ++ktry;
        }
        else if (smlgrowth < fail || nofail)
        {
            lsigma = bestshift;
            rsigma = bestshift;
            forcer = true;
        }
        else
        {
            return 1;
        }
    }

    if (shift == kSRight)
    {
        for (int i = 0; i < n; ++i)
        {
            dplus[i] = work[i];
        }
        for (int i = 0; i < n - 1; ++i)
        {
            lplus[i] = work[n + i];
        }
    }
    return 0;
}

// =======================================================================
// mrrr_single_rrr_vectors — eigenVECTORS of an UNREDUCED symmetric tridiagonal
// (d length n, e length n-1) via a SINGLE root RRR + per-eigenvalue twisted
// factorization (dlar1v) + Rayleigh-quotient refinement. WELL-SEPARATED case
// only (no clusters / child RRRs — that is v3a-3.3 dlarrv). Eigenvalues `w`
// (ascending, to high relative accuracy) are an input. z_out is RowMajor n*n
// with column k = eigenvector for w[k]: z_out[row*ldz + k]. Sign per
// D(dense-eig)-4 (lowest-index largest-magnitude component positive).
//
// Root RRR: T - sigma I = L D L^T with sigma a strict lower bound (positive-
// definite => relatively robust). dlar1v's twisted factorization gives each
// eigenvector to high accuracy; for well-separated eigenvalues the vectors are
// orthogonal BY CONSTRUCTION (no Gram-Schmidt = the O(n^2) MRRR win).
// =======================================================================
template <typename R>
inline void mrrr_single_rrr_vectors(crd::memory::IAllocator* alloc, int n, const R* d, const R* e, const R* w,
                                    R* z_out, int ldz)
{
    if (n <= 0)
    {
        return;
    }
    if (n == 1)
    {
        z_out[0] = R{1};
        return;
    }

    const R eps = std::numeric_limits<R>::epsilon();
    const R rqtol = R{2} * eps;
    const R tol = crd::math::sqrt(eps);
    constexpr int kMaxItr = 10;

    R gl = R{0};
    R gu = R{0};
    gershgorin_bounds(d, e, n, gl, gu);
    const R pivmin = compute_pivmin(e, n);
    const R sigma = gl - R{2} * pivmin - R{2} * eps * std::max(std::abs(gl), std::abs(gu));

    crd::containers::Array<R> dd(alloc);
    crd::containers::Array<R> ll(alloc);
    crd::containers::Array<R> lld_arr(alloc);
    crd::containers::Array<R> ldarr(alloc);
    crd::containers::Array<R> zcol(alloc);
    crd::containers::Array<R> work(alloc);
    dd.resize(static_cast<crd::usize>(n));
    ll.resize(static_cast<crd::usize>(n));
    lld_arr.resize(static_cast<crd::usize>(n));
    ldarr.resize(static_cast<crd::usize>(n));
    zcol.resize(static_cast<crd::usize>(n) + 2);
    work.resize(static_cast<crd::usize>(4 * n) + 8);

    // Root RRR: T - sigma I = L D L^T. D = pivots, L[i]=e[i]/D[i],
    // LD[i]=L[i]*D[i]=e[i], LLD[i]=L[i]^2*D[i].
    dd.data()[0] = d[0] - sigma;
    for (int i = 1; i < n; ++i)
    {
        ll.data()[i - 1] = e[i - 1] / dd.data()[i - 1];
        dd.data()[i] = (d[i] - sigma) - e[i - 1] * ll.data()[i - 1];
    }
    for (int i = 0; i < n - 1; ++i)
    {
        ldarr.data()[i] = ll.data()[i] * dd.data()[i];
        lld_arr.data()[i] = ll.data()[i] * ll.data()[i] * dd.data()[i];
    }

    Z1<R> zd{dd.data()};
    Z1<R> zl{ll.data()};
    Z1<R> zld{ldarr.data()};
    Z1<R> zlld{lld_arr.data()};
    Z1<R> zz{zcol.data()};
    Z1<R> zw{work.data()};

    for (int k = 0; k < n; ++k)
    {
        // Gap to the nearest neighbour (well-separated => large).
        R gap = std::numeric_limits<R>::max();
        if (k > 0)
        {
            gap = std::min(gap, w[k] - w[k - 1]);
        }
        if (k < n - 1)
        {
            gap = std::min(gap, w[k + 1] - w[k]);
        }

        R lambda = w[k] - sigma;
        const R left = lambda - R{0.5} * gap;
        const R right = lambda + R{0.5} * gap;
        R bstw = lambda;
        R bstres = std::numeric_limits<R>::max();

        for (int iter = 0; iter < kMaxItr; ++iter)
        {
            const auto out = dlar1v<R>(n, 1, n, lambda, zd, zl, zld, zlld, pivmin, R{0}, zz, 0, false, zw);
            if (out.resid < bstres)
            {
                bstres = out.resid;
                bstw = lambda;
            }
            if (out.resid <= tol * gap || std::abs(out.rqcorr) <= rqtol * std::abs(lambda))
            {
                bstw = lambda;
                break;
            }
            const R next = lambda + out.rqcorr;
            if (next > left && next < right)
            {
                lambda = next;
            }
            else
            {
                break;
            }
        }

        // Final vector at the best shift.
        const auto out = dlar1v<R>(n, 1, n, bstw, zd, zl, zld, zlld, pivmin, R{0}, zz, 0, false, zw);
        const R nrm = out.nrminv;

        // Sign convention: lowest-index largest-magnitude component positive.
        int pivot = 1;
        R best = R{0};
        for (int i = 1; i <= n; ++i)
        {
            const R av = std::abs(zz[i]);
            if (av > best)
            {
                best = av;
                pivot = i;
            }
        }
        const R sign = (zz[pivot] < R{0}) ? R{-1} : R{1};
        for (int i = 0; i < n; ++i)
        {
            z_out[i * ldz + k] = sign * zz[i + 1] * nrm;
        }
    }
}

// =======================================================================
// gram_schmidt_columns — modified Gram-Schmidt orthonormalization of columns
// [lo..hi] of a RowMajor n*ldz matrix (column c = z[row*ldz+c]). The dlarrv
// last-resort fallback for residual clusters the RRR tree cannot separate.
// =======================================================================
template <typename R>
inline void gram_schmidt_columns(R* z, int n, int ldz, int lo, int hi) noexcept
{
    for (int c = lo; c <= hi; ++c)
    {
        for (int p = lo; p < c; ++p)
        {
            R dot = R{0};
            for (int r = 0; r < n; ++r)
            {
                dot += z[r * ldz + c] * z[r * ldz + p];
            }
            for (int r = 0; r < n; ++r)
            {
                z[r * ldz + c] -= dot * z[r * ldz + p];
            }
        }
        R nrm = R{0};
        for (int r = 0; r < n; ++r)
        {
            nrm += z[r * ldz + c] * z[r * ldz + c];
        }
        nrm = crd::math::sqrt(nrm);
        if (nrm > R{0})
        {
            const R inv = R{1} / nrm;
            for (int r = 0; r < n; ++r)
            {
                z[r * ldz + c] *= inv;
            }
        }
    }
}

// One singleton eigenvector via dlar1v on the current RRR + RQ refinement,
// stored (sign-fixed, normalized) into column k of z_out. Shared by the cluster
// recursion.
template <typename R>
inline void mrrr_one_vector(int n, const R* D, const R* L, const R* LD, const R* LLD, R lambda, R gap, R pivmin,
                            R* z_out, int ldz, int k, Z1<R> zz, Z1<R> zwork)
{
    const R eps = std::numeric_limits<R>::epsilon();
    const R rqtol = R{2} * eps;
    const R tol = crd::math::sqrt(eps);
    constexpr int kMaxItr = 10;
    const R left = lambda - R{0.5} * gap;
    const R right = lambda + R{0.5} * gap;
    R bstres = std::numeric_limits<R>::max();
    Z1<R> zd{const_cast<R*>(D)};
    Z1<R> zl{const_cast<R*>(L)};
    Z1<R> zld{const_cast<R*>(LD)};
    Z1<R> zlld{const_cast<R*>(LLD)};
    // Keep the best vector inline (store on improvement) — avoids the wasteful
    // final re-run of dlar1v on the best shift (~1 fewer dlar1v per eigenvector).
    for (int iter = 0; iter < kMaxItr; ++iter)
    {
        const auto out = dlar1v<R>(n, 1, n, lambda, zd, zl, zld, zlld, pivmin, R{0}, zz, 0, false, zwork);
        if (out.resid < bstres)
        {
            bstres = out.resid;
            const R nrm = out.nrminv;
            int pivot = 1;
            R best = R{0};
            for (int i = 1; i <= n; ++i)
            {
                const R av = std::abs(zz[i]);
                if (av > best)
                {
                    best = av;
                    pivot = i;
                }
            }
            const R sign = (zz[pivot] < R{0}) ? R{-1} : R{1};
            for (int i = 0; i < n; ++i)
            {
                z_out[i * ldz + k] = sign * zz[i + 1] * nrm;
            }
        }
        if (out.resid <= tol * gap || std::abs(out.rqcorr) <= rqtol * std::abs(lambda))
        {
            break;
        }
        const R next = lambda + out.rqcorr;
        if (next > left && next < right)
        {
            lambda = next;
        }
        else
        {
            break;
        }
    }
}

// Recursive RRR cluster processor (the dlarrv loop). [lo..hi] are the
// eigenvalue indices belonging to the RRR (D,L,LD,LLD); wrel[lo..hi] are the
// eigenvalues relative to this RRR (ascending). Segments [lo..hi] by relative
// gap: singletons get a vector via dlar1v on this RRR; clusters get a child RRR
// (dlarrf) + refinement (dlarrb) + recursion; at the depth cap, residual
// clusters fall back to Gram-Schmidt. All vectors land in z_out (eigenvectors
// of the ORIGINAL T — every RRR is a shift of T, so they share eigenvectors).
template <typename R>
inline void mrrr_process(crd::memory::IAllocator* alloc, int n, R* D, R* L, R* LD, R* LLD, int lo, int hi, R* wrel,
                         R* werr, R spdiam, R pivmin, R minrgp, int depth, R* z_out, int ldz, Z1<R> zz, Z1<R> zwork)
{
    const R eps = std::numeric_limits<R>::epsilon();
    constexpr int kMaxDepth = 8;

    int i = lo;
    while (i <= hi)
    {
        int j = i;
        while (j < hi)
        {
            const R gap = wrel[j + 1] - wrel[j];
            const R denom = std::max(std::abs(wrel[j]), std::abs(wrel[j + 1]));
            const R relgap = (denom > R{0}) ? (gap / denom) : std::numeric_limits<R>::max();
            if (relgap >= minrgp)
            {
                break;  // separated: cluster ends at j
            }
            ++j;
        }

        if (j == i)
        {
            // Singleton: vector from dlar1v on this RRR.
            R gap = spdiam;
            if (i > lo)
            {
                gap = std::min(gap, wrel[i] - wrel[i - 1]);
            }
            if (i < hi)
            {
                gap = std::min(gap, wrel[i + 1] - wrel[i]);
            }
            if (!(gap > R{0}))
            {
                gap = spdiam;
            }
            mrrr_one_vector<R>(n, D, L, LD, LLD, wrel[i], gap, pivmin, z_out, ldz, i, zz, zwork);
        }
        else if (depth >= kMaxDepth)
        {
            // Residual cluster the RRR tree could not separate: best-effort
            // vectors + Gram-Schmidt (genuine near-multiplicity).
            for (int k = i; k <= j; ++k)
            {
                R gap = spdiam;
                if (k > lo)
                {
                    gap = std::min(gap, std::abs(wrel[k] - wrel[k - 1]));
                }
                if (k < hi)
                {
                    gap = std::min(gap, std::abs(wrel[k + 1] - wrel[k]));
                }
                if (!(gap > R{0}))
                {
                    gap = spdiam;
                }
                mrrr_one_vector<R>(n, D, L, LD, LLD, wrel[k], gap, pivmin, z_out, ldz, k, zz, zwork);
            }
            gram_schmidt_columns<R>(z_out, n, ldz, i, j);
        }
        else
        {
            // Cluster: form a child RRR (dlarrf), refine (dlarrb), recurse.
            crd::containers::Array<R> dplus(alloc);
            crd::containers::Array<R> lplus(alloc);
            crd::containers::Array<R> ldc(alloc);
            crd::containers::Array<R> lldc(alloc);
            crd::containers::Array<R> wgap(alloc);
            crd::containers::Array<R> rrwork(alloc);
            crd::containers::Array<R> wc(alloc);
            crd::containers::Array<R> wec(alloc);
            dplus.resize(static_cast<crd::usize>(n));
            lplus.resize(static_cast<crd::usize>(n));
            ldc.resize(static_cast<crd::usize>(n));
            lldc.resize(static_cast<crd::usize>(n));
            wgap.resize(static_cast<crd::usize>(n));
            rrwork.resize(static_cast<crd::usize>(2 * n));
            wc.resize(static_cast<crd::usize>(n));
            wec.resize(static_cast<crd::usize>(n));

            for (int k = 0; k < n; ++k)
            {
                wgap.data()[k] = (k < n - 1) ? (wrel[k + 1] - wrel[k]) : R{0};
            }
            const R clgapl = (i > lo) ? (wrel[i] - wrel[i - 1]) : spdiam;
            const R clgapr = (j < hi) ? (wrel[j + 1] - wrel[j]) : spdiam;

            R sigma = R{0};
            dlarrf<R>(n, D, L, LD, LLD, i, j, wrel, wgap.data(), werr, spdiam, clgapl, clgapr, pivmin, sigma,
                      dplus.data(), lplus.data(), rrwork.data());

            for (int k = 0; k < n - 1; ++k)
            {
                ldc.data()[k] = lplus.data()[k] * dplus.data()[k];
                lldc.data()[k] = lplus.data()[k] * lplus.data()[k] * dplus.data()[k];
            }
            for (int k = 0; k < n; ++k)
            {
                wc.data()[k] = wrel[k] - sigma;
                wec.data()[k] = werr[k];
            }
            dlarrb_refine<R>(n, dplus.data(), lldc.data(), wc.data(), wec.data(), i + 1, j + 1, R{4} * eps,
                             R{4} * eps, pivmin, spdiam, n);

            mrrr_process<R>(alloc, n, dplus.data(), lplus.data(), ldc.data(), lldc.data(), i, j, wc.data(),
                            wec.data(), spdiam, pivmin, minrgp, depth + 1, z_out, ldz, zz, zwork);
        }

        i = j + 1;
    }
}

// Process a worker's range of top-level segments [gbegin,gend): each segment
// (singleton or cluster) via the recursive mrrr_process. (A dlar1v_x4 batched-
// singleton path was tried + measured 2026-05-23 — no gain: batching 4
// eigenvectors blows the L2 cache with 4x the work footprint, so it is
// memory-bound, not divide-latency-bound. Reverted; the wins are parallelism +
// work-stealing + adaptive granularity. See project_mrrr_perf memory.)
template <typename R>
inline void mrrr_run_segments(crd::memory::IAllocator* alloc, int n, R* D, R* L, R* LD, R* LLD, R* wrel, R* werr,
                              R spdiam, R pivmin, R minrgp, R* z_out, int ldz, const int* glo, const int* ghi,
                              int gbegin, int gend, R* zbuf, R* wbuf)
{
    Z1<R> zz{zbuf};
    Z1<R> zw{wbuf};
    for (int g = gbegin; g < gend; ++g)
    {
        mrrr_process<R>(alloc, n, D, L, LD, LLD, glo[g], ghi[g], wrel, werr, spdiam, pivmin, minrgp, 0, z_out, ldz,
                        zz, zw);
    }
}

// Packed args for the parallel segment dispatch (parallel_for lambda SBO is
// ~41 B). File-scope template avoids local-type-in-lambda quirks.
template <typename R>
struct MrrrPArgs
{
    int n;
    R* D;
    R* L;
    R* LD;
    R* LLD;
    R* wrel;
    R* werr;
    R spdiam;
    R pivmin;
    R minrgp;
    R* z_out;
    int ldz;
    const int* glo;
    const int* ghi;
    // Per-worker scratch (indexed by worker_index()) — fine-grained tasks +
    // work-stealing balance the P/E cores; per-worker buffers avoid per-task malloc.
    R* wzc;            // nw * (n+2) — dlar1v eigenvector scratch
    R* wzw;            // nw * (4n+8) — dlar1v work
    crd::u8* arena;    // nw * arena_bytes — child-RRR arena (0 if all singletons)
    int zc_sz;
    int wk_sz;
    int arena_bytes;
};

// =======================================================================
// mrrr_compute_vectors — FULL MRRR eigenvectors of an unreduced symmetric
// tridiagonal (d length n, e length n-1), cluster-robust. `w` = eigenvalues of
// T ascending (high relative accuracy). z_out RowMajor n*ldz, column k =
// eigenvector for w[k]. Builds the root RRR and runs the recursive cluster
// processor (dlarrv). Orthonormal to O(n)*eps even on clustered/glued spectra.
// =======================================================================
template <typename R>
inline void mrrr_compute_vectors(crd::memory::IAllocator* alloc, int n, const R* d, const R* e, const R* w,
                                 R* z_out, int ldz)
{
    if (n <= 0)
    {
        return;
    }
    if (n == 1)
    {
        z_out[0] = R{1};
        return;
    }

    const R eps = std::numeric_limits<R>::epsilon();
    R gl = R{0};
    R gu = R{0};
    gershgorin_bounds(d, e, n, gl, gu);
    const R spdiam = gu - gl;
    const R pivmin = compute_pivmin(e, n);
    const R sigma_root = gl - R{2} * pivmin - R{2} * eps * std::max(std::abs(gl), std::abs(gu));

    crd::containers::Array<R> dd(alloc);
    crd::containers::Array<R> ll(alloc);
    crd::containers::Array<R> ldd(alloc);
    crd::containers::Array<R> lld(alloc);
    crd::containers::Array<R> wrel(alloc);
    crd::containers::Array<R> werr(alloc);
    crd::containers::Array<R> zcol(alloc);
    crd::containers::Array<R> work(alloc);
    dd.resize(static_cast<crd::usize>(n));
    ll.resize(static_cast<crd::usize>(n));
    ldd.resize(static_cast<crd::usize>(n));
    lld.resize(static_cast<crd::usize>(n));
    wrel.resize(static_cast<crd::usize>(n));
    werr.resize(static_cast<crd::usize>(n));
    zcol.resize(static_cast<crd::usize>(n) + 2);
    work.resize(static_cast<crd::usize>(4 * n) + 8);

    dd.data()[0] = d[0] - sigma_root;
    for (int k = 1; k < n; ++k)
    {
        ll.data()[k - 1] = e[k - 1] / dd.data()[k - 1];
        dd.data()[k] = (d[k] - sigma_root) - e[k - 1] * ll.data()[k - 1];
    }
    for (int k = 0; k < n - 1; ++k)
    {
        ldd.data()[k] = ll.data()[k] * dd.data()[k];
        lld.data()[k] = ll.data()[k] * ll.data()[k] * dd.data()[k];
    }
    for (int k = 0; k < n; ++k)
    {
        wrel.data()[k] = w[k] - sigma_root;
        werr.data()[k] = R{4} * eps * (std::abs(wrel.data()[k]) + spdiam);
    }

    // minrgp: cluster if relative gap below this. ~1e-3 ensures dlar1v
    // orthogonality after a child shift boosts the relative gap.
    const R minrgp = static_cast<R>(1e-3);

    // Enumerate top-level segments (clusters/singletons) by relative gap. Each
    // segment is INDEPENDENT (its own RRR subtree + disjoint z_out columns) =>
    // embarrassingly parallel. This is the crush lever: MRRR eigenvectors are
    // independent and LAPACK's dstemr/dstedc are single-threaded.
    crd::containers::Array<int> glo(alloc);
    crd::containers::Array<int> ghi(alloc);
    glo.resize(static_cast<crd::usize>(n));
    ghi.resize(static_cast<crd::usize>(n));
    int ng = 0;
    {
        int i = 0;
        while (i <= n - 1)
        {
            int j = i;
            while (j < n - 1)
            {
                const R gap = wrel.data()[j + 1] - wrel.data()[j];
                const R denom = std::max(std::abs(wrel.data()[j]), std::abs(wrel.data()[j + 1]));
                const R relgap = (denom > R{0}) ? (gap / denom) : std::numeric_limits<R>::max();
                if (relgap >= minrgp)
                {
                    break;
                }
                ++j;
            }
            glo.data()[ng] = i;
            ghi.data()[ng] = j;
            ++ng;
            i = j + 1;
        }
    }

    const int nw = static_cast<int>(crd::jobs::num_workers());
    if (nw <= 1)
    {
        // Serial (jobs down or single core): batched singletons + cluster recursion.
        mrrr_run_segments<R>(alloc, n, dd.data(), ll.data(), ldd.data(), lld.data(), wrel.data(), werr.data(),
                             spdiam, pivmin, minrgp, z_out, ldz, glo.data(), ghi.data(), 0, ng, zcol.data(),
                             work.data());
        return;
    }

    // Parallel over independent segments. Per-worker pre-allocated scratch
    // (indexed by worker_index()) + FINE-GRAINED tasks so the crd::jobs Chase-Lev
    // work-stealing deques balance the heterogeneous P/E cores (coarse static
    // chunks idle the fast cores). Child-RRR arena = per-worker external-buffer
    // Tlsf (cheap O(1) init, no per-task malloc); only allocated if clusters
    // exist (all-singleton spectra never touch it). Workers write disjoint cols.
    const int zc_sz = n + 2;
    const int wk_sz = 4 * n + 8;
    const bool has_clusters = (ng < n);
    // Child-RRR arena scales with n (recursion depth<=8 x ~9 n-arrays per level),
    // not a fixed 2MB — avoids over-allocating nw*2MB on small/mostly-singleton n.
    const int arena_bytes = has_clusters ? std::max(64 * 1024, n * 900) : 0;

    crd::containers::Array<R> wzc(alloc);
    crd::containers::Array<R> wzw(alloc);
    crd::containers::Array<crd::u8> arena(alloc);
    wzc.resize(static_cast<crd::usize>(nw) * zc_sz);
    wzw.resize(static_cast<crd::usize>(nw) * wk_sz);
    arena.resize(static_cast<crd::usize>(nw) * arena_bytes);

    MrrrPArgs<R> pa{n,         dd.data(), ll.data(),  ldd.data(),  lld.data(), wrel.data(),  werr.data(),
                    spdiam,    pivmin,    minrgp,     z_out,       ldz,        glo.data(),   ghi.data(),
                    wzc.data(), wzw.data(), arena.data(), zc_sz,    wk_sz,      arena_bytes};
    MrrrPArgs<R>* pp = &pa;
    // Adaptive granularity: per-group work scales with n, so ramp tasks/worker
    // with n — coarse for small n (task-spawn overhead dominates), fine for large
    // n (work-stealing balances the P/E cores). 1x at n<=256 .. 8x at n>=2048.
    const int gran = std::max(1, std::min(8, n / 256));
    const int num_chunks = std::min(ng, gran * nw);
    auto* counter = crd::jobs::parallel_for(
        static_cast<crd::u32>(ng), static_cast<crd::u32>(num_chunks), [pp](crd::u32 begin, crd::u32 end) {
            const crd::u32 wi = crd::jobs::worker_index();
            R* zbuf4 = pp->wzc + static_cast<crd::usize>(wi) * pp->zc_sz;
            R* wbuf4 = pp->wzw + static_cast<crd::usize>(wi) * pp->wk_sz;
            if (pp->arena_bytes > 0)
            {
                crd::memory::TlsfAllocator wa(pp->arena + static_cast<crd::usize>(wi) * pp->arena_bytes,
                                              static_cast<crd::usize>(pp->arena_bytes));
                mrrr_run_segments<R>(&wa, pp->n, pp->D, pp->L, pp->LD, pp->LLD, pp->wrel, pp->werr, pp->spdiam,
                                     pp->pivmin, pp->minrgp, pp->z_out, pp->ldz, pp->glo, pp->ghi,
                                     static_cast<int>(begin), static_cast<int>(end), zbuf4, wbuf4);
            }
            else
            {
                // All singletons: mrrr_process's singleton path never allocates.
                mrrr_run_segments<R>(nullptr, pp->n, pp->D, pp->L, pp->LD, pp->LLD, pp->wrel, pp->werr, pp->spdiam,
                                     pp->pivmin, pp->minrgp, pp->z_out, pp->ldz, pp->glo, pp->ghi,
                                     static_cast<int>(begin), static_cast<int>(end), zbuf4, wbuf4);
            }
        });
    crd::jobs::wait(counter);
}

} // namespace crd::hesap::dense::detail
