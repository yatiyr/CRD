#pragma once

// crd-ceir — CEIR-11b the COMPILED execution tier (§84 two-tier · §153 hot-path rules). A `CompiledPlan` is a Module
// COMPILED to dense instruction arrays: the Module is compile-time INPUT ONLY; the plan owns a tree of dense SEQUENCES
// (`Seq`) and the hot loop NEVER touches an `Operation*` (⛔ a runtime authoring-graph walk is itself a §153 violation).
// ⭐ §153-CLEAN: values are dense u32 SLOTS in a flat i64 array (no `Value*` map); dispatch is a dense op-id switch (a
// compile-time jump table, no `HashMap<OpId>` lookup); attrs are compile-time IMMEDIATES (no per-eval attr/string touch);
// control flow recurses into CHILD sequences by INDEX, never into Regions. ⛔ The compiled thunks are INDEPENDENT of the
// reference EvalFns (they share the TOML-pinned SPEC, never code — the differential's value IS that independence).
// Design: docs/design/ceir-11b-compiled-execution-plan.md. Stages: (2) straight-line arith ✅; (3a) control flow ✅;
// (3b) §20 state ✅; (4a) calls (frame windows) ✅; (4b) async/task (token store) ✅; (4c) data-parallel ← THIS. ⭐ CALLS: each function compiles to a
// `CompiledFn{entry_seq, num_slots, param_slots}` with FRAME-RELATIVE slots; `run` owns a growing slot STACK and a call
// pushes a WINDOW (base = stack size, grow by callee num_slots, pop after) — recursion-safe, no per-call heap; ⛔ NEVER
// hold a slot pointer/ref across a callee run (growth reallocs — index through the Array). Runs the SEQUENTIAL semantics
// (matching the §118 oracle); the §69 compile→plan provider interface is CEIR-21/26 (NOT named here).

#include <crd/ceir/context.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::ceir::plan
{
// The dense compiled op-id. Dispatch is a switch over THIS — a compile-time jump table, NOT a dynamic map lookup (§153).
// NOLINTNEXTLINE(performance-enum-size)
enum class Op : crd::u8
{
    ConstI, // result = imm
    AddI,   // result = wrapping(op0 + op1)
    MulI,   // result = wrapping(op0 * op1)
    CmpI,   // result = (op0 <pred> op1) ? 1 : 0 — pred is the imm (a CmpPred)
    // ── control flow (3a): children are CHILD SEQUENCE indices (into child_pool); each child Seq carries its yield slots ──
    If,     // op0 != 0 ? run children[0] (then) : children[1] (else); forward the child's yield → this instr's results
    For,    // for iv in [op0, op1) step op2 { run children[0], binding iv into the child's induction slot (imm) }; op2<=0 → BadForStep
    While,  // loop { run children[0] (cond, yields 1); if 0 break; run children[1] (body) }
    Switch, // sel = op0; sel out of [0, children_cnt) → SelectorOutOfRange; run children[sel] (statement)
    Scope,  // run children[0]; forward its yield → this instr's results (a structured boundary, always taken)
    // ── §20 state (3b): core.state/delay/history — a depth-N ring keyed by a dense CELL index (imm) ──
    State,  // cell_read: first eval init-fills the ring from op0 (init); result = ring[pos]. latch at SEQ (block) end.
    // ── calls (4a): func.call — the callee resolved at COMPILE to a compiled-function INDEX (imm); a call pushes a frame
    //    WINDOW onto the plan-owned slot stack (arena, no per-call heap; recursion-safe) ──
    Call,   // args = operands (caller frame); push callee frame; bind params; run callee entry_seq; min-copy yields → results
    // ── async/task (4b): §37/§38 SEQUENTIAL reference — a launch runs its body NOW (in the CURRENT frame; captures
    //    resolve), stashing yields in the run-global TOKEN STORE; a token IS the store handle. ⛔ Only Call pushes a frame. ──
    Launch,       // async.launch / task.spawn/main_thread/worker: run children[0] NOW; store its yields; result = handle
    Await,        // async.await / task.fiber_wait: op0 = token; bad handle → BadToken; min-copy stored yields → results
    Continuation, // task.continuation: op0 = antecedent token; bind ITS yields as children[0]'s args; run; store; result=handle
    Join,         // async.join: concat every operand token's yields → one new token; result = handle
    Race,         // async.race: the sequential winner is index 0 (deterministic); result = 0 (⛔ validates NO handle)
    Cancel,       // async.cancel: consume op0, no-op (real cancellation is CEIR-6c; ⛔ validates NO handle)
    // ── data-parallel (4c): the SEQUENTIAL 6z-fold reference. The map/combine body is an ISOLATED mini-function
    //    (child_pool[children_off] = its compiled-FUNCTION index, NOT a seq index — captures rejected at compile). ──
    ParallelFor,  // op[0..2]=lo,hi,step; per index run the map fn (arg=iv), collect the yield → map_output[imm] (statement)
    MapReduce,    // op[0..3]=lo,hi,step,init; map (child0), then fold the combine fn (child1, args=acc,elem) in INDEX order
};

// The compiled comparison predicate (the "predicate" STRING attr → this dense enum at compile — the §153 immediate).
// NOLINTNEXTLINE(performance-enum-size)
enum class CmpPred : crd::u8
{
    Eq, Ne, Slt, Sle, Sgt, Sge, Ult, Ule, Ugt, Uge
};

// One compiled instruction: a dense op-id + spans into the plan's pools + an immediate.
struct Instr
{
    Op       op;
    crd::u32 operands_off = 0U; // into CompiledPlan::operand_pool (slot indices)
    crd::u32 operands_cnt = 0U;
    crd::u32 results_off  = 0U; // into CompiledPlan::result_pool (slot indices)
    crd::u32 results_cnt  = 0U;
    crd::u32 children_off = 0U; // into CompiledPlan::child_pool (child SEQ indices) — control flow
    crd::u32 children_cnt = 0U;
    crd::i64 imm          = 0;  // ConstI: value; CmpI: CmpPred; For: the induction-var slot of children[0]
};

// A §20 latch: at a SEQ (block) eval END, `ring[pos] = slots[next_slot]; pos = (pos+1) % depth` for a state cell that was
// read this block (read-all-then-latch). Pre-baked at compile — NO runtime trait scan (the §153 win over the reference).
struct Latch
{
    crd::u32 cell      = 0U;
    crd::u32 next_slot = 0U;
};

// A compiled SEQUENCE (one block's worth of instrs) + the slots its terminator (core.yield / func.return) yields + its
// pre-baked state LATCH list. Region bodies compile to child Seqs; the ENTRY function is `entry_seq`. ⛔ Each Seq OWNS its
// instrs (nested Array is movable — no interior pointers), so a control-flow op's child instrs live in separate Seqs.
struct Seq
{
    containers::Array<Instr>    instrs;
    containers::Array<crd::u32> yield_slots; // the terminator's operand slots (this region's yielded values)
    containers::Array<Latch>    latches;     // §20 cells to latch at this seq's block-eval end (in block order)
    containers::Array<crd::u32> arg_slots;   // this block's arg slots (4b: a continuation binds the antecedent token here)
    explicit Seq(memory::IAllocator* a) : instrs(a), yield_slots(a), latches(a), arg_slots(a) {}
};

// A compiled FUNCTION (4a): its body's entry seq + its frame size + the param slots to bind at a call. Slots are
// FRAME-RELATIVE ([0, num_slots)); a call runs the entry_seq against a fresh window on the run's slot stack.
struct CompiledFn
{
    crd::u32                    entry_seq = 0U; // into CompiledPlan::seqs — this function's body block
    crd::u32                    num_slots = 0U; // this function's frame size (its slot allocator's high-water)
    containers::Array<crd::u32> param_slots;    // the body block's arg slots (bound from the call's args)
    explicit CompiledFn(memory::IAllocator* a) : param_slots(a) {}
};

struct CompiledPlan
{
    containers::Array<Seq>        seqs;         // seq tree (ALL functions' seqs; each CompiledFn names its entry)
    containers::Array<crd::u32>   operand_pool; // slot indices for instrs' operand spans
    containers::Array<crd::u32>   result_pool;  // slot indices for instrs' result spans
    containers::Array<crd::u32>   child_pool;   // child SEQ indices for control-flow instrs
    containers::Array<crd::u32>   cell_depths;  // §20: the ring depth per dense cell index (num_cells entries) — PLAN-GLOBAL
    containers::Array<CompiledFn> funcs;        // 4a: the compiled function table (Op::Call + the 4c map/combine fns index this)
    crd::u32                      num_cells = 0U; // the §20 state-cell count (cells are per-OP global, NOT per-frame)
    crd::u32                      num_maps  = 0U; // 4c: the data-parallel op count (a dense map-output index per ParallelFor/MapReduce)
    crd::u32                      entry_fn  = 0U; // the @entry function's index into `funcs`
    explicit CompiledPlan(memory::IAllocator* a)
        : seqs(a), operand_pool(a), result_pool(a), child_pool(a), cell_depths(a), funcs(a)
    {
    }
};

// Why a COMPILE failed (a legitimate tier difference — the differential covers programs that compile).
// NOLINTNEXTLINE(performance-enum-size)
enum class CompileError : crd::u8
{
    Ok = 0,
    NoEntry,
    NoModuleBody,
    UnsupportedOp,    // an op this stage does not compile (async/task — 4b)
    BadPredicate,
    BadConst,
    ArityUnsupported,
    UnresolvedCall,   // 4a: a func.call whose "callee" symbol has no func / no body (reference: runtime UnresolvedCall)
    CallArity,        // 4a: call operand count != callee param count (reference: runtime BadArity) — resolved at COMPILE (§4)
    CapturedValue,    // 4c: an ISOLATED map/combine body reads an outer capture (reference: runtime UndefinedValue) (§4)
    ParallelArity,    // 4c: a map/combine body block-arg count is wrong (reference: runtime BadArity)
    ParallelYield,    // 4c: a map/combine body does not yield exactly one value (reference: runtime ParallelYieldArity)
    ParallelStateful, // 4c: a map/combine body (or a resolved callee) holds a §20 cell (reference: runtime ParallelBodyStateful)
};
[[nodiscard]] containers::StringView compile_error_name(CompileError e) noexcept;

// Why a RUN failed — mirrors the reference's control-flow ExecErrors (the differential compares the ERROR too).
// NOLINTNEXTLINE(performance-enum-size)
enum class RunError : crd::u8
{
    None = 0,
    BadForStep,         // For with step <= 0
    SelectorOutOfRange, // Switch selector not in [0, children)
    CondArity,          // While cond region did not yield exactly one value
    FuelExhausted,      // the step budget ran out (runaway loop)
    BadToken,           // 4b: await/join/continuation of a value that is not a live token handle (reference: BadToken)
    ContinuationArity,  // 4b: a continuation body's arg count != the antecedent token's yield count (reference: BadArity)
};
[[nodiscard]] containers::StringView run_error_name(RunError e) noexcept;

// CEIR-11c: cheap plan-SHAPE stats, all already computed at compile — a perf-linking bridge publishes them as crd-perf
// counters (crd-ceir core cannot link crd-perf: I5 + jobs-free). Not the hot path; filled once at compile end.
struct PlanStats
{
    crd::u32 num_instrs = 0U; // total Instr across all seqs (the compiled program size)
    crd::u32 num_seqs   = 0U; // Seq count (blocks/regions)
    crd::u32 num_funcs  = 0U; // compiled-function count (@entry + reachable callees + map/combine bodies)
    crd::u32 num_cells  = 0U; // §20 state cells
    crd::u32 num_maps   = 0U; // data-parallel ops
};

struct CompileResult
{
    CompiledPlan plan;
    CompileError error = CompileError::Ok;
    PlanStats    stats;                        // CEIR-11c: the plan-compile cost/shape counters
    explicit CompileResult(memory::IAllocator* a) : plan(a) {}
    [[nodiscard]] bool ok() const noexcept { return error == CompileError::Ok; }
};

// CEIR-11c: a §153-CLEAN profiling SEAM — null-default pre/post hooks fired around each dispatched instr, receiving the
// dense `Op` id (⛔ NOT an `Operation*` — the compiled loop never touches one; the §112 StepHook seam in op-id form).
// Zero-cost when unset (one predicted branch; 11z-clean on the shipping path). ⛔ crd-ceir core CANNOT link crd-perf
// (I5 + jobs-free), so a perf-linking BRIDGE consumer supplies the hook body (the crd/perf `jobs_adapter` pattern). ⛔
// OBSERVATION-ONLY: the hook receives only the op id (a `u8`) — it cannot read or write slots, so it cannot perturb the
// differential. `post` fires only after a SUCCESSFUL dispatch, so the pre/post stream is UNBALANCED (pre>post) on an
// erroring run — the consumer must reset per run and tolerate that.
struct RunHooks
{
    void (*pre)(crd::u8 op, void* user)  = nullptr;
    void (*post)(crd::u8 op, void* user) = nullptr;
    void* user                           = nullptr;
};

struct RunResult
{
    containers::Array<crd::i64> values;
    containers::Array<crd::i64> cells; // §118 inspection parity: the current value (ring[pos]) per cell after the run,
                                       // indexed by dense cell — compared to the reference's `cell_value` (like 11a map_output)
    containers::Array<containers::Array<crd::i64>> map_outputs; // 4c §118 parity: the per-index map per dense map index —
                                       // compared to the reference's `map_output(op)` (default-empty agrees with find-fail)
    RunError                    error = RunError::None;
    explicit RunResult(memory::IAllocator* a) : values(a), cells(a), map_outputs(a) {}
    [[nodiscard]] bool ok() const noexcept { return error == RunError::None; }
};

// COMPILE `entry`'s body into a `CompiledPlan`. ⛔ The CALLER must have registered the module's dialects.
[[nodiscard]] CompileResult compile(Context& ctx, const Module& module, containers::StringView entry,
                                    memory::IAllocator* alloc);

// RUN a compiled plan: bind `args` to the param slots, execute the entry seq over a flat i64 slot array, return the
// entry seq's yield-slot values (or a typed RunError). ⛔ §153: the loop touches only dense arrays. `hooks` (CEIR-11c,
// null-default) fire around each dispatched instr for a perf-linking consumer — zero-cost when unset.
[[nodiscard]] RunResult run(const CompiledPlan& plan, containers::ConstSpan<crd::i64> args, memory::IAllocator* alloc,
                            RunHooks hooks = {});
} // namespace crd::ceir::plan
