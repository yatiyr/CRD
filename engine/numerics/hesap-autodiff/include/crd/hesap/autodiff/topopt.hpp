#pragma once

// topopt.hpp — Phase 3.1.6 v16-j: adjoint TOPOLOGY OPTIMIZATION (density-based SIMP) — the applied face of the v16-g
// implicit-differentiation lane. Minimise structural compliance c(ρ)=fᵀu s.t. K(ρ)u=f (a Q4 plane-stress FEA) and a
// volume constraint, with the gradient by the DISCRETE ADJOINT. For self-adjoint compliance the adjoint collapses to
// the classic SIMP sensitivity `dc/dρ_e = −p·ρ_e^{p−1}(E0−Emin)·u_eᵀ·KE·u_e` (differentiate the SOLUTION K⁻¹f, never
// the CG iterations — the IFT of the linear solve). Self-contained: a matrix-free Jacobi-PCG solve, the top88 KE, a
// density/sensitivity filter, and an OC update. Gated by the dolfin-adjoint-class **Taylor-remainder** test (the
// gradient is 2nd-order-correct) + top88 compliance parity. Deterministic. ADR-0097.

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::autodiff::topopt
{

// The standard Q4 plane-stress unit-element stiffness (top88/top99), interleaved DOF order [u0 v0 u1 v1 u2 v2 u3 v3],
// nodes CCW. ke is 8×8 row-major; scale by the element Young's modulus at assembly.
inline void q4_ke(crd::f64 nu, crd::f64* ke) noexcept
{
    const crd::f64 k[8] = {0.5 - nu / 6.0,        0.125 + nu / 8.0,     -0.25 - nu / 12.0, -0.125 + 3.0 * nu / 8.0,
                           -0.25 + nu / 12.0,      -0.125 - nu / 8.0,    nu / 6.0,          0.125 - 3.0 * nu / 8.0};
    static const int kIdx[64] = {0, 1, 2, 3, 4, 5, 6, 7, 1, 0, 7, 6, 5, 4, 3, 2, 2, 7, 0, 5, 6, 3, 4, 1, 3, 6, 5, 0, 7, 2, 1, 4,
                                 4, 5, 6, 7, 0, 1, 2, 3, 5, 4, 3, 2, 1, 0, 7, 6, 6, 3, 4, 1, 2, 7, 0, 5, 7, 2, 1, 4, 3, 6, 5, 0};
    const crd::f64 s = 1.0 / (1.0 - nu * nu);
    for (int i = 0; i < 64; ++i) { ke[i] = s * k[kIdx[i]]; }
}

// The mesh + design problem. Node (ix,iy): node = ix*(nely+1)+iy ; DOF = [2*node, 2*node+1]. MBB beam BCs: left edge
// x-fixed (symmetry), bottom-right corner y-fixed, unit downward load at the top-left node.
struct Problem
{
    int      nelx;
    int      nely;
    crd::f64 penal; // SIMP exponent (3)
    crd::f64 emin;  // void stiffness floor (1e-9)
    crd::f64 e0;    // solid modulus (1)
    crd::f64 nu;    // Poisson (0.3)
    [[nodiscard]] int nel() const noexcept { return nelx * nely; }
    [[nodiscard]] int ndof() const noexcept { return 2 * (nelx + 1) * (nely + 1); }
    void edof(int ex, int ey, int* e) const noexcept // 8 global DOFs of element (ex,ey)
    {
        const int n0 = ex * (nely + 1) + ey;
        const int n1 = (ex + 1) * (nely + 1) + ey;
        const int n2 = (ex + 1) * (nely + 1) + (ey + 1);
        const int n3 = ex * (nely + 1) + (ey + 1);
        e[0] = 2 * n0; e[1] = 2 * n0 + 1; e[2] = 2 * n1; e[3] = 2 * n1 + 1;
        e[4] = 2 * n2; e[5] = 2 * n2 + 1; e[6] = 2 * n3; e[7] = 2 * n3 + 1;
    }
    [[nodiscard]] crd::f64 emod(crd::f64 rho) const noexcept
    {
        return emin + crd::math::pow(rho, penal) * (e0 - emin);
    }
};

// matrix-free K(ρ)·x with fixed DOFs zeroed: out = K x. `free[d]` = 1 if free, 0 if fixed. ke = q4_ke.
inline void kmatvec(const Problem& p, const crd::f64* rho, const crd::f64* ke, const crd::u8* freem, const crd::f64* x,
                    crd::f64* out) noexcept
{
    const int nd = p.ndof();
    for (int d = 0; d < nd; ++d) { out[d] = 0.0; }
    int e[8];
    crd::f64 xe[8];
    for (int ex = 0; ex < p.nelx; ++ex)
    {
        for (int ey = 0; ey < p.nely; ++ey)
        {
            p.edof(ex, ey, e);
            const crd::f64 em = p.emod(rho[ex * p.nely + ey]);
            for (int i = 0; i < 8; ++i) { xe[i] = freem[e[i]] ? x[e[i]] : 0.0; }
            for (int i = 0; i < 8; ++i)
            {
                crd::f64 acc = 0.0;
                for (int j = 0; j < 8; ++j) { acc += ke[i * 8 + j] * xe[j]; }
                out[e[i]] += em * acc;
            }
        }
    }
    for (int d = 0; d < nd; ++d) { if (!freem[d]) { out[d] = 0.0; } }
}

// half-bandwidth of the symmetric FEA K under this DOF numbering: an element's DOFs span node(ex+1,ey+1)−node(ex,ey)
// = nely+2 nodes ⇒ 2*nely+5 DOFs.
[[nodiscard]] inline int band_hw(const Problem& p) noexcept { return 2 * p.nely + 5; }

// DIRECT banded-Cholesky solve of K(ρ)u=f — EXACT and condition-INDEPENDENT (unlike CG, immune to the SIMP
// void-element ill-conditioning that dominates topopt). K is symmetric with half-bandwidth `band_hw`; BCs by
// row/column elimination. scratch: band (ndof*(band_hw+1)) + y (ndof). Deterministic.
inline void solve(const Problem& p, const crd::f64* rho, const crd::f64* ke, const crd::u8* freem, const crd::f64* f,
                  crd::f64* u, crd::f64* band, crd::f64* y) noexcept
{
    const int nd = p.ndof();
    const int bw = band_hw(p);
    const int w  = bw + 1;
    for (int i = 0; i < nd * w; ++i) { band[i] = 0.0; }
    int e[8];
    for (int ex = 0; ex < p.nelx; ++ex) // assemble the lower band
    {
        for (int ey = 0; ey < p.nely; ++ey)
        {
            p.edof(ex, ey, e);
            const crd::f64 em = p.emod(rho[ex * p.nely + ey]);
            for (int i = 0; i < 8; ++i)
            {
                for (int j = 0; j < 8; ++j)
                {
                    if (e[i] >= e[j]) { band[e[i] * w + (e[j] - e[i] + bw)] += em * ke[i * 8 + j]; }
                }
            }
        }
    }
    for (int d = 0; d < nd; ++d) // BCs: fixed DOF → identity row + column
    {
        if (freem[d]) { continue; }
        for (int k = 0; k < w; ++k) { band[d * w + k] = 0.0; }
        band[d * w + bw] = 1.0;
        for (int i = d + 1; i <= d + bw && i < nd; ++i) { band[i * w + (d - i + bw)] = 0.0; }
    }
    for (int i = 0; i < nd; ++i) // banded Cholesky (lower L·Lᵀ), in place
    {
        const int lo = i - bw > 0 ? i - bw : 0;
        for (int j = lo; j <= i; ++j)
        {
            crd::f64 s = band[i * w + (j - i + bw)];
            for (int k = lo; k < j; ++k) { s -= band[i * w + (k - i + bw)] * band[j * w + (k - j + bw)]; }
            if (j == i) { band[i * w + bw] = crd::math::sqrt(s); }
            else { band[i * w + (j - i + bw)] = s / band[j * w + bw]; }
        }
    }
    for (int i = 0; i < nd; ++i) // forward: L y = f
    {
        const int lo = i - bw > 0 ? i - bw : 0;
        crd::f64  s  = f[i];
        for (int j = lo; j < i; ++j) { s -= band[i * w + (j - i + bw)] * y[j]; }
        y[i] = s / band[i * w + bw];
    }
    for (int i = nd - 1; i >= 0; --i) // back: Lᵀ u = y
    {
        crd::f64 s = y[i];
        for (int j = i + 1; j <= i + bw && j < nd; ++j) { s -= band[j * w + (i - j + bw)] * u[j]; }
        u[i] = s / band[i * w + bw];
    }
}

// compliance c = fᵀu and, if dc != nullptr, the SIMP adjoint sensitivity dc/dρ_e (length nel).
inline crd::f64 compliance(const Problem& p, const crd::f64* rho, const crd::f64* ke, const crd::f64* f,
                           const crd::f64* u, crd::f64* dc) noexcept
{
    const int nd = p.ndof();
    crd::f64  c  = 0.0;
    for (int d = 0; d < nd; ++d) { c += f[d] * u[d]; }
    if (dc != nullptr)
    {
        int e[8];
        for (int ex = 0; ex < p.nelx; ++ex)
        {
            for (int ey = 0; ey < p.nely; ++ey)
            {
                p.edof(ex, ey, e);
                crd::f64 ue[8];
                for (int i = 0; i < 8; ++i) { ue[i] = u[e[i]]; }
                crd::f64 ueke = 0.0; // u_eᵀ KE u_e
                for (int i = 0; i < 8; ++i)
                {
                    crd::f64 row = 0.0;
                    for (int j = 0; j < 8; ++j) { row += ke[i * 8 + j] * ue[j]; }
                    ueke += ue[i] * row;
                }
                const crd::f64 rho_e = rho[ex * p.nely + ey];
                dc[ex * p.nely + ey] = -p.penal * crd::math::pow(rho_e, p.penal - 1.0) * (p.e0 - p.emin) * ueke;
            }
        }
    }
    return c;
}

// sensitivity/density filter weights are the linear hat H_ef = max(0, rmin − dist). Applies the top88 SENSITIVITY
// filter in place: dc_e ← (Σ_f H_ef ρ_f dc_f) / (max(1e-3,ρ_e) Σ_f H_ef). scratch out (nel).
inline void sens_filter(const Problem& p, const crd::f64* rho, crd::f64 rmin, crd::f64* dc, crd::f64* out) noexcept
{
    const int rr = static_cast<int>(crd::math::ceil(rmin)) - 1;
    for (int ex = 0; ex < p.nelx; ++ex)
    {
        for (int ey = 0; ey < p.nely; ++ey)
        {
            crd::f64 num = 0.0;
            crd::f64 den = 0.0;
            for (int fx = (ex - rr > 0 ? ex - rr : 0); fx <= (ex + rr < p.nelx - 1 ? ex + rr : p.nelx - 1); ++fx)
            {
                for (int fy = (ey - rr > 0 ? ey - rr : 0); fy <= (ey + rr < p.nely - 1 ? ey + rr : p.nely - 1); ++fy)
                {
                    const crd::f64 dist = crd::math::sqrt(static_cast<crd::f64>((ex - fx) * (ex - fx) + (ey - fy) * (ey - fy)));
                    const crd::f64 w    = rmin - dist;
                    if (w > 0.0)
                    {
                        num += w * rho[fx * p.nely + fy] * dc[fx * p.nely + fy];
                        den += w;
                    }
                }
            }
            const crd::f64 re = rho[ex * p.nely + ey];
            out[ex * p.nely + ey] = num / ((re > 1e-3 ? re : 1e-3) * den);
        }
    }
    for (int i = 0; i < p.nel(); ++i) { dc[i] = out[i]; }
}

// OC update (bisection on the Lagrange multiplier) → rho_new, uniform volume sensitivity, move limit 0.2.
inline void oc_update(const Problem& p, const crd::f64* rho, const crd::f64* dc, crd::f64 volfrac, crd::f64* rho_new) noexcept
{
    const int      n    = p.nel();
    const crd::f64 move = 0.2;
    crd::f64       l1   = 0.0;
    crd::f64       l2   = 1e9;
    while ((l2 - l1) / (l1 + l2) > 1e-4)
    {
        const crd::f64 lmid = 0.5 * (l1 + l2);
        crd::f64       vol  = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const crd::f64 be  = -dc[i] / lmid; // dv=1
            crd::f64       cand = rho[i] * crd::math::sqrt(be > 0.0 ? be : 0.0);
            if (cand > rho[i] + move) { cand = rho[i] + move; }
            if (cand > 1.0) { cand = 1.0; }
            if (cand < rho[i] - move) { cand = rho[i] - move; }
            if (cand < 0.0) { cand = 0.0; }
            rho_new[i] = cand;
            vol += cand;
        }
        if (vol > volfrac * n) { l1 = lmid; } else { l2 = lmid; }
    }
}

// Full SIMP optimisation loop: uniform init at volfrac, OC updates until the design change < 0.01 or maxiter. Returns
// the final compliance and fills `rho`. Caller scratch: u/y (ndof), band (ndof*(band_hw+1)), dc/dcf/rho_new (nel).
inline crd::f64 optimize(const Problem& p, crd::f64 volfrac, crd::f64 rmin, int maxiter, const crd::f64* ke,
                         const crd::u8* freem, const crd::f64* f, crd::f64* rho, crd::f64* u, crd::f64* band,
                         crd::f64* y, crd::f64* dc, crd::f64* dcf, crd::f64* rho_new) noexcept
{
    const int n = p.nel();
    for (int i = 0; i < n; ++i) { rho[i] = volfrac; }
    crd::f64 c = 0.0;
    for (int iter = 0; iter < maxiter; ++iter)
    {
        solve(p, rho, ke, freem, f, u, band, y);
        c = compliance(p, rho, ke, f, u, dc);
        sens_filter(p, rho, rmin, dc, dcf); // dc filtered in place
        oc_update(p, rho, dc, volfrac, rho_new);
        crd::f64 change = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const crd::f64 d = rho_new[i] - rho[i];
            const crd::f64 ad = d < 0.0 ? -d : d;
            if (ad > change) { change = ad; }
            rho[i] = rho_new[i];
        }
        if (change < 0.01) { break; }
    }
    return c;
}

} // namespace crd::hesap::autodiff::topopt
