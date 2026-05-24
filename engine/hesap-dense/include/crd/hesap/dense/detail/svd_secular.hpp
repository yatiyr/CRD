#pragma once

#include <crd/core/types.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3b-2 — secular-equation solvers for the Gu-Eisenstat
// divide-and-conquer bidiagonal SVD (`dbdsdc`).
//
// When the merge step combines two sub-problems, the updated singular
// values are the roots of the SVD secular equation
//
//     f(sigma) = 1 + rho * sum_j  z_j^2 / (d_j^2 - sigma^2) = 0,
//
// with poles 0 <= d_0 < d_1 < ... and interlacing roots
// d_i < sigma_i < d_{i+1} (and d_{n-1} < sigma_{n-1} < sqrt(d_{n-1}^2+rho)).
// This is the SVD twin of the symmetric-eig rank-one secular equation in
// `detail/secular.hpp` (`f = 1 + rho*sum z^2/(d-lambda)`): the d_j^2 - sigma^2
// pole structure is genuinely different (NOT a d->d^2 rewrite of `dlaed4`),
// so these are faithful ports of LAPACK `dlasd5` (2x2 analytic) and `dlasd4`
// (the psi/phi split + rational interpolation), NOT adaptations of secular.hpp.
//
// Lower layer: raw f32/f64 (ADR-0078). Real T only.
//
// References (build/win-vs-ref/_deps/openblas-src/lapack-netlib/SRC/):
//   dlasd5.f  dlasd4.f  dlaed6.f
// -----------------------------------------------------------------------

// =======================================================================
// dlasd5 — the I-th singular value (i = 0 or 1) of the 2x2 problem
//   diag(d)^2 + rho * z z^T,   0 <= d[0] < d[1],  ||z|| = 1,  rho > 0.
// On exit: dsigma = sigma_i; delta[j] = d[j] - sigma_i; work[j] = d[j] + sigma_i
// (delta/work carry the gaps the singular-vector formulas consume). Faithful
// port of dlasd5.f. The shifted TAU = sigma - d[anchor] is formed without the
// catastrophic cancellation of a raw d - sigma subtraction.
// =======================================================================
template <typename R>
inline void dlasd5(int i, const R* d, const R* z, R* delta, R rho, R& dsigma, R* work) noexcept
{
    const R one = R{1};
    const R two = R{2};
    const R three = R{3};
    const R four = R{4};

    const R del = d[1] - d[0];
    const R delsq = del * (d[1] + d[0]);

    if (i == 0)
    {
        const R w = one +
                    four * rho * (z[1] * z[1] / (d[0] + three * d[1]) - z[0] * z[0] / (three * d[0] + d[1])) / del;
        if (w > R{0})
        {
            const R b = delsq + rho * (z[0] * z[0] + z[1] * z[1]);
            const R c = rho * z[0] * z[0] * delsq;
            // tau = sigma^2 - d[0]^2 ; then tau = sigma - d[0].
            R tau = two * c / (b + std::sqrt(std::abs(b * b - four * c)));
            tau = tau / (d[0] + std::sqrt(d[0] * d[0] + tau));
            dsigma = d[0] + tau;
            delta[0] = -tau;
            delta[1] = del - tau;
            work[0] = two * d[0] + tau;
            work[1] = (d[0] + tau) + d[1];
        }
        else
        {
            const R b = -delsq + rho * (z[0] * z[0] + z[1] * z[1]);
            const R c = rho * z[1] * z[1] * delsq;
            R tau;
            if (b > R{0})
            {
                tau = -two * c / (b + std::sqrt(b * b + four * c));
            }
            else
            {
                tau = (b - std::sqrt(b * b + four * c)) / two;
            }
            tau = tau / (d[1] + std::sqrt(std::abs(d[1] * d[1] + tau)));
            dsigma = d[1] + tau;
            delta[0] = -(del + tau);
            delta[1] = -tau;
            work[0] = d[0] + tau + d[1];
            work[1] = two * d[1] + tau;
        }
    }
    else  // i == 1
    {
        const R b = -delsq + rho * (z[0] * z[0] + z[1] * z[1]);
        const R c = rho * z[1] * z[1] * delsq;
        R tau;
        if (b > R{0})
        {
            tau = (b + std::sqrt(b * b + four * c)) / two;
        }
        else
        {
            tau = two * c / (-b + std::sqrt(b * b + four * c));
        }
        tau = tau / (d[1] + std::sqrt(d[1] * d[1] + tau));
        dsigma = d[1] + tau;
        delta[0] = -(del + tau);
        delta[1] = -tau;
        work[0] = d[0] + tau + d[1];
        work[1] = two * d[1] + tau;
    }
}

// =======================================================================
// dlaed6 — find the root of  f(x) = finit + sum_{j=1}^{3} z_j / (d_j - x)
// in the bracket the caller establishes, via the Gragg-Thornton-Warner
// cubic-convergent scheme. Used by dlasd4's 3-pole (SWTCH3) interpolation.
// `d3`/`z3` are length-3 (0-based here, accessed 1-based). On exit `tau` is
// the root (relative to the chosen origin); info=1 if not converged.
// Faithful port of dlaed6.f (MAXIT=40, recompute machine params per call).
// =======================================================================
template <typename R>
inline void dlaed6(int kniter, bool orgati, R rho, const R* d3, const R* z3, R finit, R& tau, int& info) noexcept
{
    const R zero = R{0};
    const R one = R{1};
    const R two = R{2};
    const R four = R{4};
    const R eight = R{8};
    const int maxit = 40;
    auto dd = [&](int j) -> R { return d3[j - 1]; };
    auto zz = [&](int j) -> R { return z3[j - 1]; };
    auto max3 = [](R a, R b, R c) { return std::max(std::max(a, b), c); };

    info = 0;
    R lbd;
    R ubd;
    if (orgati)
    {
        lbd = dd(2);
        ubd = dd(3);
    }
    else
    {
        lbd = dd(1);
        ubd = dd(2);
    }
    if (finit < zero)
    {
        lbd = zero;
    }
    else
    {
        ubd = zero;
    }

    int niter = 1;
    tau = zero;
    if (kniter == 2)
    {
        R a;
        R b;
        R c;
        R temp;
        if (orgati)
        {
            temp = (dd(3) - dd(2)) / two;
            c = rho + zz(1) / ((dd(1) - dd(2)) - temp);
            a = c * (dd(2) + dd(3)) + zz(2) + zz(3);
            b = c * dd(2) * dd(3) + zz(2) * dd(3) + zz(3) * dd(2);
        }
        else
        {
            temp = (dd(1) - dd(2)) / two;
            c = rho + zz(3) / ((dd(3) - dd(2)) - temp);
            a = c * (dd(1) + dd(2)) + zz(1) + zz(2);
            b = c * dd(1) * dd(2) + zz(1) * dd(2) + zz(2) * dd(1);
        }
        temp = max3(std::abs(a), std::abs(b), std::abs(c));
        a = a / temp;
        b = b / temp;
        c = c / temp;
        if (c == zero)
        {
            tau = b / a;
        }
        else if (a <= zero)
        {
            tau = (a - std::sqrt(std::abs(a * a - four * b * c))) / (two * c);
        }
        else
        {
            tau = two * b / (a + std::sqrt(std::abs(a * a - four * b * c)));
        }
        if (tau < lbd || tau > ubd)
        {
            tau = (lbd + ubd) / two;
        }
        if (dd(1) == tau || dd(2) == tau || dd(3) == tau)
        {
            tau = zero;
        }
        else
        {
            temp = finit + tau * zz(1) / (dd(1) * (dd(1) - tau)) + tau * zz(2) / (dd(2) * (dd(2) - tau)) +
                   tau * zz(3) / (dd(3) * (dd(3) - tau));
            if (temp <= zero)
            {
                lbd = tau;
            }
            else
            {
                ubd = tau;
            }
            if (std::abs(finit) <= std::abs(temp))
            {
                tau = zero;
            }
        }
    }

    const R eps = std::numeric_limits<R>::epsilon();
    const R safmin = std::numeric_limits<R>::min();
    // base = FLT_RADIX = 2: small1 = base^(int(log_base(safmin)/3)) = 2^(ilogb(safmin)/3).
    const int kexp = std::ilogb(safmin) / 3;
    const R small1 = std::ldexp(one, kexp);
    const R sminv1 = one / small1;
    const R small2 = small1 * small1;
    const R sminv2 = sminv1 * sminv1;

    R temp;
    if (orgati)
    {
        temp = std::min(std::abs(dd(2) - tau), std::abs(dd(3) - tau));
    }
    else
    {
        temp = std::min(std::abs(dd(1) - tau), std::abs(dd(2) - tau));
    }
    bool scale = false;
    R dscale[3];
    R zscale[3];
    R sclinv = one;
    if (temp <= small1)
    {
        scale = true;
        R sclfac;
        if (temp <= small2)
        {
            sclfac = sminv2;
            sclinv = small2;
        }
        else
        {
            sclfac = sminv1;
            sclinv = small1;
        }
        for (int j = 1; j <= 3; ++j)
        {
            dscale[j - 1] = dd(j) * sclfac;
            zscale[j - 1] = zz(j) * sclfac;
        }
        tau = tau * sclfac;
        lbd = lbd * sclfac;
        ubd = ubd * sclfac;
    }
    else
    {
        for (int j = 1; j <= 3; ++j)
        {
            dscale[j - 1] = dd(j);
            zscale[j - 1] = zz(j);
        }
    }
    auto ds = [&](int j) -> R { return dscale[j - 1]; };
    auto zs = [&](int j) -> R { return zscale[j - 1]; };

    R fc = zero;
    R df = zero;
    R ddf = zero;
    for (int j = 1; j <= 3; ++j)
    {
        const R t = one / (ds(j) - tau);
        const R t1 = zs(j) * t;
        const R t2 = t1 * t;
        const R t3 = t2 * t;
        fc = fc + t1 / ds(j);
        df = df + t2;
        ddf = ddf + t3;
    }
    R f = finit + tau * fc;

    if (std::abs(f) > zero)  // else: converged at the initial tau (goto undo-scaling)
    {
        if (f <= zero)
        {
            lbd = tau;
        }
        else
        {
            ubd = tau;
        }
        const int iter = niter + 1;
        bool done = false;
        for (niter = iter; niter <= maxit; ++niter)
        {
            R temp1;
            R temp2;
            if (orgati)
            {
                temp1 = ds(2) - tau;
                temp2 = ds(3) - tau;
            }
            else
            {
                temp1 = ds(1) - tau;
                temp2 = ds(2) - tau;
            }
            R a = (temp1 + temp2) * f - temp1 * temp2 * df;
            R b = temp1 * temp2 * f;
            R c = f - (temp1 + temp2) * df + temp1 * temp2 * ddf;
            temp = max3(std::abs(a), std::abs(b), std::abs(c));
            a = a / temp;
            b = b / temp;
            c = c / temp;
            R eta;
            if (c == zero)
            {
                eta = b / a;
            }
            else if (a <= zero)
            {
                eta = (a - std::sqrt(std::abs(a * a - four * b * c))) / (two * c);
            }
            else
            {
                eta = two * b / (a + std::sqrt(std::abs(a * a - four * b * c)));
            }
            if (f * eta >= zero)
            {
                eta = -f / df;
            }
            tau = tau + eta;
            if (tau < lbd || tau > ubd)
            {
                tau = (lbd + ubd) / two;
            }
            fc = zero;
            R erretm = zero;
            df = zero;
            ddf = zero;
            bool hitpole = false;
            for (int j = 1; j <= 3; ++j)
            {
                if ((ds(j) - tau) != zero)
                {
                    const R t = one / (ds(j) - tau);
                    const R t1 = zs(j) * t;
                    const R t2 = t1 * t;
                    const R t3 = t2 * t;
                    const R t4 = t1 / ds(j);
                    fc = fc + t4;
                    erretm = erretm + std::abs(t4);
                    df = df + t2;
                    ddf = ddf + t3;
                }
                else
                {
                    hitpole = true;
                    break;
                }
            }
            if (hitpole)
            {
                done = true;
                break;
            }
            f = finit + tau * fc;
            erretm = eight * (std::abs(finit) + std::abs(tau) * erretm) + std::abs(tau) * df;
            if (std::abs(f) <= four * eps * erretm || (ubd - lbd) <= four * eps * std::abs(tau))
            {
                done = true;
                break;
            }
            if (f <= zero)
            {
                lbd = tau;
            }
            else
            {
                ubd = tau;
            }
        }
        if (!done)
        {
            info = 1;  // ran to MAXIT without convergence
        }
    }

    if (scale)
    {
        tau = tau * sclinv;
    }
}

// =======================================================================
// dlasd4 — the I-th singular value (i = 0..n-1) of the n x n problem
//   diag(d)^2 + rho * z z^T,   0 <= d[0] < d[1] < ... ,  ||z|| = 1, rho > 0.
// Roots of f(sigma) = 1 + rho * sum_j z_j^2/(d_j^2 - sigma^2), interlacing the
// poles. On exit: sigma = sigma_i; delta[j] = d[j] - sigma; work[j] = d[j] +
// sigma (the gaps the singular-vector formulas in dlasd3 consume; their
// product delta_j*work_j = d_j^2 - sigma^2 is computed cancellation-free).
// info = 1 if the iteration did not converge. Faithful port of dlasd4.f: the
// psi/phi split + 2-pole rational step, with the 3-pole (SWTCH3) branch via
// dlaed6. MAXIT = 400. n==1/n==2 (dlasd5) handled directly.
// =======================================================================
template <typename R>
inline void dlasd4(int n, int i_in, const R* d, const R* z, R* delta, R rho, R& sigma, R* work, int& info) noexcept
{
    const R zero = R{0};
    const R one = R{1};
    const R two = R{2};
    const R three = R{3};
    const R four = R{4};
    const R eight = R{8};
    const R ten = R{10};
    const int maxit = 400;
    auto dia = [&](int j) -> R { return d[j - 1]; };
    auto zee = [&](int j) -> R { return z[j - 1]; };
    auto del = [&](int j) -> R& { return delta[j - 1]; };
    auto wrk = [&](int j) -> R& { return work[j - 1]; };

    info = 0;
    const int iv = i_in + 1;  // 1-based singular-value index (Fortran's I)

    if (n == 1)
    {
        sigma = std::sqrt(dia(1) * dia(1) + rho * zee(1) * zee(1));
        del(1) = one;
        wrk(1) = one;
        return;
    }
    if (n == 2)
    {
        dlasd5<R>(i_in, d, z, delta, rho, sigma, work);
        return;
    }

    const R eps = std::numeric_limits<R>::epsilon();
    const R rhoinv = one / rho;
    R tau2 = zero;
    R tau;
    R sgub;
    R sglb;

    // Working accumulators for the secular function and its split.
    R psi;
    R dpsi;
    R phi;
    R dphi;
    R erretm;
    R w;
    int ii;
    int iim1;
    int iip1;
    bool orgati;
    bool geomavg = false;

    if (iv == n)
    {
        // ---- The case I = N ----
        ii = n - 1;
        int niter = 1;
        R temp = rho / two;
        R temp1 = temp / (dia(n) + std::sqrt(dia(n) * dia(n) + temp));
        for (int j = 1; j <= n; ++j)
        {
            wrk(j) = dia(j) + dia(n) + temp1;
            del(j) = (dia(j) - dia(n)) - temp1;
        }
        psi = zero;
        for (int j = 1; j <= n - 2; ++j)
        {
            psi = psi + zee(j) * zee(j) / (del(j) * wrk(j));
        }
        R c = rhoinv + psi;
        w = c + zee(ii) * zee(ii) / (del(ii) * wrk(ii)) + zee(n) * zee(n) / (del(n) * wrk(n));

        if (w <= zero)
        {
            temp1 = std::sqrt(dia(n) * dia(n) + rho);
            temp = zee(n - 1) * zee(n - 1) /
                       ((dia(n - 1) + temp1) * (dia(n) - dia(n - 1) + rho / (dia(n) + temp1))) +
                   zee(n) * zee(n) / rho;
            if (c <= temp)
            {
                tau = rho;
            }
            else
            {
                const R delsq = (dia(n) - dia(n - 1)) * (dia(n) + dia(n - 1));
                const R a = -c * delsq + zee(n - 1) * zee(n - 1) + zee(n) * zee(n);
                const R b = zee(n) * zee(n) * delsq;
                if (a < zero)
                {
                    tau2 = two * b / (std::sqrt(a * a + four * b * c) - a);
                }
                else
                {
                    tau2 = (a + std::sqrt(a * a + four * b * c)) / (two * c);
                }
                tau = tau2 / (dia(n) + std::sqrt(dia(n) * dia(n) + tau2));
            }
        }
        else
        {
            const R delsq = (dia(n) - dia(n - 1)) * (dia(n) + dia(n - 1));
            const R a = -c * delsq + zee(n - 1) * zee(n - 1) + zee(n) * zee(n);
            const R b = zee(n) * zee(n) * delsq;
            if (a < zero)
            {
                tau2 = two * b / (std::sqrt(a * a + four * b * c) - a);
            }
            else
            {
                tau2 = (a + std::sqrt(a * a + four * b * c)) / (two * c);
            }
            tau = tau2 / (dia(n) + std::sqrt(dia(n) * dia(n) + tau2));
        }

        sigma = dia(n) + tau;
        for (int j = 1; j <= n; ++j)
        {
            del(j) = (dia(j) - dia(n)) - tau;
            wrk(j) = dia(j) + dia(n) + tau;
        }

        // Evaluate PSI / DPSI (j=1..ii) and PHI / DPHI (term n).
        auto eval = [&]() {
            dpsi = zero;
            psi = zero;
            erretm = zero;
            for (int j = 1; j <= ii; ++j)
            {
                const R t = zee(j) / (del(j) * wrk(j));
                psi = psi + zee(j) * t;
                dpsi = dpsi + t * t;
                erretm = erretm + psi;
            }
            erretm = std::abs(erretm);
            const R t = zee(n) / (del(n) * wrk(n));
            phi = zee(n) * t;
            dphi = t * t;
            erretm = eight * (-phi - psi) + erretm - phi + rhoinv;
            w = rhoinv + phi + psi;
        };
        eval();
        if (std::abs(w) <= eps * erretm)
        {
            return;
        }

        // First explicit step.
        niter = niter + 1;
        R dtnsq1 = wrk(n - 1) * del(n - 1);
        R dtnsq = wrk(n) * del(n);
        R c2 = w - dtnsq1 * dpsi - dtnsq * dphi;
        R a = (dtnsq + dtnsq1) * w - dtnsq * dtnsq1 * (dpsi + dphi);
        R b = dtnsq * dtnsq1 * w;
        if (c2 < zero)
        {
            c2 = std::abs(c2);
        }
        R eta;
        if (c2 == zero)
        {
            eta = rho - sigma * sigma;
        }
        else if (a >= zero)
        {
            eta = (a + std::sqrt(std::abs(a * a - four * b * c2))) / (two * c2);
        }
        else
        {
            eta = two * b / (a - std::sqrt(std::abs(a * a - four * b * c2)));
        }
        if (w * eta > zero)
        {
            eta = -w / (dpsi + dphi);
        }
        temp = eta - dtnsq;
        if (temp > rho)
        {
            eta = rho + dtnsq;
        }
        eta = eta / (sigma + std::sqrt(eta + sigma * sigma));
        tau = tau + eta;
        sigma = sigma + eta;
        for (int j = 1; j <= n; ++j)
        {
            del(j) = del(j) - eta;
            wrk(j) = wrk(j) + eta;
        }
        eval();

        // Main loop.
        const int iter = niter + 1;
        bool converged = false;
        for (niter = iter; niter <= maxit; ++niter)
        {
            if (std::abs(w) <= eps * erretm)
            {
                converged = true;
                break;
            }
            dtnsq1 = wrk(n - 1) * del(n - 1);
            dtnsq = wrk(n) * del(n);
            c2 = w - dtnsq1 * dpsi - dtnsq * dphi;
            a = (dtnsq + dtnsq1) * w - dtnsq1 * dtnsq * (dpsi + dphi);
            b = dtnsq1 * dtnsq * w;
            if (a >= zero)
            {
                eta = (a + std::sqrt(std::abs(a * a - four * b * c2))) / (two * c2);
            }
            else
            {
                eta = two * b / (a - std::sqrt(std::abs(a * a - four * b * c2)));
            }
            if (w * eta > zero)
            {
                eta = -w / (dpsi + dphi);
            }
            temp = eta - dtnsq;
            if (temp <= zero)
            {
                eta = eta / two;
            }
            eta = eta / (sigma + std::sqrt(eta + sigma * sigma));
            tau = tau + eta;
            sigma = sigma + eta;
            for (int j = 1; j <= n; ++j)
            {
                del(j) = del(j) - eta;
                wrk(j) = wrk(j) + eta;
            }
            eval();
        }
        if (!converged)
        {
            info = 1;
        }
        return;
    }

    // ---- The case I < N ----
    int niter = 1;
    const int ip1 = iv + 1;
    const R delsq = (dia(ip1) - dia(iv)) * (dia(ip1) + dia(iv));
    const R delsq2 = delsq / two;
    const R sq2 = std::sqrt((dia(iv) * dia(iv) + dia(ip1) * dia(ip1)) / two);
    R temp = delsq2 / (dia(iv) + sq2);
    for (int j = 1; j <= n; ++j)
    {
        wrk(j) = dia(j) + dia(iv) + temp;
        del(j) = (dia(j) - dia(iv)) - temp;
    }
    psi = zero;
    for (int j = 1; j <= iv - 1; ++j)
    {
        psi = psi + zee(j) * zee(j) / (wrk(j) * del(j));
    }
    phi = zero;
    for (int j = n; j >= iv + 2; --j)
    {
        phi = phi + zee(j) * zee(j) / (wrk(j) * del(j));
    }
    R c = rhoinv + psi + phi;
    w = c + zee(iv) * zee(iv) / (wrk(iv) * del(iv)) + zee(ip1) * zee(ip1) / (wrk(ip1) * del(ip1));

    if (w > zero)
    {
        // d(i)^2 < sigma_i^2 < (d(i)^2+d(i+1)^2)/2 ; origin = d(i).
        orgati = true;
        ii = iv;
        sglb = zero;
        sgub = delsq2 / (dia(iv) + sq2);
        const R a = c * delsq + zee(iv) * zee(iv) + zee(ip1) * zee(ip1);
        const R b = zee(iv) * zee(iv) * delsq;
        if (a > zero)
        {
            tau2 = two * b / (a + std::sqrt(std::abs(a * a - four * b * c)));
        }
        else
        {
            tau2 = (a - std::sqrt(std::abs(a * a - four * b * c))) / (two * c);
        }
        tau = tau2 / (dia(iv) + std::sqrt(dia(iv) * dia(iv) + tau2));
        const R t = std::sqrt(eps);
        if ((dia(iv) <= t * dia(ip1)) && (std::abs(zee(iv)) <= t) && (dia(iv) > zero))
        {
            tau = std::min(ten * dia(iv), sgub);
            geomavg = true;
        }
    }
    else
    {
        // origin = d(i+1).
        orgati = false;
        ii = ip1;
        sglb = -delsq2 / (dia(ii) + sq2);
        sgub = zero;
        const R a = c * delsq - zee(iv) * zee(iv) - zee(ip1) * zee(ip1);
        const R b = zee(ip1) * zee(ip1) * delsq;
        if (a < zero)
        {
            tau2 = two * b / (a - std::sqrt(std::abs(a * a + four * b * c)));
        }
        else
        {
            tau2 = -(a + std::sqrt(std::abs(a * a + four * b * c))) / (two * c);
        }
        tau = tau2 / (dia(ip1) + std::sqrt(std::abs(dia(ip1) * dia(ip1) + tau2)));
    }

    sigma = dia(ii) + tau;
    for (int j = 1; j <= n; ++j)
    {
        wrk(j) = dia(j) + dia(ii) + tau;
        del(j) = (dia(j) - dia(ii)) - tau;
    }
    iim1 = ii - 1;
    iip1 = ii + 1;

    // Evaluate PSI/DPSI (j=1..iim1), PHI/DPHI (j=n..iip1), and the removed-pole W.
    R dw;
    auto eval2 = [&]() {
        dpsi = zero;
        psi = zero;
        erretm = zero;
        for (int j = 1; j <= iim1; ++j)
        {
            const R t = zee(j) / (wrk(j) * del(j));
            psi = psi + zee(j) * t;
            dpsi = dpsi + t * t;
            erretm = erretm + psi;
        }
        erretm = std::abs(erretm);
        dphi = zero;
        phi = zero;
        for (int j = n; j >= iip1; --j)
        {
            const R t = zee(j) / (wrk(j) * del(j));
            phi = phi + zee(j) * t;
            dphi = dphi + t * t;
            erretm = erretm + phi;
        }
        w = rhoinv + phi + psi;
    };
    eval2();

    // W is the value of the secular function with its ii-th element removed.
    bool swtch3 = false;
    if (orgati)
    {
        if (w < zero)
        {
            swtch3 = true;
        }
    }
    else
    {
        if (w > zero)
        {
            swtch3 = true;
        }
    }
    if (ii == 1 || ii == n)
    {
        swtch3 = false;
    }

    temp = zee(ii) / (wrk(ii) * del(ii));
    dw = dpsi + dphi + temp * temp;
    temp = zee(ii) * temp;
    w = w + temp;
    erretm = eight * (phi - psi) + erretm + two * rhoinv + three * std::abs(temp);
    if (std::abs(w) <= eps * erretm)
    {
        return;
    }
    if (w <= zero)
    {
        sglb = std::max(sglb, tau);
    }
    else
    {
        sgub = std::min(sgub, tau);
    }

    // First step (NITER=2): 2-pole or 3-pole (dlaed6) interpolation.
    niter = niter + 1;
    R dd3[3];
    R zz3[3];
    {
        R eta;
        if (!swtch3)
        {
            const R dtipsq = wrk(ip1) * del(ip1);
            const R dtisq = wrk(iv) * del(iv);
            if (orgati)
            {
                const R zr = zee(iv) / dtisq;
                c = w - dtipsq * dw + delsq * (zr * zr);
            }
            else
            {
                const R zr = zee(ip1) / dtipsq;
                c = w - dtisq * dw - delsq * (zr * zr);
            }
            R a = (dtipsq + dtisq) * w - dtipsq * dtisq * dw;
            R b = dtipsq * dtisq * w;
            if (c == zero)
            {
                if (a == zero)
                {
                    if (orgati)
                    {
                        a = zee(iv) * zee(iv) + dtipsq * dtipsq * (dpsi + dphi);
                    }
                    else
                    {
                        a = zee(ip1) * zee(ip1) + dtisq * dtisq * (dpsi + dphi);
                    }
                }
                eta = b / a;
            }
            else if (a <= zero)
            {
                eta = (a - std::sqrt(std::abs(a * a - four * b * c))) / (two * c);
            }
            else
            {
                eta = two * b / (a + std::sqrt(std::abs(a * a - four * b * c)));
            }
        }
        else
        {
            const R dtiim = wrk(iim1) * del(iim1);
            const R dtiip = wrk(iip1) * del(iip1);
            const R tmp = rhoinv + psi + phi;
            if (orgati)
            {
                R temp1 = zee(iim1) / dtiim;
                temp1 = temp1 * temp1;
                c = (tmp - dtiip * (dpsi + dphi)) - (dia(iim1) - dia(iip1)) * (dia(iim1) + dia(iip1)) * temp1;
                zz3[0] = zee(iim1) * zee(iim1);
                if (dpsi < temp1)
                {
                    zz3[2] = dtiip * dtiip * dphi;
                }
                else
                {
                    zz3[2] = dtiip * dtiip * ((dpsi - temp1) + dphi);
                }
            }
            else
            {
                R temp1 = zee(iip1) / dtiip;
                temp1 = temp1 * temp1;
                c = (tmp - dtiim * (dpsi + dphi)) - (dia(iip1) - dia(iim1)) * (dia(iim1) + dia(iip1)) * temp1;
                if (dphi < temp1)
                {
                    zz3[0] = dtiim * dtiim * dpsi;
                }
                else
                {
                    zz3[0] = dtiim * dtiim * (dpsi + (dphi - temp1));
                }
                zz3[2] = zee(iip1) * zee(iip1);
            }
            zz3[1] = zee(ii) * zee(ii);
            dd3[0] = dtiim;
            dd3[1] = del(ii) * wrk(ii);
            dd3[2] = dtiip;
            int linfo = 0;
            dlaed6<R>(niter, orgati, c, dd3, zz3, w, eta, linfo);
            if (linfo != 0)
            {
                // dlaed6 failed -> fall back to 2-pole.
                swtch3 = false;
                const R dtipsq = wrk(ip1) * del(ip1);
                const R dtisq = wrk(iv) * del(iv);
                if (orgati)
                {
                    const R zr = zee(iv) / dtisq;
                    c = w - dtipsq * dw + delsq * (zr * zr);
                }
                else
                {
                    const R zr = zee(ip1) / dtipsq;
                    c = w - dtisq * dw - delsq * (zr * zr);
                }
                R a = (dtipsq + dtisq) * w - dtipsq * dtisq * dw;
                R b = dtipsq * dtisq * w;
                if (c == zero)
                {
                    if (a == zero)
                    {
                        if (orgati)
                        {
                            a = zee(iv) * zee(iv) + dtipsq * dtipsq * (dpsi + dphi);
                        }
                        else
                        {
                            a = zee(ip1) * zee(ip1) + dtisq * dtisq * (dpsi + dphi);
                        }
                    }
                    eta = b / a;
                }
                else if (a <= zero)
                {
                    eta = (a - std::sqrt(std::abs(a * a - four * b * c))) / (two * c);
                }
                else
                {
                    eta = two * b / (a + std::sqrt(std::abs(a * a - four * b * c)));
                }
            }
        }
        if (w * eta >= zero)
        {
            eta = -w / dw;
        }
        eta = eta / (sigma + std::sqrt(sigma * sigma + eta));
        temp = tau + eta;
        if (temp > sgub || temp < sglb)
        {
            if (w < zero)
            {
                eta = (sgub - tau) / two;
            }
            else
            {
                eta = (sglb - tau) / two;
            }
            if (geomavg)
            {
                if (w < zero)
                {
                    if (tau > zero)
                    {
                        eta = std::sqrt(sgub * tau) - tau;
                    }
                }
                else
                {
                    if (sglb > zero)
                    {
                        eta = std::sqrt(sglb * tau) - tau;
                    }
                }
            }
        }
        R prew = w;
        tau = tau + eta;
        sigma = sigma + eta;
        for (int j = 1; j <= n; ++j)
        {
            wrk(j) = wrk(j) + eta;
            del(j) = del(j) - eta;
        }
        eval2();
        temp = zee(ii) / (wrk(ii) * del(ii));
        dw = dpsi + dphi + temp * temp;
        temp = zee(ii) * temp;
        w = rhoinv + phi + psi + temp;
        erretm = eight * (phi - psi) + erretm + two * rhoinv + three * std::abs(temp);

        bool swtch = false;
        if (orgati)
        {
            if (-w > std::abs(prew) / ten)
            {
                swtch = true;
            }
        }
        else
        {
            if (w > std::abs(prew) / ten)
            {
                swtch = true;
            }
        }

        // Main loop.
        const int iter = niter + 1;
        bool converged = false;
        for (niter = iter; niter <= maxit; ++niter)
        {
            if (std::abs(w) <= eps * erretm)
            {
                converged = true;
                break;
            }
            if (w <= zero)
            {
                sglb = std::max(sglb, tau);
            }
            else
            {
                sgub = std::min(sgub, tau);
            }
            if (!swtch3)
            {
                const R dtipsq = wrk(ip1) * del(ip1);
                const R dtisq = wrk(iv) * del(iv);
                if (!swtch)
                {
                    if (orgati)
                    {
                        const R zr = zee(iv) / dtisq;
                        c = w - dtipsq * dw + delsq * (zr * zr);
                    }
                    else
                    {
                        const R zr = zee(ip1) / dtipsq;
                        c = w - dtisq * dw - delsq * (zr * zr);
                    }
                }
                else
                {
                    const R t = zee(ii) / (wrk(ii) * del(ii));
                    if (orgati)
                    {
                        dpsi = dpsi + t * t;
                    }
                    else
                    {
                        dphi = dphi + t * t;
                    }
                    c = w - dtisq * dpsi - dtipsq * dphi;
                }
                R a = (dtipsq + dtisq) * w - dtipsq * dtisq * dw;
                R b = dtipsq * dtisq * w;
                if (c == zero)
                {
                    if (a == zero)
                    {
                        if (!swtch)
                        {
                            if (orgati)
                            {
                                a = zee(iv) * zee(iv) + dtipsq * dtipsq * (dpsi + dphi);
                            }
                            else
                            {
                                a = zee(ip1) * zee(ip1) + dtisq * dtisq * (dpsi + dphi);
                            }
                        }
                        else
                        {
                            a = dtisq * dtisq * dpsi + dtipsq * dtipsq * dphi;
                        }
                    }
                    eta = b / a;
                }
                else if (a <= zero)
                {
                    eta = (a - std::sqrt(std::abs(a * a - four * b * c))) / (two * c);
                }
                else
                {
                    eta = two * b / (a + std::sqrt(std::abs(a * a - four * b * c)));
                }
                // apply step (shared below)
                if (w * eta >= zero)
                {
                    eta = -w / dw;
                }
                eta = eta / (sigma + std::sqrt(sigma * sigma + eta));
                temp = tau + eta;
                if (temp > sgub || temp < sglb)
                {
                    if (w < zero)
                    {
                        eta = (sgub - tau) / two;
                    }
                    else
                    {
                        eta = (sglb - tau) / two;
                    }
                    if (geomavg)
                    {
                        if (w < zero)
                        {
                            if (tau > zero)
                            {
                                eta = std::sqrt(sgub * tau) - tau;
                            }
                        }
                        else
                        {
                            if (sglb > zero)
                            {
                                eta = std::sqrt(sglb * tau) - tau;
                            }
                        }
                    }
                }
                prew = w;
                tau = tau + eta;
                sigma = sigma + eta;
                for (int j = 1; j <= n; ++j)
                {
                    wrk(j) = wrk(j) + eta;
                    del(j) = del(j) - eta;
                }
            }
            else
            {
                const R dtiim = wrk(iim1) * del(iim1);
                const R dtiip = wrk(iip1) * del(iip1);
                const R tmp = rhoinv + psi + phi;
                if (swtch)
                {
                    c = tmp - dtiim * dpsi - dtiip * dphi;
                    zz3[0] = dtiim * dtiim * dpsi;
                    zz3[2] = dtiip * dtiip * dphi;
                }
                else
                {
                    if (orgati)
                    {
                        R temp1 = zee(iim1) / dtiim;
                        temp1 = temp1 * temp1;
                        const R temp2 = (dia(iim1) - dia(iip1)) * (dia(iim1) + dia(iip1)) * temp1;
                        c = tmp - dtiip * (dpsi + dphi) - temp2;
                        zz3[0] = zee(iim1) * zee(iim1);
                        if (dpsi < temp1)
                        {
                            zz3[2] = dtiip * dtiip * dphi;
                        }
                        else
                        {
                            zz3[2] = dtiip * dtiip * ((dpsi - temp1) + dphi);
                        }
                    }
                    else
                    {
                        R temp1 = zee(iip1) / dtiip;
                        temp1 = temp1 * temp1;
                        const R temp2 = (dia(iip1) - dia(iim1)) * (dia(iim1) + dia(iip1)) * temp1;
                        c = tmp - dtiim * (dpsi + dphi) - temp2;
                        if (dphi < temp1)
                        {
                            zz3[0] = dtiim * dtiim * dpsi;
                        }
                        else
                        {
                            zz3[0] = dtiim * dtiim * (dpsi + (dphi - temp1));
                        }
                        zz3[2] = zee(iip1) * zee(iip1);
                    }
                }
                dd3[0] = dtiim;
                dd3[1] = del(ii) * wrk(ii);
                dd3[2] = dtiip;
                int linfo = 0;
                dlaed6<R>(niter, orgati, c, dd3, zz3, w, eta, linfo);
                if (linfo != 0)
                {
                    swtch3 = false;
                    const R dtipsq = wrk(ip1) * del(ip1);
                    const R dtisq = wrk(iv) * del(iv);
                    if (!swtch)
                    {
                        if (orgati)
                        {
                            const R zr = zee(iv) / dtisq;
                            c = w - dtipsq * dw + delsq * (zr * zr);
                        }
                        else
                        {
                            const R zr = zee(ip1) / dtipsq;
                            c = w - dtisq * dw - delsq * (zr * zr);
                        }
                    }
                    else
                    {
                        const R t = zee(ii) / (wrk(ii) * del(ii));
                        if (orgati)
                        {
                            dpsi = dpsi + t * t;
                        }
                        else
                        {
                            dphi = dphi + t * t;
                        }
                        c = w - dtisq * dpsi - dtipsq * dphi;
                    }
                    R a = (dtipsq + dtisq) * w - dtipsq * dtisq * dw;
                    R b = dtipsq * dtisq * w;
                    if (c == zero)
                    {
                        if (a == zero)
                        {
                            if (!swtch)
                            {
                                if (orgati)
                                {
                                    a = zee(iv) * zee(iv) + dtipsq * dtipsq * (dpsi + dphi);
                                }
                                else
                                {
                                    a = zee(ip1) * zee(ip1) + dtisq * dtisq * (dpsi + dphi);
                                }
                            }
                            else
                            {
                                a = dtisq * dtisq * dpsi + dtipsq * dtipsq * dphi;
                            }
                        }
                        eta = b / a;
                    }
                    else if (a <= zero)
                    {
                        eta = (a - std::sqrt(std::abs(a * a - four * b * c))) / (two * c);
                    }
                    else
                    {
                        eta = two * b / (a + std::sqrt(std::abs(a * a - four * b * c)));
                    }
                }
                if (w * eta >= zero)
                {
                    eta = -w / dw;
                }
                eta = eta / (sigma + std::sqrt(sigma * sigma + eta));
                temp = tau + eta;
                if (temp > sgub || temp < sglb)
                {
                    if (w < zero)
                    {
                        eta = (sgub - tau) / two;
                    }
                    else
                    {
                        eta = (sglb - tau) / two;
                    }
                    if (geomavg)
                    {
                        if (w < zero)
                        {
                            if (tau > zero)
                            {
                                eta = std::sqrt(sgub * tau) - tau;
                            }
                        }
                        else
                        {
                            if (sglb > zero)
                            {
                                eta = std::sqrt(sglb * tau) - tau;
                            }
                        }
                    }
                }
                prew = w;
                tau = tau + eta;
                sigma = sigma + eta;
                for (int j = 1; j <= n; ++j)
                {
                    wrk(j) = wrk(j) + eta;
                    del(j) = del(j) - eta;
                }
            }

            // Re-evaluate PSI/PHI and W after the step.
            eval2();
            tau2 = wrk(ii) * del(ii);
            temp = zee(ii) / tau2;
            dw = dpsi + dphi + temp * temp;
            temp = zee(ii) * temp;
            w = rhoinv + phi + psi + temp;
            erretm = eight * (phi - psi) + erretm + two * rhoinv + three * std::abs(temp);
            if (w * prew > zero && std::abs(w) > std::abs(prew) / ten)
            {
                swtch = !swtch;
            }
        }
        if (!converged)
        {
            info = 1;
        }
    }
}

} // namespace crd::hesap::dense::detail
