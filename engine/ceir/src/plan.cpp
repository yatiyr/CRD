#include <crd/ceir/plan.hpp>

#include <crd/ceir/attr.hpp> // AttrValue / AttrKind
#include <crd/ceir/exec.hpp> // 4c: the SHARED parallel-region pre-flight (check_parallel_region — analysis, not execution)
#include <crd/ceir/func.hpp> // func_body_block / func kinds
#include <crd/ceir/symbol_table.hpp>
#include <crd/containers/hash_map.hpp>

#include <utility> // std::move

namespace crd::ceir::plan
{
containers::StringView compile_error_name(CompileError e) noexcept
{
    switch (e) // ⛔ no default (-Werror=switch)
    {
    case CompileError::Ok: return containers::StringView("ok");
    case CompileError::NoEntry: return containers::StringView("no-entry");
    case CompileError::NoModuleBody: return containers::StringView("no-module-body");
    case CompileError::UnsupportedOp: return containers::StringView("unsupported-op");
    case CompileError::BadPredicate: return containers::StringView("bad-predicate");
    case CompileError::BadConst: return containers::StringView("bad-const");
    case CompileError::ArityUnsupported: return containers::StringView("arity-unsupported");
    case CompileError::UnresolvedCall: return containers::StringView("unresolved-call");
    case CompileError::CallArity: return containers::StringView("call-arity");
    case CompileError::CapturedValue: return containers::StringView("captured-value");
    case CompileError::ParallelArity: return containers::StringView("parallel-arity");
    case CompileError::ParallelYield: return containers::StringView("parallel-yield");
    case CompileError::ParallelStateful: return containers::StringView("parallel-stateful");
    }
    return containers::StringView("?");
}
containers::StringView run_error_name(RunError e) noexcept
{
    switch (e) // ⛔ no default (-Werror=switch)
    {
    case RunError::None: return containers::StringView("none");
    case RunError::BadForStep: return containers::StringView("bad-for-step");
    case RunError::SelectorOutOfRange: return containers::StringView("selector-out-of-range");
    case RunError::CondArity: return containers::StringView("cond-arity");
    case RunError::FuelExhausted: return containers::StringView("fuel-exhausted");
    case RunError::BadToken: return containers::StringView("bad-token");
    case RunError::ContinuationArity: return containers::StringView("continuation-arity");
    }
    return containers::StringView("?");
}

namespace
{
bool parse_pred(containers::StringView s, CmpPred& out)
{
    if (s == containers::StringView("eq")) { out = CmpPred::Eq; return true; }
    if (s == containers::StringView("ne")) { out = CmpPred::Ne; return true; }
    if (s == containers::StringView("slt")) { out = CmpPred::Slt; return true; }
    if (s == containers::StringView("sle")) { out = CmpPred::Sle; return true; }
    if (s == containers::StringView("sgt")) { out = CmpPred::Sgt; return true; }
    if (s == containers::StringView("sge")) { out = CmpPred::Sge; return true; }
    if (s == containers::StringView("ult")) { out = CmpPred::Ult; return true; }
    if (s == containers::StringView("ule")) { out = CmpPred::Ule; return true; }
    if (s == containers::StringView("ugt")) { out = CmpPred::Ugt; return true; }
    if (s == containers::StringView("uge")) { out = CmpPred::Uge; return true; }
    return false;
}
bool eval_pred(CmpPred p, crd::i64 l, crd::i64 r) noexcept // INDEPENDENT of eval_cmpi (matches the spec)
{
    const auto ul = static_cast<crd::u64>(l);
    const auto ur = static_cast<crd::u64>(r);
    switch (p) // ⛔ no default (-Werror=switch)
    {
    case CmpPred::Eq: return l == r;
    case CmpPred::Ne: return l != r;
    case CmpPred::Slt: return l < r;
    case CmpPred::Sle: return l <= r;
    case CmpPred::Sgt: return l > r;
    case CmpPred::Sge: return l >= r;
    case CmpPred::Ult: return ul < ur;
    case CmpPred::Ule: return ul <= ur;
    case CmpPred::Ugt: return ul > ur;
    case CmpPred::Uge: return ul >= ur;
    }
    return false;
}

// ── the compiler (recursive; the Module is compile-time INPUT ONLY) ──
struct CC
{
    Context&                                     ctx;
    CompiledPlan&                                plan;
    containers::HashMap<const Value*, crd::u32>& slot; // ⛔ PER-FUNCTION (fresh map + next=0 each function; frame-relative)
    crd::u32&                                    next;
    memory::IAllocator*                          alloc;
    const SymbolTable*                           syms;     // 4a: resolve_call's table (PLAN-global)
    containers::HashMap<Operation*, crd::u32>&   fn_index; // 4a: func-op → compiled-fn index (PLAN-global work map)
    containers::Array<Operation*>&               worklist; // 4a: funcs discovered at calls, pending compilation
    OpId cst, addi, muli, cmpi, cif, cfor, cwhile, cswitch, cmatch, cscope, cyield, retk;
    OpId state, delay, history; // §20 cell ops (all → Op::State)
    OpId callk;                 // 4a: func.call → Op::Call
    // 4b async/task op kinds (SEQUENTIAL reference): launch-likes → Op::Launch, await-likes → Op::Await, etc.
    OpId alaunch, aawait, ajoin, arace, acancel, ascope; // async.*
    OpId tspawn, tmain, tworker, tgroup, tfiber, tcont;  // task.*
    OpId pfor, mreduce;         // 4c: task.parallel_for / task.map_reduce
    bool         isolated = false; // 4c: this CC compiles a map/combine body → a slot MISS is a capture, not an internal bug
    CompileError err      = CompileError::Ok;
};
crd::u32 slot_of(CC& cc, const Value* v)
{
    const crd::u32* const s = cc.slot.find(v);
    if (s == nullptr)
    {
        // in an ISOLATED map/combine body a miss is an outer CAPTURE (reference: runtime UndefinedValue — the §4 reject);
        // elsewhere it is an internal invariant break (a value used before its def — the verifier should have caught it).
        cc.err = cc.isolated ? CompileError::CapturedValue : CompileError::UnsupportedOp;
        return 0U;
    }
    return *s;
}
// 4c: map a shared-preflight ExecError → the compile-tier CompileError (an if-chain, NOT a switch — ExecError is huge and
// -Werror=switch would demand every arm). check_parallel_region only ever returns these four.
CompileError preflight_error(exec::ExecError e)
{
    if (e == exec::ExecError::BadArity) { return CompileError::ParallelArity; }
    if (e == exec::ExecError::ParallelYieldArity) { return CompileError::ParallelYield; }
    if (e == exec::ExecError::ParallelBodyStateful) { return CompileError::ParallelStateful; }
    if (e == exec::ExecError::UnresolvedCall) { return CompileError::UnresolvedCall; }
    return CompileError::UnsupportedOp;
}
crd::u32 fresh(CC& cc, const Value* v)
{
    const crd::u32 s = cc.next++;
    cc.slot.insert(v, s);
    return s;
}
// Build a CC over a FRESH per-function slot map + next (the opids are interned here — idempotent + cheap). `isolated`
// marks a 4c map/combine body (a slot miss → CapturedValue). ⛔ Reference members bind to the caller's locals — keep
// `slot`/`next` alive for the CC's lifetime.
CC make_cc(Context& ctx, CompiledPlan& plan, containers::HashMap<const Value*, crd::u32>& slot, crd::u32& next,
           memory::IAllocator* alloc, const SymbolTable* syms, containers::HashMap<Operation*, crd::u32>& fn_index,
           containers::Array<Operation*>& worklist, bool isolated)
{
    return CC{ctx,
              plan,
              slot,
              next,
              alloc,
              syms,
              fn_index,
              worklist,
              ctx.intern_op("arith", "const"),
              ctx.intern_op("arith", "addi"),
              ctx.intern_op("arith", "muli"),
              ctx.intern_op("arith", "cmpi"),
              ctx.intern_op("core", "if"),
              ctx.intern_op("core", "for"),
              ctx.intern_op("core", "while"),
              ctx.intern_op("core", "switch"),
              ctx.intern_op("core", "match"),
              ctx.intern_op("core", "scope"),
              ctx.intern_op("core", "yield"),
              func::return_kind(ctx),
              ctx.intern_op("core", "state"),
              ctx.intern_op("core", "delay"),
              ctx.intern_op("core", "history"),
              ctx.intern_op("func", "call"),
              ctx.intern_op("async", "launch"),
              ctx.intern_op("async", "await"),
              ctx.intern_op("async", "join"),
              ctx.intern_op("async", "race"),
              ctx.intern_op("async", "cancel"),
              ctx.intern_op("async", "scope"),
              ctx.intern_op("task", "spawn"),
              ctx.intern_op("task", "main_thread"),
              ctx.intern_op("task", "worker"),
              ctx.intern_op("task", "group"),
              ctx.intern_op("task", "fiber_wait"),
              ctx.intern_op("task", "continuation"),
              ctx.intern_op("task", "parallel_for"),
              ctx.intern_op("task", "map_reduce"),
              isolated};
}
crd::u32 compile_seq(CC& cc, Block* b);                             // fwd (mutual recursion with compile_fn_body)
crd::u32 compile_fn_body(CC& parent, Block* body, bool isolated);  // fwd — 4c: compile an ISOLATED map/combine body → a fn
// compile one block into a Seq (its ops → instrs, recursing into region children); returns the pushed seq index.
crd::u32 compile_seq(CC& cc, Block* b) // NOLINT(misc-no-recursion)
{
    Seq seq(cc.alloc);
    // §20 DEFERRED latches: a state cell's `next` is a FEEDBACK edge (defined LATER in this block), so its slot cannot be
    // resolved when the state op is compiled — we record {cell, next VALUE} and resolve the slot after the block compiles.
    containers::Array<crd::u32>    pend_cell(cc.alloc);
    containers::Array<const Value*> pend_next(cc.alloc);
    for (crd::u32 a = 0; a < b->num_args(); ++a) // block-arg slots (child blocks; the entry's are the param slots)
    {
        if (cc.slot.find(b->arg(a)) == nullptr) { (void)fresh(cc, b->arg(a)); }
        seq.arg_slots.push_back(slot_of(cc, b->arg(a))); // 4b: a continuation binds the antecedent token's yields here
    }
    for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
    {
        if (op->kind() == cc.cyield || op->kind() == cc.retk) // terminator → the seq's yield slots
        {
            for (crd::u32 i = 0; i < op->num_operands(); ++i) { seq.yield_slots.push_back(slot_of(cc, op->operand(i))); }
            if (cc.err != CompileError::Ok) { return 0U; }
            continue;
        }
        const bool is_scope = op->kind() == cc.cscope || op->kind() == cc.ascope || op->kind() == cc.tgroup;
        const bool is_cf = op->kind() == cc.cif || op->kind() == cc.cfor || op->kind() == cc.cwhile ||
                           op->kind() == cc.cswitch || op->kind() == cc.cmatch || is_scope;
        const bool is_arith =
            op->kind() == cc.cst || op->kind() == cc.addi || op->kind() == cc.muli || op->kind() == cc.cmpi;
        const bool is_state = op->kind() == cc.state || op->kind() == cc.delay || op->kind() == cc.history;
        const bool is_call  = op->kind() == cc.callk;
        // 4b async/task (all run in the CURRENT frame; launch/continuation carry a body region → children).
        const bool is_launch =
            op->kind() == cc.alaunch || op->kind() == cc.tspawn || op->kind() == cc.tmain || op->kind() == cc.tworker;
        const bool is_cont   = op->kind() == cc.tcont;
        const bool is_await  = op->kind() == cc.aawait || op->kind() == cc.tfiber;
        const bool is_join   = op->kind() == cc.ajoin;
        const bool is_race   = op->kind() == cc.arace;
        const bool is_cancel = op->kind() == cc.acancel;
        const bool is_async  = is_launch || is_cont || is_await || is_join || is_race || is_cancel;
        // 4c data-parallel: the map/combine bodies are ISOLATED mini-functions (NOT parent-frame children) — handled in the
        // op-id dispatch, so is_dp is deliberately OUT of the `is_cf||is_launch||is_cont` child-in-parent-frame set.
        const bool is_dp = op->kind() == cc.pfor || op->kind() == cc.mreduce;
        if (!is_cf && !is_arith && !is_state && !is_call && !is_async && !is_dp)
        {
            cc.err = CompileError::UnsupportedOp; // core.foreach etc. — no compiled semantics
            return 0U;
        }

        Instr instr{};
        if (is_cf || is_launch || is_cont) // ops that carry region children (control flow + launch/continuation bodies)
        {
            // ⛔ compile each region's first block → a child seq FIRST (into a local buffer), THEN reserve a CONTIGUOUS
            // block in child_pool. A child that itself contains NESTED control flow pushes ITS children to child_pool
            // during compilation, so capturing children_off BEFORE the loop would alias a nested op's child seqs (a
            // for-body containing a match → the For's child slot would point at the match's arm0). [The 5z corpus catch.]
            containers::Array<crd::u32> child_idx(cc.alloc);
            for (crd::u32 r = 0; r < op->num_regions(); ++r)
            {
                Block* const rb = op->region(r)->first_block();
                if (rb == nullptr) { cc.err = CompileError::UnsupportedOp; return 0U; }
                const crd::u32 child = compile_seq(cc, rb); // may push NESTED children to child_pool
                if (cc.err != CompileError::Ok) { return 0U; }
                child_idx.push_back(child);
            }
            instr.children_off = static_cast<crd::u32>(cc.plan.child_pool.size()); // NOW — after all nested pushes
            for (crd::u32 r = 0; r < static_cast<crd::u32>(child_idx.size()); ++r) { cc.plan.child_pool.push_back(child_idx[r]); }
            instr.children_cnt = op->num_regions();
        }
        // operands → slots. ⛔ a state op pushes ONLY operand(0) (the init, read at cell_read); operand(1) (`next`) is a
        // FEEDBACK edge defined LATER in the block → deferred to the latch (else slot_of would reject the forward ref).
        const crd::u32 n_ops = is_state ? 1U : op->num_operands();
        instr.operands_off   = static_cast<crd::u32>(cc.plan.operand_pool.size());
        for (crd::u32 i = 0; i < n_ops; ++i) { cc.plan.operand_pool.push_back(slot_of(cc, op->operand(i))); }
        if (cc.err != CompileError::Ok) { return 0U; }
        instr.operands_cnt = n_ops;
        // results → fresh slots.
        instr.results_off = static_cast<crd::u32>(cc.plan.result_pool.size());
        for (crd::u32 i = 0; i < op->num_results(); ++i) { cc.plan.result_pool.push_back(fresh(cc, op->result(i))); }
        instr.results_cnt = op->num_results();
        // dense op-id + the compiled immediate.
        if (op->kind() == cc.cst)
        {
            instr.op         = Op::ConstI;
            const AttrId vid = op->attr("value");
            if (!vid.valid()) { cc.err = CompileError::BadConst; return 0U; }
            const AttrValue v = cc.ctx.attr_value(vid);
            if (v.kind != AttrKind::Int) { cc.err = CompileError::BadConst; return 0U; }
            instr.imm = v.i;
        }
        else if (op->kind() == cc.addi) { instr.op = Op::AddI; }
        else if (op->kind() == cc.muli) { instr.op = Op::MulI; }
        else if (op->kind() == cc.cmpi)
        {
            instr.op         = Op::CmpI;
            const AttrId pid = op->attr("predicate");
            if (!pid.valid()) { cc.err = CompileError::BadPredicate; return 0U; }
            const AttrValue pv = cc.ctx.attr_value(pid);
            CmpPred         pred{};
            if (pv.kind != AttrKind::String || !parse_pred(pv.s, pred)) { cc.err = CompileError::BadPredicate; return 0U; }
            instr.imm = static_cast<crd::i64>(pred);
        }
        else if (op->kind() == cc.cif) { instr.op = Op::If; }
        else if (op->kind() == cc.cfor)
        {
            instr.op = Op::For;
            // the induction slot = the body block's arg(0) slot (assigned when its child seq was compiled).
            Block* const body = op->region(0)->first_block();
            instr.imm = (body != nullptr && body->num_args() >= 1U) ? static_cast<crd::i64>(slot_of(cc, body->arg(0)))
                                                                    : static_cast<crd::i64>(-1);
        }
        else if (op->kind() == cc.cwhile) { instr.op = Op::While; }
        else if (op->kind() == cc.cswitch || op->kind() == cc.cmatch) { instr.op = Op::Switch; }
        else if (is_scope) { instr.op = Op::Scope; }        // core.scope / async.scope / task.group — a structured boundary
        else if (is_launch) { instr.op = Op::Launch; }       // async.launch / task.spawn/main_thread/worker
        else if (is_await) { instr.op = Op::Await; }         // async.await / task.fiber_wait
        else if (is_cont) { instr.op = Op::Continuation; }   // task.continuation
        else if (is_join) { instr.op = Op::Join; }           // async.join
        else if (is_race) { instr.op = Op::Race; }           // async.race
        else if (is_cancel) { instr.op = Op::Cancel; }       // async.cancel
        else if (is_dp) // 4c task.parallel_for / map_reduce: shared preflight + ISOLATED body fns + a dense map-output index
        {
            const bool mr = op->kind() == cc.mreduce;
            instr.op      = mr ? Op::MapReduce : Op::ParallelFor;
            // ⭐ REUSE the shared legality pre-flight (analysis, NOT execution — region_state_free was moved to core for
            // exactly this): the map region has 1 block-arg + yields 1 + is state-free (calls resolved). map_reduce also
            // checks the combine region (2 args, yields 1, state-free). A failure → the mapped CompileError (a §4 reject).
            const exec::PreflightResult p0 = exec::check_parallel_region(cc.ctx, *cc.syms, op, op->region(0), 1U);
            if (p0.err != exec::ExecError::None) { cc.err = preflight_error(p0.err); return 0U; }
            const crd::u32 map_fn = compile_fn_body(cc, op->region(0)->first_block(), /*isolated=*/true);
            if (cc.err != CompileError::Ok) { return 0U; }
            crd::u32 comb_fn = 0U;
            if (mr)
            {
                const exec::PreflightResult p1 = exec::check_parallel_region(cc.ctx, *cc.syms, op, op->region(1), 2U);
                if (p1.err != exec::ExecError::None) { cc.err = preflight_error(p1.err); return 0U; }
                comb_fn = compile_fn_body(cc, op->region(1)->first_block(), /*isolated=*/true);
                if (cc.err != CompileError::Ok) { return 0U; }
            }
            // ⛔ child_pool here holds compiled-FUNCTION indices (the map fn, then the combine fn), NOT seq indices — the
            // ParallelFor/MapReduce thunks read them as fn indices (documented at the Instr field + both thunks).
            instr.children_off = static_cast<crd::u32>(cc.plan.child_pool.size());
            cc.plan.child_pool.push_back(map_fn);
            if (mr) { cc.plan.child_pool.push_back(comb_fn); }
            instr.children_cnt = mr ? 2U : 1U;
            instr.imm          = static_cast<crd::i64>(cc.plan.num_maps++); // the dense map-output index
        }
        else if (op->kind() == cc.callk) // 4a func.call: resolve the callee at COMPILE → a compiled-function index
        {
            instr.op                = Op::Call;
            Operation* const callee = func::resolve_call(cc.ctx, op, *cc.syms);
            if (callee == nullptr) { cc.err = CompileError::UnresolvedCall; return 0U; }
            Block* const ceb = func::func_body_block(callee);
            if (ceb == nullptr) { cc.err = CompileError::UnresolvedCall; return 0U; }
            // structural arity (both counts are static) → a COMPILE reject, not the reference's runtime BadArity (§4).
            if (op->num_operands() != ceb->num_args()) { cc.err = CompileError::CallArity; return 0U; }
            const crd::u32* const existing = cc.fn_index.find(callee);
            crd::u32              cidx      = 0U;
            if (existing != nullptr) { cidx = *existing; }
            else // first sighting of this callee: assign its index NOW (so a recursive call resolves) + enqueue its body
            {
                cidx = static_cast<crd::u32>(cc.plan.funcs.size());
                cc.fn_index.insert(callee, cidx);
                cc.plan.funcs.push_back(CompiledFn(cc.alloc)); // placeholder — filled when dequeued
                cc.worklist.push_back(callee);
            }
            instr.imm = static_cast<crd::i64>(cidx);
        }
        else // §20 state / delay / history: a dense cell index + a pre-baked latch (ring[pos]=next at seq end)
        {
            instr.op              = Op::State;
            const crd::u32 cell   = cc.plan.num_cells++;
            instr.imm             = static_cast<crd::i64>(cell);
            crd::u32       depth  = 1U; // §20 default
            const AttrId   did    = op->attr("depth");
            if (did.valid())
            {
                const AttrValue dv = cc.ctx.attr_value(did);
                if (dv.kind == AttrKind::Int && dv.i >= 1) { depth = static_cast<crd::u32>(dv.i); }
            }
            cc.plan.cell_depths.push_back(depth);
            // DEFER the latch: `next` = the LAST operand, a feedback edge (defined LATER in this block). Record it now;
            // resolve its slot AFTER the block compiles (read-all-then-latch: the latch fires at block END → `next` slotted).
            pend_cell.push_back(cell);
            pend_next.push_back(op->operand(op->num_operands() - 1U));
        }
        seq.instrs.push_back(instr);
    }
    // resolve the deferred §20 latches — every op in the block is now slotted, so each `next` feedback edge is defined.
    for (crd::u32 i = 0; i < static_cast<crd::u32>(pend_cell.size()); ++i)
    {
        seq.latches.push_back(Latch{pend_cell[i], slot_of(cc, pend_next[i])});
        if (cc.err != CompileError::Ok) { return 0U; }
    }
    cc.plan.seqs.push_back(std::move(seq));
    return static_cast<crd::u32>(cc.plan.seqs.size() - 1U);
}

// 4c: compile a map/combine `body` block as an ISOLATED mini-function → its fn index. A FRESH slot map means an outer
// CAPTURE fails `slot_of` at COMPILE (CapturedValue) — the §4 pattern (the reference errors on a capture-read at runtime).
// ⛔ Do NOT hold a `funcs`/`seqs` reference across the inner compile_seq — a nested call/dp op reallocs `funcs`; index fresh.
crd::u32 compile_fn_body(CC& parent, Block* body, bool isolated) // NOLINT(misc-no-recursion)
{
    containers::HashMap<const Value*, crd::u32> slot(parent.alloc); // ⛔ FRESH — captures are not in it → CapturedValue
    crd::u32                                    next = 0U;
    containers::Array<crd::u32>                 params(parent.alloc);
    const crd::u32                              fn_idx = static_cast<crd::u32>(parent.plan.funcs.size());
    parent.plan.funcs.push_back(CompiledFn(parent.alloc)); // placeholder — filled after compile_seq (funcs may realloc)
    for (crd::u32 a = 0; a < body->num_args(); ++a) // the body's block-args (iv; or acc, elem) → param slots
    {
        slot.insert(body->arg(a), next);
        params.push_back(next);
        ++next;
    }
    CC cc = make_cc(parent.ctx, parent.plan, slot, next, parent.alloc, parent.syms, parent.fn_index, parent.worklist,
                    isolated);
    const crd::u32 eseq = compile_seq(cc, body);
    if (cc.err != CompileError::Ok) { parent.err = cc.err; return 0U; }
    CompiledFn& fn = parent.plan.funcs[fn_idx]; // index FRESH — the placeholder may have moved during compile_seq
    fn.entry_seq   = eseq;
    fn.num_slots   = next;
    fn.param_slots = std::move(params);
    return fn_idx;
}

// ── the executor (recursive over child seqs; §153 dense) ──
// A §20 cell's per-RUN ring (dense-indexed; plan-owned depth). ⛔ Replaces the reference's HashMap<Operation*, Cell>.
struct Ring
{
    containers::Array<crd::i64> ring;
    crd::u32                    pos  = 0U;
    bool                        init = false; // filled on the first cell_read
    explicit Ring(memory::IAllocator* a) : ring(a) {}
};
struct RS
{
    const CompiledPlan&          plan;
    containers::Array<crd::i64>& stack; // the growing FRAME STACK — a slot is `stack[base + slot]` (4a frame windows)
    containers::Array<Ring>&     cells;
    // 4b: the run-global TOKEN STORE (⛔ NOT per-frame — a token launched in a callee is awaited in the caller). A token IS
    // an index into this; `store_token` appends + returns the handle (mirrors the reference `store_yields`).
    containers::Array<containers::Array<crd::i64>>& tokens;
    // 4c: the §118 per-index MAP OUTPUTS (dense map index; pre-sized to num_maps, never grown at run — inner arrays fill).
    containers::Array<containers::Array<crd::i64>>& map_outputs;
    memory::IAllocator*          alloc;
    crd::u64                     fuel;  // a hang-guard (a runaway loop → FuelExhausted)
    RunHooks                     hooks; // CEIR-11c: null-default profiling seam (one predicted branch per instr when unset)
};
// store one token = a copy of `child`'s yielded slot values → a new handle (the reference's store_yields, independently).
crd::u32 store_token(RS& rs, const Seq& child, crd::u32 base)
{
    containers::Array<crd::i64> t(rs.alloc);
    for (crd::u32 i = 0; i < static_cast<crd::u32>(child.yield_slots.size()); ++i)
    {
        t.push_back(rs.stack[base + child.yield_slots[i]]);
    }
    rs.tokens.push_back(std::move(t));
    return static_cast<crd::u32>(rs.tokens.size() - 1U);
}
RunError run_seq(RS& rs, crd::u32 seq_idx, crd::u32 base); // fwd — `base` = the current frame's slot-stack offset
// 4c: run an isolated map/combine fn once in a fresh frame WINDOW (bind `args` → param slots, run, read its single yield
// → `out`, pop). Mirrors the reference sub-interpreter's invoke_region (fresh env; captures were rejected at COMPILE).
RunError run_body(RS& rs, crd::u32 fn_idx, const crd::i64* args, crd::u32 argc, crd::i64& out) // NOLINT(misc-no-recursion)
{
    const CompiledFn& fn       = rs.plan.funcs[fn_idx]; // a ref into the const PLAN — stable across the run
    const crd::u32     new_base = static_cast<crd::u32>(rs.stack.size());
    rs.stack.resize(new_base + fn.num_slots); // grow the body frame (zero-filled); ⛔ may realloc — index only after
    for (crd::u32 i = 0; i < argc && i < static_cast<crd::u32>(fn.param_slots.size()); ++i)
    {
        rs.stack[new_base + fn.param_slots[i]] = args[i];
    }
    if (const RunError e = run_seq(rs, fn.entry_seq, new_base); e != RunError::None) { return e; }
    const Seq& cs = rs.plan.seqs[fn.entry_seq];
    out           = (cs.yield_slots.size() >= 1U) ? rs.stack[new_base + cs.yield_slots[0]] : 0; // preflight guarantees 1
    rs.stack.resize(new_base); // pop the body frame (capacity retained — amortized arena)
    return RunError::None;
}
// 4c: give a parallel body a FRESH per-phase token store — the reference sub isolates m_yield_store as well as the env
// (exec.cpp: fresh "like cells/yields — NOT copied", one sub per phase, discarded). ⛔ Without this a launch/await inside
// a parallel body would see the run-global store: a body `await(parent-handle)` would WRONGLY resolve (reference:
// BadToken on the empty sub store), and a body-launched handle would leak the parent's numbering. RAII so an error-return
// restores. Numbering inside starts at 0 (the sub); the body's tokens are DISCARDED at scope end (nothing migrates back).
struct TokenScope
{
    RS&                                            rs;
    containers::Array<containers::Array<crd::i64>> saved;
    explicit TokenScope(RS& r) : rs(r), saved(r.alloc)
    {
        for (crd::u32 i = 0; i < static_cast<crd::u32>(rs.tokens.size()); ++i) { saved.push_back(std::move(rs.tokens[i])); }
        rs.tokens.clear(); // the body sees an EMPTY store, numbering from 0
    }
    TokenScope(const TokenScope&)            = delete;
    TokenScope& operator=(const TokenScope&) = delete;
    ~TokenScope()
    {
        rs.tokens.clear(); // discard the body's tokens (the sub is discarded — nothing migrates back)
        for (crd::u32 i = 0; i < static_cast<crd::u32>(saved.size()); ++i) { rs.tokens.push_back(std::move(saved[i])); }
    }
};
// forward a child seq's yield slots into `dst` result slots (if / scope) — child shares the parent's frame `base`.
RunError run_forward(RS& rs, crd::u32 child_seq, const crd::u32* results, crd::u32 results_cnt, crd::u32 base)
{
    const RunError e = run_seq(rs, child_seq, base);
    if (e != RunError::None) { return e; }
    const Seq& cs = rs.plan.seqs[child_seq];
    for (crd::u32 j = 0; j < results_cnt && j < static_cast<crd::u32>(cs.yield_slots.size()); ++j)
    {
        rs.stack[base + results[j]] = rs.stack[base + cs.yield_slots[j]];
    }
    return RunError::None;
}
RunError run_seq(RS& rs, crd::u32 seq_idx, crd::u32 base) // NOLINT(misc-no-recursion)
{
    const Seq&            seq  = rs.plan.seqs[seq_idx];
    const crd::u32* const ops  = rs.plan.operand_pool.data();
    const crd::u32* const rez  = rs.plan.result_pool.data();
    const crd::u32* const kids = rs.plan.child_pool.data();
    for (crd::u32 k = 0; k < static_cast<crd::u32>(seq.instrs.size()); ++k)
    {
        if (rs.fuel == 0U) { return RunError::FuelExhausted; }
        --rs.fuel;
        const Instr&   in    = seq.instrs[k];
        const crd::u32 rslot = base + ((in.results_cnt > 0U) ? rez[in.results_off] : 0U);
        // CEIR-11c profiling seam: `pre` fires for EVERY dispatched instr (⛔ terminators/latches are NOT instrs — they
        // are captured at compile, so this counts exactly the dispatched Ops, matching the §112 semantics). Null-default →
        // one predicted branch. ⛔ receives only the dense op id (§153 — no Operation*).
        if (rs.hooks.pre != nullptr) { rs.hooks.pre(static_cast<crd::u8>(in.op), rs.hooks.user); }
        switch (in.op) // a compile-time jump table (§153); every slot is `stack[base + slot]` (the current frame window)
        {
        case Op::ConstI: rs.stack[rslot] = in.imm; break;
        case Op::AddI:
            rs.stack[rslot] = static_cast<crd::i64>(static_cast<crd::u64>(rs.stack[base + ops[in.operands_off]]) +
                                                    static_cast<crd::u64>(rs.stack[base + ops[in.operands_off + 1U]]));
            break;
        case Op::MulI:
            rs.stack[rslot] = static_cast<crd::i64>(static_cast<crd::u64>(rs.stack[base + ops[in.operands_off]]) *
                                                    static_cast<crd::u64>(rs.stack[base + ops[in.operands_off + 1U]]));
            break;
        case Op::CmpI:
            rs.stack[rslot] = eval_pred(static_cast<CmpPred>(in.imm), rs.stack[base + ops[in.operands_off]],
                                        rs.stack[base + ops[in.operands_off + 1U]])
                                  ? 1
                                  : 0;
            break;
        case Op::If:
        {
            const crd::i64 c     = rs.stack[base + ops[in.operands_off]];
            const crd::u32 child = kids[in.children_off + (c != 0 ? 0U : 1U)]; // nonzero → then (child 0)
            if (const RunError e = run_forward(rs, child, &rez[in.results_off], in.results_cnt, base);
                e != RunError::None)
            {
                return e;
            }
            break;
        }
        case Op::Scope:
            if (const RunError e = run_forward(rs, kids[in.children_off], &rez[in.results_off], in.results_cnt, base);
                e != RunError::None)
            {
                return e;
            }
            break;
        case Op::For:
        {
            const crd::i64 lo = rs.stack[base + ops[in.operands_off]];
            const crd::i64 hi = rs.stack[base + ops[in.operands_off + 1U]];
            const crd::i64 st = rs.stack[base + ops[in.operands_off + 2U]];
            if (st <= 0) { return RunError::BadForStep; }
            const crd::u32 body = kids[in.children_off];
            const crd::i64 ivslot = in.imm; // the body's induction slot (−1 if no arg)
            for (crd::i64 iv = lo; iv < hi; iv += st)
            {
                if (rs.fuel == 0U) { return RunError::FuelExhausted; }
                --rs.fuel;
                if (ivslot >= 0) { rs.stack[base + static_cast<crd::u32>(ivslot)] = iv; }
                if (const RunError e = run_seq(rs, body, base); e != RunError::None) { return e; }
            }
            break;
        }
        case Op::While:
        {
            const crd::u32 cond = kids[in.children_off];
            const crd::u32 body = kids[in.children_off + 1U];
            for (;;)
            {
                if (rs.fuel == 0U) { return RunError::FuelExhausted; }
                --rs.fuel;
                if (const RunError e = run_seq(rs, cond, base); e != RunError::None) { return e; }
                const Seq& cs = rs.plan.seqs[cond];
                if (cs.yield_slots.size() != 1U) { return RunError::CondArity; }
                if (rs.stack[base + cs.yield_slots[0]] == 0) { break; }
                if (const RunError e = run_seq(rs, body, base); e != RunError::None) { return e; }
            }
            break;
        }
        case Op::Switch:
        {
            const crd::i64 sel = rs.stack[base + ops[in.operands_off]];
            if (sel < 0 || sel >= static_cast<crd::i64>(in.children_cnt)) { return RunError::SelectorOutOfRange; }
            if (const RunError e = run_seq(rs, kids[in.children_off + static_cast<crd::u32>(sel)], base);
                e != RunError::None)
            {
                return e;
            }
            break;
        }
        case Op::State: // §20 cell_read: init-fill the ring on first read; result = ring[pos]
        {
            const crd::u32 cell = static_cast<crd::u32>(in.imm);
            Ring&          c    = rs.cells[cell];
            if (!c.init)
            {
                const crd::u32 depth = rs.plan.cell_depths[cell];
                const crd::i64 initv = rs.stack[base + ops[in.operands_off]]; // operand(0) = init
                for (crd::u32 d = 0; d < depth; ++d) { c.ring.push_back(initv); }
                c.pos  = 0U;
                c.init = true;
            }
            rs.stack[rslot] = c.ring[c.pos];
            break;
        }
        case Op::Call: // 4a: push a callee frame WINDOW, bind args, run, min-copy yields → results, pop
        {
            const CompiledFn& fn       = rs.plan.funcs[static_cast<crd::u32>(in.imm)];
            const crd::u32     new_base = static_cast<crd::u32>(rs.stack.size());
            rs.stack.resize(new_base + fn.num_slots); // grow the callee frame (zero-filled); ⛔ may realloc — index only
            // bind args: read caller frame (base + arg-slot), write callee frame (new_base + param-slot). LIFO ⇒ disjoint.
            for (crd::u32 i = 0; i < in.operands_cnt && i < static_cast<crd::u32>(fn.param_slots.size()); ++i)
            {
                rs.stack[new_base + fn.param_slots[i]] = rs.stack[base + ops[in.operands_off + i]];
            }
            if (const RunError e = run_seq(rs, fn.entry_seq, new_base); e != RunError::None) { return e; }
            // min-copy the callee's yields → the call's results (caller frame), read BEFORE popping. The COPY COUNT
            // matches the reference; the RESIDUE differs — the reference leaves extra results UNSET (an UndefinedValue
            // error if one is ever read), our zero-filled frame yields 0. ⛔ NOT an arity error (never reject what the
            // reference ran — it runs such a program fine when the extra result is never read; §4 + the stage-5 watch).
            const Seq& cs = rs.plan.seqs[fn.entry_seq];
            for (crd::u32 j = 0; j < in.results_cnt && j < static_cast<crd::u32>(cs.yield_slots.size()); ++j)
            {
                rs.stack[base + rez[in.results_off + j]] = rs.stack[new_base + cs.yield_slots[j]];
            }
            rs.stack.resize(new_base); // pop the callee frame (capacity retained — amortized arena)
            break;
        }
        case Op::Launch: // 4b: run the body NOW in the CURRENT frame (captures resolve), store its yields → a token handle
        {
            const crd::u32 body = kids[in.children_off];
            if (const RunError e = run_seq(rs, body, base); e != RunError::None) { return e; }
            rs.stack[rslot] = static_cast<crd::i64>(store_token(rs, rs.plan.seqs[body], base));
            break;
        }
        case Op::Await: // 4b: op0 = token; retrieve its stored yields → results (min-copy). A bad handle → BadToken.
        {
            const crd::i64 tok = rs.stack[base + ops[in.operands_off]];
            if (tok < 0 || static_cast<crd::u32>(tok) >= static_cast<crd::u32>(rs.tokens.size()))
            { // ⛔ truncate to u32 EXACTLY like the reference `valid_yield_handle(static_cast<u32>(tok))` (a 2^32+ handle
                return RunError::BadToken; //  wraps identically in both tiers — no divergence even on unreachable inputs).
            }
            const containers::Array<crd::i64>& ys = rs.tokens[static_cast<crd::u32>(tok)];
            for (crd::u32 j = 0; j < in.results_cnt && j < static_cast<crd::u32>(ys.size()); ++j)
            {
                rs.stack[base + rez[in.results_off + j]] = ys[j];
            }
            break;
        }
        case Op::Continuation: // 4b: read the antecedent token, bind ITS yields as the body's args, run, store → new token
        {
            const crd::i64 tok = rs.stack[base + ops[in.operands_off]];
            if (tok < 0 || static_cast<crd::u32>(tok) >= static_cast<crd::u32>(rs.tokens.size()))
            { // ⛔ truncate to u32 EXACTLY like the reference `valid_yield_handle(static_cast<u32>(tok))` (a 2^32+ handle
                return RunError::BadToken; //  wraps identically in both tiers — no divergence even on unreachable inputs).
            }
            const crd::u32 body = kids[in.children_off];
            const Seq&     bs   = rs.plan.seqs[body];
            // ⛔ COPY the antecedent's yields to a local BEFORE running the body — the body may store_token, reallocating
            // the token store out from under a span into it (the reference's exact hazard).
            containers::Array<crd::i64> ante(rs.alloc);
            {
                const containers::Array<crd::i64>& src = rs.tokens[static_cast<crd::u32>(tok)];
                for (crd::u32 i = 0; i < static_cast<crd::u32>(src.size()); ++i) { ante.push_back(src[i]); }
            }
            if (bs.arg_slots.size() != ante.size()) { return RunError::ContinuationArity; } // dynamic → runtime (BadArity)
            for (crd::u32 i = 0; i < static_cast<crd::u32>(bs.arg_slots.size()); ++i)
            {
                rs.stack[base + bs.arg_slots[i]] = ante[i];
            }
            if (const RunError e = run_seq(rs, body, base); e != RunError::None) { return e; }
            rs.stack[rslot] = static_cast<crd::i64>(store_token(rs, rs.plan.seqs[body], base));
            break;
        }
        case Op::Join: // 4b: concatenate every operand token's yields → one new token (all inputs already completed)
        {
            containers::Array<crd::i64> merged(rs.alloc);
            for (crd::u32 i = 0; i < in.operands_cnt; ++i)
            {
                const crd::i64 tok = rs.stack[base + ops[in.operands_off + i]];
                if (tok < 0 || static_cast<crd::u32>(tok) >= static_cast<crd::u32>(rs.tokens.size()))
                {
                    return RunError::BadToken; // ⛔ u32-truncate like the reference (identical wrap in both tiers)
                }
                const containers::Array<crd::i64>& ys = rs.tokens[static_cast<crd::u32>(tok)]; // store not grown in-loop
                for (crd::u32 w = 0; w < static_cast<crd::u32>(ys.size()); ++w) { merged.push_back(ys[w]); }
            }
            rs.tokens.push_back(std::move(merged)); // ONE store after reading all spans (the reference order)
            if (in.results_cnt > 0U) { rs.stack[rslot] = static_cast<crd::i64>(rs.tokens.size() - 1U); }
            break;
        }
        case Op::Race: // 4b: the sequential winner is index 0 (deterministic). ⛔ validates NO handle (the reference does
            if (in.results_cnt > 0U) { rs.stack[rslot] = 0; }             // value_of only — never valid_yield_handle here).
            break;
        case Op::Cancel: break; // 4b: consume op0, no-op (real cancellation is CEIR-6c). ⛔ validates NO handle.
        case Op::ParallelFor: // 4c: map the range → map_outputs[imm] (a statement — no SSA result). ⛔ imm = dense map index;
        {                     //   kids[children_off] = the map FN index (NOT a seq index).
            const crd::i64 lo = rs.stack[base + ops[in.operands_off]];
            const crd::i64 hi = rs.stack[base + ops[in.operands_off + 1U]];
            const crd::i64 st = rs.stack[base + ops[in.operands_off + 2U]];
            if (st <= 0) { return RunError::BadForStep; }
            const crd::u32 map_fn = kids[in.children_off];
            const crd::u32 mapidx = static_cast<crd::u32>(in.imm);
            const crd::u32 count  = (hi > lo) ? static_cast<crd::u32>((hi - lo + st - 1) / st) : 0U; // verbatim reference
            rs.map_outputs[mapidx].clear();
            const crd::u64 saved = rs.fuel; // ⛔ the reference gives the body a SEPARATE budget — restore so the parent is uncharged
            {
                TokenScope ts(rs); // ⛔ fresh per-phase token store (the reference sub's m_yield_store)
                for (crd::u32 i = 0; i < count; ++i)
                {
                    const crd::i64 iv     = lo + static_cast<crd::i64>(i) * st;
                    const crd::i64 arg[1] = {iv};
                    crd::i64       y      = 0;
                    if (const RunError e = run_body(rs, map_fn, arg, 1U, y); e != RunError::None) { return e; } // ts restores
                    rs.map_outputs[mapidx].push_back(y);
                }
            }
            rs.fuel = saved;
            break;
        }
        case Op::MapReduce: // 4c: map the range, then FOLD the combine fn over it in INDEX order; result = the reduced acc
        {
            const crd::i64 lo = rs.stack[base + ops[in.operands_off]];
            const crd::i64 hi = rs.stack[base + ops[in.operands_off + 1U]];
            const crd::i64 st = rs.stack[base + ops[in.operands_off + 2U]];
            if (st <= 0) { return RunError::BadForStep; }
            const crd::u32 map_fn  = kids[in.children_off];      // ⛔ FN indices, not seq indices
            const crd::u32 comb_fn = kids[in.children_off + 1U];
            const crd::u32 mapidx  = static_cast<crd::u32>(in.imm);
            const crd::u32 count   = (hi > lo) ? static_cast<crd::u32>((hi - lo + st - 1) / st) : 0U;
            rs.map_outputs[mapidx].clear();
            const crd::u64 saved = rs.fuel;
            {
                TokenScope ts(rs); // the MAP phase — fresh per-phase token store (the 1st sub)
                for (crd::u32 i = 0; i < count; ++i)
                {
                    const crd::i64 iv     = lo + static_cast<crd::i64>(i) * st;
                    const crd::i64 arg[1] = {iv};
                    crd::i64       y      = 0;
                    if (const RunError e = run_body(rs, map_fn, arg, 1U, y); e != RunError::None) { return e; }
                    rs.map_outputs[mapidx].push_back(y);
                }
            }
            rs.fuel      = saved;                                   // the fold gets its own fresh budget (the 2nd sub)
            crd::i64 acc = rs.stack[base + ops[in.operands_off + 3U]]; // operand(3) = the init
            const crd::u32 n = static_cast<crd::u32>(rs.map_outputs[mapidx].size());
            {
                TokenScope ts(rs); // the FOLD phase — ANOTHER fresh per-phase token store (the 2nd sub)
                for (crd::u32 i = 0; i < n; ++i) // SEQUENTIAL fold in INDEX order — (acc, elem) → acc (NON-associative witness)
                {
                    const crd::i64 ba[2] = {acc, rs.map_outputs[mapidx][i]};
                    crd::i64       y     = 0;
                    if (const RunError e = run_body(rs, comb_fn, ba, 2U, y); e != RunError::None) { return e; }
                    acc = y;
                }
            }
            rs.fuel         = saved; // parent fuel untouched by the body runs
            rs.stack[rslot] = acc;   // the reduced value is the SSA result
            break;
        }
        }
        // CEIR-11c: `post` fires only after a SUCCESSFUL dispatch — an erroring case `return`s above, so the pre/post
        // stream is UNBALANCED (pre>post) on an error; the consumer resets per run + tolerates it.
        if (rs.hooks.post != nullptr) { rs.hooks.post(static_cast<crd::u8>(in.op), rs.hooks.user); }
    }
    // §20: LATCH every cell read this block at block-eval end (read-all-then-latch — the pre-baked list, no trait scan).
    for (crd::u32 i = 0; i < static_cast<crd::u32>(seq.latches.size()); ++i)
    {
        const Latch& l = seq.latches[i];
        Ring&        c = rs.cells[l.cell];
        if (c.init) // only if read this block (the reference's cell_latch skips an unread cell)
        {
            c.ring[c.pos] = rs.stack[base + l.next_slot];
            c.pos         = (c.pos + 1U) % static_cast<crd::u32>(c.ring.size());
        }
    }
    return RunError::None;
}
} // namespace

CompileResult compile(Context& ctx, const Module& module, containers::StringView entry, memory::IAllocator* alloc)
{
    CompileResult res(alloc);
    const SymbolTable* const syms = module.symbols();
    if (syms == nullptr) { res.error = CompileError::NoEntry; return res; }
    const SymbolEntry* const se = syms->lookup(entry);
    if (se == nullptr || se->op == nullptr) { res.error = CompileError::NoEntry; return res; }
    if (func::func_body_block(se->op) == nullptr) { res.error = CompileError::NoModuleBody; return res; }

    // 4a: compile @entry + every callee reachable through func.call — a WORK-LIST over the call graph. The callee's index
    // is assigned at the call site (BEFORE its body compiles), so a recursive/mutually-recursive call resolves. ⛔ Each
    // function gets a FRESH frame-relative slot map (next=0); seqs/pools/cells/the fn-table are PLAN-global.
    containers::HashMap<Operation*, crd::u32> fn_index(alloc);
    containers::Array<Operation*>             worklist(alloc);
    res.plan.entry_fn = 0U;
    fn_index.insert(se->op, 0U);
    res.plan.funcs.push_back(CompiledFn(alloc)); // funcs[0] = @entry (placeholder, filled below)
    worklist.push_back(se->op);

    for (crd::u32 wi = 0; wi < static_cast<crd::u32>(worklist.size()); ++wi) // worklist grows as calls are discovered
    {
        Operation* const fn_op = worklist[wi];
        Block* const     fb    = func::func_body_block(fn_op);
        if (fb == nullptr) { res.error = CompileError::UnresolvedCall; return res; } // bodyless callee (entry checked above)

        containers::HashMap<const Value*, crd::u32> slot(alloc); // ⛔ FRESH per function — slots are frame-relative
        crd::u32                                    next = 0U;
        containers::Array<crd::u32>                 params(alloc);
        for (crd::u32 a = 0; a < fb->num_args(); ++a) // this function's param slots (bound from a call's args)
        {
            slot.insert(fb->arg(a), next);
            params.push_back(next);
            ++next;
        }
        CC cc = make_cc(ctx, res.plan, slot, next, alloc, syms, fn_index, worklist, /*isolated=*/false);
        const crd::u32 eseq = compile_seq(cc, fb);
        if (cc.err != CompileError::Ok) { res.error = cc.err; return res; }
        const crd::u32* const idx = fn_index.find(fn_op); // stable — assigned before this function was dequeued
        CompiledFn&           cf  = res.plan.funcs[*idx];
        cf.entry_seq   = eseq;
        cf.num_slots   = next;
        cf.param_slots = std::move(params);
    }
    // CEIR-11c: the plan-compile cost/shape stats (cheap — a scan over the already-built seqs; a perf bridge publishes them).
    res.stats.num_seqs  = static_cast<crd::u32>(res.plan.seqs.size());
    res.stats.num_funcs = static_cast<crd::u32>(res.plan.funcs.size());
    res.stats.num_cells = res.plan.num_cells;
    res.stats.num_maps  = res.plan.num_maps;
    for (crd::u32 s = 0; s < static_cast<crd::u32>(res.plan.seqs.size()); ++s)
    {
        res.stats.num_instrs += static_cast<crd::u32>(res.plan.seqs[s].instrs.size());
    }
    return res;
}

RunResult run(const CompiledPlan& plan, containers::ConstSpan<crd::i64> args, memory::IAllocator* alloc, RunHooks hooks)
{
    RunResult r(alloc);
    if (plan.entry_fn >= static_cast<crd::u32>(plan.funcs.size())) { return r; } // an empty/failed compile — no entry
    const CompiledFn& entry = plan.funcs[plan.entry_fn];
    // the @entry frame occupies base 0 of the slot stack; nested calls push windows above it (LIFO), pop on return.
    containers::Array<crd::i64> stack(alloc);
    stack.resize(entry.num_slots); // zero-filled
    for (crd::u32 i = 0; i < static_cast<crd::u32>(entry.param_slots.size()); ++i)
    {
        stack[entry.param_slots[i]] = (i < static_cast<crd::u32>(args.size())) ? args[i] : 0;
    }
    containers::Array<Ring> cells(alloc); // §20 per-run cell rings (dense; plan-owned depth; PER-OP GLOBAL, not per-frame)
    for (crd::u32 i = 0; i < plan.num_cells; ++i) { cells.push_back(Ring(alloc)); }
    containers::Array<containers::Array<crd::i64>> tokens(alloc); // 4b async/task token store (run-global, grows per launch)
    containers::Array<containers::Array<crd::i64>> map_outputs(alloc); // 4c: pre-sized to num_maps (dense index; never grown)
    for (crd::u32 i = 0; i < plan.num_maps; ++i) { map_outputs.push_back(containers::Array<crd::i64>(alloc)); }
    RS rs{plan, stack, cells, tokens, map_outputs, alloc, crd::u64{1} << 24U, hooks}; // reference's step budget + the 11c seam
    r.error = run_seq(rs, entry.entry_seq, 0U);
    // §118 inspection parity: the current value per cell (ring[pos] if read; 0 if never read — matches "never evaluated").
    for (crd::u32 i = 0; i < plan.num_cells; ++i) { r.cells.push_back(cells[i].init ? cells[i].ring[cells[i].pos] : 0); }
    if (r.error != RunError::None) { return r; }
    // 4c §118 parity: the per-index map per dense map index (the reference stores map_output per dp op on SUCCESS only).
    for (crd::u32 i = 0; i < plan.num_maps; ++i) { r.map_outputs.push_back(std::move(map_outputs[i])); }
    const Seq& es = plan.seqs[entry.entry_seq]; // the entry's yield = func.return operands (read at base 0)
    for (crd::u32 i = 0; i < static_cast<crd::u32>(es.yield_slots.size()); ++i) { r.values.push_back(stack[es.yield_slots[i]]); }
    return r;
}
} // namespace crd::ceir::plan
