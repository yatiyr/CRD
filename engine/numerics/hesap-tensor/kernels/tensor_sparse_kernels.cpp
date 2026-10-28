// The sparse tensor walks of crd/hesap/tensor/tensor_kernels.hpp (SparseTensorKernels), the fourth unit of the tensor
// family, compiled once per CPU level by crd_add_cpu_kernels (../CMakeLists.txt, ADR-0134 §5).
//
// The TTM scatter, the staged tile, the part fold and both MTTKRP walks step their r (or f) columns in registers of
// kEwLanes values (Vec8f, Vec4d; scalar arrays on portable), two groups in one Vec16f or Vec8d at avx512, the column
// tails on partial loads and stores; a fiber of leaves keeps four registers of sums over its leaves before it
// multiplies its parent's row in. The strided scatter of an outer mode is a scalar fma per element. Every output
// element is one chain of single-rounded fmas (simd::fma and libm::fma round once on every level) in the canonical
// nonzero order, or the parts' sums in part order, so the register width changes no bit.

#include "tensor_sparse_kernels.hpp"

#include <crd/core/platform.hpp>
#include <crd/math/libm.hpp>
#include <crd/math/simd/simd.hpp>
#include <crd/math/simd/vec16f.hpp>
#include <crd/math/simd/vec8d.hpp>

#include <cstring>
#include <type_traits>

namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
{
namespace
{

namespace simd = crd::math::simd;
namespace libm = crd::math::libm;

using crd::f32;
using crd::f64;
using crd::u32;
using crd::u64;
using crd::usize;

// The register a step takes: two lane groups at avx512, else one.
#if CRD_SIMD_HAS_AVX512
template <typename T> using Wide = std::conditional_t<std::is_same_v<T, f64>, simd::Vec8d, simd::Vec16f>;
#else
template <typename T> using Wide = std::conditional_t<std::is_same_v<T, f64>, simd::Vec4d, simd::Vec8f>;
#endif
template <typename T> inline constexpr u64 kWide = sizeof(Wide<T>) / sizeof(T);
static_assert(kWide<f32> % kEwLanes<f32> == 0U && kWide<f64> % kEwLanes<f64> == 0U);

// dst[q] = fma(a, row[q], dst[q]) for q in [0, r).
template <typename T> CRD_FORCEINLINE void axpy_row(T* dst, const T* row, T a, u64 r) noexcept
{
    using V = Wide<T>;
    const V av(a);
    u64 q = 0;
    for (; q + kWide<T> <= r; q += kWide<T>)
    {
        simd::fma(av, V::load(row + q), V::load(dst + q)).store(dst + q);
    }
    if (q < r)
    {
        const usize c = static_cast<usize>(r - q);
        simd::fma(av, V::load_partial(row + q, c), V::load_partial(dst + q, c)).store_partial(dst + q, c);
    }
}

// dst[q] = fma(a[q], b[q], dst[q]) for q in [0, r).
template <typename T> CRD_FORCEINLINE void hadamard_acc(T* dst, const T* a, const T* b, u64 r) noexcept
{
    using V = Wide<T>;
    u64 q = 0;
    for (; q + kWide<T> <= r; q += kWide<T>)
    {
        simd::fma(V::load(a + q), V::load(b + q), V::load(dst + q)).store(dst + q);
    }
    if (q < r)
    {
        const usize c = static_cast<usize>(r - q);
        simd::fma(V::load_partial(a + q, c), V::load_partial(b + q, c), V::load_partial(dst + q, c))
            .store_partial(dst + q, c);
    }
}

// dst[q] += src[q] for q in [0, r).
template <typename T> CRD_FORCEINLINE void add_acc(T* dst, const T* src, u64 r) noexcept
{
    using V = Wide<T>;
    u64 q = 0;
    for (; q + kWide<T> <= r; q += kWide<T>)
    {
        (V::load(dst + q) + V::load(src + q)).store(dst + q);
    }
    if (q < r)
    {
        const usize c = static_cast<usize>(r - q);
        (V::load_partial(dst + q, c) + V::load_partial(src + q, c)).store_partial(dst + q, c);
    }
}

// The output element offset of nonzero e over the modes [m0, m1) other than skip.
template <typename T> CRD_FORCEINLINE u64 offset_of(const SparseTtmWalk<T>& w, u64 e, u32 m0, u32 m1, u32 skip) noexcept
{
    u64 off = 0;
    for (u32 m = m0; m < m1; ++m)
    {
        if (m != skip)
        {
            off += static_cast<u64>(w.planes[m][e]) * w.ostride[m];
        }
    }
    return off;
}

// A fiber whose children are leaves: dst[q] = fma(sum[q], prow[q], dst[q]), sum[q] the fma chain over the leaves
// [cb, ce) of val[t] * (leaf factor row lf[t])[q], seeded by 0, in registers.
template <typename T> void leaf_fiber(const MttkrpCsfWalk<T>& w, u32 cb, u32 ce, const T* prow, T* dst) noexcept
{
    using V = Wide<T>;
    constexpr u64 lanes = kWide<T>;
    const u32* lf = w.fids[w.rank - 1U];
    const T* fac = w.fac[w.rank - 1U];
    const T* val = w.val;
    const u64 r = w.r;
    u64 q0 = 0;
    for (; q0 + 4U * lanes <= r; q0 += 4U * lanes)
    {
        V a0(T(0));
        V a1(T(0));
        V a2(T(0));
        V a3(T(0));
        for (u32 t = cb; t < ce; ++t)
        {
            const T* row = fac + static_cast<u64>(lf[t]) * r + q0;
            const V vv(val[t]);
            a0 = simd::fma(vv, V::load(row), a0);
            a1 = simd::fma(vv, V::load(row + lanes), a1);
            a2 = simd::fma(vv, V::load(row + 2U * lanes), a2);
            a3 = simd::fma(vv, V::load(row + 3U * lanes), a3);
        }
        const T* p = prow + q0;
        T* d = dst + q0;
        simd::fma(a0, V::load(p), V::load(d)).store(d);
        simd::fma(a1, V::load(p + lanes), V::load(d + lanes)).store(d + lanes);
        simd::fma(a2, V::load(p + 2U * lanes), V::load(d + 2U * lanes)).store(d + 2U * lanes);
        simd::fma(a3, V::load(p + 3U * lanes), V::load(d + 3U * lanes)).store(d + 3U * lanes);
    }
    for (; q0 + lanes <= r; q0 += lanes)
    {
        V a0(T(0));
        for (u32 t = cb; t < ce; ++t)
        {
            a0 = simd::fma(V(val[t]), V::load(fac + static_cast<u64>(lf[t]) * r + q0), a0);
        }
        simd::fma(a0, V::load(prow + q0), V::load(dst + q0)).store(dst + q0);
    }
    if (q0 < r)
    {
        const usize c = static_cast<usize>(r - q0);
        V a0(T(0));
        for (u32 t = cb; t < ce; ++t)
        {
            a0 = simd::fma(V(val[t]), V::load_partial(fac + static_cast<u64>(lf[t]) * r + q0, c), a0);
        }
        simd::fma(a0, V::load_partial(prow + q0, c), V::load_partial(dst + q0, c)).store_partial(dst + q0, c);
    }
}

// Adds the subtree of node z at level l (1 <= l <= rank - 1) into dst: a leaf's val * leaf factor row, an inner node's
// children's sum (tmp[l], zeroed) times its level's factor row. The recursion is shallower than kSparseMaxRank.
template <typename T> void mttkrp_node(const MttkrpCsfWalk<T>& w, T* const* tmp, u32 l, u64 z, T* dst) noexcept
{
    const T* row = w.fac[l] + static_cast<u64>(w.fids[l][z]) * w.r;
    if (l == w.rank - 1U)
    {
        axpy_row(dst, row, w.val[z], w.r);
        return;
    }
    const u32 cb = w.fptr[l][z];
    const u32 ce = w.fptr[l][z + 1U];
    if (l + 1U == w.rank - 1U)
    {
        leaf_fiber(w, cb, ce, row, dst);
        return;
    }
    T* acc = tmp[l];
    std::memset(acc, 0, static_cast<usize>(w.r) * sizeof(T));
    for (u32 t = cb; t < ce; ++t)
    {
        mttkrp_node(w, tmp, l + 1U, t, acc);
    }
    hadamard_acc(dst, acc, row, w.r);
}

} // namespace

template <typename T> void SparseKernel<T>::ttm_scatter(const SparseTtmWalk<T>& w, u64 eb, u64 ee, T* dst) noexcept
{
    const u64 f = w.f;
    const u64 osm = w.ostride[w.mode];
    for (u64 e = eb; e < ee; ++e)
    {
        T* o = dst + offset_of(w, e, 0U, w.rank, w.mode);
        const T v = w.val[e];
        const T* urow = w.u + static_cast<u64>(w.planes[w.mode][e]) * f;
        if (w.mode == w.rank - 1U)
        {
            axpy_row(o, urow, v, f);
        }
        else
        {
            for (u64 q = 0; q < f; ++q)
            {
                o[q * osm] = libm::fma(v, urow[q], o[q * osm]);
            }
        }
    }
}

template <typename T>
void SparseKernel<T>::ttm_staged(const SparseTtmWalk<T>& w, u64 eb, u64 ee, T* tile, T* out) noexcept
{
    const u32 mode = w.mode;
    const u64 f = w.f;
    const u64 suffix = w.ostride[mode]; // the product of the trailing out dims
    u64 e = eb;
    while (e < ee)
    {
        u64 re = e + 1U; // the end of the run of equal modes 0 .. mode - 1
        while (re < ee)
        {
            bool same = true;
            for (u32 m = 0; m < mode; ++m)
            {
                if (w.planes[m][re] != w.planes[m][e])
                {
                    same = false;
                    break;
                }
            }
            if (!same)
            {
                break;
            }
            ++re;
        }
        std::memset(tile, 0, static_cast<usize>(suffix * f) * sizeof(T));
        for (u64 e2 = e; e2 < re; ++e2)
        {
            const u64 soff = offset_of(w, e2, mode + 1U, w.rank, mode);
            axpy_row(tile + soff * f, w.u + static_cast<u64>(w.planes[mode][e2]) * f, w.val[e2], f);
        }
        T* oslice = out + offset_of(w, e, 0U, mode, mode); // blocked transpose tile[s][q] -> out[q][s]
        constexpr u64 bs = 128U;
        for (u64 s0 = 0; s0 < suffix; s0 += bs)
        {
            const u64 s1 = suffix - s0 < bs ? suffix : s0 + bs;
            for (u64 q = 0; q < f; ++q)
            {
                T* orow = oslice + q * suffix;
                for (u64 s = s0; s < s1; ++s)
                {
                    orow[s] = tile[s * f + q];
                }
            }
        }
        e = re;
    }
}

template <typename T> void SparseKernel<T>::fold(T* out, const T* parts, u64 osize, u32 nparts) noexcept
{
    // Blocks of out stay in L1 while every part adds into them, in part order: each element's sum is unchanged.
    constexpr u64 block = 2048U;
    for (u64 i0 = 0; i0 < osize; i0 += block)
    {
        const u64 len = osize - i0 < block ? osize - i0 : block;
        for (u32 t = 0; t < nparts; ++t)
        {
            add_acc(out + i0, parts + static_cast<u64>(t) * osize + i0, len);
        }
    }
}

template <typename T> void SparseKernel<T>::mttkrp_csf(const MttkrpCsfWalk<T>& w, u64 rb, u64 re, T* tmp) noexcept
{
    T* acc[kSparseMaxRank]{};
    for (u32 l = 1; l + 1U < w.rank; ++l)
    {
        acc[l] = tmp + static_cast<u64>(l - 1U) * w.r;
    }
    for (u64 z0 = rb; z0 < re; ++z0)
    {
        T* dst = w.out + static_cast<u64>(w.fids[0][z0]) * w.r;
        const u32 cb = w.fptr[0][z0];
        const u32 ce = w.fptr[0][z0 + 1U];
        for (u32 t = cb; t < ce; ++t)
        {
            mttkrp_node(w, acc, 1U, t, dst);
        }
    }
}

template <typename T> void SparseKernel<T>::mttkrp_coo(const MttkrpCooWalk<T>& w, T* tmp) noexcept
{
    // The CSF walk's operations, found from the canonical order: a level opens when its key (or a shallower one)
    // changes, and a finished fiber's sum (tmp[l]) multiplies its level's factor row into its parent, deepest first.
    const u32 rank = w.rank;
    const u64 r = w.r;
    T* acc[kSparseMaxRank]{};
    for (u32 l = 1; l + 1U < rank; ++l)
    {
        acc[l] = tmp + static_cast<u64>(l - 1U) * r;
    }
    T* dst = nullptr;
    for (u64 e = 0; e < w.n; ++e)
    {
        u32 d = 0; // the first level whose key differs from the previous nonzero's (rank: none, a duplicate tuple)
        if (e > 0U)
        {
            d = rank;
            for (u32 m = 0; m < rank; ++m)
            {
                if (w.planes[m][e] != w.planes[m][e - 1U])
                {
                    d = m;
                    break;
                }
            }
            if (rank >= 3U)
            {
                const u32 lo = d > 1U ? d : 1U;
                for (u32 l = rank - 2U; l + 1U > lo; --l)
                {
                    T* parent = l == 1U ? dst : acc[l - 1U];
                    hadamard_acc(parent, acc[l], w.fac[l] + static_cast<u64>(w.planes[l][e - 1U]) * r, r);
                }
            }
        }
        for (u32 l = d; l + 1U < rank; ++l)
        {
            if (l == 0U)
            {
                dst = w.out + static_cast<u64>(w.planes[0][e]) * r;
            }
            else
            {
                std::memset(acc[l], 0, static_cast<usize>(r) * sizeof(T));
            }
        }
        T* leaf = rank >= 3U ? acc[rank - 2U] : dst;
        axpy_row(leaf, w.fac[rank - 1U] + static_cast<u64>(w.planes[rank - 1U][e]) * r, w.val[e], r);
    }
    if (rank >= 3U)
    {
        for (u32 l = rank - 2U; l >= 1U; --l)
        {
            T* parent = l == 1U ? dst : acc[l - 1U];
            hadamard_acc(parent, acc[l], w.fac[l] + static_cast<u64>(w.planes[l][w.n - 1U]) * r, r);
        }
    }
}

template struct SparseKernel<f32>;
template struct SparseKernel<f64>;

} // namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
