#pragma once

// einsum_reverse.hpp — Phase 3.1.6 v16-c: the einsum VJP, executed on the v14 `EinsumPlan` (reuse > reimplement,
// SANITY #8). The gradient of `einsum(spec, A_0..A_{n-1}) = Y` wrt operand k is ITSELF an einsum:
//     Ā_k = einsum( output = A_k's subscripts ;  inputs = {A_j : j≠k} ∪ {Ȳ} [∪ a `ones` operand] )
// where Ȳ carries the forward OUTPUT subscripts, and a `ones` operand supplies any index PRIVATE to A_k and summed
// out in the forward (so every reverse-output index appears in some reverse input — einsum cannot broadcast an
// output-only index otherwise). This is the general form of "matmul VJP = einsum with a permuted spec": it runs on
// the SAME TTGT-over-the-deterministic-GEMM machinery as the forward, so the VJP inherits the {1..16}-worker
// determinism moat by construction (torch/JAX scatter-add gradients drift).
//
// SCOPE: no diagonals (a repeated index within a term) — a diagonal VJP is a scatter-to-the-diagonal, returned as
// BadInput here; the v14-m corpus (matmul / attention QKᵀ·V / conv-as-matmul) has none.
//
// HEADER-ONLY bridge (ADR-0097 §1): this header includes the hesap-tensor + hesap-dense einsum machinery, so ONLY a
// target that already links those pays the edge. The autodiff library never compiles it, and the link-isolation
// smoke (`crd-hesap-autodiff-tests`, which links ONLY autodiff) never includes it — a lean scalar-Dual consumer
// still drags neither tensor nor dense.

#include <crd/hesap/tensor/einsum.hpp>      // EinsumExpr / EinsumPlan / einsum_parse / einsum_plan_build
#include <crd/hesap/tensor/einsum_exec.hpp> // einsum_execute (TTGT over the deterministic GEMM) + Tensor / TensorView

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <bit>

namespace crd::hesap::autodiff::reverse
{

using crd::hesap::tensor::EinsumExpr;
using crd::hesap::tensor::EinsumOptimize;
using crd::hesap::tensor::EinsumPlan;
using crd::hesap::tensor::kEinsumMaxOperands;
using crd::hesap::tensor::kMaxRank;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorStatus;
using crd::hesap::tensor::TensorView;

// Build the reverse `EinsumExpr` for the VJP wrt operand `k` (operand order: ȳ, then {A_j : j≠k}, then a `ones`
// operand iff some index of A_k is summed-out-and-private). `missing_mask`/`ones_rank` describe that ones operand.
[[nodiscard]] inline TensorStatus build_einsum_vjp_expr(const EinsumExpr& expr, const crd::u64* idx_size, crd::u32 k,
                                                        EinsumExpr& re, crd::u64& missing_mask,
                                                        crd::u64* ones_shape, crd::u32& ones_rank) noexcept
{
    if (k >= expr.n_ops || expr.has_diagonal)
    {
        return TensorStatus::BadInput;
    }
    re = EinsumExpr{};
    // operand 0 = ȳ (the forward output subscripts)
    re.term[0].count = expr.out_count;
    re.term[0].mask  = expr.out_mask;
    for (crd::u32 d = 0; d < expr.out_count; ++d) { re.term[0].idx[d] = expr.out_idx[d]; }
    crd::u32 no      = 1;
    crd::u64 covered = expr.out_mask;
    for (crd::u32 j = 0; j < expr.n_ops; ++j)
    {
        if (j == k) { continue; }
        re.term[no] = expr.term[j];
        covered |= expr.term[j].mask;
        ++no;
    }
    missing_mask = expr.term[k].mask & ~covered; // indices of A_k not present in any reverse input yet
    ones_rank    = 0;
    if (missing_mask != 0U)
    {
        if (no >= kEinsumMaxOperands)
        {
            return TensorStatus::RankOverflow;
        }
        crd::u64 m = missing_mask;
        while (m != 0U)
        {
            const crd::u32 b            = static_cast<crd::u32>(std::countr_zero(m));
            re.term[no].idx[ones_rank]  = static_cast<crd::u8>(b);
            ones_shape[ones_rank]       = idx_size[b];
            ++ones_rank;
            m &= m - 1U;
        }
        re.term[no].count = ones_rank;
        re.term[no].mask  = missing_mask;
        ++no;
    }
    re.n_ops         = no;
    re.out_count     = expr.term[k].count;
    re.out_mask      = expr.term[k].mask;
    for (crd::u32 d = 0; d < expr.term[k].count; ++d) { re.out_idx[d] = expr.term[k].idx[d]; }
    re.ellipsis_rank = expr.ellipsis_rank;
    re.has_diagonal  = false;
    return TensorStatus::Ok;
}

// Compute Ā_k (the VJP wrt operand k) for a forward einsum. `operands` are the forward inputs in `expr.term` order;
// `ybar` is the output cotangent in the forward output order (shape idx_size[out_idx]). `grad_out` comes back in
// operand k's own layout (expr.term[k].idx order). Deterministic + allocation via the caller's IAllocator.
template <typename T>
[[nodiscard]] inline TensorStatus einsum_vjp(const EinsumExpr& expr, const crd::u64* idx_size,
                                             crd::containers::ConstSpan<TensorView<const T>> operands,
                                             const TensorView<const T>& ybar, crd::u32 k, Tensor<T>& grad_out,
                                             crd::memory::IAllocator* alloc) noexcept
{
    if (alloc == nullptr || operands.size() != expr.n_ops)
    {
        return TensorStatus::BadInput;
    }
    EinsumExpr re;
    crd::u64   missing_mask = 0;
    crd::u64   ones_shape[kMaxRank];
    crd::u32   ones_rank = 0;
    const TensorStatus est = build_einsum_vjp_expr(expr, idx_size, k, re, missing_mask, ones_shape, ones_rank);
    if (est != TensorStatus::Ok)
    {
        return est;
    }

    EinsumPlan         plan;
    const TensorStatus pst = crd::hesap::tensor::einsum_plan_build(re, idx_size, EinsumOptimize::Optimal, plan);
    if (pst != TensorStatus::Ok)
    {
        return pst;
    }

    // Assemble the reverse operands in lockstep with re.term: ȳ, {A_j : j≠k}, then the ones operand.
    TensorView<const T> rv[kEinsumMaxOperands];
    rv[0]        = ybar;
    crd::u32 ri  = 1;
    for (crd::u32 j = 0; j < expr.n_ops; ++j)
    {
        if (j == k) { continue; }
        rv[ri++] = operands[j];
    }
    Tensor<T> ones(alloc);
    if (missing_mask != 0U)
    {
        ones = Tensor<T>(alloc, {ones_shape, ones_rank});
        if (ones.size() == 0U && ones_rank > 0U)
        {
            return TensorStatus::AllocFailed;
        }
        T* od = ones.data();
        for (crd::usize i = 0; i < ones.size(); ++i) { od[i] = T{1}; }
        rv[ri++] = TensorView<const T>(ones.view());
    }
    return crd::hesap::tensor::einsum_execute<T>(plan, {rv, ri}, grad_out, alloc);
}

} // namespace crd::hesap::autodiff::reverse
