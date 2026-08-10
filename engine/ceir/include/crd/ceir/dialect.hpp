#pragma once

// crd-ceir — the DIALECT REGISTRY + op traits/interfaces (CEIR-1d, §6/§7/§101). The open-world core of CEIR: the
// engine NEVER switches on `op.kind`. Dialects (built-in OR plugin) register their ops with traits, a verifier, and
// (reserved for CEIR-1e) printer/parser hooks; analyses query TRAITS + INTERFACES to dispatch, so a new op or dialect
// needs no edit to any central enum (§7's "do not implement CEIR as a giant enum"). An op of an UNREGISTERED dialect
// is still a fully valid Operation — unknown-dialect preservation (§6.11): tools carry it opaquely.

#include <crd/ceir/effect.hpp>
#include <crd/ceir/hazard.hpp> // ResourceClass (CEIR-8c location-class resource class)
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
struct Type;      // CEIR-8a: the open-world type-class verify hook takes a `const Type&` (full def in type.hpp)
struct AttrValue; // CEIR-8b: the open-world attribute-class verify hook takes a `const AttrValue&` (full def in attr.hpp)

// Structural traits an op-kind may carry (a flags set). ⛔ CEIR-8e (ADR-0115) CLOSED-vocabulary policy (struck in
// place — the SUPERSEDED discipline): traits are the fixed REASONING AXES the CORE's verifiers switch on (5b reads
// Terminator, 5d reads StateEdge, 6a reads TokenProducer/Consumer). A dialect **SETS** these core-minted bits on its
// ops; it NEVER **MINTS** a new bit — plugin BEHAVIOR lives in an open-world op-INTERFACE (OpInterface below), never
// a new trait bit. `register_op` REJECTS a stray/out-of-vocabulary bit (`kKnownTraitsMask`), the `kLastEffectFamily`
// parallel. A FLAGS enum whose bits OR into OpInfo::traits (a u32 word), room for 32; performance-enum-size is a false
// positive here. ⛔ Append at END + keep `kKnownTraitsMask` AND ceir_opgen.py's OP_TRAITS in lockstep.
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

// ⛔ CEIR-8e (ADR-0115): the closed core-trait vocabulary as a mask — every bit the core has assigned. `register_op`
// asserts `(spec.traits & ~kKnownTraitsMask) == 0` so a dialect cannot MINT a bit (the kLastEffectFamily parallel).
// ⛔ Append every new OpTrait here (and to ceir_opgen.py OP_TRAITS) — the drift is a silent future bit-collision.
inline constexpr u32 kKnownTraitsMask =
    flags_of(OpTrait::Terminator) | flags_of(OpTrait::Symbol) | flags_of(OpTrait::SymbolTable) |
    flags_of(OpTrait::Pure) | flags_of(OpTrait::IsolatedFromAbove) | flags_of(OpTrait::StateEdge) |
    flags_of(OpTrait::TokenProducer) | flags_of(OpTrait::TokenConsumer);
// Cross-language pin: 8 traits today (bits 0..7). If this fires, OpTrait and OP_TRAITS (ceir_opgen.py) diverged.
static_assert(kKnownTraitsMask == 0xFFU, "OpTrait must be 8 contiguous bits (0..7); sync kKnownTraitsMask + OP_TRAITS");

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
using EffectsFn = bool (*)(const Context&, const Operation&, const EffectQuery&, u64&); // u64 mask (CEIR-8c widened)

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
    // CEIR-8f (ADR-0116, U-§57): the host-granted capabilities this op-kind REQUIRES to run (arena-copied CapabilityId
    // set). Joins the §107 interface hash (a program's required set = the module-wide union). nullptr ⇒ requires none.
    // ⛔ Append at END.
    const CapabilityId*    required_capabilities = nullptr;
    u32                    num_capabilities       = 0U;
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
    // CEIR-8f (ADR-0116): required host-granted capability NAMES (borrowed StringViews; register_op interns them to
    // CapabilityId and arena-copies the set — the effects precedent). The opgen emits these from `[op.native] capabilities`.
    containers::ConstSpan<containers::StringView> capabilities = {}; // ⛔ Append at END.
};

// ── CEIR-8a open-world TYPE-CLASS registration (ADR-0111) — the type analogue of OpInfo/OpSpec ──
// A dialect-defined type-class's verify hook: is this `Extern` Type instance well-formed for its class? The registered
// class owns its own arity (the Context-free `type_is_well_formed` accepts Extern structurally — ADR-0111 §2.5).
// nullptr ⇒ the class declares no shape constraint (trivially valid).
using VerifyTypeFn = bool (*)(const Context&, const Type&);

// The registered record for a dialect-defined type-class (held on the Context, keyed by TypeClassId). ⛔ Append fields
// at END (the OpInfo discipline). `version` is stored in the binary record + range-checked on decode (ADR-0111 §2.4).
struct TypeClassInfo
{
    TypeClassId            id;
    containers::StringView name;              // "dialect.class"
    const Dialect*         dialect = nullptr;
    VerifyTypeFn           verify  = nullptr; // nullptr ⇒ trivially valid
    u32                    version = 1U;      // the class schema version
};

// The registration descriptor for `Dialect::register_type_class` (CEIR-8a). Every field defaults.
struct TypeClassSpec
{
    VerifyTypeFn verify  = nullptr;
    u32          version = 1U;
};

// ── CEIR-8b open-world ATTRIBUTE-CLASS registration (ADR-0112) — the attribute analogue of the type-class surface ──
using VerifyAttrFn = bool (*)(const Context&, const AttrValue&); // is this Extern AttrValue well-formed for its class?
struct AttrClassInfo
{
    AttrClassId            id;
    containers::StringView name;              // "dialect.attr"
    const Dialect*         dialect = nullptr;
    VerifyAttrFn           verify  = nullptr; // nullptr ⇒ trivially valid
    u32                    version = 1U;      // the class schema version (binary record range-check)
};
struct AttrClassSpec
{
    VerifyAttrFn verify  = nullptr;
    u32          version = 1U;
};

// ── CEIR-8c open-world effect-LOCATION-CLASS registration (ADR-0113) — the location analogue of the type/attr-class
// surface. A dialect-defined location class carries a DECLARED ResourceClass (the hazard analysis reads it to place
// the effect in a conflict class) + a verify hook (is this Extern EffectRecord well-formed for its class?). ──
using VerifyLocationFn = bool (*)(const Context&, const EffectRecord&);
struct LocationClassInfo
{
    LocationClassId        id;
    containers::StringView name;                              // "dialect.location"
    const Dialect*         dialect       = nullptr;
    VerifyLocationFn       verify        = nullptr;           // nullptr ⇒ trivially valid
    ResourceClass          resource_class = ResourceClass::Universe; // the conflict class (default = maximally conservative)
    u32                    version       = 1U;                // the class schema version
};
struct LocationClassSpec
{
    VerifyLocationFn verify         = nullptr;
    ResourceClass    resource_class = ResourceClass::Universe; // ⛔ default Universe: an under-declared class over-conflicts (safe)
    u32              version        = 1U;
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

    // Register type-class `cls` (interned as "<dialect>.<class>", CEIR-8a) from a `TypeClassSpec`. Returns its
    // `TypeClassId`. Idempotent by class — re-registering returns the same id and keeps the first registration's info
    // (late registration works — the id is a content hash; an unregistered Extern round-trips until its class registers).
    TypeClassId register_type_class(containers::StringView cls, const TypeClassSpec& spec = {});

    // Register attribute-class `cls` (interned as "<dialect>.<class>", CEIR-8b). Idempotent by class; late registration
    // works (an unregistered Extern attr round-trips until its class registers — the id is a content hash).
    AttrClassId register_attr_class(containers::StringView cls, const AttrClassSpec& spec = {});

    // Register effect-location-class `cls` (interned as "<dialect>.<location>", CEIR-8c). Idempotent by class; late
    // registration works (an op declaring an Extern location whose class is not yet registered is treated as
    // maximally-conflicting — ResourceClass::Universe — until its class registers with a declared resource_class).
    LocationClassId register_location_class(containers::StringView cls, const LocationClassSpec& spec = {});

private:
    Context*               m_ctx = nullptr;
    containers::StringView m_name;
};
} // namespace crd::ceir
