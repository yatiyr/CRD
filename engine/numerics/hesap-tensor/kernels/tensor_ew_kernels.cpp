// The elementwise, Tier D and einsum kernels of crd/hesap/tensor/tensor_kernels.hpp, the second unit of the tensor
// family (tensor_kernels.cpp holds the table), compiled once per CPU level by crd_add_cpu_kernels (../CMakeLists.txt,
// ADR-0134 §5). The bodies step lane groups of kEwLanes values (Vec8f, Vec4d; scalar arrays on portable), two groups
// at once in one Vec16f or Vec8d at avx512, and give the same bits on every level: an elementwise op and an einsum
// product are the same IEEE operations per element in the same order whatever the register, and Tier D's block partial
// keeps its four-group shape (at avx512 one register holds two neighbouring groups, split before they combine).

#include "tensor_ew_kernels.hpp"

#include <crd/core/platform.hpp>
#include <crd/math/simd/simd.hpp>
#include <crd/math/simd/vec16f.hpp>
#include <crd/math/simd/vec8d.hpp>

#include <type_traits>

namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
{
namespace
{

namespace simd = crd::math::simd;

using crd::f32;
using crd::f64;
using crd::i64;
using crd::u32;
using crd::u64;

// One lane group, and the register a step takes: two groups at avx512, else one.
template <typename T> using Group = std::conditional_t<std::is_same_v<T, f64>, simd::Vec4d, simd::Vec8f>;
#if CRD_SIMD_HAS_AVX512
template <typename T> using Wide = std::conditional_t<std::is_same_v<T, f64>, simd::Vec8d, simd::Vec16f>;
#else
template <typename T> using Wide = Group<T>;
#endif
template <typename T, typename V> inline constexpr u64 kLanes = sizeof(V) / sizeof(T);
static_assert(kLanes<f32, Group<f32>> == kEwLanes<f32> && kLanes<f64, Group<f64>> == kEwLanes<f64>);

// a op b on a scalar or a vector; Min and Max are minps and maxps (the second operand on a NaN or a signed-zero tie),
// which the vector types implement on every backend.
template <BinaryOp Op, typename X> CRD_FORCEINLINE X apply(X a, X b) noexcept
{
    if constexpr (Op == BinaryOp::Add)
    {
        return a + b;
    }
    else if constexpr (Op == BinaryOp::Sub)
    {
        return a - b;
    }
    else if constexpr (Op == BinaryOp::Mul)
    {
        return a * b;
    }
    else if constexpr (Op == BinaryOp::Div)
    {
        return a / b;
    }
    else if constexpr (Op == BinaryOp::Min)
    {
        if constexpr (std::is_floating_point_v<X>)
        {
            return a < b ? a : b;
        }
        else
        {
            return simd::min(a, b);
        }
    }
    else
    {
        if constexpr (std::is_floating_point_v<X>)
        {
            return a > b ? a : b;
        }
        else
        {
            return simd::max(a, b);
        }
    }
}

// The binary op a reduction folds with.
constexpr BinaryOp fold_of(ReduceOp op) noexcept
{
    switch (op)
    {
        case ReduceOp::Sum:
            return BinaryOp::Add;
        case ReduceOp::Prod:
            return BinaryOp::Mul;
        case ReduceOp::Min:
            return BinaryOp::Min;
        default:
            return BinaryOp::Max;
    }
}
template <ReduceOp Op> inline constexpr BinaryOp kFold = fold_of(Op);

// d[i] = a op b for i in [i, n) in steps of V, a and b read at i where they step and at 0 where they broadcast.
// Returns where the steps stop.
template <typename T, BinaryOp Op, bool AStep, bool BStep, typename V>
CRD_FORCEINLINE u64 binary_steps(T* d, const T* a, const T* b, u64 i, u64 n) noexcept
{
    constexpr u64 lanes = kLanes<T, V>;
    const V sa(a[0]);
    const V sb(b[0]);
    for (; i + lanes <= n; i += lanes)
    {
        const V x = AStep ? V::load(a + i) : sa;
        const V y = BStep ? V::load(b + i) : sb;
        apply<Op>(x, y).store(d + i);
    }
    return i;
}

// One inner run of n > 0 elements in a vector form: Wide steps, an odd last group at avx512, the scalar tail.
template <typename T, BinaryOp Op, bool AStep, bool BStep>
CRD_FORCEINLINE void binary_run(T* d, const T* a, const T* b, u64 n) noexcept
{
    u64 i = binary_steps<T, Op, AStep, BStep, Wide<T>>(d, a, b, 0U, n);
    if constexpr (!std::is_same_v<Wide<T>, Group<T>>)
    {
        i = binary_steps<T, Op, AStep, BStep, Group<T>>(d, a, b, i, n);
    }
    for (; i < n; ++i)
    {
        d[i] = apply<Op>(AStep ? a[i] : a[0], BStep ? b[i] : b[0]);
    }
}

// out[j] = out[j] op row[j] for j in [j0, j1).
template <typename T, BinaryOp Op, typename V>
CRD_FORCEINLINE u64 fold_steps(T* out, const T* row, u64 j, u64 j1) noexcept
{
    constexpr u64 lanes = kLanes<T, V>;
    for (; j + lanes <= j1; j += lanes)
    {
        apply<Op>(V::load(out + j), V::load(row + j)).store(out + j);
    }
    return j;
}

template <typename T, BinaryOp Op> CRD_FORCEINLINE void fold_row(T* out, const T* row, u64 j0, u64 j1) noexcept
{
    u64 j = fold_steps<T, Op, Wide<T>>(out, row, j0, j1);
    if constexpr (!std::is_same_v<Wide<T>, Group<T>>)
    {
        j = fold_steps<T, Op, Group<T>>(out, row, j, j1);
    }
    for (; j < j1; ++j)
    {
        out[j] = apply<Op>(out[j], row[j]);
    }
}

// The four striped accumulators of the block partial over [0, n - n % (4 kEwLanes)), combined
// ((a0 op a1) op (a2 op a3)). At avx512 one register holds a0 and a1, another a2 and a3, as they lie in memory.
template <typename T, BinaryOp Op> CRD_FORCEINLINE Group<T> stripe_groups(const T* p, u64 n, u64& i) noexcept
{
    using G = Group<T>;
    using W = Wide<T>;
    constexpr u64 group_lanes = kEwLanes<T>;
    constexpr u64 stripe = 4U * group_lanes;
    if constexpr (std::is_same_v<W, G>)
    {
        G a0 = G::load(p);
        G a1 = G::load(p + group_lanes);
        G a2 = G::load(p + 2U * group_lanes);
        G a3 = G::load(p + 3U * group_lanes);
        for (i = stripe; i + stripe <= n; i += stripe)
        {
            a0 = apply<Op>(a0, G::load(p + i));
            a1 = apply<Op>(a1, G::load(p + i + group_lanes));
            a2 = apply<Op>(a2, G::load(p + i + 2U * group_lanes));
            a3 = apply<Op>(a3, G::load(p + i + 3U * group_lanes));
        }
        return apply<Op>(apply<Op>(a0, a1), apply<Op>(a2, a3));
    }
    else
    {
        W a01 = W::load(p);
        W a23 = W::load(p + 2U * group_lanes);
        for (i = stripe; i + stripe <= n; i += stripe)
        {
            a01 = apply<Op>(a01, W::load(p + i));
            a23 = apply<Op>(a23, W::load(p + i + 2U * group_lanes));
        }
        return apply<Op>(apply<Op>(simd::low_half(a01), simd::high_half(a01)),
                         apply<Op>(simd::low_half(a23), simd::high_half(a23)));
    }
}

template <typename T, ReduceOp Op> CRD_FORCEINLINE T block_partial(const T* p, u64 n, T init) noexcept
{
    constexpr BinaryOp fold = kFold<Op>;
    constexpr u64 group_lanes = kEwLanes<T>;
    T acc = init;
    u64 i = 0;
    if (n >= 4U * group_lanes)
    {
        const Group<T> c = stripe_groups<T, fold>(p, n, i);
        alignas(64) T lanes[group_lanes];
        c.store(lanes);
        T lane_fold = lanes[0];
        for (u64 l = 1; l < group_lanes; ++l)
        {
            lane_fold = apply<fold>(lane_fold, lanes[l]);
        }
        acc = apply<fold>(acc, lane_fold);
    }
    for (; i < n; ++i)
    {
        acc = apply<fold>(acc, p[i]);
    }
    return acc;
}

// Columns [j, n) of einsum's thin product in pairs of V over 8-row blocks, then the leftover rows; returns where the
// pairs stop.
template <typename T, bool ATrans, typename V>
u64 smallm_pairs(u64 m_count, u64 k_count, u64 n_count, const T* a, const T* b, T* c, u64 j) noexcept
{
    constexpr u64 lanes = kLanes<T, V>;
    const auto at = [=](u64 m, u64 k) noexcept
    {
        return ATrans ? a[k * m_count + m] : a[m * k_count + k];
    };
    for (; j + 2U * lanes <= n_count; j += 2U * lanes)
    {
        const T* bj = b + j;
        u64 m0 = 0;
        for (; m0 + 8U <= m_count; m0 += 8U)
        {
            V acc0[8];
            V acc1[8];
            for (u32 r = 0; r < 8U; ++r)
            {
                acc0[r] = V(T{0});
                acc1[r] = V(T{0});
            }
            for (u64 k = 0; k < k_count; ++k)
            {
                const V b0 = V::load(bj + k * n_count);
                const V b1 = V::load(bj + k * n_count + lanes);
                for (u32 r = 0; r < 8U; ++r)
                {
                    const V av(at(m0 + r, k));
                    acc0[r] = acc0[r] + av * b0;
                    acc1[r] = acc1[r] + av * b1;
                }
            }
            for (u32 r = 0; r < 8U; ++r)
            {
                acc0[r].store(c + (m0 + r) * n_count + j);
                acc1[r].store(c + (m0 + r) * n_count + j + lanes);
            }
        }
        for (; m0 < m_count; ++m0)
        {
            V a0(T{0});
            V a1(T{0});
            for (u64 k = 0; k < k_count; ++k)
            {
                const V av(at(m0, k));
                a0 = a0 + av * V::load(bj + k * n_count);
                a1 = a1 + av * V::load(bj + k * n_count + lanes);
            }
            a0.store(c + m0 * n_count + j);
            a1.store(c + m0 * n_count + j + lanes);
        }
    }
    return j;
}

template <typename T, bool ATrans>
void smallm_product(u64 m_count, u64 k_count, u64 n_count, const T* a, const T* b, T* c) noexcept
{
    u64 done = smallm_pairs<T, ATrans, Wide<T>>(m_count, k_count, n_count, a, b, c, 0U);
    if constexpr (!std::is_same_v<Wide<T>, Group<T>>)
    {
        done = smallm_pairs<T, ATrans, Group<T>>(m_count, k_count, n_count, a, b, c, done);
    }
    for (u64 m = 0; m < m_count; ++m)
    {
        for (u64 n = done; n < n_count; ++n)
        {
            T acc{};
            for (u64 k = 0; k < k_count; ++k)
            {
                acc += (ATrans ? a[k * m_count + m] : a[m * k_count + k]) * b[k * n_count + n];
            }
            c[m * n_count + n] = acc;
        }
    }
}

} // namespace

template <typename T, BinaryOp Op> void EwBinaryKernel<T, Op>::walk(const EwBinaryWalk<T>& walk) noexcept
{
    if (walk.rank == 0U)
    {
        walk.dst[0] = apply<Op>(walk.a[0], walk.b[0]);
        return;
    }
    const u32 last = walk.rank - 1U;
    const u64 inner = walk.shape[last];
    u64 outer = 1;
    for (u32 d = 0; d < last; ++d)
    {
        outer *= walk.shape[d];
    }
    if (inner == 0U || outer == 0U)
    {
        return;
    }
    const i64 sa = walk.sa[last];
    const i64 sb = walk.sb[last];
    u64 idx[kPermuteMaxRank] = {};
    const T* pa = walk.a;
    const T* pb = walk.b;
    T* pd = walk.dst;
    for (u64 o = 0; o < outer; ++o, pd += inner)
    {
        if (sa == 1 && sb == 1)
        {
            binary_run<T, Op, true, true>(pd, pa, pb, inner);
        }
        else if (sa == 1 && sb == 0)
        {
            binary_run<T, Op, true, false>(pd, pa, pb, inner);
        }
        else if (sa == 0 && sb == 1)
        {
            binary_run<T, Op, false, true>(pd, pa, pb, inner);
        }
        else
        {
            for (u64 i = 0; i < inner; ++i)
            {
                pd[i] = apply<Op>(pa[static_cast<i64>(i) * sa], pb[static_cast<i64>(i) * sb]);
            }
        }
        // The outer counter, row-major.
        for (u32 d = last; d-- > 0U;)
        {
            pa += walk.sa[d];
            pb += walk.sb[d];
            if (++idx[d] < walk.shape[d])
            {
                break;
            }
            pa -= walk.sa[d] * static_cast<i64>(walk.shape[d]);
            pb -= walk.sb[d] * static_cast<i64>(walk.shape[d]);
            idx[d] = 0;
        }
    }
}

template <typename T, ReduceOp Op> T TierDKernel<T, Op>::block(const T* p, u64 n, T init) noexcept
{
    return block_partial<T, Op>(p, n, init);
}

template <typename T, ReduceOp Op> void TierDKernel<T, Op>::rows(T* out, const T* src, u64 len, u64 o0, u64 o1) noexcept
{
    for (u64 o = o0; o < o1; ++o)
    {
        const T* run = src + o * len;
        T init{};
        if constexpr (Op == ReduceOp::Prod)
        {
            init = T{1};
        }
        else if constexpr (Op == ReduceOp::Min || Op == ReduceOp::Max)
        {
            init = run[0];
        }
        T acc = init;
        for (u64 off = 0; off < len; off += detail::kReduceBlock)
        {
            const u64 n = len - off < detail::kReduceBlock ? len - off : detail::kReduceBlock;
            const T partial = block_partial<T, Op>(run + off, n, init);
            acc = off == 0U ? partial : apply<kFold<Op>>(acc, partial);
        }
        out[o] = acc;
    }
}

template <typename T, ReduceOp Op>
void TierDKernel<T, Op>::columns(T* out, const T* src, u64 rows, u64 w, u64 j0, u64 j1) noexcept
{
    // Column blocks of kReduceBlock stay in cache across the rows; every output folds its rows in order.
    for (u64 c0 = j0; c0 < j1; c0 += detail::kReduceBlock)
    {
        const u64 c1 = j1 - c0 < detail::kReduceBlock ? j1 : c0 + detail::kReduceBlock;
        for (u64 j = c0; j < c1; ++j)
        {
            out[j] = src[j];
        }
        for (u64 i = 1; i < rows; ++i)
        {
            fold_row<T, kFold<Op>>(out, src + i * w, c0, c1);
        }
    }
}

template <typename T>
void EinsumKernel<T>::thin(u64 m_count, u64 k_count, u64 n_count, const T* a, const T* b, T* c) noexcept
{
    using W = Wide<T>;
    using G = Group<T>;
    constexpr u64 wide_lanes = kLanes<T, W>;
    constexpr u64 group_lanes = kEwLanes<T>;
    const u64 nw = n_count / wide_lanes; // at most 8: n <= 8 * kEwLanes
    const u64 wide_end = nw * wide_lanes;
    const bool group = !std::is_same_v<W, G> && n_count - wide_end >= group_lanes; // an odd last group at avx512
    const u64 vec_end = group ? wide_end + group_lanes : wide_end;
    for (u64 m = 0; m < m_count; ++m)
    {
        const T* arow = a + m * k_count;
        T* crow = c + m * n_count;
        W acc[8];
        for (u64 j = 0; j < nw; ++j)
        {
            acc[j] = W(T{0});
        }
        G acc_group(T{0});
        for (u64 j = vec_end; j < n_count; ++j)
        {
            crow[j] = T{0};
        }
        for (u64 k = 0; k < k_count; ++k)
        {
            const T ak = arow[k];
            const T* brow = b + k * n_count;
            const W av(ak);
            for (u64 j = 0; j < nw; ++j)
            {
                acc[j] = acc[j] + av * W::load(brow + j * wide_lanes);
            }
            if (group)
            {
                acc_group = acc_group + G(ak) * G::load(brow + wide_end);
            }
            for (u64 j = vec_end; j < n_count; ++j)
            {
                crow[j] += ak * brow[j];
            }
        }
        for (u64 j = 0; j < nw; ++j)
        {
            acc[j].store(crow + j * wide_lanes);
        }
        if (group)
        {
            acc_group.store(crow + wide_end);
        }
    }
}

template <typename T>
void EinsumKernel<T>::smallm(u64 m_count, u64 k_count, u64 n_count, const T* a, const T* b, T* c, bool a_trans) noexcept
{
    if (a_trans)
    {
        smallm_product<T, true>(m_count, k_count, n_count, a, b, c);
    }
    else
    {
        smallm_product<T, false>(m_count, k_count, n_count, a, b, c);
    }
}

template struct EwBinaryKernel<f32, BinaryOp::Add>;
template struct EwBinaryKernel<f32, BinaryOp::Sub>;
template struct EwBinaryKernel<f32, BinaryOp::Mul>;
template struct EwBinaryKernel<f32, BinaryOp::Div>;
template struct EwBinaryKernel<f32, BinaryOp::Min>;
template struct EwBinaryKernel<f32, BinaryOp::Max>;
template struct EwBinaryKernel<f64, BinaryOp::Add>;
template struct EwBinaryKernel<f64, BinaryOp::Sub>;
template struct EwBinaryKernel<f64, BinaryOp::Mul>;
template struct EwBinaryKernel<f64, BinaryOp::Div>;
template struct EwBinaryKernel<f64, BinaryOp::Min>;
template struct EwBinaryKernel<f64, BinaryOp::Max>;
template struct TierDKernel<f32, ReduceOp::Sum>;
template struct TierDKernel<f32, ReduceOp::Prod>;
template struct TierDKernel<f32, ReduceOp::Min>;
template struct TierDKernel<f32, ReduceOp::Max>;
template struct TierDKernel<f64, ReduceOp::Sum>;
template struct TierDKernel<f64, ReduceOp::Prod>;
template struct TierDKernel<f64, ReduceOp::Min>;
template struct TierDKernel<f64, ReduceOp::Max>;
template struct EinsumKernel<f32>;
template struct EinsumKernel<f64>;

} // namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
