#pragma once

#include "tensor.hpp"

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/math/simd/simd.hpp>

#include <bit>

// ---------------------------------------------------------------------------
// crd-hesap-tensor elementwise — the broadcasting elementwise engine
// (Phase 3.1.6 v14-b; ADR-0096).
//
// Full NumPy broadcasting rules (right-aligned; size-1 dims stretch with
// stride 0) over the rank<=8 stride-view substrate. Every op is a single IEEE
// operation per element ⇒ results are BIT-EXACT vs NumPy by construction; the
// gate corpus (scripts/v14b_elementwise_corpus.py) proves the ITERATION
// (broadcast/stride mapping) bit-exact.
//
// Engine shape (the crush levers):
//   P0: every operand contiguous-canonical → one flat SIMD loop (Vec8f/Vec4d).
//   P1: unit-or-broadcast inner stride     → SIMD per inner run (stride-0
//       operands splat once per run — this is where NumPy's generic iterator
//       pays overhead and Cerid does not).
//   P2: generic strides                    → scalar per element.
// The outer dims iterate through a plain counter; adjacent dims that are
// jointly contiguous across ALL operands are collapsed first, so P0 catches
// e.g. same-shape sliced blocks.
//
// Semantics notes (pinned):
//   - Neg/Abs are sign-BIT ops (XOR/AND) — bit-exact vs np.negative/np.abs
//     including ±0 and NaN payloads (a `0 - x` negation would give +0 for
//     x == +0). The scalar loops auto-vectorize to xorps/andps.
//   - Min/Max use the IEEE hardware semantics (x86 minps/maxps: the SECOND
//     operand wins on a NaN) — matched to std::min/max-style usage; the
//     NumPy NaN-propagating minimum/maximum and fmin/fmax variants land when
//     a consumer needs the distinction (documented, not silently different:
//     the corpus gates finite values only for Min/Max).
//   - Compare writes u8 0/1 masks; Where(mask, a, b) selects per element.
//   - dst must be contiguous canonical row-major with exactly the broadcast
//     shape (allocate via Tensor<T>::resize) — status, never UB, otherwise.
//
// v13 pillars: zero heap per call, noexcept, status-not-exception. Serial +
// SIMD (deterministic trivially — per-element pure); the parallel path arrives
// with v14-c's Tier-D grain contract and reuses this engine's runs.
// ---------------------------------------------------------------------------

namespace crd::hesap::tensor
{

enum class BinaryOp : crd::u8
{
    Add,
    Sub,
    Mul,
    Div,
    Min,
    Max,
};

enum class UnaryOp : crd::u8
{
    Neg,
    Abs,
};

enum class CompareOp : crd::u8
{
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
};

// NumPy broadcast of two shapes (right-aligned; 1-dims stretch).
// out must have room for kMaxRank entries; out_rank = max(rank_a, rank_b).
[[nodiscard]] inline TensorStatus broadcast_shapes(crd::containers::ConstSpan<crd::u64> a,
                                                   crd::containers::ConstSpan<crd::u64> b,
                                                   crd::containers::Span<crd::u64> out, crd::u32& out_rank) noexcept
{
    if (a.size() > kMaxRank || b.size() > kMaxRank)
    {
        return TensorStatus::RankOverflow;
    }
    const crd::u32 ra = static_cast<crd::u32>(a.size());
    const crd::u32 rb = static_cast<crd::u32>(b.size());
    const crd::u32 r = ra > rb ? ra : rb;
    CRD_ASSERT_MSG(out.size() >= r, "broadcast_shapes: out too small");
    for (crd::u32 d = 0; d < r; ++d)
    {
        const crd::u64 da = d < r - ra ? 1U : a[d - (r - ra)];
        const crd::u64 db = d < r - rb ? 1U : b[d - (r - rb)];
        if (da != db && da != 1U && db != 1U)
        {
            return TensorStatus::ShapeMismatch;
        }
        out[d] = da > db ? da : db;
    }
    out_rank = r;
    return TensorStatus::Ok;
}

namespace detail
{

// ---- SIMD trait: the vector type + width per compute dtype ----------------
template <typename T> struct EwSimd;
template <> struct EwSimd<crd::f32>
{
    using Vec = crd::math::simd::Vec8f;
    static constexpr crd::u64 kWidth = 8U;
};
template <> struct EwSimd<crd::f64>
{
    using Vec = crd::math::simd::Vec4d;
    static constexpr crd::u64 kWidth = 4U;
};

// ---- op functors: scalar + vector lanes ------------------------------------
template <typename T> struct OpAdd
{
    using V = typename EwSimd<T>::Vec;
    static T s(T a, T b) noexcept { return a + b; }
    static V v(V a, V b) noexcept { return a + b; }
};
template <typename T> struct OpSub
{
    using V = typename EwSimd<T>::Vec;
    static T s(T a, T b) noexcept { return a - b; }
    static V v(V a, V b) noexcept { return a - b; }
};
template <typename T> struct OpMul
{
    using V = typename EwSimd<T>::Vec;
    static T s(T a, T b) noexcept { return a * b; }
    static V v(V a, V b) noexcept { return a * b; }
};
template <typename T> struct OpDiv
{
    using V = typename EwSimd<T>::Vec;
    static T s(T a, T b) noexcept { return a / b; }
    static V v(V a, V b) noexcept { return a / b; }
};
template <typename T> struct OpMin
{
    using V = typename EwSimd<T>::Vec;
    static T s(T a, T b) noexcept { return b < a ? b : a; } // IEEE minps: second wins on NaN
    static V v(V a, V b) noexcept
    {
        using crd::math::simd::min;
        return min(a, b);
    }
};
template <typename T> struct OpMax
{
    using V = typename EwSimd<T>::Vec;
    static T s(T a, T b) noexcept { return a < b ? b : a; }
    static V v(V a, V b) noexcept
    {
        using crd::math::simd::max;
        return max(a, b);
    }
};

// Sign-bit unaries (bit-exact vs np.negative/np.abs incl. ±0 / NaN payloads);
// plain scalar loops — the compiler lowers them to xorps/andps.
template <typename T> struct SignBits;
template <> struct SignBits<crd::f32>
{
    using Bits = crd::u32;
    static constexpr crd::u32 kSign = 0x80000000U;
};
template <> struct SignBits<crd::f64>
{
    using Bits = crd::u64;
    static constexpr crd::u64 kSign = 0x8000000000000000ULL;
};

template <typename T> [[nodiscard]] inline T neg_bits(T x) noexcept
{
    using B = typename SignBits<T>::Bits;
    return std::bit_cast<T>(static_cast<B>(std::bit_cast<B>(x) ^ SignBits<T>::kSign));
}
template <typename T> [[nodiscard]] inline T abs_bits(T x) noexcept
{
    using B = typename SignBits<T>::Bits;
    return std::bit_cast<T>(static_cast<B>(std::bit_cast<B>(x) & ~SignBits<T>::kSign));
}

// ---- broadcast-prepared operand: view metadata against the dst shape ------
template <typename T> struct Prepared
{
    const T* data;
    crd::i64 stride[kMaxRank]; // vs the dst shape; 0 where broadcast
};

// Right-align `v` against the dst shape and materialize per-dim strides
// (0 for stretched dims). Returns ShapeMismatch when not broadcastable.
template <typename T>
[[nodiscard]] inline TensorStatus prepare(const TensorView<const T>& v, crd::containers::ConstSpan<crd::u64> dshape,
                                          Prepared<T>& out) noexcept
{
    const crd::u32 dr = static_cast<crd::u32>(dshape.size());
    const crd::u32 vr = v.rank();
    if (vr > dr)
    {
        return TensorStatus::ShapeMismatch;
    }
    out.data = v.data();
    for (crd::u32 d = 0; d < dr; ++d)
    {
        if (d < dr - vr)
        {
            out.stride[d] = 0;
            continue;
        }
        const crd::u32 sd = d - (dr - vr);
        if (v.shape(sd) == dshape[d])
        {
            out.stride[d] = v.stride(sd);
        }
        else if (v.shape(sd) == 1U)
        {
            out.stride[d] = 0;
        }
        else
        {
            return TensorStatus::ShapeMismatch;
        }
    }
    return TensorStatus::Ok;
}

// Collapse adjacent dims that are jointly contiguous across dst + all
// operands (stride[d] == stride[d+1] * shape[d+1] for every stream, with
// broadcast-0 collapsing only with broadcast-0). Shrinks the outer loop and
// widens the inner SIMD runs; same-shape dense operands collapse to rank 1.
struct Collapsed
{
    crd::u64 shape[kMaxRank];
    crd::u32 rank;
};

template <crd::u32 N, typename T>
inline void collapse(crd::containers::ConstSpan<crd::u64> dshape, Prepared<T> (&ops)[N], Collapsed& c) noexcept
{
    const crd::u32 r = static_cast<crd::u32>(dshape.size());
    if (r == 0U)
    {
        c.rank = 0U;
        return;
    }
    // Working copy (front-to-back), fusing dim d into d+1 where possible.
    crd::u64 shp[kMaxRank];
    crd::i64 st[N][kMaxRank];
    for (crd::u32 d = 0; d < r; ++d)
    {
        shp[d] = dshape[d];
        for (crd::u32 o = 0; o < N; ++o)
        {
            st[o][d] = ops[o].stride[d];
        }
    }
    crd::u32 w = 0; // write cursor over the collapsed dims
    for (crd::u32 d = 1; d < r; ++d)
    {
        bool fuse = true;
        for (crd::u32 o = 0; o < N; ++o)
        {
            const bool both_bcast = st[o][w] == 0 && st[o][d] == 0;
            const bool contig = st[o][w] == st[o][d] * static_cast<crd::i64>(shp[d]);
            if (!(both_bcast || contig))
            {
                fuse = false;
                break;
            }
        }
        if (fuse)
        {
            shp[w] *= shp[d];
            for (crd::u32 o = 0; o < N; ++o)
            {
                st[o][w] = st[o][d];
            }
        }
        else
        {
            ++w;
            shp[w] = shp[d];
            for (crd::u32 o = 0; o < N; ++o)
            {
                st[o][w] = st[o][d];
            }
        }
    }
    c.rank = w + 1U;
    for (crd::u32 d = 0; d <= w; ++d)
    {
        c.shape[d] = shp[d];
        for (crd::u32 o = 0; o < N; ++o)
        {
            ops[o].stride[d] = st[o][d];
        }
    }
}

// Validate dst: contiguous canonical with exactly the broadcast shape.
template <typename T>
[[nodiscard]] inline TensorStatus check_dst(const TensorView<T>& dst, crd::containers::ConstSpan<crd::u64> bshape,
                                            crd::u32 brank) noexcept
{
    if (dst.rank() != brank)
    {
        return TensorStatus::ShapeMismatch;
    }
    for (crd::u32 d = 0; d < brank; ++d)
    {
        if (dst.shape(d) != bshape[d])
        {
            return TensorStatus::ShapeMismatch;
        }
    }
    if (!dst.is_contiguous())
    {
        return TensorStatus::NotContiguous;
    }
    return TensorStatus::Ok;
}

// ---- the binary engine ------------------------------------------------------
template <typename T, typename Op, typename D, typename Store>
inline void run_binary(const Collapsed& c, const Prepared<T>& a, const Prepared<T>& b, D* dst, Store store) noexcept
{
    using V = typename EwSimd<T>::Vec;
    constexpr crd::u64 kW = EwSimd<T>::kWidth;
    constexpr bool kVecStore = std::is_same_v<D, T>; // compare stores u8 → scalar inner

    if (c.rank == 0U) // rank-0 scalar
    {
        store(dst, 0, Op::s(a.data[0], b.data[0]));
        return;
    }
    const crd::u32 last = c.rank - 1U;
    const crd::u64 inner = c.shape[last];
    const crd::i64 sa = a.stride[last];
    const crd::i64 sb = b.stride[last];

    crd::u64 idx[kMaxRank] = {};
    crd::u64 outer = 1;
    for (crd::u32 d = 0; d < last; ++d)
    {
        outer *= c.shape[d];
    }
    const T* pa = a.data;
    const T* pb = b.data;
    crd::u64 w = 0;
    for (crd::u64 o = 0; o < outer; ++o)
    {
        // inner run
        if constexpr (kVecStore)
        {
            if (sa == 1 && sb == 1)
            {
                crd::u64 i = 0;
                for (; i + kW <= inner; i += kW, w += kW)
                {
                    Op::v(V::load(pa + i), V::load(pb + i)).store(dst + w);
                }
                for (; i < inner; ++i, ++w)
                {
                    dst[w] = Op::s(pa[i], pb[i]);
                }
            }
            else if (sa == 1 && sb == 0)
            {
                const V vb(pb[0]);
                crd::u64 i = 0;
                for (; i + kW <= inner; i += kW, w += kW)
                {
                    Op::v(V::load(pa + i), vb).store(dst + w);
                }
                for (; i < inner; ++i, ++w)
                {
                    dst[w] = Op::s(pa[i], pb[0]);
                }
            }
            else if (sa == 0 && sb == 1)
            {
                const V va(pa[0]);
                crd::u64 i = 0;
                for (; i + kW <= inner; i += kW, w += kW)
                {
                    Op::v(va, V::load(pb + i)).store(dst + w);
                }
                for (; i < inner; ++i, ++w)
                {
                    dst[w] = Op::s(pa[0], pb[i]);
                }
            }
            else
            {
                for (crd::u64 i = 0; i < inner; ++i, ++w)
                {
                    dst[w] = Op::s(pa[static_cast<crd::i64>(i) * sa], pb[static_cast<crd::i64>(i) * sb]);
                }
            }
        }
        else
        {
            for (crd::u64 i = 0; i < inner; ++i, ++w)
            {
                store(dst, w, Op::s(pa[static_cast<crd::i64>(i) * sa], pb[static_cast<crd::i64>(i) * sb]));
            }
        }
        // outer counter advance (row-major)
        for (crd::u32 d = last; d-- > 0U;)
        {
            pa += a.stride[d];
            pb += b.stride[d];
            if (++idx[d] < c.shape[d])
            {
                break;
            }
            // rewind this dim
            pa -= a.stride[d] * static_cast<crd::i64>(c.shape[d]);
            pb -= b.stride[d] * static_cast<crd::i64>(c.shape[d]);
            idx[d] = 0;
        }
    }
}

template <typename T, typename Op>
[[nodiscard]] inline TensorStatus ew_binary_impl(const TensorView<const T>& a, const TensorView<const T>& b,
                                                 const TensorView<T>& dst) noexcept
{
    crd::u64 bshape[kMaxRank];
    crd::u32 brank = 0;
    TensorStatus st = broadcast_shapes(a.shape(), b.shape(), {bshape, kMaxRank}, brank);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    st = check_dst(dst, {bshape, brank}, brank);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    Prepared<T> ops[2];
    st = prepare(a, {bshape, brank}, ops[0]);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    st = prepare(b, {bshape, brank}, ops[1]);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    Collapsed c;
    collapse<2>({bshape, brank}, ops, c);
    run_binary<T, Op>(c, ops[0], ops[1], dst.data(), [](T* d, crd::u64 i, T v) noexcept { d[i] = v; });
    return TensorStatus::Ok;
}

} // namespace detail

// ============================================================================
// Public entry points (runtime op enum → zero-overhead templated kernels)
// ============================================================================

template <typename T>
[[nodiscard]] inline TensorStatus ew_binary(BinaryOp op, const TensorView<const T>& a, const TensorView<const T>& b,
                                            const TensorView<T>& dst) noexcept
{
    switch (op)
    {
        case BinaryOp::Add:
            return detail::ew_binary_impl<T, detail::OpAdd<T>>(a, b, dst);
        case BinaryOp::Sub:
            return detail::ew_binary_impl<T, detail::OpSub<T>>(a, b, dst);
        case BinaryOp::Mul:
            return detail::ew_binary_impl<T, detail::OpMul<T>>(a, b, dst);
        case BinaryOp::Div:
            return detail::ew_binary_impl<T, detail::OpDiv<T>>(a, b, dst);
        case BinaryOp::Min:
            return detail::ew_binary_impl<T, detail::OpMin<T>>(a, b, dst);
        case BinaryOp::Max:
            return detail::ew_binary_impl<T, detail::OpMax<T>>(a, b, dst);
    }
    return TensorStatus::BadInput;
}

// Unary (sign-bit exact): dst shape must equal a's shape (contiguous canonical).
template <typename T>
[[nodiscard]] inline TensorStatus ew_unary(UnaryOp op, const TensorView<const T>& a, const TensorView<T>& dst) noexcept
{
    TensorStatus st = detail::check_dst(dst, a.shape(), a.rank());
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    T* out = dst.data();
    crd::u64 k = 0;
    switch (op)
    {
        case UnaryOp::Neg:
            a.for_each([&](const crd::u64*, const T& v) { out[k++] = detail::neg_bits(v); });
            return TensorStatus::Ok;
        case UnaryOp::Abs:
            a.for_each([&](const crd::u64*, const T& v) { out[k++] = detail::abs_bits(v); });
            return TensorStatus::Ok;
    }
    return TensorStatus::BadInput;
}

// Compare: u8 0/1 mask with the broadcast shape.
template <typename T>
[[nodiscard]] inline TensorStatus ew_compare(CompareOp op, const TensorView<const T>& a, const TensorView<const T>& b,
                                             const TensorView<crd::u8>& dst) noexcept
{
    crd::u64 bshape[kMaxRank];
    crd::u32 brank = 0;
    TensorStatus st = broadcast_shapes(a.shape(), b.shape(), {bshape, kMaxRank}, brank);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    st = detail::check_dst(dst, {bshape, brank}, brank);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    detail::Prepared<T> ops[2];
    st = detail::prepare(a, {bshape, brank}, ops[0]);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    st = detail::prepare(b, {bshape, brank}, ops[1]);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    detail::Collapsed c;
    detail::collapse<2>({bshape, brank}, ops, c);

    // One generic strided walker with the comparator inlined per case.
    const auto run = [&](auto cmp) noexcept
    {
        crd::u8* out = dst.data();
        crd::u64 idx[kMaxRank] = {};
        const crd::u32 last = c.rank == 0U ? 0U : c.rank - 1U;
        const crd::u64 inner = c.rank == 0U ? 1U : c.shape[last];
        const crd::i64 sa = c.rank == 0U ? 0 : ops[0].stride[last];
        const crd::i64 sb = c.rank == 0U ? 0 : ops[1].stride[last];
        crd::u64 outer = 1;
        for (crd::u32 d = 0; d < last; ++d)
        {
            outer *= c.shape[d];
        }
        const T* pa = ops[0].data;
        const T* pb = ops[1].data;
        crd::u64 w = 0;
        for (crd::u64 o = 0; o < outer; ++o)
        {
            for (crd::u64 i = 0; i < inner; ++i, ++w)
            {
                out[w] = cmp(pa[static_cast<crd::i64>(i) * sa], pb[static_cast<crd::i64>(i) * sb]) ? 1U : 0U;
            }
            for (crd::u32 d = last; d-- > 0U;)
            {
                pa += ops[0].stride[d];
                pb += ops[1].stride[d];
                if (++idx[d] < c.shape[d])
                {
                    break;
                }
                pa -= ops[0].stride[d] * static_cast<crd::i64>(c.shape[d]);
                pb -= ops[1].stride[d] * static_cast<crd::i64>(c.shape[d]);
                idx[d] = 0;
            }
        }
    };
    switch (op)
    {
        case CompareOp::Eq:
            run([](T x, T y) noexcept { return x == y; });
            return TensorStatus::Ok;
        case CompareOp::Ne:
            run([](T x, T y) noexcept { return x != y; });
            return TensorStatus::Ok;
        case CompareOp::Lt:
            run([](T x, T y) noexcept { return x < y; });
            return TensorStatus::Ok;
        case CompareOp::Le:
            run([](T x, T y) noexcept { return x <= y; });
            return TensorStatus::Ok;
        case CompareOp::Gt:
            run([](T x, T y) noexcept { return x > y; });
            return TensorStatus::Ok;
        case CompareOp::Ge:
            run([](T x, T y) noexcept { return x >= y; });
            return TensorStatus::Ok;
    }
    return TensorStatus::BadInput;
}

// where(mask, a, b): NumPy select with full three-way broadcasting.
template <typename T>
[[nodiscard]] inline TensorStatus ew_where(const TensorView<const crd::u8>& mask, const TensorView<const T>& a,
                                           const TensorView<const T>& b, const TensorView<T>& dst) noexcept
{
    crd::u64 ab[kMaxRank];
    crd::u32 abr = 0;
    TensorStatus st = broadcast_shapes(a.shape(), b.shape(), {ab, kMaxRank}, abr);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    crd::u64 bshape[kMaxRank];
    crd::u32 brank = 0;
    st = broadcast_shapes({ab, abr}, mask.shape(), {bshape, kMaxRank}, brank);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    st = detail::check_dst(dst, {bshape, brank}, brank);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    detail::Prepared<T> opa;
    detail::Prepared<T> opb;
    detail::Prepared<crd::u8> opm;
    st = detail::prepare(a, {bshape, brank}, opa);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    st = detail::prepare(b, {bshape, brank}, opb);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    st = detail::prepare(mask, {bshape, brank}, opm);
    if (st != TensorStatus::Ok)
    {
        return st;
    }

    T* out = dst.data();
    crd::u64 idx[kMaxRank] = {};
    const crd::u32 last = brank == 0U ? 0U : brank - 1U;
    const crd::u64 inner = brank == 0U ? 1U : bshape[last];
    crd::u64 outer = 1;
    for (crd::u32 d = 0; d < last; ++d)
    {
        outer *= bshape[d];
    }
    const T* pa = opa.data;
    const T* pb = opb.data;
    const crd::u8* pm = opm.data;
    const crd::i64 sa = brank == 0U ? 0 : opa.stride[last];
    const crd::i64 sb = brank == 0U ? 0 : opb.stride[last];
    const crd::i64 sm = brank == 0U ? 0 : opm.stride[last];
    crd::u64 w = 0;
    for (crd::u64 o = 0; o < outer; ++o)
    {
        for (crd::u64 i = 0; i < inner; ++i, ++w)
        {
            const crd::i64 ii = static_cast<crd::i64>(i);
            out[w] = pm[ii * sm] != 0U ? pa[ii * sa] : pb[ii * sb];
        }
        for (crd::u32 d = last; d-- > 0U;)
        {
            pa += opa.stride[d];
            pb += opb.stride[d];
            pm += opm.stride[d];
            if (++idx[d] < bshape[d])
            {
                break;
            }
            pa -= opa.stride[d] * static_cast<crd::i64>(bshape[d]);
            pb -= opb.stride[d] * static_cast<crd::i64>(bshape[d]);
            pm -= opm.stride[d] * static_cast<crd::i64>(bshape[d]);
            idx[d] = 0;
        }
    }
    return TensorStatus::Ok;
}

// Exact widening / RNE-narrowing dtype casts (f32 ↔ f64), strided source.
[[nodiscard]] inline TensorStatus ew_cast(const TensorView<const crd::f32>& a, const TensorView<crd::f64>& dst) noexcept
{
    const TensorStatus st = detail::check_dst(dst, a.shape(), a.rank());
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    crd::f64* out = dst.data();
    crd::u64 k = 0;
    a.for_each([&](const crd::u64*, const crd::f32& v) { out[k++] = static_cast<crd::f64>(v); });
    return TensorStatus::Ok;
}
[[nodiscard]] inline TensorStatus ew_cast(const TensorView<const crd::f64>& a, const TensorView<crd::f32>& dst) noexcept
{
    const TensorStatus st = detail::check_dst(dst, a.shape(), a.rank());
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    crd::f32* out = dst.data();
    crd::u64 k = 0;
    a.for_each([&](const crd::u64*, const crd::f64& v) { out[k++] = static_cast<crd::f32>(v); });
    return TensorStatus::Ok;
}

} // namespace crd::hesap::tensor
