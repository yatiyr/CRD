#pragma once

#include "einsum.hpp"
#include "permute.hpp"
#include "reduce_axes.hpp"

#include <crd/hesap/dense/blas3.hpp>

// ---------------------------------------------------------------------------
// crd-hesap-tensor einsum execution (Phase 3.1.6 v14-f; ADR-0096 §3) —
// TTGT over the engine's OWN deterministic GEMM (ADR-0063).
//
// Executes an `EinsumPlan` (v14-e) against TensorView operands:
//   per input   : repeated indices resolve as zero-copy STRIDE-SUM diagonal
//                 views; indices private to one operand and not needed
//                 downstream are pre-summed (reduce_axes, Tier-D order).
//   per step    : classify indices — Batch (in both, kept) · M (A-only kept)
//                 · N (B-only kept) · K (in both, contracted) — permute_copy
//                 both operands into [B,M,K] / [B,K,N] canonical layouts
//                 (the v14-d HPTT-crushing kernel), then one deterministic
//                 GEMM per batch entry (gemm_parallel when a jobs pool is
//                 live: bit-exact across {1..16} workers by ADR-0063).
//   final       : permute_copy into the requested output order.
//
// HEADER-ONLY on purpose: the tensor library keeps its link isolation (the
// smoke gate links tensor WITHOUT hesap-dense); only einsum-executing
// targets pay the dense link edge. GETT-fused kernels arrive ONLY if a
// profile names the transpose as the wall (SANITY #5).
//
// Intermediates allocate from the caller's IAllocator (einsum temporaries
// are inherently dynamic); everything else is fixed-size metadata. Statuses,
// never exceptions. Deterministic: fixed classification orders, fixed batch
// loop, deterministic permute + GEMM.
// ---------------------------------------------------------------------------

namespace crd::hesap::tensor
{

namespace detail
{

// A live SSA slot during execution: a tensor (owned or a view of an input)
// plus its CURRENT index order (ids, one per dim).
template <typename T> struct EinsumSlot
{
    TensorView<const T> view;
    crd::u8 order[kMaxRank];
    crd::u32 rank;
    crd::u64 mask;
};

// Zero-copy diagonal resolution: repeated ids in a term collapse to ONE dim
// whose stride is the SUM of the repeated dims' strides.
template <typename T>
[[nodiscard]] inline TensorStatus resolve_diagonals(const TensorView<const T>& v, const EinsumTerm& t,
                                                    EinsumSlot<T>& slot) noexcept
{
    crd::u64 shape[kMaxRank];
    crd::i64 stride[kMaxRank];
    crd::u32 r = 0;
    crd::u8 seen_at[kEinsumMaxIndices];
    for (crd::u32 i = 0; i < kEinsumMaxIndices; ++i)
    {
        seen_at[i] = 0xFFU;
    }
    for (crd::u32 d = 0; d < t.count; ++d)
    {
        const crd::u8 id = t.idx[d];
        if (seen_at[id] != 0xFFU)
        {
            if (v.shape(d) != shape[seen_at[id]])
            {
                return TensorStatus::ShapeMismatch; // a[i,i] with unequal extents
            }
            stride[seen_at[id]] += v.stride(d);
            continue;
        }
        seen_at[id] = static_cast<crd::u8>(r);
        shape[r] = v.shape(d);
        stride[r] = v.stride(d);
        slot.order[r] = id;
        ++r;
    }
    slot.view = TensorView<const T>(v.data(), {shape, r}, {stride, r});
    slot.rank = r;
    slot.mask = t.mask;
    return TensorStatus::Ok;
}

// Does the slot already carry [g0-run, g1-run, g2-run] contiguously, with
// ANY id order inside each run? (The GEMM only needs the RUNS separated; the
// within-run order becomes the shared sub-order the other operand must
// match.) Fills `sub` with the accepted full sequence when true.
template <typename T>
[[nodiscard]] inline bool grouped_runs(const EinsumSlot<T>& slot, crd::u64 g0, crd::u64 g1, crd::u64 g2,
                                       crd::u8* sub) noexcept
{
    if (!slot.view.is_contiguous())
    {
        return false;
    }
    crd::u32 d = 0;
    const crd::u64 groups[3] = {g0, g1, g2};
    for (const crd::u64 g : groups)
    {
        const crd::u32 cnt = static_cast<crd::u32>(std::popcount(g));
        for (crd::u32 i = 0; i < cnt; ++i)
        {
            if (d >= slot.rank || (g & (1ULL << slot.order[d])) == 0U)
            {
                return false;
            }
            sub[d] = slot.order[d];
            ++d;
        }
    }
    return d == slot.rank;
}

// Materialize `slot` into an EXPLICIT id sequence (contiguous canonical).
template <typename T>
[[nodiscard]] inline TensorStatus materialize_seq(EinsumSlot<T>& slot, const crd::u8* seq, crd::u32 n,
                                                  Tensor<T>& store) noexcept
{
    crd::u32 perm[kMaxRank];
    for (crd::u32 i = 0; i < n; ++i)
    {
        bool found = false;
        for (crd::u32 d = 0; d < slot.rank; ++d)
        {
            if (slot.order[d] == seq[i])
            {
                perm[i] = d;
                found = true;
                break;
            }
        }
        if (!found)
        {
            return TensorStatus::BadInput;
        }
    }
    if (n != slot.rank)
    {
        return TensorStatus::BadInput;
    }
    const TensorStatus st = permute_copy(slot.view, {perm, n}, store);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    slot.view = TensorView<const T>(store.view());
    for (crd::u32 d = 0; d < n; ++d)
    {
        slot.order[d] = seq[d];
    }
    return TensorStatus::Ok;
}

// Does slot.order begin with the exact `prefix` (len np) followed by a run of
// `tail_mask` ids in any order? (The other operand fixed the batch/K
// sub-orders; this operand's free group is the tail.)
template <typename T>
[[nodiscard]] inline bool matches_prefix_run(const EinsumSlot<T>& slot, const crd::u8* prefix, crd::u32 np,
                                             crd::u64 tail_mask) noexcept
{
    if (!slot.view.is_contiguous() || slot.rank < np)
    {
        return false;
    }
    for (crd::u32 i = 0; i < np; ++i)
    {
        if (slot.order[i] != prefix[i])
        {
            return false;
        }
    }
    for (crd::u32 d = np; d < slot.rank; ++d)
    {
        if ((tail_mask & (1ULL << slot.order[d])) == 0U)
        {
            return false;
        }
    }
    return true;
}

// Append mask ids to seq: those in `first_mask` first (ascending), rest after
// (ascending) — the consumer-aware within-group ordering.
inline void append_split(crd::u8* seq, crd::u32& n, crd::u64 mask, crd::u64 first_mask) noexcept
{
    for (crd::u64 pass = 0; pass < 2U; ++pass)
    {
        crd::u64 m = pass == 0U ? (mask & first_mask) : (mask & ~first_mask);
        while (m != 0U)
        {
            seq[n++] = static_cast<crd::u8>(std::countr_zero(m));
            m &= m - 1U;
        }
    }
}

// Sum out `remove_mask` indices from the slot (Tier-D order), leaving the
// kept indices in their current relative order.
template <typename T>
[[nodiscard]] inline TensorStatus slot_sum_out(EinsumSlot<T>& slot, crd::u64 remove_mask, Tensor<T>& store,
                                               crd::memory::IAllocator* alloc) noexcept
{
    crd::u32 axes = 0;
    crd::u64 kshape[kMaxRank];
    crd::u32 nk = 0;
    crd::u8 new_order[kMaxRank];
    for (crd::u32 d = 0; d < slot.rank; ++d)
    {
        if ((remove_mask & (1ULL << slot.order[d])) != 0U)
        {
            axes |= 1U << d;
        }
        else
        {
            kshape[nk] = slot.view.shape(d);
            new_order[nk] = slot.order[d];
            ++nk;
        }
    }
    Tensor<T> reduced(alloc, {kshape, nk});
    if (reduced.size() == 0U && nk > 0U)
    {
        return TensorStatus::AllocFailed;
    }
    const TensorStatus st = reduce_axes(ReduceOp::Sum, slot.view, axes, reduced.view());
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    store = static_cast<Tensor<T>&&>(reduced);
    slot.view = TensorView<const T>(store.view());
    slot.rank = nk;
    slot.mask &= ~remove_mask;
    for (crd::u32 d = 0; d < nk; ++d)
    {
        slot.order[d] = new_order[d];
    }
    return TensorStatus::Ok;
}

// Pack-free direct kernel for THIN contractions (K and N small: B stays
// L1-resident, A streams once, C rows accumulate in registers). The v0d
// packed GEMM pays pack traffic comparable to the FLOPs at these shapes —
// measured 0.54× torch on 24^4 tensor networks before this kernel.
// The sibling shape (M and K small, N large — the tensor-network step) is
// REGISTER-blocked over M (8 accumulators live across the full k loop per
// n-column pair): ~2 loads per 16 vector mul+adds — flop-bound, not L1-bound.
// Both run in the tensor kernels at the CPU's level; deterministic: fixed
// k-order per output, dispatch shape-only.
template <typename T>
inline void direct_gemm_thin(crd::u64 m_count, crd::u64 k_count, crd::u64 n_count, const T* a, const T* b,
                             T* c) noexcept
{
    const TensorKernels& k = *tensor_kernels().table;
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        k.einsum_thin_f32(m_count, k_count, n_count, a, b, c);
    }
    else
    {
        k.einsum_thin_f64(m_count, k_count, n_count, a, b, c);
    }
}

template <typename T>
inline void direct_gemm_smallm(crd::u64 m_count, crd::u64 k_count, crd::u64 n_count, const T* a, const T* b, T* c,
                               bool a_trans) noexcept
{
    const TensorKernels& k = *tensor_kernels().table;
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        k.einsum_smallm_f32(m_count, k_count, n_count, a, b, c, a_trans);
    }
    else
    {
        k.einsum_smallm_f64(m_count, k_count, n_count, a, b, c, a_trans);
    }
}

} // namespace detail

// Execute the plan. `operands` must match plan.expr (rank + extents per the
// subscripts; extent consistency is validated). The result tensor is resized
// to the output shape in the parsed output order.
template <typename T>
[[nodiscard]] inline TensorStatus einsum_execute(const EinsumPlan& plan,
                                                 crd::containers::ConstSpan<TensorView<const T>> operands,
                                                 Tensor<T>& out, crd::memory::IAllocator* alloc) noexcept
{
    const EinsumExpr& e = plan.expr;
    if (operands.size() != e.n_ops || alloc == nullptr)
    {
        return TensorStatus::BadInput;
    }
    // idx extents from the operands (validated against the plan's sizes).
    crd::u64 idx_size[kEinsumMaxIndices];
    for (crd::u32 i = 0; i < kEinsumMaxIndices; ++i)
    {
        idx_size[i] = plan.idx_size[i];
    }
    for (crd::u32 t = 0; t < e.n_ops; ++t)
    {
        if (operands[t].rank() != e.term[t].count)
        {
            return TensorStatus::ShapeMismatch;
        }
        for (crd::u32 d = 0; d < e.term[t].count; ++d)
        {
            if (operands[t].shape(d) != idx_size[e.term[t].idx[d]])
            {
                return TensorStatus::ShapeMismatch;
            }
        }
    }

    // SSA slots + owned intermediates. Each slot double-buffers (a/b stores)
    // so a permute/reduce NEVER writes into the store holding its own source
    // (permute_copy's resize frees dst's buffer before the copy — aliasing
    // the source would read freed memory). `held` tracks the current holder:
    // 0 = external view, 1 = store_a, 2 = store_b.
    detail::EinsumSlot<T> slot[2U * kEinsumMaxOperands];
    Tensor<T> store_a[2U * kEinsumMaxOperands];
    Tensor<T> store_b[2U * kEinsumMaxOperands];
    crd::u8 held[2U * kEinsumMaxOperands] = {};
    for (crd::u32 i = 0; i < 2U * kEinsumMaxOperands; ++i)
    {
        store_a[i] = Tensor<T>(alloc);
        store_b[i] = Tensor<T>(alloc);
    }
    const auto other_store = [&](crd::u32 id) -> Tensor<T>&
    {
        Tensor<T>& s = held[id] == 1U ? store_b[id] : store_a[id];
        held[id] = held[id] == 1U ? 2U : 1U;
        return s;
    };
    for (crd::u32 t = 0; t < e.n_ops; ++t)
    {
        const TensorStatus st = detail::resolve_diagonals(operands[t], e.term[t], slot[t]);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
    }

    // downstream-need masks per step (indices needed AFTER this step)
    crd::u32 n_ssa = e.n_ops;
    for (crd::u32 s = 0; s < plan.n_steps; ++s)
    {
        const EinsumStep& stp = plan.step[s];
        detail::EinsumSlot<T>& lhs = slot[stp.a];
        detail::EinsumSlot<T>& rhs = slot[stp.b];
        const crd::u64 result = stp.result_mask;

        // pre-sum indices private to one side and not in the result
        const crd::u64 a_private = lhs.mask & ~rhs.mask & ~result;
        if (a_private != 0U)
        {
            const TensorStatus st = detail::slot_sum_out(lhs, a_private, other_store(stp.a), alloc);
            if (st != TensorStatus::Ok)
            {
                return st;
            }
        }
        const crd::u64 b_private = rhs.mask & ~lhs.mask & ~result;
        if (b_private != 0U)
        {
            const TensorStatus st = detail::slot_sum_out(rhs, b_private, other_store(stp.b), alloc);
            if (st != TensorStatus::Ok)
            {
                return st;
            }
        }

        const crd::u64 batch = lhs.mask & rhs.mask & result;
        const crd::u64 contract = lhs.mask & rhs.mask & ~result;
        const crd::u64 m_mask = lhs.mask & ~rhs.mask & result;
        const crd::u64 n_mask = rhs.mask & ~lhs.mask & result;

        // TTGT copy avoidance, generalized: the LARGER operand keeps its
        // natural layout when it is any (batch|M|K)-run partition (any
        // within-run order, trans accepted); the shared batch/K sub-orders it
        // fixes are then imposed on the smaller operand (natural [sB,sK,tail]
        // check, else one small materialize). Materialized free groups order
        // their ids CONSUMER-KEPT-FIRST so the next step finds a natural
        // partition — this is what kills the inter-step permutes.
        using DTrans = dense::Trans;
        DTrans ta = DTrans::None;
        DTrans tb = DTrans::None;
        const crd::u32 cnt_b = static_cast<crd::u32>(std::popcount(batch));
        const crd::u32 cnt_m = static_cast<crd::u32>(std::popcount(m_mask));
        const crd::u32 cnt_k = static_cast<crd::u32>(std::popcount(contract));
        crd::u8 seq_a[kMaxRank];
        crd::u8 seq_b[kMaxRank];
        crd::u8 pre[kMaxRank];
        crd::u8 m_ord[kMaxRank];
        crd::u8 n_ord[kMaxRank];
        crd::u32 pn = 0;

        // consumer-kept mask for the RESULT of this step (kept-first ordering)
        crd::u64 consumer_kept = e.out_mask;
        for (crd::u32 s2 = s + 1U; s2 < plan.n_steps; ++s2)
        {
            if (plan.step[s2].a == n_ssa || plan.step[s2].b == n_ssa)
            {
                consumer_kept = plan.step[s2].result_mask;
                break;
            }
        }

        bool a_nat = false;
        bool b_nat = false;
        if (detail::grouped_runs(lhs, batch, m_mask, contract, seq_a))
        {
            a_nat = true; // [B,M,K]
        }
        else if (detail::grouped_runs(lhs, batch, contract, m_mask, seq_a))
        {
            a_nat = true;
            ta = DTrans::Transpose; // [B,K,M]
        }
        if (a_nat)
        {
            pn = 0;
            for (crd::u32 i = 0; i < cnt_b; ++i)
            {
                pre[pn++] = seq_a[i];
            }
            const crd::u32 koff = ta == DTrans::None ? cnt_b + cnt_m : cnt_b;
            for (crd::u32 i = 0; i < cnt_k; ++i)
            {
                pre[pn++] = seq_a[koff + i];
            }
            const crd::u32 moff = ta == DTrans::None ? cnt_b : cnt_b + cnt_k;
            for (crd::u32 i = 0; i < cnt_m; ++i)
            {
                m_ord[i] = seq_a[moff + i];
            }
            // fit B: natural [sB,sK,N-run] or materialize to it
            if (detail::matches_prefix_run(rhs, pre, pn, n_mask))
            {
                for (crd::u32 i = 0; i < rhs.rank - pn; ++i)
                {
                    n_ord[i] = rhs.order[pn + i];
                }
            }
            else
            {
                crd::u32 nlen = pn;
                detail::append_split(pre, nlen, n_mask, consumer_kept);
                const TensorStatus st = detail::materialize_seq(rhs, pre, nlen, other_store(stp.b));
                if (st != TensorStatus::Ok)
                {
                    return st;
                }
                for (crd::u32 i = 0; i < nlen - pn; ++i)
                {
                    n_ord[i] = pre[pn + i];
                }
            }
        }
        else
        {
            if (detail::grouped_runs(rhs, batch, contract, n_mask, seq_b))
            {
                b_nat = true; // [B,K,N]
            }
            else if (detail::grouped_runs(rhs, batch, n_mask, contract, seq_b))
            {
                b_nat = true;
                tb = DTrans::Transpose; // [B,N,K]
            }
            if (b_nat)
            {
                pn = 0;
                for (crd::u32 i = 0; i < cnt_b; ++i)
                {
                    pre[pn++] = seq_b[i];
                }
                const crd::u32 koff2 = tb == DTrans::None ? cnt_b : cnt_b + (rhs.rank - cnt_b - cnt_k);
                for (crd::u32 i = 0; i < cnt_k; ++i)
                {
                    pre[pn++] = seq_b[koff2 + i];
                }
                const crd::u32 noff = tb == DTrans::None ? cnt_b + cnt_k : cnt_b;
                for (crd::u32 i = 0; i < rhs.rank - cnt_b - cnt_k; ++i)
                {
                    n_ord[i] = seq_b[noff + i];
                }
            }
            else
            {
                // neither natural: canonical sub-orders (batch asc, K asc)
                pn = 0;
                detail::append_split(pre, pn, batch, ~crd::u64{0});
                detail::append_split(pre, pn, contract, ~crd::u64{0});
                crd::u32 blen = pn;
                detail::append_split(pre, blen, n_mask, consumer_kept);
                const TensorStatus st = detail::materialize_seq(rhs, pre, blen, other_store(stp.b));
                if (st != TensorStatus::Ok)
                {
                    return st;
                }
                for (crd::u32 i = 0; i < blen - pn; ++i)
                {
                    n_ord[i] = pre[pn + i];
                }
                tb = DTrans::None;
            }
            // fit A as [sB,sK,M-run] (trans) or materialize to it
            if (detail::matches_prefix_run(lhs, pre, pn, m_mask))
            {
                ta = DTrans::Transpose;
                for (crd::u32 i = 0; i < lhs.rank - pn; ++i)
                {
                    m_ord[i] = lhs.order[pn + i];
                }
            }
            else
            {
                crd::u32 alen = pn;
                detail::append_split(pre, alen, m_mask, consumer_kept);
                const TensorStatus st = detail::materialize_seq(lhs, pre, alen, other_store(stp.a));
                if (st != TensorStatus::Ok)
                {
                    return st;
                }
                ta = DTrans::Transpose;
                for (crd::u32 i = 0; i < alen - pn; ++i)
                {
                    m_ord[i] = pre[pn + i];
                }
            }
        }

        const crd::u64 nb = detail::mask_size(batch, idx_size);
        const crd::u64 nm = detail::mask_size(m_mask, idx_size);
        const crd::u64 nk = detail::mask_size(contract, idx_size);
        const crd::u64 nn = detail::mask_size(n_mask, idx_size);

        // result slot: [batch, M, N] in the ACTUAL sub-orders the GEMM emits
        // (batch = the shared prefix, M = A's m-run, N = B's n-run)
        detail::EinsumSlot<T>& out_slot = slot[n_ssa];
        crd::u64 rshape[kMaxRank];
        crd::u32 rr = 0;
        const crd::u32 cnt_n = static_cast<crd::u32>(std::popcount(n_mask));
        for (crd::u32 i = 0; i < cnt_b; ++i)
        {
            out_slot.order[rr] = pre[i];
            rshape[rr] = idx_size[pre[i]];
            ++rr;
        }
        for (crd::u32 i = 0; i < cnt_m; ++i)
        {
            out_slot.order[rr] = m_ord[i];
            rshape[rr] = idx_size[m_ord[i]];
            ++rr;
        }
        for (crd::u32 i = 0; i < cnt_n; ++i)
        {
            out_slot.order[rr] = n_ord[i];
            rshape[rr] = idx_size[n_ord[i]];
            ++rr;
        }
        Tensor<T> res(alloc, {rshape, rr});
        if (res.size() == 0U && rr > 0U)
        {
            return TensorStatus::AllocFailed;
        }
        // one deterministic GEMM per batch entry (row-major, packed layouts)
        const T* pa = lhs.view.data();
        const T* pb = rhs.view.data();
        T* pc = res.data();
        const crd::u32 nw = crd::jobs::num_workers();
        for (crd::u64 bi = 0; bi < nb; ++bi)
        {
            using dense::Layout;
            using dense::MatrixView;
            const crd::u64 ar = ta == dense::Trans::None ? nm : nk;
            const crd::u64 ac = ta == dense::Trans::None ? nk : nm;
            const crd::u64 br = tb == dense::Trans::None ? nk : nn;
            const crd::u64 bc = tb == dense::Trans::None ? nn : nk;
            // (comparisons above use the enum's qualified name only — fine)
            const MatrixView<const T, Layout::RowMajor> ma(pa + bi * nm * nk, ar, ac, ac);
            const MatrixView<const T, Layout::RowMajor> mb(pb + bi * nk * nn, br, bc, bc);
            MatrixView<T, Layout::RowMajor> mc(pc + bi * nm * nn, nm, nn, nn);
            // thin-contraction direct kernel: pack-free when B is L1-resident
            const bool thin = ta == DTrans::None && tb == DTrans::None && nk <= 48U && nn <= 8U * kEwLanes<T> &&
                              nk * nn * sizeof(T) <= 32768U;
            const bool thin_m = tb == DTrans::None && nm <= 64U && nk <= 64U && nn >= 8U * kEwLanes<T>;
            if (thin)
            {
                detail::direct_gemm_thin<T>(nm, nk, nn, pa + bi * nm * nk, pb + bi * nk * nn, pc + bi * nm * nn);
            }
            else if (thin_m)
            {
                detail::direct_gemm_smallm<T>(nm, nk, nn, pa + bi * nm * nk, pb + bi * nk * nn, pc + bi * nm * nn,
                                              ta == DTrans::Transpose);
            }
            else if (nw > 1U)
            {
                dense::gemm_parallel<T, Layout::RowMajor>(nw, T{1}, ma, mb, T{0}, mc, ta, tb, alloc);
            }
            else
            {
                dense::gemm<T, Layout::RowMajor>(T{1}, ma, mb, T{0}, mc, ta, tb, alloc);
            }
        }
        store_a[n_ssa] = static_cast<Tensor<T>&&>(res);
        held[n_ssa] = 1U;
        out_slot.view = TensorView<const T>(store_a[n_ssa].view());
        out_slot.rank = rr;
        out_slot.mask = result;
        ++n_ssa;
    }

    // final slot: the single survivor (n_steps==0 → the lone input)
    detail::EinsumSlot<T>& last_slot = slot[plan.n_steps == 0U ? 0U : n_ssa - 1U];
    // 1-operand plans may still need a sum-out (e.g. "ii" trace, "ab->a")
    const crd::u64 extra = last_slot.mask & ~e.out_mask;
    if (extra != 0U)
    {
        const crd::u32 fid = plan.n_steps == 0U ? 0U : n_ssa - 1U;
        const TensorStatus st = detail::slot_sum_out(last_slot, extra, other_store(fid), alloc);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
    }
    // permute into the requested output order
    crd::u32 perm[kMaxRank];
    for (crd::u32 o = 0; o < e.out_count; ++o)
    {
        bool found = false;
        for (crd::u32 d = 0; d < last_slot.rank; ++d)
        {
            if (last_slot.order[d] == e.out_idx[o])
            {
                perm[o] = d;
                found = true;
                break;
            }
        }
        if (!found)
        {
            return TensorStatus::BadInput;
        }
    }
    return permute_copy(last_slot.view, {perm, e.out_count}, out);
}

} // namespace crd::hesap::tensor
