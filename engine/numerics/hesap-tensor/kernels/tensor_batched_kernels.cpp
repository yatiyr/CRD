// The batched dense linear algebra of crd/hesap/tensor/tensor_kernels.hpp (BatchedKernels), the third unit of the
// tensor family, compiled once per CPU level by crd_add_cpu_kernels (../CMakeLists.txt, ADR-0134 §5).
//
// The tiny GEMM keeps up to eight independent fma chains in registers (R <= 4 rows by two column vectors, or 5 to 8
// rows by one), its column tails on partial loads and stores; at avx512 a column vector is a Vec16f or Vec8d. The
// factorizations run the lane tier: a group of kEwLanes matrices packed element-major, one matrix per lane, so one
// vector square root or division serves the group. At avx512 a step takes two groups in one zmm when their staging
// tile fits the one-group tile, which keeps the stack at its 32 KiB on a jobs fiber (64 KiB). Every lane runs the
// scalar tier's operations (batched_core.hpp) and a lane that is done keeps its bits through a select, so each matrix
// gives the same bits on every level, in every lane and in the scalar tier.

#include "tensor_batched_kernels.hpp"

#include <crd/hesap/tensor/batched_core.hpp>
#include <crd/math/libm.hpp>
#include <crd/math/simd/simd.hpp>
#include <crd/math/simd/vec16f.hpp>
#include <crd/math/simd/vec8d.hpp>

#include <type_traits>

namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
{
namespace
{

namespace simd = crd::math::simd;
namespace libm = crd::math::libm;

using crd::f32;
using crd::f64;
using crd::i32;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::usize;

// One lane group, and the register a step takes when two groups fit: two groups at avx512, else one.
template <typename T> using Group = std::conditional_t<std::is_same_v<T, f64>, simd::Vec4d, simd::Vec8f>;
#if CRD_SIMD_HAS_AVX512
template <typename T> using Wide = std::conditional_t<std::is_same_v<T, f64>, simd::Vec8d, simd::Vec16f>;
#else
template <typename T> using Wide = Group<T>;
#endif
template <typename T, typename V> inline constexpr u64 kLanes = sizeof(V) / sizeof(T);
static_assert(kLanes<f32, Group<f32>> == kEwLanes<f32> && kLanes<f64, Group<f64>> == kEwLanes<f64>);

// ---- tiny GEMM ----------------------------------------------------------------------------------------------------

// The rows [i0, i0 + R) of c, R <= 4, in blocks of two column vectors.
template <typename T, typename V, u32 R>
void gemm_rows(T alpha, const T* a, const T* b, T beta, T* c, u64 i0, u64 k, u64 n) noexcept
{
    constexpr u64 lanes = kLanes<T, V>;
    for (u64 j0 = 0; j0 < n; j0 += 2U * lanes)
    {
        const u64 left = n - j0;
        const u64 c0 = left < lanes ? left : lanes; // the lanes of vector 0
        const u64 rest = left - c0;
        const u64 c1 = rest < lanes ? rest : lanes; // the lanes of vector 1
        V acc0[R];
        V acc1[R];
        for (u32 r = 0; r < R; ++r)
        {
            acc0[r] = V(T(0));
            acc1[r] = V(T(0));
        }
        for (u64 p = 0; p < k; ++p)
        {
            const T* brow = b + p * n + j0;
            const V b0 = c0 == lanes ? V::load(brow) : V::load_partial(brow, static_cast<usize>(c0));
            V b1 = V(T(0));
            if (c1 != 0U)
            {
                b1 = c1 == lanes ? V::load(brow + lanes) : V::load_partial(brow + lanes, static_cast<usize>(c1));
            }
            for (u32 r = 0; r < R; ++r)
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
        for (u32 r = 0; r < R; ++r)
        {
            T* crow = c + (i0 + r) * n + j0;
            V out0 = acc0[r] * valpha;
            if (beta != T(0))
            {
                const V cold = c0 == lanes ? V::load(crow) : V::load_partial(crow, static_cast<usize>(c0));
                out0 = simd::fma(vbeta, cold, out0);
            }
            if (c0 == lanes)
            {
                out0.store(crow);
            }
            else
            {
                out0.store_partial(crow, static_cast<usize>(c0));
            }
            if (c1 != 0U)
            {
                V out1 = acc1[r] * valpha;
                if (beta != T(0))
                {
                    const V cold =
                        c1 == lanes ? V::load(crow + lanes) : V::load_partial(crow + lanes, static_cast<usize>(c1));
                    out1 = simd::fma(vbeta, cold, out1);
                }
                if (c1 == lanes)
                {
                    out1.store(crow + lanes);
                }
                else
                {
                    out1.store_partial(crow + lanes, static_cast<usize>(c1));
                }
            }
        }
    }
}

// All R rows of c (5 to 8) in blocks of one column vector: R chains hide the fma latency, and b streams once.
template <typename T, typename V, u32 R>
void gemm_rows_1v(T alpha, const T* a, const T* b, T beta, T* c, u64 k, u64 n) noexcept
{
    constexpr u64 lanes = kLanes<T, V>;
    for (u64 j0 = 0; j0 < n; j0 += lanes)
    {
        const u64 cw = n - j0 < lanes ? n - j0 : lanes;
        V acc[R];
        for (u32 r = 0; r < R; ++r)
        {
            acc[r] = V(T(0));
        }
        for (u64 p = 0; p < k; ++p)
        {
            const T* brow = b + p * n + j0;
            const V bv = cw == lanes ? V::load(brow) : V::load_partial(brow, static_cast<usize>(cw));
            for (u32 r = 0; r < R; ++r)
            {
                acc[r] = simd::fma(V(a[r * k + p]), bv, acc[r]);
            }
        }
        const V valpha = V(alpha);
        const V vbeta = V(beta);
        for (u32 r = 0; r < R; ++r)
        {
            T* crow = c + r * n + j0;
            V out = acc[r] * valpha;
            if (beta != T(0))
            {
                const V cold = cw == lanes ? V::load(crow) : V::load_partial(crow, static_cast<usize>(cw));
                out = simd::fma(vbeta, cold, out);
            }
            if (cw == lanes)
            {
                out.store(crow);
            }
            else
            {
                out.store_partial(crow, static_cast<usize>(cw));
            }
        }
    }
}

template <typename T> void gemm_one(T alpha, const T* a, const T* b, T beta, T* c, u64 m, u64 k, u64 n) noexcept
{
    using V = Wide<T>;
    switch (m)
    {
        case 5U:
            gemm_rows_1v<T, V, 5U>(alpha, a, b, beta, c, k, n);
            return;
        case 6U:
            gemm_rows_1v<T, V, 6U>(alpha, a, b, beta, c, k, n);
            return;
        case 7U:
            gemm_rows_1v<T, V, 7U>(alpha, a, b, beta, c, k, n);
            return;
        case 8U:
            gemm_rows_1v<T, V, 8U>(alpha, a, b, beta, c, k, n);
            return;
        default:
            break;
    }
    u64 i = 0;
    for (; i + 4U <= m; i += 4U)
    {
        gemm_rows<T, V, 4U>(alpha, a, b, beta, c, i, k, n);
    }
    switch (m - i)
    {
        case 3U:
            gemm_rows<T, V, 3U>(alpha, a, b, beta, c, i, k, n);
            break;
        case 2U:
            gemm_rows<T, V, 2U>(alpha, a, b, beta, c, i, k, n);
            break;
        case 1U:
            gemm_rows<T, V, 1U>(alpha, a, b, beta, c, i, k, n);
            break;
        default:
            break;
    }
}

// ---- lane tier ----------------------------------------------------------------------------------------------------

// Two groups take one step when their n x n staging tile fits the tile of one group at the tier's largest n.
[[nodiscard]] constexpr bool two_groups_fit(u64 n, u64 max_n) noexcept
{
    return n * n * 2U <= max_n * max_n;
}

// The matrices [b0, b1) in lane steps: two groups (one Wide register) while more than one group is left and two fit,
// else one group; a step's last lanes may hold no matrix. step(std::type_identity<V>, first matrix, matrices).
template <typename T, typename Step> void lane_steps(u64 b0, u64 b1, bool two_fit, const Step& step) noexcept
{
    constexpr u64 group = kEwLanes<T>;
    (void)two_fit;
    for (u64 lo = b0; lo < b1;)
    {
        const u64 left = b1 - lo;
        if constexpr (!std::is_same_v<Wide<T>, Group<T>>)
        {
            if (two_fit && left > group)
            {
                constexpr u64 wide = kLanes<T, Wide<T>>;
                const u64 w = left < wide ? left : wide;
                step(std::type_identity<Wide<T>>{}, lo, w);
                lo += w;
                continue;
            }
        }
        const u64 w = left < group ? left : group;
        step(std::type_identity<Group<T>>{}, lo, w);
        lo += w;
    }
}

// mats[0 .. w) into the element-major tile buf[n * n][L]; the lanes past w hold the identity, so their square roots
// and divisions stay finite.
template <typename T, u64 L> void pack(const T* const* mats, u64 n, u64 w, T* buf) noexcept
{
    for (u64 e = 0; e < n * n; ++e)
    {
        const T identity = e % (n + 1U) == 0U ? T(1) : T(0);
        for (u64 q = 0; q < L; ++q)
        {
            buf[e * L + q] = identity;
        }
        for (u64 q = 0; q < w; ++q)
        {
            buf[e * L + q] = mats[q][e];
        }
    }
}

template <typename T, u64 L> void unpack(const T* buf, u64 n, u64 w, T* const* mats) noexcept
{
    for (u64 q = 0; q < w; ++q)
    {
        for (u64 e = 0; e < n * n; ++e)
        {
            mats[q][e] = buf[e * L + q];
        }
    }
}

// chol_scalar_one on every lane of the tile; info[0 .. w).
template <typename T, typename V> void chol_lanes(T* buf, u64 n, i32* info, u64 w) noexcept
{
    constexpr u64 lanes = kLanes<T, V>;
    const auto at = [buf, n](u64 i, u64 j) noexcept -> T*
    {
        return buf + (i * n + j) * lanes;
    };
    i32 flagged[lanes] = {};
    for (u64 j = 0; j < n; ++j)
    {
        V d = V::load(at(j, j));
        for (u64 p = 0; p < j; ++p)
        {
            const V l = V::load(at(j, p));
            d = simd::fnmadd(l, l, d);
        }
        // the lane's pivot check as a vector select, its flag read from a stored mask (the MSVC auto-vectorization
        // scar of the LU pivot scan)
        const V good = simd::cmp_gt(d, V(T(0)));
        d = simd::select(good, d, V(T(1)));
        alignas(64) T gm[lanes];
        good.store(gm);
        for (u64 q = 0; q < lanes; ++q)
        {
            if (gm[q] == T(0) && flagged[q] == 0)
            {
                flagged[q] = static_cast<i32>(j) + 1;
            }
        }
        const V ljj = simd::sqrt(d);
        const V inv = V(T(1)) / ljj;
        ljj.store(at(j, j));
        for (u64 i = j + 1U; i < n; ++i)
        {
            V s = V::load(at(i, j));
            for (u64 p = 0; p < j; ++p)
            {
                s = simd::fnmadd(V::load(at(i, p)), V::load(at(j, p)), s);
            }
            (s * inv).store(at(i, j));
        }
        const V z = V(T(0));
        for (u64 i = 0; i < j; ++i)
        {
            z.store(at(i, j));
        }
    }
    for (u64 q = 0; q < w; ++q)
    {
        info[q] = flagged[q];
    }
}

// lu_scalar_one on every lane of the tile; piv[q * n + j] and info[q] for q < w.
template <typename T, typename V> void lu_lanes(T* buf, u64 n, i32* piv, i32* info, u64 w) noexcept
{
    constexpr u64 lanes = kLanes<T, V>;
    const auto at = [buf, n](u64 i, u64 j) noexcept -> T*
    {
        return buf + (i * n + j) * lanes;
    };
    i32 flagged[lanes] = {};
    for (u64 j = 0; j < n; ++j)
    {
        // The pivot search as a pure vector argmax (strictly greater wins, so the lowest row takes a tie). The per-lane
        // scalar form `if (v > best[q]) { best[q] = v; pr[q] = i; }` is miscompiled by MSVC /O1 and /O2
        // auto-vectorization (wrong masked blends over the two-array conditional update; a standalone repro on
        // 2026-07-05, /Od and GCC correct). The row indices ride the T lanes, exact for n <= kBatchedCholMaxN.
        V bestv = simd::abs(V::load(at(j, j)));
        V bestidx = V(static_cast<T>(j));
        for (u64 i = j + 1U; i < n; ++i)
        {
            const V v = simd::abs(V::load(at(i, j)));
            const V m = simd::cmp_gt(v, bestv);
            bestv = simd::select(m, v, bestv);
            bestidx = simd::select(m, V(static_cast<T>(i)), bestidx);
        }
        u64 pr[lanes];
        alignas(64) T bi[lanes];
        bestidx.store(bi);
        for (u64 q = 0; q < lanes; ++q)
        {
            pr[q] = static_cast<u64>(bi[q]);
        }
        for (u64 q = 0; q < lanes; ++q)
        {
            if (q < w)
            {
                piv[q * n + j] = static_cast<i32>(pr[q]);
            }
            if (pr[q] != j)
            {
                for (u64 c = 0; c < n; ++c)
                {
                    T* rj = at(j, c) + q;
                    T* rp = at(pr[q], c) + q;
                    const T t = *rj;
                    *rj = *rp;
                    *rp = t;
                }
            }
        }
        // a singular lane continues on a pivot of 1, its flag read from a stored mask (the scar above)
        const V praw = V::load(at(j, j));
        const V bad = simd::cmp_eq(praw, V(T(0)));
        const V p = simd::select(bad, V(T(1)), praw);
        alignas(64) T bm[lanes];
        bad.store(bm);
        for (u64 q = 0; q < lanes; ++q)
        {
            if (bm[q] != T(0) && flagged[q] == 0)
            {
                flagged[q] = static_cast<i32>(j) + 1;
            }
        }
        const V inv = V(T(1)) / p;
        for (u64 i = j + 1U; i < n; ++i)
        {
            const V l = V::load(at(i, j)) * inv;
            l.store(at(i, j));
            for (u64 c = j + 1U; c < n; ++c)
            {
                simd::fnmadd(l, V::load(at(j, c)), V::load(at(i, c))).store(at(i, c));
            }
        }
    }
    for (u64 q = 0; q < w; ++q)
    {
        info[q] = flagged[q];
    }
}

// svd_scalar_sweeps on every lane of the tiles a and v. A pair's rotation applies through a select, so a lane that
// does not rotate keeps its exact bits (an arithmetic identity would flip -0), and a lane done before the others
// computes the same no-rotation sweep again. info[q] for q < w.
template <typename T, typename V> void svd_lanes(T* abuf, T* vbuf, u64 n, u32 max_sweeps, i32* info, u64 w) noexcept
{
    constexpr u64 lanes = kLanes<T, V>;
    const auto at = [n](T* buf, u64 r, u64 c) noexcept -> T*
    {
        return buf + (r * n + c) * lanes;
    };
    const V zero = V(T(0));
    const V one = V(T(1));
    const V two = V(T(2));
    for (u64 e = 0; e < n * n; ++e)
    {
        zero.store(vbuf + e * lanes);
    }
    for (u64 i = 0; i < n; ++i)
    {
        one.store(at(vbuf, i, i));
    }
    const V tol = V(batcheddetail::svd_tol<T>());
    i32 lane_info[lanes];
    for (u64 q = 0; q < lanes; ++q)
    {
        lane_info[q] = 1;
    }
    for (u32 sweep = 0; sweep < max_sweeps; ++sweep)
    {
        V any_rot = zero; // the lanes that rotated in this sweep
        for (u64 p = 0; p + 1U < n; ++p)
        {
            for (u64 q = p + 1U; q < n; ++q)
            {
                V alpha = zero;
                V beta = zero;
                V gamma = zero;
                for (u64 r = 0; r < n; ++r)
                {
                    const V ap = V::load(at(abuf, r, p));
                    const V aq = V::load(at(abuf, r, q));
                    alpha = simd::fma(ap, ap, alpha);
                    beta = simd::fma(aq, aq, beta);
                    gamma = simd::fma(ap, aq, gamma);
                }
                const V rot = simd::cmp_gt(simd::abs(gamma), tol * simd::sqrt(alpha * beta));
                alignas(64) T rl[lanes];
                rot.store(rl);
                bool any = false;
                for (u64 q0 = 0; q0 < lanes; ++q0)
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
                const V gsafe = simd::select(rot, gamma, one); // no division by 0 on a lane that skips
                const V zeta = (beta - alpha) / (two * gsafe);
                const V sgn = simd::select(simd::cmp_ge(zeta, zero), one, V(T(-1)));
                const V t = sgn / (simd::abs(zeta) + simd::sqrt(simd::fma(zeta, zeta, one)));
                const V c = one / simd::sqrt(simd::fma(t, t, one));
                const V s = c * t;
                for (u64 r = 0; r < n; ++r)
                {
                    const V ap = V::load(at(abuf, r, p));
                    const V aq = V::load(at(abuf, r, q));
                    simd::select(rot, simd::fma(c, ap, zero - s * aq), ap).store(at(abuf, r, p));
                    simd::select(rot, simd::fma(s, ap, c * aq), aq).store(at(abuf, r, q));
                    const V vp = V::load(at(vbuf, r, p));
                    const V vq = V::load(at(vbuf, r, q));
                    simd::select(rot, simd::fma(c, vp, zero - s * vq), vp).store(at(vbuf, r, p));
                    simd::select(rot, simd::fma(s, vp, c * vq), vq).store(at(vbuf, r, q));
                }
            }
        }
        alignas(64) T al[lanes];
        any_rot.store(al);
        bool all_done = true;
        for (u64 q = 0; q < lanes; ++q)
        {
            if (al[q] == T(0) && lane_info[q] == 1)
            {
                lane_info[q] = 0; // the lane's first quiet sweep: converged
            }
            if (al[q] != T(0))
            {
                all_done = false;
            }
        }
        if (all_done)
        {
            break;
        }
    }
    for (u64 q = 0; q < w; ++q)
    {
        info[q] = lane_info[q];
    }
}

// The staging tiles: one group at the tier's largest n.
template <typename T> inline constexpr u64 kFactorTile = kBatchedCholMaxN * kBatchedCholMaxN * kEwLanes<T>;
template <typename T> inline constexpr u64 kSvdTile = kBatchedSvdMaxN * kBatchedSvdMaxN * kEwLanes<T>;

} // namespace

template <typename T> void BatchedKernel<T>::gemm_tiny(const BatchedGemm<T>& g, u64 b0, u64 b1) noexcept
{
    for (u64 i = b0; i < b1; ++i)
    {
        gemm_one<T>(g.alpha, g.a + static_cast<i64>(i) * g.sa, g.b + static_cast<i64>(i) * g.sb, g.beta,
                    g.c + static_cast<i64>(i) * g.sc, g.m, g.k, g.n);
    }
}

template <typename T> void BatchedKernel<T>::cholesky(const BatchedFactor<T>& f, u64 b0, u64 b1) noexcept
{
    const u64 n = f.n;
    if (n > kBatchedCholMaxN)
    {
        for (u64 i = b0; i < b1; ++i)
        {
            batcheddetail::chol_scalar_one(f.a + static_cast<i64>(i) * f.sa, n, f.info + i);
        }
        return;
    }
    alignas(64) T buf[kFactorTile<T>];
    const auto step = [&f, n, &buf]<typename V>(std::type_identity<V>, u64 lo, u64 w) noexcept
    {
        constexpr u64 lanes = kLanes<T, V>;
        T* mats[lanes] = {};
        for (u64 q = 0; q < w; ++q)
        {
            mats[q] = f.a + static_cast<i64>(lo + q) * f.sa;
        }
        pack<T, lanes>(mats, n, w, buf);
        chol_lanes<T, V>(buf, n, f.info + lo, w);
        unpack<T, lanes>(buf, n, w, mats);
    };
    lane_steps<T>(b0, b1, two_groups_fit(n, kBatchedCholMaxN), step);
}

template <typename T> void BatchedKernel<T>::lu(const BatchedFactor<T>& f, u64 b0, u64 b1) noexcept
{
    const u64 n = f.n;
    if (n > kBatchedCholMaxN)
    {
        for (u64 i = b0; i < b1; ++i)
        {
            batcheddetail::lu_scalar_one(f.a + static_cast<i64>(i) * f.sa, n, f.piv + i * n, f.info + i);
        }
        return;
    }
    alignas(64) T buf[kFactorTile<T>];
    const auto step = [&f, n, &buf]<typename V>(std::type_identity<V>, u64 lo, u64 w) noexcept
    {
        constexpr u64 lanes = kLanes<T, V>;
        T* mats[lanes] = {};
        for (u64 q = 0; q < w; ++q)
        {
            mats[q] = f.a + static_cast<i64>(lo + q) * f.sa;
        }
        pack<T, lanes>(mats, n, w, buf);
        lu_lanes<T, V>(buf, n, f.piv + lo * n, f.info + lo, w);
        unpack<T, lanes>(buf, n, w, mats);
    };
    lane_steps<T>(b0, b1, two_groups_fit(n, kBatchedCholMaxN), step);
}

template <typename T> void BatchedKernel<T>::svd(const BatchedSvd<T>& s, u64 b0, u64 b1) noexcept
{
    const u64 n = s.n;
    if (n > kBatchedSvdMaxN)
    {
        for (u64 i = b0; i < b1; ++i)
        {
            const T* a = s.a + static_cast<i64>(i) * s.sa;
            T* u = s.u + static_cast<i64>(i) * s.su;
            T* v = s.v + static_cast<i64>(i) * s.sv;
            for (u64 e = 0; e < n * n; ++e)
            {
                u[e] = a[e];
            }
            batcheddetail::svd_scalar_sweeps(u, v, n, s.max_sweeps, s.info + i);
            batcheddetail::svd_finalize_one(u, v, s.sigma + i * n, n);
        }
        return;
    }
    alignas(64) T abuf[kSvdTile<T>];
    alignas(64) T vbuf[kSvdTile<T>];
    const auto step = [&s, n, &abuf, &vbuf]<typename V>(std::type_identity<V>, u64 lo, u64 w) noexcept
    {
        constexpr u64 lanes = kLanes<T, V>;
        const T* src[lanes] = {};
        T* du[lanes] = {};
        T* dv[lanes] = {};
        for (u64 q = 0; q < w; ++q)
        {
            src[q] = s.a + static_cast<i64>(lo + q) * s.sa;
            du[q] = s.u + static_cast<i64>(lo + q) * s.su;
            dv[q] = s.v + static_cast<i64>(lo + q) * s.sv;
        }
        pack<T, lanes>(src, n, w, abuf);
        svd_lanes<T, V>(abuf, vbuf, n, s.max_sweeps, s.info + lo, w);
        unpack<T, lanes>(abuf, n, w, du);
        unpack<T, lanes>(vbuf, n, w, dv);
        for (u64 q = 0; q < w; ++q)
        {
            batcheddetail::svd_finalize_one(du[q], dv[q], s.sigma + (lo + q) * n, n);
        }
    };
    lane_steps<T>(b0, b1, two_groups_fit(n, kBatchedSvdMaxN), step);
}

template <typename T> void BatchedKernel<T>::cholesky_solve(const BatchedSolve<T>& s, u64 b0, u64 b1) noexcept
{
    const u64 n = s.n;
    const u64 r = s.r;
    for (u64 b = b0; b < b1; ++b)
    {
        const T* l = s.f + static_cast<i64>(b) * s.sf;
        T* x = s.x + static_cast<i64>(b) * s.sx;
        for (u64 i = 0; i < n; ++i) // lanes y = x
        {
            const T inv = T(1) / l[i * n + i];
            for (u64 c = 0; c < r; ++c)
            {
                T acc = x[i * r + c];
                for (u64 p = 0; p < i; ++p)
                {
                    acc = libm::fma(-l[i * n + p], x[p * r + c], acc);
                }
                x[i * r + c] = acc * inv;
            }
        }
        for (u64 ii = n; ii-- > 0U;) // lanes^T x = y
        {
            const T inv = T(1) / l[ii * n + ii];
            for (u64 c = 0; c < r; ++c)
            {
                T acc = x[ii * r + c];
                for (u64 p = ii + 1U; p < n; ++p)
                {
                    acc = libm::fma(-l[p * n + ii], x[p * r + c], acc);
                }
                x[ii * r + c] = acc * inv;
            }
        }
    }
}

template <typename T> void BatchedKernel<T>::lu_solve(const BatchedSolve<T>& s, u64 b0, u64 b1) noexcept
{
    const u64 n = s.n;
    const u64 r = s.r;
    for (u64 b = b0; b < b1; ++b)
    {
        const T* m = s.f + static_cast<i64>(b) * s.sf;
        T* x = s.x + static_cast<i64>(b) * s.sx;
        const i32* pv = s.piv + b * n;
        for (u64 j = 0; j < n; ++j) // the pivot rows, swapped in order
        {
            const u64 pj = static_cast<u64>(pv[j]);
            if (pj != j)
            {
                for (u64 c = 0; c < r; ++c)
                {
                    const T t = x[j * r + c];
                    x[j * r + c] = x[pj * r + c];
                    x[pj * r + c] = t;
                }
            }
        }
        for (u64 i = 1; i < n; ++i) // unit lower, forward
        {
            for (u64 c = 0; c < r; ++c)
            {
                T acc = x[i * r + c];
                for (u64 p = 0; p < i; ++p)
                {
                    acc = libm::fma(-m[i * n + p], x[p * r + c], acc);
                }
                x[i * r + c] = acc;
            }
        }
        for (u64 ii = n; ii-- > 0U;) // upper, backward
        {
            const T inv = T(1) / m[ii * n + ii];
            for (u64 c = 0; c < r; ++c)
            {
                T acc = x[ii * r + c];
                for (u64 p = ii + 1U; p < n; ++p)
                {
                    acc = libm::fma(-m[ii * n + p], x[p * r + c], acc);
                }
                x[ii * r + c] = acc * inv;
            }
        }
    }
}

template struct BatchedKernel<f32>;
template struct BatchedKernel<f64>;

} // namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
