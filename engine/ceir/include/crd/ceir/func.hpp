#pragma once

// crd-ceir — the `ceir.func` dialect (CEIR-1b, §34): `func.func` (a symbol-defining function with a body region),
// `func.call` (a symbol-referencing call), `func.return` (the body terminator). "Reusable subgraphs are functions."
// Built ENTIRELY on the generic `Context` factories + the `SymbolTable` — the core graph is NOT special-cased for
// func (open-world dialects; the core never switches on a func kind). Op-kinds are interned lazily against the Context.

#include <crd/ceir/context.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/symbol_table.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::func
{
// The three dialect op-kinds, interned against `ctx` (idempotent).
[[nodiscard]] inline OpId func_kind(Context& ctx) { return ctx.intern_op("func", "func"); }
[[nodiscard]] inline OpId call_kind(Context& ctx) { return ctx.intern_op("func", "call"); }
[[nodiscard]] inline OpId return_kind(Context& ctx) { return ctx.intern_op("func", "return"); }

// Create `func.func @name` (visibility `vis`) with `num_params` entry-block parameters (all opaque `param_type` until
// CEIR-3 gives them real types) and a single body Region + entry Block; register it in `module`'s SymbolTable.
// Returns the op, or nullptr if `name` is empty or already defined in the module (nothing is created on a duplicate).
// Fill the body via `func_body_block(op)`; terminate it with `create_return`.
[[nodiscard]] Operation* create_func(Context& ctx, Module& module, containers::StringView name, Visibility vis,
                                     u32 num_params, TypeId param_type = {});

// The func.func's entry Block (its parameters are the block args; body ops + the terminator live here). nullptr if
// `func_op` has no body region.
[[nodiscard]] Block* func_body_block(Operation* func_op) noexcept;

// A `func.return` terminator carrying `values` as operands.
[[nodiscard]] Operation* create_return(Context& ctx, containers::ConstSpan<Value*> values);

// A `func.call` of symbol `callee` with `args`, producing `num_results` results (opaque `result_type` until CEIR-3).
// The callee is recorded as a symbol reference (Context side-table) resolved lazily by `resolve_call`.
[[nodiscard]] Operation* create_call(Context& ctx, containers::StringView callee, containers::ConstSpan<Value*> args,
                                     u32 num_results, TypeId result_type = {});

// Resolve `call`'s callee against `table` → the defining func.func op, or nullptr if unresolved. Cross-module: pass a
// DIFFERENT module's table (resolution is by symbol name, not by which module the call lives in).
[[nodiscard]] Operation* resolve_call(const Context& ctx, const Operation* call, const SymbolTable& table);

// Register the `func` DIALECT + its ops on `ctx` (CEIR-1d). OPTIONAL — the helpers above work WITHOUT registration
// (open-world), but registration records traits (func.func = Symbol, func.return = Terminator) + a verifier + the
// CEIR-5c `func.call` EffectsFn hook (callee-derived effects), and is where CEIR-1e's printer/parser hooks will live.
// Idempotent. Returns the dialect.
Dialect* register_dialect(Context& ctx);

// ── §34 recursion policy (CEIR-5c) ── a `func.func` may DECLARE how it recurses; the verifier checks the declaration
// against the resolved call graph (the constitution's "recursion where legal" / "bounded recursion attributes"). A
// CLOSED machine-verified vocabulary (the CEIR-4c `region_exec` precedent), stored as the int attr `recursion` (plus
// `recursion_max_depth` when Bounded) — reserved attr vocabulary.
enum class RecursionPolicy : u8
{
    Unspecified = 0, // no claim — recursion neither promised nor forbidden (the executor's concern at 5d/5z)
    None,            // NON-recursive — must be acyclic in the resolved call graph (verified)
    Bounded,         // may recurse to a declared depth — requires recursion_max_depth >= 1 (present+positive verified)
    Unbounded,       // explicitly permits unbounded recursion (legal — the acknowledgement itself is the contract)
};
[[nodiscard]] containers::StringView recursion_policy_name(RecursionPolicy p) noexcept;

// Declare `func_op`'s recursion policy (sets the `recursion` int attr; `max_depth` sets `recursion_max_depth` when > 0).
void set_recursion_policy(Context& ctx, Operation* func_op, RecursionPolicy policy, u32 max_depth = 0U);
// Read it back. Returns false iff the `recursion` attr is PRESENT but out-of-range (corrupt ⇒ an InvalidPolicyAttr);
// absent ⇒ `out = Unspecified`, true. `recursion_max_depth_of` ⇒ 0 if absent.
[[nodiscard]] bool recursion_policy_of(const Context& ctx, const Operation& func_op, RecursionPolicy& out) noexcept;
[[nodiscard]] u32  recursion_max_depth_of(const Context& ctx, const Operation& func_op) noexcept;

// The §34 recursion-policy verifier's pointing result.
enum class RecursionViolationKind : u8
{
    None = 0,
    DeclaredNoneRecurses, // a `None`-declared func participates in a call-graph cycle (incl. self-recursion)
    BoundedMissingDepth,  // a `Bounded`-declared func lacks recursion_max_depth >= 1 (declared-words-must-be-validated)
    InvalidPolicyAttr,    // the `recursion` attr is present but corrupt / out-of-range (the 4c corrupt-tag precedent)
};
[[nodiscard]] containers::StringView recursion_violation_kind_name(RecursionViolationKind k) noexcept;
struct RecursionViolation
{
    const Operation*       func_op = nullptr;
    RecursionViolationKind kind    = RecursionViolationKind::None;
};

// Verify every DECLARED recursion policy in `m` against the resolved call graph (calls resolved via `table`). Returns the
// FIRST offender in module PRE-ORDER (deterministic — the printer's walk order) or `{}`. ⛔ Verifies only where DECLARED:
// an UNDECLARED (Unspecified) func on a cycle is NOT a violation — unacknowledged recursion is the executor's concern at
// 5d/5z (a documented relaxation, NOT a silent invalidation of every unannotated recursive module). `None` is proven
// acyclic over the RESOLVED graph only — an unresolvable callee is an unverifiable edge (single-module scope).
[[nodiscard]] RecursionViolation find_recursion_violation(const Context& ctx, const Module& m, const SymbolTable& table);
} // namespace crd::ceir::func
