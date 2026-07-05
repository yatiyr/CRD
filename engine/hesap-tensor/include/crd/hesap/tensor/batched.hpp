#pragma once
// ---------------------------------------------------------------------------
// crd-hesap-tensor — v14-h: BATCHED dense linear algebra over rank-3 views
// [B, rows, cols] — the EKF/sensor-fusion/skinning regime (small matrices,
// huge batches). Reuse per SANITY #8: per-matrix compute = hesap-dense
// (gemm + the unblocked factorization algorithms); batch parallelism =
// crd-jobs deterministic partition (grain a function of shape ONLY, disjoint
// outputs) ⇒ results are BIT-IDENTICAL at any worker count. Peers on the
// board: torch-CPU (bmm/linalg on stacks), MATLAB pagemtimes, native MKL
// cblas_?gemm_batch (docs/bench 2026-07-05 v14-h sections).
//
// v1 layout contract: every operand is rank-3 with a TIGHT inner matrix
// (stride(2)==1, stride(1)==cols — row-major per matrix); the BATCH stride
// (stride(0)) is arbitrary ⇒ strided/sliced batch views work. Returns
// BadInput otherwise (the general-strided fallback rides a later increment
// with the einsum copy-avoidance machinery).
// ---------------------------------------------------------------------------
#include "tensor.hpp"

#include <crd/hesap/dense/blas3.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/simd/simd.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>

#include <cmath>  // std::fma in the scalar tail (single-rounded, the codelet rule)

namespace crd::hesap::tensor
{

namespace batcheddetail
{

// Allocation-free direct GEMM for the tiny tier (the packing pipeline's
// overhead exceeds the work below ~32K flops; this kernel also keeps the
// batch loop thread-safe without contending the scratch allocator).
// C = alpha*A@B + beta*C, row-major tight, any m/k/n.
//
// Register shape: R(≤4) rows × 2 column-vectors of C accumulate together =
// up to 8 independent fma chains (a single chain is FMA-LATENCY-bound — the
// first cut measured 0.59–0.77× vs MKL dgemm_batch_strided at n∈{6,8,16};
// the tile is the fix). Column tails ride masked partial load/store (no
// scalar rounding divergence). The BIT CONTRACT is unchanged: every element
// is the k-ordered single-rounded fma chain — the tile reorders only ACROSS
// elements, never within a chain.
template <typename T, crd::u32 R>
inline void direct_gemm_tile_rows(T alpha, const T* a, const T* b, T beta, T* c, crd::u64 i0, crd::u64 k,
                                  crd::u64 n) noexcept
{
    namespace simd = crd::math::simd;
    using V = std::conditional_t<std::is_same_v<T, crd::f32>, simd::Vec8f, simd::Vec4d>;
    constexpr crd::u64 W = std::is_same_v<T, crd::f32> ? 8U : 4U;
    for (crd::u64 j0 = 0; j0 < n; j0 += 2U * W)
    {
        const crd::u64 c0 = n - j0 < W ? n - j0 : W;                              // lanes in vector 0
        const crd::u64 c1 = n - j0 > W ? (n - j0 - W < W ? n - j0 - W : W) : 0U;  // lanes in vector 1
        V acc0[R];
        V acc1[R];
        for (crd::u32 r = 0; r < R; ++r)
        {
            acc0[r] = V(T(0));
            acc1[r] = V(T(0));
        }
        for (crd::u64 p = 0; p < k; ++p)
        {
            const T* brow = b + p * n + j0;
            const V b0 = c0 == W ? V::load(brow) : V::load_partial(brow, static_cast<crd::usize>(c0));
            V b1 = V(T(0));
            if (c1 != 0U)
            {
                b1 = c1 == W ? V::load(brow + W) : V::load_partial(brow + W, static_cast<crd::usize>(c1));
            }
            for (crd::u32 r = 0; r < R; ++r)
            {
                const V av = V(a[(i0 + r) * k + p]);
                acc0[r] = simd::fma(av, b0, acc0[r]);
                if (c1 != 0U)
                {
                    acc1[r] = simd::fma(av, b1, acc1[r]);
                }
            }
        }
        const V valpha = V(alpha);
        const V vbeta = V(beta);
        for (crd::u32 r = 0; r < R; ++r)
        {
            T* crow = c + (i0 + r) * n + j0;
            V out0 = acc0[r] * valpha;
            if (beta != T(0))
            {
                const V cl = c0 == W ? V::load(crow) : V::load_partial(crow, static_cast<crd::usize>(c0));
                out0 = simd::fma(vbeta, cl, out0);
            }
            if (c0 == W)
            {
                out0.store(crow);
            }
            else
            {
                out0.store_partial(crow, static_cast<crd::usize>(c0));
            }
            if (c1 != 0U)
            {
                V out1 = acc1[r] * valpha;
                if (beta != T(0))
                {
                    const V cl =
                        c1 == W ? V::load(crow + W) : V::load_partial(crow + W, static_cast<crd::usize>(c1));
                    out1 = simd::fma(vbeta, cl, out1);
                }
                if (c1 == W)
                {
                    out1.store(crow + W);
                }
                else
                {
                    out1.store_partial(crow + W, static_cast<crd::usize>(c1));
                }
            }
        }
    }
}

// Single-column-vector row tile for 5..8 rows: one j-block at a time, R
// accumulators (R >= 6 chains hide fma latency; 10 registers total) — B rows
// stream exactly ONCE per matrix. The winning shape for m in [5,8] where the
// 4-row/2-vector tile would reload B per row-block (n=6 @100k measured 0.97x
// vs MKL from exactly that reload; this variant removes it).
template <typename T, crd::u32 R>
inline void direct_gemm_tile_rows_1v(T alpha, const T* a, const T* b, T beta, T* c, crd::u64 k,
                                     crd::u64 n) noexcept
{
    namespace simd = crd::math::simd;
    using V = std::conditional_t<std::is_same_v<T, crd::f32>, simd::Vec8f, simd::Vec4d>;
    constexpr crd::u64 W = std::is_same_v<T, crd::f32> ? 8U : 4U;
    for (crd::u64 j0 = 0; j0 < n; j0 += W)
    {
        const crd::u64 cw = n - j0 < W ? n - j0 : W;
        V acc[R];
        for (crd::u32 r = 0; r < R; ++r)
        {
            acc[r] = V(T(0));
        }
        for (crd::u64 p = 0; p < k; ++p)
        {
            const T* brow = b + p * n + j0;
            const V bv = cw == W ? V::load(brow) : V::load_partial(brow, static_cast<crd::usize>(cw));
            for (crd::u32 r = 0; r < R; ++r)
            {
                acc[r] = simd::fma(V(a[r * k + p]), bv, acc[r]);
            }
        }
        const V valpha = V(alpha);
        const V vbeta = V(beta);
        for (crd::u32 r = 0; r < R; ++r)
        {
            T* crow = c + r * n + j0;
            V out = acc[r] * valpha;
            if (beta != T(0))
            {
                const V cl = cw == W ? V::load(crow) : V::load_partial(crow, static_cast<crd::usize>(cw));
                out = simd::fma(vbeta, cl, out);
            }
            if (cw == W)
            {
                out.store(crow);
            }
            else
            {
                out.store_partial(crow, static_cast<crd::usize>(cw));
            }
        }
    }
}

template <typename T>
inline void direct_gemm_tiny(T alpha, const T* a, const T* b, T beta, T* c, crd::u64 m, crd::u64 k,
                             crd::u64 n) noexcept
{
    // whole-matrix single block for 5..8 rows: B streams once (see above)
    switch (m)
    {
    case 5U:
        direct_gemm_tile_rows_1v<T, 5U>(alpha, a, b, beta, c, k, n);
        return;
    case 6U:
        direct_gemm_tile_rows_1v<T, 6U>(alpha, a, b, beta, c, k, n);
        return;
    case 7U:
        direct_gemm_tile_rows_1v<T, 7U>(alpha, a, b, beta, c, k, n);
        return;
    case 8U:
        direct_gemm_tile_rows_1v<T, 8U>(alpha, a, b, beta, c, k, n);
        return;
    default:
        break;
    }
    crd::u64 i = 0;
    for (; i + 4U <= m; i += 4U)
    {
        direct_gemm_tile_rows<T, 4U>(alpha, a, b, beta, c, i, k, n);
    }
    switch (m - i)
    {
    case 3U:
        direct_gemm_tile_rows<T, 3U>(alpha, a, b, beta, c, i, k, n);
        break;
    case 2U:
        direct_gemm_tile_rows<T, 2U>(alpha, a, b, beta, c, i, k, n);
        break;
    case 1U:
        direct_gemm_tile_rows<T, 1U>(alpha, a, b, beta, c, i, k, n);
        break;
    default:
        break;
    }
}

// tight [B, r, c] check (see the layout contract above)
template <typename T>
[[nodiscard]] inline bool tight3(const TensorView<T>& v) noexcept
{
    return v.rank() == 3U && v.stride(2) == 1 &&
           v.stride(1) == static_cast<crd::i64>(v.shape(2));
}

// deterministic batch grain: a function of shape ONLY (Tier-D rule) — never
// of the live worker count, so partitions are stable across pools.
[[nodiscard]] inline crd::u32 batch_grain(crd::u64 batch, crd::u64 per_matrix_flops) noexcept
{
    // target ~64K flops per task minimum so tiny matrices amortize task setup
    const crd::u64 per = per_matrix_flops > 0U ? per_matrix_flops : 1U;
    crd::u64 g = (64ULL * 1024ULL + per - 1ULL) / per;
    if (g < 1U)
    {
        g = 1U;
    }
    if (g > batch)
    {
        g = batch;
    }
    return static_cast<crd::u32>(g);
}

// ---- batched Cholesky (increment B) ----------------------------------------
// Lane-batched AoSoA kernel: W matrices per vector (4 f64 / 8 f32) — one
// vsqrt/vdiv retires W scalar sqrt/divs (the expensive ops of the
// factorization). The scalar tier mirrors the EXACT op order (fma/fnma
// chains), so lane-batched == scalar is bit-identical (lane-wise IEEE) — the
// tier gate. No pivoting (SPD contract); a non-SPD matrix flags its OWN info
// lane and never poisons group siblings (its lanes keep computing on garbage
// that is masked out of nothing — every matrix's result depends only on its
// own lane).

inline constexpr crd::u64 kBatchedCholMaxN = 32;

// scalar reference tier: identical op order to the lane kernel
template <typename T>
inline void chol_scalar_one(T* a, crd::u64 n, crd::i32* info) noexcept
{
    *info = 0;
    for (crd::u64 j = 0; j < n; ++j)
    {
        T d = a[j * n + j];
        for (crd::u64 p = 0; p < j; ++p)
        {
            d = std::fma(-a[j * n + p], a[j * n + p], d);
        }
        if (!(d > T(0)))
        {
            if (*info == 0)
            {
                *info = static_cast<crd::i32>(j) + 1;
            }
            d = T(1); // keep computing (lane-kernel parity); result is flagged
        }
        const T ljj = std::sqrt(d);
        const T inv = T(1) / ljj;
        a[j * n + j] = ljj;
        for (crd::u64 i = j + 1U; i < n; ++i)
        {
            T s = a[i * n + j];
            for (crd::u64 p = 0; p < j; ++p)
            {
                s = std::fma(-a[i * n + p], a[j * n + p], s);
            }
            a[i * n + j] = s * inv;
        }
        for (crd::u64 i = 0; i < j; ++i)
        {
            a[i * n + j] = T(0); // canonical lower factor: zero the strict upper
        }
    }
}

// lane-batched tier over an element-major staging tile buf[n*n][W]
template <typename T>
inline void chol_lanes_group(T* buf, crd::u64 n, crd::u64 w, crd::i32* info) noexcept
{
    namespace simd = crd::math::simd;
    using V = std::conditional_t<std::is_same_v<T, crd::f32>, simd::Vec8f, simd::Vec4d>;
    constexpr crd::u64 W = std::is_same_v<T, crd::f32> ? 8U : 4U;
    (void)w;
    const auto at = [&](crd::u64 i, crd::u64 j) noexcept -> T* { return buf + (i * n + j) * W; };
    crd::i32 flagged[W] = {};
    for (crd::u64 j = 0; j < n; ++j)
    {
        V d = V::load(at(j, j));
        for (crd::u64 p = 0; p < j; ++p)
        {
            const V l = V::load(at(j, p));
            d = simd::fnmadd(l, l, d);
        }
        // per-lane SPD check: pure vector substitute + flags from a STORED
        // mask (the MSVC auto-vectorization scar — see the LU pivot scan)
        const V goodmask = simd::cmp_gt(d, V(T(0)));
        d = simd::select(goodmask, d, V(T(1)));
        {
            alignas(32) T gm[W];
            goodmask.store(gm);
            for (crd::u64 q = 0; q < W; ++q)
            {
                if (gm[q] == T(0) && flagged[q] == 0)
                {
                    flagged[q] = static_cast<crd::i32>(j) + 1;
                }
            }
        }
        const V ljj = simd::sqrt(d);
        const V inv = V(T(1)) / ljj;
        ljj.store(at(j, j));
        for (crd::u64 i = j + 1U; i < n; ++i)
        {
            V s = V::load(at(i, j));
            for (crd::u64 p = 0; p < j; ++p)
            {
                s = simd::fnmadd(V::load(at(i, p)), V::load(at(j, p)), s);
            }
            (s * inv).store(at(i, j));
        }
        const V z = V(T(0));
        for (crd::u64 i = 0; i < j; ++i)
        {
            z.store(at(i, j));
        }
    }
    for (crd::u64 q = 0; q < W; ++q)
    {
        info[q] = flagged[q];
    }
}

// ---- batched LU with partial pivoting (increment C) ------------------------
// Per-lane pivoting: selection + elimination are vector ops; the row swap is
// O(n*W) scalar per column (noise next to the O(n^2*W) elimination). Pivot
// rule (both tiers, bit-gated): largest |a[i][j]|, STRICTLY greater to win ⇒
// the lowest row index takes ties. Singular column ⇒ info = j+1 (1-based),
// pivot continues on 1.0 (flagged result unspecified, siblings unaffected).

template <typename T>
inline void lu_scalar_one(T* a, crd::u64 n, crd::i32* piv, crd::i32* info) noexcept
{
    *info = 0;
    for (crd::u64 j = 0; j < n; ++j)
    {
        crd::u64 pr = j;
        T best = a[j * n + j] < T(0) ? -a[j * n + j] : a[j * n + j];
        for (crd::u64 i = j + 1U; i < n; ++i)
        {
            const T v = a[i * n + j] < T(0) ? -a[i * n + j] : a[i * n + j];
            if (v > best)
            {
                best = v;
                pr = i;
            }
        }
        piv[j] = static_cast<crd::i32>(pr);
        if (pr != j)
        {
            for (crd::u64 c = 0; c < n; ++c)
            {
                const T t = a[j * n + c];
                a[j * n + c] = a[pr * n + c];
                a[pr * n + c] = t;
            }
        }
        T p = a[j * n + j];
        if (p == T(0))
        {
            if (*info == 0)
            {
                *info = static_cast<crd::i32>(j) + 1;
            }
            p = T(1);
        }
        const T inv = T(1) / p;
        for (crd::u64 i = j + 1U; i < n; ++i)
        {
            const T l = a[i * n + j] * inv;
            a[i * n + j] = l;
            for (crd::u64 c = j + 1U; c < n; ++c)
            {
                a[i * n + c] = std::fma(-l, a[j * n + c], a[i * n + c]);
            }
        }
    }
}

// lane tier over the element-major staging tile buf[n*n][W]; piv is [n][W]
template <typename T>
inline void lu_lanes_group(T* buf, crd::u64 n, crd::i32* piv, crd::i32* info) noexcept
{
    namespace simd = crd::math::simd;
    using V = std::conditional_t<std::is_same_v<T, crd::f32>, simd::Vec8f, simd::Vec4d>;
    constexpr crd::u64 W = std::is_same_v<T, crd::f32> ? 8U : 4U;
    const auto at = [&](crd::u64 i, crd::u64 j) noexcept -> T* { return buf + (i * n + j) * W; };
    crd::i32 flagged[W] = {};
    for (crd::u64 j = 0; j < n; ++j)
    {
        // Per-lane pivot selection as PURE VECTOR argmax (strictly-greater ⇒
        // lowest index wins ties). The obvious per-lane scalar loop
        // `if (v > best[q]) { best[q] = v; pr[q] = i; }` is MISCOMPILED by
        // MSVC /O1+/O2 auto-vectorization (wrong masked blends over the
        // two-array conditional update; root-caused 2026-07-05 via a
        // standalone repro — /Od and gcc correct, fprintf-in-loop suppressed
        // it). Indices ride f64 lanes (exact for n <= 2^53).
        V bestv = simd::abs(V::load(at(j, j)));
        V bestidx = V(static_cast<T>(j));
        for (crd::u64 i = j + 1U; i < n; ++i)
        {
            const V v = simd::abs(V::load(at(i, j)));
            const V m = simd::cmp_gt(v, bestv);
            bestv = simd::select(m, v, bestv);
            bestidx = simd::select(m, V(static_cast<T>(i)), bestidx);
        }
        crd::u64 pr[W];
        {
            alignas(32) T bi[W];
            bestidx.store(bi);
            for (crd::u64 q = 0; q < W; ++q)
            {
                pr[q] = static_cast<crd::u64>(bi[q]);
            }
        }
        for (crd::u64 q = 0; q < W; ++q)
        {
            piv[j * W + q] = static_cast<crd::i32>(pr[q]);
            if (pr[q] != j)
            {
                // per-lane row swap (2n scalar moves per lane)
                for (crd::u64 c = 0; c < n; ++c)
                {
                    T* rj = at(j, c) + q;
                    T* rp = at(pr[q], c) + q;
                    const T t = *rj;
                    *rj = *rp;
                    *rp = t;
                }
            }
        }
        // singular-lane handling: pure vector substitute + flags read from a
        // STORED mask (never the two-array conditional-update loop shape —
        // the MSVC auto-vectorization scar above)
        const V praw = V::load(at(j, j));
        const V badmask = simd::cmp_eq(praw, V(T(0)));
        const V p = simd::select(badmask, V(T(1)), praw);
        {
            alignas(32) T bm[W];
            badmask.store(bm);
            for (crd::u64 q = 0; q < W; ++q)
            {
                if (bm[q] != T(0) && flagged[q] == 0)
                {
                    flagged[q] = static_cast<crd::i32>(j) + 1;
                }
            }
        }
        const V inv = V(T(1)) / p;
        for (crd::u64 i = j + 1U; i < n; ++i)
        {
            const V l = V::load(at(i, j)) * inv;
            l.store(at(i, j));
            for (crd::u64 c = j + 1U; c < n; ++c)
            {
                const V x = simd::fnmadd(l, V::load(at(j, c)), V::load(at(i, c)));
                x.store(at(i, c));
            }
        }
    }
    for (crd::u64 q = 0; q < W; ++q)
    {
        info[q] = flagged[q];
    }
}

// ---- batched small-SVD via one-sided Jacobi (increment D) ------------------
// Rotation-based and pivot-free ⇒ lane-perfect: every column-pair rotation is
// a masked lane op (converged lanes keep their exact bits through `select` —
// never an arithmetic identity, which would flip -0). Bounded iteration:
// max_sweeps hard cap, info = 1 when a matrix still rotated in the last sweep
// (ADR-0095 bounded-iteration pillar). The σ/U extraction + descending sort
// is ONE shared scalar post-pass for both tiers ⇒ tier bit-identity needs
// only the sweep phase to match (gated).

inline constexpr crd::u64 kBatchedSvdMaxN = 16;

template <typename T>
[[nodiscard]] inline T svd_tol() noexcept
{
    return std::is_same_v<T, crd::f32> ? T(1e-6) : T(1e-14);
}

// one cyclic sweep phase on a single matrix (in-place columns of a, v)
template <typename T>
inline void svd_scalar_sweeps(T* a, T* v, crd::u64 n, crd::u32 max_sweeps, crd::i32* info) noexcept
{
    for (crd::u64 i = 0; i < n * n; ++i)
    {
        v[i] = T(0);
    }
    for (crd::u64 i = 0; i < n; ++i)
    {
        v[i * n + i] = T(1);
    }
    const T tol = svd_tol<T>();
    *info = 1;
    for (crd::u32 sweep = 0; sweep < max_sweeps; ++sweep)
    {
        bool rotated = false;
        for (crd::u64 p = 0; p + 1U < n; ++p)
        {
            for (crd::u64 q = p + 1U; q < n; ++q)
            {
                T alpha = T(0);
                T beta = T(0);
                T gamma = T(0);
                for (crd::u64 r = 0; r < n; ++r)
                {
                    alpha = std::fma(a[r * n + p], a[r * n + p], alpha);
                    beta = std::fma(a[r * n + q], a[r * n + q], beta);
                    gamma = std::fma(a[r * n + p], a[r * n + q], gamma);
                }
                const T g = gamma < T(0) ? -gamma : gamma;
                if (!(g > tol * std::sqrt(alpha * beta)))
                {
                    continue;
                }
                rotated = true;
                // EXACT op mirror of the lane tier (fma forms + 0-x negation,
                // the signed-zero rule) — the tier bit-identity contract
                const T zeta = (beta - alpha) / (T(2) * gamma);
                const T az = zeta < T(0) ? -zeta : zeta;
                const T t = (zeta >= T(0) ? T(1) : T(-1)) / (az + std::sqrt(std::fma(zeta, zeta, T(1))));
                const T c = T(1) / std::sqrt(std::fma(t, t, T(1)));
                const T s = c * t;
                for (crd::u64 r = 0; r < n; ++r)
                {
                    const T ap = a[r * n + p];
                    const T aq = a[r * n + q];
                    a[r * n + p] = std::fma(c, ap, T(0) - s * aq);
                    a[r * n + q] = std::fma(s, ap, c * aq);
                    const T vp = v[r * n + p];
                    const T vq = v[r * n + q];
                    v[r * n + p] = std::fma(c, vp, T(0) - s * vq);
                    v[r * n + q] = std::fma(s, vp, c * vq);
                }
            }
        }
        if (!rotated)
        {
            *info = 0;
            return;
        }
    }
    // ran to the cap: converged only if the LAST sweep applied no rotation —
    // reaching here means it did ⇒ info stays 1
}

// lane tier: same sweeps over element-major tiles abuf/vbuf [n*n][W]
template <typename T>
inline void svd_lanes_sweeps(T* abuf, T* vbuf, crd::u64 n, crd::u32 max_sweeps, crd::i32* info) noexcept
{
    namespace simd = crd::math::simd;
    using V = std::conditional_t<std::is_same_v<T, crd::f32>, simd::Vec8f, simd::Vec4d>;
    constexpr crd::u64 W = std::is_same_v<T, crd::f32> ? 8U : 4U;
    const auto at = [&](T* buf, crd::u64 r, crd::u64 c) noexcept -> T* { return buf + (r * n + c) * W; };
    for (crd::u64 e = 0; e < n * n; ++e)
    {
        V(T(0)).store(vbuf + e * W);
    }
    for (crd::u64 i = 0; i < n; ++i)
    {
        V(T(1)).store(at(vbuf, i, i));
    }
    const V tol = V(svd_tol<T>());
    const V zero = V(T(0));
    const V one = V(T(1));
    const V two = V(T(2));
    for (crd::u64 q0 = 0; q0 < W; ++q0)
    {
        info[q0] = 1;
    }
    for (crd::u32 sweep = 0; sweep < max_sweeps; ++sweep)
    {
        V any_rot = zero; // lanes that rotated this sweep (mask accumulate)
        for (crd::u64 p = 0; p + 1U < n; ++p)
        {
            for (crd::u64 q = p + 1U; q < n; ++q)
            {
                V alpha = zero;
                V beta = zero;
                V gamma = zero;
                for (crd::u64 r = 0; r < n; ++r)
                {
                    const V ap = V::load(at(abuf, r, p));
                    const V aq = V::load(at(abuf, r, q));
                    alpha = simd::fma(ap, ap, alpha);
                    beta = simd::fma(aq, aq, beta);
                    gamma = simd::fma(ap, aq, gamma);
                }
                const V g = simd::abs(gamma);
                const V rot = simd::cmp_gt(g, tol * simd::sqrt(alpha * beta));
                // all-lanes-skip fast path (movemask-free: max of mask as f64)
                alignas(32) T rl[W];
                rot.store(rl);
                bool any = false;
                for (crd::u64 q0 = 0; q0 < W; ++q0)
                {
                    if (rl[q0] != T(0))
                    {
                        any = true;
                        break;
                    }
                }
                if (!any)
                {
                    continue;
                }
                any_rot = simd::select(rot, one, any_rot);
                // rotation params on rotating lanes; identity elsewhere is
                // enforced by SELECT on the results (exact old bits)
                const V gsafe = simd::select(rot, gamma, one); // avoid /0 on skipped lanes
                const V zeta = (beta - alpha) / (two * gsafe);
                const V sgn = simd::select(simd::cmp_ge(zeta, zero), one, V(T(-1)));
                const V t = sgn / (simd::abs(zeta) + simd::sqrt(simd::fma(zeta, zeta, one)));
                const V c = one / simd::sqrt(simd::fma(t, t, one));
                const V s = c * t;
                for (crd::u64 r = 0; r < n; ++r)
                {
                    const V ap = V::load(at(abuf, r, p));
                    const V aq = V::load(at(abuf, r, q));
                    const V nap = simd::fma(c, ap, zero - s * aq);
                    const V naq = simd::fma(s, ap, c * aq);
                    simd::select(rot, nap, ap).store(at(abuf, r, p));
                    simd::select(rot, naq, aq).store(at(abuf, r, q));
                    const V vp = V::load(at(vbuf, r, p));
                    const V vq = V::load(at(vbuf, r, q));
                    const V nvp = simd::fma(c, vp, zero - s * vq);
                    const V nvq = simd::fma(s, vp, c * vq);
                    simd::select(rot, nvp, vp).store(at(vbuf, r, p));
                    simd::select(rot, nvq, vq).store(at(vbuf, r, q));
                }
            }
        }
        alignas(32) T al[W];
        any_rot.store(al);
        bool alldone = true;
        for (crd::u64 q0 = 0; q0 < W; ++q0)
        {
            if (al[q0] == T(0) && info[q0] == 1)
            {
                info[q0] = 0; // this lane's first quiet sweep = converged
            }
            if (al[q0] != T(0))
            {
                alldone = false;
            }
        }
        if (alldone)
        {
            return;
        }
    }
}

// shared post-pass (both tiers): sigma = column norms, U = normalized columns,
// stable descending sort permuting U columns, sigma, V columns together.
template <typename T>
inline void svd_finalize_one(T* a /*becomes U*/, T* v, T* sigma, crd::u64 n) noexcept
{
    for (crd::u64 j = 0; j < n; ++j)
    {
        T ss = T(0);
        for (crd::u64 r = 0; r < n; ++r)
        {
            ss = std::fma(a[r * n + j], a[r * n + j], ss);
        }
        const T nrm = std::sqrt(ss);
        sigma[j] = nrm;
        if (nrm > T(0))
        {
            const T inv = T(1) / nrm;
            for (crd::u64 r = 0; r < n; ++r)
            {
                a[r * n + j] *= inv;
            }
        }
        else
        {
            for (crd::u64 r = 0; r < n; ++r)
            {
                a[r * n + j] = r == j ? T(1) : T(0);
            }
        }
    }
    // stable insertion sort, descending sigma (ties keep original order)
    for (crd::u64 j = 1; j < n; ++j)
    {
        const T sj = sigma[j];
        crd::u64 pos = j;
        while (pos > 0U && sigma[pos - 1U] < sj)
        {
            --pos;
        }
        if (pos == j)
        {
            continue;
        }
        for (crd::u64 m = j; m > pos; --m)
        {
            sigma[m] = sigma[m - 1U];
            for (crd::u64 r = 0; r < n; ++r)
            {
                const T tu = a[r * n + m];
                a[r * n + m] = a[r * n + m - 1U];
                a[r * n + m - 1U] = tu;
                const T tv = v[r * n + m];
                v[r * n + m] = v[r * n + m - 1U];
                v[r * n + m - 1U] = tv;
            }
        }
        sigma[pos] = sj;
    }
}

// pack/unpack a group of `w` (<= W) matrices [w][n*n] <-> element-major
// [n*n][W]; missing lanes (w < W) are filled with the identity so their
// sqrt/div lanes stay benign.
template <typename T>
inline void chol_pack_group(const T* const* src, crd::u64 n, crd::u64 w, T* buf) noexcept
{
    constexpr crd::u64 W = std::is_same_v<T, crd::f32> ? 8U : 4U;
    for (crd::u64 e = 0; e < n * n; ++e)
    {
        for (crd::u64 q = 0; q < W; ++q)
        {
            buf[e * W + q] = q < w ? src[q][e] : (e % (n + 1U) == 0U ? T(1) : T(0));
        }
    }
}

template <typename T>
inline void chol_unpack_group(const T* buf, crd::u64 n, crd::u64 w, T* const* dst) noexcept
{
    constexpr crd::u64 W = std::is_same_v<T, crd::f32> ? 8U : 4U;
    for (crd::u64 q = 0; q < w; ++q)
    {
        for (crd::u64 e = 0; e < n * n; ++e)
        {
            dst[q][e] = buf[e * W + q];
        }
    }
}

} // namespace batcheddetail

// In-place batched Cholesky: A[b] (SPD, row-major tight) -> its lower factor
// L[b] (strict upper zeroed). info[b] = 0 on success, or 1-based pivot column
// where positivity failed (that matrix's factor content is then unspecified;
// SIBLING matrices in its SIMD group are UNAFFECTED — gated). Lane-batched
// AoSoA kernel for n <= kBatchedCholMaxN, scalar tier otherwise/remainder;
// the tiers are BIT-IDENTICAL (gated). Deterministic at any worker count.
template <typename T>
[[nodiscard]] TensorStatus batched_cholesky_factor(TensorView<T> a, crd::containers::Span<crd::i32> info,
                                                   crd::u32 num_workers = 0) noexcept
{
    using namespace batcheddetail;
    if (!tight3(a) || a.shape(1) != a.shape(2))
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 batch = a.shape(0);
    const crd::u64 n = a.shape(1);
    if (info.size() != batch)
    {
        return TensorStatus::BadInput;
    }
    if (batch == 0U)
    {
        return TensorStatus::Ok;
    }
    T* pa = a.data();
    const crd::i64 sa = a.stride(0);
    crd::i32* pinfo = info.data();
    constexpr crd::u64 W = std::is_same_v<T, crd::f32> ? 8U : 4U;
    const bool lanes = n <= kBatchedCholMaxN;
    const auto run_groups = [=](crd::u32 gb, crd::u32 ge) noexcept
    {
        // one staging tile per task frame (stack: n^2*W*sizeof(T) <= 32KB f64)
        T buf[kBatchedCholMaxN * kBatchedCholMaxN * W];
        for (crd::u32 g = gb; g < ge; ++g)
        {
            const crd::u64 lo = static_cast<crd::u64>(g) * W;
            const crd::u64 w = batch - lo < W ? batch - lo : W;
            if (!lanes)
            {
                for (crd::u64 q = 0; q < w; ++q)
                {
                    chol_scalar_one(pa + static_cast<crd::i64>(lo + q) * sa, n, pinfo + lo + q);
                }
                continue;
            }
            const T* src[W];
            T* dst[W];
            for (crd::u64 q = 0; q < w; ++q)
            {
                src[q] = pa + static_cast<crd::i64>(lo + q) * sa;
                dst[q] = pa + static_cast<crd::i64>(lo + q) * sa;
            }
            chol_pack_group(src, n, w, buf);
            crd::i32 ginfo[W];
            chol_lanes_group(buf, n, w, ginfo);
            chol_unpack_group(buf, n, w, dst);
            for (crd::u64 q = 0; q < w; ++q)
            {
                pinfo[lo + q] = ginfo[q];
            }
        }
    };
    const crd::u64 groups64 = (batch + W - 1U) / W;
    const crd::u32 groups = static_cast<crd::u32>(groups64);
    crd::u32 nw = num_workers;
    if (nw == 0U)
    {
        nw = crd::jobs::num_workers();
    }
    if (nw <= 1U || groups < 2U)
    {
        run_groups(0U, groups);
        return TensorStatus::Ok;
    }
    struct Ctx
    {
        const decltype(run_groups)* run;
    };
    Ctx ctx{&run_groups};
    Ctx* const cp = &ctx;
    auto* const counter =
        crd::jobs::parallel_for(groups, nw, [cp](crd::u32 gb, crd::u32 ge) { (*cp->run)(gb, ge); });
    crd::jobs::wait(counter);
    return TensorStatus::Ok;
}

// Batched triangular solves with the batched factor: X = L^-T (L^-1 RHS)
// — the SPD solve. RHS = [B, n, r] tight, solved in place. Same op order in
// both tiers (bit-identical, gated).
template <typename T>
[[nodiscard]] TensorStatus batched_cholesky_solve(TensorView<const T> l, TensorView<T> rhs,
                                                  crd::u32 num_workers = 0) noexcept
{
    using namespace batcheddetail;
    if (!tight3(l) || !tight3(rhs) || l.shape(1) != l.shape(2) || rhs.shape(1) != l.shape(1) ||
        rhs.shape(0) != l.shape(0))
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 batch = l.shape(0);
    const crd::u64 n = l.shape(1);
    const crd::u64 r = rhs.shape(2);
    if (batch == 0U)
    {
        return TensorStatus::Ok;
    }
    const T* pl = l.data();
    T* px = rhs.data();
    const crd::i64 sl = l.stride(0);
    const crd::i64 sx = rhs.stride(0);
    const auto solve_one = [=](crd::u64 b) noexcept
    {
        const T* lm = pl + static_cast<crd::i64>(b) * sl;
        T* x = px + static_cast<crd::i64>(b) * sx;
        // forward: L y = rhs
        for (crd::u64 i = 0; i < n; ++i)
        {
            const T inv = T(1) / lm[i * n + i];
            for (crd::u64 c = 0; c < r; ++c)
            {
                T s = x[i * r + c];
                for (crd::u64 p = 0; p < i; ++p)
                {
                    s = std::fma(-lm[i * n + p], x[p * r + c], s);
                }
                x[i * r + c] = s * inv;
            }
        }
        // backward: L^T x = y
        for (crd::u64 ii = n; ii-- > 0U;)
        {
            const T inv = T(1) / lm[ii * n + ii];
            for (crd::u64 c = 0; c < r; ++c)
            {
                T s = x[ii * r + c];
                for (crd::u64 p = ii + 1U; p < n; ++p)
                {
                    s = std::fma(-lm[p * n + ii], x[p * r + c], s);
                }
                x[ii * r + c] = s * inv;
            }
        }
    };
    const auto run_range = [=](crd::u32 lo, crd::u32 hi) noexcept
    {
        for (crd::u32 b = lo; b < hi; ++b)
        {
            solve_one(b);
        }
    };
    crd::u32 nw = num_workers;
    if (nw == 0U)
    {
        nw = crd::jobs::num_workers();
    }
    if (nw <= 1U || batch < 2U)
    {
        run_range(0U, static_cast<crd::u32>(batch));
        return TensorStatus::Ok;
    }
    struct Ctx
    {
        const decltype(run_range)* run;
    };
    Ctx ctx{&run_range};
    Ctx* const cp = &ctx;
    auto* const counter = crd::jobs::parallel_for(static_cast<crd::u32>(batch), nw,
                                                  [cp](crd::u32 lo, crd::u32 hi) { (*cp->run)(lo, hi); });
    crd::jobs::wait(counter);
    return TensorStatus::Ok;
}

// Batched small-SVD (square n <= kBatchedSvdMaxN lane-batched, scalar above):
// A[b] = U[b] diag(sigma[b]) V[b]^T. sigma descending (stable ties), U/V
// column-orthonormal; info[b] = 0 converged, 1 = still rotating at max_sweeps
// (bounded iteration — never spins). Tier bit-identity gated.
template <typename T>
[[nodiscard]] TensorStatus batched_svd_small(TensorView<const T> a, TensorView<T> u,
                                             crd::containers::Span<T> sigma, TensorView<T> v,
                                             crd::containers::Span<crd::i32> info, crd::u32 max_sweeps = 30,
                                             crd::u32 num_workers = 0) noexcept
{
    using namespace batcheddetail;
    if (!tight3(a) || !tight3(u) || !tight3(v) || a.shape(1) != a.shape(2))
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 batch = a.shape(0);
    const crd::u64 n = a.shape(1);
    if (u.shape(0) != batch || u.shape(1) != n || u.shape(2) != n || v.shape(0) != batch || v.shape(1) != n ||
        v.shape(2) != n || sigma.size() != batch * n || info.size() != batch || max_sweeps == 0U)
    {
        return TensorStatus::BadInput;
    }
    if (batch == 0U)
    {
        return TensorStatus::Ok;
    }
    const T* pa = a.data();
    T* pu = u.data();
    T* pv = v.data();
    T* psig = sigma.data();
    crd::i32* pinfo = info.data();
    const crd::i64 sa = a.stride(0);
    const crd::i64 su = u.stride(0);
    const crd::i64 sv = v.stride(0);
    constexpr crd::u64 W = std::is_same_v<T, crd::f32> ? 8U : 4U;
    const bool lanes = n <= kBatchedSvdMaxN;
    const auto run_groups = [=](crd::u32 gb, crd::u32 ge) noexcept
    {
        T abuf[kBatchedSvdMaxN * kBatchedSvdMaxN * W];
        T vbuf[kBatchedSvdMaxN * kBatchedSvdMaxN * W];
        for (crd::u32 g = gb; g < ge; ++g)
        {
            const crd::u64 lo = static_cast<crd::u64>(g) * W;
            const crd::u64 w = batch - lo < W ? batch - lo : W;
            if (!lanes)
            {
                for (crd::u64 q = 0; q < w; ++q)
                {
                    T* uw = pu + static_cast<crd::i64>(lo + q) * su;
                    T* vw = pv + static_cast<crd::i64>(lo + q) * sv;
                    const T* aw = pa + static_cast<crd::i64>(lo + q) * sa;
                    for (crd::u64 e = 0; e < n * n; ++e)
                    {
                        uw[e] = aw[e];
                    }
                    svd_scalar_sweeps(uw, vw, n, max_sweeps, pinfo + lo + q);
                    svd_finalize_one(uw, vw, psig + (lo + q) * n, n);
                }
                continue;
            }
            const T* src[W];
            T* du[W];
            T* dv[W];
            for (crd::u64 q = 0; q < w; ++q)
            {
                src[q] = pa + static_cast<crd::i64>(lo + q) * sa;
                du[q] = pu + static_cast<crd::i64>(lo + q) * su;
                dv[q] = pv + static_cast<crd::i64>(lo + q) * sv;
            }
            chol_pack_group(src, n, w, abuf);
            crd::i32 ginfo[W];
            svd_lanes_sweeps(abuf, vbuf, n, max_sweeps, ginfo);
            chol_unpack_group(abuf, n, w, du);
            chol_unpack_group(vbuf, n, w, dv);
            for (crd::u64 q = 0; q < w; ++q)
            {
                pinfo[lo + q] = ginfo[q];
                svd_finalize_one(du[q], dv[q], psig + (lo + q) * n, n);
            }
        }
    };
    const crd::u32 groups = static_cast<crd::u32>((batch + W - 1U) / W);
    crd::u32 nw = num_workers;
    if (nw == 0U)
    {
        nw = crd::jobs::num_workers();
    }
    if (nw <= 1U || groups < 2U)
    {
        run_groups(0U, groups);
        return TensorStatus::Ok;
    }
    struct Ctx
    {
        const decltype(run_groups)* run;
    };
    Ctx ctx{&run_groups};
    Ctx* const cp = &ctx;
    auto* const counter =
        crd::jobs::parallel_for(groups, nw, [cp](crd::u32 gb, crd::u32 ge) { (*cp->run)(gb, ge); });
    crd::jobs::wait(counter);
    return TensorStatus::Ok;
}

// In-place batched LU with partial pivoting: A[b] -> L (unit lower, strict) +
// U (upper). piv = [B*n] 0-based pivot ROW chosen at each column (LAPACK-style
// sequential swaps); info[b] = 0 or the 1-based first singular column. Lane
// tier for n <= kBatchedCholMaxN, scalar otherwise; tiers BIT-IDENTICAL
// (gated, incl. the pivot tie rule).
template <typename T>
[[nodiscard]] TensorStatus batched_lu_factor(TensorView<T> a, crd::containers::Span<crd::i32> piv,
                                             crd::containers::Span<crd::i32> info,
                                             crd::u32 num_workers = 0) noexcept
{
    using namespace batcheddetail;
    if (!tight3(a) || a.shape(1) != a.shape(2))
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 batch = a.shape(0);
    const crd::u64 n = a.shape(1);
    if (info.size() != batch || piv.size() != batch * n)
    {
        return TensorStatus::BadInput;
    }
    if (batch == 0U)
    {
        return TensorStatus::Ok;
    }
    T* pa = a.data();
    const crd::i64 sa = a.stride(0);
    crd::i32* ppiv = piv.data();
    crd::i32* pinfo = info.data();
    constexpr crd::u64 W = std::is_same_v<T, crd::f32> ? 8U : 4U;
    const bool lanes = n <= kBatchedCholMaxN;
    const auto run_groups = [=](crd::u32 gb, crd::u32 ge) noexcept
    {
        T buf[kBatchedCholMaxN * kBatchedCholMaxN * W];
        crd::i32 gpiv[kBatchedCholMaxN * W];
        for (crd::u32 g = gb; g < ge; ++g)
        {
            const crd::u64 lo = static_cast<crd::u64>(g) * W;
            const crd::u64 w = batch - lo < W ? batch - lo : W;
            if (!lanes)
            {
                for (crd::u64 q = 0; q < w; ++q)
                {
                    lu_scalar_one(pa + static_cast<crd::i64>(lo + q) * sa, n, ppiv + (lo + q) * n,
                                  pinfo + lo + q);
                }
                continue;
            }
            const T* src[W];
            T* dst[W];
            for (crd::u64 q = 0; q < w; ++q)
            {
                src[q] = pa + static_cast<crd::i64>(lo + q) * sa;
                dst[q] = pa + static_cast<crd::i64>(lo + q) * sa;
            }
            chol_pack_group(src, n, w, buf);
            crd::i32 ginfo[W];
            lu_lanes_group(buf, n, gpiv, ginfo);
            chol_unpack_group(buf, n, w, dst);
            for (crd::u64 q = 0; q < w; ++q)
            {
                pinfo[lo + q] = ginfo[q];
                for (crd::u64 j = 0; j < n; ++j)
                {
                    ppiv[(lo + q) * n + j] = gpiv[j * W + q];
                }
            }
        }
    };
    const crd::u32 groups = static_cast<crd::u32>((batch + W - 1U) / W);
    crd::u32 nw = num_workers;
    if (nw == 0U)
    {
        nw = crd::jobs::num_workers();
    }
    if (nw <= 1U || groups < 2U)
    {
        run_groups(0U, groups);
        return TensorStatus::Ok;
    }
    struct Ctx
    {
        const decltype(run_groups)* run;
    };
    Ctx ctx{&run_groups};
    Ctx* const cp = &ctx;
    auto* const counter =
        crd::jobs::parallel_for(groups, nw, [cp](crd::u32 gb, crd::u32 ge) { (*cp->run)(gb, ge); });
    crd::jobs::wait(counter);
    return TensorStatus::Ok;
}

// Batched LU solve (in place on rhs = [B, n, r] tight): apply the pivot
// sequence, unit-lower forward substitution, upper backward substitution.
template <typename T>
[[nodiscard]] TensorStatus batched_lu_solve(TensorView<const T> lu, crd::containers::ConstSpan<crd::i32> piv,
                                            TensorView<T> rhs, crd::u32 num_workers = 0) noexcept
{
    using namespace batcheddetail;
    if (!tight3(lu) || !tight3(rhs) || lu.shape(1) != lu.shape(2) || rhs.shape(1) != lu.shape(1) ||
        rhs.shape(0) != lu.shape(0))
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 batch = lu.shape(0);
    const crd::u64 n = lu.shape(1);
    const crd::u64 r = rhs.shape(2);
    if (piv.size() != batch * n)
    {
        return TensorStatus::BadInput;
    }
    if (batch == 0U)
    {
        return TensorStatus::Ok;
    }
    const T* pl = lu.data();
    T* px = rhs.data();
    const crd::i64 sl = lu.stride(0);
    const crd::i64 sx = rhs.stride(0);
    const crd::i32* ppiv = piv.data();
    const auto solve_one = [=](crd::u64 b) noexcept
    {
        const T* m = pl + static_cast<crd::i64>(b) * sl;
        T* x = px + static_cast<crd::i64>(b) * sx;
        const crd::i32* pv = ppiv + b * n;
        for (crd::u64 j = 0; j < n; ++j) // pivot application (sequential swaps)
        {
            const crd::u64 pj = static_cast<crd::u64>(pv[j]);
            if (pj != j)
            {
                for (crd::u64 c = 0; c < r; ++c)
                {
                    const T t = x[j * r + c];
                    x[j * r + c] = x[pj * r + c];
                    x[pj * r + c] = t;
                }
            }
        }
        for (crd::u64 i = 1; i < n; ++i) // unit-lower forward
        {
            for (crd::u64 c = 0; c < r; ++c)
            {
                T s = x[i * r + c];
                for (crd::u64 p = 0; p < i; ++p)
                {
                    s = std::fma(-m[i * n + p], x[p * r + c], s);
                }
                x[i * r + c] = s;
            }
        }
        for (crd::u64 ii = n; ii-- > 0U;) // upper backward
        {
            const T inv = T(1) / m[ii * n + ii];
            for (crd::u64 c = 0; c < r; ++c)
            {
                T s = x[ii * r + c];
                for (crd::u64 p = ii + 1U; p < n; ++p)
                {
                    s = std::fma(-m[ii * n + p], x[p * r + c], s);
                }
                x[ii * r + c] = s * inv;
            }
        }
    };
    const auto run_range = [=](crd::u32 lo, crd::u32 hi) noexcept
    {
        for (crd::u32 b = lo; b < hi; ++b)
        {
            solve_one(b);
        }
    };
    crd::u32 nw = num_workers;
    if (nw == 0U)
    {
        nw = crd::jobs::num_workers();
    }
    if (nw <= 1U || batch < 2U)
    {
        run_range(0U, static_cast<crd::u32>(batch));
        return TensorStatus::Ok;
    }
    struct Ctx
    {
        const decltype(run_range)* run;
    };
    Ctx ctx{&run_range};
    Ctx* const cp = &ctx;
    auto* const counter = crd::jobs::parallel_for(static_cast<crd::u32>(batch), nw,
                                                  [cp](crd::u32 lo, crd::u32 hi) { (*cp->run)(lo, hi); });
    crd::jobs::wait(counter);
    return TensorStatus::Ok;
}

// C[b] = alpha * A[b] @ B[b] + beta * C[b]  for b in [0, batch)
// A=[B,m,k] B=[B,k,n] C=[B,m,n], tight inner dims (see the layout contract).
// num_workers: 0 = use the jobs pool when profitable (serial without a pool);
// any worker count produces BIT-IDENTICAL results (disjoint outputs, fixed
// per-matrix kernels).
template <typename T>
[[nodiscard]] TensorStatus batched_gemm(T alpha, TensorView<const T> a, TensorView<const T> b, T beta,
                                        TensorView<T> c, crd::memory::IAllocator* scratch,
                                        crd::u32 num_workers = 0) noexcept
{
    using namespace batcheddetail;
    if (!tight3(a) || !tight3(b) || !tight3(c))
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 batch = a.shape(0);
    const crd::u64 m = a.shape(1);
    const crd::u64 k = a.shape(2);
    const crd::u64 n = b.shape(2);
    if (b.shape(0) != batch || c.shape(0) != batch || b.shape(1) != k || c.shape(1) != m || c.shape(2) != n)
    {
        return TensorStatus::ShapeMismatch;
    }
    if (batch == 0U)
    {
        return TensorStatus::Ok;
    }
    const T* pa = a.data();
    const T* pb = b.data();
    T* pc = c.data();
    const crd::i64 sa = a.stride(0);
    const crd::i64 sb = b.stride(0);
    const crd::i64 sc = c.stride(0);
    const bool tiny = 2ULL * m * n * k <= 32ULL * 1024ULL; // allocation-free direct tier
    // the dense-gemm tier packs via `scratch` — concurrent workers get a
    // thread-safe wrap (contended once per pack, amortized at these sizes)
    crd::memory::ThreadSafeAllocator ts_scratch(scratch);
    const auto run_range = [=, &ts_scratch](crd::u32 begin, crd::u32 end, bool threaded) noexcept
    {
        for (crd::u32 i = begin; i < end; ++i)
        {
            const T* ai = pa + static_cast<crd::i64>(i) * sa;
            const T* bi = pb + static_cast<crd::i64>(i) * sb;
            T* ci = pc + static_cast<crd::i64>(i) * sc;
            if (tiny)
            {
                direct_gemm_tiny(alpha, ai, bi, beta, ci, m, k, n);
                continue;
            }
            const crd::hesap::dense::MatrixView<const T, crd::hesap::dense::Layout::RowMajor> av{
                ai, static_cast<crd::usize>(m), static_cast<crd::usize>(k), static_cast<crd::usize>(k)};
            const crd::hesap::dense::MatrixView<const T, crd::hesap::dense::Layout::RowMajor> bv{
                bi, static_cast<crd::usize>(k), static_cast<crd::usize>(n), static_cast<crd::usize>(n)};
            crd::hesap::dense::MatrixView<T, crd::hesap::dense::Layout::RowMajor> cv{
                ci, static_cast<crd::usize>(m), static_cast<crd::usize>(n), static_cast<crd::usize>(n)};
            crd::hesap::dense::gemm<T, crd::hesap::dense::Layout::RowMajor>(
                alpha, av, bv, beta, cv, crd::hesap::dense::Trans::None, crd::hesap::dense::Trans::None,
                threaded ? static_cast<crd::memory::IAllocator*>(&ts_scratch) : scratch);
        }
    };
    crd::u32 nw = num_workers;
    if (nw == 0U)
    {
        nw = crd::jobs::num_workers();
    }
    if (nw <= 1U || batch < 2U)
    {
        run_range(0U, static_cast<crd::u32>(batch), false);
        return TensorStatus::Ok;
    }
    // across-batch parallel: disjoint C slices per task; grain = f(shape) only
    const crd::u64 flops = 2ULL * m * n * k;
    const crd::u32 grain = batch_grain(batch, flops);
    const crd::u32 tasks = static_cast<crd::u32>((batch + grain - 1U) / grain);
    struct Ctx
    {
        const decltype(run_range)* run;
        crd::u32 grain;
        crd::u32 batch;
    };
    Ctx ctx{&run_range, grain, static_cast<crd::u32>(batch)};
    Ctx* const cp = &ctx;
    auto* const counter = crd::jobs::parallel_for(tasks, nw, [cp](crd::u32 tb, crd::u32 te) {
        for (crd::u32 t = tb; t < te; ++t)
        {
            const crd::u32 lo = t * cp->grain;
            crd::u32 hi = lo + cp->grain;
            if (hi > cp->batch)
            {
                hi = cp->batch;
            }
            (*cp->run)(lo, hi, true);
        }
    });
    crd::jobs::wait(counter);
    return TensorStatus::Ok;
}

} // namespace crd::hesap::tensor
