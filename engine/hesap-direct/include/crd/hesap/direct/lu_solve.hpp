#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/direct/dense_lu_kernels.hpp>
#include <crd/hesap/direct/supernodal_lu.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::direct
{
// =======================================================================
// Shared static-pivot LU SOLVE (the single path for SupernodalLU + MultifrontalLU).
//
// Extracted verbatim from the PROVEN supernodal_lu.cpp solve (the 2026-06-01 Demmel-Li
// stagnation-fixed iterative refinement). Both static-pivot LU factorizations produce the
// SAME CSC L/U (unit-lower L diagonal-first, upper U diagonal-last) + the SAME MC64
// StaticLuScaling transform + keep B (CSC) for the IR true residual ⇒ ONE solve, no dual path.
// =======================================================================

// In-place L\ then U\ on z (length n). L unit-lower CSC (lp/li/lx, unit diagonal first per
// column); U upper CSC (up/ui/ux, diagonal last per column). z = RHS in, solution out.
template <typename T>
inline void lu_lu_solve(crd::u32 n, const crd::u32* lp, const crd::u32* li, const T* lx, const crd::u32* up,
                        const crd::u32* ui, const T* ux, T* z) noexcept
{
    for (crd::u32 j = 0; j < n; ++j) // forward: L\z (unit lower)
    {
        const T zj = z[j];
        for (crd::u32 p = lp[j] + 1; p < lp[j + 1]; ++p) // skip the unit diagonal
        {
            z[li[p]] = z[li[p]] - lx[p] * zj;
        }
    }
    for (crd::u32 jj = 0; jj < n; ++jj) // backward: U\z (upper)
    {
        const crd::u32 j = n - 1 - jj;
        const crd::u32 pdiag = up[j + 1] - 1; // U(j,j) = last entry of column j
        z[j] = z[j] / ux[pdiag];
        const T zj = z[j];
        for (crd::u32 p = up[j]; p < pdiag; ++p) // off-diagonals (rows < j)
        {
            z[ui[p]] = z[ui[p]] - ux[p] * zj;
        }
    }
}

// Solve A·X = B in place for a static-pivot LU. `rhs` is column-major n × nrhs (B in, X out).
// lp/li/lx + up/ui/ux are the CSC factors; bp/bi/bx is B (CSC, the MC64-transformed matrix) for
// the IR true residual; `scale` is the MC64 StaticLuScaling. Returns false if a column's residual
// fails the accept_tol gate (static factor diverged — indefinite/saddle-point: do NOT trust X).
template <typename T>
[[nodiscard]] inline bool static_lu_ir_solve(crd::u32 n, const crd::u32* lp, const crd::u32* li, const T* lx,
                                             const crd::u32* up, const crd::u32* ui, const T* ux, const crd::u32* bp,
                                             const crd::u32* bi, const T* bx, const StaticLuScaling<T>& scale,
                                             crd::containers::Span<T> rhs, crd::usize nrhs,
                                             crd::memory::IAllocator* alloc)
{
    if (n == 0)
    {
        return true;
    }
    CRD_ASSERT_MSG(rhs.size() == static_cast<crd::usize>(n) * nrhs, "static_lu_ir_solve rhs size mismatch");

    crd::containers::Array<T> c(alloc);
    crd::containers::Array<T> y(alloc);
    crd::containers::Array<T> r(alloc);
    c.resize(n);
    y.resize(n);
    r.resize(n);
    const dense::RealType<T> eps = std::numeric_limits<dense::RealType<T>>::epsilon();
    const dense::RealType<T> refine_tol = dense::RealType<T>(64) * eps;
    // Acceptance gate: static pivoting can DIVERGE on indefinite/saddle-point systems where IR never
    // recovers. Below this bound the solution is trustworthy; above it, return false rather than
    // silently returning a wrong answer (such systems need the dynamic-pivot or saddle-point path).
    const dense::RealType<T> accept_tol = std::sqrt(eps); // ~1.5e-8
    bool ok = true;

    for (crd::usize col = 0; col < nrhs; ++col)
    {
        T* b = rhs.data() + col * n;
        scale.transform_rhs({b, n}, {c.data(), n}); // c = D_r·b
        for (crd::u32 i = 0; i < n; ++i)
        {
            y[i] = c[i];
        }
        lu_lu_solve<T>(n, lp, li, lx, up, ui, ux, y.data()); // B·y = c (static pivot ⇒ approximate)
        // Iterative refinement on the transformed system B·y = c (Demmel-Li GESP) — drives the
        // TRUE residual to machine precision so the bench compares at a matched residual.
        bool converged = false;
        dense::RealType<T> prev_rn = std::numeric_limits<dense::RealType<T>>::max(); // IR stagnation tracker
        for (crd::u32 it = 0; it < kLuRefineMax; ++it)
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                r[i] = c[i];
            }
            for (crd::u32 j = 0; j < n; ++j) // r -= B·y (B in CSC)
            {
                const T yj = y[j];
                for (crd::u32 p = bp[j]; p < bp[j + 1]; ++p)
                {
                    r[bi[p]] = r[bi[p]] - bx[p] * yj;
                }
            }
            dense::RealType<T> rn = dense::RealType<T>(0);
            dense::RealType<T> cn = dense::RealType<T>(0);
            for (crd::u32 i = 0; i < n; ++i)
            {
                const dense::RealType<T> rm = lu2_mag<T>(r[i]);
                if (rm > rn)
                {
                    rn = rm;
                }
                const dense::RealType<T> cm = lu2_mag<T>(c[i]);
                if (cm > cn)
                {
                    cn = cm;
                }
            }
            const dense::RealType<T> cnorm = (cn > dense::RealType<T>(0) ? cn : dense::RealType<T>(1));
            if (rn <= refine_tol * cnorm)
            {
                converged = true; // machine-precision on the transformed system B·y = c
                break;
            }
            // Stagnation guard (Demmel-Li GESP): once the residual stops improving by >=2x per step it
            // has hit the static-pivot round-off floor. Stop, leaving converged=false so the post-loop
            // accept_tol recheck still flags genuine divergence (saddle-point systems stall at O(1)).
            if (it >= 1U && rn >= static_cast<dense::RealType<T>>(0.5) * prev_rn)
            {
                break;
            }
            prev_rn = rn;
            lu_lu_solve<T>(n, lp, li, lx, up, ui, ux, r.data()); // dy = (LU)\r, in place
            for (crd::u32 i = 0; i < n; ++i)
            {
                y[i] = y[i] + r[i];
            }
        }
        if (!converged) // IR didn't reach refine_tol — recheck the FINAL y's residual
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                r[i] = c[i];
            }
            for (crd::u32 j = 0; j < n; ++j)
            {
                const T yj = y[j];
                for (crd::u32 p = bp[j]; p < bp[j + 1]; ++p)
                {
                    r[bi[p]] = r[bi[p]] - bx[p] * yj;
                }
            }
            dense::RealType<T> rn2 = dense::RealType<T>(0);
            dense::RealType<T> cn2 = dense::RealType<T>(0);
            for (crd::u32 i = 0; i < n; ++i)
            {
                const dense::RealType<T> rm = lu2_mag<T>(r[i]);
                if (rm > rn2)
                {
                    rn2 = rm;
                }
                const dense::RealType<T> cm = lu2_mag<T>(c[i]);
                if (cm > cn2)
                {
                    cn2 = cm;
                }
            }
            if (rn2 > accept_tol * (cn2 > dense::RealType<T>(0) ? cn2 : dense::RealType<T>(1)))
            {
                ok = false; // static factor diverged (indefinite/saddle-point) — solution is unreliable
            }
        }
        scale.untransform_solution({y.data(), n}, {b, n}); // x[col_match[j]] = D_c·y[j]
    }
    return ok; // false ⇒ a column failed to converge; the caller must NOT trust the solution
}

} // namespace crd::hesap::direct
