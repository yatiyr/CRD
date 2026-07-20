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
#include <crd/core/assert.hpp>
#include <crd/math/cmath.hpp>

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
            // ⛔ THIS EVALUATOR IS SCALAR. One f64 per node per lane — there is nowhere to put a second or third
            //    component. A vector-valued node reaching the fallback below would be handed to apply_ternary /
            //    apply_unary, which do not implement these ops, and would evaluate to GARBAGE with no diagnostic: the
            //    kernel runs, produces plausible numbers, and is wrong. Refuse instead.
            //    Vector maths in a COMPUTE kernel is written component-wise on scalar nodes (see ckir_lss.hpp's V3);
            //    the vec3 forms are for the RASTER tier, where the emitters lower them to native vector types.
            case KOp::Vec2:
            case KOp::Vec3:
            case KOp::VecComp:
            case KOp::VecConcat:
            case KOp::Swizzle:
            case KOp::Splat:
            case KOp::Dot:
            case KOp::Cross:
                CRD_ASSERT_MSG(false, "eval_cpu_kernel is scalar: vector-valued CKIR nodes (Vec*/Swizzle/Dot/Cross) "
                                      "cannot be evaluated in the statement tier - write component-wise scalars");
                break;
            default:
                if (n.c >= 0 && n.b >= 0 && n.a >= 0 && !is_compare(n.op) && n.op != KOp::Select)
                { r = apply_ternary(n.op, self(self, n.a), self(self, n.b), self(self, n.c)); }
                else if (n.b >= 0) { r = apply_binary_typed(n.op, self(self, n.a), self(self, n.b), n.dtype()); }
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
                case KStmtKind::BufferAtomicAddFetch:
                case KStmtKind::BufferAtomicExchange:
                {                                 // VALUE-RETURNING atomics are ORDER-DEPENDENT (the returned OLD value depends on
                    const int node = st.result;   // execution order); the oracle runs active threads SEQUENTIALLY (a valid order, not
                    if (mat_off[static_cast<crd::usize>(node)] < 0) // the GPU's) ⇒ validate the DETERMINISTIC downstream (sorted resolve).
                    {
                        mat_off[static_cast<crd::usize>(node)] = static_cast<crd::i32>(mat_pool.size());
                        for (crd::u32 t = 0; t < local_size; ++t) { mat_pool.push_back(0.0); }
                    }
                    const crd::usize off = static_cast<crd::usize>(mat_off[static_cast<crd::usize>(node)]);
                    const bool       add = st.kind == KStmtKind::BufferAtomicAddFetch;
                    for (crd::usize a = 0; a < active.size(); ++a)
                    {
                        tid                = active[a];
                        const int      idx = static_cast<int>(eval(eval, st.index));
                        const crd::f64 val = eval(eval, st.value);
                        KernelBuffer*  kb  = buffer_for(st.target);
                        crd::f64       old = 0.0;
                        if (kb != nullptr && idx >= 0 && idx < kb->len) { old = kb->data[idx]; kb->data[idx] = add ? old + val : val; }
                        mat_pool[off + static_cast<crd::usize>(active[a])] = old;
                    }
                    ++i;
                    break;
                }
                case KStmtKind::TraceRayCurves:
                {   // B18-f: PROCEDURAL curve traversal oracle. Computed in FLOAT, not double: the intersection is OUR
                    // shader code rather than vendor traversal, so it must be comparable TIGHTLY — and per the
                    // project oracle doctrine an oracle more accurate than the device cannot certify it.
                    // (Measured before this change: an f64 oracle differed from the f32 IR path by 19 ulp.)
                    // PROCEDURAL curve traversal oracle — brute-force ray vs LINEAR SWEPT SPHERE over every
                    // segment. On the device the BLAS narrows this to candidate AABBs and the shader runs the same
                    // maths per candidate; the answer must agree, which is exactly what the GPU gate compares.
                    const int node  = st.result;
                    const int unode = g.stmt_ext_operand(st, 9);
                    const int pnode = g.stmt_ext_operand(st, 10);
                    if (mat_off[static_cast<crd::usize>(node)] < 0)
                    {
                        mat_off[static_cast<crd::usize>(node)] = static_cast<crd::i32>(mat_pool.size());
                        for (crd::u32 t = 0; t < local_size; ++t) { mat_pool.push_back(0.0); }
                    }
                    if (mat_off[static_cast<crd::usize>(unode)] < 0)
                    {
                        mat_off[static_cast<crd::usize>(unode)] = static_cast<crd::i32>(mat_pool.size());
                        for (crd::u32 t = 0; t < local_size; ++t) { mat_pool.push_back(0.0); }
                    }
                    if (mat_off[static_cast<crd::usize>(pnode)] < 0)
                    {
                        mat_off[static_cast<crd::usize>(pnode)] = static_cast<crd::i32>(mat_pool.size());
                        for (crd::u32 t = 0; t < local_size; ++t) { mat_pool.push_back(0.0); }
                    }
                    const crd::usize    toff = static_cast<crd::usize>(mat_off[static_cast<crd::usize>(node)]);
                    const crd::usize    uoff = static_cast<crd::usize>(mat_off[static_cast<crd::usize>(unode)]);
                    const crd::usize    poff = static_cast<crd::usize>(mat_off[static_cast<crd::usize>(pnode)]);
                    const KernelBuffer* segs = buffer_for(g.stmt_ext_operand(st, 8)); // 8 floats per segment
                    const int nseg = (segs != nullptr) ? segs->len / 8 : 0;
                    for (crd::usize a = 0; a < active.size(); ++a)
                    {
                        tid                = active[a];
                        const float    ox  = (float)eval(eval, g.stmt_ext_operand(st, 0));
                        const float    oy  = (float)eval(eval, g.stmt_ext_operand(st, 1));
                        const float    oz  = (float)eval(eval, g.stmt_ext_operand(st, 2));
                        const float    dx  = (float)eval(eval, g.stmt_ext_operand(st, 3));
                        const float    dy  = (float)eval(eval, g.stmt_ext_operand(st, 4));
                        const float    dz  = (float)eval(eval, g.stmt_ext_operand(st, 5));
                        const float    tmn = (float)eval(eval, g.stmt_ext_operand(st, 6));
                        const float    tmx = (float)eval(eval, g.stmt_ext_operand(st, 7));
                        // ⛔ Quilez round-cone, with the direction NORMALISED — the reference form assumes a unit
                        //    direction, and the earlier version did not normalise, used d2 = m0 + rr*rr instead of
                        //    m0 - rr*rr, scaled the k-coefficients by m0, and tested the axial span against m0 instead
                        //    of d2. Four defects, all invisible for a capsule (rr == 0), together producing hits in
                        //    empty space: 118 of 132 reported hits were off-surface against a ray-march ground truth.
                        const float rl   = crd::math::sqrt(dx * dx + dy * dy + dz * dz);
                        const float rinv = rl > 1.0e-20F ? 1.0F / rl : 0.0F;
                        const float ux  = dx * rinv;
                        const float uy  = dy * rinv;
                        const float uz2 = dz * rinv;
                        const float tminu = tmn * rl;
                        float       best  = tmx * rl; // work in unit-direction units, convert back at the end
                        float       bu    = 0.0F;
                        crd::u32    bp    = 0xFFFFFFFFU; // winning segment; 0xFFFFFFFF on a miss, as both emitters do
                        for (int sgi = 0; sgi < nseg; ++sgi)
                        {
                            const crd::f64* q  = segs->data + static_cast<crd::isize>(sgi) * 8;
                            const float bax = (float)q[4] - (float)q[0];
                            const float bay = (float)q[5] - (float)q[1];
                            const float baz = (float)q[6] - (float)q[2];
                            // RE-ORIGIN AT THE SEGMENT before forming the k-coefficients. The oracle runs in FLOAT
                            // precisely so it models the shader's numerics; without this it loses the radius term to
                            // cancellation at realistic fibre thickness exactly as the shader did.
                            const float tsh = ((float)q[0] - ox) * ux + ((float)q[1] - oy) * uy + ((float)q[2] - oz) * uz2;
                            const float lox = ox + ux * tsh;
                            const float loy = oy + uy * tsh;
                            const float loz = oz + uz2 * tsh;
                            const float oax = lox - (float)q[0];
                            const float oay = loy - (float)q[1];
                            const float oaz = loz - (float)q[2];
                            const float obx = lox - (float)q[4];
                            const float oby = loy - (float)q[5];
                            const float obz = loz - (float)q[6];
                            const float ra = (float)q[3];
                            const float rb = (float)q[7];
                            const float rr = ra - rb;
                            const float m0 = bax * bax + bay * bay + baz * baz;
                            const float m1 = bax * oax + bay * oay + baz * oaz;
                            const float m2 = bax * ux + bay * uy + baz * uz2;
                            const float m3 = ux * oax + uy * oay + uz2 * oaz;
                            const float m5 = oax * oax + oay * oay + oaz * oaz;
                            const float m6 = obx * ux + oby * uy + obz * uz2;
                            const float m7 = obx * obx + oby * oby + obz * obz;
                            const float d2 = m0 - rr * rr;
                            // conical side
                            const float k2 = d2 - m2 * m2;
                            const float k1 = d2 * m3 - m1 * m2 + m2 * rr * ra;
                            const float k0 = d2 * m5 - m1 * m1 + m1 * rr * ra * 2.0F - m0 * ra * ra;
                            const float hh = k1 * k1 - k0 * k2;
                            if (hh > 0.0F && (k2 < 0.0F ? -k2 : k2) > 1.0e-20F)
                            {
                                const float tcl = (-crd::math::sqrt(hh) - k1) / k2; // local to the shifted origin
                                const float tc  = tcl + tsh;                        // ...back to the true parameter
                                const float yc  = m1 - ra * rr + tcl * m2;          // y pairs with m1/m2, so LOCAL t
                                if (tc > tminu && tc < best && yc > 0.0F && yc < d2)
                                {
                                    best = tc;
                                    bu   = yc / (d2 > 1.0e-20F ? d2 : 1.0e-20F);
                                    bp   = static_cast<crd::u32>(sgi);
                                }
                            }
                            // the two end caps: without them a ray grazing a segment end misses entirely, which shows
                            // up as pinholes exactly at the joints between segments.
                            const float h1 = m3 * m3 - m5 + ra * ra;
                            const float h2 = m6 * m6 - m7 + rb * rb;
                            if (h1 > 0.0F)
                            {
                                const float t1 = -m3 - crd::math::sqrt(h1) + tsh;
                                if (t1 > tminu && t1 < best) { best = t1; bu = 0.0F; bp = static_cast<crd::u32>(sgi); }
                            }
                            if (h2 > 0.0F)
                            {
                                const float t2 = -m6 - crd::math::sqrt(h2) + tsh;
                                if (t2 > tminu && t2 < best) { best = t2; bu = 1.0F; bp = static_cast<crd::u32>(sgi); }
                            }
                        }
                        best = best * rinv; // back to RAY units
                        mat_pool[toff + static_cast<crd::usize>(active[a])] = static_cast<crd::f64>(best);
                        mat_pool[uoff + static_cast<crd::usize>(active[a])] = static_cast<crd::f64>(bu);
                        mat_pool[poff + static_cast<crd::usize>(active[a])] = static_cast<crd::f64>(bp);
                    }
                    ++i;
                    break;
                }
                case KStmtKind::TraceRayClosest:
                case KStmtKind::TraceRayHit:
                {                               // INLINE RAY QUERY oracle — the ground truth: brute-force watertight ray-triangle
                    const int  node    = st.result; // (Möller-Trumbore) over the AS's geometry, materialize the closest-hit t
                    const bool is_hit  = st.kind == KStmtKind::TraceRayHit; // ...and (TraceRayHit) the triangle index.
                    const int  pnode   = is_hit ? g.stmt_ext_operand(st, 8) : -1;
                    if (mat_off[static_cast<crd::usize>(node)] < 0)
                    {
                        mat_off[static_cast<crd::usize>(node)] = static_cast<crd::i32>(mat_pool.size());
                        for (crd::u32 t = 0; t < local_size; ++t) { mat_pool.push_back(0.0); }
                    }
                    if (is_hit && mat_off[static_cast<crd::usize>(pnode)] < 0)
                    {
                        mat_off[static_cast<crd::usize>(pnode)] = static_cast<crd::i32>(mat_pool.size());
                        for (crd::u32 t = 0; t < local_size; ++t) { mat_pool.push_back(0.0); }
                    }
                    const crd::usize    off  = static_cast<crd::usize>(mat_off[static_cast<crd::usize>(node)]);
                    const KernelBuffer* geo  = buffer_for(st.target); // the AS binding holds the triangle geometry in the oracle
                    // layout: data[0] = triangle count; then per triangle 9 floats (v0.xyz, v1.xyz, v2.xyz).
                    const int ntri = (geo != nullptr && geo->len > 0) ? static_cast<int>(geo->data[0]) : 0;
                    for (crd::usize a = 0; a < active.size(); ++a)
                    {
                        tid                  = active[a];
                        const crd::f64 ox    = eval(eval, g.stmt_ext_operand(st, 0));
                        const crd::f64 oy    = eval(eval, g.stmt_ext_operand(st, 1));
                        const crd::f64 oz    = eval(eval, g.stmt_ext_operand(st, 2));
                        const crd::f64 dx    = eval(eval, g.stmt_ext_operand(st, 3));
                        const crd::f64 dy    = eval(eval, g.stmt_ext_operand(st, 4));
                        const crd::f64 dz    = eval(eval, g.stmt_ext_operand(st, 5));
                        const crd::f64 tmin  = eval(eval, g.stmt_ext_operand(st, 6));
                        const crd::f64 tmax  = eval(eval, g.stmt_ext_operand(st, 7));
                        crd::f64       best  = tmax;
                        int            best_tri = -1; // 0xFFFFFFFF sentinel on miss
                        for (int tr = 0; tr < ntri; ++tr)
                        {
                            const int      b   = 1 + tr * 9;
                            const crd::f64 e1x = geo->data[b + 3] - geo->data[b + 0];
                            const crd::f64 e1y = geo->data[b + 4] - geo->data[b + 1];
                            const crd::f64 e1z = geo->data[b + 5] - geo->data[b + 2];
                            const crd::f64 e2x = geo->data[b + 6] - geo->data[b + 0];
                            const crd::f64 e2y = geo->data[b + 7] - geo->data[b + 1];
                            const crd::f64 e2z = geo->data[b + 8] - geo->data[b + 2];
                            const crd::f64 px = dy * e2z - dz * e2y; // dir x e2
                            const crd::f64 py = dz * e2x - dx * e2z;
                            const crd::f64 pz = dx * e2y - dy * e2x;
                            const crd::f64 det = e1x * px + e1y * py + e1z * pz;
                            if (det > -1.0e-12 && det < 1.0e-12) { continue; } // ray parallel to the triangle
                            const crd::f64 inv = 1.0 / det;
                            const crd::f64 tvx = ox - geo->data[b + 0];
                            const crd::f64 tvy = oy - geo->data[b + 1];
                            const crd::f64 tvz = oz - geo->data[b + 2];
                            const crd::f64 u   = (tvx * px + tvy * py + tvz * pz) * inv;
                            if (u < 0.0 || u > 1.0) { continue; }
                            const crd::f64 qx = tvy * e1z - tvz * e1y; // tvec x e1
                            const crd::f64 qy = tvz * e1x - tvx * e1z;
                            const crd::f64 qz = tvx * e1y - tvy * e1x;
                            const crd::f64 v  = (dx * qx + dy * qy + dz * qz) * inv;
                            if (v < 0.0 || u + v > 1.0) { continue; }
                            const crd::f64 t = (e2x * qx + e2y * qy + e2z * qz) * inv;
                            if (t > tmin && t < best) { best = t; best_tri = tr; }
                        }
                        mat_pool[off + static_cast<crd::usize>(active[a])] = best;
                        if (is_hit)
                        {
                            const crd::f64 prim = best_tri >= 0 ? static_cast<crd::f64>(best_tri) : 4294967295.0; // 0xFFFFFFFF on miss
                            mat_pool[static_cast<crd::usize>(mat_off[static_cast<crd::usize>(pnode)]) + static_cast<crd::usize>(active[a])] = prim;
                        }
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
                // ⛔ These four had NO case, and this switch has no `default` — so an unhandled kind fell through WITHOUT
                //    advancing `i`, meaning the oracle would spin forever rather than fail loudly. They are all RT-PIPELINE
                //    statements (raygen/hit/miss stages), which this compute-kernel oracle does not simulate: their gate is
                //    the GPU RT tests, not eval_cpu_kernel. Skipping is correct; hanging was not.
                case KStmtKind::TraceRayPipeline:
                case KStmtKind::PayloadStore:
                case KStmtKind::ReorderThread:
                case KStmtKind::IgnoreHitIf: ++i; break;
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
