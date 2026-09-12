#pragma once

// crd-ceir — the CEIR-5z REFERENCE EXECUTOR (§118): a SLOW, CORRECT semantic interpreter for the core CEIR subset —
// arith (const/addi/muli/cmpi), ceir.core control flow (scope/if/for/while/switch/match/yield + §20 state/delay/history),
// and ceir.func calls. Its purpose is differential testing / headless validation / deterministic debugging (§118/§119),
// NOT speed. ⛔ Dispatch is an Interpreter-OWNED `OpId → EvalFn` table, NOT an OpInfo hook: `register_op` is first-wins
// idempotent, so a hook attached after the generated registration would silently no-op (the registered-default-empty
// scar). Semantics are INSTALLED per dialect (open-world — a caller may add or replace); an op with no installed EvalFn is
// a typed error, never skipped (EMPTY≠UNKNOWN at the executor). Values are wrapping `i64` scalars (the arith dialect is
// integer-only through CEIR-11a; real per-width/float typing is a future refinement when a float op is defined). State
// cells are a §20 depth-N ring keyed by the STATIC op instance, persisting across evaluations (incl.
// across function calls) — `state` ≡ `delay` ≡ `history(depth=1)` under one register mechanism.

#include <crd/ceir/context.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

#include <atomic> // the CEIR-6c cooperative cancel flag (a primitive, thread-safe monotonic signal — not an std container)

namespace crd::ceir
{
class SymbolTable;
}

namespace crd::ceir::exec
{
// The typed failure modes (§118 — a reference executor REPORTS, never crashes or hangs).
// NOLINTNEXTLINE(performance-enum-size)
enum class ExecError : u8
{
    None = 0,
    NoSemantics,        // an op-kind with no installed EvalFn (EMPTY≠UNKNOWN at the executor)
    UnresolvedCall,     // a func.call whose callee symbol does not resolve
    UndefinedValue,     // an operand read before its value is in the environment
    BadForStep,         // core.for with step <= 0 (a non-terminating / backwards counted loop)
    SelectorOutOfRange, // a switch/match selector not in [0, num_regions)
    UnknownPredicate,   // arith.cmpi with an unrecognized predicate string
    CondArity,          // a while/if condition region did not yield exactly one value
    FuelExhausted,      // the step budget ran out (a runaway loop — timeout is not a hang-proof)
    NoEntry,            // the named entry function was not found in the module
    BadArity,           // entry/call argument count != the callee's parameter count
    ParallelBodyStateful, // CEIR-6b: a task.parallel_for body (or a resolved callee) holds a StateEdge cell — results would
                          // depend on the range split (non-deterministic); a parallel body must be state-free.
    ParallelYieldArity,   // CEIR-6b: a task.parallel_for body does not yield exactly one value (its per-index map output).
    BadToken,             // CEIR-6b: an async.await/join on a token value that is not a live session handle (a forged /
                          // cross-session token — token runtime values are session-scoped; the 6a static verifier catches
                          // static misuse, this catches a runtime-forged handle instead of silently yielding nothing).
    Cancelled,            // CEIR-6c: cooperative cancellation was requested (the cancel flag) and the running op observed
                          // it in the step loop — distinct from FuelExhausted (both stop loops; this is a REQUESTED stop).
};
[[nodiscard]] containers::StringView exec_error_name(ExecError e) noexcept;

// The result of an execution: the entry function's `func.return` values, or the FIRST error + the offending op.
struct ExecResult
{
    containers::Array<crd::i64> values;
    ExecError                   error = ExecError::None;
    const Operation*            op    = nullptr; // the op the error points at (nullptr when ok / NoEntry)
    explicit ExecResult(memory::IAllocator* a) : values(a) {}
    [[nodiscard]] bool ok() const noexcept { return error == ExecError::None; }
};

// A migration SNAPSHOT of one §20 state cell (CEIR-10a): its 8d STABLE ID (the cross-generation key — op pointers do NOT
// survive a generation swap / round-trip), the whole ring, and the ring position. A hot-swap snapshots the OLD
// generation's cells then restores them into the NEW generation's interpreter matched by id.
struct StateSnapshot
{
    crd::u64                    id  = 0U;
    containers::Array<crd::i64> ring;
    crd::u32                    pos = 0U;
    explicit StateSnapshot(memory::IAllocator* a) : ring(a) {}
};

class Interpreter;
// An op-kind's reference semantics: read operands / write results / run sub-regions via `in`. Returns None or a failure.
using EvalFn = ExecError (*)(Interpreter& in, const Operation& op);

// The reference interpreter. Construct → install semantics (`install_builtin_semantics` or the per-dialect installers) →
// `invoke`. ⛔ The CALLER registers the module's dialects first (traits are registry state, not serialized — the CEIR-5d
// finding): the executor reads `StateEdge`/`Terminator` traits + `resolve_call`, which need the dialect registered.
// ⛔ An Interpreter is ONE EXECUTION SESSION: the fuel budget and the §20 state cells CARRY ACROSS `invoke` calls (so a
// stateful function keeps its cells between top-level calls). Construct a FRESH Interpreter for an independent run.
class Interpreter
{
public:
    // `scratch` (null ⇒ `ctx.allocator()`) backs ALL per-run state (the semantics table, cells, frames, EvalFn scratch,
    // the result). ⛔ For PARALLEL execution (CEIR-6b): a worker range must NOT touch the shared Context arena — give each
    // sub-interpreter its OWN scratch allocator + build it from a PROTOTYPE (below), and reads of the const Context are
    // then the only shared access (safe).
    explicit Interpreter(Context& ctx, crd::u64 max_steps = crd::u64{1} << 24U, memory::IAllocator* scratch = nullptr);
    // PROTOTYPE constructor (CEIR-6b): copy `proto`'s installed semantics table (intern-free — the keys are already
    // OpId.value) with FRESH env / cells / fuel over `scratch`. The submitting thread installs once into a prototype; each
    // parallel range builds its own interpreter from it. Does NOT copy proto's cells/env/fuel (a fresh session).
    Interpreter(const Interpreter& proto, memory::IAllocator* scratch, crd::u64 max_steps);
    Interpreter(Interpreter&&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;
    Interpreter& operator=(Interpreter&&) = delete;
    ~Interpreter()                        = default;

    void install(OpId kind, EvalFn fn); // open-world: (re-)bind a kind's semantics; last install wins
    [[nodiscard]] Context&              ctx() const noexcept { return m_ctx; }
    [[nodiscard]] memory::IAllocator*   allocator() const noexcept { return m_scratch; } // the per-run scratch (EvalFns use it)
    // The module being invoked (captured by invoke/invoke_region) + the remaining fuel — the CEIR-11a sequential
    // reference for task.parallel_for/map_reduce runs a body via a sub-interpreter (invoke_region needs the module; the
    // sub gets the parent's remaining budget). nullptr module ⇒ not inside an invoke.
    [[nodiscard]] const Module*         module() const noexcept { return m_module; }
    [[nodiscard]] crd::u64              fuel() const noexcept { return m_fuel; }
    // A generic extension pointer for installed EvalFns — opaque to crd-ceir; the installer sets + interprets it (the
    // CEIR-6b host provider threads its parallel context / map-output buffers here). NOT copied by the prototype ctor.
    void                                set_user(void* u) noexcept { m_user = u; }
    [[nodiscard]] void*                 user() const noexcept { return m_user; }
    // §30 cooperative cancellation (CEIR-6c): a monotonic flag the step loop checks. The host provider threads ONE shared
    // flag into every parallel sub-interpreter (set from any thread; reads are relaxed — a monotonic true never un-sets).
    // NOT copied by the prototype ctor. `cancelled()` ⇒ the running program should stop with `ExecError::Cancelled`.
    void                                set_cancel_flag(const std::atomic<bool>* f) noexcept { m_cancel = f; }
    [[nodiscard]] bool cancelled() const noexcept { return m_cancel != nullptr && m_cancel->load(std::memory_order_relaxed); }
    // §112 STEP HOOKS (CEIR-11a — the debugger SEAM, hooks only; no debugger/stepping UI): `pre` fires BEFORE each op
    // dispatches, `post` fires ONLY after a SUCCESSFUL dispatch (never on an error). Null by default → ZERO work when
    // unset (a single null check in the hot loop). ⛔ NOT copied by the prototype ctor (per-session, like set_user/cancel).
    using StepHook = void (*)(const Operation& op, void* user);
    void set_step_hooks(StepHook pre, StepHook post, void* user) noexcept { m_pre_hook = pre; m_post_hook = post; m_hook_user = user; }

    // Run `@entry(args)` against `m` (calls resolve via `m.symbols()`).
    [[nodiscard]] ExecResult invoke(const Module& m, containers::StringView entry, containers::ConstSpan<crd::i64> args);

    // Run `body` as an ISOLATED entry: a FRESH frame, `block_args` bind its entry block's args, calls resolve via `m`;
    // capture its terminator's (core.yield) operands into `out_yield`. The CEIR-6b parallel provider runs a
    // task.parallel_for body per index this way. ⛔ `body` must be SELF-CONTAINED — it uses only its block-args (the
    // induction var) + body-local defs (inline consts) + self-contained calls; OUTER captures are NOT seeded.
    [[nodiscard]] ExecError invoke_region(const Module& m, const Region& body, containers::ConstSpan<crd::i64> block_args,
                                          containers::Array<crd::i64>& out_yield);

    // ---- the surface the installed EvalFns use ----
    [[nodiscard]] bool      value_of(const Value* v, crd::i64& out) const noexcept; // env lookup; false ⇒ undefined
    void                    set_value(const Value* v, crd::i64 x);                  // bind a result / block-arg value
    [[nodiscard]] ExecError run_region(const Region& r, containers::Array<crd::i64>* out_yield); // eval; capture terminator
    [[nodiscard]] ExecError call(const Operation& call_op, containers::Array<crd::i64>& out_results); // a func.call frame
    [[nodiscard]] crd::i64  cell_read(const Operation& state_op);       // §20: init-fill on first eval; return ring oldest
    [[nodiscard]] bool      spend_fuel() noexcept;                      // decrement the step budget; false ⇒ exhausted
    ExecError               fail(ExecError e, const Operation* op) noexcept; // record the offending op (first wins), return e

    // Inspection (§118 deterministic debugging): a state cell's current value. Builder-form ONLY — cells are keyed by op
    // POINTER, which does not survive a text/binary round-trip. false ⇒ the op has never been evaluated.
    [[nodiscard]] bool cell_value(const Operation* state_op, crd::i64& out) const noexcept;

    // ---- CEIR-10a hot-reload state migration ----
    // SNAPSHOT every live §20 cell keyed by its op's 8d STABLE ID, into `out` (each snapshot's ring copied from `alloc`).
    // ⛔ The live cells' ops must carry stable ids (the module was cooked/loaded, which assigns them). A cell never
    // evaluated is simply absent from the live set → absent from the snapshot. Order is unspecified (restore is id-keyed).
    void snapshot_state_by_id(containers::Array<StateSnapshot>& out, memory::IAllocator* alloc) const;

    // RESTORE cells into THIS (fresh, new-generation) interpreter from `snap`, matched by 8d stable id against
    // `new_module`'s StateEdge ops: a cell present in `snap` AND in `new_module` with a MATCHING ring depth is seeded
    // (value reuse); an absent id or a depth mismatch is SKIPPED (the new generation init-fills — EMPTY≠UNKNOWN). Returns
    // the number of cells actually restored. ⛔ Assigns stable ids on `new_module` first (the migration key must be valid).
    crd::u32 restore_state_by_id(const Module& new_module, containers::ConstSpan<StateSnapshot> snap);

    // A store of value-lists indexed by a u32 handle. The CEIR-6b SEQUENTIAL-async installer uses it: `async.launch`
    // stashes its body's yields and the token's runtime value IS the handle; `async.await` retrieves them. Generic — the
    // builtin (arith/core/func) installer never touches it. ⛔ Handles are SESSION-SCOPED (they index this interpreter's
    // store, which never resets across invokes); a token value forged at runtime (or carried across sessions) is NOT a
    // valid handle — `valid_yield_handle` distinguishes it from a legitimately-empty launch (both give an empty span).
    [[nodiscard]] crd::u32                        store_yields(containers::ConstSpan<crd::i64> ys);
    [[nodiscard]] bool                            valid_yield_handle(crd::u32 handle) const noexcept;
    [[nodiscard]] containers::ConstSpan<crd::i64> stored_yields(crd::u32 handle) const noexcept;

    // §118 MAP-OUTPUT store (CEIR-11a): the per-index yields of a task.parallel_for / task.map_reduce, keyed by op
    // POINTER (builder-form only — pointers don't survive a round-trip; the `cell_value` caveat). ⭐ Named `map_output`
    // VERBATIM to mirror `HostProvider::map_output` — the CEIR-11b harness compares `in.map_output(op)` vs
    // `provider.map_output(op)` symmetrically. The sequential parallel_for/map_reduce EvalFns store here; the builtin
    // (arith/core/func) installer never touches it (generic, like the yield-store).
    void                                          store_map_output(const Operation* op, containers::Array<crd::i64>&& out);
    [[nodiscard]] containers::ConstSpan<crd::i64> map_output(const Operation* op) const noexcept;

private:
    using Env = containers::HashMap<const Value*, crd::i64>;
    // A §20 state cell: a ring of the last `depth` `next` values; `current` = the value from `depth` evaluations ago
    // (init-filled until warm). `pos` is the oldest slot (the next `cell_read` returns `ring[pos]`; a latch overwrites it).
    struct Cell
    {
        containers::Array<crd::i64> ring;
        crd::u32                    pos = 0U;
    };

    [[nodiscard]] ExecError eval_block(const Block& b);
    [[nodiscard]] ExecError eval_op(const Operation& op);
    [[nodiscard]] ExecError cell_latch(const Operation& op); // read env[next], push into the ring (block-eval end)

    Context&                                     m_ctx;
    memory::IAllocator*                           m_scratch; // backs all per-run state (null-defaulted to ctx.allocator())
    crd::u64                                      m_fuel;
    const SymbolTable*                            m_symbols = nullptr; // the invoked module's table (call resolution)
    Env*                                          m_env     = nullptr; // the CURRENT frame (a function call's env)
    containers::HashMap<crd::u64, EvalFn>         m_sem;               // OpId.value → semantics
    containers::HashMap<const Operation*, Cell>   m_cells;             // persistent §20 state cells
    containers::Array<containers::Array<crd::i64>> m_yield_store;      // §37 sequential-async completed-yields (CEIR-6b)
    containers::HashMap<const Operation*, containers::Array<crd::i64>> m_map_output; // §118 task map-output (CEIR-11a)
    const Module*                                 m_module = nullptr; // the module being invoked (invoke/invoke_region)
    ExecError                                     m_err    = ExecError::None;
    const Operation*                              m_err_op = nullptr;
    void*                                         m_user   = nullptr; // opaque EvalFn extension pointer (CEIR-6b host provider)
    const std::atomic<bool>*                      m_cancel = nullptr; // §30 cooperative cancel flag (CEIR-6c); not proto-copied
    StepHook                                      m_pre_hook  = nullptr; // §112 pre-op hook (CEIR-11a); not proto-copied
    StepHook                                      m_post_hook = nullptr; // §112 post-op hook (successful dispatch only)
    void*                                         m_hook_user = nullptr; // opaque user for the step hooks
};

// Install the built-in reference semantics (open-world — a caller may install more or override).
void install_arith_semantics(Interpreter& in);
void install_core_semantics(Interpreter& in);
void install_func_semantics(Interpreter& in);
void install_builtin_semantics(Interpreter& in); // arith + core + func
// §37 async SEQUENTIAL reference semantics (CEIR-6b — the 6a IOU's sequential half): launch runs the body at launch and
// stashes its yields (the token's value is the store handle); await returns them; join concatenates; race → index 0
// deterministically; cancel consumes (a no-op — real cancellation is CEIR-6c). Jobs-backed parallel async (launch/await
// ON-POOL) is CEIR-11a (the §84 async-host subset — routed at 6z).
// A SEPARATE installer (NOT in install_builtin_semantics).
void install_async_semantics(Interpreter& in);
// §38 host-task ops SEQUENTIAL reference (CEIR-11a — the §118 oracle, host-neutral): spawn/main_thread/worker (share
// launch's EvalFn — placement is value-unobservable in the oracle), group (shares scope), fiber_wait (mirrors await),
// continuation (antecedent yields → body block-args → new token). Jobs-backed placement is the crd-ceir-host provider
// (CEIR-11a stage 3). A SEPARATE installer (uses the yield-store, like install_async_semantics; NOT in builtin).
void install_task_semantics(Interpreter& in);

// ── the parallel-purity PRE-FLIGHT (CEIR-11a — moved from crd-ceir-host at the sequential reference's arrival) ──
// ⭐ The 9d hoist-at-second-consumer: the provider's PARALLEL run (submit-thread check) and the reference's SEQUENTIAL run
// (legality at the op) share ONE legality analysis, so they AGREE by construction rather than by duplication. PURE IR
// analysis — StateEdge/Terminator traits + resolved callees; ⛔ NO jobs types (crd-ceir stays host-only, I4).
struct PreflightResult
{
    ExecError        err = ExecError::None;
    const Operation* op  = nullptr; // the offender (owner for arity/shape; the precise inner op for stateful/unresolved)
};
// The STATE-FREE / calls-resolved transitive walk over a region (the pre-flight core, WITHOUT the arity/terminator
// checks): every op in `r` + its resolved callees is StateEdge-free (a §20 cell would make a parallel result depend on
// the schedule) and every call resolves. ⛔ `err == ParallelBodyStateful` / `UnresolvedCall` on the offender, else None.
// Exported so a variadic-body consumer (the CEIR-11a jobs-backed launch pool-eligibility classifier — launch bodies take
// 0 block-args + yield variadic, so `check_parallel_region`'s arity/terminator checks don't apply) reuses it, not dup it.
[[nodiscard]] PreflightResult region_state_free(Context& ctx, const SymbolTable& syms, Region* r);

// A single parallel region (a parallel_for/map_reduce MAP body, or a map_reduce COMBINE body) must: have a first block
// with EXACTLY `expect_args` block-args, a Terminator yielding EXACTLY 1 value, and be StateEdge-free transitively (a cell
// would make the result depend on the range split). The sequential EvalFns call this on their own regions AT eval.
[[nodiscard]] PreflightResult check_parallel_region(Context& ctx, const SymbolTable& syms, const Operation* owner,
                                                    Region* r, crd::u32 expect_args);
// Module-wide: pre-flight every task.parallel_for + task.map_reduce (the provider's submit-thread check + the CEIR-11b
// harness precondition). ⛔ Deliberately CONSERVATIVE: rejects the whole module if ANY parallel op is impure, even one the
// entry never reaches (over-rejection, safe — a sequential reference simply never reaches such an op; a documented
// oracle/provider divergence, not a bug). `ctx` is non-const (interns the op kinds it dispatches on — idempotent).
[[nodiscard]] PreflightResult preflight_parallel(Context& ctx, const Module& module);

// Byte-pin `values` deterministically: 8 bytes little-endian per i64 (the §118 "byte-pinned output" the gate compares).
[[nodiscard]] containers::Array<crd::u8> pin_values(containers::ConstSpan<crd::i64> values, memory::IAllocator* alloc);
} // namespace crd::ceir::exec
