#pragma once

// ckir_subgroup_test.hpp — B11: a shared CKIR kernel exercising the wave/subgroup reduce · scan · broadcast · shuffle ops, so the
// Vulkan (GLSL) and DX12 (HLSL) gates run the IDENTICAL kernel and the same CPU-oracle reference (bit-exact both backends). One
// 64-thread workgroup = two 32-lane subgroups; each thread writes 7 results to out[k*threads + tid].

#include <crd/kir/ckir.hpp>

namespace crd::gputest
{

constexpr int kSubgroupNOut = 11; // add·max·incl·excl·bcast·shuffle·or + quad{broadcast·swapX·swapY·swapDiag}

// in (binding 0, u32[threads]) → out (binding 1, u32[kSubgroupNOut*threads]). INTEGER ops ⇒ bit-exact vs the oracle.
inline crd::kir::KEntry build_subgroup_ops_kernel(crd::kir::KGraph& g, int threads)
{
    namespace kir     = crd::kir;
    const auto  sh    = kir::make_shape({1});
    const auto  ku    = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh, kir::DType::U32); };
    const auto  add   = [&](int a, int b) { return g.binary(kir::KOp::Add, a, b); };
    const auto  mul   = [&](int a, int b) { return g.binary(kir::KOp::Mul, a, b); };

    const int in   = g.buffer_decl(kir::DType::U32, 0, 0, false);
    const int out  = g.buffer_decl(kir::DType::U32, 0, 1, true);
    const int mark = g.kernel_stmt_mark();
    const int tid  = g.builtin(kir::KBuiltin::LocalInvocationIndex);
    const int x    = g.buffer_load(in, tid);
    g.stmt_materialize(x); // freeze the load so every lane feeds the subgroup ops from the SAME materialized value (uniform flow)

    const auto store = [&](int k, int val) {
        g.stmt_materialize(val); // ⛔ freeze the subgroup result in uniform flow before the (per-lane) store index diverges
        g.stmt_buffer_store(out, add(mul(ku(static_cast<crd::u32>(k)), ku(static_cast<crd::u32>(threads))), tid), val);
    };
    store(0, g.subgroup_add(x));
    store(1, g.subgroup_max(x));
    store(2, g.subgroup_inclusive_add(x));
    store(3, g.subgroup_exclusive_add(x));
    store(4, g.subgroup_broadcast_first(x));
    const int lane = g.binary(kir::KOp::BitAnd, add(tid, ku(1)), ku(31)); // read from the next lane in the subgroup ((tid+1)&31)
    store(5, g.subgroup_shuffle(x, lane));
    store(6, g.subgroup_or(x));
    store(7, g.quad_broadcast(x, ku(2)));  // every lane in a 2×2 quad ← quad lane 2's value
    store(8, g.quad_swap_x(x));            // horizontal swap
    store(9, g.quad_swap_y(x));            // vertical swap
    store(10, g.quad_swap_diagonal(x));    // diagonal swap

    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(threads);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::gputest
