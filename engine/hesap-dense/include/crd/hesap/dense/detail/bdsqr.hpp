#pragma once

#include <crd/core/types.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3b-1b — bidiagonal SVD via Demmel-Kahan implicit-zero-shift
// QR (`dbdsqr`) + its 2x2 / plane-rotation helpers, ported faithfully from
// LAPACK. Lower layer: raw f32/f64 (ADR-0078).
//
// Demmel & Kahan, "Computing Small Singular Values of Bidiagonal Matrices
// With Guaranteed High Relative Accuracy", LAPACK Working Note #3
// (SIAM J. Sci. Statist. Comput. 11(5):873-912, 1990).
//
// PORT FIDELITY pins (→ ADR-0065 §18 at slice close):
//   dia(svd)-1 dlartg uses the f90 convention (c >= 0, r = sign(d,f),
//     s = g/r). dbdsqr's zero-shift sweep CHAINS `r` across consecutive
//     dlartg calls, so the convention is load-bearing — eig_sym.cpp's
//     `lartg` (whose c carries the sign of f) is a DIFFERENT rotation and
//     must NOT be substituted here.
//   dia(svd)-4 dlasr is rewritten for RowMajor (A(r,c) = a[r*lda + c]) — the
//     transpose of the column-major Fortran inner loops. Only PIVOT='V'
//     (the plane (k,k+1)) is ported — the only pivot dbdsqr uses.
//
// References (build/win-debug/_deps/openblas-src/lapack-netlib/SRC/):
//   dbdsqr.f  dlasv2.f  dlas2.f  dlasr.f  dlartg.f90
// -----------------------------------------------------------------------

// Fortran SIGN(a,b): |a| with the sign of b; SIGN(a,0) = +|a|.
template <typename R>
[[nodiscard]] inline R fsign(R a, R b) noexcept
{
    const R aa = std::abs(a);
    return b >= R{0} ? aa : -aa;
}

// =======================================================================
// dlartg — plane rotation [ c s; -s c ] * [f; g] = [r; 0], c^2 + s^2 = 1.
// Faithful port of dlartg.f90 (Anderson 2017 safe-scaling): c >= 0,
// r = sign(d,f), s = g/r. Overflow/underflow guarded.
// =======================================================================
template <typename R>
inline void dlartg(R f, R g, R& c, R& s, R& r) noexcept
{
    const R safmin = std::numeric_limits<R>::min();
    const R safmax = std::numeric_limits<R>::max();
    const R rtmin = std::sqrt(safmin);
    const R rtmax = std::sqrt(safmax / R{2});

    const R f1 = std::abs(f);
    const R g1 = std::abs(g);
    if (g == R{0})
    {
        c = R{1};
        s = R{0};
        r = f;
    }
    else if (f == R{0})
    {
        c = R{0};
        s = fsign(R{1}, g);
        r = g1;
    }
    else if (f1 > rtmin && f1 < rtmax && g1 > rtmin && g1 < rtmax)
    {
        const R d = std::sqrt(f * f + g * g);
        c = f1 / d;
        r = fsign(d, f);
        s = g / r;
    }
    else
    {
        const R u = std::min(safmax, std::max(safmin, std::max(f1, g1)));
        const R fs = f / u;
        const R gs = g / u;
        const R d = std::sqrt(fs * fs + gs * gs);
        c = std::abs(fs) / d;
        r = fsign(d, f);
        s = gs / r;
        r = r * u;
    }
}

// =======================================================================
// dlas2 — singular values of the 2x2 upper-triangular [ f g; 0 h ].
// ssmin = smaller, ssmax = larger. Faithful port of dlas2.f.
// =======================================================================
template <typename R>
inline void dlas2(R f, R g, R h, R& ssmin, R& ssmax) noexcept
{
    const R fa = std::abs(f);
    const R ga = std::abs(g);
    const R ha = std::abs(h);
    const R fhmn = std::min(fa, ha);
    const R fhmx = std::max(fa, ha);
    if (fhmn == R{0})
    {
        ssmin = R{0};
        if (fhmx == R{0})
        {
            ssmax = ga;
        }
        else
        {
            const R rt = std::min(fhmx, ga) / std::max(fhmx, ga);
            ssmax = std::max(fhmx, ga) * std::sqrt(R{1} + rt * rt);
        }
    }
    else if (ga < fhmx)
    {
        const R as = R{1} + fhmn / fhmx;
        const R at = (fhmx - fhmn) / fhmx;
        const R au = (ga / fhmx) * (ga / fhmx);
        const R cc = R{2} / (std::sqrt(as * as + au) + std::sqrt(at * at + au));
        ssmin = fhmn * cc;
        ssmax = fhmx / cc;
    }
    else
    {
        const R au = fhmx / ga;
        if (au == R{0})
        {
            ssmin = (fhmn * fhmx) / ga;
            ssmax = ga;
        }
        else
        {
            const R as = R{1} + fhmn / fhmx;
            const R at = (fhmx - fhmn) / fhmx;
            const R cc = R{1} / (std::sqrt(R{1} + (as * au) * (as * au)) + std::sqrt(R{1} + (at * au) * (at * au)));
            ssmin = ((fhmn * cc) * au) * R{2};
            ssmax = ga / (cc + cc);
        }
    }
}

// =======================================================================
// dlasv2 — full SVD of the 2x2 upper-triangular [ f g; 0 h ]:
//   [ csl snl ] [ f g ] [ csr -snr ] = [ ssmax  0    ]
//   [-snl csl ] [ 0 h ] [ snr  csr ]   [  0     ssmin].
// Faithful port of dlasv2.f.
// =======================================================================
template <typename R>
inline void dlasv2(R f, R g, R h, R& ssmin, R& ssmax, R& snr, R& csr, R& snl, R& csl) noexcept
{
    const R eps = std::numeric_limits<R>::epsilon();

    R ft = f;
    R fa = std::abs(ft);
    R ht = h;
    R ha = std::abs(h);

    int pmax = 1;
    const bool swap = ha > fa;
    if (swap)
    {
        pmax = 3;
        std::swap(ft, ht);
        std::swap(fa, ha);
        // now fa >= ha
    }
    const R gt = g;
    const R ga = std::abs(gt);

    R clt = R{0};
    R crt = R{0};
    R slt = R{0};
    R srt = R{0};
    if (ga == R{0})
    {
        // Diagonal matrix.
        ssmin = ha;
        ssmax = fa;
        clt = R{1};
        crt = R{1};
        slt = R{0};
        srt = R{0};
    }
    else
    {
        bool gasmal = true;
        if (ga > fa)
        {
            pmax = 2;
            if ((fa / ga) < eps)
            {
                // Very large ga.
                gasmal = false;
                ssmax = ga;
                if (ha > R{1})
                {
                    ssmin = fa / (ga / ha);
                }
                else
                {
                    ssmin = (fa / ga) * ha;
                }
                clt = R{1};
                slt = ht / gt;
                srt = R{1};
                crt = ft / gt;
            }
        }
        if (gasmal)
        {
            // Normal case.
            const R d = fa - ha;
            R l = (d == fa) ? R{1} : d / fa;  // copes with infinite f or h
            const R m = gt / ft;
            R t = R{2} - l;
            const R mm = m * m;
            const R tt = t * t;
            const R sv = std::sqrt(tt + mm);
            const R rr = (l == R{0}) ? std::abs(m) : std::sqrt(l * l + mm);
            const R a = R{0.5} * (sv + rr);
            ssmin = ha / a;
            ssmax = fa * a;
            if (mm == R{0})
            {
                if (l == R{0})
                {
                    t = fsign(R{2}, ft) * fsign(R{1}, gt);
                }
                else
                {
                    t = gt / fsign(d, ft) + m / t;
                }
            }
            else
            {
                t = (m / (sv + t) + m / (rr + l)) * (R{1} + a);
            }
            l = std::sqrt(t * t + R{4});
            crt = R{2} / l;
            srt = t / l;
            clt = (crt + srt * m) / a;
            slt = (ht / ft) * srt / a;
        }
    }

    if (swap)
    {
        csl = srt;
        snl = crt;
        csr = slt;
        snr = clt;
    }
    else
    {
        csl = clt;
        snl = slt;
        csr = crt;
        snr = srt;
    }

    // Correct signs of ssmax and ssmin.
    R tsign = R{0};
    if (pmax == 1)
    {
        tsign = fsign(R{1}, csr) * fsign(R{1}, csl) * fsign(R{1}, f);
    }
    else if (pmax == 2)
    {
        tsign = fsign(R{1}, snr) * fsign(R{1}, csl) * fsign(R{1}, g);
    }
    else  // pmax == 3
    {
        tsign = fsign(R{1}, snr) * fsign(R{1}, snl) * fsign(R{1}, h);
    }
    ssmax = fsign(ssmax, tsign);
    ssmin = fsign(ssmin, tsign * fsign(R{1}, f) * fsign(R{1}, h));
}

// =======================================================================
// dlasr — apply a sequence of plane rotations to a RowMajor matrix A
// (m x n, A(r,c) = a[r*lda + c]), PIVOT='V' only (plane (k,k+1)).
//   side = 'L'  -> A := P * A     (rotate rows k,k+1)
//   side = 'R'  -> A := A * P^T   (rotate columns k,k+1)
//   direct = 'F' -> P = P(z-1)...P(1); 'B' -> P = P(1)...P(z-1).
// c/s are 0-based, length (m-1) for side='L', (n-1) for side='R'. Faithful
// to dlasr.f (column-major) transposed to RowMajor (dia(svd)-4).
// =======================================================================
template <typename R>
inline void dlasr_lv(bool forward, int m, int n, const R* c, const R* s, R* a, int lda) noexcept
{
    if (m == 0 || n == 0)
    {
        return;
    }
    auto apply = [&](int j) {
        const R ctemp = c[j];
        const R stemp = s[j];
        if (ctemp != R{1} || stemp != R{0})
        {
            R* row_j = a + j * lda;
            R* row_j1 = a + (j + 1) * lda;
            for (int i = 0; i < n; ++i)
            {
                const R temp = row_j1[i];
                row_j1[i] = ctemp * temp - stemp * row_j[i];
                row_j[i] = stemp * temp + ctemp * row_j[i];
            }
        }
    };
    if (forward)
    {
        for (int j = 0; j < m - 1; ++j)
        {
            apply(j);
        }
    }
    else
    {
        for (int j = m - 2; j >= 0; --j)
        {
            apply(j);
        }
    }
}

template <typename R>
inline void dlasr_rv(bool forward, int m, int n, const R* c, const R* s, R* a, int lda) noexcept
{
    if (m == 0 || n == 0)
    {
        return;
    }
    auto apply = [&](int j) {
        const R ctemp = c[j];
        const R stemp = s[j];
        if (ctemp != R{1} || stemp != R{0})
        {
            for (int i = 0; i < m; ++i)
            {
                R* row = a + i * lda;
                const R temp = row[j + 1];
                row[j + 1] = ctemp * temp - stemp * row[j];
                row[j] = stemp * temp + ctemp * row[j];
            }
        }
    };
    if (forward)
    {
        for (int j = 0; j < n - 1; ++j)
        {
            apply(j);
        }
    }
    else
    {
        for (int j = n - 2; j >= 0; --j)
        {
            apply(j);
        }
    }
}

// drot — BLAS plane rotation of two strided vectors x,y (length nn):
//   x_i = c*x_i + s*y_i ; y_i = c*y_i - s*x_i (using the OLD x_i).
template <typename R>
inline void drot(int nn, R* x, int incx, R* y, int incy, R c, R s) noexcept
{
    int ix = 0;
    int iy = 0;
    for (int i = 0; i < nn; ++i)
    {
        const R tx = x[ix];
        const R ty = y[iy];
        x[ix] = c * tx + s * ty;
        y[iy] = c * ty - s * tx;
        ix += incx;
        iy += incy;
    }
}

// =======================================================================
// dbdsqr — SVD of an n x n (upper, when `upper`) bidiagonal matrix B via the
// implicit-zero-shift QR algorithm. d[0..n-1] diagonal, e[0..n-2] super-diag.
// On exit d holds the singular values in DESCENDING order (non-negative).
// Accumulates rotations:
//   vt (n x ncvt, RowMajor ldvt): VT := P^T * VT   (right singular vectors)
//   u  (nru x n,  RowMajor ldu) : U  := U  * Q     (left  singular vectors)
//   c  (n x ncc,  RowMajor ldc) : C  := Q^T * C    (ncc=0 in our use)
// `work` length >= 4*(n-1). Returns 0 on success; >0 = #unconverged e's.
// Faithful port of dbdsqr.f (UPLO='U' or 'L'); ROTATE branch only — the
// values-only fast path (dlasq1) is handled at the driver via dlasq2.
// =======================================================================
template <typename R>
inline int dbdsqr(bool upper, int n, int ncvt, int nru, int ncc, R* d, R* e, R* vt, int ldvt, R* u, int ldu, R* c,
                  int ldc, R* work) noexcept
{
    const R zero = R{0};
    const R one = R{1};
    const R negone = R{-1};
    const R hndrth = static_cast<R>(0.01);
    const R ten = R{10};
    const R hndrd = R{100};
    const int maxitr = 6;

    if (n == 0)
    {
        return 0;
    }

    const bool lower = !upper;

    // 1-based array accessors mirroring the Fortran (named to avoid clashing
    // with the d/e/u/c array params; capitalised forms would trip clang-tidy).
    auto dia = [&](int i) -> R& { return d[i - 1]; };
    auto eoff = [&](int i) -> R& { return e[i - 1]; };
    auto wrk = [&](int i) -> R& { return work[i - 1]; };
    auto vtm = [&](int r, int col) -> R& { return vt[(r - 1) * ldvt + (col - 1)]; };
    auto um = [&](int r, int col) -> R& { return u[(r - 1) * ldu + (col - 1)]; };
    auto cm = [&](int r, int col) -> R& { return c[(r - 1) * ldc + (col - 1)]; };

    const R eps = std::numeric_limits<R>::epsilon();
    const R unfl = std::numeric_limits<R>::min();

    if (n == 1)
    {
        if (dia(1) < zero)
        {
            dia(1) = -dia(1);
            if (ncvt > 0)
            {
                for (int k = 1; k <= ncvt; ++k)
                {
                    vtm(1, k) = negone * vtm(1, k);
                }
            }
        }
        return 0;
    }

    const int nm1 = n - 1;
    const int nm12 = nm1 + nm1;
    const int nm13 = nm12 + nm1;
    int idir = 0;

    R cs = zero;
    R sn = zero;
    R rr = zero;
    R oldcs = zero;
    R oldsn = zero;

    // If lower bidiagonal, rotate to upper via left Givens (saved in WORK).
    if (lower)
    {
        for (int i = 1; i <= n - 1; ++i)
        {
            dlartg(dia(i), eoff(i), cs, sn, rr);
            dia(i) = rr;
            eoff(i) = sn * dia(i + 1);
            dia(i + 1) = cs * dia(i + 1);
            wrk(i) = cs;
            wrk(nm1 + i) = sn;
        }
        if (nru > 0)
        {
            dlasr_rv(true, nru, n, &wrk(1), &wrk(n), u, ldu);
        }
        if (ncc > 0)
        {
            dlasr_lv(true, n, ncc, &wrk(1), &wrk(n), c, ldc);
        }
    }

    // Convergence tolerance. TOLMUL = max(10, min(100, eps^(-1/8))); the
    // eighth root of 1/eps is sqrt(sqrt(sqrt(.))) — avoids std::pow (and the
    // no-std-math guard) while staying bit-faithful to LAPACK's intent.
    const R tolmul = std::max(ten, std::min(hndrd, std::sqrt(std::sqrt(std::sqrt(one / eps)))));
    const R tol = tolmul * eps;

    // Approximate max / min singular values.
    R smax = zero;
    for (int i = 1; i <= n; ++i)
    {
        smax = std::max(smax, std::abs(dia(i)));
    }
    for (int i = 1; i <= n - 1; ++i)
    {
        smax = std::max(smax, std::abs(eoff(i)));
    }
    R sminl = zero;
    R thresh;
    if (tol >= zero)
    {
        R sminoa = std::abs(dia(1));
        if (sminoa != zero)
        {
            R mu = sminoa;
            for (int i = 2; i <= n; ++i)
            {
                mu = std::abs(dia(i)) * (mu / (mu + std::abs(eoff(i - 1))));
                sminoa = std::min(sminoa, mu);
                if (sminoa == zero)
                {
                    break;
                }
            }
        }
        sminoa = sminoa / std::sqrt(static_cast<R>(n));
        thresh = std::max(tol * sminoa, static_cast<R>(maxitr) * (static_cast<R>(n) * (static_cast<R>(n) * unfl)));
    }
    else
    {
        thresh = std::max(std::abs(tol) * smax, static_cast<R>(maxitr) * (static_cast<R>(n) * (static_cast<R>(n) * unfl)));
    }

    // Main iteration. M = last index of unconverged part.
    const int maxitdivn = maxitr * n;
    int iterdivn = 0;
    int iter = -1;
    int oldll = -1;
    int oldm = -1;
    int m = n;
    int ll = 0;

    bool maxit_exceeded = false;
    while (true)
    {
        // ---- label 60 (check convergence / iteration count) ----
        if (m <= 1)
        {
            break;  // GO TO 160 (converged)
        }
        if (iter >= n)
        {
            iter = iter - n;
            iterdivn = iterdivn + 1;
            if (iterdivn >= maxitdivn)
            {
                maxit_exceeded = true;
                break;
            }
        }

        // Find diagonal block to work on.
        if (tol < zero && std::abs(dia(m)) <= thresh)
        {
            dia(m) = zero;
        }
        smax = std::abs(dia(m));
        bool found80 = false;
        for (int lll = 1; lll <= m - 1; ++lll)
        {
            ll = m - lll;
            const R abss = std::abs(dia(ll));
            const R abse = std::abs(eoff(ll));
            if (tol < zero && abss <= thresh)
            {
                dia(ll) = zero;
            }
            if (abse <= thresh)
            {
                found80 = true;
                break;
            }
            smax = std::max({smax, abss, abse});
        }
        if (!found80)
        {
            ll = 0;
        }
        else
        {
            // label 80: eoff(LL) = 0 -> split.
            eoff(ll) = zero;
            if (ll == m - 1)
            {
                // Convergence of bottom singular value.
                m = m - 1;
                continue;  // GO TO 60
            }
        }
        // label 90:
        ll = ll + 1;

        // 2x2 block — handle separately.
        if (ll == m - 1)
        {
            R sigmn;
            R sigmx;
            R sinr;
            R cosr;
            R sinl;
            R cosl;
            dlasv2(dia(m - 1), eoff(m - 1), dia(m), sigmn, sigmx, sinr, cosr, sinl, cosl);
            dia(m - 1) = sigmx;
            eoff(m - 1) = zero;
            dia(m) = sigmn;
            if (ncvt > 0)
            {
                drot(ncvt, &vtm(m - 1, 1), 1, &vtm(m, 1), 1, cosr, sinr);
            }
            if (nru > 0)
            {
                drot(nru, &um(1, m - 1), ldu, &um(1, m), ldu, cosl, sinl);
            }
            if (ncc > 0)
            {
                drot(ncc, &cm(m - 1, 1), 1, &cm(m, 1), 1, cosl, sinl);
            }
            m = m - 2;
            continue;  // GO TO 60
        }

        // Choose shift direction for a new submatrix.
        if (ll > oldm || m < oldll)
        {
            idir = (std::abs(dia(ll)) >= std::abs(dia(m))) ? 1 : 2;
        }

        // Convergence tests.
        if (idir == 1)
        {
            if (std::abs(eoff(m - 1)) <= std::abs(tol) * std::abs(dia(m)) || (tol < zero && std::abs(eoff(m - 1)) <= thresh))
            {
                eoff(m - 1) = zero;
                continue;  // GO TO 60
            }
            if (tol >= zero)
            {
                R mu = std::abs(dia(ll));
                sminl = mu;
                bool conv = false;
                for (int lll = ll; lll <= m - 1; ++lll)
                {
                    if (std::abs(eoff(lll)) <= tol * mu)
                    {
                        eoff(lll) = zero;
                        conv = true;
                        break;
                    }
                    mu = std::abs(dia(lll + 1)) * (mu / (mu + std::abs(eoff(lll))));
                    sminl = std::min(sminl, mu);
                }
                if (conv)
                {
                    continue;  // GO TO 60
                }
            }
        }
        else
        {
            if (std::abs(eoff(ll)) <= std::abs(tol) * std::abs(dia(ll)) || (tol < zero && std::abs(eoff(ll)) <= thresh))
            {
                eoff(ll) = zero;
                continue;  // GO TO 60
            }
            if (tol >= zero)
            {
                R mu = std::abs(dia(m));
                sminl = mu;
                bool conv = false;
                for (int lll = m - 1; lll >= ll; --lll)
                {
                    if (std::abs(eoff(lll)) <= tol * mu)
                    {
                        eoff(lll) = zero;
                        conv = true;
                        break;
                    }
                    mu = std::abs(dia(lll)) * (mu / (mu + std::abs(eoff(lll))));
                    sminl = std::min(sminl, mu);
                }
                if (conv)
                {
                    continue;  // GO TO 60
                }
            }
        }
        oldll = ll;
        oldm = m;

        // Compute shift; zero it if it would ruin relative accuracy.
        R shift = zero;
        R sll;
        if (tol >= zero && static_cast<R>(n) * tol * (sminl / smax) <= std::max(eps, hndrth * tol))
        {
            shift = zero;
        }
        else
        {
            R temp;
            if (idir == 1)
            {
                sll = std::abs(dia(ll));
                dlas2(dia(m - 1), eoff(m - 1), dia(m), shift, temp);
            }
            else
            {
                sll = std::abs(dia(m));
                dlas2(dia(ll), eoff(ll), dia(ll + 1), shift, temp);
            }
            if (sll > zero)
            {
                if ((shift / sll) * (shift / sll) < eps)
                {
                    shift = zero;
                }
            }
        }

        iter = iter + m - ll;

        // ---- QR sweep ----
        if (shift == zero)
        {
            if (idir == 1)
            {
                // Chase bulge top -> bottom, zero shift.
                cs = one;
                oldcs = one;
                for (int i = ll; i <= m - 1; ++i)
                {
                    dlartg(dia(i) * cs, eoff(i), cs, sn, rr);
                    if (i > ll)
                    {
                        eoff(i - 1) = oldsn * rr;
                    }
                    dlartg(oldcs * rr, dia(i + 1) * sn, oldcs, oldsn, dia(i));
                    wrk(i - ll + 1) = cs;
                    wrk(i - ll + 1 + nm1) = sn;
                    wrk(i - ll + 1 + nm12) = oldcs;
                    wrk(i - ll + 1 + nm13) = oldsn;
                }
                R h = dia(m) * cs;
                dia(m) = h * oldcs;
                eoff(m - 1) = h * oldsn;
                if (ncvt > 0)
                {
                    dlasr_lv(true, m - ll + 1, ncvt, &wrk(1), &wrk(n), &vtm(ll, 1), ldvt);
                }
                if (nru > 0)
                {
                    dlasr_rv(true, nru, m - ll + 1, &wrk(nm12 + 1), &wrk(nm13 + 1), &um(1, ll), ldu);
                }
                if (ncc > 0)
                {
                    dlasr_lv(true, m - ll + 1, ncc, &wrk(nm12 + 1), &wrk(nm13 + 1), &cm(ll, 1), ldc);
                }
                if (std::abs(eoff(m - 1)) <= thresh)
                {
                    eoff(m - 1) = zero;
                }
            }
            else
            {
                // Chase bulge bottom -> top, zero shift.
                cs = one;
                oldcs = one;
                for (int i = m; i >= ll + 1; --i)
                {
                    dlartg(dia(i) * cs, eoff(i - 1), cs, sn, rr);
                    if (i < m)
                    {
                        eoff(i) = oldsn * rr;
                    }
                    dlartg(oldcs * rr, dia(i - 1) * sn, oldcs, oldsn, dia(i));
                    wrk(i - ll) = cs;
                    wrk(i - ll + nm1) = -sn;
                    wrk(i - ll + nm12) = oldcs;
                    wrk(i - ll + nm13) = -oldsn;
                }
                R h = dia(ll) * cs;
                dia(ll) = h * oldcs;
                eoff(ll) = h * oldsn;
                if (ncvt > 0)
                {
                    dlasr_lv(false, m - ll + 1, ncvt, &wrk(nm12 + 1), &wrk(nm13 + 1), &vtm(ll, 1), ldvt);
                }
                if (nru > 0)
                {
                    dlasr_rv(false, nru, m - ll + 1, &wrk(1), &wrk(n), &um(1, ll), ldu);
                }
                if (ncc > 0)
                {
                    dlasr_lv(false, m - ll + 1, ncc, &wrk(1), &wrk(n), &cm(ll, 1), ldc);
                }
                if (std::abs(eoff(ll)) <= thresh)
                {
                    eoff(ll) = zero;
                }
            }
        }
        else
        {
            if (idir == 1)
            {
                // Nonzero shift, top -> bottom.
                R f = (std::abs(dia(ll)) - shift) * (fsign(one, dia(ll)) + shift / dia(ll));
                R g = eoff(ll);
                R cosr;
                R sinr;
                R cosl;
                R sinl;
                for (int i = ll; i <= m - 1; ++i)
                {
                    dlartg(f, g, cosr, sinr, rr);
                    if (i > ll)
                    {
                        eoff(i - 1) = rr;
                    }
                    f = cosr * dia(i) + sinr * eoff(i);
                    eoff(i) = cosr * eoff(i) - sinr * dia(i);
                    g = sinr * dia(i + 1);
                    dia(i + 1) = cosr * dia(i + 1);
                    dlartg(f, g, cosl, sinl, rr);
                    dia(i) = rr;
                    f = cosl * eoff(i) + sinl * dia(i + 1);
                    dia(i + 1) = cosl * dia(i + 1) - sinl * eoff(i);
                    if (i < m - 1)
                    {
                        g = sinl * eoff(i + 1);
                        eoff(i + 1) = cosl * eoff(i + 1);
                    }
                    wrk(i - ll + 1) = cosr;
                    wrk(i - ll + 1 + nm1) = sinr;
                    wrk(i - ll + 1 + nm12) = cosl;
                    wrk(i - ll + 1 + nm13) = sinl;
                }
                eoff(m - 1) = f;
                if (ncvt > 0)
                {
                    dlasr_lv(true, m - ll + 1, ncvt, &wrk(1), &wrk(n), &vtm(ll, 1), ldvt);
                }
                if (nru > 0)
                {
                    dlasr_rv(true, nru, m - ll + 1, &wrk(nm12 + 1), &wrk(nm13 + 1), &um(1, ll), ldu);
                }
                if (ncc > 0)
                {
                    dlasr_lv(true, m - ll + 1, ncc, &wrk(nm12 + 1), &wrk(nm13 + 1), &cm(ll, 1), ldc);
                }
                if (std::abs(eoff(m - 1)) <= thresh)
                {
                    eoff(m - 1) = zero;
                }
            }
            else
            {
                // Nonzero shift, bottom -> top.
                R f = (std::abs(dia(m)) - shift) * (fsign(one, dia(m)) + shift / dia(m));
                R g = eoff(m - 1);
                R cosr;
                R sinr;
                R cosl;
                R sinl;
                for (int i = m; i >= ll + 1; --i)
                {
                    dlartg(f, g, cosr, sinr, rr);
                    if (i < m)
                    {
                        eoff(i) = rr;
                    }
                    f = cosr * dia(i) + sinr * eoff(i - 1);
                    eoff(i - 1) = cosr * eoff(i - 1) - sinr * dia(i);
                    g = sinr * dia(i - 1);
                    dia(i - 1) = cosr * dia(i - 1);
                    dlartg(f, g, cosl, sinl, rr);
                    dia(i) = rr;
                    f = cosl * eoff(i - 1) + sinl * dia(i - 1);
                    dia(i - 1) = cosl * dia(i - 1) - sinl * eoff(i - 1);
                    if (i > ll + 1)
                    {
                        g = sinl * eoff(i - 2);
                        eoff(i - 2) = cosl * eoff(i - 2);
                    }
                    wrk(i - ll) = cosr;
                    wrk(i - ll + nm1) = -sinr;
                    wrk(i - ll + nm12) = cosl;
                    wrk(i - ll + nm13) = -sinl;
                }
                eoff(ll) = f;
                if (std::abs(eoff(ll)) <= thresh)
                {
                    eoff(ll) = zero;
                }
                if (ncvt > 0)
                {
                    dlasr_lv(false, m - ll + 1, ncvt, &wrk(nm12 + 1), &wrk(nm13 + 1), &vtm(ll, 1), ldvt);
                }
                if (nru > 0)
                {
                    dlasr_rv(false, nru, m - ll + 1, &wrk(1), &wrk(n), &um(1, ll), ldu);
                }
                if (ncc > 0)
                {
                    dlasr_lv(false, m - ll + 1, ncc, &wrk(1), &wrk(n), &cm(ll, 1), ldc);
                }
            }
        }
        // GO TO 60 (loop)
    }

    if (maxit_exceeded)
    {
        int info = 0;
        for (int i = 1; i <= n - 1; ++i)
        {
            if (eoff(i) != zero)
            {
                info = info + 1;
            }
        }
        return info;
    }

    // ---- label 160: make singular values positive, sort descending ----
    for (int i = 1; i <= n; ++i)
    {
        if (dia(i) < zero)
        {
            dia(i) = -dia(i);
            if (ncvt > 0)
            {
                for (int k = 1; k <= ncvt; ++k)
                {
                    vtm(i, k) = negone * vtm(i, k);
                }
            }
        }
    }
    for (int i = 1; i <= n - 1; ++i)
    {
        int isub = 1;
        R smin = dia(1);
        for (int j = 2; j <= n + 1 - i; ++j)
        {
            if (dia(j) <= smin)
            {
                isub = j;
                smin = dia(j);
            }
        }
        if (isub != n + 1 - i)
        {
            dia(isub) = dia(n + 1 - i);
            dia(n + 1 - i) = smin;
            if (ncvt > 0)
            {
                for (int k = 1; k <= ncvt; ++k)
                {
                    std::swap(vtm(isub, k), vtm(n + 1 - i, k));
                }
            }
            if (nru > 0)
            {
                for (int k = 1; k <= nru; ++k)
                {
                    std::swap(um(k, isub), um(k, n + 1 - i));
                }
            }
            if (ncc > 0)
            {
                for (int k = 1; k <= ncc; ++k)
                {
                    std::swap(cm(isub, k), cm(n + 1 - i, k));
                }
            }
        }
    }
    return 0;
}

} // namespace crd::hesap::dense::detail
