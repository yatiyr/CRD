#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/detail/sturm_count.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3a-3.1-dqds-a — dqds inner kernels (dlasq5/dlasq6) + the
// qd-array build + an unshifted dqd driver. This is the MRRR whole-block
// fast eigenvalue engine's foundation: dqds (Fernando-Parlett differential
// quotient-difference with shifts) computes the eigenvalues of a positive-
// definite LDL^T to HIGH RELATIVE accuracy in O(n) per sweep — the route
// dlarre uses for the root representation. Lower layer: raw f32/f64.
//
// PORT FIDELITY — D(dense-eig)-MRRR-Z1base: the qd workspace Z is accessed
// 1-based through the `Z1` wrapper below, so the notorious `4*N0+PP-3`
// ping-pong index arithmetic of dlasq5/6 ports LINE-FOR-LINE from the
// Fortran. Hand-translating those indices to 0-based is the classic dqds
// failure mode; every correct C port (CLAPACK, the f2c'd OpenBLAS LAPACK in
// build/_deps/) keeps 1-based. The `i-1` happens inside operator[] so no
// out-of-bounds pointer is ever formed.
//
// D(dense-eig)-MRRR-dqds-ieee-only: only the IEEE=.TRUE. branches of
// dlasq5/dlasq6 are ported (ADR-0063 mandates IEEE-754 — a contract
// simplification, not a corner-cut). This drops the non-IEEE early-RETURN-
// on-negative-d paths, the most error-prone surface in dqds.
//
// D(dense-eig)-12: dqds is fully deterministic (fixed iteration caps, no
// RNG, IEEE-defined NaN/Inf propagation).
//
// References (build/win-debug/_deps/openblas-src/lapack-netlib/SRC/):
//   dlasq5.f / dlasq6.f  the dqds / dqd inner sweeps
//   dlasq2.f:277-353     the (q,qq,e,ee) 4-wide ping-pong Z layout
// -----------------------------------------------------------------------

// Z1 — 1-based view over a 0-based buffer (z1[1] == base[0]). Keeps the dqds
// index arithmetic identical to the Fortran reference.
template <typename R>
struct Z1
{
    R* base;
    [[nodiscard]] R& operator[](int i) const noexcept { return base[i - 1]; }
};

// =======================================================================
// dlasq6 — one dqd (zero-shift) transform with EMIN. Faithful to dlasq6.f
// (single version, safmin-guarded division — IEEE-agnostic in LAPACK too).
// Updates dmin/dmin1/dmin2/dn/dnm1/dnm2 by reference.
// =======================================================================
template <typename R>
inline void dlasq6(int i0, int n0, Z1<R> z, int pp, R& dmin, R& dmin1, R& dmin2, R& dn, R& dnm1, R& dnm2) noexcept
{
    if ((n0 - i0 - 1) <= 0)
    {
        return;
    }
    const R safmin = std::numeric_limits<R>::min();
    int j4 = 4 * i0 + pp - 3;
    R emin = z[j4 + 4];
    R d = z[j4];
    dmin = d;

    if (pp == 0)
    {
        for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4)
        {
            z[j4 - 2] = d + z[j4 - 1];
            if (z[j4 - 2] == R{0})
            {
                z[j4] = R{0};
                d = z[j4 + 1];
                dmin = d;
                emin = R{0};
            }
            else if (safmin * z[j4 + 1] < z[j4 - 2] && safmin * z[j4 - 2] < z[j4 + 1])
            {
                const R temp = z[j4 + 1] / z[j4 - 2];
                z[j4] = z[j4 - 1] * temp;
                d = d * temp;
            }
            else
            {
                z[j4] = z[j4 + 1] * (z[j4 - 1] / z[j4 - 2]);
                d = z[j4 + 1] * (d / z[j4 - 2]);
            }
            dmin = std::min(dmin, d);
            emin = std::min(emin, z[j4]);
        }
    }
    else
    {
        for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4)
        {
            z[j4 - 3] = d + z[j4];
            if (z[j4 - 3] == R{0})
            {
                z[j4 - 1] = R{0};
                d = z[j4 + 2];
                dmin = d;
                emin = R{0};
            }
            else if (safmin * z[j4 + 2] < z[j4 - 3] && safmin * z[j4 - 3] < z[j4 + 2])
            {
                const R temp = z[j4 + 2] / z[j4 - 3];
                z[j4 - 1] = z[j4] * temp;
                d = d * temp;
            }
            else
            {
                z[j4 - 1] = z[j4 + 2] * (z[j4] / z[j4 - 3]);
                d = z[j4 + 2] * (d / z[j4 - 3]);
            }
            dmin = std::min(dmin, d);
            emin = std::min(emin, z[j4 - 1]);
        }
    }

    // Unroll last two steps.
    dnm2 = d;
    dmin2 = dmin;
    j4 = 4 * (n0 - 2) - pp;
    int j4p2 = j4 + 2 * pp - 1;
    z[j4 - 2] = dnm2 + z[j4p2];
    if (z[j4 - 2] == R{0})
    {
        z[j4] = R{0};
        dnm1 = z[j4p2 + 2];
        dmin = dnm1;
        emin = R{0};
    }
    else if (safmin * z[j4p2 + 2] < z[j4 - 2] && safmin * z[j4 - 2] < z[j4p2 + 2])
    {
        const R temp = z[j4p2 + 2] / z[j4 - 2];
        z[j4] = z[j4p2] * temp;
        dnm1 = dnm2 * temp;
    }
    else
    {
        z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
        dnm1 = z[j4p2 + 2] * (dnm2 / z[j4 - 2]);
    }
    dmin = std::min(dmin, dnm1);

    dmin1 = dmin;
    j4 = j4 + 4;
    j4p2 = j4 + 2 * pp - 1;
    z[j4 - 2] = dnm1 + z[j4p2];
    if (z[j4 - 2] == R{0})
    {
        z[j4] = R{0};
        dn = z[j4p2 + 2];
        dmin = dn;
        emin = R{0};
    }
    else if (safmin * z[j4p2 + 2] < z[j4 - 2] && safmin * z[j4 - 2] < z[j4p2 + 2])
    {
        const R temp = z[j4p2 + 2] / z[j4 - 2];
        z[j4] = z[j4p2] * temp;
        dn = dnm1 * temp;
    }
    else
    {
        z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
        dn = z[j4p2 + 2] * (dnm1 / z[j4 - 2]);
    }
    dmin = std::min(dmin, dn);

    z[j4 + 2] = dn;
    z[4 * n0 - pp] = emin;
}

// =======================================================================
// dlasq5 — one dqds (shifted) transform with EMIN. IEEE-only port of
// dlasq5.f. `tau` is the shift; `sigma` the accumulated shift; `eps` the
// machine precision. Updates dmin/dmin1/dmin2/dn/dnm1/dnm2 and (possibly)
// `tau` by reference.
// =======================================================================
template <typename R>
inline void dlasq5(int i0, int n0, Z1<R> z, int pp, R& tau, R sigma, R& dmin, R& dmin1, R& dmin2, R& dn, R& dnm1,
                   R& dnm2, R eps) noexcept
{
    if ((n0 - i0 - 1) <= 0)
    {
        return;
    }

    const R dthresh = eps * (sigma + tau);
    if (tau < dthresh * R{0.5})
    {
        tau = R{0};
    }

    int j4;
    int j4p2;
    R d;
    R emin;
    R temp;

    if (tau != R{0})
    {
        j4 = 4 * i0 + pp - 3;
        emin = z[j4 + 4];
        d = z[j4] - tau;
        dmin = d;
        dmin1 = -z[j4];

        if (pp == 0)
        {
            for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4)
            {
                z[j4 - 2] = d + z[j4 - 1];
                temp = z[j4 + 1] / z[j4 - 2];
                d = d * temp - tau;
                dmin = std::min(dmin, d);
                z[j4] = z[j4 - 1] * temp;
                emin = std::min(z[j4], emin);
            }
        }
        else
        {
            for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4)
            {
                z[j4 - 3] = d + z[j4];
                temp = z[j4 + 2] / z[j4 - 3];
                d = d * temp - tau;
                dmin = std::min(dmin, d);
                z[j4 - 1] = z[j4] * temp;
                emin = std::min(z[j4 - 1], emin);
            }
        }

        // Unroll last two steps.
        dnm2 = d;
        dmin2 = dmin;
        j4 = 4 * (n0 - 2) - pp;
        j4p2 = j4 + 2 * pp - 1;
        z[j4 - 2] = dnm2 + z[j4p2];
        z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
        dnm1 = z[j4p2 + 2] * (dnm2 / z[j4 - 2]) - tau;
        dmin = std::min(dmin, dnm1);

        dmin1 = dmin;
        j4 = j4 + 4;
        j4p2 = j4 + 2 * pp - 1;
        z[j4 - 2] = dnm1 + z[j4p2];
        z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
        dn = z[j4p2 + 2] * (dnm1 / z[j4 - 2]) - tau;
        dmin = std::min(dmin, dn);
    }
    else
    {
        // tau == 0 — set d's to zero if small enough (dthresh).
        j4 = 4 * i0 + pp - 3;
        emin = z[j4 + 4];
        d = z[j4] - tau;
        dmin = d;
        dmin1 = -z[j4];

        if (pp == 0)
        {
            for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4)
            {
                z[j4 - 2] = d + z[j4 - 1];
                temp = z[j4 + 1] / z[j4 - 2];
                d = d * temp - tau;
                if (d < dthresh)
                {
                    d = R{0};
                }
                dmin = std::min(dmin, d);
                z[j4] = z[j4 - 1] * temp;
                emin = std::min(z[j4], emin);
            }
        }
        else
        {
            for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4)
            {
                z[j4 - 3] = d + z[j4];
                temp = z[j4 + 2] / z[j4 - 3];
                d = d * temp - tau;
                if (d < dthresh)
                {
                    d = R{0};
                }
                dmin = std::min(dmin, d);
                z[j4 - 1] = z[j4] * temp;
                emin = std::min(z[j4 - 1], emin);
            }
        }

        // Unroll last two steps.
        dnm2 = d;
        dmin2 = dmin;
        j4 = 4 * (n0 - 2) - pp;
        j4p2 = j4 + 2 * pp - 1;
        z[j4 - 2] = dnm2 + z[j4p2];
        z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
        dnm1 = z[j4p2 + 2] * (dnm2 / z[j4 - 2]) - tau;
        dmin = std::min(dmin, dnm1);

        dmin1 = dmin;
        j4 = j4 + 4;
        j4p2 = j4 + 2 * pp - 1;
        z[j4 - 2] = dnm1 + z[j4p2];
        z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
        dn = z[j4p2 + 2] * (dnm1 / z[j4 - 2]) - tau;
        dmin = std::min(dmin, dn);
    }

    z[j4 + 2] = dn;
    z[4 * n0 - pp] = emin;
}

// =======================================================================
// dlasq4 — choose a dqds shift TAU + classify it (TTYPE). Faithful IEEE port
// of dlasq4.f. n0in = N0 on entry to the current dlasq3 step (records how many
// eigenvalues just deflated). dmin/dmin1/dmin2/dn/dn1/dn2 are the running
// minima from the previous dqds sweep. Early `return` (without setting tau)
// keeps the caller's current tau — faithful "cannot improve the shift".
// =======================================================================
template <typename R>
inline void dlasq4(int i0, int n0, Z1<R> z, int pp, int n0in, R dmin, R dmin1, R dmin2, R dn, R dn1, R dn2, R& tau,
                   int& ttype, R& g) noexcept
{
    const R cnst1 = R{0.5630};
    const R cnst2 = R{1.010};
    const R cnst3 = R{1.050};
    const R qurtr = R{0.250};
    const R third = R{0.3330};
    const R half = R{0.50};
    const R hundrd = R{100};

    if (dmin <= R{0})
    {
        tau = -dmin;
        ttype = -1;
        return;
    }

    const int nn = 4 * n0 + pp;
    R s = R{0};
    R a2 = R{0};
    R b1 = R{0};
    R b2 = R{0};
    R gam = R{0};
    R gap1 = R{0};
    R gap2 = R{0};

    if (n0in == n0)
    {
        // No eigenvalues deflated.
        if (dmin == dn || dmin == dn1)
        {
            b1 = std::sqrt(z[nn - 3]) * std::sqrt(z[nn - 5]);
            b2 = std::sqrt(z[nn - 7]) * std::sqrt(z[nn - 9]);
            a2 = z[nn - 7] + z[nn - 5];

            if (dmin == dn && dmin1 == dn1)
            {
                // Cases 2 and 3.
                gap2 = dmin2 - a2 - dmin2 * qurtr;
                if (gap2 > R{0} && gap2 > b2)
                {
                    gap1 = a2 - dn - (b2 / gap2) * b2;
                }
                else
                {
                    gap1 = a2 - dn - (b1 + b2);
                }
                if (gap1 > R{0} && gap1 > b1)
                {
                    s = std::max(dn - (b1 / gap1) * b1, half * dmin);
                    ttype = -2;
                }
                else
                {
                    s = R{0};
                    if (dn > b1)
                    {
                        s = dn - b1;
                    }
                    if (a2 > (b1 + b2))
                    {
                        s = std::min(s, a2 - (b1 + b2));
                    }
                    s = std::max(s, third * dmin);
                    ttype = -3;
                }
            }
            else
            {
                // Case 4.
                ttype = -4;
                s = qurtr * dmin;
                int np;
                if (dmin == dn)
                {
                    gam = dn;
                    a2 = R{0};
                    if (z[nn - 5] > z[nn - 7])
                    {
                        return;
                    }
                    b2 = z[nn - 5] / z[nn - 7];
                    np = nn - 9;
                }
                else
                {
                    np = nn - 2 * pp;
                    gam = dn1;
                    if (z[np - 4] > z[np - 2])
                    {
                        return;
                    }
                    a2 = z[np - 4] / z[np - 2];
                    if (z[nn - 9] > z[nn - 11])
                    {
                        return;
                    }
                    b2 = z[nn - 9] / z[nn - 11];
                    np = nn - 13;
                }
                // Approximate contribution to norm squared from I < NN-1.
                a2 = a2 + b2;
                for (int i4 = np; i4 >= 4 * i0 - 1 + pp; i4 -= 4)
                {
                    if (b2 == R{0})
                    {
                        break;
                    }
                    b1 = b2;
                    if (z[i4] > z[i4 - 2])
                    {
                        return;
                    }
                    b2 = b2 * (z[i4] / z[i4 - 2]);
                    a2 = a2 + b2;
                    if (hundrd * std::max(b2, b1) < a2 || cnst1 < a2)
                    {
                        break;
                    }
                }
                a2 = cnst3 * a2;
                if (a2 < cnst1)
                {
                    s = gam * (R{1} - std::sqrt(a2)) / (R{1} + a2);
                }
            }
        }
        else if (dmin == dn2)
        {
            // Case 5.
            ttype = -5;
            s = qurtr * dmin;
            const int np = nn - 2 * pp;
            b1 = z[np - 2];
            b2 = z[np - 6];
            gam = dn2;
            if (z[np - 8] > b2 || z[np - 4] > b1)
            {
                return;
            }
            a2 = (z[np - 8] / b2) * (R{1} + z[np - 4] / b1);
            // Approximate contribution to norm squared from I < NN-2.
            if (n0 - i0 > 2)
            {
                b2 = z[nn - 13] / z[nn - 15];
                a2 = a2 + b2;
                for (int i4 = nn - 17; i4 >= 4 * i0 - 1 + pp; i4 -= 4)
                {
                    if (b2 == R{0})
                    {
                        break;
                    }
                    b1 = b2;
                    if (z[i4] > z[i4 - 2])
                    {
                        return;
                    }
                    b2 = b2 * (z[i4] / z[i4 - 2]);
                    a2 = a2 + b2;
                    if (hundrd * std::max(b2, b1) < a2 || cnst1 < a2)
                    {
                        break;
                    }
                }
                a2 = cnst3 * a2;
            }
            if (a2 < cnst1)
            {
                s = gam * (R{1} - std::sqrt(a2)) / (R{1} + a2);
            }
        }
        else
        {
            // Case 6, no information to guide us.
            if (ttype == -6)
            {
                g = g + third * (R{1} - g);
            }
            else if (ttype == -18)
            {
                g = qurtr * third;
            }
            else
            {
                g = qurtr;
            }
            s = g * dmin;
            ttype = -6;
        }
    }
    else if (n0in == n0 + 1)
    {
        // One eigenvalue just deflated. Use DMIN1, DN1 for DMIN and DN.
        if (dmin1 == dn1 && dmin2 == dn2)
        {
            // Cases 7 and 8.
            ttype = -7;
            s = third * dmin1;
            if (z[nn - 5] > z[nn - 7])
            {
                return;
            }
            b1 = z[nn - 5] / z[nn - 7];
            b2 = b1;
            if (b2 != R{0})
            {
                for (int i4 = 4 * n0 - 9 + pp; i4 >= 4 * i0 - 1 + pp; i4 -= 4)
                {
                    a2 = b1;
                    if (z[i4] > z[i4 - 2])
                    {
                        return;
                    }
                    b1 = b1 * (z[i4] / z[i4 - 2]);
                    b2 = b2 + b1;
                    if (hundrd * std::max(b1, a2) < b2)
                    {
                        break;
                    }
                }
            }
            b2 = std::sqrt(cnst3 * b2);
            a2 = dmin1 / (R{1} + b2 * b2);
            gap2 = half * dmin2 - a2;
            if (gap2 > R{0} && gap2 > b2 * a2)
            {
                s = std::max(s, a2 * (R{1} - cnst2 * a2 * (b2 / gap2) * b2));
            }
            else
            {
                s = std::max(s, a2 * (R{1} - cnst2 * b2));
                ttype = -8;
            }
        }
        else
        {
            // Case 9.
            s = qurtr * dmin1;
            if (dmin1 == dn1)
            {
                s = half * dmin1;
            }
            ttype = -9;
        }
    }
    else if (n0in == n0 + 2)
    {
        // Two eigenvalues deflated. Use DMIN2, DN2 for DMIN and DN. Cases 10 and 11.
        if (dmin2 == dn2 && R{2} * z[nn - 5] < z[nn - 7])
        {
            ttype = -10;
            s = third * dmin2;
            if (z[nn - 5] > z[nn - 7])
            {
                return;
            }
            b1 = z[nn - 5] / z[nn - 7];
            b2 = b1;
            if (b2 != R{0})
            {
                for (int i4 = 4 * n0 - 9 + pp; i4 >= 4 * i0 - 1 + pp; i4 -= 4)
                {
                    if (z[i4] > z[i4 - 2])
                    {
                        return;
                    }
                    b1 = b1 * (z[i4] / z[i4 - 2]);
                    b2 = b2 + b1;
                    if (hundrd * b1 < b2)
                    {
                        break;
                    }
                }
            }
            b2 = std::sqrt(cnst3 * b2);
            a2 = dmin2 / (R{1} + b2 * b2);
            gap2 = z[nn - 7] + z[nn - 9] - std::sqrt(z[nn - 11]) * std::sqrt(z[nn - 9]) - a2;
            if (gap2 > R{0} && gap2 > b2 * a2)
            {
                s = std::max(s, a2 * (R{1} - cnst2 * a2 * (b2 / gap2) * b2));
            }
            else
            {
                s = std::max(s, a2 * (R{1} - cnst2 * b2));
            }
        }
        else
        {
            s = qurtr * dmin2;
            ttype = -11;
        }
    }
    else if (n0in > n0 + 2)
    {
        // Case 12, more than two eigenvalues deflated. No information.
        s = R{0};
        ttype = -12;
    }

    tau = s;
}

// =======================================================================
// dlasq3 — one good dqds step on the unreduced submatrix I0:N0, with
// deflation (1 or 2 eigenvalues), the qd-array reversal heuristic, shift
// selection (dlasq4), the dqds sweep (dlasq5), failure-driven shift retry,
// and the dqd underflow fallback (dlasq6). Faithful IEEE port of dlasq3.f.
// Mutates n0, pp, sigma, desig, qmax, nfail, iter, ndiv, ttype, the dmin/dn
// family, g, tau by reference.
// =======================================================================
template <typename R>
inline void dlasq3(int i0, int& n0, Z1<R> z, int& pp, R& dmin, R& sigma, R& desig, R& qmax, int& nfail, int& iter,
                   int& ndiv, int& ttype, R& dmin1, R& dmin2, R& dn, R& dn1, R& dn2, R& g, R& tau) noexcept
{
    const R cbias = R{1.50};
    const R qurtr = R{0.250};
    const R half = R{0.5};
    const R two = R{2};
    const R eps = std::numeric_limits<R>::epsilon();
    const R tol = eps * R{100};
    const R tol2 = tol * tol;

    const int n0in = n0;

    // ---- Deflation loop ------------------------------------------------
    while (true)
    {
        if (n0 < i0)
        {
            return;
        }

        bool deflate1 = false;
        bool deflate2 = false;
        if (n0 == i0)
        {
            deflate1 = true;
        }
        else
        {
            const int nn = 4 * n0 + pp;
            if (n0 == i0 + 1)
            {
                deflate2 = true;
            }
            else if (z[nn - 5] > tol2 * (sigma + z[nn - 3]) && z[nn - 2 * pp - 4] > tol2 * z[nn - 7])
            {
                // E(N0-1) not negligible — check E(N0-2).
                if (z[nn - 9] > tol2 * sigma && z[nn - 2 * pp - 8] > tol2 * z[nn - 11])
                {
                    break;  // neither negligible → take a dqds step
                }
                deflate2 = true;
            }
            else
            {
                deflate1 = true;
            }
        }

        if (deflate1)
        {
            z[4 * n0 - 3] = z[4 * n0 + pp - 3] + sigma;
            n0 = n0 - 1;
            continue;
        }
        // deflate2: 2-by-2 block.
        const int nn = 4 * n0 + pp;
        if (z[nn - 3] > z[nn - 7])
        {
            const R sw = z[nn - 3];
            z[nn - 3] = z[nn - 7];
            z[nn - 7] = sw;
        }
        R t = half * ((z[nn - 7] - z[nn - 3]) + z[nn - 5]);
        if (z[nn - 5] > z[nn - 3] * tol2 && t != R{0})
        {
            R s = z[nn - 3] * (z[nn - 5] / t);
            if (s <= t)
            {
                s = z[nn - 3] * (z[nn - 5] / (t * (R{1} + std::sqrt(R{1} + s / t))));
            }
            else
            {
                s = z[nn - 3] * (z[nn - 5] / (t + std::sqrt(t) * std::sqrt(t + s)));
            }
            t = z[nn - 7] + (s + z[nn - 5]);
            z[nn - 3] = z[nn - 3] * (z[nn - 7] / t);
            z[nn - 7] = t;
        }
        z[4 * n0 - 7] = z[nn - 7] + sigma;
        z[4 * n0 - 3] = z[nn - 3] + sigma;
        n0 = n0 - 2;
    }

    if (pp == 2)
    {
        pp = 0;
    }

    // ---- Reverse the qd-array, if warranted ----------------------------
    if (dmin <= R{0} || n0 < n0in)
    {
        if (cbias * z[4 * i0 + pp - 3] < z[4 * n0 + pp - 3])
        {
            const int ipn4 = 4 * (i0 + n0);
            for (int j4 = 4 * i0; j4 <= 2 * (i0 + n0 - 1); j4 += 4)
            {
                R temp = z[j4 - 3];
                z[j4 - 3] = z[ipn4 - j4 - 3];
                z[ipn4 - j4 - 3] = temp;
                temp = z[j4 - 2];
                z[j4 - 2] = z[ipn4 - j4 - 2];
                z[ipn4 - j4 - 2] = temp;
                temp = z[j4 - 1];
                z[j4 - 1] = z[ipn4 - j4 - 5];
                z[ipn4 - j4 - 5] = temp;
                temp = z[j4];
                z[j4] = z[ipn4 - j4 - 4];
                z[ipn4 - j4 - 4] = temp;
            }
            if (n0 - i0 <= 4)
            {
                z[4 * n0 + pp - 1] = z[4 * i0 + pp - 1];
                z[4 * n0 - pp] = z[4 * i0 - pp];
            }
            dmin2 = std::min(dmin2, z[4 * n0 + pp - 1]);
            z[4 * n0 + pp - 1] = std::min(std::min(z[4 * n0 + pp - 1], z[4 * i0 + pp - 1]), z[4 * i0 + pp + 3]);
            z[4 * n0 - pp] = std::min(std::min(z[4 * n0 - pp], z[4 * i0 - pp]), z[4 * i0 - pp + 4]);
            qmax = std::max(std::max(qmax, z[4 * i0 + pp - 3]), z[4 * i0 + pp + 1]);
            dmin = -R{0};
        }
    }

    // ---- Choose a shift ------------------------------------------------
    dlasq4(i0, n0, z, pp, n0in, dmin, dmin1, dmin2, dn, dn1, dn2, tau, ttype, g);

    // ---- Call dqds until DMIN > 0 --------------------------------------
    bool done = false;
    int guard = 0;  // safety: bound the inner shift-retry (faithful path terminates)
    while (!done)
    {
        dlasq5(i0, n0, z, pp, tau, sigma, dmin, dmin1, dmin2, dn, dn1, dn2, eps);
        ndiv = ndiv + (n0 - i0 + 2);
        iter = iter + 1;

        if (dmin >= R{0} && dmin1 >= R{0})
        {
            done = true;  // success
        }
        else if (dmin < R{0} && dmin1 > R{0} && z[4 * (n0 - 1) - pp] < tol * (sigma + dn1) &&
                 std::abs(dn) < tol * sigma)
        {
            // Convergence hidden by negative DN.
            z[4 * (n0 - 1) - pp + 2] = R{0};
            dmin = R{0};
            done = true;
        }
        else if (dmin < R{0})
        {
            // TAU too big. Select new TAU and try again.
            nfail = nfail + 1;
            if (ttype < -22)
            {
                tau = R{0};
            }
            else if (dmin1 > R{0})
            {
                tau = (tau + dmin) * (R{1} - two * eps);
                ttype = ttype - 11;
            }
            else
            {
                tau = qurtr * tau;
                ttype = ttype - 12;
            }
        }
        else if (std::isnan(dmin))
        {
            if (tau == R{0})
            {
                dlasq6(i0, n0, z, pp, dmin, dmin1, dmin2, dn, dn1, dn2);
                ndiv = ndiv + (n0 - i0 + 2);
                iter = iter + 1;
                tau = R{0};
                done = true;
            }
            else
            {
                tau = R{0};
            }
        }
        else
        {
            // Possible underflow. Play it safe with a dqd step.
            dlasq6(i0, n0, z, pp, dmin, dmin1, dmin2, dn, dn1, dn2);
            ndiv = ndiv + (n0 - i0 + 2);
            iter = iter + 1;
            tau = R{0};
            done = true;
        }

        if (!done && ++guard > 60)
        {
            // Defensive only — never reached on the faithful path; guarantees
            // termination if a denormal pathology stalls the shift retry.
            dlasq6(i0, n0, z, pp, dmin, dmin1, dmin2, dn, dn1, dn2);
            ndiv = ndiv + (n0 - i0 + 2);
            iter = iter + 1;
            tau = R{0};
            done = true;
        }
    }

    // ---- Update the accumulated shift (compensated) --------------------
    R t;
    if (tau < sigma)
    {
        desig = desig + tau;
        t = sigma + desig;
        desig = desig - (t - sigma);
    }
    else
    {
        t = sigma + tau;
        desig = sigma - (t - tau) + desig;
    }
    sigma = t;
}

// =======================================================================
// dlasq2 — eigenvalues of the qd array Z (compact form Z[1..2n-1] =
// (q1,e1,q2,e2,...,qn) on entry; Z must have length >= 4n). Faithful IEEE
// port of dlasq2.f. On exit Z[1..n] holds the eigenvalues in DESCENDING
// order. Returns 0 on success; >0 on failure (caller falls through to
// bisection). All q's must be > 0 on entry (positive-definite qd array).
// =======================================================================
template <typename R>
[[nodiscard]] inline int dlasq2(int n, Z1<R> z) noexcept
{
    const R cbias = R{1.50};
    const R half = R{0.5};
    const R two = R{2};
    const R four = R{4};
    const R eps = std::numeric_limits<R>::epsilon();
    const R safmin = std::numeric_limits<R>::min();
    const R tol = eps * R{100};
    const R tol2 = tol * tol;

    if (n < 0)
    {
        return -1;
    }
    if (n == 0)
    {
        return 0;
    }
    if (n == 1)
    {
        return (z[1] < R{0}) ? -201 : 0;
    }
    if (n == 2)
    {
        if (z[1] < R{0} || z[2] < R{0} || z[3] < R{0})
        {
            return -2;
        }
        if (z[3] > z[1])
        {
            const R d = z[3];
            z[3] = z[1];
            z[1] = d;
        }
        z[5] = z[1] + z[2] + z[3];
        if (z[2] > z[3] * tol2)
        {
            R t = half * ((z[1] - z[3]) + z[2]);
            R s = z[3] * (z[2] / t);
            if (s <= t)
            {
                s = z[3] * (z[2] / (t * (R{1} + std::sqrt(R{1} + s / t))));
            }
            else
            {
                s = z[3] * (z[2] / (t + std::sqrt(t) * std::sqrt(t + s)));
            }
            t = z[1] + (s + z[2]);
            z[3] = z[3] * (z[1] / t);
            z[1] = t;
        }
        z[2] = z[3];
        return 0;
    }

    // Check for negative data and compute sums of q's and e's.
    z[2 * n] = R{0};
    R d = R{0};
    R e = R{0};
    for (int k = 1; k <= 2 * (n - 1); k += 2)
    {
        if (z[k] < R{0} || z[k + 1] < R{0})
        {
            return -(200 + k);
        }
        d = d + z[k];
        e = e + z[k + 1];
    }
    if (z[2 * n - 1] < R{0})
    {
        return -(200 + 2 * n - 1);
    }
    d = d + z[2 * n - 1];

    // Check for diagonality.
    if (e == R{0})
    {
        for (int k = 2; k <= n; ++k)
        {
            z[k] = z[2 * k - 1];
        }
        // Sort descending.
        for (int i = 1; i <= n - 1; ++i)
        {
            int m = i;
            for (int j = i + 1; j <= n; ++j)
            {
                if (z[j] > z[m])
                {
                    m = j;
                }
            }
            if (m != i)
            {
                const R sw = z[i];
                z[i] = z[m];
                z[m] = sw;
            }
        }
        z[2 * n - 1] = d;
        return 0;
    }

    const R trace = d + e;
    if (trace == R{0})
    {
        z[2 * n - 1] = R{0};
        return 0;
    }

    // Rearrange data for locality: Z=(q1,qq1,e1,ee1,q2,qq2,e2,ee2,...).
    for (int k = 2 * n; k >= 2; k -= 2)
    {
        z[2 * k] = R{0};
        z[2 * k - 1] = z[k];
        z[2 * k - 2] = R{0};
        z[2 * k - 3] = z[k - 1];
    }

    int i0 = 1;
    int n0 = n;

    // Reverse the qd-array, if warranted.
    if (cbias * z[4 * i0 - 3] < z[4 * n0 - 3])
    {
        const int ipn4 = 4 * (i0 + n0);
        for (int i4 = 4 * i0; i4 <= 2 * (i0 + n0 - 1); i4 += 4)
        {
            R temp = z[i4 - 3];
            z[i4 - 3] = z[ipn4 - i4 - 3];
            z[ipn4 - i4 - 3] = temp;
            temp = z[i4 - 1];
            z[i4 - 1] = z[ipn4 - i4 - 5];
            z[ipn4 - i4 - 5] = temp;
        }
    }

    // Initial split checking via dqd and Li's test.
    int pp = 0;
    R emin = z[2];
    R qmax = R{0};
    for (int k = 0; k < 2; ++k)
    {
        d = z[4 * n0 + pp - 3];
        for (int i4 = 4 * (n0 - 1) + pp; i4 >= 4 * i0 + pp; i4 -= 4)
        {
            if (z[i4 - 1] <= tol2 * d)
            {
                z[i4 - 1] = -R{0};
                d = z[i4 - 3];
            }
            else
            {
                d = z[i4 - 3] * (d / (d + z[i4 - 1]));
            }
        }
        // dqd maps Z to ZZ plus Li's test.
        emin = z[4 * i0 + pp + 1];
        d = z[4 * i0 + pp - 3];
        for (int i4 = 4 * i0 + pp; i4 <= 4 * (n0 - 1) + pp; i4 += 4)
        {
            z[i4 - 2 * pp - 2] = d + z[i4 - 1];
            if (z[i4 - 1] <= tol2 * d)
            {
                z[i4 - 1] = -R{0};
                z[i4 - 2 * pp - 2] = d;
                z[i4 - 2 * pp] = R{0};
                d = z[i4 + 1];
            }
            else if (safmin * z[i4 + 1] < z[i4 - 2 * pp - 2] && safmin * z[i4 - 2 * pp - 2] < z[i4 + 1])
            {
                const R temp = z[i4 + 1] / z[i4 - 2 * pp - 2];
                z[i4 - 2 * pp] = z[i4 - 1] * temp;
                d = d * temp;
            }
            else
            {
                z[i4 - 2 * pp] = z[i4 + 1] * (z[i4 - 1] / z[i4 - 2 * pp - 2]);
                d = z[i4 + 1] * (d / z[i4 - 2 * pp - 2]);
            }
            emin = std::min(emin, z[i4 - 2 * pp]);
        }
        z[4 * n0 - pp - 2] = d;

        // Now find qmax.
        qmax = z[4 * i0 - pp - 2];
        for (int i4 = 4 * i0 - pp + 2; i4 <= 4 * n0 - pp - 2; i4 += 4)
        {
            qmax = std::max(qmax, z[i4]);
        }
        pp = 1 - pp;
    }

    // Initialise variables to pass to DLASQ3.
    int ttype = 0;
    R dmin1 = R{0};
    R dmin2 = R{0};
    R dn = R{0};
    R dn1 = R{0};
    R dn2 = R{0};
    R g = R{0};
    R tau = R{0};

    int iter = 2;
    int nfail = 0;
    int ndiv = 2 * (n0 - i0);
    R dmin = R{0};
    R sigma = R{0};
    R desig = R{0};

    for (int iwhila = 1; iwhila <= n + 1; ++iwhila)
    {
        if (n0 < 1)
        {
            // Done: move q's to front, sort descending, store trace info.
            for (int k = 2; k <= n; ++k)
            {
                z[k] = z[4 * k - 3];
            }
            for (int i = 1; i <= n - 1; ++i)
            {
                int m = i;
                for (int j = i + 1; j <= n; ++j)
                {
                    if (z[j] > z[m])
                    {
                        m = j;
                    }
                }
                if (m != i)
                {
                    const R sw = z[i];
                    z[i] = z[m];
                    z[m] = sw;
                }
            }
            return 0;
        }

        desig = R{0};
        if (n0 == n)
        {
            sigma = R{0};
        }
        else
        {
            sigma = -z[4 * n0 - 1];
        }
        if (sigma < R{0})
        {
            return 1;
        }

        // Find last unreduced submatrix's top index I0, QMAX, EMIN.
        R emax = R{0};
        if (n0 > i0)
        {
            emin = std::abs(z[4 * n0 - 5]);
        }
        else
        {
            emin = R{0};
        }
        R qmin = z[4 * n0 - 3];
        qmax = qmin;
        int i4found = 4;
        bool broke = false;
        for (int i4 = 4 * n0; i4 >= 8; i4 -= 4)
        {
            if (z[i4 - 5] <= R{0})
            {
                i4found = i4;
                broke = true;
                break;
            }
            if (qmin >= four * emax)
            {
                qmin = std::min(qmin, z[i4 - 3]);
                emax = std::max(emax, z[i4 - 5]);
            }
            qmax = std::max(qmax, z[i4 - 7] + z[i4 - 5]);
            emin = std::min(emin, z[i4 - 5]);
        }
        if (!broke)
        {
            i4found = 4;
        }
        i0 = i4found / 4;
        pp = 0;

        if (n0 - i0 > 1)
        {
            R dee = z[4 * i0 - 3];
            R deemin = dee;
            int kmin = i0;
            for (int i4 = 4 * i0 + 1; i4 <= 4 * n0 - 3; i4 += 4)
            {
                dee = z[i4] * (dee / (dee + z[i4 - 2]));
                if (dee <= deemin)
                {
                    deemin = dee;
                    kmin = (i4 + 3) / 4;
                }
            }
            if ((kmin - i0) * 2 < n0 - kmin && deemin <= half * z[4 * n0 - 3])
            {
                const int ipn4 = 4 * (i0 + n0);
                pp = 2;
                for (int i4 = 4 * i0; i4 <= 2 * (i0 + n0 - 1); i4 += 4)
                {
                    R temp = z[i4 - 3];
                    z[i4 - 3] = z[ipn4 - i4 - 3];
                    z[ipn4 - i4 - 3] = temp;
                    temp = z[i4 - 2];
                    z[i4 - 2] = z[ipn4 - i4 - 2];
                    z[ipn4 - i4 - 2] = temp;
                    temp = z[i4 - 1];
                    z[i4 - 1] = z[ipn4 - i4 - 5];
                    z[ipn4 - i4 - 5] = temp;
                    temp = z[i4];
                    z[i4] = z[ipn4 - i4 - 4];
                    z[ipn4 - i4 - 4] = temp;
                }
            }
        }

        // Put -(initial shift) into DMIN.
        dmin = -std::max(R{0}, qmin - two * std::sqrt(qmin) * std::sqrt(emax));

        const int nbig = 100 * (n0 - i0 + 1);
        bool submatrix_done = false;
        for (int iwhilb = 1; iwhilb <= nbig; ++iwhilb)
        {
            if (i0 > n0)
            {
                submatrix_done = true;
                break;
            }

            // Take a good dqds step.
            dlasq3(i0, n0, z, pp, dmin, sigma, desig, qmax, nfail, iter, ndiv, ttype, dmin1, dmin2, dn, dn1, dn2, g,
                   tau);
            pp = 1 - pp;

            // When EMIN is very small check for splits.
            if (pp == 0 && n0 - i0 >= 3)
            {
                if (z[4 * n0] <= tol2 * qmax || z[4 * n0 - 1] <= tol2 * sigma)
                {
                    int splt = i0 - 1;
                    qmax = z[4 * i0 - 3];
                    emin = z[4 * i0 - 1];
                    R oldemn = z[4 * i0];
                    for (int i4 = 4 * i0; i4 <= 4 * (n0 - 3); i4 += 4)
                    {
                        if (z[i4] <= tol2 * z[i4 - 3] || z[i4 - 1] <= tol2 * sigma)
                        {
                            z[i4 - 1] = -sigma;
                            splt = i4 / 4;
                            qmax = R{0};
                            emin = z[i4 + 3];
                            oldemn = z[i4 + 4];
                        }
                        else
                        {
                            qmax = std::max(qmax, z[i4 + 1]);
                            emin = std::min(emin, z[i4 - 1]);
                            oldemn = std::min(oldemn, z[i4]);
                        }
                    }
                    z[4 * n0 - 1] = emin;
                    z[4 * n0] = oldemn;
                    i0 = splt + 1;
                }
            }
        }
        if (!submatrix_done)
        {
            return 2;  // max iterations exceeded
        }
    }
    return 3;
}

// =======================================================================
// build_qd_ldlt — build the qd array of  T - sigma I = L D L^T  for a
// positive-definite shift. q (1-based, length n) = the D pivots; qe (1-based,
// length n-1) = lld_i = e_{i-1}^2 / q_{i-1}. d/e are the 0-based tridiagonal.
// Returns false if any pivot is non-positive (the shift was unsafe — the
// caller must fall through to bisection; never run dqds on a degenerate qd).
// =======================================================================
template <typename R>
[[nodiscard]] inline bool build_qd_ldlt(const R* d, const R* e, int n, R sigma, R* q, R* qe) noexcept
{
    q[1] = d[0] - sigma;
    if (!(q[1] > R{0}))
    {
        return false;
    }
    for (int i = 2; i <= n; ++i)
    {
        const R off = e[i - 2];
        const R ee = (off * off) / q[i - 1];
        qe[i - 1] = ee;
        q[i] = (d[i - 1] - sigma) - ee;
        if (!(q[i] > R{0}))
        {
            return false;
        }
    }
    return true;
}

// =======================================================================
// dqd_eigenvalues_unshifted — eigenvalues of the (whole, unreduced) symmetric
// tridiagonal (d length n, e length n-1) via the UNSHIFTED dqd algorithm:
// shift by a strict Gershgorin lower bound to a PD qd array, then sweep
// dlasq6 (ping-pong) until every off-diagonal qd entry is negligible; the
// q's are then the eigenvalues of T - sigma I, so eigenvalues of T = q + sigma.
//
// This is the v3a-3.1-dqds-a milestone: it exercises the Z layout + dlasq6
// kernel WITHOUT the shift/deflation machinery (dlasq2/3/4 land at .1-dqds-b),
// so a correct result here proves the fragile ping-pong indexing is right.
// Unshifted dqd converges only linearly — use it for small/well-separated
// blocks (the shifted dlasq2 driver is the production O(n^2) path).
//
// Scratch (caller-owned, no allocation): z_scratch length >= 4*n+4 (work qd
// array), q/qe length n+1 / n. w_out length n ascending. Returns false if the
// PD shift failed (caller falls through to bisection) or iteration cap hit.
// =======================================================================
template <typename R>
[[nodiscard]] inline bool dqd_eigenvalues_unshifted(const R* d, const R* e, int n, R* z_scratch, R* q, R* qe,
                                                    R* w_out, int max_sweeps = 100000) noexcept
{
    if (n <= 0)
    {
        return false;
    }
    if (n == 1)
    {
        w_out[0] = d[0];
        return true;
    }

    R gl;
    R gu;
    gershgorin_bounds(d, e, n, gl, gu);
    const R pivmin = compute_pivmin(e, n);
    const R sigma = gl - R{2} * pivmin - R{2} * std::numeric_limits<R>::epsilon() * std::max(std::abs(gl), std::abs(gu));

    if (!build_qd_ldlt(d, e, n, sigma, q, qe))
    {
        return false;
    }

    // Lay out the 4-wide ping-pong qd array: z[4k-3] = q_k, z[4k-1] = e_k.
    Z1<R> z{z_scratch};
    for (int i = 0; i < 4 * n + 4; ++i)
    {
        z_scratch[i] = R{0};
    }
    for (int k = 1; k <= n; ++k)
    {
        z[4 * k - 3] = q[k];
    }
    for (int k = 1; k <= n - 1; ++k)
    {
        z[4 * k - 1] = qe[k];
    }

    const R eps = std::numeric_limits<R>::epsilon();
    const R tol2 = (R{100} * eps) * (R{100} * eps);

    int pp = 0;
    R dmin;
    R dmin1;
    R dmin2;
    R dn;
    R dnm1;
    R dnm2;
    bool converged = false;
    for (int sweep = 0; sweep < max_sweeps; ++sweep)
    {
        // Largest q and largest off-diagonal in the CURRENT (pp) layout.
        R qmax = R{0};
        for (int k = 1; k <= n; ++k)
        {
            qmax = std::max(qmax, z[4 * k - 3 + pp]);
        }
        R emax = R{0};
        for (int k = 1; k <= n - 1; ++k)
        {
            emax = std::max(emax, z[4 * k - 1 + pp]);
        }
        if (emax <= tol2 * qmax)
        {
            converged = true;
            break;
        }
        dlasq6(1, n, z, pp, dmin, dmin1, dmin2, dn, dnm1, dnm2);
        pp = 1 - pp;
    }

    if (!converged)
    {
        return false;
    }

    for (int k = 1; k <= n; ++k)
    {
        w_out[k - 1] = z[4 * k - 3 + pp] + sigma;
    }
    // Ascending sort (selection — n small for the unshifted path).
    for (int i = 0; i < n - 1; ++i)
    {
        int m = i;
        for (int j = i + 1; j < n; ++j)
        {
            if (w_out[j] < w_out[m])
            {
                m = j;
            }
        }
        if (m != i)
        {
            const R t = w_out[i];
            w_out[i] = w_out[m];
            w_out[m] = t;
        }
    }
    return true;
}

// =======================================================================
// dqds_eigenvalues — eigenvalues of ONE unreduced symmetric tridiagonal
// (d length n, e length n-1) via the SHIFTED dqds engine (dlasq2): strict-
// shift LDLᵀ -> compact qd array -> dlasq2 (descending) -> + sigma, reversed
// to ascending. The production O(n²) whole-block path. Returns false if the
// PD shift failed or dlasq2 did not converge (caller falls to bisection).
// Scratch (caller-owned): z_scratch length >= 4n+4, q length n+2, qe n+1.
// =======================================================================
template <typename R>
[[nodiscard]] inline bool dqds_eigenvalues(const R* d, const R* e, int n, R* z_scratch, R* q, R* qe,
                                           R* w_out) noexcept
{
    if (n <= 0)
    {
        return false;
    }
    if (n == 1)
    {
        w_out[0] = d[0];
        return true;
    }

    R gl;
    R gu;
    gershgorin_bounds(d, e, n, gl, gu);
    const R pivmin = compute_pivmin(e, n);
    const R sigma =
        gl - R{2} * pivmin - R{2} * std::numeric_limits<R>::epsilon() * std::max(std::abs(gl), std::abs(gu));
    if (!build_qd_ldlt(d, e, n, sigma, q, qe))
    {
        return false;
    }

    // Compact qd array Z[1..2n-1] = (q1,e1,q2,e2,...,qn); zero the work tail.
    for (int i = 0; i < 4 * n + 4; ++i)
    {
        z_scratch[i] = R{0};
    }
    Z1<R> z{z_scratch};
    for (int k = 1; k <= n; ++k)
    {
        z[2 * k - 1] = q[k];
    }
    for (int k = 1; k <= n - 1; ++k)
    {
        z[2 * k] = qe[k];
    }

    if (dlasq2<R>(n, z) != 0)
    {
        return false;
    }

    // Z[1..n] holds eigenvalues of (T - sigma I) DESCENDING; add sigma, reverse.
    for (int k = 0; k < n; ++k)
    {
        w_out[k] = z[n - k] + sigma;
    }
    return true;
}

// =======================================================================
// tridiag_eigenvalues_dqds — all eigenvalues of a (possibly reducible)
// symmetric tridiagonal, ascending, via split (dlarra) + per-block dqds, with
// a per-block fall-through to Sturm bisection if the dqds PD shift fails (an
// ill-conditioned block). This is the v3a-3.1 production whole-block fast
// path — O(n²), high relative accuracy.
//
// Scratch (caller-owned, no allocation): e_work/e2_work length n, isplit
// length n, z_scratch length 4n+8, q length n+2, qe length n+1. w_out asc.
// =======================================================================
template <typename R>
inline void tridiag_eigenvalues_dqds(const R* d_in, const R* e_in, int n, R* e_work, R* e2_work, int* isplit,
                                     R* z_scratch, R* q, R* qe, R* w_out, R reltol) noexcept
{
    if (n <= 0)
    {
        return;
    }
    if (n == 1)
    {
        w_out[0] = d_in[0];
        return;
    }

    const R pivmin = compute_pivmin(e_in, n);
    for (int i = 0; i < n - 1; ++i)
    {
        e_work[i] = e_in[i];
        e2_work[i] = e_in[i] * e_in[i];
    }
    e_work[n - 1] = R{0};
    e2_work[n - 1] = R{0};

    const R spltol = std::sqrt(std::numeric_limits<R>::epsilon());
    const int nsplit = tridiag_split(d_in, e_work, e2_work, n, spltol, isplit);

    int out = 0;
    int begin = 0;
    for (int b = 0; b < nsplit; ++b)
    {
        const int end = isplit[b];
        const int in = end - begin + 1;
        const R* db = d_in + begin;
        const R* eb = e_work + begin;

        if (in == 1)
        {
            w_out[out++] = db[0];
        }
        else if (dqds_eigenvalues(db, eb, in, z_scratch, q, qe, w_out + out))
        {
            out += in;
        }
        else
        {
            // Ill-conditioned block — fall through to bisection (Gershgorin).
            R gl;
            R gu;
            gershgorin_bounds(db, eb, in, gl, gu);
            const R* e2b = e2_work + begin;
            for (int k = 1; k <= in; ++k)
            {
                const EigBracket<R> ebk = bisect_eigenvalue(db, e2b, in, k, gl, gu, pivmin, reltol);
                w_out[out++] = ebk.w;
            }
        }
        begin = end + 1;
    }

    // Globally order ascending across blocks (selection sort).
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
            const R t = w_out[i];
            w_out[i] = w_out[m];
            w_out[m] = t;
        }
    }
}

} // namespace crd::hesap::dense::detail
