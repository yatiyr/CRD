#pragma once

// kan.hpp -- Phase 3.1.6 v16-k (part 2): KOLMOGOROV-ARNOLD NETWORK (KAN, ICLR 2025) with the **Efficient-KAN
// restructuring**. Each edge (i,j) carries a learnable 1-D function φ_ij(x) = wb_ij·silu(x) + Σ_g ws_ijg·B_g(x)
// (a base + a B-spline). The naive form re-evaluates every edge's spline separately -- O(din·dout·nbasis) basis work.
// The KEY restructuring: the spline is LINEAR in its coefficients and the B-spline basis B_g(x_i) depends only on the
// INPUT i (not the output j), so compute the basis ONCE per input (`kan_forward` builds `Bmat` = din×nbasis) and let
// the layer be two MATMULs `y = Wb·silu(x) + Ws·vec(B(x))` -- O(din·nbasis) basis work + a GEMM, reused across all
// dout. Bit-identical to the naive form, ~dout× less basis work. The reverse pass (`kan_vjp`) uses the B-spline
// derivative (a degree-(p−1) combination) so deep KANs differentiate through the edge nonlinearity. Self-contained;
// deterministic. ADR-0097.

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::autodiff::kan
{

constexpr int kMaxBasis = 64; // G + 2p working room for the de-Boor recursion
constexpr int kMaxIn    = 32; // max layer input width (Bmat scratch = kMaxIn * nbasis)

// uniform open-knot grid on [gmin,gmax], G intervals, degree p: grid[i] = gmin + (i−p)·h, i=0..G+2p (G+2p+1 points).
inline void make_grid(crd::f64 gmin, crd::f64 gmax, int gcount, int p, crd::f64* grid) noexcept
{
    const crd::f64 h = (gmax - gmin) / static_cast<crd::f64>(gcount);
    for (int i = 0; i <= gcount + 2 * p; ++i) { grid[i] = gmin + static_cast<crd::f64>(i - p) * h; }
}

// B-spline basis of degree p at x: fills B[0..nbasis) with B_g^p(x) and, if dB != nullptr, dB_g^p/dx. nbasis = G+p.
inline void bspline(crd::f64 x, const crd::f64* grid, int nbasis, int p, crd::f64* b_out, crd::f64* db_out) noexcept
{
    crd::f64  b[kMaxBasis];
    const int n0 = nbasis + p; // G + 2p degree-0 functions
    for (int g = 0; g < n0; ++g) { b[g] = (grid[g] <= x && x < grid[g + 1]) ? 1.0 : 0.0; }
    for (int k = 1; k <= p - 1; ++k) // recurse to degree p−1
    {
        for (int g = 0; g < n0 - k; ++g)
        {
            const crd::f64 d1 = grid[g + k] - grid[g];
            const crd::f64 d2 = grid[g + k + 1] - grid[g + 1];
            const crd::f64 t1 = d1 > 0.0 ? (x - grid[g]) / d1 : 0.0;
            const crd::f64 t2 = d2 > 0.0 ? (grid[g + k + 1] - x) / d2 : 0.0;
            b[g] = t1 * b[g] + t2 * b[g + 1];
        }
    }
    for (int g = 0; g < nbasis; ++g) // final degree-p combination + its derivative
    {
        const crd::f64 d1 = grid[g + p] - grid[g];
        const crd::f64 d2 = grid[g + p + 1] - grid[g + 1];
        const crd::f64 t1 = d1 > 0.0 ? (x - grid[g]) / d1 : 0.0;
        const crd::f64 t2 = d2 > 0.0 ? (grid[g + p + 1] - x) / d2 : 0.0;
        b_out[g] = t1 * b[g] + t2 * b[g + 1];
        if (db_out != nullptr)
        {
            db_out[g] = static_cast<crd::f64>(p) * ((d1 > 0.0 ? b[g] / d1 : 0.0) - (d2 > 0.0 ? b[g + 1] / d2 : 0.0));
        }
    }
}

[[nodiscard]] inline crd::f64 silu(crd::f64 x) noexcept { const crd::f64 s = 1.0 / (1.0 + crd::math::exp(-x)); return x * s; }
[[nodiscard]] inline crd::f64 silu_der(crd::f64 x) noexcept
{
    const crd::f64 s = 1.0 / (1.0 + crd::math::exp(-x));
    return s + x * s * (1.0 - s);
}

// EFFICIENT forward: y[j] = Σ_i wb[j][i]·silu(x_i) + Σ_i Σ_g ws[j][i][g]·B_g(x_i). Basis computed ONCE per input into
// Bmat (din*nbasis), then two matmuls. wb (dout*din), ws (dout*din*nbasis).
inline void kan_forward(int din, int dout, int nbasis, int p, const crd::f64* wb, const crd::f64* ws,
                        const crd::f64* grid, const crd::f64* x, crd::f64* y) noexcept
{
    crd::f64 base[kMaxIn];
    crd::f64 bmat[kMaxIn * kMaxBasis];
    for (int i = 0; i < din; ++i) { base[i] = silu(x[i]); bspline(x[i], grid, nbasis, p, bmat + i * nbasis, nullptr); }
    for (int j = 0; j < dout; ++j)
    {
        crd::f64 s = 0.0;
        for (int i = 0; i < din; ++i) // interleave per input (matches kan_forward_naive's order ⇒ bit-identical)
        {
            s += wb[j * din + i] * base[i];
            const crd::f64* wrow = ws + (static_cast<crd::usize>(j) * din + i) * nbasis;
            const crd::f64* brow = bmat + i * nbasis;
            for (int g = 0; g < nbasis; ++g) { s += wrow[g] * brow[g]; }
        }
        y[j] = s;
    }
}

// NAIVE forward (re-evaluate each edge's spline separately) -- for the efficient-vs-naive crush; SAME result.
inline void kan_forward_naive(int din, int dout, int nbasis, int p, const crd::f64* wb, const crd::f64* ws,
                              const crd::f64* grid, const crd::f64* x, crd::f64* y) noexcept
{
    for (int j = 0; j < dout; ++j)
    {
        crd::f64 s = 0.0;
        for (int i = 0; i < din; ++i)
        {
            crd::f64 bi[kMaxBasis];
            bspline(x[i], grid, nbasis, p, bi, nullptr); // recomputed for EVERY j -- the waste
            s += wb[j * din + i] * silu(x[i]);
            const crd::f64* wrow = ws + (static_cast<crd::usize>(j) * din + i) * nbasis;
            for (int g = 0; g < nbasis; ++g) { s += wrow[g] * bi[g]; }
        }
        y[j] = s;
    }
}

// reverse pass: given dL/dy, ACCUMULATE dL/dwb (gwb), dL/dws (gws), and fill dL/dx (gx, for deep KANs).
inline void kan_vjp(int din, int dout, int nbasis, int p, const crd::f64* wb, const crd::f64* ws, const crd::f64* grid,
                    const crd::f64* x, const crd::f64* dy, crd::f64* gwb, crd::f64* gws, crd::f64* gx) noexcept
{
    crd::f64 base[kMaxIn];
    crd::f64 dbase[kMaxIn];
    crd::f64 bmat[kMaxIn * kMaxBasis];
    crd::f64 dbmat[kMaxIn * kMaxBasis];
    for (int i = 0; i < din; ++i)
    {
        base[i]  = silu(x[i]);
        dbase[i] = silu_der(x[i]);
        bspline(x[i], grid, nbasis, p, bmat + i * nbasis, dbmat + i * nbasis);
    }
    for (int j = 0; j < dout; ++j)
    {
        for (int i = 0; i < din; ++i) { gwb[j * din + i] += dy[j] * base[i]; }
        for (int i = 0; i < din; ++i)
        {
            crd::f64* grow = gws + (static_cast<crd::usize>(j) * din + i) * nbasis;
            const crd::f64* brow = bmat + i * nbasis;
            for (int g = 0; g < nbasis; ++g) { grow[g] += dy[j] * brow[g]; }
        }
    }
    for (int i = 0; i < din; ++i)
    {
        crd::f64 gxi = 0.0;
        for (int j = 0; j < dout; ++j)
        {
            crd::f64        e    = wb[j * din + i] * dbase[i];
            const crd::f64* wrow = ws + (static_cast<crd::usize>(j) * din + i) * nbasis;
            const crd::f64* drow = dbmat + i * nbasis;
            for (int g = 0; g < nbasis; ++g) { e += wrow[g] * drow[g]; }
            gxi += dy[j] * e;
        }
        gx[i] = gxi;
    }
}

} // namespace crd::hesap::autodiff::kan
