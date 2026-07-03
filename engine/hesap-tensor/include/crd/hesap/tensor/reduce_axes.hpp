#pragma once

#include "reduce.hpp"

// ---------------------------------------------------------------------------
// crd-hesap-tensor reduce_axes — general axis-set reductions (v14-c close).
//
// reduce_axes(op, v, axes_mask, dst): fold the axes named in axes_mask
// (bit d = reduce axis d), keeping the others in order — NumPy
// np.sum(v, axis=tuple) semantics with keepdims=False. dst must be canonical
// contiguous with exactly the kept shape (rank-0 = single element).
//
// Dispatch (the shape-adaptive engine; every path's order is a function of
// shape + contiguity class ONLY — the Tier-D contract):
//   VERTICAL — the mask is a leading prefix of a contiguous view: out[j]
//     accumulates x[i][j] in i-order, SIMD across j. Each output element's
//     chain is the plain serial i-order — bit-exact vs the scalar loop BY
//     CONSTRUCTION, independent of SIMD width, and the parallel split over
//     j-blocks touches disjoint outputs (bit-identical trivially).
//   ROW — the mask is a trailing suffix of a contiguous view: each output
//     folds one contiguous row through the SAME fixed block tree as the full
//     reduction (block_partial + left-to-right fold); parallel over outputs.
//   GENERAL — any other mask/stride pattern: kept-first permuted view walked
//     by a dst-order odometer, each output folding its reduced sub-space in
//     canonical reduced-dims order (scalar; the correctness fallback).
//
// argmin/argmax over ONE axis (i64 outputs, NumPy first-wins ties) and
// cumsum ALONG one axis ride the same dispatch. Mean = Sum then one divide
// pass. Parallelism arrives via crd-jobs when a pool is live; every path is
// bit-identical across {1..16} workers (gated).
// ---------------------------------------------------------------------------

namespace crd::hesap::tensor
{

enum class ReduceOp : crd::u8
{
    Sum,
    Prod,
    Min,
    Max,
    Mean,
};

namespace detail
{

// Vertical fold: out[0..w) over `rows` slices of width w (contiguous), i-order
// per output element. SIMD across outputs; tail scalar. Op applies per lane.
template <typename T, typename Op> inline void vertical_fold(T* out, const T* src, crd::u64 rows, crd::u64 w) noexcept
{
    using V = typename EwSimd<T>::Vec;
    constexpr crd::u64 kW = EwSimd<T>::kWidth;
    // init with slice 0 (correct for min/max; sum/prod fold from the first
    // value identically to the scalar chain)
    for (crd::u64 j = 0; j < w; ++j)
    {
        out[j] = src[j];
    }
    for (crd::u64 i = 1; i < rows; ++i)
    {
        const T* row = src + i * w;
        crd::u64 j = 0;
        for (; j + kW <= w; j += kW)
        {
            Op::v(V::load(out + j), V::load(row + j)).store(out + j);
        }
        for (; j < w; ++j)
        {
            out[j] = Op::s(out[j], row[j]);
        }
    }
}

// Op-init: Sum/Prod fold from their identity; Min/Max (no identity) seed
// from the first element of the run (the full-reduction convention).
template <typename Op, typename T> [[nodiscard]] inline T ax_init(const T& first) noexcept
{
    if constexpr (std::is_same_v<Op, RAdd<T>>)
    {
        return T{0};
    }
    else if constexpr (std::is_same_v<Op, RMul<T>>)
    {
        return T{1};
    }
    else
    {
        return first;
    }
}

// Row fold: each output folds one contiguous run of `len` through the fixed
// block tree (identical order to the full reduction of that run).
template <typename T, typename Op>
inline void row_fold_range(T* out, const T* src, crd::u64 len, crd::u64 o0, crd::u64 o1) noexcept
{
    for (crd::u64 o = o0; o < o1; ++o)
    {
        out[o] = reduce_fixed_tree<T, Op>(src + o * len, len, ax_init<Op>(src[o * len]), {});
    }
}

} // namespace detail

// dst: canonical contiguous, shape = kept dims (in order). ws: optional (the
// jobs pool parallelizes over outputs when live; ws is unused — outputs are
// disjoint). Zero-size reduced extents are BadInput for Min/Max (no identity).
template <typename T>
[[nodiscard]] inline TensorStatus reduce_axes(ReduceOp op, const TensorView<const T>& v, crd::u32 axes_mask,
                                              const TensorView<T>& dst) noexcept
{
    const crd::u32 r = v.rank();
    if (axes_mask == 0U || (axes_mask >> r) != 0U)
    {
        return TensorStatus::BadInput;
    }
    // kept shape + counts
    crd::u64 kept_shape[kMaxRank];
    crd::u32 kept[kMaxRank];
    crd::u32 red[kMaxRank];
    crd::u32 nk = 0;
    crd::u32 nr = 0;
    crd::u64 red_count = 1;
    for (crd::u32 d = 0; d < r; ++d)
    {
        if ((axes_mask & (1U << d)) != 0U)
        {
            red[nr++] = d;
            red_count *= v.shape(d);
        }
        else
        {
            kept_shape[nk] = v.shape(d);
            kept[nk] = d;
            ++nk;
        }
    }
    if (dst.rank() != nk || !dst.is_contiguous())
    {
        return TensorStatus::ShapeMismatch;
    }
    crd::u64 out_count = 1;
    for (crd::u32 d = 0; d < nk; ++d)
    {
        if (dst.shape(d) != kept_shape[d])
        {
            return TensorStatus::ShapeMismatch;
        }
        out_count *= kept_shape[d];
    }
    if (red_count == 0U || out_count == 0U)
    {
        return TensorStatus::BadInput; // empty reduce: no identity for min/max — pinned as status
    }

    const bool contiguous = v.is_contiguous();
    const bool leading = axes_mask == ((1U << nr) - 1U);                // reduce dims 0..nr-1
    const bool trailing = axes_mask == (((1U << nr) - 1U) << (r - nr)); // reduce dims r-nr..r-1
    T* out = dst.data();

    const auto run_dispatch = [&](auto opTag) noexcept
    {
        using Op = decltype(opTag);
        if (contiguous && leading)
        {
            // VERTICAL: parallel over output blocks (disjoint j ranges).
            const T* src = v.data();
            const crd::u64 w = out_count;
            const crd::u64 nblocks = (w + detail::kReduceBlock - 1U) / detail::kReduceBlock;
            if (crd::jobs::num_workers() > 1U && nblocks >= 2U && w <= 0xFFFFFFFFULL)
            {
                crd::jobs::Counter* c =
                    crd::jobs::parallel_for(static_cast<crd::u32>(nblocks), crd::jobs::num_workers(),
                                            [out, src, red_count, w](crd::u32 b0, crd::u32 b1)
                                            {
                                                for (crd::u32 b = b0; b < b1; ++b)
                                                {
                                                    const crd::u64 j0 = static_cast<crd::u64>(b) * detail::kReduceBlock;
                                                    const crd::u64 j1 =
                                                        j0 + detail::kReduceBlock < w ? j0 + detail::kReduceBlock : w;
                                                    // vertical fold restricted to columns [j0, j1)
                                                    for (crd::u64 j = j0; j < j1; ++j)
                                                    {
                                                        out[j] = src[j];
                                                    }
                                                    for (crd::u64 i = 1; i < red_count; ++i)
                                                    {
                                                        const T* row = src + i * w;
                                                        using V = typename detail::EwSimd<T>::Vec;
                                                        constexpr crd::u64 kW = detail::EwSimd<T>::kWidth;
                                                        crd::u64 j = j0;
                                                        for (; j + kW <= j1; j += kW)
                                                        {
                                                            Op::v(V::load(out + j), V::load(row + j)).store(out + j);
                                                        }
                                                        for (; j < j1; ++j)
                                                        {
                                                            out[j] = Op::s(out[j], row[j]);
                                                        }
                                                    }
                                                }
                                            });
                crd::jobs::wait(c);
                crd::jobs::frame_reset();
            }
            else
            {
                detail::vertical_fold<T, Op>(out, src, red_count, w);
            }
            return TensorStatus::Ok;
        }
        if (contiguous && trailing)
        {
            // ROW: parallel over outputs (each output's fold independent).
            const T* src = v.data();
            if (crd::jobs::num_workers() > 1U && out_count >= 8U && out_count <= 0xFFFFFFFFULL)
            {
                crd::jobs::Counter* c =
                    crd::jobs::parallel_for(static_cast<crd::u32>(out_count), crd::jobs::num_workers(),
                                            [out, src, red_count](crd::u32 o0, crd::u32 o1)
                                            { detail::row_fold_range<T, Op>(out, src, red_count, o0, o1); });
                crd::jobs::wait(c);
                crd::jobs::frame_reset();
            }
            else
            {
                detail::row_fold_range<T, Op>(out, src, red_count, 0U, out_count);
            }
            return TensorStatus::Ok;
        }
        // GENERAL: kept-first permuted view; dst-order odometer over kept dims,
        // canonical odometer over reduced dims per output. Scalar, serial.
        crd::u32 order[kMaxRank];
        for (crd::u32 d = 0; d < nk; ++d)
        {
            order[d] = kept[d];
        }
        for (crd::u32 d = 0; d < nr; ++d)
        {
            order[nk + d] = red[d];
        }
        TensorView<const T> pv = v.permute({order, r});
        crd::u64 kidx[kMaxRank] = {};
        for (crd::u64 o = 0; o < out_count; ++o)
        {
            // fold the reduced sub-space of this output
            crd::i64 base = 0;
            for (crd::u32 d = 0; d < nk; ++d)
            {
                base += static_cast<crd::i64>(kidx[d]) * pv.stride(d);
            }
            const T* p = pv.data() + base;
            crd::u64 ridx[kMaxRank] = {};
            T acc = p[0];
            bool first = true;
            for (crd::u64 e = 0; e < red_count; ++e)
            {
                crd::i64 off = 0;
                for (crd::u32 d = 0; d < nr; ++d)
                {
                    off += static_cast<crd::i64>(ridx[d]) * pv.stride(nk + d);
                }
                if (first)
                {
                    acc = p[off];
                    first = false;
                }
                else
                {
                    acc = Op::s(acc, p[off]);
                }
                for (crd::u32 d = nr; d-- > 0U;)
                {
                    if (++ridx[d] < pv.shape(nk + d))
                    {
                        break;
                    }
                    ridx[d] = 0;
                }
            }
            out[o] = acc;
            for (crd::u32 d = nk; d-- > 0U;)
            {
                if (++kidx[d] < static_cast<crd::u64>(kept_shape[d]))
                {
                    break;
                }
                kidx[d] = 0;
            }
        }
        return TensorStatus::Ok;
    };

    TensorStatus st = TensorStatus::BadInput;
    switch (op)
    {
        case ReduceOp::Sum:
        case ReduceOp::Mean:
            st = run_dispatch(detail::RAdd<T>{});
            break;
        case ReduceOp::Prod:
            st = run_dispatch(detail::RMul<T>{});
            break;
        case ReduceOp::Min:
            st = run_dispatch(detail::RMin<T>{});
            break;
        case ReduceOp::Max:
            st = run_dispatch(detail::RMax<T>{});
            break;
    }
    if (st == TensorStatus::Ok && op == ReduceOp::Mean)
    {
        // true divide (NumPy mean = sum/n): a reciprocal-multiply would differ
        // by a rounding in the last place for non-power-of-two counts.
        const T den = static_cast<T>(red_count);
        for (crd::u64 o = 0; o < out_count; ++o)
        {
            out[o] = out[o] / den;
        }
    }
    return st;
}

// argmin/argmax over ONE axis: i64 outputs (NumPy first-wins ties). The axis
// is folded in its canonical order per output element.
template <typename T, bool Max>
[[nodiscard]] inline TensorStatus reduce_arg_axis(const TensorView<const T>& v, crd::u32 axis,
                                                  const TensorView<crd::i64>& dst) noexcept
{
    const crd::u32 r = v.rank();
    if (axis >= r || v.shape(axis) == 0U)
    {
        return TensorStatus::BadInput;
    }
    if (dst.rank() != r - 1U || !dst.is_contiguous())
    {
        return TensorStatus::ShapeMismatch;
    }
    for (crd::u32 d = 0, o = 0; d < r; ++d)
    {
        if (d == axis)
        {
            continue;
        }
        if (dst.shape(o) != v.shape(d))
        {
            return TensorStatus::ShapeMismatch;
        }
        ++o;
    }
    // kept-first permuted view (axis last), dst-order odometer.
    crd::u32 order[kMaxRank];
    crd::u32 nk = 0;
    for (crd::u32 d = 0; d < r; ++d)
    {
        if (d != axis)
        {
            order[nk++] = d;
        }
    }
    order[nk] = axis;
    TensorView<const T> pv = v.permute({order, r});
    const crd::u64 n = pv.shape(nk);
    const crd::i64 s = pv.stride(nk);
    crd::i64* out = dst.data();
    crd::u64 kidx[kMaxRank] = {};
    crd::u64 outs = dst.size();
    for (crd::u64 o = 0; o < outs; ++o)
    {
        crd::i64 base = 0;
        for (crd::u32 d = 0; d < nk; ++d)
        {
            base += static_cast<crd::i64>(kidx[d]) * pv.stride(d);
        }
        const T* p = pv.data() + base;
        T best = p[0];
        crd::u64 bi = 0;
        for (crd::u64 i = 1; i < n; ++i)
        {
            const T x = p[static_cast<crd::i64>(i) * s];
            if (Max ? (best < x) : (x < best))
            {
                best = x;
                bi = i;
            }
        }
        out[o] = static_cast<crd::i64>(bi);
        for (crd::u32 d = nk; d-- > 0U;)
        {
            if (++kidx[d] < pv.shape(d))
            {
                break;
            }
            kidx[d] = 0;
        }
    }
    return TensorStatus::Ok;
}

template <typename T>
[[nodiscard]] inline TensorStatus reduce_argmin_axis(const TensorView<const T>& v, crd::u32 axis,
                                                     const TensorView<crd::i64>& dst) noexcept
{
    return reduce_arg_axis<T, false>(v, axis, dst);
}
template <typename T>
[[nodiscard]] inline TensorStatus reduce_argmax_axis(const TensorView<const T>& v, crd::u32 axis,
                                                     const TensorView<crd::i64>& dst) noexcept
{
    return reduce_arg_axis<T, true>(v, axis, dst);
}

// cumsum ALONG one axis (np.cumsum(v, axis)): dst has v's full shape; each
// 1-D lane along `axis` is an exact serial prefix scan in its canonical order.
template <typename T>
[[nodiscard]] inline TensorStatus reduce_cumsum_axis(const TensorView<const T>& v, crd::u32 axis,
                                                     const TensorView<T>& dst) noexcept
{
    const crd::u32 r = v.rank();
    if (axis >= r)
    {
        return TensorStatus::BadInput;
    }
    if (dst.rank() != r || dst.size() != v.size() || !dst.is_contiguous())
    {
        return TensorStatus::ShapeMismatch;
    }
    for (crd::u32 d = 0; d < r; ++d)
    {
        if (dst.shape(d) != v.shape(d))
        {
            return TensorStatus::ShapeMismatch;
        }
    }
    // lanes-first permuted views of BOTH tensors (axis last).
    crd::u32 order[kMaxRank];
    crd::u32 nk = 0;
    for (crd::u32 d = 0; d < r; ++d)
    {
        if (d != axis)
        {
            order[nk++] = d;
        }
    }
    order[nk] = axis;
    TensorView<const T> pv = v.permute({order, r});
    TensorView<T> pd = TensorView<T>(dst).permute({order, r});
    const crd::u64 n = pv.shape(nk);
    const crd::i64 si = pv.stride(nk);
    const crd::i64 so = pd.stride(nk);
    crd::u64 lanes = n == 0U ? 0U : v.size() / n;
    crd::u64 kidx[kMaxRank] = {};
    for (crd::u64 l = 0; l < lanes; ++l)
    {
        crd::i64 bi = 0;
        crd::i64 bo = 0;
        for (crd::u32 d = 0; d < nk; ++d)
        {
            bi += static_cast<crd::i64>(kidx[d]) * pv.stride(d);
            bo += static_cast<crd::i64>(kidx[d]) * pd.stride(d);
        }
        const T* p = pv.data() + bi;
        T* q = pd.data() + bo;
        T acc = T{0};
        for (crd::u64 i = 0; i < n; ++i)
        {
            acc += p[static_cast<crd::i64>(i) * si];
            q[static_cast<crd::i64>(i) * so] = acc;
        }
        for (crd::u32 d = nk; d-- > 0U;)
        {
            if (++kidx[d] < pv.shape(d))
            {
                break;
            }
            kidx[d] = 0;
        }
    }
    return TensorStatus::Ok;
}

} // namespace crd::hesap::tensor
