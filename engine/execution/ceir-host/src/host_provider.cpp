#include <crd/ceir/host/host_provider.hpp>

#include <crd/ceir/func.hpp> // resolve_call (pre-flight callee walk)
#include <crd/jobs/jobs.hpp> // crd::jobs::parallel_for / wait / Counter — the PRIVATE backend (never in a public header)
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <new>     // placement new
#include <utility> // std::move

namespace crd::ceir::host
{
using exec::ExecError;

// CEIR-11a stage 3: one jobs-backed launch's state — its OWN allocator (the worker's sub-interpreter + the result buffer),
// the body's yields (filled by the worker BEFORE the counter decrements — the counter is the happens-before edge), the
// job context, and the completion counter. ⛔ HEAP-owned by the provider (the growable pooled table never moves it — the
// JobDecl captures &token by pointer; the push-back-UAF scar). A `GrowableTlsfAllocator` member ⇒ non-movable ⇒ pointer-owned.
struct PooledToken
{
    crd::memory::GrowableTlsfAllocator scratch;                 // this token's OWN allocator (worker sub + result)
    containers::Array<crd::i64>  result;                  // the body's yields (valid after the counter reaches 0)
    const exec::Interpreter*     proto    = nullptr;      // the worker clones its sub from this
    const Module*                module   = nullptr;
    const Region*                body     = nullptr;
    const std::atomic<bool>*     cancel   = nullptr;
    crd::u64                     sub_fuel = 0U;
    crd::jobs::Counter*          counter  = nullptr;
    ExecError                    err      = ExecError::None;
    bool                         waited   = false;        // resolve waits the counter exactly once
    PooledToken() : result(&scratch) {}
};

namespace
{
// The pool JOB (runs on a worker): clone a fresh sub from the proto over the token's OWN scratch, run the launch body
// (0 block-args), copy its yields into the token's result — all BEFORE returning (the counter decrements on return).
void run_launch(void* data)
{
    auto* const t = static_cast<PooledToken*>(data);
    exec::Interpreter sub(*t->proto, &t->scratch, t->sub_fuel);
    sub.set_cancel_flag(t->cancel);
    containers::Array<crd::i64> y(&t->scratch);
    const ExecError e = sub.invoke_region(*t->module, *t->body, containers::ConstSpan<crd::i64>(), y);
    if (e != ExecError::None) { t->err = e; }
    else
    {
        for (crd::u32 i = 0; i < static_cast<crd::u32>(y.size()); ++i) { t->result.push_back(y[i]); }
    }
}
// POOL-ELIGIBILITY (bridge-local — the launch classifier): the body is StateEdge-free + calls-resolved (reuse the core
// `exec::region_state_free`) AND has NO outer captures (every operand of every op is defined region-locally or is a
// block-arg). ⛔ Ineligible ⇒ the sequential IN-FRAME fallback (captures + state legally work there — stage 3 must never
// reject what the sequential reference ran). The no-captures walk is the genuinely NEW part (bridge-local; hoist at a 2nd consumer).
bool region_defines(Region* r, const Value* v); // fwd
bool block_defines(Block* b, const Value* v)
{
    for (crd::u32 a = 0; a < b->num_args(); ++a)
    {
        if (b->arg(a) == v) { return true; }
    }
    for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
    {
        for (crd::u32 rr = 0; rr < op->num_results(); ++rr)
        {
            if (op->result(rr) == v) { return true; }
        }
        for (crd::u32 i = 0; i < op->num_regions(); ++i)
        {
            if (region_defines(op->region(i), v)) { return true; }
        }
    }
    return false;
}
bool region_defines(Region* r, const Value* v)
{
    if (r == nullptr) { return false; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        if (block_defines(b, v)) { return true; }
    }
    return false;
}
bool no_outer_captures(Region* body)
{
    if (body == nullptr) { return true; }
    for (Block* b = body->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            for (crd::u32 i = 0; i < op->num_operands(); ++i)
            {
                if (!region_defines(body, op->operand(i))) { return false; } // an operand defined OUTSIDE the body = a capture
            }
        }
    }
    return true;
}
bool pool_eligible(Context& ctx, const SymbolTable& syms, Region* body)
{
    return exec::region_state_free(ctx, syms, body).err == ExecError::None && no_outer_captures(body);
}

// The SEQUENTIAL in-frame launch (the fallback — reimplements the core reference via public surface): run the body in
// `in`'s CURRENT frame (captures resolve), store the yields, token = a sequential yield-store handle.
ExecError launch_seq_inframe(exec::Interpreter& in, const Operation& op)
{
    containers::Array<crd::i64> ys(in.allocator());
    if (const ExecError e = in.run_region(*op.region(0), &ys); e != ExecError::None) { return e; }
    const crd::u32 handle = in.store_yields(containers::ConstSpan<crd::i64>(ys.data(), ys.size()));
    in.set_value(op.result(0), static_cast<crd::i64>(handle));
    return ExecError::None;
}
// The context the parallel_for EvalFn reads from Interpreter::user() (set by execute()).
struct ParallelCtx
{
    HostProvider*            self;
    const Module*            module;
    const exec::Interpreter* proto; // the fully-installed prototype (each range clones from it)
    const std::atomic<bool>* cancel; // §30 cooperative cancel flag, threaded into every sub-interpreter
    crd::u32                 num_jobs;
    crd::u64                 sub_fuel;
};

// The POD job context captured (by pointer) into the trivially-copyable parallel_for lambda.
struct RangeJob
{
    const exec::Interpreter* proto;
    const Module*            module;
    const Region*            body;
    const std::atomic<bool>* cancel; // §30 cooperative cancel flag, set on each sub-interpreter
    crd::i64                 lo;
    crd::i64                 step;
    crd::u64                 sub_fuel;
    crd::i64*                out;  // pre-sized [count]; each index writes its DISJOINT slot
    ExecError*               errs; // pre-sized [count], all None; each failing index writes its own slot (no atomics)
};

// Run op's MAP region (op.region(0)) in PARALLEL over [operand0, operand1) stepping operand2 — each index via a FRESH
// interpreter (from the prototype) over its OWN scratch, writing its yield to a DISJOINT index-order slot of `out`
// (caller pre-owns `out`, empty). Shared by task.parallel_for and task.map_reduce (both read operands 0..2 identically).
// Returns None (out filled) or the FIRST-in-index-order error (already recorded via in.fail). Empty range → out stays empty.
ExecError run_parallel_map(exec::Interpreter& in, const Operation& op, ParallelCtx* pc, containers::Array<crd::i64>& out)
{
    crd::i64 lo   = 0;
    crd::i64 hi   = 0;
    crd::i64 step = 0;
    if (!in.value_of(op.operand(0), lo) || !in.value_of(op.operand(1), hi) || !in.value_of(op.operand(2), step))
    {
        return in.fail(ExecError::UndefinedValue, &op);
    }
    if (step <= 0) { return in.fail(ExecError::BadForStep, &op); }

    const crd::u32 count = (hi > lo) ? static_cast<crd::u32>((hi - lo + step - 1) / step) : 0U;
    for (crd::u32 i = 0; i < count; ++i) { out.push_back(0); }
    if (count == 0U) { return ExecError::None; } // empty range: an empty map, no jobs
    containers::Array<ExecError> errs(pc->self->map_allocator());
    for (crd::u32 i = 0; i < count; ++i) { errs.push_back(ExecError::None); }

    // §32: the dispatch priority = the op region's RealtimeClass tag (audio/frame-critical → High, …).
    RegionExec re{};
    (void)in.ctx().op_region_exec(op, re);
    const crd::jobs::Priority prio = priority_for(re.realtime);

    RangeJob rj{pc->proto, pc->module, op.region(0), pc->cancel, lo, step, pc->sub_fuel, out.data(), errs.data()};
    crd::jobs::Counter* const counter = crd::jobs::parallel_for(
        count, pc->num_jobs,
        [rjp = &rj](crd::u32 begin, crd::u32 end) {
            for (crd::u32 idx = begin; idx < end; ++idx)
            {
                crd::memory::GrowableTlsfAllocator item_scratch; // this index's OWN scratch — never the shared Context arena
                exec::Interpreter           sub(*rjp->proto, &item_scratch, rjp->sub_fuel);
                sub.set_cancel_flag(rjp->cancel); // §30 cooperative cancel — ranges observe it in the step loop
                const crd::i64              iv     = rjp->lo + static_cast<crd::i64>(idx) * rjp->step;
                crd::i64                    iva[1] = {iv};
                containers::Array<crd::i64> yield(&item_scratch);
                const ExecError e = sub.invoke_region(*rjp->module, *rjp->body, containers::ConstSpan<crd::i64>(iva, 1U),
                                                       yield);
                if (e != ExecError::None) { rjp->errs[idx] = e; }
                else if (yield.size() >= 1U) { rjp->out[idx] = yield[0]; }
                else { rjp->errs[idx] = ExecError::ParallelYieldArity; } // (pre-flight makes this unreachable)
            }
        },
        crd::jobs::StackSize::Small, prio);
    crd::jobs::wait(counter);

    // first-in-index-order error (deterministic first-offender; disjoint per-index slots, no atomics).
    for (crd::u32 idx = 0; idx < count; ++idx)
    {
        if (errs[idx] != ExecError::None) { return in.fail(errs[idx], &op); }
    }
    return ExecError::None;
}

// task.parallel_for's reference semantics: run the map in parallel; stash the per-index yields on the provider (map_output).
ExecError eval_parallel_for(exec::Interpreter& in, const Operation& op)
{
    auto* const pc = static_cast<ParallelCtx*>(in.user());
    if (pc == nullptr) { return in.fail(ExecError::NoSemantics, &op); } // no parallel context (a nested/misconfigured pf)
    containers::Array<crd::i64> output(pc->self->map_allocator());
    if (const ExecError e = run_parallel_map(in, op, pc, output); e != ExecError::None) { return e; }
    pc->self->store_map(&op, std::move(output));
    return ExecError::None; // task.parallel_for is a statement op — no SSA results; the map is in map_output
}

// task.map_reduce's reference semantics (CEIR-6z): the PARALLEL map (region 0) then a SEQUENTIAL INDEX-ORDER fold
// (region 1) from init → the op's SSA result. ⛔ The fold runs on ONE fresh sub-interpreter cloned from the prototype —
// NOT a re-entrant invoke_region on `in`, which is mid-invoke (its frame is live). A plain i64 accumulator + a FIXED
// index order ⇒ the result is num_jobs-independent + bit-identical across {1..16} even for a NON-associative combine.
ExecError eval_map_reduce(exec::Interpreter& in, const Operation& op)
{
    auto* const pc = static_cast<ParallelCtx*>(in.user());
    if (pc == nullptr) { return in.fail(ExecError::NoSemantics, &op); }
    containers::Array<crd::i64> out(pc->self->map_allocator());
    if (const ExecError e = run_parallel_map(in, op, pc, out); e != ExecError::None) { return e; } // region(0) = map

    crd::i64 acc = 0;
    if (!in.value_of(op.operand(3), acc)) { return in.fail(ExecError::UndefinedValue, &op); } // the init operand

    const Region* const           combine = op.region(1);
    crd::memory::GrowableTlsfAllocator  fold_scratch; // the fold's OWN scratch — never the shared Context arena or `in`'s frame
    exec::Interpreter             fold(*pc->proto, &fold_scratch, pc->sub_fuel);
    fold.set_cancel_flag(pc->cancel); // §30: request_cancel also stops a long reduce
    for (crd::u32 i = 0; i < static_cast<crd::u32>(out.size()); ++i)
    {
        crd::i64                    ba[2] = {acc, out[i]}; // (acc, elem) — the combine reads its two block-args in INDEX order
        containers::Array<crd::i64> yield(&fold_scratch);
        const ExecError e = fold.invoke_region(*pc->module, *combine, containers::ConstSpan<crd::i64>(ba, 2U), yield);
        if (e != ExecError::None) { return in.fail(e, &op); } // a fold-step error is attributed to the map_reduce op
        if (yield.size() < 1U) { return in.fail(ExecError::ParallelYieldArity, &op); } // (pre-flight makes this unreachable)
        acc = yield[0];
    }
    pc->self->store_map(&op, std::move(out)); // §118 inspection parity — the intermediate map is free (already built)
    in.set_value(op.result(0U), acc);
    return ExecError::None; // task.map_reduce is an EXPRESSION op — the reduced value is its SSA result
}

// ── CEIR-11a stage 3: the jobs-backed launch/await ON-POOL EvalFns (override the sequential via last-install-wins) ──
// Resolve a token's yields: pooled ⇒ wait its counter (once) + read its result; sequential ⇒ the yield-store. ⛔ Routes by
// FULL i64 (kPoolBase) so a pooled handle never truncates into a sequential one. None ⇒ `out` valid; else the typed error.
ExecError resolve_yields(exec::Interpreter& in, HostProvider* self, crd::i64 tok, containers::ConstSpan<crd::i64>& out)
{
    if (self != nullptr && self->is_pooled(tok))
    {
        ExecError e = ExecError::None;
        if (!self->resolve_pooled(tok, out, e)) { return ExecError::BadToken; } // forged / out-of-range pooled handle
        return e;
    }
    if (tok < 0 || !in.valid_yield_handle(static_cast<crd::u32>(tok))) { return ExecError::BadToken; }
    out = in.stored_yields(static_cast<crd::u32>(tok));
    return ExecError::None;
}
bool token_valid(exec::Interpreter& in, HostProvider* self, crd::i64 tok) // validity WITHOUT waiting (race/cancel)
{
    if (self != nullptr && self->is_pooled(tok)) { return self->pooled_index_valid(tok); }
    return tok >= 0 && in.valid_yield_handle(static_cast<crd::u32>(tok));
}
// launch/spawn/worker/main_thread: pool the body if eligible (pure + self-contained), else the sequential in-frame
// fallback (captures + state legally work). `pin_thread`: -1 for spawn/worker (any), 0 for main_thread.
ExecError launch_pooled_impl(exec::Interpreter& in, const Operation& op, crd::i32 pin_thread)
{
    auto* const pc = static_cast<ParallelCtx*>(in.user());
    if (pc == nullptr) { return launch_seq_inframe(in, op); } // NESTED launch (a sub has no user) ⇒ sequential fallback
    const SymbolTable* const syms = pc->module->symbols();
    if (syms == nullptr || !pool_eligible(in.ctx(), *syms, op.region(0))) { return launch_seq_inframe(in, op); }
    RegionExec re{};
    (void)in.ctx().op_region_exec(op, re);
    const crd::i64 handle =
        pc->self->pool_launch(*pc->proto, *pc->module, op.region(0), pc->cancel, pc->sub_fuel, priority_for(re.realtime), pin_thread);
    in.set_value(op.result(0), handle);
    return ExecError::None;
}
ExecError eval_launch_pooled(exec::Interpreter& in, const Operation& op) { return launch_pooled_impl(in, op, -1); }
ExecError eval_main_thread_pooled(exec::Interpreter& in, const Operation& op) { return launch_pooled_impl(in, op, 0); }
// await/fiber_wait: resolve the token, bind the yields to the results.
ExecError eval_await_pooled(exec::Interpreter& in, const Operation& op)
{
    crd::i64 tok = 0;
    if (!in.value_of(op.operand(0), tok)) { return in.fail(ExecError::UndefinedValue, &op); }
    auto* const                     pc = static_cast<ParallelCtx*>(in.user());
    containers::ConstSpan<crd::i64> ys;
    if (const ExecError e = resolve_yields(in, pc != nullptr ? pc->self : nullptr, tok, ys); e != ExecError::None)
    {
        return in.fail(e, &op);
    }
    for (crd::u32 j = 0; j < op.num_results() && j < static_cast<crd::u32>(ys.size()); ++j)
    {
        in.set_value(op.result(j), ys[static_cast<crd::usize>(j)]);
    }
    return ExecError::None;
}
// join: resolve+concatenate every operand token's yields → ONE new SEQUENTIAL token (the result is eager).
ExecError eval_join_pooled(exec::Interpreter& in, const Operation& op)
{
    auto* const                 pc = static_cast<ParallelCtx*>(in.user());
    containers::Array<crd::i64> merged(in.allocator());
    for (crd::u32 i = 0; i < op.num_operands(); ++i)
    {
        crd::i64 tok = 0;
        if (!in.value_of(op.operand(i), tok)) { return in.fail(ExecError::UndefinedValue, &op); }
        containers::ConstSpan<crd::i64> ys;
        if (const ExecError e = resolve_yields(in, pc != nullptr ? pc->self : nullptr, tok, ys); e != ExecError::None)
        {
            return in.fail(e, &op);
        }
        for (crd::usize k = 0; k < ys.size(); ++k) { merged.push_back(ys[k]); }
    }
    in.set_value(op.result(0),
                 static_cast<crd::i64>(in.store_yields(containers::ConstSpan<crd::i64>(merged.data(), merged.size()))));
    return ExecError::None;
}
// race: validate every token (no wait), produce index 0 (deterministic — the Nondeterministic contract; real first-ready
// is 24/29). cancel: validate + consume (no-op — the pooled counter is drained at execute() exit; crd-jobs has no preempt).
ExecError eval_race_pooled(exec::Interpreter& in, const Operation& op)
{
    auto* const pc = static_cast<ParallelCtx*>(in.user());
    for (crd::u32 i = 0; i < op.num_operands(); ++i)
    {
        crd::i64 t = 0;
        if (!in.value_of(op.operand(i), t)) { return in.fail(ExecError::UndefinedValue, &op); }
        if (!token_valid(in, pc != nullptr ? pc->self : nullptr, t)) { return in.fail(ExecError::BadToken, &op); }
    }
    if (op.num_results() > 0U) { in.set_value(op.result(0), 0); }
    return ExecError::None;
}
ExecError eval_cancel_pooled(exec::Interpreter& in, const Operation& op)
{
    crd::i64    t  = 0;
    auto* const pc = static_cast<ParallelCtx*>(in.user());
    if (!in.value_of(op.operand(0), t)) { return in.fail(ExecError::UndefinedValue, &op); }
    if (!token_valid(in, pc != nullptr ? pc->self : nullptr, t)) { return in.fail(ExecError::BadToken, &op); }
    return ExecError::None; // consume + no-op (the counter is drained at execute() exit)
}

// ⛔ The parallel-purity PRE-FLIGHT moved to crd-ceir core at CEIR-11a stage 2b-ii (`exec::preflight_parallel` /
// `exec::check_parallel_region`) — the 9d hoist-at-second-consumer: the provider's PARALLEL submit-thread check and the
// reference's SEQUENTIAL run share ONE legality analysis (agree by construction). The provider delegates in `execute()`.
} // namespace

HostProvider::HostProvider(memory::IAllocator* alloc, crd::u32 num_jobs, crd::u64 sub_fuel)
    : m_alloc(alloc), m_num_jobs(num_jobs), m_sub_fuel(num_jobs == 0U ? 1U : sub_fuel), m_map(alloc)
{
    if (m_num_jobs == 0U) { m_num_jobs = 1U; }
}

containers::StringView HostProvider::name() const noexcept { return containers::StringView("host-jobs"); }

bool HostProvider::advertises(const Context& ctx, OpId k) const
{
    const containers::StringView n = ctx.op_name(k); // §69: the provider's distinctive capabilities (the task dialect)
    return n == containers::StringView("task.parallel_for") || n == containers::StringView("task.map_reduce");
}

void HostProvider::store_map(const Operation* pf_op, containers::Array<crd::i64>&& out)
{
    if (containers::Array<crd::i64>* const slot = m_map.find(pf_op); slot != nullptr) { *slot = std::move(out); }
    else { m_map.insert(pf_op, std::move(out)); }
}

containers::ConstSpan<crd::i64> HostProvider::map_output(const Operation* pf_op) const noexcept
{
    const containers::Array<crd::i64>* const slot = m_map.find(pf_op);
    if (slot == nullptr) { return {}; }
    return containers::ConstSpan<crd::i64>(slot->data(), slot->size());
}

exec::ExecResult HostProvider::execute(Context& ctx, const Module& m, containers::StringView entry,
                                       containers::ConstSpan<crd::i64> args)
{
    exec::ExecResult r(ctx.allocator());
    // 1. parallel-purity pre-flight on the SUBMIT thread (a parallel body must be state-free + yield exactly 1) — the
    // SHARED core analysis (CEIR-11a: the provider + the sequential reference agree on legality by construction).
    if (const exec::PreflightResult pf = exec::preflight_parallel(ctx, m); pf.err != ExecError::None)
    {
        r.error = pf.err;
        r.op    = pf.op;
        return r;
    }
    // 2. one fully-installed PROTOTYPE (builtin + task + sequential async); each parallel range clones from it.
    exec::Interpreter proto(ctx);
    exec::install_builtin_semantics(proto);
    exec::install_async_semantics(proto); // §37 (so a scope / launch in an entry runs — sequential reference)
    exec::install_task_semantics(proto);  // §38 host-task ops (so a task.spawn in an entry runs — sequential reference)
    // ⛔ then OVERRIDE parallel_for/map_reduce with the PARALLEL (jobs-backed) versions (last-install-wins — the seam).
    proto.install(ctx.intern_op("task", "parallel_for"), &eval_parallel_for);
    proto.install(ctx.intern_op("task", "map_reduce"), &eval_map_reduce); // CEIR-6z: the in-IR fixed-order reduction
    // CEIR-11a stage 3: OVERRIDE async launch/await/join/race/cancel + the launch-shaped task ops with the jobs-backed
    // ON-POOL versions. ⛔ ALL FIVE consumers overridden together — a pooled handle (> u32-max) would truncate + alias a
    // sequential handle in the un-overridden `u32(tok)` cast (a silent wrong read).
    proto.install(ctx.intern_op("async", "launch"), &eval_launch_pooled);
    proto.install(ctx.intern_op("async", "await"), &eval_await_pooled);
    proto.install(ctx.intern_op("async", "join"), &eval_join_pooled);
    proto.install(ctx.intern_op("async", "race"), &eval_race_pooled);
    proto.install(ctx.intern_op("async", "cancel"), &eval_cancel_pooled);
    proto.install(ctx.intern_op("task", "spawn"), &eval_launch_pooled);      // fork as a host job (any thread)
    proto.install(ctx.intern_op("task", "worker"), &eval_launch_pooled);     // worker-pool affinity (any thread)
    proto.install(ctx.intern_op("task", "main_thread"), &eval_main_thread_pooled); // pin_thread=0
    proto.install(ctx.intern_op("task", "fiber_wait"), &eval_await_pooled);  // the host-level await
    proto.set_cancel_flag(&m_cancel); // §30 cooperative cancel — the top run + every clone observes it
    ParallelCtx pc{this, &m, &proto, &m_cancel, m_num_jobs, m_sub_fuel};
    proto.set_user(&pc);
    // 3. run the entry (the task/async EvalFns drive crd::jobs from inside).
    exec::ExecResult res = proto.invoke(m, entry, args);
    drain_pooled(); // ⛔ wait + free every pooled token BEFORE returning (leak containment — a worker must not outlive execute)
    return res;
}

crd::jobs::Priority priority_for(RealtimeClass rc) noexcept
{
    switch (rc) // ⛔ no default: a new §32 execution class is a -Werror=switch compile error
    {
    case RealtimeClass::FrameCritical:
    case RealtimeClass::AudioRealTime: return crd::jobs::Priority::High;
    case RealtimeClass::Background:
    case RealtimeClass::Offline: return crd::jobs::Priority::Low;
    case RealtimeClass::Unspecified:
    case RealtimeClass::SimulationCritical:
    case RealtimeClass::LatencySensitive:
    case RealtimeClass::Throughput: return crd::jobs::Priority::Normal;
    }
    return crd::jobs::Priority::Normal; // unreachable (total switch)
}

crd::i64 HostProvider::pool_launch(const exec::Interpreter& proto, const Module& module, const Region* body,
                                   const std::atomic<bool>* cancel, crd::u64 sub_fuel, crd::jobs::Priority prio,
                                   crd::i32 pin_thread)
{
    void* const  mem = m_alloc->allocate(sizeof(PooledToken), alignof(PooledToken));
    auto* const  t   = new (mem) PooledToken(); // heap-owned (the table never moves it — the JobDecl captures &t)
    t->proto = &proto;
    t->module = &module;
    t->body   = body;
    t->cancel = cancel;
    t->sub_fuel = sub_fuel;
    const crd::i64 handle = kPoolBase + static_cast<crd::i64>(m_pooled.size());
    m_pooled.push_back(t);
    ++m_pooled_total; // the cumulative witness (survives the drain)
    crd::jobs::JobDecl jd{};
    jd.fn         = &run_launch;
    jd.data       = t;
    jd.pin_thread = pin_thread;
    jd.priority   = prio;
    t->counter    = crd::jobs::run(jd);
    return handle;
}

bool HostProvider::resolve_pooled(crd::i64 tok, containers::ConstSpan<crd::i64>& out, exec::ExecError& out_err) noexcept
{
    if (!pooled_index_valid(tok)) { return false; } // forged / out-of-range pooled handle
    PooledToken* const t = m_pooled[static_cast<crd::usize>(tok - kPoolBase)];
    if (!t->waited) // wait the completion counter exactly once — the happens-before edge to the worker's result write
    {
        crd::jobs::wait(t->counter);
        t->waited = true;
    }
    out_err = t->err;
    out     = containers::ConstSpan<crd::i64>(t->result.data(), t->result.size());
    return true;
}

void HostProvider::drain_pooled() noexcept
{
    // wait every outstanding counter (a leaked/un-awaited token — the provider runs no verifier) then free every entry.
    for (crd::usize i = 0; i < m_pooled.size(); ++i)
    {
        PooledToken* const t = m_pooled[i];
        if (!t->waited && t->counter != nullptr) { crd::jobs::wait(t->counter); }
        t->~PooledToken();
        m_alloc->deallocate(t);
    }
    m_pooled.clear();
}
} // namespace crd::ceir::host
