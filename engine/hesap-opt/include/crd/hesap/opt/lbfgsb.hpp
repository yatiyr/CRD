#pragma once

// lbfgsb.hpp — Phase 3.1.6 v7-d-3: L-BFGS-B (bound-constrained L-BFGS, Byrd-Lu-Nocedal-Zhu 1995). A FAITHFUL port
// of the Zhu-Byrd-Lu-Nocedal reference (the exact code scipy.optimize.minimize(method="L-BFGS-B") wraps), via
// Stephen Becker's C translation — routine-for-routine (the dcstep/UMFPACK/CHOLMOD "read the source" discipline),
// because the generalized Cauchy point is the most error-prone piece in the optimization spine and re-deriving it
// from the paper is the bug farm (advisor-pinned). ADR-0090.
//
// HONEST VERDICT TARGET (advisor): L-BFGS-B has real implementation latitude (breakpoint handling, subspace solver,
// bound-projection ties) ⇒ Cerid will NOT bit-match scipy's iteration count and must not chase 1.00×. The honest
// claim is the SAME constrained minimizer + comparable iterations + the {1..16} determinism moat scipy lacks.
//
// FAITHFUL-PORT CONVENTIONS (to keep index math identical to the source, the only safe way to port a GCP):
//   • 1-based indexing throughout (matching the Fortran/f2c source line-for-line). Every work array is
//     OVER-ALLOCATED so the 1-based + column-major access `a[i + j*lda]` lands inside the allocation (ASan-clean —
//     no pointer-before-array UB). The driver sizes them; the kernels index exactly as the source.
//   • 2-D arrays are column-major: an (lda × cols) matrix is `a[i + j*lda]` for i∈[1,lda], j∈[1,cols] ⇒ accessed
//     indices ∈ [lda+1, lda*(cols+1)] ⇒ allocate lda*(cols+1)+1.
//   • Reals are raw T (f32/f64), the lower-layer numerical convention (ADR-0078 §5); integer/flag arrays are int.
//   • Local 1-based BLAS (lb_ddot/lb_dcopy/lb_daxpy/lb_dscal) keep the source's (n, x, incx, …) calls verbatim.
//
// ✅ STATUS — ALL SUBROUTINES DIFFERENTIALLY VERIFIED (driver pending). Every routine below is bit-verified vs the
// reference C (stephenbeckr/L-BFGS-B-C) by `runtime/examples/lbfgsb_difftest.cpp` (54 checks, 0 fail) on identical
// inputs — REALS AND INTEGER ARRAYS — in the bug-HIDING regimes (dtrsl all 4 jobs, matupd iupdat>m ring-wrap,
// hpsolb ties, cauchy breakpoint-rich, formk iupdat>m + inner Cholesky, subsm projection+backtracking). That
// harness adjudicated the 4 off-by-ones the manual audit had found in the "easy" routines (manual audit ≠
// verification — the harness is the oracle). PORTED + VERIFIED: dpofa · dtrsl · active · bmv · cmprlb · formt ·
// freev · hpsolb · matupd · projgr · cauchy (GCP) · formk · subsm. STILL TO PORT: mainlb (the driver — a
// reverse-communication state machine ⇒ a RESTRUCTURE into a direct-call loop, not a verbatim copy) + lnsrlb (the
// bounded line search via dcsrch). Then the public `minimize_lbfgsb<T>` driver → wire into opt.hpp/CMake → scipy
// L-BFGS-B head-to-head (same minimizer + comparable iters, NOT 1.00×) + {1..16} active-bound moat + the advisor
// GCP review + 6-config close. Header UNWIRED from opt.hpp / the build until then; tree green.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/more_thuente_line_search.hpp> // detail::mt_dcstep (verified dcstep) for lnsrlb
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/memory/allocator.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace crd::hesap::opt::detail::lbfgsb
{

// ---- 1-based BLAS helpers (verbatim semantics of the source's ddot/dcopy/daxpy/dscal calls) -----------------

template <typename T>
[[nodiscard]] inline T lb_ddot(int n, const T* x, int incx, const T* y, int incy) noexcept
{
    // x, y are 1-based pointers (caller passes &a[base]); accumulate in source order.
    T sum = static_cast<T>(0);
    int ix = 1;
    int iy = 1;
    for (int i = 1; i <= n; ++i)
    {
        sum += x[ix] * y[iy];
        ix += incx;
        iy += incy;
    }
    return sum;
}

template <typename T>
inline void lb_dcopy(int n, const T* x, int incx, T* y, int incy) noexcept
{
    int ix = 1;
    int iy = 1;
    for (int i = 1; i <= n; ++i)
    {
        y[iy] = x[ix];
        ix += incx;
        iy += incy;
    }
}

template <typename T>
inline void lb_daxpy(int n, T a, const T* x, int incx, T* y, int incy) noexcept
{
    if (a == static_cast<T>(0))
    {
        return;
    }
    int ix = 1;
    int iy = 1;
    for (int i = 1; i <= n; ++i)
    {
        y[iy] += a * x[ix];
        ix += incx;
        iy += incy;
    }
}

template <typename T>
inline void lb_dscal(int n, T a, T* x, int incx) noexcept
{
    int ix = 1;
    for (int i = 1; i <= n; ++i)
    {
        x[ix] *= a;
        ix += incx;
    }
}

// ---- LINPACK dpofa: Cholesky R'R of an SPD matrix, upper triangle; info=0 ok, else leading-minor order ------
// `a` is column-major, 1-based, offset so a[i + j*lda] is valid for i,j∈[1,n]. Caller over-allocates.
template <typename T>
inline void dpofa(T* a, int lda, int n, int& info) noexcept
{
    for (int j = 1; j <= n; ++j)
    {
        info = j;
        T s = static_cast<T>(0);
        const int jm1 = j - 1;
        if (jm1 >= 1)
        {
            for (int k = 1; k <= jm1; ++k)
            {
                T t = a[k + j * lda] - lb_ddot<T>(k - 1, &a[k * lda], 1, &a[j * lda], 1);
                t /= a[k + k * lda];
                a[k + j * lda] = t;
                s += t * t;
            }
        }
        s = a[j + j * lda] - s;
        if (s <= static_cast<T>(0))
        {
            return; // info = j (not positive definite)
        }
        a[j + j * lda] = std::sqrt(s);
    }
    info = 0;
}

// ---- LINPACK dtrsl: solve a triangular system. job: 00 T·x=b (lower), 01 T·x=b (upper),
//      10 Tᵀ·x=b (lower), 11 Tᵀ·x=b (upper). info=0 ok, else zero-diagonal index. ----------------------------
template <typename T>
inline void dtrsl(const T* t, int ldt, int n, T* b, int job, int& info) noexcept
{
    for (info = 1; info <= n; ++info)
    {
        if (t[info + info * ldt] == static_cast<T>(0))
        {
            return; // singular
        }
    }
    info = 0;
    int solve_case = 1;
    if (job % 10 != 0)
    {
        solve_case = 2;
    }
    if (job % 100 / 10 != 0)
    {
        solve_case += 2;
    }
    if (solve_case == 1) // solve T·x = b, T lower triangular
    {
        b[1] /= t[1 + 1 * ldt];
        for (int j = 2; j <= n; ++j)
        {
            const T temp = -b[j - 1];
            lb_daxpy<T>(n - j + 1, temp, &t[j + (j - 1) * ldt - 1], 1, &b[j - 1], 1);
            b[j] /= t[j + j * ldt];
        }
    }
    else if (solve_case == 2) // solve T·x = b, T upper triangular
    {
        b[n] /= t[n + n * ldt];
        for (int jj = 2; jj <= n; ++jj)
        {
            const int j = n - jj + 1;
            const T   temp = -b[j + 1];
            lb_daxpy<T>(j, temp, &t[(j + 1) * ldt], 1, &b[0], 1);
            b[j] /= t[j + j * ldt];
        }
    }
    else if (solve_case == 3) // solve Tᵀ·x = b, T lower triangular
    {
        b[n] /= t[n + n * ldt];
        for (int jj = 2; jj <= n; ++jj)
        {
            const int j = n - jj + 1;
            b[j] -= lb_ddot<T>(jj - 1, &t[j + j * ldt], 1, &b[j], 1);
            b[j] /= t[j + j * ldt];
        }
    }
    else // case_ == 4: solve Tᵀ·x = b, T upper triangular
    {
        b[1] /= t[1 + 1 * ldt];
        for (int j = 2; j <= n; ++j)
        {
            b[j] -= lb_ddot<T>(j - 1, &t[j * ldt], 1, &b[0], 1);
            b[j] /= t[j + j * ldt];
        }
    }
}

// ---- active: initialize iwhere + project x0 onto the box. iwhere[i] = -1 free, 3 fixed (l==u), 0 otherwise. ---
template <typename T>
inline void active(int n, const T* l, const T* u, const int* nbd, T* x, int* iwhere, bool& prjctd, bool& cnstnd,
                   bool& boxed) noexcept
{
    prjctd = false;
    cnstnd = false;
    boxed = true;
    for (int i = 1; i <= n; ++i)
    {
        if (nbd[i] > 0)
        {
            if (nbd[i] <= 2 && x[i] <= l[i])
            {
                if (x[i] < l[i])
                {
                    prjctd = true;
                    x[i] = l[i];
                }
            }
            else if (nbd[i] >= 2 && x[i] >= u[i])
            {
                if (x[i] > u[i])
                {
                    prjctd = true;
                    x[i] = u[i];
                }
            }
        }
    }
    for (int i = 1; i <= n; ++i)
    {
        if (nbd[i] != 2)
        {
            boxed = false;
        }
        if (nbd[i] == 0)
        {
            iwhere[i] = -1;
        }
        else
        {
            cnstnd = true;
            if (nbd[i] == 2 && u[i] - l[i] <= static_cast<T>(0))
            {
                iwhere[i] = 3;
            }
            else
            {
                iwhere[i] = 0;
            }
        }
    }
}

// ---- bmv: product of the 2m×2m middle matrix M of the compact L-BFGS B with a 2·col vector v → p -------------
// sy, wt are m×m column-major 1-based (offset). v, p are 1-based length 2·col. info=0 ok, else dtrsl singular.
template <typename T>
inline void bmv(int m, const T* sy, const T* wt, int col, const T* v, T* p, int& info) noexcept
{
    if (col == 0)
    {
        return;
    }
    // PART I: solve J·p2 = v2 + L·D^{-1}·v1.
    p[col + 1] = v[col + 1];
    for (int i = 2; i <= col; ++i)
    {
        const int i2 = col + i;
        T         sum = static_cast<T>(0);
        for (int k = 1; k <= i - 1; ++k)
        {
            sum += sy[i + k * m] * v[k] / sy[k + k * m];
        }
        p[i2] = v[i2] + sum;
    }
    // dtrsl on wt with the p2 block (&p[col+1] is a 1-based pointer into p starting at col+1).
    dtrsl<T>(wt, m, col, &p[col], /*job=*/11, info);
    if (info != 0)
    {
        return;
    }
    // solve D^{1/2}·p1 = v1.
    for (int i = 1; i <= col; ++i)
    {
        p[i] = v[i] / std::sqrt(sy[i + i * m]);
    }
    // PART II: solve Jᵀ·p2 = p2.
    dtrsl<T>(wt, m, col, &p[col], /*job=*/1, info);
    if (info != 0)
    {
        return;
    }
    // p1 = -D^{-1/2}·p1 + D^{-1}·Lᵀ·p2.
    for (int i = 1; i <= col; ++i)
    {
        p[i] = -p[i] / std::sqrt(sy[i + i * m]);
    }
    for (int i = 1; i <= col; ++i)
    {
        T sum = static_cast<T>(0);
        for (int k = i + 1; k <= col; ++k)
        {
            sum += sy[k + i * m] * p[col + k] / sy[i + i * m];
        }
        p[i] += sum;
    }
}

// ---- cmprlb: compute r = -Z'(B(xcp-x) + g), the reduced gradient of the quadratic model at the GCP ----------
template <typename T>
inline void cmprlb(int n, int m, const T* x, const T* g, const T* ws, const T* wy, const T* sy, const T* wt,
                   const T* z, T* r, T* wa, const int* index, T theta, int col, int head, int nfree, bool cnstnd,
                   int& info) noexcept
{
    if (!cnstnd && col > 0)
    {
        for (int i = 1; i <= n; ++i)
        {
            r[i] = -g[i];
        }
    }
    else
    {
        for (int i = 1; i <= nfree; ++i)
        {
            const int k = index[i];
            r[i] = -theta * (z[k] - x[k]) - g[k];
        }
        bmv<T>(m, sy, wt, col, &wa[2 * m], &wa[0], info); // wa[(2m)+1..] in, wa[1..] out (1-based)
        if (info != 0)
        {
            info = -8;
            return;
        }
        int pointr = head;
        for (int j = 1; j <= col; ++j)
        {
            const T a1 = wa[j];
            const T a2 = theta * wa[col + j];
            for (int i = 1; i <= nfree; ++i)
            {
                const int k = index[i];
                r[i] = r[i] + wy[k + pointr * n] * a1 + ws[k + pointr * n] * a2;
            }
            pointr = pointr % m + 1;
        }
    }
}

// ---- formt: form & Cholesky-factor the m×m middle matrix T = θ·S'S + L·D^{-1}·L' (upper, into wt). -----------
template <typename T>
inline void formt(int m, T* wt, const T* sy, const T* ss, int col, T theta, int& info) noexcept
{
    for (int j = 1; j <= col; ++j)
    {
        wt[1 + j * m] = theta * ss[1 + j * m];
    }
    for (int i = 2; i <= col; ++i)
    {
        for (int j = i; j <= col; ++j)
        {
            const int k1 = (i < j ? i : j) - 1;
            T         ddum = static_cast<T>(0);
            for (int k = 1; k <= k1; ++k)
            {
                ddum += sy[i + k * m] * sy[j + k * m] / sy[k + k * m];
            }
            wt[i + j * m] = ddum + theta * ss[i + j * m];
        }
    }
    dpofa<T>(wt, m, col, info);
    if (info != 0)
    {
        info = -3;
    }
}

// ---- freev: partition variables into free (index[1..nfree]) and active (index[nfree+1..n]) at the GCP. -------
template <typename T = double>
inline void freev(int n, int& nfree, int* index, int& nenter, int& ileave, int* indx2, const int* iwhere, bool& wrk,
                  bool updatd, bool cnstnd, int iter) noexcept
{
    nenter = 0;
    ileave = n + 1;
    if (iter > 0 && cnstnd)
    {
        for (int i = 1; i <= nfree; ++i)
        {
            const int k = index[i];
            if (iwhere[k] > 0)
            {
                --ileave;
                indx2[ileave] = k;
            }
        }
        for (int i = nfree + 1; i <= n; ++i)
        {
            const int k = index[i];
            if (iwhere[k] <= 0)
            {
                ++nenter;
                indx2[nenter] = k;
            }
        }
    }
    wrk = (ileave < n + 1) || (nenter > 0) || updatd;
    nfree = 0;
    int iact = n + 1;
    for (int i = 1; i <= n; ++i)
    {
        if (iwhere[i] <= 0)
        {
            ++nfree;
            index[nfree] = i;
        }
        else
        {
            --iact;
            index[iact] = i;
        }
    }
}

// ---- hpsolb: heap-sort t[1..n] ascending, carrying iorder; iheap=0 builds the heap, else sift the new last. ---
template <typename T>
inline void hpsolb(int n, T* t, int* iorder, int iheap) noexcept
{
    if (iheap == 0)
    {
        for (int k = 2; k <= n; ++k)
        {
            const T   ddum = t[k];
            const int indxin = iorder[k];
            int       i = k;
            while (i > 1)
            {
                const int j = i / 2;
                if (ddum < t[j])
                {
                    t[i] = t[j];
                    iorder[i] = iorder[j];
                    i = j;
                }
                else
                {
                    break;
                }
            }
            t[i] = ddum;
            iorder[i] = indxin;
        }
    }
    if (n > 1)
    {
        int       i = 1;
        const T   out = t[1];
        const int indxou = iorder[1];
        const T   ddum = t[n];
        const int indxin = iorder[n];
        while (true)
        {
            int j = i + i;
            if (j <= n - 1)
            {
                if (t[j + 1] < t[j])
                {
                    ++j;
                }
                if (t[j] < ddum)
                {
                    t[i] = t[j];
                    iorder[i] = iorder[j];
                    i = j;
                    continue;
                }
            }
            break;
        }
        t[i] = ddum;
        iorder[i] = indxin;
        t[n] = out;
        iorder[n] = indxou;
    }
}

// ---- matupd: roll the L-BFGS (s,y) pairs into ws/wy and update the SY / SS / θ compact-rep data. -------------
template <typename T>
inline void matupd(int n, int m, T* ws, T* wy, T* sy, T* ss, const T* d, const T* r, int& itail, int iupdat,
                   int& col, int& head, T& theta, T rr, T dr, T stp, T dtd) noexcept
{
    if (iupdat <= m)
    {
        col = iupdat;
        itail = (head + iupdat - 2) % m + 1;
    }
    else
    {
        itail = itail % m + 1;
        head = head % m + 1;
    }
    lb_dcopy<T>(n, &d[0], 1, &ws[itail * n], 1); // &ws[itail*n + 1] base ⇒ pass &ws[itail*n] (1-based)
    lb_dcopy<T>(n, &r[0], 1, &wy[itail * n], 1);
    theta = rr / dr;
    if (iupdat > m)
    {
        for (int j = 1; j <= col - 1; ++j)
        {
            lb_dcopy<T>(j, &ss[(j + 1) * m + 1], 1, &ss[j * m], 1);
            lb_dcopy<T>(col - j, &sy[j + (j + 1) * m], 1, &sy[j - 1 + j * m], 1);
        }
    }
    int pointr = head;
    for (int j = 1; j <= col - 1; ++j)
    {
        sy[col + j * m] = lb_ddot<T>(n, &d[0], 1, &wy[pointr * n], 1);
        ss[j + col * m] = lb_ddot<T>(n, &ws[pointr * n], 1, &d[0], 1);
        pointr = pointr % m + 1;
    }
    if (stp == static_cast<T>(1))
    {
        ss[col + col * m] = dtd;
    }
    else
    {
        ss[col + col * m] = stp * stp * dtd;
    }
    sy[col + col * m] = dr;
}

// ---- projgr: ‖projected gradient‖∞ — the first-order optimality measure for the bound-constrained problem. ---
template <typename T>
inline void projgr(int n, const T* l, const T* u, const int* nbd, const T* x, const T* g, T& sbgnrm) noexcept
{
    sbgnrm = static_cast<T>(0);
    for (int i = 1; i <= n; ++i)
    {
        T gi = g[i];
        if (nbd[i] != 0)
        {
            if (gi < static_cast<T>(0))
            {
                if (nbd[i] >= 2)
                {
                    const T d1 = x[i] - u[i];
                    gi = d1 > gi ? d1 : gi;
                }
            }
            else
            {
                if (nbd[i] <= 2)
                {
                    const T d1 = x[i] - l[i];
                    gi = d1 < gi ? d1 : gi;
                }
            }
        }
        const T ag = std::fabs(gi);
        sbgnrm = ag > sbgnrm ? ag : sbgnrm;
    }
}

// ---- cauchy: the GENERALIZED CAUCHY POINT (GCP) — the heart of L-BFGS-B and the most error-prone routine. Walks
//      the projected steepest-descent path across the box's breakpoints, fixing variables as it hits bounds, and
//      locates the first local minimizer of the quadratic model. Verified bit-for-bit vs the reference C. ----
template <typename T>
inline void cauchy(int n, const T* x, const T* l, const T* u, const int* nbd, const T* g, int* iorder, int* iwhere,
                   T* t, T* d, T* xcp, int m, const T* wy, const T* ws, const T* sy, const T* wt, T theta, int col,
                   int head, T* p, T* c, T* wbp, T* v, int& nseg, T sbgnrm, int& info, T epsmch) noexcept
{
    if (sbgnrm <= static_cast<T>(0))
    {
        lb_dcopy<T>(n, &x[0], 1, &xcp[0], 1);
        return;
    }
    bool      bnded = true;
    int       nfree = n + 1;
    int       nbreak = 0;
    int       ibkmin = 0;
    T         bkmin = static_cast<T>(0);
    const int col2 = 2 * col;
    T         f1 = static_cast<T>(0);
    for (int i = 1; i <= col2; ++i)
    {
        p[i] = static_cast<T>(0);
    }
    // Determine each variable's bound status + breakpoint; build p; track the smallest breakpoint.
    for (int i = 1; i <= n; ++i)
    {
        const T neggi = -g[i];
        T       tl = static_cast<T>(0);
        T       tu = static_cast<T>(0);
        if (iwhere[i] != 3 && iwhere[i] != -1)
        {
            if (nbd[i] <= 2)
            {
                tl = x[i] - l[i];
            }
            if (nbd[i] >= 2)
            {
                tu = u[i] - x[i];
            }
            const bool xlower = nbd[i] <= 2 && tl <= static_cast<T>(0);
            const bool xupper = nbd[i] >= 2 && tu <= static_cast<T>(0);
            iwhere[i] = 0;
            if (xlower)
            {
                if (neggi <= static_cast<T>(0))
                {
                    iwhere[i] = 1;
                }
            }
            else if (xupper)
            {
                if (neggi >= static_cast<T>(0))
                {
                    iwhere[i] = 2;
                }
            }
            else
            {
                if (std::fabs(neggi) <= static_cast<T>(0))
                {
                    iwhere[i] = -3;
                }
            }
        }
        int pointr = head;
        if (iwhere[i] != 0 && iwhere[i] != -1)
        {
            d[i] = static_cast<T>(0);
        }
        else
        {
            d[i] = neggi;
            f1 -= neggi * neggi;
            for (int j = 1; j <= col; ++j)
            {
                p[j] += wy[i + pointr * n] * neggi;
                p[col + j] += ws[i + pointr * n] * neggi;
                pointr = pointr % m + 1;
            }
            if (nbd[i] <= 2 && nbd[i] != 0 && neggi < static_cast<T>(0))
            {
                ++nbreak;
                iorder[nbreak] = i;
                t[nbreak] = tl / (-neggi);
                if (nbreak == 1 || t[nbreak] < bkmin)
                {
                    bkmin = t[nbreak];
                    ibkmin = nbreak;
                }
            }
            else if (nbd[i] >= 2 && neggi > static_cast<T>(0))
            {
                ++nbreak;
                iorder[nbreak] = i;
                t[nbreak] = tu / neggi;
                if (nbreak == 1 || t[nbreak] < bkmin)
                {
                    bkmin = t[nbreak];
                    ibkmin = nbreak;
                }
            }
            else
            {
                --nfree;
                iorder[nfree] = i;
                if (std::fabs(neggi) > static_cast<T>(0))
                {
                    bnded = false;
                }
            }
        }
    }
    if (theta != static_cast<T>(1))
    {
        lb_dscal<T>(col, theta, &p[col], 1);
    }
    lb_dcopy<T>(n, &x[0], 1, &xcp[0], 1);
    if (nbreak == 0 && nfree == n + 1)
    {
        return; // d is the zero vector ⇒ GCP = x
    }
    for (int j = 1; j <= col2; ++j)
    {
        c[j] = static_cast<T>(0);
    }
    T       f2 = -theta * f1;
    const T f2_org = f2;
    if (col > 0)
    {
        bmv<T>(m, sy, wt, col, &p[0], &v[0], info);
        if (info != 0)
        {
            return;
        }
        f2 -= lb_ddot<T>(col2, &v[0], 1, &p[0], 1);
    }
    T dtm = -f1 / f2;
    T tsum = static_cast<T>(0);
    nseg = 1;
    bool goto_999 = false;
    if (nbreak != 0)
    {
        int nleft = nbreak;
        int iter = 1;
        T   tj = static_cast<T>(0);
        for (;;) // walk the breakpoints in increasing order
        {
            const T tj0 = tj;
            int     ibp;
            if (iter == 1)
            {
                tj = bkmin; // smallest breakpoint already known — skip the heapsort
                ibp = iorder[ibkmin];
            }
            else
            {
                if (iter == 2)
                {
                    if (ibkmin != nbreak)
                    {
                        t[ibkmin] = t[nbreak];
                        iorder[ibkmin] = iorder[nbreak];
                    }
                }
                hpsolb<T>(nleft, &t[0], &iorder[0], iter - 2);
                tj = t[nleft];
                ibp = iorder[nleft];
            }
            const T dt = tj - tj0;
            if (dtm < dt)
            {
                break; // the minimizer is within this interval (L888)
            }
            tsum += dt;
            --nleft;
            ++iter;
            const T dibp = d[ibp];
            d[ibp] = static_cast<T>(0);
            T zibp;
            if (dibp > static_cast<T>(0))
            {
                zibp = u[ibp] - x[ibp];
                xcp[ibp] = u[ibp];
                iwhere[ibp] = 2;
            }
            else
            {
                zibp = l[ibp] - x[ibp];
                xcp[ibp] = l[ibp];
                iwhere[ibp] = 1;
            }
            if (nleft == 0 && nbreak == n)
            {
                dtm = dt; // all variables fixed ⇒ xcp is the GCP (L999)
                goto_999 = true;
                break;
            }
            ++nseg;
            const T dibp2 = dibp * dibp;
            f1 = f1 + dt * f2 + dibp2 - theta * dibp * zibp;
            f2 -= theta * dibp2;
            if (col > 0)
            {
                lb_daxpy<T>(col2, dt, &p[0], 1, &c[0], 1);
                int pointr = head;
                for (int j = 1; j <= col; ++j)
                {
                    wbp[j] = wy[ibp + pointr * n];
                    wbp[col + j] = theta * ws[ibp + pointr * n];
                    pointr = pointr % m + 1;
                }
                bmv<T>(m, sy, wt, col, &wbp[0], &v[0], info);
                if (info != 0)
                {
                    return;
                }
                const T wmc = lb_ddot<T>(col2, &c[0], 1, &v[0], 1);
                const T wmp = lb_ddot<T>(col2, &p[0], 1, &v[0], 1);
                const T wmw = lb_ddot<T>(col2, &wbp[0], 1, &v[0], 1);
                lb_daxpy<T>(col2, -dibp, &wbp[0], 1, &p[0], 1);
                f1 += dibp * wmc;
                f2 = f2 + dibp * static_cast<T>(2) * wmp - dibp2 * wmw;
            }
            const T lim = epsmch * f2_org;
            f2 = lim > f2 ? lim : f2;
            if (nleft > 0)
            {
                dtm = -f1 / f2;
                continue;
            }
            if (bnded)
            {
                f1 = static_cast<T>(0);
                f2 = static_cast<T>(0);
                dtm = static_cast<T>(0);
            }
            else
            {
                dtm = -f1 / f2;
            }
            break;
        }
    }
    if (!goto_999)
    {
        if (dtm <= static_cast<T>(0))
        {
            dtm = static_cast<T>(0);
        }
        tsum += dtm;
        lb_daxpy<T>(n, tsum, &d[0], 1, &xcp[0], 1); // move the still-free variables
    }
    if (col > 0)
    {
        lb_daxpy<T>(col2, dtm, &p[0], 1, &c[0], 1); // c = W'(x^c − x), used by cmprlb
    }
}

// ---- formk: form & Cholesky-factor the 2col×2col middle matrix K of the subspace Newton system, updating the
//      running wn1 incrementally (entering/leaving free variables). The compact-representation linear algebra.
//      Verified bit-for-bit vs the reference C. ----
template <typename T>
inline void formk(int n, int nsub, const int* ind, int nenter, int ileave, const int* indx2, int iupdat,
                  bool updatd, T* wn, T* wn1, int m, const T* ws, const T* wy, const T* sy, T theta, int col,
                  int head, int& info) noexcept
{
    const int m2 = 2 * m;
    int       upcl;
    if (updatd)
    {
        if (iupdat > m)
        {
            // shift old wn1 (the [1:m-1,1:m-1] / [m+1:2m-1] / cross blocks) up by one.
            for (int jy = 1; jy <= m - 1; ++jy)
            {
                const int js = m + jy;
                lb_dcopy<T>(m - jy, &wn1[jy + (jy + 1) * m2], 1, &wn1[jy - 1 + jy * m2], 1);
                lb_dcopy<T>(m - jy, &wn1[js + (js + 1) * m2], 1, &wn1[js - 1 + js * m2], 1);
                lb_dcopy<T>(m - 1, &wn1[m + 1 + (jy + 1) * m2], 1, &wn1[m + jy * m2], 1);
            }
        }
        // put new rows in blocks (1,1), (2,1), (2,2).
        const int pbegin = 1;
        const int pend = nsub;
        const int dbegin = nsub + 1;
        const int dend = n;
        const int iy0 = col;
        const int is0 = m + col;
        int       ipntr = head + col - 1;
        if (ipntr > m)
        {
            ipntr -= m;
        }
        int jpntr = head;
        for (int jy = 1; jy <= col; ++jy)
        {
            const int js = m + jy;
            T temp1 = static_cast<T>(0);
            T temp2 = static_cast<T>(0);
            T temp3 = static_cast<T>(0);
            for (int k = pbegin; k <= pend; ++k)
            {
                const int k1 = ind[k];
                temp1 += wy[k1 + ipntr * n] * wy[k1 + jpntr * n];
            }
            for (int k = dbegin; k <= dend; ++k)
            {
                const int k1 = ind[k];
                temp2 += ws[k1 + ipntr * n] * ws[k1 + jpntr * n];
                temp3 += ws[k1 + ipntr * n] * wy[k1 + jpntr * n];
            }
            wn1[iy0 + jy * m2] = temp1;
            wn1[is0 + js * m2] = temp2;
            wn1[is0 + jy * m2] = temp3;
            jpntr = jpntr % m + 1;
        }
        jpntr = head + col - 1;
        if (jpntr > m)
        {
            jpntr -= m;
        }
        ipntr = head;
        for (int i = 1; i <= col; ++i)
        {
            const int is = m + i;
            T         temp3 = static_cast<T>(0);
            for (int k = pbegin; k <= pend; ++k)
            {
                const int k1 = ind[k];
                temp3 += ws[k1 + ipntr * n] * wy[k1 + jpntr * n];
            }
            ipntr = ipntr % m + 1;
            wn1[is + col * m2] = temp3;
        }
        upcl = col - 1;
    }
    else
    {
        upcl = col;
    }
    // modify the old parts in blocks (1,1) and (2,2) due to changes in the free-variable set.
    int ipntr = head;
    for (int iy = 1; iy <= upcl; ++iy)
    {
        const int is = m + iy;
        int       jpntr = head;
        for (int jy = 1; jy <= iy; ++jy)
        {
            const int js = m + jy;
            T temp1 = static_cast<T>(0);
            T temp2 = static_cast<T>(0);
            T temp3 = static_cast<T>(0);
            T temp4 = static_cast<T>(0);
            for (int k = 1; k <= nenter; ++k)
            {
                const int k1 = indx2[k];
                temp1 += wy[k1 + ipntr * n] * wy[k1 + jpntr * n];
                temp2 += ws[k1 + ipntr * n] * ws[k1 + jpntr * n];
            }
            for (int k = ileave; k <= n; ++k)
            {
                const int k1 = indx2[k];
                temp3 += wy[k1 + ipntr * n] * wy[k1 + jpntr * n];
                temp4 += ws[k1 + ipntr * n] * ws[k1 + jpntr * n];
            }
            wn1[iy + jy * m2] = wn1[iy + jy * m2] + temp1 - temp3;
            wn1[is + js * m2] = wn1[is + js * m2] - temp2 + temp4;
            jpntr = jpntr % m + 1;
        }
        ipntr = ipntr % m + 1;
    }
    // modify the old parts in block (2,1).
    ipntr = head;
    for (int is = m + 1; is <= m + upcl; ++is)
    {
        int jpntr = head;
        for (int jy = 1; jy <= upcl; ++jy)
        {
            T temp1 = static_cast<T>(0);
            T temp3 = static_cast<T>(0);
            for (int k = 1; k <= nenter; ++k)
            {
                const int k1 = indx2[k];
                temp1 += ws[k1 + ipntr * n] * wy[k1 + jpntr * n];
            }
            for (int k = ileave; k <= n; ++k)
            {
                const int k1 = indx2[k];
                temp3 += ws[k1 + ipntr * n] * wy[k1 + jpntr * n];
            }
            if (is <= jy + m)
            {
                wn1[is + jy * m2] = wn1[is + jy * m2] + temp1 - temp3;
            }
            else
            {
                wn1[is + jy * m2] = wn1[is + jy * m2] - temp1 + temp3;
            }
            jpntr = jpntr % m + 1;
        }
        ipntr = ipntr % m + 1;
    }
    // form the upper triangle of wn = [D+Y'ZZ'Y/theta   -L_a'+R_z' ] from wn1.
    for (int iy = 1; iy <= col; ++iy)
    {
        const int is = col + iy;
        const int is1 = m + iy;
        for (int jy = 1; jy <= iy; ++jy)
        {
            const int js = col + jy;
            const int js1 = m + jy;
            wn[jy + iy * m2] = wn1[iy + jy * m2] / theta;
            wn[js + is * m2] = wn1[is1 + js1 * m2] * theta;
        }
        for (int jy = 1; jy <= iy - 1; ++jy)
        {
            wn[jy + is * m2] = -wn1[is1 + jy * m2];
        }
        for (int jy = iy; jy <= col; ++jy)
        {
            wn[jy + is * m2] = wn1[is1 + jy * m2];
        }
        wn[iy + iy * m2] += sy[iy + iy * m];
    }
    // Cholesky factor the (1,1) block, then form & factor the Schur complement in the (2,2) block.
    dpofa<T>(wn, m2, col, info);
    if (info != 0)
    {
        info = -1;
        return;
    }
    const int col2 = 2 * col;
    for (int js = col + 1; js <= col2; ++js)
    {
        dtrsl<T>(wn, m2, col, &wn[js * m2], 11, info);
    }
    for (int is = col + 1; is <= col2; ++is)
    {
        for (int js = is; js <= col2; ++js)
        {
            wn[is + js * m2] += lb_ddot<T>(col, &wn[is * m2], 1, &wn[js * m2], 1);
        }
    }
    dpofa<T>(&wn[col + col * m2], m2, col, info);
    if (info != 0)
    {
        info = -2;
        return;
    }
}

// ---- subsm: minimize the quadratic model over the FREE variables (the subspace) given the GCP, by solving the
//      compact-rep Newton system K·wv = W'Zd, then project the Newton point back onto the box (with a backtracking
//      safeguard if the projection raises the model). Verified bit-for-bit vs the reference C. ----
template <typename T>
inline void subsm(int n, int m, int nsub, const int* ind, const T* l, const T* u, const int* nbd, T* x, T* d, T* xp,
                  const T* ws, const T* wy, T theta, const T* xx, const T* gg, int col, int head, int& iword, T* wv,
                  const T* wn, int& info) noexcept
{
    if (nsub <= 0)
    {
        return;
    }
    // wv = W'·Z·d.
    int pointr = head;
    for (int i = 1; i <= col; ++i)
    {
        T temp1 = static_cast<T>(0);
        T temp2 = static_cast<T>(0);
        for (int j = 1; j <= nsub; ++j)
        {
            const int k = ind[j];
            temp1 += wy[k + pointr * n] * d[j];
            temp2 += ws[k + pointr * n] * d[j];
        }
        wv[i] = temp1;
        wv[col + i] = theta * temp2;
        pointr = pointr % m + 1;
    }
    // wv := K^{-1}·wv via the two triangular solves of the factored K.
    const int m2 = 2 * m;
    const int col2 = 2 * col;
    dtrsl<T>(wn, m2, col2, &wv[0], 11, info);
    if (info != 0)
    {
        return;
    }
    for (int i = 1; i <= col; ++i)
    {
        wv[i] = -wv[i];
    }
    dtrsl<T>(wn, m2, col2, &wv[0], 1, info);
    if (info != 0)
    {
        return;
    }
    // d = (1/theta)·d + (1/theta²)·Z'·W·wv.
    pointr = head;
    for (int jy = 1; jy <= col; ++jy)
    {
        const int js = col + jy;
        for (int i = 1; i <= nsub; ++i)
        {
            const int k = ind[i];
            d[i] = d[i] + wy[k + pointr * n] * wv[jy] / theta + ws[k + pointr * n] * wv[js];
        }
        pointr = pointr % m + 1;
    }
    lb_dscal<T>(nsub, static_cast<T>(1) / theta, &d[0], 1);
    // try the projected Newton point.
    iword = 0;
    lb_dcopy<T>(n, &x[0], 1, &xp[0], 1);
    for (int i = 1; i <= nsub; ++i)
    {
        const int k = ind[i];
        const T   dk = d[i];
        const T   xk = x[k];
        if (nbd[k] != 0)
        {
            if (nbd[k] == 1)
            {
                const T cand = xk + dk;
                x[k] = l[k] > cand ? l[k] : cand;
                if (x[k] == l[k])
                {
                    iword = 1;
                }
            }
            else if (nbd[k] == 2)
            {
                const T cand = xk + dk;
                const T xk2 = l[k] > cand ? l[k] : cand;
                x[k] = u[k] < xk2 ? u[k] : xk2;
                if (x[k] == l[k] || x[k] == u[k])
                {
                    iword = 1;
                }
            }
            else if (nbd[k] == 3)
            {
                const T cand = xk + dk;
                x[k] = u[k] < cand ? u[k] : cand;
                if (x[k] == u[k])
                {
                    iword = 1;
                }
            }
        }
        else
        {
            x[k] = xk + dk;
        }
    }
    bool done = (iword == 0);
    if (!done)
    {
        T dd_p = static_cast<T>(0);
        for (int i = 1; i <= n; ++i)
        {
            dd_p += (x[i] - xx[i]) * gg[i];
        }
        if (dd_p > static_cast<T>(0))
        {
            lb_dcopy<T>(n, &xp[0], 1, &x[0], 1); // projection raised the model ⇒ backtrack from x
        }
        else
        {
            done = true;
        }
    }
    if (!done)
    {
        T   alpha = static_cast<T>(1);
        T   temp1 = alpha;
        int ibd = 0;
        for (int i = 1; i <= nsub; ++i)
        {
            const int k = ind[i];
            const T   dk = d[i];
            if (nbd[k] != 0)
            {
                if (dk < static_cast<T>(0) && nbd[k] <= 2)
                {
                    const T temp2 = l[k] - x[k];
                    if (temp2 >= static_cast<T>(0))
                    {
                        temp1 = static_cast<T>(0);
                    }
                    else if (dk * alpha < temp2)
                    {
                        temp1 = temp2 / dk;
                    }
                }
                else if (dk > static_cast<T>(0) && nbd[k] >= 2)
                {
                    const T temp2 = u[k] - x[k];
                    if (temp2 <= static_cast<T>(0))
                    {
                        temp1 = static_cast<T>(0);
                    }
                    else if (dk * alpha > temp2)
                    {
                        temp1 = temp2 / dk;
                    }
                }
                if (temp1 < alpha)
                {
                    alpha = temp1;
                    ibd = i;
                }
            }
        }
        if (alpha < static_cast<T>(1))
        {
            const T   dk = d[ibd];
            const int k = ind[ibd];
            if (dk > static_cast<T>(0))
            {
                x[k] = u[k];
                d[ibd] = static_cast<T>(0);
            }
            else if (dk < static_cast<T>(0))
            {
                x[k] = l[k];
                d[ibd] = static_cast<T>(0);
            }
        }
        for (int i = 1; i <= nsub; ++i)
        {
            const int k = ind[i];
            x[k] += alpha * d[i];
        }
    }
}

// ---- lnsrlb: the bounded line search (Moré-Thuente dcsrch, safeguarded so trial points stay feasible). Ported as
//      a DIRECT loop (the reference is reverse-communication) calling obj at each trial; reuses the verified
//      mt_dcstep. ftol/gtol/xtol = the reference's .001/.9/.1. On return x/f/g hold the accepted point; info=-4 if
//      d is not a descent direction. ----
template <typename T, typename Obj>
inline void lnsrlb(const Obj& obj, int n, const T* l, const T* u, const int* nbd, T* x, T& f, T& fold, T& gd,
                   T& gdold, T* g, const T* d, T* r, T* t, const T* z, T& stp, T& dnorm, T& dtd, T& stpmx, int iter,
                   int& ifun, int& iback, int& nfgv, int& info, bool boxed, bool cnstnd) noexcept
{
    const T ftol = static_cast<T>(0.001);
    const T gtol = static_cast<T>(0.9);
    const T xtol = static_cast<T>(0.1);
    const T stpmn = static_cast<T>(0);
    info = 0;
    dtd = lb_ddot<T>(n, &d[0], 1, &d[0], 1);
    dnorm = std::sqrt(dtd);
    stpmx = static_cast<T>(1e10);
    if (cnstnd)
    {
        if (iter == 0)
        {
            stpmx = static_cast<T>(1);
        }
        else
        {
            for (int i = 1; i <= n; ++i)
            {
                const T a1 = d[i];
                if (nbd[i] != 0)
                {
                    if (a1 < static_cast<T>(0) && nbd[i] <= 2)
                    {
                        const T a2 = l[i] - x[i];
                        if (a2 >= static_cast<T>(0))
                        {
                            stpmx = static_cast<T>(0);
                        }
                        else if (a1 * stpmx < a2)
                        {
                            stpmx = a2 / a1;
                        }
                    }
                    else if (a1 > static_cast<T>(0) && nbd[i] >= 2)
                    {
                        const T a2 = u[i] - x[i];
                        if (a2 <= static_cast<T>(0))
                        {
                            stpmx = static_cast<T>(0);
                        }
                        else if (a1 * stpmx > a2)
                        {
                            stpmx = a2 / a1;
                        }
                    }
                }
            }
        }
    }
    if (iter == 0 && !boxed)
    {
        const T inv = static_cast<T>(1) / dnorm;
        stp = inv < stpmx ? inv : stpmx;
    }
    else
    {
        stp = static_cast<T>(1);
    }
    lb_dcopy<T>(n, &x[0], 1, &t[0], 1); // t = x_old
    lb_dcopy<T>(n, &g[0], 1, &r[0], 1); // r = g_old
    fold = f;
    ifun = 0;
    iback = 0;
    gd = lb_ddot<T>(n, &g[0], 1, &d[0], 1);
    gdold = gd;
    if (gd >= static_cast<T>(0))
    {
        info = -4; // not a descent direction
        return;
    }
    // dcsrch state.
    bool    brackt = false;
    int     stage = 1;
    const T finit = f;
    const T ginit = gd;
    const T gtest = ftol * ginit;
    T       width = stpmx - stpmn;
    T       width1 = width / static_cast<T>(0.5);
    T       stx = static_cast<T>(0);
    T       fx = finit;
    T       gx = ginit;
    T       sty = static_cast<T>(0);
    T       fy = finit;
    T       gy = ginit;
    T       stmin = static_cast<T>(0);
    T       stmax = stp + stp * static_cast<T>(4);
    const crd::usize un = static_cast<crd::usize>(n);
    for (;;)
    {
        if (stp == static_cast<T>(1))
        {
            lb_dcopy<T>(n, &z[0], 1, &x[0], 1);
        }
        else
        {
            for (int i = 1; i <= n; ++i)
            {
                x[i] = stp * d[i] + t[i];
            }
        }
        f = obj.value({&x[1], un});
        (void)obj.gradient({&x[1], un}, {&g[1], un});
        ++ifun;
        ++nfgv;
        iback = ifun - 1;
        gd = lb_ddot<T>(n, &g[0], 1, &d[0], 1);
        const T ftest = finit + stp * gtest;
        if (stage == 1 && f <= ftest && gd >= static_cast<T>(0))
        {
            stage = 2;
        }
        bool warn = (brackt && (stp <= stmin || stp >= stmax)) || (brackt && stmax - stmin <= xtol * stmax) ||
                    (stp == stpmx && f <= ftest && gd <= gtest) ||
                    (stp == stpmn && (f > ftest || gd >= gtest));
        const bool conv = (f <= ftest && std::fabs(gd) <= gtol * (-ginit));
        if (warn || conv || iback >= 20)
        {
            break; // line search done (x/f/g hold the accepted/best point); mainlb handles iback>=20
        }
        if (stage == 1 && f <= fx && f > ftest)
        {
            T fm = f - stp * gtest;
            T fxm = fx - stx * gtest;
            T fym = fy - sty * gtest;
            T gm = gd - gtest;
            T gxm = gx - gtest;
            T gym = gy - gtest;
            detail::mt_dcstep<T>(stx, fxm, gxm, sty, fym, gym, stp, fm, gm, brackt, stmin, stmax);
            fx = fxm + stx * gtest;
            fy = fym + sty * gtest;
            gx = gxm + gtest;
            gy = gym + gtest;
        }
        else
        {
            detail::mt_dcstep<T>(stx, fx, gx, sty, fy, gy, stp, f, gd, brackt, stmin, stmax);
        }
        if (brackt)
        {
            if (std::fabs(sty - stx) >= static_cast<T>(0.66) * width1)
            {
                stp = stx + static_cast<T>(0.5) * (sty - stx);
            }
            width1 = width;
            width = std::fabs(sty - stx);
        }
        if (brackt)
        {
            stmin = std::min(stx, sty);
            stmax = std::max(stx, sty);
        }
        else
        {
            stmin = stp + static_cast<T>(1.1) * (stp - stx);
            stmax = stp + static_cast<T>(4) * (stp - stx);
        }
        stp = std::max(stp, stpmn);
        stp = std::min(stp, stpmx);
        if ((brackt && (stp <= stmin || stp >= stmax)) || (brackt && stmax - stmin <= xtol * stmax))
        {
            stp = stx;
        }
    }
}

// ---- minimize_lbfgsb: the public driver. mainlb (the reverse-communication reference) ported as a DIRECT loop
//      over the verified subroutines (cauchy → freev → formk → cmprlb → subsm → lnsrlb → matupd/formt). Bounds are
//      l ≤ x ≤ u; pass ±1e30 (or larger) for an unbounded side. `factr` is scipy's relative-f tolerance
//      (CONV_F: fold−f ≤ factr·ε·max(|fold|,|f|,1)); opts.grad_tol is pgtol (the projected-gradient ∞-norm). ----
template <typename T>
[[nodiscard]] OptResult<T> minimize_lbfgsb(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                           crd::containers::ConstSpan<T> lower, crd::containers::ConstSpan<T> upper,
                                           const OptOptions<T>& opts, crd::memory::IAllocator* alloc,
                                           crd::usize memory = 8, T factr = static_cast<T>(1e7)) noexcept
{
    const int nn = static_cast<int>(obj.n());
    const int m = static_cast<int>(memory > 0 ? memory : 1);
    const T   kBig = static_cast<T>(1e30);

    OptResult<T> result(alloc);
    result.x.resize(static_cast<crd::usize>(nn));
    if (nn == 0)
    {
        result.status = OptStatus::Success;
        result.converged = true;
        return result;
    }

    // 1-based / over-allocated work arrays (the faithful-port convention).
    crd::containers::Array<T>   xa(alloc);
    crd::containers::Array<T>   la(alloc);
    crd::containers::Array<T>   ua(alloc);
    crd::containers::Array<T>   ga(alloc);
    crd::containers::Array<T>   za(alloc);
    crd::containers::Array<T>   ra(alloc);
    crd::containers::Array<T>   da(alloc);
    crd::containers::Array<T>   ta(alloc);
    crd::containers::Array<T>   xpa(alloc);
    crd::containers::Array<T>   waa(alloc);
    crd::containers::Array<T>   wsa(alloc);
    crd::containers::Array<T>   wya(alloc);
    crd::containers::Array<T>   sya(alloc);
    crd::containers::Array<T>   ssa(alloc);
    crd::containers::Array<T>   wta(alloc);
    crd::containers::Array<T>   wna(alloc);
    crd::containers::Array<T>   snda(alloc);
    crd::containers::Array<int> nbda(alloc);
    crd::containers::Array<int> indexa(alloc);
    crd::containers::Array<int> iwherea(alloc);
    crd::containers::Array<int> indx2a(alloc);
    const int                   n1 = nn + 1;
    const int                   nm = nn * (m + 1) + 1;
    const int                   mm = m * (m + 1) + 1;
    const int                   m22 = (2 * m) * (2 * m + 1) + 1;
    xa.resize(static_cast<crd::usize>(n1));
    la.resize(static_cast<crd::usize>(n1));
    ua.resize(static_cast<crd::usize>(n1));
    ga.resize(static_cast<crd::usize>(n1));
    za.resize(static_cast<crd::usize>(n1));
    ra.resize(static_cast<crd::usize>(n1));
    da.resize(static_cast<crd::usize>(n1));
    ta.resize(static_cast<crd::usize>(n1));
    xpa.resize(static_cast<crd::usize>(n1));
    waa.resize(static_cast<crd::usize>(8 * m + 1));
    wsa.resize(static_cast<crd::usize>(nm));
    wya.resize(static_cast<crd::usize>(nm));
    sya.resize(static_cast<crd::usize>(mm));
    ssa.resize(static_cast<crd::usize>(mm));
    wta.resize(static_cast<crd::usize>(mm));
    wna.resize(static_cast<crd::usize>(m22));
    snda.resize(static_cast<crd::usize>(m22));
    nbda.resize(static_cast<crd::usize>(n1));
    indexa.resize(static_cast<crd::usize>(n1));
    iwherea.resize(static_cast<crd::usize>(n1));
    indx2a.resize(static_cast<crd::usize>(n1));

    T*   x = xa.data();
    T*   l = la.data();
    T*   u = ua.data();
    T*   g = ga.data();
    T*   z = za.data();
    T*   r = ra.data();
    T*   d = da.data();
    T*   t = ta.data();
    T*   xp = xpa.data();
    T*   wa = waa.data();
    T*   ws = wsa.data();
    T*   wy = wya.data();
    T*   sy = sya.data();
    T*   ss = ssa.data();
    T*   wt = wta.data();
    T*   wn = wna.data();
    T*   snd = snda.data();
    int* nbd = nbda.data();
    int* index = indexa.data();
    int* iwhere = iwherea.data();
    int* indx2 = indx2a.data();

    for (int i = 1; i <= nn; ++i)
    {
        x[i] = x0[static_cast<crd::usize>(i - 1)];
        const T lo = lower[static_cast<crd::usize>(i - 1)];
        const T hi = upper[static_cast<crd::usize>(i - 1)];
        l[i] = lo;
        u[i] = hi;
        const bool has_lo = lo > -kBig;
        const bool has_hi = hi < kBig;
        nbd[i] = has_lo ? (has_hi ? 2 : 1) : (has_hi ? 3 : 0);
    }

    const T   epsmch = std::numeric_limits<T>::epsilon();
    const T   pgtol = opts.grad_tol;
    const T   tol = factr * epsmch;
    const crd::usize un = static_cast<crd::usize>(nn);

    int  col = 0;
    int  head = 1;
    int  iupdat = 0;
    int  itail = 0;
    int  iword = 0;
    int  nfree = nn;
    int  nseg = 0;
    int  ifun = 0;
    int  iback = 0;
    int  nfgv = 0;
    int  nenter = 0;
    int  ileave = 0;
    bool updatd = false;
    bool prjctd = false;
    bool cnstnd = false;
    bool boxed = false;
    bool wrk = false;
    T    theta = static_cast<T>(1);
    T    fval = static_cast<T>(0);
    T    fold = static_cast<T>(0);
    T    sbgnrm = static_cast<T>(0);
    T    gd = static_cast<T>(0);
    T    gdold = static_cast<T>(0);
    T    stp = static_cast<T>(0);
    T    dnorm = static_cast<T>(0);
    T    dtd = static_cast<T>(0);
    T    stpmx = static_cast<T>(0);
    int       info = 0;
    OptStatus status = OptStatus::MaxIterations;

    active<T>(nn, l, u, nbd, x, iwhere, prjctd, cnstnd, boxed);
    fval = obj.value({&x[1], un});
    (void)obj.gradient({&x[1], un}, {&g[1], un});
    nfgv = 1;
    projgr<T>(nn, l, u, nbd, x, g, sbgnrm);
    if (sbgnrm <= pgtol)
    {
        status = OptStatus::Success;
    }
    else
    {
        crd::usize iter = 0;
        for (;;) // mainlb loop (L222)
        {
            if (opts.record_history)
            {
                result.history.push_back(fval);
            }
            iword = -1;
            bool refresh = false;
            if (!cnstnd && col > 0)
            {
                lb_dcopy<T>(nn, &x[0], 1, &z[0], 1);
                wrk = updatd;
                nseg = 0;
            }
            else
            {
                info = 0;
                cauchy<T>(nn, x, l, u, nbd, g, indx2, iwhere, t, d, z, m, wy, ws, sy, wt, theta, col, head, &wa[0],
                          &wa[2 * m], &wa[4 * m], &wa[6 * m], nseg, sbgnrm, info, epsmch);
                if (info != 0)
                {
                    refresh = true;
                }
                else
                {
                    freev<T>(nn, nfree, index, nenter, ileave, indx2, iwhere, wrk, updatd, cnstnd,
                             static_cast<int>(iter));
                }
            }
            if (!refresh && !(nfree == 0 || col == 0))
            {
                if (wrk)
                {
                    formk<T>(nn, nfree, index, nenter, ileave, indx2, iupdat, updatd, wn, snd, m, ws, wy, sy, theta,
                             col, head, info);
                }
                if (info != 0)
                {
                    refresh = true;
                }
                else
                {
                    cmprlb<T>(nn, m, x, g, ws, wy, sy, wt, z, r, wa, index, theta, col, head, nfree, cnstnd, info);
                    if (info == 0)
                    {
                        subsm<T>(nn, m, nfree, index, l, u, nbd, z, r, xp, ws, wy, theta, x, g, col, head, iword,
                                 wa, wn, info);
                    }
                    if (info != 0)
                    {
                        refresh = true;
                    }
                }
            }
            if (!refresh)
            {
                for (int i = 1; i <= nn; ++i)
                {
                    d[i] = z[i] - x[i];
                }
                info = 0;
                lnsrlb<T>(obj, nn, l, u, nbd, x, fval, fold, gd, gdold, g, d, r, t, z, stp, dnorm, dtd, stpmx,
                          static_cast<int>(iter), ifun, iback, nfgv, info, boxed, cnstnd);
            }
            if (refresh || info != 0 || iback >= 20)
            {
                if (!refresh)
                {
                    // line search failed ⇒ restore the previous iterate.
                    lb_dcopy<T>(nn, &t[0], 1, &x[0], 1);
                    lb_dcopy<T>(nn, &r[0], 1, &g[0], 1);
                    fval = fold;
                    if (col == 0)
                    {
                        status = OptStatus::LineSearchFailed;
                        ++iter;
                        break;
                    }
                }
                // refresh the L-BFGS memory and restart the iteration.
                info = 0;
                col = 0;
                head = 1;
                theta = static_cast<T>(1);
                iupdat = 0;
                updatd = false;
                if (refresh)
                {
                    continue; // GCP/subspace singular ⇒ restart without consuming an iteration
                }
                continue;
            }
            // line search succeeded.
            ++iter;
            projgr<T>(nn, l, u, nbd, x, g, sbgnrm);
            if (sbgnrm <= pgtol)
            {
                status = OptStatus::Success;
                break;
            }
            T ddum = std::fabs(fold);
            ddum = std::max(ddum, std::fabs(fval));
            ddum = std::max(ddum, static_cast<T>(1));
            if (fold - fval <= tol * ddum)
            {
                status = OptStatus::Success; // CONV_F (relative function reduction below tolerance)
                break;
            }
            // y = g − g_old (r holds g_old); rr = y'y, dr = y's.
            for (int i = 1; i <= nn; ++i)
            {
                r[i] = g[i] - r[i];
            }
            const T rr = lb_ddot<T>(nn, &r[0], 1, &r[0], 1);
            T       dr = static_cast<T>(0);
            T       ddum2 = static_cast<T>(0);
            if (stp == static_cast<T>(1))
            {
                dr = gd - gdold;
                ddum2 = -gdold;
            }
            else
            {
                dr = (gd - gdold) * stp;
                lb_dscal<T>(nn, stp, &d[0], 1);
                ddum2 = -gdold * stp;
            }
            if (dr <= epsmch * ddum2)
            {
                updatd = false; // skip the L-BFGS update (curvature too small)
            }
            else
            {
                updatd = true;
                ++iupdat;
                matupd<T>(nn, m, ws, wy, sy, ss, d, r, itail, iupdat, col, head, theta, rr, dr, stp, dtd);
                formt<T>(m, wt, sy, ss, col, theta, info);
                if (info != 0)
                {
                    info = 0;
                    col = 0;
                    head = 1;
                    theta = static_cast<T>(1);
                    iupdat = 0;
                    updatd = false;
                    continue;
                }
            }
            if (iter >= opts.max_iters)
            {
                status = OptStatus::MaxIterations;
                break;
            }
        }
        result.iterations = iter;
    }

    for (int i = 1; i <= nn; ++i)
    {
        result.x[static_cast<crd::usize>(i - 1)] = x[i];
    }
    result.fx = fval;
    result.grad_norm = sbgnrm; // the projected-gradient ∞-norm
    result.fn_evals = static_cast<crd::usize>(nfgv);
    result.grad_evals = static_cast<crd::usize>(nfgv);
    result.status = status;
    result.converged = (status == OptStatus::Success);
    return result;
}

} // namespace crd::hesap::opt::detail::lbfgsb

namespace crd::hesap::opt
{
// Public entry point: bound-constrained L-BFGS-B (the body lives in detail::lbfgsb over the verified subroutines).
using detail::lbfgsb::minimize_lbfgsb;
} // namespace crd::hesap::opt
