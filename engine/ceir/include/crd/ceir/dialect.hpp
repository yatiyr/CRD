#pragma once

// crd-ceir — the DIALECT REGISTRY + op traits/interfaces (CEIR-1d, §6/§7/§101). The open-world core of CEIR: the
// engine NEVER switches on `op.kind`. Dialects (built-in OR plugin) register their ops with traits, a verifier, and
// (reserved for CEIR-1e) printer/parser hooks; analyses query TRAITS + INTERFACES to dispatch, so a new op or dialect
// needs no edit to any central enum (§7's "do not implement CEIR as a giant enum"). An op of an UNREGISTERED dialect
// is still a fully valid Operation — unknown-dialect preservation (§6.11): tools carry it opaquely.

#include <crd/ceir/effect.hpp>
#include <crd/ceir/id.hpp>
#include <crd/ceir/semantics.hpp>
#include <crd/containers/span.hpp>
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
    // A §20 explicit-state op (core.state/delay/history): it BREAKS a feedback cycle. ⛔ CONVENTION: its LAST operand is
    // the feedback ("next") value — the ONLY operand ever exempt from CEIR-5b def-before-use (it may forward-reference the
    // value the cycle feeds back). (A 1-operand StateEdge op's SOLE operand is its feedback — no separate dominating init.)
    // The 5d verifier reads this trait (open-world — any dialect may mark a state op; the core never name-checks a kind) to
    // legalize "graph cycles must pass through explicit state/delay semantics" (§20).
    StateEdge         = 1U << 5U,
    // §37 async: this op PRODUCES completion token(s) — ⛔ CONVENTION: EVERY result is a token. (async.launch/join.)
    TokenProducer     = 1U << 6U,
    // §37 async: this op CONSUMES token(s) — ⛔ CONVENTION: EVERY operand is a consumed token (revisitable per the
    // StateEdge-last-operand precedent when a mixed-operand consumer arrives). (async.await/join/race/cancel; join is
    // BOTH.) The CEIR-6a `find_token_misuse` verifier reads these traits (open-world — any dialect may mark async ops; the
    // core never name-checks a kind) to enforce the §116 use-once discipline (every token consumed EXACTLY once).
    TokenConsumer     = 1U << 7U,
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

// The query context threaded through an instance-dependent effect resolution (the §34 callee-derived-effects case).
// Defined in context.hpp (needs the SymbolTable + a visited map); only referenced by pointer/reference here.
struct EffectQuery;

// An op-kind's INSTANCE-DEPENDENT effect hook (the CEIR-4a "EffectsFn" landing, §26/§34): a `func.call`'s effective
// effects are its CALLEE's, resolved through the SymbolTable — not expressible as a kind-level `EffectRecord` array.
// The hook ORs the op instance's effective §26 families into `mask` (a `u32` EffectFamily bitmask — 27 families < 32)
// and returns true iff it HANDLED the op; false ⇒ the dispatcher falls back to the static `effects` records. nullptr on
// an OpInfo ⇒ no instance-dependent effects (the common case). Kept a plain fn-ptr (the `verify` precedent) so the core
// never special-cases a dialect (I6) — the func dialect registers this for `func.call`.
using EffectsFn = bool (*)(const Context&, const Operation&, const EffectQuery&, u32&);

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
    const Dialect*         dialect     = nullptr;
    u32                    traits      = 0U;     // OpTrait flags
    VerifyFn               verify      = nullptr; // nullptr ⇒ trivially valid
    OpInterface*           ifaces      = nullptr; // arena; grows by rebuild
    u32                    num_ifaces  = 0U;
    const EffectRecord*    effects     = nullptr; // §26 declared effects (arena copy); nullptr ⇒ none declared
    u32                    num_effects = 0U;
    EffectsFn              effects_fn  = nullptr; // §34 instance-dependent effects (a call's callee-derived set); nullptr ⇒ none
    DeterminismClass       determinism = DeterminismClass::Unspecified; // §27 op-level class; Unspecified ⇒ no claim
    EvalDomain             domain      = EvalDomain::Unspecified;       // §15 op-kind domain affinity; Unspecified ⇒ none
    // ADR-0110 §2.1 native binding (CEIR-7a): true iff the op declares `[op.native]` (an INTRINSIC); `native_provider` is
    // its provider name (""⇒none). Promoted from OpSchema to the RUNTIME OpInfo (the determinism/domain precedent) so the
    // §106 dependency-record collector can read a kind's intrinsic/provider without a schema index. `native_determinism`
    // is NOT promoted (no runtime consumer yet — CEIR-13c). ⛔ Append at END.
    bool                   intrinsic       = false;
    containers::StringView native_provider = {};
};

// The registration descriptor for `Dialect::register_op` (CEIR-4c) — one struct instead of a growing positional param
// list. Every field defaults, so a call names only what it declares (C++20 designated initializers:
// `register_op("addi", {.effects = …, .determinism = …})`). ⛔ `effects` is a BORROWED span — `register_op` COPIES the
// records into the Context arena, so the spec (and any array it points at) is a TEMPORARY that need not outlive the call.
struct OpSpec
{
    u32                                 traits      = 0U;                          // OpTrait flags
    VerifyFn                            verify      = nullptr;                      // nullptr ⇒ trivially valid
    containers::ConstSpan<EffectRecord> effects     = {};                           // §26 (CEIR-4a)
    EffectsFn                           effects_fn  = nullptr;                        // §34 callee-derived effects (CEIR-5c)
    DeterminismClass                    determinism = DeterminismClass::Unspecified; // §27 (CEIR-4b)
    EvalDomain                          domain      = EvalDomain::Unspecified;       // §15 (CEIR-4c)
    // ADR-0110 §2.1 native binding (CEIR-7a) — promoted to the runtime OpInfo so `collect_dependencies` reads a kind's
    // intrinsic/provider without a schema index (the determinism/domain precedent). ⛔ Append at END.
    bool                                intrinsic       = false;
    containers::StringView              native_provider = {};   // "" unless intrinsic; asserted non-empty when intrinsic
};

// A registered dialect: a name + a factory for its ops. Created via `Context::register_dialect`. A thin typed handle —
// the Context owns the OpInfos; `Dialect` interns "<dialect>.<op>" and registers on the owning Context.
class Dialect
{
public:
    // Constructed only through `Context::register_dialect` (a public ctor for arena placement; do not build directly).
    Dialect(Context* ctx, containers::StringView name) noexcept : m_ctx(ctx), m_name(name) {}

    [[nodiscard]] containers::StringView name() const noexcept { return m_name; }

    // Register op `op` (interned as "<dialect>.<op>") from an `OpSpec` (traits + verifier + §26 effects + §27 determinism
    // + §15 domain). Returns its OpId. Idempotent by kind — re-registering returns the same kind and keeps the first
    // registration's info. The `spec.effects` records are COPIED into the Context arena (the span need not outlive the
    // call). ⛔ Asserts: a `Pure` op declares zero effects; every effect's family/target is in range; `domain` is in range.
    OpId register_op(containers::StringView op, const OpSpec& spec = {});

private:
    Context*               m_ctx = nullptr;
    containers::StringView m_name;
};
} // namespace crd::ceir
