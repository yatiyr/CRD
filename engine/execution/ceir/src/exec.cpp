#include <crd/ceir/exec.hpp>

#include <crd/ceir/func.hpp>
#include <crd/ceir/symbol_table.hpp>

#include <utility> // std::move

namespace crd::ceir::exec
{
containers::StringView exec_error_name(ExecError e) noexcept
{
    switch (e) // ⛔ no default: a new failure mode must be named here (a -Werror=switch compile error otherwise)
    {
    case ExecError::None: return containers::StringView("none");
    case ExecError::NoSemantics: return containers::StringView("no-semantics");
    case ExecError::UnresolvedCall: return containers::StringView("unresolved-call");
    case ExecError::UndefinedValue: return containers::StringView("undefined-value");
    case ExecError::BadForStep: return containers::StringView("bad-for-step");
    case ExecError::SelectorOutOfRange: return containers::StringView("selector-out-of-range");
    case ExecError::UnknownPredicate: return containers::StringView("unknown-predicate");
    case ExecError::CondArity: return containers::StringView("cond-arity");
    case ExecError::FuelExhausted: return containers::StringView("fuel-exhausted");
    case ExecError::NoEntry: return containers::StringView("no-entry");
    case ExecError::BadArity: return containers::StringView("bad-arity");
    case ExecError::ParallelBodyStateful: return containers::StringView("parallel-body-stateful");
    case ExecError::ParallelYieldArity: return containers::StringView("parallel-yield-arity");
    case ExecError::BadToken: return containers::StringView("bad-token");
    case ExecError::Cancelled: return containers::StringView("cancelled");
    }
    return containers::StringView("?");
}

Interpreter::Interpreter(Context& ctx, crd::u64 max_steps, memory::IAllocator* scratch)
    : m_ctx(ctx), m_scratch(scratch != nullptr ? scratch : ctx.allocator()), m_fuel(max_steps), m_sem(m_scratch),
      m_cells(m_scratch), m_yield_store(m_scratch), m_map_output(m_scratch)
{
}

Interpreter::Interpreter(const Interpreter& proto, memory::IAllocator* scratch, crd::u64 max_steps)
    : m_ctx(proto.m_ctx), m_scratch(scratch), m_fuel(max_steps), m_sem(scratch), m_cells(scratch), m_yield_store(scratch),
      m_map_output(scratch) // fresh (per-session, like cells/yields — NOT copied from proto)
{
    // copy the installed semantics (intern-free — the keys are already OpId.value); env / cells / fuel start FRESH.
    for (auto it = proto.m_sem.begin(); it != proto.m_sem.end(); ++it) { m_sem.insert(it.key(), it.value()); }
}

crd::u32 Interpreter::store_yields(containers::ConstSpan<crd::i64> ys)
{
    containers::Array<crd::i64> copy(m_scratch);
    for (crd::usize i = 0; i < ys.size(); ++i) { copy.push_back(ys[i]); }
    const auto handle = static_cast<crd::u32>(m_yield_store.size());
    m_yield_store.push_back(std::move(copy));
    return handle;
}

bool Interpreter::valid_yield_handle(crd::u32 handle) const noexcept
{
    return handle < static_cast<crd::u32>(m_yield_store.size());
}

containers::ConstSpan<crd::i64> Interpreter::stored_yields(crd::u32 handle) const noexcept
{
    if (handle >= static_cast<crd::u32>(m_yield_store.size())) { return {}; }
    const containers::Array<crd::i64>& ys = m_yield_store[handle];
    return containers::ConstSpan<crd::i64>(ys.data(), ys.size());
}

void Interpreter::store_map_output(const Operation* op, containers::Array<crd::i64>&& out)
{
    if (containers::Array<crd::i64>* const slot = m_map_output.find(op); slot != nullptr) { *slot = std::move(out); }
    else { m_map_output.insert(op, std::move(out)); }
}

containers::ConstSpan<crd::i64> Interpreter::map_output(const Operation* op) const noexcept
{
    const containers::Array<crd::i64>* const slot = m_map_output.find(op);
    if (slot == nullptr) { return {}; }
    return containers::ConstSpan<crd::i64>(slot->data(), slot->size());
}

void Interpreter::install(OpId kind, EvalFn fn)
{
    EvalFn* const existing = m_sem.find(kind.value);
    if (existing != nullptr) { *existing = fn; } // last install wins (open-world override)
    else { m_sem.insert(kind.value, fn); }
}

bool Interpreter::value_of(const Value* v, crd::i64& out) const noexcept
{
    if (m_env == nullptr) { return false; }
    const crd::i64* const p = m_env->find(v);
    if (p == nullptr) { return false; }
    out = *p;
    return true;
}

void Interpreter::set_value(const Value* v, crd::i64 x)
{
    crd::i64* const p = m_env->find(v); // UPSERT: HashMap::insert does not overwrite, but a loop re-binds the same Value*
    if (p != nullptr) { *p = x; }
    else { m_env->insert(v, x); }
}

bool Interpreter::spend_fuel() noexcept
{
    if (m_fuel == 0U) { return false; }
    --m_fuel;
    return true;
}

ExecError Interpreter::fail(ExecError e, const Operation* op) noexcept
{
    if (m_err == ExecError::None) // first offender wins (a nested failure keeps its more precise op)
    {
        m_err    = e;
        m_err_op = op;
    }
    return e;
}

crd::i64 Interpreter::cell_read(const Operation& state_op)
{
    Cell* c = m_cells.find(&state_op);
    if (c == nullptr) // first-ever evaluation: size the ring from `depth` (absent = 1) and init-fill it
    {
        crd::u32     depth = 1U;
        const AttrId d     = state_op.attr("depth");
        if (d.valid())
        {
            const AttrValue dv = m_ctx.attr_value(d);
            if (dv.kind == AttrKind::Int && dv.i >= 1) { depth = static_cast<crd::u32>(dv.i); }
        }
        crd::i64 init = 0; // the first operand; the 5d verifier guarantees it dominates, so this read succeeds
        (void)value_of(state_op.operand(0), init);
        Cell cell{containers::Array<crd::i64>(m_scratch), 0U};
        for (crd::u32 i = 0; i < depth; ++i) { cell.ring.push_back(init); }
        m_cells.insert(&state_op, std::move(cell));
        c = m_cells.find(&state_op);
    }
    return c->ring[c->pos];
}

ExecError Interpreter::cell_latch(const Operation& op)
{
    Cell* const c = m_cells.find(&op);
    if (c == nullptr) { return ExecError::None; } // not read this block (a conditional/untaken cell) ⇒ nothing to latch
    crd::i64 nx = 0;
    if (op.num_operands() == 0U || !value_of(op.operand(op.num_operands() - 1U), nx))
    {
        return fail(ExecError::UndefinedValue, &op);
    }
    c->ring[c->pos] = nx; // read-all-then-latch: env is fully populated, so cross-feeding cells latch simultaneously
    c->pos          = (c->pos + 1U) % static_cast<crd::u32>(c->ring.size());
    return ExecError::None;
}

bool Interpreter::cell_value(const Operation* state_op, crd::i64& out) const noexcept
{
    const Cell* const c = m_cells.find(state_op);
    if (c == nullptr) { return false; }
    out = c->ring[c->pos];
    return true;
}

namespace
{
// Collect every module-wide StateEdge op (pre-order, nested regions) — the ops a migration seeds. Mirrors the
// program_asset state walk, but returns OPS (not the schema) because restore keys m_cells by the op pointer.
void collect_state_ops(Context& c, Region* r, containers::Array<const Operation*>& out)
{
    if (r == nullptr) { return; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (c.has_trait(op->kind(), OpTrait::StateEdge) && op->num_results() >= 1U) { out.push_back(op); }
            for (crd::u32 i = 0; i < op->num_regions(); ++i) { collect_state_ops(c, op->region(i), out); }
        }
    }
}
// The §20 ring depth of a StateEdge op (the "depth" attr; absent / <1 ⇒ 1) — mirrors cell_read.
crd::u32 state_depth(Context& c, const Operation& op)
{
    crd::u32     depth = 1U;
    const AttrId d     = op.attr("depth");
    if (d.valid())
    {
        const AttrValue dv = c.attr_value(d);
        if (dv.kind == AttrKind::Int && dv.i >= 1) { depth = static_cast<crd::u32>(dv.i); }
    }
    return depth;
}
} // namespace

void Interpreter::snapshot_state_by_id(containers::Array<StateSnapshot>& out, memory::IAllocator* alloc) const
{
    for (auto it = m_cells.begin(); it != m_cells.end(); ++it)
    {
        const Operation* const op = it.key();
        const Cell&            cl = it.value();
        StateSnapshot          s(alloc);
        s.id  = op->stable_id().value;
        s.pos = cl.pos;
        for (crd::u32 i = 0; i < static_cast<crd::u32>(cl.ring.size()); ++i) { s.ring.push_back(cl.ring[i]); }
        out.push_back(std::move(s));
    }
}

crd::u32 Interpreter::restore_state_by_id(const Module& new_module, containers::ConstSpan<StateSnapshot> snap)
{
    m_ctx.assign_stable_ids(new_module); // the migration key must be valid on the new generation
    containers::Array<const Operation*> ops(m_scratch);
    collect_state_ops(m_ctx, new_module.body(), ops);
    crd::u32 restored = 0U;
    for (crd::u32 k = 0; k < static_cast<crd::u32>(ops.size()); ++k)
    {
        const Operation* const op    = ops[k];
        const crd::u64         id    = op->stable_id().value;
        const crd::u32         depth = state_depth(m_ctx, *op);
        for (crd::u32 s = 0; s < static_cast<crd::u32>(snap.size()); ++s)
        {
            if (snap[s].id != id) { continue; }
            // a matching depth (ring size) is required — else the new cell has a different §20 shape; skip → init-fill.
            if (static_cast<crd::u32>(snap[s].ring.size()) != depth) { break; }
            Cell cell{containers::Array<crd::i64>(m_scratch), snap[s].pos};
            for (crd::u32 i = 0; i < static_cast<crd::u32>(snap[s].ring.size()); ++i) { cell.ring.push_back(snap[s].ring[i]); }
            m_cells.insert(op, std::move(cell));
            ++restored;
            break;
        }
    }
    return restored;
}

ExecError Interpreter::eval_op(const Operation& op)
{
    EvalFn* const fn = m_sem.find(op.kind().value);
    if (fn == nullptr) { return fail(ExecError::NoSemantics, &op); }
    if (m_pre_hook != nullptr) { m_pre_hook(op, m_hook_user); } // §112: fires before EVERY dispatched op
    const ExecError e = (*fn)(*this, op);
    if (e == ExecError::None && m_post_hook != nullptr) { m_post_hook(op, m_hook_user); } // post: successful dispatch only
    return e;
}

ExecError Interpreter::eval_block(const Block& b)
{
    for (Operation* op = b.first_op(); op != nullptr; op = op->next_in_block()) // list order (4d hazards / 5d canonical)
    {
        if (cancelled()) { return fail(ExecError::Cancelled, op); }      // §30 cooperative cancel — checked before fuel
        if (!spend_fuel()) { return fail(ExecError::FuelExhausted, op); }
        const ExecError e = eval_op(*op);
        if (e != ExecError::None) { return e; }
    }
    // §20: LATCH every StateEdge cell at block-eval end — read-all-then-latch (env fully populated). Register timing.
    for (Operation* op = b.first_op(); op != nullptr; op = op->next_in_block())
    {
        if (m_ctx.has_trait(op->kind(), OpTrait::StateEdge))
        {
            const ExecError e = cell_latch(*op);
            if (e != ExecError::None) { return e; }
        }
    }
    return ExecError::None;
}

ExecError Interpreter::run_region(const Region& r, containers::Array<crd::i64>* out_yield)
{
    for (Block* b = r.first_block(); b != nullptr; b = b->next_in_region())
    {
        const ExecError e = eval_block(*b);
        if (e != ExecError::None) { return e; }
    }
    if (out_yield != nullptr) // the region's value = its terminator's (core.yield / func.return) operands
    {
        Block* const     lb   = r.last_block();
        Operation* const term = (lb != nullptr) ? lb->last_op() : nullptr;
        if (term != nullptr && m_ctx.has_trait(term->kind(), OpTrait::Terminator))
        {
            for (crd::u32 i = 0; i < term->num_operands(); ++i)
            {
                crd::i64 v = 0;
                if (!value_of(term->operand(i), v)) { return fail(ExecError::UndefinedValue, term); }
                out_yield->push_back(v);
            }
        }
    }
    return ExecError::None;
}

ExecError Interpreter::call(const Operation& call_op, containers::Array<crd::i64>& out_results)
{
    if (m_symbols == nullptr) { return fail(ExecError::UnresolvedCall, &call_op); }
    Operation* const callee = func::resolve_call(m_ctx, &call_op, *m_symbols);
    if (callee == nullptr) { return fail(ExecError::UnresolvedCall, &call_op); }
    Block* const eb = func::func_body_block(callee);
    if (eb == nullptr) { return fail(ExecError::UnresolvedCall, &call_op); }
    if (call_op.num_operands() != eb->num_args()) { return fail(ExecError::BadArity, &call_op); }
    // gather the argument values from the CURRENT frame BEFORE switching frames.
    containers::Array<crd::i64> argv(m_scratch);
    for (crd::u32 i = 0; i < call_op.num_operands(); ++i)
    {
        crd::i64 a = 0;
        if (!value_of(call_op.operand(i), a)) { return fail(ExecError::UndefinedValue, &call_op); }
        argv.push_back(a);
    }
    Env        frame(m_scratch); // a fresh frame — a func body is its own scope (supports recursion)
    Env* const saved = m_env;
    m_env            = &frame;
    for (crd::u32 i = 0; i < eb->num_args(); ++i) { set_value(eb->arg(i), argv[i]); }
    const ExecError e = run_region(*callee->region(0), &out_results);
    m_env             = saved; // restore the caller's frame
    return e;
}

ExecResult Interpreter::invoke(const Module& m, containers::StringView entry, containers::ConstSpan<crd::i64> args)
{
    ExecResult r(m_scratch);
    m_symbols = m.symbols();
    m_module  = &m; // the sequential parallel_for/map_reduce EvalFns invoke_region on a sub with THIS module
    m_err     = ExecError::None;
    m_err_op  = nullptr;
    if (m_symbols == nullptr)
    {
        r.error = ExecError::NoEntry;
        return r;
    }
    const SymbolEntry* const e = m_symbols->lookup(entry);
    if (e == nullptr || e->op == nullptr)
    {
        r.error = ExecError::NoEntry;
        return r;
    }
    Operation* const fn = e->op;
    Block* const     eb = func::func_body_block(fn);
    if (eb == nullptr)
    {
        r.error = ExecError::NoEntry;
        r.op    = fn;
        return r;
    }
    if (args.size() != eb->num_args())
    {
        r.error = ExecError::BadArity;
        r.op    = fn;
        return r;
    }
    Env frame(m_scratch);
    m_env = &frame;
    for (crd::u32 i = 0; i < eb->num_args(); ++i) { set_value(eb->arg(i), args[static_cast<crd::usize>(i)]); }
    const ExecError ee = run_region(*fn->region(0), &r.values);
    m_env              = nullptr;
    if (ee != ExecError::None)
    {
        r.error = ee;
        r.op    = m_err_op;
        r.values.clear();
    }
    return r;
}

ExecError Interpreter::invoke_region(const Module& m, const Region& body, containers::ConstSpan<crd::i64> block_args,
                                     containers::Array<crd::i64>& out_yield)
{
    m_symbols = m.symbols();
    m_module  = &m;
    m_err     = ExecError::None;
    m_err_op  = nullptr;
    Block* const eb = body.first_block();
    if (eb == nullptr) { return fail(ExecError::NoEntry, nullptr); }
    if (block_args.size() != eb->num_args()) { return fail(ExecError::BadArity, nullptr); }
    Env frame(m_scratch);
    m_env = &frame;
    for (crd::u32 i = 0; i < eb->num_args(); ++i) { set_value(eb->arg(i), block_args[static_cast<crd::usize>(i)]); }
    const ExecError e = run_region(body, &out_yield);
    m_env             = nullptr;
    return e;
}

// ── the built-in reference semantics ──
namespace
{
[[nodiscard]] ExecError bin_ops(Interpreter& in, const Operation& op, crd::i64& l, crd::i64& r)
{
    if (!in.value_of(op.operand(0), l)) { return in.fail(ExecError::UndefinedValue, &op); }
    if (!in.value_of(op.operand(1), r)) { return in.fail(ExecError::UndefinedValue, &op); }
    return ExecError::None;
}

ExecError eval_const(Interpreter& in, const Operation& op)
{
    const AttrId a = op.attr("value");
    if (!a.valid()) { return in.fail(ExecError::UndefinedValue, &op); }
    const AttrValue v = in.ctx().attr_value(a);
    if (v.kind != AttrKind::Int) { return in.fail(ExecError::UndefinedValue, &op); }
    in.set_value(op.result(0), v.i);
    return ExecError::None;
}
ExecError eval_addi(Interpreter& in, const Operation& op)
{
    crd::i64 l = 0;
    crd::i64 r = 0;
    if (const ExecError e = bin_ops(in, op, l, r); e != ExecError::None) { return e; }
    // ⛔ wrap through u64 — signed overflow is UB (don't hand gcc/asan a signed overflow). Reference = wrapping i64.
    in.set_value(op.result(0), static_cast<crd::i64>(static_cast<crd::u64>(l) + static_cast<crd::u64>(r)));
    return ExecError::None;
}
ExecError eval_muli(Interpreter& in, const Operation& op)
{
    crd::i64 l = 0;
    crd::i64 r = 0;
    if (const ExecError e = bin_ops(in, op, l, r); e != ExecError::None) { return e; }
    in.set_value(op.result(0), static_cast<crd::i64>(static_cast<crd::u64>(l) * static_cast<crd::u64>(r)));
    return ExecError::None;
}
ExecError eval_cmpi(Interpreter& in, const Operation& op)
{
    crd::i64 l = 0;
    crd::i64 r = 0;
    if (const ExecError e = bin_ops(in, op, l, r); e != ExecError::None) { return e; }
    const AttrId p = op.attr("predicate");
    if (!p.valid()) { return in.fail(ExecError::UnknownPredicate, &op); }
    const AttrValue pv = in.ctx().attr_value(p);
    if (pv.kind != AttrKind::String) { return in.fail(ExecError::UnknownPredicate, &op); }
    const containers::StringView s  = pv.s;
    const auto                   ul = static_cast<crd::u64>(l);
    const auto                   ur = static_cast<crd::u64>(r);
    bool                         res = false;
    if (s == containers::StringView("eq")) { res = l == r; }
    else if (s == containers::StringView("ne")) { res = l != r; }
    else if (s == containers::StringView("slt")) { res = l < r; }
    else if (s == containers::StringView("sle")) { res = l <= r; }
    else if (s == containers::StringView("sgt")) { res = l > r; }
    else if (s == containers::StringView("sge")) { res = l >= r; }
    else if (s == containers::StringView("ult")) { res = ul < ur; }
    else if (s == containers::StringView("ule")) { res = ul <= ur; }
    else if (s == containers::StringView("ugt")) { res = ul > ur; }
    else if (s == containers::StringView("uge")) { res = ul >= ur; }
    else { return in.fail(ExecError::UnknownPredicate, &op); }
    in.set_value(op.result(0), res ? 1 : 0);
    return ExecError::None;
}

// forward the taken/only region's yield to this op's (variadic) results.
[[nodiscard]] ExecError forward_yield(Interpreter& in, const Operation& op, const Region& reg)
{
    containers::Array<crd::i64> y(in.allocator());
    if (const ExecError e = in.run_region(reg, &y); e != ExecError::None) { return e; }
    for (crd::u32 j = 0; j < op.num_results() && j < static_cast<crd::u32>(y.size()); ++j)
    {
        in.set_value(op.result(j), y[static_cast<crd::usize>(j)]);
    }
    return ExecError::None;
}
ExecError eval_scope(Interpreter& in, const Operation& op) { return forward_yield(in, op, *op.region(0)); }
ExecError eval_if(Interpreter& in, const Operation& op)
{
    crd::i64 c = 0;
    if (!in.value_of(op.operand(0), c)) { return in.fail(ExecError::UndefinedValue, &op); }
    return forward_yield(in, op, *op.region(c != 0 ? 0U : 1U)); // nonzero selects THEN (region 0)
}
ExecError eval_for(Interpreter& in, const Operation& op)
{
    crd::i64 lo = 0;
    crd::i64 hi = 0;
    crd::i64 st = 0;
    if (!in.value_of(op.operand(0), lo)) { return in.fail(ExecError::UndefinedValue, &op); }
    if (!in.value_of(op.operand(1), hi)) { return in.fail(ExecError::UndefinedValue, &op); }
    if (!in.value_of(op.operand(2), st)) { return in.fail(ExecError::UndefinedValue, &op); }
    if (st <= 0) { return in.fail(ExecError::BadForStep, &op); } // a backwards / zero step never terminates
    const Region* const body = op.region(0);
    Block* const        eb   = body->first_block();
    for (crd::i64 iv = lo; iv < hi; iv += st)
    {
        if (in.cancelled()) { return in.fail(ExecError::Cancelled, &op); }
        if (!in.spend_fuel()) { return in.fail(ExecError::FuelExhausted, &op); }
        if (eb != nullptr && eb->num_args() >= 1U) { in.set_value(eb->arg(0), iv); } // bind the induction variable
        if (const ExecError e = in.run_region(*body, nullptr); e != ExecError::None) { return e; }
    }
    return ExecError::None;
}
ExecError eval_while(Interpreter& in, const Operation& op)
{
    const Region* const cond = op.region(0);
    const Region* const body = op.region(1);
    for (;;)
    {
        if (in.cancelled()) { return in.fail(ExecError::Cancelled, &op); }
        if (!in.spend_fuel()) { return in.fail(ExecError::FuelExhausted, &op); }
        containers::Array<crd::i64> cv(in.allocator());
        if (const ExecError e = in.run_region(*cond, &cv); e != ExecError::None) { return e; }
        if (cv.size() != 1U) { return in.fail(ExecError::CondArity, &op); } // the cond region yields exactly one bool
        if (cv[0] == 0) { break; }
        if (const ExecError e = in.run_region(*body, nullptr); e != ExecError::None) { return e; }
    }
    return ExecError::None;
}
ExecError eval_switch(Interpreter& in, const Operation& op) // match ≡ switch under reference behavior (selector = index)
{
    crd::i64 sel = 0;
    if (!in.value_of(op.operand(0), sel)) { return in.fail(ExecError::UndefinedValue, &op); }
    if (sel < 0 || sel >= static_cast<crd::i64>(op.num_regions())) { return in.fail(ExecError::SelectorOutOfRange, &op); }
    return in.run_region(*op.region(static_cast<crd::u32>(sel)), nullptr);
}
ExecError eval_state(Interpreter& in, const Operation& op) // core.state / delay / history — one register mechanism
{
    in.set_value(op.result(0), in.cell_read(op));
    return ExecError::None;
}
ExecError eval_call(Interpreter& in, const Operation& op)
{
    containers::Array<crd::i64> results(in.allocator());
    if (const ExecError e = in.call(op, results); e != ExecError::None) { return e; }
    for (crd::u32 j = 0; j < op.num_results() && j < static_cast<crd::u32>(results.size()); ++j)
    {
        in.set_value(op.result(j), results[static_cast<crd::usize>(j)]);
    }
    return ExecError::None;
}
// yield / func.return / func.func: a no-op at eval time — a terminator's operands are captured by run_region; func.func
// is never evaluated directly (its body is run by `call`).
ExecError eval_noop(Interpreter& /*in*/, const Operation& /*op*/) { return ExecError::None; }

// §37 async SEQUENTIAL reference (CEIR-6b). launch runs its body NOW, stashes the yields; the token's runtime value = the
// store handle. await retrieves them. join concatenates (all inputs already completed sequentially). race → index 0
// (the sequential "first ready" is deterministic). cancel consumes + no-ops (real cancellation is CEIR-6c).
ExecError eval_launch(Interpreter& in, const Operation& op)
{
    containers::Array<crd::i64> ys(in.allocator());
    if (const ExecError e = in.run_region(*op.region(0), &ys); e != ExecError::None) { return e; }
    const crd::u32 handle = in.store_yields(containers::ConstSpan<crd::i64>(ys.data(), ys.size()));
    in.set_value(op.result(0), static_cast<crd::i64>(handle)); // the token IS the store handle
    return ExecError::None;
}
ExecError eval_await(Interpreter& in, const Operation& op)
{
    crd::i64 tok = 0;
    if (!in.value_of(op.operand(0), tok)) { return in.fail(ExecError::UndefinedValue, &op); }
    if (tok < 0 || !in.valid_yield_handle(static_cast<crd::u32>(tok))) { return in.fail(ExecError::BadToken, &op); }
    const containers::ConstSpan<crd::i64> ys = in.stored_yields(static_cast<crd::u32>(tok));
    for (crd::u32 j = 0; j < op.num_results() && j < static_cast<crd::u32>(ys.size()); ++j)
    {
        in.set_value(op.result(j), ys[static_cast<crd::usize>(j)]);
    }
    return ExecError::None;
}
// CEIR-11a task.continuation: consume the antecedent token, bind ITS yields as the body's block-args, run the body, and
// store the body's yields as a NEW token. A hybrid of await (read antecedent) + launch (run body, store). ⛔ Read the
// antecedent's values into the env BEFORE run_region (the body may store_yields, growing the store under the span).
ExecError eval_continuation(Interpreter& in, const Operation& op)
{
    crd::i64 tok = 0;
    if (!in.value_of(op.operand(0), tok)) { return in.fail(ExecError::UndefinedValue, &op); }
    if (tok < 0 || !in.valid_yield_handle(static_cast<crd::u32>(tok))) { return in.fail(ExecError::BadToken, &op); }
    const containers::ConstSpan<crd::i64> vals = in.stored_yields(static_cast<crd::u32>(tok));
    const Region* const                   body = op.region(0);
    Block* const                          eb   = body->first_block();
    if (eb != nullptr) // bind the antecedent's yields as the body's block-args (arity-checked — the invoke_region contract)
    {
        if (eb->num_args() != static_cast<crd::u32>(vals.size())) { return in.fail(ExecError::BadArity, &op); }
        for (crd::u32 i = 0; i < eb->num_args(); ++i) { in.set_value(eb->arg(i), vals[static_cast<crd::usize>(i)]); }
    }
    containers::Array<crd::i64> ys(in.allocator());
    if (const ExecError e = in.run_region(*body, &ys); e != ExecError::None) { return e; }
    const crd::u32 handle = in.store_yields(containers::ConstSpan<crd::i64>(ys.data(), ys.size()));
    in.set_value(op.result(0), static_cast<crd::i64>(handle)); // the new token IS the store handle
    return ExecError::None;
}
ExecError eval_join(Interpreter& in, const Operation& op)
{
    containers::Array<crd::i64> merged(in.allocator());
    for (crd::u32 i = 0; i < op.num_operands(); ++i)
    {
        crd::i64 tok = 0;
        if (!in.value_of(op.operand(i), tok)) { return in.fail(ExecError::UndefinedValue, &op); }
        if (tok < 0 || !in.valid_yield_handle(static_cast<crd::u32>(tok))) { return in.fail(ExecError::BadToken, &op); }
        const containers::ConstSpan<crd::i64> ys = in.stored_yields(static_cast<crd::u32>(tok));
        for (crd::usize k = 0; k < ys.size(); ++k) { merged.push_back(ys[k]); }
    }
    in.set_value(op.result(0), static_cast<crd::i64>(in.store_yields(containers::ConstSpan<crd::i64>(merged.data(),
                                                                                                     merged.size()))));
    return ExecError::None;
}
ExecError eval_race(Interpreter& in, const Operation& op)
{
    for (crd::u32 i = 0; i < op.num_operands(); ++i)
    {
        crd::i64 t = 0;
        if (!in.value_of(op.operand(i), t)) { return in.fail(ExecError::UndefinedValue, &op); }
    }
    if (op.num_results() > 0U) { in.set_value(op.result(0), 0); } // the winning index (0, deterministically)
    return ExecError::None;
}
ExecError eval_cancel(Interpreter& in, const Operation& op)
{
    crd::i64 t = 0;
    if (!in.value_of(op.operand(0), t)) { return in.fail(ExecError::UndefinedValue, &op); }
    return ExecError::None; // consume + no-op; real cancellation is CEIR-6c
}

// ── the SEQUENTIAL reference for task.parallel_for / task.map_reduce (CEIR-11a — the §118 oracle) ──
// ⛔ The body runs on ONE fresh SUB-interpreter cloned from `in` (the 6z fold pattern): NOT run_region in `in`'s frame
// (outer captures would wrongly RESOLVE — the TOML pins them UndefinedValue) and NOT invoke_region on `in` (its frame is
// live). The sub gets `in`'s remaining fuel; captures are structurally UndefinedValue, matching the provider. Runs the
// map SEQUENTIALLY in index order — the SAME output as the provider's parallel run (index-order-disjoint slots), so the
// §118 differential compare is byte-identical WITHOUT sharing execution (only the pre-flight ANALYSIS is shared).
ExecError run_seq_map(Interpreter& in, const Operation& op, containers::Array<crd::i64>& out)
{
    crd::i64 lo   = 0;
    crd::i64 hi   = 0;
    crd::i64 step = 0;
    if (!in.value_of(op.operand(0), lo) || !in.value_of(op.operand(1), hi) || !in.value_of(op.operand(2), step))
    {
        return in.fail(ExecError::UndefinedValue, &op);
    }
    if (step <= 0) { return in.fail(ExecError::BadForStep, &op); }
    const Module* const m = in.module();
    if (m == nullptr || m->symbols() == nullptr) { return in.fail(ExecError::NoEntry, &op); }
    // legality AT the op (the SHARED core pre-flight — the map region: 1 block-arg, yields 1, state-free).
    if (const PreflightResult pf = check_parallel_region(in.ctx(), *m->symbols(), &op, op.region(0), 1U);
        pf.err != ExecError::None)
    {
        return in.fail(pf.err, pf.op);
    }
    const crd::u32 count = (hi > lo) ? static_cast<crd::u32>((hi - lo + step - 1) / step) : 0U; // == the provider's count
    for (crd::u32 i = 0; i < count; ++i) { out.push_back(0); }
    if (count == 0U) { return ExecError::None; } // empty range: an empty map (matches the provider's empty store)
    Interpreter sub(in, in.allocator(), in.fuel()); // ONE fresh sub (semantics copied; fresh env/cells/fuel)
    for (crd::u32 i = 0; i < count; ++i)
    {
        if (in.cancelled()) { return in.fail(ExecError::Cancelled, &op); } // per-index (eval_for granularity)
        const crd::i64              iv     = lo + static_cast<crd::i64>(i) * step;
        const crd::i64              iva[1] = {iv};
        containers::Array<crd::i64> yield(in.allocator());
        const ExecError e = sub.invoke_region(*m, *op.region(0), containers::ConstSpan<crd::i64>(iva, 1U), yield);
        if (e != ExecError::None) { return in.fail(e, &op); }
        out[i] = (yield.size() >= 1U) ? yield[0] : 0; // pre-flight guarantees yield == 1
    }
    return ExecError::None;
}
ExecError eval_parallel_for_seq(Interpreter& in, const Operation& op)
{
    containers::Array<crd::i64> out(in.allocator());
    if (const ExecError e = run_seq_map(in, op, out); e != ExecError::None) { return e; }
    in.store_map_output(&op, std::move(out)); // statement op — no SSA result; the map is the map_output inspection
    return ExecError::None;
}
ExecError eval_map_reduce_seq(Interpreter& in, const Operation& op)
{
    containers::Array<crd::i64> out(in.allocator());
    if (const ExecError e = run_seq_map(in, op, out); e != ExecError::None) { return e; } // region(0) = map
    const Module* const m = in.module(); // (run_seq_map already validated m + symbols)
    // legality of the COMBINE region (2 block-args, yields 1, state-free).
    if (const PreflightResult pf = check_parallel_region(in.ctx(), *m->symbols(), &op, op.region(1), 2U);
        pf.err != ExecError::None)
    {
        return in.fail(pf.err, pf.op);
    }
    crd::i64 acc = 0;
    if (!in.value_of(op.operand(3), acc)) { return in.fail(ExecError::UndefinedValue, &op); } // the init operand
    Interpreter sub(in, in.allocator(), in.fuel());
    for (crd::u32 i = 0; i < static_cast<crd::u32>(out.size()); ++i) // SEQUENTIAL fold in INDEX order (the fixed order)
    {
        const crd::i64              ba[2] = {acc, out[i]}; // (acc, elem) in index order
        containers::Array<crd::i64> yield(in.allocator());
        const ExecError e = sub.invoke_region(*m, *op.region(1), containers::ConstSpan<crd::i64>(ba, 2U), yield);
        if (e != ExecError::None) { return in.fail(e, &op); } // a fold-step error → the map_reduce op
        acc = (yield.size() >= 1U) ? yield[0] : 0;
    }
    in.set_value(op.result(0U), acc);           // expression op — the reduced value is its SSA result
    in.store_map_output(&op, std::move(out));   // §118 inspection parity (the intermediate map — the provider stores it too)
    return ExecError::None;
}
} // namespace

void install_arith_semantics(Interpreter& in)
{
    Context& c = in.ctx();
    in.install(c.intern_op("arith", "const"), &eval_const);
    in.install(c.intern_op("arith", "addi"), &eval_addi);
    in.install(c.intern_op("arith", "muli"), &eval_muli);
    in.install(c.intern_op("arith", "cmpi"), &eval_cmpi);
}
void install_core_semantics(Interpreter& in)
{
    Context& c = in.ctx();
    in.install(c.intern_op("core", "scope"), &eval_scope);
    in.install(c.intern_op("core", "if"), &eval_if);
    in.install(c.intern_op("core", "for"), &eval_for);
    in.install(c.intern_op("core", "while"), &eval_while);
    in.install(c.intern_op("core", "switch"), &eval_switch);
    in.install(c.intern_op("core", "match"), &eval_switch);
    in.install(c.intern_op("core", "yield"), &eval_noop);
    in.install(c.intern_op("core", "state"), &eval_state);
    in.install(c.intern_op("core", "delay"), &eval_state);
    in.install(c.intern_op("core", "history"), &eval_state);
    // ⛔ core.foreach is intentionally NOT installed — no collection value domain exists until types land (CEIR-6);
    //    a foreach in an executed program is a NAMED NoSemantics error, not a silent skip.
}
void install_func_semantics(Interpreter& in)
{
    Context& c = in.ctx();
    in.install(c.intern_op("func", "call"), &eval_call);
    in.install(c.intern_op("func", "return"), &eval_noop);
    in.install(c.intern_op("func", "func"), &eval_noop);
}
void install_builtin_semantics(Interpreter& in)
{
    install_arith_semantics(in);
    install_core_semantics(in);
    install_func_semantics(in);
}

void install_async_semantics(Interpreter& in) // §37 SEQUENTIAL reference (CEIR-6b/6c) — a SEPARATE installer
{
    Context& c = in.ctx();
    in.install(c.intern_op("async", "launch"), &eval_launch);
    in.install(c.intern_op("async", "await"), &eval_await);
    in.install(c.intern_op("async", "join"), &eval_join);
    in.install(c.intern_op("async", "race"), &eval_race);
    in.install(c.intern_op("async", "cancel"), &eval_cancel);
    // §30 structured-concurrency scope (CEIR-6c): the SEQUENTIAL reference is a no-op boundary — run the region + forward
    // its yield (everything launched inside already completed at launch). Same shape as core.scope.
    in.install(c.intern_op("async", "scope"), &eval_scope);
}

void install_task_semantics(Interpreter& in) // §38 host-task SEQUENTIAL reference (CEIR-11a) — the §118 oracle, host-neutral
{
    Context& c = in.ctx();
    // ⭐ Placement is value-unobservable in the oracle, so spawn/main_thread/worker SHARE launch's EvalFn (run the body at
    // the op, token = the yield-store handle) and group SHARES scope's (a no-op boundary forwarding the yield) — distinct
    // vocabulary, shared EvalFn (the state/delay/history + switch/match precedent). The jobs-backed placement is stage 3.
    in.install(c.intern_op("task", "spawn"), &eval_launch);
    in.install(c.intern_op("task", "main_thread"), &eval_launch);
    in.install(c.intern_op("task", "worker"), &eval_launch);
    in.install(c.intern_op("task", "group"), &eval_scope);
    in.install(c.intern_op("task", "fiber_wait"), &eval_await); // the host-level await (mirrors async.await)
    in.install(c.intern_op("task", "continuation"), &eval_continuation);
    // the data-parallel ops' SEQUENTIAL reference (the §118 oracle; the jobs-backed provider OVERRIDES these via
    // last-install-wins). ⛔ Requires the ENTRY to run under invoke() (so in.module() is set — the sub invoke_regions on it).
    in.install(c.intern_op("task", "parallel_for"), &eval_parallel_for_seq);
    in.install(c.intern_op("task", "map_reduce"), &eval_map_reduce_seq);
}

// ── the parallel-purity pre-flight (CEIR-11a — moved from crd-ceir-host; the 9d hoist-at-second-consumer) ──
namespace
{
// The body + its resolved callees must be StateEdge-free (a cell would make the result depend on the range split).
// `call_kind` is passed in (interned once by the caller) so this recursion never re-interns. Offenses point at the
// precise inner op (stateful / unresolved-call).
PreflightResult preflight_region_walk(const Context& ctx, const SymbolTable& syms, OpId call_kind, Region* r,
                                      containers::HashMap<const Operation*, crd::u8>& visited)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.has_trait(op->kind(), OpTrait::StateEdge)) { return {ExecError::ParallelBodyStateful, op}; }
            if (op->kind() == call_kind) // interned-id compare (was a bridge-era "func.call" string match)
            {
                Operation* const callee = func::resolve_call(ctx, op, syms);
                if (callee == nullptr) { return {ExecError::UnresolvedCall, op}; }
                if (!visited.contains(callee))
                {
                    visited.insert(callee, static_cast<crd::u8>(1));
                    const PreflightResult e = preflight_region_walk(ctx, syms, call_kind, callee->region(0), visited);
                    if (e.err != ExecError::None) { return e; }
                }
            }
            for (crd::u32 i = 0; i < op->num_regions(); ++i)
            {
                const PreflightResult e = preflight_region_walk(ctx, syms, call_kind, op->region(i), visited);
                if (e.err != ExecError::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

PreflightResult region_state_free(Context& ctx, const SymbolTable& syms, Region* r)
{
    containers::HashMap<const Operation*, crd::u8> visited(ctx.allocator());
    return preflight_region_walk(ctx, syms, func::call_kind(ctx), r, visited);
}

PreflightResult check_parallel_region(Context& ctx, const SymbolTable& syms, const Operation* owner, Region* r,
                                      crd::u32 expect_args)
{
    Block* const bb = (r != nullptr) ? r->first_block() : nullptr;
    if (bb == nullptr || bb->num_args() != expect_args) { return {ExecError::BadArity, owner}; }
    Operation* const term = bb->last_op();
    if (term == nullptr || !ctx.has_trait(term->kind(), OpTrait::Terminator) || term->num_operands() != 1U)
    {
        return {ExecError::ParallelYieldArity, owner};
    }
    return region_state_free(ctx, syms, r); // the StateEdge-free + calls-resolved transitive walk
}

PreflightResult preflight_parallel(Context& ctx, const Module& module)
{
    const SymbolTable* const syms = module.symbols();
    if (syms == nullptr) { return {}; }
    const OpId pf_kind = ctx.intern_op("task", "parallel_for");
    const OpId mr_kind = ctx.intern_op("task", "map_reduce");
    // collect every task.parallel_for + task.map_reduce (pre-order over the module body).
    containers::Array<Operation*> ops(ctx.allocator());
    struct Walk
    {
        static void go(Region* r, OpId pf, OpId mr, containers::Array<Operation*>& out)
        {
            if (r == nullptr) { return; }
            for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
            {
                for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
                {
                    if (op->kind() == pf || op->kind() == mr) { out.push_back(op); }
                    for (crd::u32 i = 0; i < op->num_regions(); ++i) { go(op->region(i), pf, mr, out); }
                }
            }
        }
    };
    Walk::go(module.body(), pf_kind, mr_kind, ops);
    for (crd::u32 i = 0; i < static_cast<crd::u32>(ops.size()); ++i)
    {
        Operation* const op = ops[i];
        // MAP region (both ops): 1 block-arg (the induction var), yields 1, state-free.
        if (const PreflightResult e = check_parallel_region(ctx, *syms, op, op->region(0), 1U); e.err != ExecError::None)
        {
            return e;
        }
        if (op->kind() == mr_kind) // COMBINE region (map_reduce only): 2 block-args (acc, elem), yields 1, state-free.
        {
            if (const PreflightResult e = check_parallel_region(ctx, *syms, op, op->region(1), 2U); e.err != ExecError::None)
            {
                return e;
            }
        }
    }
    return {};
}

containers::Array<crd::u8> pin_values(containers::ConstSpan<crd::i64> values, memory::IAllocator* alloc)
{
    containers::Array<crd::u8> out(alloc);
    for (crd::usize k = 0; k < values.size(); ++k)
    {
        const auto u = static_cast<crd::u64>(values[k]);
        for (crd::u32 b = 0; b < 8U; ++b) { out.push_back(static_cast<crd::u8>((u >> (8U * b)) & 0xFFU)); } // little-endian
    }
    return out;
}
} // namespace crd::ceir::exec
