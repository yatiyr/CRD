#pragma once

// crd-ceir — the DIALECT REGISTRY + op traits/interfaces (CEIR-1d, §6/§7/§101). The open-world core of CEIR: the
// engine NEVER switches on `op.kind`. Dialects (built-in OR plugin) register their ops with traits, a verifier, and
// (reserved for CEIR-1e) printer/parser hooks; analyses query TRAITS + INTERFACES to dispatch, so a new op or dialect
// needs no edit to any central enum (§7's "do not implement CEIR as a giant enum"). An op of an UNREGISTERED dialect
// is still a fully valid Operation — unknown-dialect preservation (§6.11): tools carry it opaquely.

#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir
{
class Context;
class Operation;
class Dialect;

// Structural traits an op-kind may carry (a flags set — the core never enumerates them centrally; a dialect ORs its
// own bits). E.g. CEIR-5b's CFG verifier reads Terminator; Symbol/SymbolTable drive symbol resolution. A FLAGS enum
// whose bits OR into OpInfo::traits (a u32 word) — sized for growth toward 32 traits, not the current value set
// (matches RtFeature/EventCategory); performance-enum-size is a false positive here.
// NOLINTNEXTLINE(performance-enum-size)
enum class OpTrait : u32
{
    Terminator        = 1U << 0U, // ends a block (func.return, cf.br, ...)
    Symbol            = 1U << 1U, // defines a symbol (func.func)
    SymbolTable       = 1U << 2U, // holds a symbol table (a module)
    Pure              = 1U << 3U, // no side effects (safe to CSE / DCE)
    IsolatedFromAbove = 1U << 4U, // a value-capture barrier (nested regions don't see outer SSA values)
};
[[nodiscard]] constexpr u32 operator|(OpTrait a, OpTrait b) noexcept
{
    return static_cast<u32>(a) | static_cast<u32>(b);
}
[[nodiscard]] constexpr u32 operator|(u32 a, OpTrait b) noexcept { return a | static_cast<u32>(b); }
// A single trait → the flags word (ergonomic register_op(..., flags_of(OpTrait::Symbol))).
[[nodiscard]] constexpr u32 flags_of(OpTrait t) noexcept { return static_cast<u32>(t); }

// A verifier hook: true iff `op` is well-formed for its kind. nullptr ⇒ trivially valid (opaque — unknown dialects).
using VerifyFn = bool (*)(const Context&, const Operation&);

// One op-interface implementation on an op-kind — an opaque pointer to the interface's function table; the analysis
// that owns the `InterfaceId` casts it back. Small per-op list (linear scan).
struct OpInterface
{
    InterfaceId id;
    const void* impl = nullptr;
};

// Per-op registration — the ODS-lite descriptor (§8). printer/parser hooks are reserved for CEIR-1e (add fields here,
// no retrofit of Operation).
struct OpInfo
{
    OpId                   kind;
    containers::StringView name;               // "dialect.op"
    const Dialect*         dialect    = nullptr;
    u32                    traits     = 0U;     // OpTrait flags
    VerifyFn               verify     = nullptr; // nullptr ⇒ trivially valid
    OpInterface*           ifaces     = nullptr; // arena; grows by rebuild
    u32                    num_ifaces = 0U;
};

// A registered dialect: a name + a factory for its ops. Created via `Context::register_dialect`. A thin typed handle —
// the Context owns the OpInfos; `Dialect` interns "<dialect>.<op>" and registers on the owning Context.
class Dialect
{
public:
    // Constructed only through `Context::register_dialect` (a public ctor for arena placement; do not build directly).
    Dialect(Context* ctx, containers::StringView name) noexcept : m_ctx(ctx), m_name(name) {}

    [[nodiscard]] containers::StringView name() const noexcept { return m_name; }

    // Register op `op` (interned as "<dialect>.<op>") with `traits` (OpTrait flags) + an optional verifier. Returns
    // its OpId. Idempotent by kind — re-registering returns the same kind and keeps the first registration's info.
    OpId register_op(containers::StringView op, u32 traits = 0U, VerifyFn verify = nullptr);

private:
    Context*               m_ctx = nullptr;
    containers::StringView m_name;
};
} // namespace crd::ceir
