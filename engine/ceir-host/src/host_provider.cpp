#include <crd/ceir/host/host_provider.hpp>

#include <crd/ceir/func.hpp> // resolve_call (pre-flight callee walk)
#include <crd/jobs/jobs.hpp> // crd::jobs::parallel_for / wait / Counter — the PRIVATE backend (never in a public header)
#include <crd/memory/allocators/malloc_allocator.hpp>

#include <utility> // std::move

namespace crd::ceir::host
{
using exec::ExecError;

namespace
{
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
                crd::memory::MallocAllocator item_scratch; // this index's OWN scratch — never the shared Context arena
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
    crd::memory::MallocAllocator  fold_scratch; // the fold's OWN scratch — never the shared Context arena or `in`'s frame
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

// ── parallel-purity pre-flight (submit thread) — the body + resolved callees must be StateEdge-free ──
struct PfResult
{
    ExecError        err = ExecError::None;
    const Operation* op  = nullptr;
};
PfResult preflight_region(const Context& ctx, const SymbolTable& syms, Region* r,
                          containers::HashMap<const Operation*, crd::u8>& visited)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.has_trait(op->kind(), OpTrait::StateEdge)) { return {ExecError::ParallelBodyStateful, op}; }
            if (ctx.op_name(op->kind()) == containers::StringView("func.call"))
            {
                Operation* const callee = func::resolve_call(ctx, op, syms);
                if (callee == nullptr) { return {ExecError::UnresolvedCall, op}; }
                if (!visited.contains(callee))
                {
                    visited.insert(callee, static_cast<crd::u8>(1));
                    const PfResult e = preflight_region(ctx, syms, callee->region(0), visited);
                    if (e.err != ExecError::None) { return e; }
                }
            }
            for (crd::u32 i = 0; i < op->num_regions(); ++i)
            {
                const PfResult e = preflight_region(ctx, syms, op->region(i), visited);
                if (e.err != ExecError::None) { return e; }
            }
        }
    }
    return {};
}
// A parallel region (a parallel_for/map_reduce MAP body, or a map_reduce COMBINE body) must: have a first block with
// EXACTLY `expect_args` block-args (so invoke_region's per-index / fold bind never trips BadArity mid-run — caught here on
// the submit thread instead), a Terminator yielding EXACTLY 1 value, and be StateEdge-free transitively (a cell would make
// the result depend on the range split). Offenses point at `owner` (arity/shape) or the precise inner op (stateful/call).
PfResult check_parallel_region(const Context& ctx, const SymbolTable& syms, Operation* owner, Region* r, crd::u32 expect_args)
{
    Block* const bb = (r != nullptr) ? r->first_block() : nullptr;
    if (bb == nullptr || bb->num_args() != expect_args) { return {ExecError::BadArity, owner}; }
    Operation* const term = bb->last_op();
    if (term == nullptr || !ctx.has_trait(term->kind(), OpTrait::Terminator) || term->num_operands() != 1U)
    {
        return {ExecError::ParallelYieldArity, owner};
    }
    containers::HashMap<const Operation*, crd::u8> visited(ctx.allocator());
    return preflight_region(ctx, syms, r, visited);
}
// Pre-flight every task.parallel_for AND task.map_reduce in the module: each parallel region (+ its resolved callees)
// StateEdge-free, correct block-arity, and yields exactly 1 — the {1..16} bit-identity precondition.
PfResult preflight(const Context& ctx, const Module& m)
{
    const SymbolTable* const syms = m.symbols();
    if (syms == nullptr) { return {}; }
    containers::Array<Operation*> ops(ctx.allocator());
    // collect all task.parallel_for + task.map_reduce ops (pre-order over the module body).
    // (a small local walk — the module is host-authored, not huge.)
    struct Walk
    {
        static void go(const Context& c, Region* r, containers::Array<Operation*>& out)
        {
            if (r == nullptr) { return; }
            for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
            {
                for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
                {
                    const containers::StringView n = c.op_name(op->kind());
                    if (n == containers::StringView("task.parallel_for") || n == containers::StringView("task.map_reduce"))
                    {
                        out.push_back(op);
                    }
                    for (crd::u32 i = 0; i < op->num_regions(); ++i) { go(c, op->region(i), out); }
                }
            }
        }
    };
    Walk::go(ctx, m.body(), ops);
    for (crd::u32 i = 0; i < static_cast<crd::u32>(ops.size()); ++i)
    {
        Operation* const op    = ops[i];
        const bool       is_mr = ctx.op_name(op->kind()) == containers::StringView("task.map_reduce");
        // MAP region (both ops): 1 block-arg (the induction var), yields 1, state-free.
        if (const PfResult e = check_parallel_region(ctx, *syms, op, op->region(0), 1U); e.err != ExecError::None)
        {
            return e;
        }
        if (is_mr)
        {
            // COMBINE region (map_reduce only): 2 block-args (acc, elem), yields 1, state-free.
            if (const PfResult e = check_parallel_region(ctx, *syms, op, op->region(1), 2U); e.err != ExecError::None)
            {
                return e;
            }
        }
    }
    return {};
}
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
    // 1. parallel-purity pre-flight on the SUBMIT thread (a parallel body must be state-free + yield exactly 1).
    if (const PfResult pf = preflight(ctx, m); pf.err != ExecError::None)
    {
        r.error = pf.err;
        r.op    = pf.op;
        return r;
    }
    // 2. one fully-installed PROTOTYPE (builtin + task + sequential async); each parallel range clones from it.
    exec::Interpreter proto(ctx);
    exec::install_builtin_semantics(proto);
    exec::install_async_semantics(proto); // §37 (so a scope / launch in an entry runs — sequential reference)
    proto.install(ctx.intern_op("task", "parallel_for"), &eval_parallel_for);
    proto.install(ctx.intern_op("task", "map_reduce"), &eval_map_reduce); // CEIR-6z: the in-IR fixed-order reduction
    proto.set_cancel_flag(&m_cancel); // §30 cooperative cancel — the top run + every clone observes it
    ParallelCtx pc{this, &m, &proto, &m_cancel, m_num_jobs, m_sub_fuel};
    proto.set_user(&pc);
    // 3. run the entry (task.parallel_for's EvalFn drives crd::jobs from inside).
    return proto.invoke(m, entry, args);
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
} // namespace crd::ceir::host
