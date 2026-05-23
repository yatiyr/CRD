#pragma once

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/detail/bdsqr.hpp>       // drot
#include <crd/hesap/dense/detail/householder.hpp> // hypot2 (dlapy2), fsign
#include <crd/hesap/dense/detail/svd_secular.hpp> // dlasd4
#include <crd/memory/allocator.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3b-2.2 — Gu-Eisenstat divide-and-conquer merge of two adjacent
// bidiagonal sub-problems: dlasd1 (driver) -> dlasd2 (deflation) -> dlasd3
// (secular solve via dlasd4 + singular-vector assembly). Faithful ports of
// LAPACK dlasd1.f / dlasd2.f / dlasd3.f, kept in COLUMN-MAJOR (matching the
// Fortran exactly: U(i,j) = u[(j-1)*ldu + (i-1)]), which also makes each
// U(:,j)/VT(:,j) column contiguous — directly feedable to dlasd4's delta/work.
//
// The trailing GEMM assembly is written as explicit column-major loops here
// (correctness-first); the gemm_parallel crush is v3b-2.3 (the dbdsdc driver).
//
// Lower layer: raw f32/f64 (ADR-0078). Real T only.
//
// References (build/win-vs-ref/_deps/openblas-src/lapack-netlib/SRC/):
//   dlasd1.f  dlasd2.f  dlasd3.f  dlamrg.f
// -----------------------------------------------------------------------

// dlamrg — merge two sorted subsets a[0:n1) (direction dtrd1) and a[n1:n1+n2)
// (direction dtrd2) into a single ascending order; writes the 1-based merge
// permutation into index[0:n1+n2). Faithful port of dlamrg.f (a/index here are
// 0-based buffers; the produced indices are 1-based as the callers expect).
template <typename R>
inline void dlamrg(int n1, int n2, const R* a, int dtrd1, int dtrd2, int* index) noexcept
{
    int n1sv = n1;
    int n2sv = n2;
    int ind1 = (dtrd1 > 0) ? 1 : n1;
    int ind2 = (dtrd2 > 0) ? (1 + n1) : (n1 + n2);
    int i = 0;
    while (n1sv > 0 && n2sv > 0)
    {
        if (a[ind1 - 1] <= a[ind2 - 1])
        {
            index[i++] = ind1;
            ind1 += dtrd1;
            --n1sv;
        }
        else
        {
            index[i++] = ind2;
            ind2 += dtrd2;
            --n2sv;
        }
    }
    while (n1sv > 0)
    {
        index[i++] = ind1;
        ind1 += dtrd1;
        --n1sv;
    }
    while (n2sv > 0)
    {
        index[i++] = ind2;
        ind2 += dtrd2;
        --n2sv;
    }
}

// dnrm2 — Euclidean norm of a strided vector, overflow/underflow-safe by
// scaling with the largest |element| (sufficient for the merge's use).
template <typename R>
[[nodiscard]] inline R dnrm2(int n, const R* x, int incx) noexcept
{
    if (n <= 0)
    {
        return R{0};
    }
    R scale = R{0};
    for (int k = 0; k < n; ++k)
    {
        scale = std::max(scale, std::abs(x[k * incx]));
    }
    if (scale == R{0})
    {
        return R{0};
    }
    R ssq = R{0};
    for (int k = 0; k < n; ++k)
    {
        const R t = x[k * incx] / scale;
        ssq += t * t;
    }
    return scale * std::sqrt(ssq);
}

// =======================================================================
// dlasd2 — deflate the secular equation for the merge of two sub-problems.
// COLUMN-MAJOR. NL/NR sub-problem sizes, SQRE in {0,1}, M = N + SQRE,
// N = NL+NR+1. On entry D holds the two sub-problems' singular values (with
// D[NL]=0 the placeholder), U/VT the block-diagonal sub-problem vectors,
// IDXQ the per-half sort. On exit: K = #non-deflated; D[0:K) deflated poles
// ascending in DSIGMA; Z the rank-one weights; U2/VT2 the permuted vectors;
// IDXC/COLTYP(=CTOT) the column-type permutation dlasd3 consumes. Faithful
// port of dlasd2.f. Reuses detail::drot for the equal-pole Givens (applied to
// BOTH U columns and VT rows). 1-based accessor lambdas mirror the Fortran.
// =======================================================================
template <typename R>
inline void dlasd2(int nl, int nr, int sqre, int& k, R* d, R* z, R alpha, R beta, R* u, int ldu, R* vt, int ldvt,
                   R* dsigma, R* u2, int ldu2, R* vt2, int ldvt2, int* idxp, int* idx, int* idxc, int* idxq,
                   int* coltyp) noexcept
{
    const R zero = R{0};
    const R one = R{1};
    const R two = R{2};
    const R eight = R{8};

    const int n = nl + nr + 1;
    const int m = n + sqre;
    const int nlp1 = nl + 1;
    const int nlp2 = nl + 2;

    auto dia = [&](int j) -> R& { return d[j - 1]; };
    auto zee = [&](int j) -> R& { return z[j - 1]; };
    auto dsig = [&](int j) -> R& { return dsigma[j - 1]; };
    auto um = [&](int r, int c) -> R& { return u[(c - 1) * ldu + (r - 1)]; };
    auto vtm = [&](int r, int c) -> R& { return vt[(c - 1) * ldvt + (r - 1)]; };
    auto u2m = [&](int r, int c) -> R& { return u2[(c - 1) * ldu2 + (r - 1)]; };
    auto vt2m = [&](int r, int c) -> R& { return vt2[(c - 1) * ldvt2 + (r - 1)]; };
    auto idxp1 = [&](int j) -> int& { return idxp[j - 1]; };
    auto idx1 = [&](int j) -> int& { return idx[j - 1]; };
    auto idxc1 = [&](int j) -> int& { return idxc[j - 1]; };
    auto idxq1 = [&](int j) -> int& { return idxq[j - 1]; };
    auto colt = [&](int j) -> int& { return coltyp[j - 1]; };

    // Generate the first part of Z; shift the first part of D back one slot.
    const R z1 = alpha * vtm(nlp1, nlp1);
    zee(1) = z1;
    for (int i = nl; i >= 1; --i)
    {
        zee(i + 1) = alpha * vtm(i, nlp1);
        dia(i + 1) = dia(i);
        idxq1(i + 1) = idxq1(i) + 1;
    }
    // Second part of Z.
    for (int i = nlp2; i <= m; ++i)
    {
        zee(i) = beta * vtm(i, nlp2);
    }
    // Initialize reference column types.
    for (int i = 2; i <= nlp1; ++i)
    {
        colt(i) = 1;
    }
    for (int i = nlp2; i <= n; ++i)
    {
        colt(i) = 2;
    }
    // Sort the singular values into increasing order.
    for (int i = nlp2; i <= n; ++i)
    {
        idxq1(i) = idxq1(i) + nlp1;
    }
    // DSIGMA, U2(:,1), IDXC used as scratch.
    for (int i = 2; i <= n; ++i)
    {
        dsig(i) = dia(idxq1(i));
        u2m(i, 1) = zee(idxq1(i));
        idxc1(i) = colt(idxq1(i));
    }
    dlamrg<R>(nl, nr, &dsig(2), 1, 1, &idx1(2));
    for (int i = 2; i <= n; ++i)
    {
        const int idxi = 1 + idx1(i);
        dia(i) = dsig(idxi);
        zee(i) = u2m(idxi, 1);
        colt(i) = idxc1(idxi);
    }

    // Deflation tolerance.
    const R eps = std::numeric_limits<R>::epsilon();
    R tol = std::max(std::abs(alpha), std::abs(beta));
    tol = eight * eps * std::max(std::abs(dia(n)), tol);

    // Deflation: small z, or near-equal poles (two-sided Givens).
    k = 1;
    int k2 = n + 1;
    int jprev = 0;
    bool finished_scan = false;
    int j = 0;
    for (j = 2; j <= n; ++j)
    {
        if (std::abs(zee(j)) <= tol)
        {
            k2 = k2 - 1;
            idxp1(k2) = j;
            colt(j) = 4;
            if (j == n)
            {
                finished_scan = true;  // GO TO 120
                break;
            }
        }
        else
        {
            jprev = j;
            break;  // GO TO 90
        }
    }
    if (!finished_scan)
    {
        j = jprev;
        bool record_last = true;
        while (true)
        {
            j = j + 1;
            if (j > n)
            {
                break;  // GO TO 110
            }
            if (std::abs(zee(j)) <= tol)
            {
                k2 = k2 - 1;
                idxp1(k2) = j;
                colt(j) = 4;
            }
            else if (std::abs(dia(j) - dia(jprev)) <= tol)
            {
                // Deflation by near-equal poles: rotate to zero one z entry.
                R s = zee(jprev);
                R c = zee(j);
                const R tau = hypot2(c, s);
                c = c / tau;
                s = -s / tau;
                zee(j) = tau;
                zee(jprev) = zero;
                int idxjp = idxq1(idx1(jprev) + 1);
                int idxj = idxq1(idx1(j) + 1);
                if (idxjp <= nlp1)
                {
                    idxjp = idxjp - 1;
                }
                if (idxj <= nlp1)
                {
                    idxj = idxj - 1;
                }
                drot<R>(n, &um(1, idxjp), 1, &um(1, idxj), 1, c, s);
                drot<R>(m, &vtm(idxjp, 1), ldvt, &vtm(idxj, 1), ldvt, c, s);
                if (colt(j) != colt(jprev))
                {
                    colt(j) = 3;
                }
                colt(jprev) = 4;
                k2 = k2 - 1;
                idxp1(k2) = jprev;
                jprev = j;
            }
            else
            {
                k = k + 1;
                u2m(k, 1) = zee(jprev);
                dsig(k) = dia(jprev);
                idxp1(k) = jprev;
                jprev = j;
            }
        }
        // Record the last singular value.
        if (record_last)
        {
            k = k + 1;
            u2m(k, 1) = zee(jprev);
            dsig(k) = dia(jprev);
            idxp1(k) = jprev;
        }
    }

    // Count column types, then build IDXC grouping types 1..4.
    int ctot[4] = {0, 0, 0, 0};
    for (int jj = 2; jj <= n; ++jj)
    {
        const int ct = colt(jj);
        ctot[ct - 1] = ctot[ct - 1] + 1;
    }
    int psm[4];
    psm[0] = 2;
    psm[1] = 2 + ctot[0];
    psm[2] = psm[1] + ctot[1];
    psm[3] = psm[2] + ctot[2];
    for (int jj = 2; jj <= n; ++jj)
    {
        const int jp = idxp1(jj);
        const int ct = colt(jp);
        idxc1(psm[ct - 1]) = jj;
        psm[ct - 1] = psm[ct - 1] + 1;
    }

    // Sort vectors/poles into DSIGMA, U2, VT2 (non-deflated first K, deflated last).
    for (int jj = 2; jj <= n; ++jj)
    {
        const int jp = idxp1(jj);
        dsig(jj) = dia(jp);
        int idxj = idxq1(idx1(idxp1(idxc1(jj))) + 1);
        if (idxj <= nlp1)
        {
            idxj = idxj - 1;
        }
        for (int r = 1; r <= n; ++r)
        {
            u2m(r, jj) = um(r, idxj);
        }
        for (int col = 1; col <= m; ++col)
        {
            vt2m(jj, col) = vtm(idxj, col);
        }
    }

    // DSIGMA(1), DSIGMA(2), Z(1).
    dsig(1) = zero;
    const R hlftol = tol / two;
    if (std::abs(dsig(2)) <= hlftol)
    {
        dsig(2) = hlftol;
    }
    R cc = one;
    R ss = zero;
    if (m > n)
    {
        zee(1) = hypot2(z1, zee(m));
        if (zee(1) <= tol)
        {
            cc = one;
            ss = zero;
            zee(1) = tol;
        }
        else
        {
            cc = z1 / zee(1);
            ss = zee(m) / zee(1);
        }
    }
    else
    {
        if (std::abs(z1) <= tol)
        {
            zee(1) = tol;
        }
        else
        {
            zee(1) = z1;
        }
    }
    // Move the rest of the updating row into Z.
    for (int i = 2; i <= k; ++i)
    {
        zee(i) = u2m(i, 1);
    }

    // First column of U2, first row of VT2, last row of VT.
    for (int r = 1; r <= n; ++r)
    {
        u2m(r, 1) = zero;
    }
    u2m(nlp1, 1) = one;
    if (m > n)
    {
        for (int i = 1; i <= nlp1; ++i)
        {
            vtm(m, i) = -ss * vtm(nlp1, i);
            vt2m(1, i) = cc * vtm(nlp1, i);
        }
        for (int i = nlp2; i <= m; ++i)
        {
            vt2m(1, i) = ss * vtm(m, i);
            vtm(m, i) = cc * vtm(m, i);
        }
        for (int col = 1; col <= m; ++col)
        {
            vt2m(m, col) = vtm(m, col);
        }
    }
    else
    {
        for (int col = 1; col <= m; ++col)
        {
            vt2m(1, col) = vtm(nlp1, col);
        }
    }

    // Deflated values/vectors go into the back of D, U, VT.
    if (n > k)
    {
        for (int i = k + 1; i <= n; ++i)
        {
            dia(i) = dsig(i);
        }
        for (int col = k + 1; col <= n; ++col)
        {
            for (int r = 1; r <= n; ++r)
            {
                um(r, col) = u2m(r, col);
            }
        }
        for (int col = 1; col <= m; ++col)
        {
            for (int r = k + 1; r <= n; ++r)
            {
                vtm(r, col) = vt2m(r, col);
            }
        }
    }

    // Copy CTOT into COLTYP(1..4) for dlasd3.
    for (int jj = 1; jj <= 4; ++jj)
    {
        colt(jj) = ctot[jj - 1];
    }
}

// gemm_cm — column-major C(M x N) = alpha*A(M x K)*B(K x N) + beta*C, explicit
// loops (correctness-first; the gemm_parallel crush is v3b-2.3). A(i,l) =
// a[l*lda+i], B(l,j) = b[j*ldb+l], C(i,j) = c[j*ldc+i].
template <typename R>
inline void gemm_cm_nn(int mm, int nn, int kk, R alpha, const R* a, int lda, const R* b, int ldb, R beta, R* c,
                       int ldc) noexcept
{
    for (int j = 0; j < nn; ++j)
    {
        for (int i = 0; i < mm; ++i)
        {
            R acc = R{0};
            for (int l = 0; l < kk; ++l)
            {
                acc += a[l * lda + i] * b[j * ldb + l];
            }
            c[j * ldc + i] = alpha * acc + beta * c[j * ldc + i];
        }
    }
}

// =======================================================================
// dlasd3 — solve the (deflated) secular equation and assemble the merged
// singular vectors. COLUMN-MAJOR. K = #non-deflated; DSIGMA the deflated
// poles; Z the rank-one weights; U2/VT2 the dlasd2-permuted sub-problem
// vectors; IDXC/CTOT the column-type permutation. On exit D[0:K) the new
// singular values, U/VT the assembled vectors. Faithful port of dlasd3.f:
// dlasd4 per root, the interleaved-Lowner Z recompute, then the type-grouped
// GEMM assembly (here gemm_cm_nn). Q is K x K scratch.
// =======================================================================
template <typename R>
inline int dlasd3(int nl, int nr, int sqre, int k, R* d, R* q, int ldq, const R* dsigma, R* u, int ldu,
                  const R* u2, int ldu2, R* vt, int ldvt, R* vt2, int ldvt2, const int* idxc,
                  const int* ctot, R* z) noexcept
{
    const R one = R{1};
    const R zero = R{0};
    const R negone = R{-1};

    const int n = nl + nr + 1;
    const int m = n + sqre;
    const int nlp1 = nl + 1;
    const int nlp2 = nl + 2;

    auto dia = [&](int j) -> R& { return d[j - 1]; };
    auto zee = [&](int j) -> R& { return z[j - 1]; };
    auto dsig = [&](int j) -> R { return dsigma[j - 1]; };
    auto qm = [&](int r, int c) -> R& { return q[(c - 1) * ldq + (r - 1)]; };
    auto um = [&](int r, int c) -> R& { return u[(c - 1) * ldu + (r - 1)]; };
    auto u2m = [&](int r, int c) -> R { return u2[(c - 1) * ldu2 + (r - 1)]; };
    auto vtm = [&](int r, int c) -> R& { return vt[(c - 1) * ldvt + (r - 1)]; };
    auto vt2m = [&](int r, int c) -> R& { return vt2[(c - 1) * ldvt2 + (r - 1)]; };
    auto u2p = [&](int r, int c) -> const R* { return u2 + (c - 1) * ldu2 + (r - 1); };
    auto vt2p = [&](int r, int c) -> R* { return vt2 + (c - 1) * ldvt2 + (r - 1); };
    auto idxc1 = [&](int j) -> int { return idxc[j - 1]; };

    if (k == 1)
    {
        dia(1) = std::abs(zee(1));
        for (int col = 1; col <= m; ++col)
        {
            vtm(1, col) = vt2m(1, col);
        }
        if (zee(1) > zero)
        {
            for (int i = 1; i <= n; ++i)
            {
                um(i, 1) = u2m(i, 1);
            }
        }
        else
        {
            for (int i = 1; i <= n; ++i)
            {
                um(i, 1) = -u2m(i, 1);
            }
        }
        return 0;
    }

    // Keep a copy of Z in Q(:,1); normalize Z; rho = ||z||^2.
    for (int i = 1; i <= k; ++i)
    {
        qm(i, 1) = zee(i);
    }
    R rho = dnrm2<R>(k, z, 1);
    for (int i = 1; i <= k; ++i)
    {
        zee(i) = zee(i) / rho;
    }
    rho = rho * rho;

    // Find the new singular values (delta -> U(:,j), work -> VT(:,j)).
    for (int j = 1; j <= k; ++j)
    {
        int linfo = 0;
        dlasd4<R>(k, j - 1, dsigma, z, &um(1, j), rho, dia(j), &vtm(1, j), linfo);
        if (linfo != 0)
        {
            return linfo;
        }
    }

    // Updated Z via the interleaved Lowner product (overflow-safe).
    for (int i = 1; i <= k; ++i)
    {
        zee(i) = um(i, k) * vtm(i, k);
        for (int j = 1; j <= i - 1; ++j)
        {
            zee(i) = zee(i) * (um(i, j) * vtm(i, j) / (dsig(i) - dsig(j)) / (dsig(i) + dsig(j)));
        }
        for (int j = i; j <= k - 1; ++j)
        {
            zee(i) = zee(i) * (um(i, j) * vtm(i, j) / (dsig(i) - dsig(j + 1)) / (dsig(i) + dsig(j + 1)));
        }
        zee(i) = fsign(std::sqrt(std::abs(zee(i))), qm(i, 1));
    }

    // Left singular vectors of the modified diagonal matrix; store right info.
    for (int i = 1; i <= k; ++i)
    {
        vtm(1, i) = zee(1) / um(1, i) / vtm(1, i);
        um(1, i) = negone;
        for (int j = 2; j <= k; ++j)
        {
            vtm(j, i) = zee(j) / um(j, i) / vtm(j, i);
            um(j, i) = dsig(j) * vtm(j, i);
        }
        const R temp = dnrm2<R>(k, &um(1, i), 1);
        qm(1, i) = um(1, i) / temp;
        for (int j = 2; j <= k; ++j)
        {
            const int jc = idxc1(j);
            qm(j, i) = um(jc, i) / temp;
        }
    }

    // Update the left singular vector matrix.
    if (k == 2)
    {
        gemm_cm_nn<R>(n, k, k, one, u2, ldu2, q, ldq, zero, u, ldu);
    }
    else
    {
        if (ctot[0] > 0)
        {
            gemm_cm_nn<R>(nl, k, ctot[0], one, u2p(1, 2), ldu2, &qm(2, 1), ldq, zero, &um(1, 1), ldu);
            if (ctot[2] > 0)
            {
                const int ktemp = 2 + ctot[0] + ctot[1];
                gemm_cm_nn<R>(nl, k, ctot[2], one, u2p(1, ktemp), ldu2, &qm(ktemp, 1), ldq, one, &um(1, 1),
                              ldu);
            }
        }
        else if (ctot[2] > 0)
        {
            const int ktemp = 2 + ctot[0] + ctot[1];
            gemm_cm_nn<R>(nl, k, ctot[2], one, u2p(1, ktemp), ldu2, &qm(ktemp, 1), ldq, zero, &um(1, 1), ldu);
        }
        else
        {
            for (int c = 1; c <= k; ++c)
            {
                for (int r = 1; r <= nl; ++r)
                {
                    um(r, c) = u2m(r, c);
                }
            }
        }
        for (int c = 1; c <= k; ++c)
        {
            um(nlp1, c) = qm(1, c);
        }
        const int ktemp = 2 + ctot[0];
        const int ctemp = ctot[1] + ctot[2];
        gemm_cm_nn<R>(nr, k, ctemp, one, u2p(nlp2, ktemp), ldu2, &qm(ktemp, 1), ldq, zero, &um(nlp2, 1), ldu);
    }

    // Generate the right singular vectors.
    for (int i = 1; i <= k; ++i)
    {
        const R temp = dnrm2<R>(k, &vtm(1, i), 1);
        qm(i, 1) = vtm(1, i) / temp;
        for (int j = 2; j <= k; ++j)
        {
            const int jc = idxc1(j);
            qm(i, j) = vtm(jc, i) / temp;
        }
    }

    // Update the right singular vector matrix.
    if (k == 2)
    {
        gemm_cm_nn<R>(k, m, k, one, q, ldq, vt2, ldvt2, zero, vt, ldvt);
        return 0;
    }
    int ktemp = 1 + ctot[0];
    gemm_cm_nn<R>(k, nlp1, ktemp, one, &qm(1, 1), ldq, vt2p(1, 1), ldvt2, zero, &vtm(1, 1), ldvt);
    ktemp = 2 + ctot[0] + ctot[1];
    if (ktemp <= ldvt2)
    {
        gemm_cm_nn<R>(k, nlp1, ctot[2], one, &qm(1, ktemp), ldq, vt2p(ktemp, 1), ldvt2, one, &vtm(1, 1), ldvt);
    }

    ktemp = ctot[0] + 1;
    const int nrp1 = nr + sqre;
    if (ktemp > 1)
    {
        for (int i = 1; i <= k; ++i)
        {
            qm(i, ktemp) = qm(i, 1);
        }
        for (int i = nlp2; i <= m; ++i)
        {
            vt2m(ktemp, i) = vt2m(1, i);
        }
    }
    const int ctemp = 1 + ctot[1] + ctot[2];
    gemm_cm_nn<R>(k, nrp1, ctemp, one, &qm(1, ktemp), ldq, vt2p(ktemp, nlp2), ldvt2, zero, &vtm(1, nlp2), ldvt);
    return 0;
}

// =======================================================================
// dlasd1 — driver: merge two adjacent sub-problems. Scales, deflates (dlasd2),
// solves + assembles (dlasd3), unscales, and builds the final IDXQ sort.
// COLUMN-MAJOR. d holds the two sub-problems' singular values (d[nl]=0
// placeholder); u/vt the block-diagonal sub-problem vectors; alpha/beta the
// boundary connection (modified in place by the scaling). Scratch allocated
// from `alloc`. Faithful port of dlasd1.f. Returns dlasd3's info.
// =======================================================================
template <typename R>
inline int dlasd1(int nl, int nr, int sqre, R* d, R& alpha, R& beta, R* u, int ldu, R* vt, int ldvt, int* idxq,
                  crd::memory::IAllocator* alloc) noexcept
{
    const R one = R{1};
    const R zero = R{0};
    const int n = nl + nr + 1;
    const int m = n + sqre;
    const int ldu2 = n;
    const int ldvt2 = m;

    crd::containers::Array<R> z(alloc);
    crd::containers::Array<R> dsigma(alloc);
    crd::containers::Array<R> u2(alloc);
    crd::containers::Array<R> vt2(alloc);
    crd::containers::Array<R> qb(alloc);
    z.resize(static_cast<crd::usize>(m));
    dsigma.resize(static_cast<crd::usize>(n));
    u2.resize(static_cast<crd::usize>(ldu2 * n));
    vt2.resize(static_cast<crd::usize>(ldvt2 * m));
    qb.resize(static_cast<crd::usize>(n * n));  // >= K x K
    crd::containers::Array<int> idx(alloc);
    crd::containers::Array<int> idxc(alloc);
    crd::containers::Array<int> coltyp(alloc);
    crd::containers::Array<int> idxp(alloc);
    idx.resize(static_cast<crd::usize>(n));
    idxc.resize(static_cast<crd::usize>(n));
    coltyp.resize(static_cast<crd::usize>(n));
    idxp.resize(static_cast<crd::usize>(n));

    // Scale.
    R orgnrm = std::max(std::abs(alpha), std::abs(beta));
    d[nl] = zero;  // D(NL+1) = 0
    for (int i = 0; i < n; ++i)
    {
        orgnrm = std::max(orgnrm, std::abs(d[i]));
    }
    const R invnrm = one / orgnrm;
    for (int i = 0; i < n; ++i)
    {
        d[i] = d[i] * invnrm;
    }
    alpha = alpha * invnrm;
    beta = beta * invnrm;

    int k = 0;
    dlasd2<R>(nl, nr, sqre, k, d, z.data(), alpha, beta, u, ldu, vt, ldvt, dsigma.data(), u2.data(), ldu2,
              vt2.data(), ldvt2, idxp.data(), idx.data(), idxc.data(), idxq, coltyp.data());

    const int ldq = k;
    const int info = dlasd3<R>(nl, nr, sqre, k, d, qb.data(), ldq, dsigma.data(), u, ldu, u2.data(), ldu2, vt,
                               ldvt, vt2.data(), ldvt2, idxc.data(), coltyp.data(), z.data());
    if (info != 0)
    {
        return info;
    }

    // Unscale.
    for (int i = 0; i < n; ++i)
    {
        d[i] = d[i] * orgnrm;
    }

    // Final sort permutation.
    dlamrg<R>(k, n - k, d, 1, -1, idxq);
    return 0;
}

} // namespace crd::hesap::dense::detail
