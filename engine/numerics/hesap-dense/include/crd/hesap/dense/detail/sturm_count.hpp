#pragma once

#include <crd/core/types.hpp>

#include <algorithm>
#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3a-3.1 — MRRR eigenvalue substrate (Sturm-count bisection).
//
// Faithful ports of the LAPACK auxiliaries the MRRR eigenvalue engine
// (dstemr -> dlarre -> dlarra/dlarrc/dlarrk/dlarrd) is built from. All pure
// scalar, header-only, deterministic: only correctly-rounded + - * / sqrt
// and abs/sign; no transcendentals in the numeric path, no RNG (D(dense-eig)
// -9..12, -> ADR-0065 section 17). Operates on the LOWER layer: raw f32/f64
// tridiagonals (d, e, e^2) per ADR-0078 section 5.
//
// The Sturm count is the number of negative pivots of T - x I = L D L^T,
// which equals the number of eigenvalues of the symmetric tridiagonal T
// strictly below x. The |pivot| < pivmin -> pivot = -pivmin guard makes the
// count an EXACT, deterministic step function of x (it also avoids the 1/0)
// — this guard is the determinism-critical line (D(dense-eig)-9).
//
// References (build/win-debug/_deps/openblas-src/lapack-netlib/SRC/):
//   dlarrk.f:219-233  the Sturm recurrence (negcount inner loop)
//   dlarrc.f:183-203  the two-pivot interval count
//   dlarra.f:176-201  splitting into unreduced blocks
//   dlarre.f:423-444  Gershgorin discs + pivmin + spectral diameter
//   dlarrk.f:186-247  single-eigenvalue bisection driver
// -----------------------------------------------------------------------

// safmin — smallest positive normal (LAPACK DLAMCH('S')).
template <typename R>
[[nodiscard]] constexpr R sturm_safmin() noexcept
{
    return std::numeric_limits<R>::min();
}

// =======================================================================
// sturm_negcount — number of eigenvalues of the symmetric tridiagonal
// T = tridiag(e, d, e) that are <= x.  d has length n, e2 holds the n-1
// SQUARED sub-diagonals (e2[i] = e[i]^2, couples rows i,i+1). pivmin > 0.
//
// Faithful to the dlarrk.f:219-233 recurrence (the `.LE.ZERO` test counts an
// exact pivot as negative, so an eigenvalue lying exactly at x is counted —
// the (vl,vu] half-open convention falls out of differencing two counts).
// =======================================================================
template <typename R>
[[nodiscard]] inline int sturm_negcount(const R* d, const R* e2, int n, R x, R pivmin) noexcept
{
    int negcnt = 0;
    R t = d[0] - x;
    if (std::abs(t) < pivmin)
    {
        t = -pivmin;
    }
    if (t <= R{0})
    {
        ++negcnt;
    }
    for (int i = 1; i < n; ++i)
    {
        t = (d[i] - x) - e2[i - 1] / t;
        if (std::abs(t) < pivmin)
        {
            t = -pivmin;
        }
        if (t <= R{0})
        {
            ++negcnt;
        }
    }
    return negcnt;
}

// =======================================================================
// sturm_interval_count — number of eigenvalues of T in the half-open
// interval (vl, vu]. Faithful to dlarrc.f (JOBT='T', EIGCNT = RCNT - LCNT).
//
// The vl- and vu-shifted Sturm recurrences are independent, so this is
// bit-identical to negcount(vu) - negcount(vl); we keep the single fused
// pass (the LAPACK form) so the two pivot sequences share the loop.
// =======================================================================
template <typename R>
[[nodiscard]] inline int sturm_interval_count(const R* d, const R* e2, int n, R vl, R vu, R pivmin) noexcept
{
    int lcnt = 0;
    int rcnt = 0;
    R lpivot = d[0] - vl;
    R rpivot = d[0] - vu;
    if (std::abs(lpivot) < pivmin)
    {
        lpivot = -pivmin;
    }
    if (std::abs(rpivot) < pivmin)
    {
        rpivot = -pivmin;
    }
    if (lpivot <= R{0})
    {
        ++lcnt;
    }
    if (rpivot <= R{0})
    {
        ++rcnt;
    }
    for (int i = 1; i < n; ++i)
    {
        const R tmp = e2[i - 1];
        lpivot = (d[i] - vl) - tmp / lpivot;
        rpivot = (d[i] - vu) - tmp / rpivot;
        if (std::abs(lpivot) < pivmin)
        {
            lpivot = -pivmin;
        }
        if (std::abs(rpivot) < pivmin)
        {
            rpivot = -pivmin;
        }
        if (lpivot <= R{0})
        {
            ++lcnt;
        }
        if (rpivot <= R{0})
        {
            ++rcnt;
        }
    }
    return rcnt - lcnt;
}

// =======================================================================
// compute_pivmin — the minimum pivot for the Sturm sequence.
//   pivmin = safmin * max(1, max_i e[i]^2)        (dlarre.f:441)
// e has length n-1 (the raw sub-diagonals). For n <= 1, pivmin = safmin.
// =======================================================================
template <typename R>
[[nodiscard]] inline R compute_pivmin(const R* e, int n) noexcept
{
    R emax = R{0};
    for (int i = 0; i < n - 1; ++i)
    {
        const R ea = std::abs(e[i]);
        if (ea > emax)
        {
            emax = ea;
        }
    }
    const R emax2 = emax * emax;
    return sturm_safmin<R>() * (emax2 > R{1} ? emax2 : R{1});
}

// =======================================================================
// gershgorin_bounds — Gershgorin discs of the tridiagonal and the global
// spectral bracket. For row i the disc is [d_i - r_i, d_i + r_i] with
// radius r_i = |e_{i-1}| + |e_i|. Returns gl = min left edge, gu = max
// right edge. If `gers` (length 2n) is non-null, the per-row edges are
// written as gers[2i] = left, gers[2i+1] = right. This 0-based pairing is
// exactly LAPACK's 1-based GERS(2I-1)=left / GERS(2I)=right after the index
// shift — the per-eigenvalue tight-bracket lookup in v3a-3.2 reads it that
// way. Faithful to dlarre.f:423-438 (EOLD carries |e_{i-1}|). The caller
// widens by the FUDGE factor.
// =======================================================================
template <typename R>
inline void gershgorin_bounds(const R* d, const R* e, int n, R& gl, R& gu, R* gers = nullptr) noexcept
{
    R eold = R{0};
    gl = d[0];
    gu = d[0];
    for (int i = 0; i < n; ++i)
    {
        const R eabs = (i < n - 1) ? std::abs(e[i]) : R{0};
        const R radius = eabs + eold;
        const R left = d[i] - radius;
        const R right = d[i] + radius;
        if (gers != nullptr)
        {
            gers[2 * i] = left;
            gers[2 * i + 1] = right;
        }
        if (left < gl)
        {
            gl = left;
        }
        if (right > gu)
        {
            gu = right;
        }
        eold = eabs;
    }
}

// =======================================================================
// tridiag_split — set "small" off-diagonals to zero and record the block
// boundaries the tridiagonal decouples into (dlarra.f). The relative
// criterion (spltol > 0) preserves relative accuracy:
//   split at i  iff  |e[i]| <= spltol * sqrt(|d[i] * d[i+1]|).
// On exit e[i]/e2[i] at split points are zeroed; isplit[0..nsplit-1] holds
// the (0-based, inclusive) END row of each block (last is n-1). Returns
// nsplit (>= 1). e and e2 are length n (index n-1 untouched/ignored).
// =======================================================================
template <typename R>
[[nodiscard]] inline int tridiag_split(const R* d, R* e, R* e2, int n, R spltol, int* isplit) noexcept
{
    int nsplit = 0;
    if (n <= 0)
    {
        return 0;
    }
    for (int i = 0; i < n - 1; ++i)
    {
        const R eabs = std::abs(e[i]);
        const R thresh = spltol * crd::math::sqrt(std::abs(d[i])) * crd::math::sqrt(std::abs(d[i + 1]));
        if (eabs <= thresh)
        {
            e[i] = R{0};
            e2[i] = R{0};
            isplit[nsplit] = i;  // block ends at row i
            ++nsplit;
        }
    }
    isplit[nsplit] = n - 1;  // final block ends at the last row
    ++nsplit;
    return nsplit;
}

// =======================================================================
// bisect_eigenvalue — the iw-th eigenvalue (1-based, ascending) of an
// UNREDUCED block (d, e2 of length `n`) by Sturm-count bisection, faithful
// to dlarrk.f:186-247. [gl, gu] is the block's Gershgorin bracket (widened
// here by the FUDGE factor). reltol is the minimum relative interval width.
// Returns the eigenvalue w and its error bound werr; info = 0 on convergence,
// -1 if the iteration cap was hit (bracket still returned).
// =======================================================================
template <typename R>
struct EigBracket
{
    R w;
    R werr;
    int info;
};

template <typename R>
[[nodiscard]] inline EigBracket<R> bisect_eigenvalue(const R* d, const R* e2, int n, int iw, R gl, R gu, R pivmin,
                                                     R reltol) noexcept
{
    constexpr R kFudge = R{2};
    const R eps = std::numeric_limits<R>::epsilon();
    const R tnorm = std::max(std::abs(gl), std::abs(gu));
    const R atoli = kFudge * kFudge * pivmin;  // FUDGE*TWO*PIVMIN (dlarrk.f:191)
    const R rtoli = reltol;

    // Deterministic iteration cap ~ log2((tnorm+pivmin)/pivmin)+2, via integer
    // doubling (no crd::math::log in the numeric path). Hard ceiling guards overflow.
    int itmax = 2;
    {
        const R ratio = (tnorm + pivmin) / pivmin;
        R p = R{1};
        while (p < ratio && itmax < 2000)
        {
            p *= R{2};
            ++itmax;
        }
    }

    R left = gl - kFudge * tnorm * eps * static_cast<R>(n) - kFudge * kFudge * pivmin;
    R right = gu + kFudge * tnorm * eps * static_cast<R>(n) + kFudge * kFudge * pivmin;

    int info = -1;
    int it = 0;
    while (true)
    {
        const R width = std::abs(right - left);
        const R mag = std::max(std::abs(right), std::abs(left));
        const R conv = std::max(atoli, std::max(pivmin, rtoli * mag));
        if (width < conv)
        {
            info = 0;
            break;
        }
        if (it > itmax)
        {
            break;
        }
        ++it;
        const R mid = R{0.5} * (left + right);
        const int negcnt = sturm_negcount(d, e2, n, mid, pivmin);
        if (negcnt >= iw)
        {
            right = mid;
        }
        else
        {
            left = mid;
        }
    }

    return EigBracket<R>{R{0.5} * (left + right), R{0.5} * std::abs(right - left), info};
}

// =======================================================================
// multisection_chunk — compute eigenvalues with indices [klo, khi) (0-based,
// ascending) of an UNREDUCED block (d, e2 length n) by SHARED-Sturm
// multisection: one negcount(mid) splits an interval for ALL eigenvalues it
// contains (the dlaebz sharing structure), eliminating the ~16x redundancy of
// naive per-eigenvalue bisection. Embarrassingly parallel across disjoint
// index chunks — the lever LAPACK's serial dsterf/dstemr cannot answer.
//
// [gl, gu] is the block's (widened) Gershgorin bracket. Stack scratch (caller-
// owned, no allocation): sl/sr length >= 2*(khi-klo)+64, sclo/schi same. Output
// w[klo..khi-1] ascending. Deterministic (pivmin Sturm guard, fixed bracket).
// =======================================================================
template <typename R>
inline void multisection_chunk(const R* d, const R* e2, int n, int klo, int khi, R gl, R gu, R pivmin, R reltol,
                               R* w, R* sl, R* sr, int* sclo, int* schi) noexcept
{
    if (khi <= klo)
    {
        return;
    }
    const R atol = R{2} * pivmin;

    // Separator search: a point x with negcount(x) == target (boundary between
    // eigenvalue target-1 and target). Endpoints fall back to the bracket.
    auto find_sep = [&](int target) noexcept -> R {
        if (target <= 0)
        {
            return gl;
        }
        if (target >= n)
        {
            return gu;
        }
        R a = gl;
        R b = gu;
        for (int it = 0; it < 64; ++it)
        {
            const R m = R{0.5} * (a + b);
            if (sturm_negcount(d, e2, n, m, pivmin) >= target)
            {
                b = m;
            }
            else
            {
                a = m;
            }
        }
        return R{0.5} * (a + b);
    };

    int sp = 0;
    sl[sp] = find_sep(klo);
    sr[sp] = find_sep(khi);
    sclo[sp] = klo;
    schi[sp] = khi;
    ++sp;

    while (sp > 0)
    {
        --sp;
        const R lft = sl[sp];
        const R rgt = sr[sp];
        const int clo = sclo[sp];
        const int chi = schi[sp];
        if (chi <= clo)
        {
            continue;
        }
        const R mag = std::max(std::abs(lft), std::abs(rgt));
        if ((rgt - lft) <= reltol * mag + atol)
        {
            const R mid = R{0.5} * (lft + rgt);
            for (int k = clo; k < chi; ++k)
            {
                w[k] = mid;
            }
            continue;
        }
        const R mid = R{0.5} * (lft + rgt);
        int cmid = sturm_negcount(d, e2, n, mid, pivmin);
        if (cmid < clo)
        {
            cmid = clo;
        }
        if (cmid > chi)
        {
            cmid = chi;
        }
        // Push children (one negcount(mid) served BOTH halves = the sharing win).
        if (cmid > clo)
        {
            sl[sp] = lft;
            sr[sp] = mid;
            sclo[sp] = clo;
            schi[sp] = cmid;
            ++sp;
        }
        if (chi > cmid)
        {
            sl[sp] = mid;
            sr[sp] = rgt;
            sclo[sp] = cmid;
            schi[sp] = chi;
            ++sp;
        }
    }
}

// =======================================================================
// tridiag_eigenvalues — all eigenvalues of a (possibly reducible) symmetric
// tridiagonal (d_in length n, e_in length n-1), ascending. The end-to-end
// driver that validates the substrate: split (dlarra) -> per-block
// Gershgorin bracket + pivmin -> per-index bisection (dlarrk).
//
// D(dense-eig)-MRRR-divergence-1: this .1-start driver isolates EVERY
// eigenvalue with per-index dlarrk bisection (O(in^2 log) per block). The
// dlasq2 dqds whole-block fast path (the LAPACK production route through
// dlarre) lands at v3a-3.1 completion; the eigenvalues are identical, dqds
// is only faster. Correctness-first per the phase rule.
//
// Scratch (caller-owned, no allocation here): e_work, e2_work (length n
// each) + isplit (length n) + gers unused. w_out (length n) ascending;
// werr_out (length n, nullable). reltol = the relative bisection width
// (e.g. 4*eps for full accuracy).
// =======================================================================
template <typename R>
inline void tridiag_eigenvalues(const R* d_in, const R* e_in, int n, R* e_work, R* e2_work, int* isplit, R* w_out,
                                R* werr_out, R reltol) noexcept
{
    if (n <= 0)
    {
        return;
    }
    if (n == 1)
    {
        w_out[0] = d_in[0];
        if (werr_out != nullptr)
        {
            werr_out[0] = R{0};
        }
        return;
    }

    const R pivmin = compute_pivmin(e_in, n);  // pivmin from the sub-diagonals
    for (int i = 0; i < n - 1; ++i)
    {
        e_work[i] = e_in[i];
        e2_work[i] = e_in[i] * e_in[i];
    }
    e_work[n - 1] = R{0};
    e2_work[n - 1] = R{0};

    const R spltol = crd::math::sqrt(std::numeric_limits<R>::epsilon());  // relative split tolerance
    const int nsplit = tridiag_split(d_in, e_work, e2_work, n, spltol, isplit);

    int out = 0;
    int begin = 0;
    for (int b = 0; b < nsplit; ++b)
    {
        const int end = isplit[b];        // inclusive block-end row
        const int in = end - begin + 1;   // block size
        const R* db = d_in + begin;
        const R* e2b = e2_work + begin;

        if (in == 1)
        {
            w_out[out] = db[0];
            if (werr_out != nullptr)
            {
                werr_out[out] = R{0};
            }
            ++out;
        }
        else
        {
            R gl;
            R gu;
            gershgorin_bounds(db, e_work + begin, in, gl, gu);
            for (int k = 1; k <= in; ++k)
            {
                const EigBracket<R> eb = bisect_eigenvalue(db, e2b, in, k, gl, gu, pivmin, reltol);
                w_out[out] = eb.w;
                if (werr_out != nullptr)
                {
                    werr_out[out] = eb.werr;
                }
                ++out;
            }
        }
        begin = end + 1;
    }

    // Eigenvalues come out ascending within each block but blocks are not
    // globally ordered; sort the whole set ascending (selection sort — n is
    // small in tests; the production path orders by block + dqds). Stable to
    // ties; werr rides along.
    for (int i = 0; i < out - 1; ++i)
    {
        int m = i;
        for (int j = i + 1; j < out; ++j)
        {
            if (w_out[j] < w_out[m])
            {
                m = j;
            }
        }
        if (m != i)
        {
            const R tw = w_out[i];
            w_out[i] = w_out[m];
            w_out[m] = tw;
            if (werr_out != nullptr)
            {
                const R te = werr_out[i];
                werr_out[i] = werr_out[m];
                werr_out[m] = te;
            }
        }
    }
}

} // namespace crd::hesap::dense::detail
