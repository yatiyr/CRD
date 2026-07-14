#pragma once

// ckir_reduce.hpp — B-cmp: a device-wide PARALLEL REDUCTION (sum / min / max) authored as a CKIR shared-memory compute kernel.
// The reusable REDUCTION AUTHORING layer (auto-exposure histogram totals, Hi-Z / SDSM depth min-max, luminance) — one source,
// bit-exact on the CPU oracle + Vulkan + DX12. NOT the elementwise `ReduceSum` tensor op (that is a per-output serial loop);
// this is the CUB `DeviceReduce`-class primitive: a grid of workgroups each reduces a contiguous span to one partial, then a
// second dispatch reduces the partials to the scalar.
//
// `build_reduce_block` is ONE kernel used by BOTH passes: workgroup w owns the contiguous span [w·elems, (w+1)·elems); its
// `threads` threads each SERIALLY pre-reduce every `threads`-th element of the span (block-strided, coalesced), then a
// log2(threads) shared TREE combine leaves the span's result in thread 0, which writes out[w]. The order (serial then tree) is
// fixed at authoring time, so the CPU oracle (`eval_cpu_kernel`) runs the identical graph ⇒ the float sum is BIT-EXACT vs the
// GPU (min/max are order-invariant regardless). `elems` must be a multiple of `threads`, `threads` a power of two.
//
// Buffers (set 0): in = 0 (ro), out = 1 (rw). `op` ∈ {Add (sum), Max, Min}. Authored in CKIR — plain index arithmetic + an
// `If`-guarded shared write per tree stage; every backend lowers it identically.

#include <crd/kir/ckir.hpp>

namespace crd::kir
{

// Reduction op → the associative combine used at every level (Add = sum, Max, Min). Kept explicit so a caller cannot pass a
// non-associative op (the tree/serial split assumes associativity for a correct — and, for Add, deterministically bit-exact —
// result).
[[nodiscard]] inline bool is_reduce_combine(KOp op) noexcept { return op == KOp::Add || op == KOp::Max || op == KOp::Min; }

struct ReducePlan
{
    KEntry   block;                 // pass 0: grid = nblocks, each reduces `elems_per_block` → partials[nblocks]
    KEntry   final_pass;            // pass 1: grid = 1, reduces the `nblocks` partials → out[0]
    KGraph*  block_graph = nullptr; // the KGraph `block` was authored into (graphs[0])
    KGraph*  final_graph = nullptr; // the KGraph `final_pass` was authored into (graphs[1])
    KOp      op          = KOp::Add;
    int      n           = 0;       // total element count
    int      threads     = 0;       // threads per workgroup (power of two)
    int      nblocks      = 0;      // pass-0 workgroups (= partial count fed to pass 1)
    int      elems_per_block = 0;   // pass-0 span per workgroup (= n / nblocks)
    bool     single_pass = false;   // n small enough for ONE workgroup ⇒ pass 1 unused
};

// The reusable per-workgroup reduce kernel: workgroup w reduces the contiguous span [w·elems, (w+1)·elems) → out[w].
[[nodiscard]] inline KEntry build_reduce_block(KGraph& g, int elems, int threads, KOp op)
{
    const int   per_thread = elems / threads; // serial elements per thread (elems is a multiple of threads)
    const Shape sh1        = make_shape({1});
    const auto  ku         = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add        = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  mul        = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int in_buf  = g.buffer_decl(DType::F32, 0, 0, false);
    const int out_buf = g.buffer_decl(DType::F32, 0, 1, true);
    const int sh      = g.shared_decl(DType::F32, threads);
    const int tid     = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wid     = g.builtin(KBuiltin::WorkgroupIndex);
    const int base    = add(mul(wid, ku(static_cast<crd::u32>(elems))), tid); // first element this thread owns

    const int mark = g.kernel_stmt_mark();

    // SERIAL pre-reduce: acc = combine over in[base + k·threads], k = 0..per_thread-1 (block-strided ⇒ coalesced loads).
    int acc = g.buffer_load(in_buf, base);
    for (int k = 1; k < per_thread; ++k)
    {
        const int e = g.buffer_load(in_buf, add(base, ku(static_cast<crd::u32>(k * threads))));
        acc         = g.binary(op, acc, e);
    }
    g.stmt_shared_store(sh, tid, acc);
    g.stmt_barrier();

    // TREE combine: log2(threads) halving stages; only the lower half writes, all threads barrier.
    for (int stride = threads / 2; stride >= 1; stride /= 2)
    {
        const int cond = g.binary(KOp::CmpLt, tid, ku(static_cast<crd::u32>(stride)));
        const int ifid = g.stmt_if_begin(cond);
        const int a    = g.shared_load(sh, tid);
        const int b    = g.shared_load(sh, add(tid, ku(static_cast<crd::u32>(stride))));
        g.stmt_shared_store(sh, tid, g.binary(op, a, b));
        g.stmt_if_end(ifid);
        g.stmt_barrier();
    }

    // thread 0 writes this workgroup's result.
    const int cond0 = g.binary(KOp::CmpEq, tid, ku(0));
    const int if0   = g.stmt_if_begin(cond0);
    g.stmt_buffer_store(out_buf, wid, g.shared_load(sh, ku(0)));
    g.stmt_if_end(if0);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(threads);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Author a device-wide reduction of `n` elements → 1 scalar. `graphs` = TWO caller-owned KGraphs (pass 0, pass 1) — one per
// unique entry (a CKIR emitter emits every decl in a graph, so two entries in one graph collide on binding-0). `threads` =
// workgroup size (power of two); `nblocks` = pass-0 workgroups. Requires n = nblocks·elems_per_block with elems_per_block a
// multiple of `threads`, and nblocks ≤ some multiple of `threads` reducible in one final workgroup (nblocks a multiple of
// `threads`, or ≤ threads). If `nblocks` == 1 the reduction is single-pass.
[[nodiscard]] inline ReducePlan build_reduce(KGraph** graphs, int n, KOp op, int threads = 256, int nblocks = 64)
{
    ReducePlan plan;
    plan.op      = op;
    plan.n       = n;
    plan.threads = threads;

    if (n <= threads * 8) // small ⇒ ONE workgroup (avoid a needless second dispatch); span = n, threads capped to n
    {
        int t = threads;
        while (t > n) { t /= 2; }
        plan.single_pass     = true;
        plan.nblocks         = 1;
        plan.elems_per_block = n;
        plan.threads         = t;
        plan.block           = build_reduce_block(*graphs[0], n, t, op);
        plan.block_graph     = graphs[0];
        return plan;
    }

    plan.single_pass     = false;
    plan.nblocks         = nblocks;
    plan.elems_per_block = n / nblocks;
    plan.block           = build_reduce_block(*graphs[0], plan.elems_per_block, threads, op);
    plan.block_graph     = graphs[0];
    // pass 1 reduces the `nblocks` partials in ONE workgroup (threads capped to nblocks).
    int ft = threads;
    while (ft > nblocks) { ft /= 2; }
    plan.final_pass  = build_reduce_block(*graphs[1], nblocks, ft, op);
    plan.final_graph = graphs[1];
    return plan;
}

} // namespace crd::kir
