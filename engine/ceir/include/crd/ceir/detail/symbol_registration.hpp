#pragma once

// crd-ceir — the shared symbol re-registration step (CEIR-1e/1f). A symbol-defining op carries its identity ON the op
// as a `sym_name` (+ optional `sym_visibility`) string attribute — the MLIR model where the SymbolTable is an INDEX
// over `sym_name`, not the source of truth. BOTH the textual parser and the binary deserializer must rebuild that
// index the SAME way (including the duplicate-name error), so the logic lives here once instead of drifting in two.

#include <crd/core/types.hpp>

namespace crd::ceir
{
class Context;
class Module;
class Operation;

namespace detail
{
// If `op` carries a `sym_name` string attr, register it into `module`'s SymbolTable (visibility from `sym_visibility`,
// default Public). Returns false ONLY on a duplicate name (a load error the caller reports); an op with no `sym_name`
// (or a non-string one) is not a symbol definition and returns true.
[[nodiscard]] bool register_symbol(Context& ctx, Module& module, Operation* op) noexcept;
} // namespace detail
} // namespace crd::ceir
