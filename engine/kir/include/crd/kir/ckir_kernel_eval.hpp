#pragma once

// ckir_kernel_eval.hpp — B-cmp: the CPU ORACLE for imperative compute kernels (the shared-memory / barrier IR). Simulates
// ONE workgroup of `local_size` threads running the KEntry statement body, so a hand-authored shared-memory kernel (FFT,
// reduction, transpose, scan, …) is bit-exact-testable on the CPU exactly like the functional ops.
//
// EXECUTION MODEL — a lockstep tree interpreter over the structured statement body (BufferStore/SharedStore/Barrier/For/If):
//   • All threads run in lockstep; a statement executes for every ACTIVE thread before the next statement.
//   • Between barriers, a thread sees its OWN writes (an overlay keyed by thread) but NOT other threads' — cross-thread
//     reads resolve to the COMMITTED state as of the last barrier. All pending writes COMMIT at a barrier (and at the end).
//   • `For` has a UNIFORM bound (evaluated once) and may contain barriers (recursion re-enters the body per iteration, so a
//     barrier inside a loop synchronizes each iteration). `If` reduces the active-thread set to those whose cond is true —
//     a divergent guard around stores; a well-formed kernel never puts a barrier under a divergent `If` (that is GPU UB).
// A well-formed kernel (barrier before any cross-thread read) is order-independent ⇒ deterministic, and matches every GPU.
// Single workgroup (the FFT's on-chip primitives + the tests fit one workgroup; multi-workgroup global indexing is Phase-1).

#include <crd/kir/ckir.hpp>

#include <crd/containers/array.hpp>

namespace crd::kir
{

// A host-side storage buffer bound to the kernel at (set, binding). `data` holds `len` f64 scalars (read + written in place).
struct KernelBuffer
{
    crd::f64* data    = nullptr;
    crd::i32  len     = 0;
    crd::u8   set     = 0;
    crd::u8   binding = 0;
};

namespace kernel_detail
{
// one pending write (to shared array or storage buffer), keyed by the resource NODE id and the writing THREAD.
struct PendingWrite
{
    int      res_node  = -1;
    int      index     = 0;
    crd::f64 value     = 0.0;
    crd::u32 thread    = 0;
    bool     is_shared = false;
};
} // namespace kernel_detail

// Evaluate the kernel `entry` of `g` over `num_workgroups` INDEPENDENT workgroups of `local_size` threads (default 1).
// Each workgroup gets fresh (zeroed) shared memory and sees its own `WorkgroupIndex`; buffers persist across workgroups (a
// batched kernel offsets its global buffers by `WorkgroupIndex`, so each workgroup writes its own slice). In place.
inline void eval_cpu_kernel(const KGraph& g, const KEntry& entry, KernelBuffer* bufs, int nbufs, crd::u32 local_size,
                            crd::memory::IAllocator* scratch, crd::u32 num_workgroups = 1)
{
    using crd::containers::Array;
    using kernel_detail::PendingWrite;

    // ── shared arrays: one f64[length+pad] per SharedDecl node (indexed by node id). `shared_pool` IS the COMMITTED state
    //    (only barriers/commit change it); cross-thread reads see it, same-thread reads see the overlay first. ────────────
    const int       nnode = g.size();
    Array<crd::i32> shared_off(scratch);
    shared_off.resize(static_cast<crd::usize>(nnode));
    for (int i = 0; i < nnode; ++i) { shared_off[i] = -1; }
    Array<crd::f64> shared_pool(scratch);
    for (int i = 0; i < nnode; ++i)
    {
        const KNode& n = g.node(i);
        if (n.op == KOp::SharedDecl)
        {
            shared_off[i]        = static_cast<crd::i32>(shared_pool.size());
            const crd::usize len = static_cast<crd::usize>(n.iidx) + static_cast<crd::usize>(n.axes); // length + pad
            for (crd::usize k = 0; k < len; ++k) { shared_pool.push_back(0.0); }
        }
    }
    auto buffer_for = [&](int decl_node) -> KernelBuffer* {
        const KNode& d = g.node(decl_node);
        for (int b = 0; b < nbufs; ++b) { if (bufs[b].set == d.dset && bufs[b].binding == static_cast<crd::u8>(d.iidx)) { return &bufs[b]; } }
        return nullptr;
    };

    Array<crd::i64> loopval(scratch); // per-For induction value (indexed by the For statement id)
    loopval.resize(static_cast<crd::usize>(g.stmt_count()), 0);
    Array<PendingWrite> overlay(scratch); // all threads' pending writes since the last barrier (execution order)
    crd::u32            tid = 0;           // the thread the value interpreter is currently evaluating for
    crd::u32            wg  = 0;           // the workgroup currently being simulated (the WorkgroupIndex builtin)

    // Materialize cache: a frozen value node holds a per-thread snapshot (`mat_pool[mat_off[node] + thread]`) so later
    // reads return the snapshot, not a fresh shared re-read — the register-residency semantics. Structural (mat_off persists
    // across workgroups); the values are recomputed each workgroup when the Materialize statement re-runs.
    Array<crd::i32> mat_off(scratch);
    mat_off.resize(static_cast<crd::usize>(nnode), -1);
    Array<crd::f64> mat_pool(scratch);

    // read a resource element: this thread's most-recent overlay write wins, else the committed state.
    auto read_res = [&](int res_node, int idx, bool is_shared) -> crd::f64 {
        for (crd::isize k = static_cast<crd::isize>(overlay.size()) - 1; k >= 0; --k)
        {
            const PendingWrite& w = overlay[static_cast<crd::usize>(k)];
            if (w.thread == tid && w.is_shared == is_shared && w.res_node == res_node && w.index == idx) { return w.value; }
        }
        if (is_shared) { return shared_pool[static_cast<crd::usize>(shared_off[res_node]) + static_cast<crd::usize>(idx)]; }
        const KernelBuffer* kb = buffer_for(res_node);
        if (kb == nullptr || idx < 0 || idx >= kb->len) { return 0.0; }
        return kb->data[idx];
    };

    // MEMOIZATION — the deep-graph accelerant. Within ONE top-level eval the tid/loop/buffer/shared state is FIXED, so a node's
    // value is stable ⇒ cache it, keyed by a generation bumped on each top-level entry (so contexts self-invalidate — no clear).
    // Collapses the recursive re-walk that makes deep noise / cloud-density graphs intractable. DISABLED when the graph has
    // subgroup ops (they change `tid` MID-eval ⇒ a value cached under one lane would be wrong for another).
    bool has_subgroup = false;
    for (int i = 0; i < nnode; ++i) { const KOp op = g.node(i).op; if (op == KOp::SubgroupBallot || op == KOp::SubgroupMatch || op == KOp::SubgroupBallotExclCount) { has_subgroup = true; break; } }
    Array<crd::f64> memo(scratch);     memo.resize(static_cast<crd::usize>(nnode), 0.0);
    Array<crd::i64> memo_gen(scratch); memo_gen.resize(static_cast<crd::usize>(nnode), -1);
    crd::i64        cur_gen    = 0;
    int             eval_depth = 0;

    // recursive per-thread scalar evaluation of a value node (for the current `tid`).
    auto eval = [&](auto&& self, int node) -> crd::f64 {
        if (eval_depth == 0 && !has_subgroup) { ++cur_gen; } // top-level entry ⇒ a fresh context (tid/loop/state fixed within it)
        ++eval_depth;
        if (mat_off[static_cast<crd::usize>(node)] >= 0) // a frozen node returns its per-thread snapshot
        {
            --eval_depth;
            return mat_pool[static_cast<crd::usize>(mat_off[static_cast<crd::usize>(node)]) + static_cast<crd::usize>(tid)];
        }
        if (!has_subgroup && memo_gen[static_cast<crd::usize>(node)] == cur_gen) { --eval_depth; return memo[static_cast<crd::usize>(node)]; }
        const KNode& n = g.node(node);
        crd::f64     r = 0.0;
        switch (n.op)
        {
            case KOp::Const: r = n.cval; break;
            case KOp::Builtin: // LocalInvocationIndex = thread id; WorkgroupIndex = the current workgroup (1-D)
                if (static_cast<KBuiltin>(n.iidx) == KBuiltin::LocalInvocationIndex) { r = static_cast<crd::f64>(tid); }
                else if (static_cast<KBuiltin>(n.iidx) == KBuiltin::WorkgroupIndex) { r = static_cast<crd::f64>(wg); }
                else { r = 0.0; }
                break;
            case KOp::KernelLoopVar: r = static_cast<crd::f64>(loopval[static_cast<crd::usize>(n.a)]); break;
            case KOp::BufferLoad: r = read_res(n.a, static_cast<int>(self(self, n.b)), false); break;
            case KOp::SharedLoad: r = read_res(n.a, static_cast<int>(self(self, n.b)), true); break;
            case KOp::Select:     r = self(self, n.c) != 0.0 ? self(self, n.a) : self(self, n.b); break; // a=true b=false c=cond
            case KOp::Cast:       r = self(self, n.a); break;                                             // round below to n.dtype
            case KOp::SubgroupBallot: // model 32-lane subgroups: bit `lane` set iff lane's predicate is nonzero (all lanes active)
            {
                const crd::u32 saved  = tid;
                const crd::u32 sgbase = (saved / 32U) * 32U;
                crd::u32       mask   = 0U;
                for (crd::u32 l = sgbase; l < sgbase + 32U && l < local_size; ++l)
                {
                    tid = l;
                    if (self(self, n.a) != 0.0) { mask |= (1U << (l - sgbase)); }
                }
                tid = saved;
                r   = static_cast<crd::f64>(mask);
                break;
            }
            case KOp::SubgroupMatch: // 32-lane subgroup: bit `lane` set iff lane's value equals THIS thread's value
            {
                const crd::u32 saved  = tid;
                const crd::u32 sgbase = (saved / 32U) * 32U;
                const crd::f64 mine   = self(self, n.a);
                crd::u32       mask   = 0U;
                for (crd::u32 l = sgbase; l < sgbase + 32U && l < local_size; ++l)
                {
                    tid = l;
                    if (self(self, n.a) == mine) { mask |= (1U << (l - sgbase)); }
                }
                tid = saved;
                r   = static_cast<crd::f64>(mask);
                break;
            }
            case KOp::SubgroupBallotExclCount: // popcount of ballot bits strictly below this lane
            {
                const crd::u32 mask = static_cast<crd::u32>(static_cast<crd::i64>(self(self, n.a)));
                const crd::u32 lane = tid % 32U;
                const crd::u32 low  = (lane == 0U) ? 0U : (mask & ((1U << lane) - 1U));
                int            cnt  = 0;
                for (crd::u32 v = low; v != 0U; v >>= 1U) { cnt += static_cast<int>(v & 1U); }
                r = static_cast<crd::f64>(cnt);
                break;
            }
            default:
                if (n.c >= 0 && n.b >= 0 && n.a >= 0 && !is_compare(n.op) && n.op != KOp::Select)
                { r = apply_ternary(n.op, self(self, n.a), self(self, n.b), self(self, n.c)); }
                else if (n.b >= 0) { r = apply_binary(n.op, self(self, n.a), self(self, n.b)); }
                else if (n.a >= 0) { r = apply_unary(n.op, self(self, n.a)); }
                break;
        }
        const crd::f64 rr = round_dtype(r, n.dtype());
        if (!has_subgroup) { memo[static_cast<crd::usize>(node)] = rr; memo_gen[static_cast<crd::usize>(node)] = cur_gen; }
        --eval_depth;
        return rr;
    };

    // commit every pending write (in execution order → deterministic; a well-formed kernel has no cross-thread conflicts).
    auto commit_all = [&]() {
        for (crd::usize k = 0; k < overlay.size(); ++k)
        {
            const PendingWrite& w = overlay[k];
            if (w.is_shared) { shared_pool[static_cast<crd::usize>(shared_off[w.res_node]) + static_cast<crd::usize>(w.index)] = w.value; }
            else { KernelBuffer* kb = buffer_for(w.res_node); if (kb != nullptr && w.index >= 0 && w.index < kb->len) { kb->data[w.index] = w.value; } }
        }
        overlay.resize(0);
    };

    // lockstep interpreter: run statements [begin, begin+count) for every thread in `active`. MUTABLE: ForBreakIf removes
    // breaking threads from the CURRENT set (a For passes a loop-local copy, so the shrink persists across its iterations).
    auto exec = [&](auto&& self, int begin, int count, Array<crd::u32>& active) -> void {
        int i = begin;
        while (i < begin + count)
        {
            const KStmt& st = g.stmt(i);
            switch (st.kind)
            {
                case KStmtKind::BufferStore:
                case KStmtKind::SharedStore:
                {
                    const bool is_shared = st.kind == KStmtKind::SharedStore;
                    for (crd::usize a = 0; a < active.size(); ++a)
                    {
                        tid                = active[a];
                        const int      idx = static_cast<int>(eval(eval, st.index));
                        const crd::f64 val = round_dtype(eval(eval, st.value), g.node(st.target).dtype());
                        PendingWrite   w;
                        w.res_node = st.target; w.index = idx; w.value = val; w.thread = tid; w.is_shared = is_shared;
                        overlay.push_back(w); // visible to this thread's later same-segment reads; committed at the next barrier
                    }
                    ++i;
                    break;
                }
                case KStmtKind::Barrier:
                case KStmtKind::SyncWarp: commit_all(); ++i; break; // lockstep oracle: warp sync == block barrier == commit
                case KStmtKind::SpinUntilNonzero: ++i; break; // sequential workgroups ⇒ the predecessor already published (its
                                                              // stores committed) ⇒ the flag is nonzero ⇒ the spin exits at once
                case KStmtKind::SharedAtomicAdd: // accumulate every active thread's value into the bin (a SUM ⇒ order-independent)
                {
                    for (crd::usize a = 0; a < active.size(); ++a)
                    {
                        tid                = active[a];
                        const int      idx = static_cast<int>(eval(eval, st.index));
                        const crd::f64 val = eval(eval, st.value);
                        shared_pool[static_cast<crd::usize>(shared_off[st.target]) + static_cast<crd::usize>(idx)] += val;
                    }
                    ++i;
                    break;
                }
                case KStmtKind::Materialize:
                {
                    const int      node  = st.value;
                    const crd::i32 saved = mat_off[static_cast<crd::usize>(node)];
                    mat_off[static_cast<crd::usize>(node)] = -1; // compute the FRESH value (bypass any prior-workgroup snapshot)
                    Array<crd::f64> vals(scratch);
                    for (crd::usize a = 0; a < active.size(); ++a)
                    {
                        tid = active[a];
                        vals.push_back(round_dtype(eval(eval, node), g.node(node).dtype()));
                    }
                    if (saved >= 0) { mat_off[static_cast<crd::usize>(node)] = saved; }
                    else
                    {
                        mat_off[static_cast<crd::usize>(node)] = static_cast<crd::i32>(mat_pool.size());
                        for (crd::u32 t = 0; t < local_size; ++t) { mat_pool.push_back(0.0); }
                    }
                    const crd::usize off = static_cast<crd::usize>(mat_off[static_cast<crd::usize>(node)]);
                    for (crd::usize a = 0; a < active.size(); ++a) { mat_pool[off + static_cast<crd::usize>(active[a])] = vals[a]; }
                    ++i;
                    break;
                }
                case KStmtKind::For:
                {
                    if (active.size() > 0)
                    {
                        tid           = active[0]; // the bound is uniform across the workgroup
                        const int cnt = static_cast<int>(eval(eval, st.value));
                        Array<crd::u32> loop_active(scratch); // loop-local set: ForBreakIf shrinks it for the REMAINING iterations
                        for (crd::usize a = 0; a < active.size(); ++a) { loop_active.push_back(active[a]); }
                        for (int it = 0; it < cnt && loop_active.size() > 0; ++it)
                        {
                            loopval[static_cast<crd::usize>(i)] = it;
                            self(self, st.body_begin, st.body_count, loop_active);
                        }
                        loopval[static_cast<crd::usize>(i)] = 0;
                    }
                    i = st.body_begin + st.body_count; // skip the body (already executed via recursion)
                    break;
                }
                case KStmtKind::ForBreakIf: // per-thread break: drop threads whose cond fired from the loop's active set
                {
                    Array<crd::u32> keep(scratch);
                    for (crd::usize a = 0; a < active.size(); ++a) { tid = active[a]; if (eval(eval, st.value) == 0.0) { keep.push_back(active[a]); } }
                    active.resize(0);
                    for (crd::usize a = 0; a < keep.size(); ++a) { active.push_back(keep[a]); }
                    ++i;
                    break;
                }
                case KStmtKind::BufferTicket: // once per workgroup: sh[0] = buf[index]++ (sequential oracle ⇒ ticket == wg order)
                {
                    if (active.size() > 0)
                    {
                        tid                = active[0];
                        const int     idx  = static_cast<int>(eval(eval, st.index));
                        KernelBuffer* kb   = buffer_for(st.target);
                        crd::f64      tick = 0.0;
                        if (kb != nullptr && idx >= 0 && idx < kb->len) { tick = kb->data[idx]; kb->data[idx] += 1.0; }
                        shared_pool[static_cast<crd::usize>(shared_off[st.value])] = tick;
                    }
                    ++i;
                    break;
                }
                case KStmtKind::BufferAtomicAdd: // accumulate every active thread's value into the buffer cell (SUM ⇒ order-independent)
                {
                    for (crd::usize a = 0; a < active.size(); ++a)
                    {
                        tid                = active[a];
                        const int      idx = static_cast<int>(eval(eval, st.index));
                        const crd::f64 val = eval(eval, st.value);
                        KernelBuffer*  kb  = buffer_for(st.target);
                        if (kb != nullptr && idx >= 0 && idx < kb->len) { kb->data[idx] += val; }
                    }
                    ++i;
                    break;
                }
                case KStmtKind::BufferAtomicMin: // B4-vis: keep the SMALLEST value per cell (MIN ⇒ order-independent, like the SUM
                {                                // atomics). Buffers hold u32 keys as exact f64 integers (< 2^53), so a numeric min
                    for (crd::usize a = 0; a < active.size(); ++a) // equals the GPU's UNSIGNED atomicMin bit-for-bit.
                    {
                        tid                = active[a];
                        const int      idx = static_cast<int>(eval(eval, st.index));
                        const crd::f64 val = eval(eval, st.value);
                        KernelBuffer*  kb  = buffer_for(st.target);
                        if (kb != nullptr && idx >= 0 && idx < kb->len && val < kb->data[idx]) { kb->data[idx] = val; }
                    }
                    ++i;
                    break;
                }
                case KStmtKind::If:
                {
                    Array<crd::u32> sub(scratch);
                    for (crd::usize a = 0; a < active.size(); ++a) { tid = active[a]; if (eval(eval, st.value) != 0.0) { sub.push_back(active[a]); } }
                    self(self, st.body_begin, st.body_count, sub);
                    i = st.body_begin + st.body_count;
                    break;
                }
            }
        }
    };

    Array<crd::u32> all(scratch);
    for (crd::u32 t = 0; t < local_size; ++t) { all.push_back(t); }
    for (wg = 0; wg < num_workgroups; ++wg) // each workgroup is independent: fresh shared, its own WorkgroupIndex
    {
        for (crd::usize k = 0; k < shared_pool.size(); ++k) { shared_pool[k] = 0.0; }
        overlay.resize(0);
        exec(exec, entry.kernel_body_begin, entry.kernel_body_count, all);
        commit_all(); // flush any writes after the last barrier
    }
}

} // namespace crd::kir
