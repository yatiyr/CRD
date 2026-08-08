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
// (open-world), but registration records traits (func.func = Symbol, func.return = Terminator) + a verifier, and is
// where CEIR-1e's printer/parser hooks will live. Idempotent. Returns the dialect.
Dialect* register_dialect(Context& ctx);
} // namespace crd::ceir::func
