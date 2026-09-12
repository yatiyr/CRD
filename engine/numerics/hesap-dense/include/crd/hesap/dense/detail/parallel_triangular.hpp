#pragma once

#include <crd/core/types.hpp>
#include <crd/jobs/jobs.hpp>

#include <crd/math/cmath.hpp>

// -----------------------------------------------------------------------
// Balanced-triangular parallel partition (Phase 3.1.6 v7-e-2).
//
// A LOWER-triangular kernel (Cholesky panel trailing, supernodal cmod/cdiv
// diagonal SYRK, future eig) does work for row i proportional to (i+1) — it
// touches columns [0, i]. Partitioning [0, n) into equal-COUNT row-slabs
// (the plain jobs::parallel_for) is therefore badly load-imbalanced: the
// last worker does ~W× the first (measured: it REGRESSED the supernodal
// node-parallel (C) 8T factor). The cumulative work to row r is ≈ r²/2, so
// the work-balanced boundaries are
//        r_k = round( n · sqrt(k / W) ),   k = 0 … W
// (worker k gets [r_k, r_{k+1}): many cheap early rows, few expensive late
// rows, ~equal triangular area each).
//
// DETERMINISM: the partition decides only WHICH worker computes a row, never
// the VALUE of any entry (each entry's reduction is partition-independent),
// so the result is bit-identical across worker counts — the {1..16} moat
// holds regardless of W. A reusable primitive for every triangular kernel.
// -----------------------------------------------------------------------

namespace crd::hesap::dense::detail
{
// The k-th boundary (0 ≤ k ≤ w) of a balanced lower-triangular partition of [0, n) into w parts.
// triangular_bound(n,0,w)=0, triangular_bound(n,w,w)=n, non-decreasing in k. Pure (unit-testable).
[[nodiscard]] inline crd::u32 triangular_bound(crd::u32 n, crd::u32 k, crd::u32 w) noexcept
{
    if (k == 0 || w == 0)
    {
        return 0;
    }
    if (k >= w)
    {
        return n;
    }
    const double frac = static_cast<double>(k) / static_cast<double>(w);
    const double r = static_cast<double>(n) * crd::math::sqrt(frac) + 0.5;
    auto b = static_cast<crd::u32>(r);
    return b > n ? n : b; // clamp (defensive against fp rounding at the top)
}

// Run `body(r0, r1, worker)` over the balanced-triangular partition of [0, n) across `num_workers`.
// `worker` is the executing worker index (for per-lane scratch). Serial (num_workers ≤ 1) runs body(0,n,0)
// directly. The caller must have jobs::init()'d when num_workers > 1.
template <typename Fn>
inline void parallel_for_triangular(crd::u32 n, crd::u32 num_workers, Fn&& body)
{
    if (n == 0)
    {
        return;
    }
    const crd::u32 w = (num_workers == 0) ? 1U : num_workers;
    if (w <= 1)
    {
        body(0U, n, 0U);
        return;
    }
    // One task per worker; task k computes its sqrt-balanced row range. The lambda captures only {body,n,w}
    // (≤ 41-byte parallel_for SBO).
    auto* counter = crd::jobs::parallel_for(w, w,
                                            [&body, n, w](crd::u32 b, crd::u32 e)
                                            {
                                                for (crd::u32 k = b; k < e; ++k)
                                                {
                                                    const crd::u32 r0 = triangular_bound(n, k, w);
                                                    const crd::u32 r1 = triangular_bound(n, k + 1U, w);
                                                    if (r0 < r1)
                                                    {
                                                        body(r0, r1, crd::jobs::worker_index());
                                                    }
                                                }
                                            });
    crd::jobs::wait(counter);
}
} // namespace crd::hesap::dense::detail
