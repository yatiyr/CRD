#pragma once

// crd-ceir Context (CEIR-1a) — owns the IR arena + the op-kind name table, and is the ONLY factory for IR nodes.
//
// ── The arena mutation policy (the contract the whole IR is built on) ──
// Nodes are ARENA-allocated (a `crd::memory::GrowableLinearAllocator`) and NEVER freed individually. To edit the IR:
//   • change an operand     → `Operation::set_operand` (updates the def-use lists);
//   • move / insert an op   → `Block::insert_before` / `Block::append` (O(1) intrusive);
//   • remove an op          → `Operation::erase` (unlink + tombstone; its results must be use-free);
//   • grow an operand list  → rebuild the op; the old operand slice LEAKS into the arena BY DESIGN.
// Everything is reclaimed when the Context is destroyed. Node handles (`Operation*`/`Value*`/`Block*`/`Region*`)
// remain valid for the Context's entire life. Constructing the IR does NOT hit the parent allocator per op — the
// arena mallocs a chunk only ~once per thousands of nodes (a first chunk is reserved at construction).

#include <crd/ceir/attr.hpp>
#include <crd/ceir/detail/string_view_hash.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/hazard.hpp>
#include <crd/ceir/id.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/growable_linear_allocator.hpp>

namespace crd::ceir
{
// The pointing result of the CEIR-3z borrowed-view escape check: the FIRST value typed `!qual<borrow,_>` whose use
// leaves its defining region, paired with the exact escaping USE (its `Operation` carries the SourceLoc that points at
// the leak, §1c). `{nullptr, nullptr}` ⇒ no borrow escapes. A single pointing struct, matching the house pattern
// (ParseResult / SubstResult / DimMismatch / BroadcastResult all latch the first offender, never a diagnostic list).
struct BorrowEscape
{
    const Value*     value        = nullptr;
    const Operation* escaping_use = nullptr;
};

// The pointing result of the CEIR-4c domain-legality walk: the FIRST op that violates its enclosing region's execution
// tag. `region_owner` is the op whose `region_exec` set the constraint (it carries the SourceLoc to point the diagnostic
// at). `effect` is the offending §26 family (meaningful when `!unknown_kind`); `unknown_kind` marks the op as an
// UNREGISTERED kind — maximally effectful, flagged conservatively. `{nullptr,…}` ⇒ no violation. ⛔ A CORRUPT `region_exec`
// on a region-owner reports `{owner, owner, MemoryRead, false}` — `effect` is filler and the ONLY discriminator is
// `op == region_owner` (no effect-violation yields MemoryRead today, so the ambiguity is theoretical).
struct DomainViolation
{
    const Operation* op           = nullptr;
    const Operation* region_owner = nullptr;
    EffectFamily     effect       = EffectFamily::MemoryRead;
    bool             unknown_kind = false;
};

// One effect-derived ordering constraint (CEIR-4d): the LATER op (`after`) must not be reordered before the EARLIER op
// (`before`) because they share a resource with at least one write. `kind` is the strongest hazard over all their
// overlapping effect-pairs (WAW > RAW > WAR). The edge a scheduler (CEIR-12d) must preserve.
struct Hazard
{
    const Operation* before = nullptr;
    const Operation* after  = nullptr;
    HazardKind       kind   = HazardKind::None;
};

// ── CEIR-12c §78/§26: the resource ALIAS/LIFETIME analysis vocabulary — the effect-derived hazard model (CEIR-4d) gives
// ORDERING; this gives per-resource LIVE RANGES + the may-alias predicate the memory planner (CEIR-12d) colors. Ports the
// frame graph's greedy interval model (first_use/last_use over schedule positions; transients alias within a NON-ZERO
// (size_class, kind) bucket when their ranges are disjoint; persistent/history/unspecified get dedicated slots). ⛔ a view
// laundered through a region YIELD / call RESULT (its SSA identity no longer traces to its declare's result) escapes the
// view→root map — such a use is invisible here (an unbound alias-model refinement, shared with the 4d hazard walk). ──

// The §20 lifetime class a resource.declare's `lifetime` attr names (⛔ ABSENT ⇒ Unspecified, NOT Transient — 12b pinned
// absent as "the planner's default"; the may-alias predicate treats only an EXPLICIT Transient as aliasable).
// NOLINTNEXTLINE(performance-enum-size)
enum class ResourceLifetimeClass : u8
{
    Unspecified = 0, // no `lifetime` attr (or an unrecognized value — find_resource_intent_misuse rejects bad values first)
    Transient,       // lives within the frame; the ONLY class the aliaser may pool
    Persistent,      // lives across frames; a dedicated slot (never aliased)
    History,         // a persistent ring (`history<T>`); a dedicated slot
};

// One graph-owned resource's LIVE RANGE over a block's op list (the schedule-position proxy — real scheduling is §79).
// `first`/`last` are op POSITIONS (0-based); a resource is LIVE across [first, last]. `last` extends to the last op whose
// operand's ROOT resource is this one (following resource.view chains), to any op with an ambient Memory/Universe effect
// at-or-after `first` (the 4d conservative "more-live-never-less" rule), and to block-END when `exported` (external code
// may touch it past any position — the WAR-scar IR edition). Two resources INTERFERE iff their [first,last] overlap.
struct ResourceLifetime
{
    const Value*          resource = nullptr;                          // the resource.declare's result SSA Value (identity)
    const Operation*      declare  = nullptr;                          // the defining resource.declare op
    u32                   first    = 0;                                // the declare's op position (its definition site)
    u32                   last     = 0;                                // the last-use position (>= first)
    ResourceLifetimeClass lifetime = ResourceLifetimeClass::Unspecified;
    TypeKind              kind     = TypeKind::Buffer;                  // the CEIR-3c resource kind (buffers never alias images)
    i64                   size_class = 0;                              // the §78 `size_class` bucket (0 = unspecified — never pools)
    bool                  exported = false;                            // reached a resource.export ⇒ never aliasable
};

// ── CEIR-12d §78/§162: the memory PLANNER — colors 12c's live ranges into physical SLOTS. The "planner interface" is this
// inspectable `MemoryPlan` contract + the §78 `PlanProfile`; the "first planner" is the greedy interval-coloring (the frame
// graph's aliaser, ported). Downstream-consumed (§15d frame-compile → the CEIR-12 planner), so ADR-0124 pins the contract. ──

// §78 optimization profiles. ⛔ its name switch has ALL arms (append-at-end + -Werror=switch). Real TODAY: `Memory` (alias
// aggressively) vs `Latency` (no aliasing — preserve parallelism); `Balanced` (a parallelism-aware cost model) +
// `Deterministic` (cross-run stability guarantees) RIDE Memory's already-deterministic behavior this slice (named-forward,
// ADR-0124). Every arm produces defined behavior (aliasing = profile != Latency).
enum class PlanProfile : u8 // NOLINT(performance-enum-size)
{
    Memory = 0,
    Balanced,
    Latency,
    Deterministic,
};
[[nodiscard]] containers::StringView plan_profile_name(PlanProfile p) noexcept;

// WHY a resource got its slot (§162 "explain decisions" — the negative reasons matter as much as the positive). ⛔ ONE
// name switch, all arms. `Pooled` sets SlotAssignment::prior (the reused occupant); the rest leave it null.
enum class SlotReason : u8 // NOLINT(performance-enum-size)
{
    Pooled = 0,        // shares a slot with a prior DISJOINT poolable resource (the frame-graph alias — memory saved)
    NewPoolSlot,       // poolable-eligible; opened a fresh SHAREABLE slot (no existing bucket slot was free)
    DedicatedLifetime, // Persistent/History/Unspecified never pool (a history<T> ring records its depth on the slot)
    DedicatedExported, // reached resource.export -- external code may touch it past any position
    DedicatedUnsized,  // a transient with size_class 0 (an unknown size cannot be safely bucketed)
    DedicatedProfile,  // the Latency profile disabled pooling to preserve parallelism (a would-be-poolable resource)
};
[[nodiscard]] containers::StringView slot_reason_name(SlotReason r) noexcept;

// One physical slot in the plan (the inspectable unit — §162 "why N slots / why this big"). `dedicated` ⇒ never shared.
struct MemorySlot
{
    TypeKind kind           = TypeKind::Buffer;
    i64      size_class     = 0;
    bool     dedicated      = false;
    u32      first          = 0; // span start (min occupant `first`)
    u32      last           = 0; // span end (max occupant `last`)
    u32      occupant_count = 0;
    i64      history_length = 0; // >0 iff a history<T> ring realizes here (its depth = the memory MULTIPLE; §162)
};

// Where one resource landed + WHY (§162). `prior` is non-null ONLY when `reason == Pooled` (the occupant it reused).
struct SlotAssignment
{
    const Value* resource = nullptr;
    u32          slot     = 0;
    SlotReason   reason   = SlotReason::DedicatedLifetime;
    const Value* prior    = nullptr;
};

// The inspectable output of the CEIR-12d memory planner. `assignments` is in resource DECLARATION order (parallel to
// compute_block_lifetimes). `transient_logical`/`transient_physical` count ONLY poolable-eligible resources (Transient,
// non-exported, non-zero size_class): logical = the slots they'd need WITHOUT aliasing; physical = the slots they actually
// use. physical ≤ logical, STRICT iff any pooling happened — the REN-1 "aliasing saves memory" proof, IR edition (§78).
struct MemoryPlan
{
    containers::Array<MemorySlot>     slots;
    containers::Array<SlotAssignment> assignments;
    u32                               transient_logical  = 0;
    u32                               transient_physical = 0;
    PlanProfile                       profile            = PlanProfile::Memory;
    explicit MemoryPlan(memory::IAllocator* a) : slots(a), assignments(a) {}
};

class SymbolTable; // forward — the EffectQuery resolves callees against it (full def in symbol_table.hpp)

// The context threaded through an INSTANCE-DEPENDENT effect resolution (CEIR-5c, §34): a `func.call`'s effective effects
// are its callee's, found by resolving `symbols` and walking the callee body. `visited` guards RECURSION — a func already
// on the collection stack contributes nothing new (its effects are a union member up-stack), so a cyclic call graph
// terminates. Both borrowed; the public `effective_effects` builds them, the `EffectsFn` hook consumes them.
struct EffectQuery
{
    const SymbolTable*                         symbols = nullptr; // resolve callees; null ⇒ decline (static fallback)
    containers::HashMap<const Operation*, u8>* visited = nullptr; // funcs already collected (the recursion cycle guard)
};

// The §115 STRUCTURE-layer error kinds (CEIR-5b, extended by CEIR-5d §20). `UseBeforeDef`: an operand's value is not
// visible at the use — defined in a LATER block / a different region / never (a same-block forward reference is instead a
// FEEDBACK edge, see below). `CaptureThroughIsolation`: the value WOULD be visible from an enclosing scope but an
// `IsolatedFromAbove` boundary (e.g. func.func) blocks it. `MissingTerminator`: an SsaCfg-region block does not end with
// a Terminator-trait op. `TerminatorNotLast`: a Terminator op sits mid-block. `YieldCountMismatch`: a structured
// region's terminating `core.yield` operand count ≠ the region owner's expected count (owner result count, or a table
// override — `core.while`'s cond region yields exactly 1). **CEIR-5d §20:** `FeedbackWithoutState`: a same-block back-edge
// (an operand defined later in the SAME block) that is NOT a `StateEdge` op's feedback (last) operand — a combinational
// feedback cycle, illegal ("graph cycles must pass through explicit state/delay semantics"). `StateDepthInvalid`: a
// `StateEdge` op's optional `depth` attribute is present but not an Int ≥ 1.
// NOLINTNEXTLINE(performance-enum-size)
enum class StructureErrorKind : u8
{
    None = 0,
    UseBeforeDef,
    CaptureThroughIsolation,
    MissingTerminator,
    TerminatorNotLast,
    YieldCountMismatch,
    FeedbackWithoutState,
    StateDepthInvalid,
};
[[nodiscard]] containers::StringView structure_error_kind_name(StructureErrorKind k) noexcept;

// The pointing result of the CEIR-5b structure walk: the FIRST structural defect, its op, and (for a dominance error)
// the offending `value`. `{nullptr,…,None}` ⇒ structurally sound.
struct StructureError
{
    const Operation*   op    = nullptr;
    const Value*       value = nullptr; // the offending operand value (dominance kinds); nullptr otherwise
    StructureErrorKind kind  = StructureErrorKind::None;
};

// The §116 async-token-misuse kinds (CEIR-6a). A token (a result of a `TokenProducer` op) must be CONSUMED EXACTLY ONCE
// by a `TokenConsumer` op's operand slot. `Unconsumed`: 0 consuming uses (a leaked/dropped async op — no await XOR cancel).
// `MultiplyConsumed`: >1 consuming operand slots reference it (double-await / use-after-consume; `join(t, t)` counts as 2).
// `ConsumedByNonConsumer`: a token used as an operand of a NON-`TokenConsumer` op (→ structurally region-confined: a token
// yielded / passed to a func.call / fed to arith is caught here — §30 structured discipline falls out free).
// NOLINTNEXTLINE(performance-enum-size)
enum class TokenMisuseKind : u8
{
    None = 0,
    Unconsumed,
    MultiplyConsumed,
    ConsumedByNonConsumer,
};
[[nodiscard]] containers::StringView token_misuse_kind_name(TokenMisuseKind k) noexcept;

// The pointing result of the CEIR-6a token-misuse walk: the FIRST misused token (pre-order), the offending `op` (the
// bad USE for MultiplyConsumed/ConsumedByNonConsumer; the PRODUCER for Unconsumed), and the token `value`.
struct TokenMisuse
{
    const Value*    value = nullptr;
    const Operation* op   = nullptr;
    TokenMisuseKind kind  = TokenMisuseKind::None;
};

// CEIR-12a §36: the resource-dialect type-system ENFORCEMENT (the `find_token_misuse` house pattern) — a deterministic
// pre-order module walk enforcing the resource ops' typing that the per-op structural verify (arity/attr-kind) and the
// doc-only TOML `type` fields CANNOT: resource-ness rides the CEIR-3c resource TYPE, and this walk checks it cross-op.
// NOLINTNEXTLINE(performance-enum-size)
enum class ResourceMisuseKind : u8
{
    None = 0,
    ViewOperandNotViewable,   // resource.view operand(0) is not a VIEWABLE resource (must be a Buffer or Image -- the only
                              // kinds view_combination_valid admits; ⛔ rejects view-of-view + views over Sampler/Tensor/etc.)
    ViewResultNotView,        // resource.view result(0) is not a View type
    ViewUnderlyingMismatch,   // the view result's underlying type != operand(0)'s type
    ViewMaskInvalid,          // the view result's range mask is illegal for the underlying (view_combination_valid)
    ViewRangeArity,           // num_operands != 1 + 2*popcount(mask) (offset/size PAIRS per masked ViewRange dim)
    ExportOperandNotResource, // resource.export operand(0) is not a resource-kinded value
    DeclImportResultNotResource, // resource.declare / resource.import result(0) is not a resource-kinded value
};
[[nodiscard]] containers::StringView resource_misuse_kind_name(ResourceMisuseKind k) noexcept;

// The pointing result of the CEIR-12a resource-misuse walk: the FIRST misuse (pre-order), the offending `op`, and the
// `value` it points at (the bad operand / the malformed result).
struct ResourceMisuse
{
    const Value*       value = nullptr;
    const Operation*   op    = nullptr;
    ResourceMisuseKind kind  = ResourceMisuseKind::None;
};

// CEIR-12b §24/§25: the resource planning-INTENT attribute-vocabulary enforcement (a SEPARATE walk from the 12a TYPE
// contract — see find_resource_intent_misuse). Each CLOSED-vocabulary attr has ONE misuse kind: a wrong VALUE and a wrong
// attr-KIND both fold into it (the state-depth precedent `kind != Int || i < 1` → one kind, context.cpp). OPEN tags
// (streaming_priority/budget_class) mint no kind — they are unchecked (the 12d planner consumes them).
// NOLINTNEXTLINE(performance-enum-size)
enum class ResourceIntentMisuseKind : u8
{
    None = 0,
    LifetimeValueInvalid,        // resource.declare `lifetime` present but not a String in {transient, persistent, history}
    HistoryLengthInvalid,        // resource.declare `history_length` present but not an Int >= 1
    HistoryLengthWithoutHistory, // resource.declare `history_length` present while `lifetime` is not "history" (incl. absent)
    MemoryDomainValueInvalid,    // resource.declare `memory_domain` present but not a String in the §24 domain vocabulary
    ResidencyValueInvalid,       // resource.declare `residency` present but not a String in {resident, streamable, evictable}
    DirectionValueInvalid,       // resource.export `direction` present but not a String in {read, readwrite}
    IntentAttrOnImport,          // resource.import carries a declare-only planning-intent attr (imports are never planned —
                                 // nonsense-by-construction, the view-of-view legal-by-accident shape)
};
[[nodiscard]] containers::StringView resource_intent_misuse_kind_name(ResourceIntentMisuseKind k) noexcept;

// The pointing result of the CEIR-12b intent walk: the FIRST misuse (pre-order) and the offending `op` (the misuse is an
// ATTRIBUTE of the op — there is no bad Value to point at, unlike ResourceMisuse).
struct ResourceIntentMisuse
{
    const Operation*         op   = nullptr;
    ResourceIntentMisuseKind kind = ResourceIntentMisuseKind::None;
};

// CEIR-13a §42: the `ceir.compute` dispatch well-formedness ENFORCEMENT (the `find_token_misuse`/`find_resource_misuse`
// house pattern) — a deterministic module walk over compute.dispatch / compute.dispatch_indirect checking the binding
// contract the per-op structural verify (arity/attr-kind) cannot: the `access` string's tokens + arity, that each binding
// is a CEIR-3c resource, that a direct dispatch's grid operands are Index-typed, and that an indirect dispatch's args is a
// Buffer (or a View of one). Binding KIND/SLOT are DERIVED (type + position), not stored — nothing to verify there.
// NOLINTNEXTLINE(performance-enum-size)
enum class DispatchMisuseKind : u8
{
    None = 0,
    GridNotIndex,        // compute.dispatch grid operand (0..2) is not an Index-typed value
    AccessTokenInvalid,  // the `access` string has a token outside {r, w, rw} (or an empty/malformed token)
    AccessArityMismatch, // the `access` token count != the number of bindings (num_operands-3 direct / -1 indirect)
    BindingNotResource,  // a binding operand is not a resource-kinded value (the 12a ceir_is_resource_kind predicate)
    ArgsNotBuffer,       // compute.dispatch_indirect operand 0 (args) is not a Buffer nor a View of a Buffer
    KernelNotSymbol,     // the `kernel` @identity attr is absent or not a Symbol (CEIR-13c: identity became load-bearing —
                         // a standalone-robust fold, like the access kind-fold; checked FIRST — identity before contract)
};
[[nodiscard]] containers::StringView dispatch_misuse_kind_name(DispatchMisuseKind k) noexcept;

// The pointing result of the CEIR-13a dispatch walk: the FIRST misuse (pre-order), the offending `op`, and the `value` it
// points at (a bad grid/binding/args operand; null for an access-string misuse, which is an attribute of the op).
struct DispatchMisuse
{
    const Value*       value = nullptr;
    const Operation*   op    = nullptr;
    DispatchMisuseKind kind  = DispatchMisuseKind::None;
};

// CEIR-13b §50: the `ceir.transfer` well-formedness ENFORCEMENT (the find_resource/dispatch_misuse house pattern) — a
// deterministic module walk over the transfer ops checking the movement contract: every operand is TRANSFERABLE (a
// Buffer/Image, or a View of one — the 12a one-hop underlying); a copy's src != dst by SSA identity (a whole-resource
// self-copy is API-invalid; same-root-through-distinct-views is LEGAL); a clear's `value` fill word is Int + not on an
// image; a mip_gen operand is an Image. ⛔ opaque ExternalResource is NOT transferable (kind unknowable — import it typed).
// NOLINTNEXTLINE(performance-enum-size)
enum class TransferMisuseKind : u8
{
    None = 0,
    OperandNotTransferable, // a copy/upload/readback/clear operand is not a Buffer/Image nor a View of one
    CopySrcIsDst,           // transfer.copy src and dst are the SAME SSA Value (a whole-resource self-copy)
    MipGenNotImage,         // transfer.mip_gen operand (through a view one-hop) is not an Image
    ClearValueInvalid,      // transfer.clear `value` is present but not an Int (the fill word is a u32 pattern) — 13a fold
    ClearValueOnImage,      // transfer.clear `value` (a buffer FILL WORD) is on an IMAGE clear — a typed image clear is 13d
};
[[nodiscard]] containers::StringView transfer_misuse_kind_name(TransferMisuseKind k) noexcept;

// The pointing result of the CEIR-13b transfer walk: the FIRST misuse, the offending `op`, and the `value` it points at (a
// bad operand; null for a `value`-attribute misuse, which is an attribute of the op).
struct TransferMisuse
{
    const Value*       value = nullptr;
    const Operation*   op    = nullptr;
    TransferMisuseKind kind  = TransferMisuseKind::None;
};

class Context
{
public:
    explicit Context(memory::IAllocator* alloc, usize arena_chunk_bytes = 64U * 1024U);
    ~Context()                         = default;
    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&)                 = delete;
    Context& operator=(Context&&)      = delete;

    // Intern an op-kind identity: the FNV-1a hash of "dialect.op". The name is retained for diagnostics only.
    [[nodiscard]] OpId intern_op(containers::StringView dialect, containers::StringView name);
    // Reverse lookup for diagnostics — the "dialect.op" string, or "" if the id was never interned here.
    [[nodiscard]] containers::StringView op_name(OpId id) const noexcept;

    // ── CEIR-8a open-world TYPE classes (ADR-0111) ── intern a type-class identity (FNV of "dialect.class", mirroring
    // intern_op) + its reverse lookup. An `Extern` Type stores the id; the string survives serialization via STRP so an
    // unregistered decoder round-trips it. `Dialect::register_type_class` attaches the verify hook + version.
    [[nodiscard]] TypeClassId          intern_type_class(containers::StringView dialect, containers::StringView cls);
    [[nodiscard]] containers::StringView type_class_name(TypeClassId id) const noexcept;
    // The registered descriptor for `id`, or nullptr if the class is not registered in THIS Context (⇒ preserve opaquely
    // — the U-§56 unknown-plugin policy; EMPTY≠UNKNOWN — ADR-0111 §2.5).
    [[nodiscard]] const TypeClassInfo* type_class_info(TypeClassId id) const noexcept;

    // Arena-copy a symbol NAME into stable storage (SymbolTable + symbol-ref keys point here). Empty → empty. Not
    // deduplicated (symbols are few); the SymbolTable dedups by VALUE, so two copies of the same name still resolve.
    [[nodiscard]] containers::StringView intern_symbol(containers::StringView name);

    // ── Attributes (CEIR-1c, §7/§8) ── intern a typed attribute VALUE (identical values dedup to one AttrId), then
    // attach it to an op by name. The convenience makers intern the underlying text for String/SymbolRef.
    [[nodiscard]] AttrId intern_attr(const AttrValue& v);
    [[nodiscard]] AttrId attr_int(i64 v) { return intern_attr(AttrValue::of_int(v)); }
    [[nodiscard]] AttrId attr_float(f64 v) { return intern_attr(AttrValue::of_float(v)); }
    [[nodiscard]] AttrId attr_bool(bool v) { return intern_attr(AttrValue::of_bool(v)); }
    [[nodiscard]] AttrId attr_string(containers::StringView s) { return intern_attr(AttrValue::of_string(intern_symbol(s))); }
    [[nodiscard]] AttrId attr_symbol(containers::StringView s) { return intern_attr(AttrValue::of_symbol(intern_symbol(s))); }
    [[nodiscard]] AttrId attr_type(TypeId t) { return intern_attr(AttrValue::of_type(t)); }
    // ── CEIR-8b aggregate + wrapper + open-world attributes (ADR-0112) ── the makers intern the value (deep-copying the
    // borrowed spans into the arena, like the type factories). `attr_dict` CANONICALIZES its keys (byte-order sorted).
    // ⛔ a wrapper's payload must not itself be a wrapper (the composition rule); the factory asserts via intern_attr.
    [[nodiscard]] AttrId attr_array(containers::ConstSpan<AttrId> elems) { return intern_attr(AttrValue::of_array(elems)); }
    [[nodiscard]] AttrId attr_dict(containers::ConstSpan<containers::StringView> keys, containers::ConstSpan<AttrId> values);
    [[nodiscard]] AttrId attr_typed(TypeId ty, AttrId value);  // asserts the composition rule (payload not a wrapper)
    [[nodiscard]] AttrId attr_extern(AttrClassId cls, AttrId value); // asserts composition + the class verify hook
    // Intern an attribute-class identity (FNV "dialect.attr", mirroring intern_type_class) + reverse lookup + descriptor.
    [[nodiscard]] AttrClassId          intern_attr_class(containers::StringView dialect, containers::StringView cls);
    [[nodiscard]] containers::StringView attr_class_name(AttrClassId id) const noexcept;
    [[nodiscard]] const AttrClassInfo* attr_class_info(AttrClassId id) const noexcept;
    // Run the Extern attr's class verify hook (true if unregistered [preserve] / no hook / passes) — the decoder/parser
    // gate (⛔ `v.kind` must be Extern). Also enforces the no-wrapper-payload composition rule.
    [[nodiscard]] bool verify_attr_extern(const AttrValue& v) const noexcept;
    // Intern an effect-location-class identity (FNV "dialect.location", CEIR-8c) + reverse lookup + descriptor.
    [[nodiscard]] LocationClassId       intern_location_class(containers::StringView dialect, containers::StringView cls);
    [[nodiscard]] containers::StringView location_class_name(LocationClassId id) const noexcept;
    [[nodiscard]] const LocationClassInfo* location_class_info(LocationClassId id) const noexcept;
    // The effective conflict ResourceClass an EffectRecord touches (CEIR-8c): for an Extern location, its registered
    // class's declared resource_class — or ResourceClass::Universe when UNREGISTERED (EMPTY≠UNKNOWN, maximally
    // conflicting); for every other target kind, the family's own class (hazard.hpp::effect_access).
    [[nodiscard]] ResourceClass effect_resource_class(const EffectRecord& e) const noexcept;
    // Run an Extern effect-location's class verify hook (true if not Extern / unregistered [preserve] / no hook /
    // passes) — the register_op factory gate (CEIR-8c).
    [[nodiscard]] bool effect_location_valid(const EffectRecord& e) const noexcept;
    // CEIR-8d (ADR-0114): assign content-independent STABLE IDS to `m`'s ops — idempotent, module-scoped, pre-order:
    // find the current max id, then give every UNASSIGNED (id 0) op the next id in pre-order. Already-assigned ops are
    // never re-derived (survives edits/reorders). `const` + a `mutable` id field: assignment is memoization, so the
    // const `serialize`/`interface_hash` can give a never-persisted op a deterministic id. One routine, no drift.
    void assign_stable_ids(const Module& m) const noexcept;
    // The interned value behind `id` (by value — the intern table may reallocate; an invalid id yields Int(0)).
    [[nodiscard]] AttrValue attr_value(AttrId id) const noexcept;
    // Attach (or overwrite) attribute `name` = `value` on `op`. The name is interned; the op's dict grows by rebuild
    // (the old slice leaks into the arena, exactly like an operand-list grow — the CEIR-1a mutation policy).
    void set_attr(Operation* op, containers::StringView name, AttrId value);

    // ── Types (CEIR-3a, §16) ── intern a structural type (identical types dedup to one TypeId); the child spans are
    // arena-copied so the interned type owns stable storage. `type_of` reads one back (by value; the table may realloc).
    [[nodiscard]] TypeId intern_type(const Type& t);
    [[nodiscard]] Type   type_of(TypeId id) const noexcept;
    // Scalars.
    [[nodiscard]] TypeId type_bool() { return intern_type(Type::scalar(TypeKind::Bool)); }
    [[nodiscard]] TypeId type_index() { return intern_type(Type::scalar(TypeKind::Index)); }
    [[nodiscard]] TypeId type_int(u32 width, bool is_signed);
    [[nodiscard]] TypeId type_float(FloatKind fk);
    [[nodiscard]] TypeId type_i32() { return type_int(32U, true); }
    [[nodiscard]] TypeId type_i64() { return type_int(64U, true); }
    [[nodiscard]] TypeId type_f32() { return type_float(FloatKind::F32); }
    [[nodiscard]] TypeId type_f64() { return type_float(FloatKind::F64); }
    // Numeric aggregates.
    [[nodiscard]] TypeId type_vector(TypeId elem, u32 count);
    [[nodiscard]] TypeId type_matrix(TypeId elem, u32 rows, u32 cols);
    [[nodiscard]] TypeId type_complex(TypeId elem);
    [[nodiscard]] TypeId type_quaternion(TypeId elem);
    // General aggregates.
    [[nodiscard]] TypeId type_array(TypeId elem, u32 count);
    [[nodiscard]] TypeId type_tuple(containers::ConstSpan<TypeId> members);
    [[nodiscard]] TypeId type_struct(containers::StringView name, containers::ConstSpan<TypeId> field_types,
                                     containers::ConstSpan<containers::StringView> field_names);
    [[nodiscard]] TypeId type_enum(containers::StringView name, containers::ConstSpan<containers::StringView> cases);
    [[nodiscard]] TypeId type_variant(containers::ConstSpan<TypeId> alternatives);
    [[nodiscard]] TypeId type_option(TypeId elem);
    [[nodiscard]] TypeId type_result(TypeId ok, TypeId err);
    // Generics (CEIR-3b, §16/§98). A type PARAMETER = a name + the traits it must satisfy (its constraints); a TRAIT =
    // a name + its supertraits; a CALLABLE = params -> results. All interned structurally like every other type.
    [[nodiscard]] TypeId type_param(containers::StringView name, containers::ConstSpan<TypeId> constraints);
    [[nodiscard]] TypeId type_trait(containers::StringView name, containers::ConstSpan<TypeId> supertraits);
    [[nodiscard]] TypeId type_callable(containers::ConstSpan<TypeId> params, containers::ConstSpan<TypeId> results);
    // ── CEIR-8a open-world custom type (ADR-0111) ── build an `Extern` type of class `cls` whose PARAMETERS ride the
    // slots of `params` (its `members`/`count`/`cols`/`is_signed`/`fkind`/`name`/`labels`; `params.kind`/`type_class` are
    // ignored — set here). ⛔ The FACTORY boundary: if `cls` is registered its verify hook runs and this ASSERTS on
    // failure (builder misuse = programmer error); an unregistered class is accepted opaquely (preserve — U-§56). The
    // decoder/parser use `verify_extern` (reject, not assert) instead. Returns the interned TypeId.
    [[nodiscard]] TypeId type_extern(TypeClassId cls, const Type& params);
    // Run type-class `t.type_class`'s verify hook on the `Extern` type `t`. true iff the class is UNREGISTERED (preserve),
    // has no hook, or the hook passes — the decoder/parser gate (reject on false). ⛔ `t.kind` must be Extern.
    [[nodiscard]] bool   verify_extern(const Type& t) const noexcept;
    // True if `id` mentions any TypeParam (i.e. it is not fully ground). Recursive over the interned DAG.
    [[nodiscard]] bool   type_has_params(TypeId id) const noexcept;

    // ── Trait conformance + generic substitution (CEIR-3b) ── conformance is a live Context relation (NOT serialized;
    // when an `impl` op exists it will rebuild this on load, exactly as symbols rebuild from op attrs). `satisfies`
    // walks supertraits transitively (acyclic: a supertrait is interned before the trait that names it).
    void                 register_conformance(TypeId concrete, TypeId trait);
    [[nodiscard]] bool   satisfies(TypeId concrete, TypeId trait) const noexcept;
    // Substitute concrete types for type params in `id`. Each binding's concrete must `satisfy` every constraint on its
    // param, else the result reports the offending (param, trait). Unbound params REMAIN (the result stays generic).
    [[nodiscard]] SubstResult substitute(TypeId id, containers::ConstSpan<TypeBinding> bindings);

    // Resources + views (CEIR-3c, §23). A view's TYPE says WHICH range dimensions it constrains (a ViewRange mask), not
    // the runtime offsets/sizes. `view_combination_valid` is the tri-split check (the PARSER fail()s, the DECODER
    // rejects, the FACTORY asserts) — the mask must be legal for the underlying resource kind.
    [[nodiscard]] TypeId type_buffer(BufferMode mode, TypeId element = {});
    [[nodiscard]] TypeId type_image(ImageDim dim, TypeId format);
    [[nodiscard]] TypeId type_sampler(bool comparison);
    [[nodiscard]] TypeId type_resource_table(TypeId element);
    [[nodiscard]] TypeId type_accel_struct();
    [[nodiscard]] TypeId type_video_frame();
    [[nodiscard]] TypeId type_audio_buffer();
    [[nodiscard]] TypeId type_external_resource();
    [[nodiscard]] TypeId type_view(TypeId underlying, u32 range_mask);
    [[nodiscard]] bool   view_combination_valid(TypeId underlying, u32 range_mask) const noexcept;

    // Shapes + tensors (CEIR-3d, §21/§35). The type-level foundation; the `ceir.shape`/`ceir.tensor` value-op dialects +
    // layout (§22) land at CEIR-18. A dim is Static (extent) / Symbolic (name, ⛔ not the reserved "dyn") / Dynamic; a
    // Shape composes Dims; a Tensor composes [element, shape]. `shape_members_valid`/`tensor_composition_valid` are the
    // tri-split composition checks (parser fails / decoder rejects / factory asserts).
    [[nodiscard]] TypeId type_dim_static(u32 extent);
    [[nodiscard]] TypeId type_dim_symbolic(containers::StringView name);
    [[nodiscard]] TypeId type_dim_dynamic();
    [[nodiscard]] TypeId type_shape(containers::ConstSpan<TypeId> dims);
    [[nodiscard]] TypeId type_tensor(TypeId element, TypeId shape);
    [[nodiscard]] TypeId type_sparse_tensor(TypeId element, TypeId shape);
    [[nodiscard]] bool   shape_members_valid(containers::ConstSpan<TypeId> dims) const noexcept;
    [[nodiscard]] bool   tensor_composition_valid(TypeId element, TypeId shape) const noexcept;
    // Shape relations (§21) — tri-state (Unknown defers to a CEIR-18 runtime check). `shapes_broadcast` reports the
    // right-aligned position of the first incompatible dim; both take Shape TypeIds.
    [[nodiscard]] BroadcastResult shapes_broadcast(TypeId a, TypeId b) const noexcept;
    [[nodiscard]] ShapeCompat     shapes_reshape(TypeId a, TypeId b) const noexcept;

    // Physical quantities (CEIR-3e, §17/§18). A Quantity tags an underlying NUMERIC type with a physical DIMENSION
    // (8 SI base exponents, ADR-0078; §17 "dimensional errors caught before lowering"). `quantity_composition_valid` is
    // the tri-split check (parser fails / decoder rejects / factory asserts). The raw-boundary erase is the `units.erase`
    // op (authored through the CEIR-2 generator). `quantity_dimensions_equal` reports the first clashing base (§17
    // `Length+Time`); untagged-numeric enforcement on public domain-API ops wires in when those ops exist (CEIR-4+).
    [[nodiscard]] TypeId      type_quantity(TypeId underlying, const QuantityDim& dim);
    [[nodiscard]] bool        quantity_composition_valid(TypeId underlying) const noexcept;
    [[nodiscard]] QuantityDim quantity_dim_of(TypeId quantity) const noexcept;
    [[nodiscard]] DimMismatch quantity_dimensions_equal(TypeId a, TypeId b) const noexcept;

    // Ownership / lifetime qualifiers (CEIR-3f, §19). A Qualified type tags an underlying with an ownership category.
    // `qualified_composition_valid` is the tri-split check. `value_escapes_region` is the region-visible ESCAPE predicate
    // the CEIR-3z borrowed-view diagnostic composes (a borrow may not outlive its region — the borrow-scar, IR edition).
    [[nodiscard]] TypeId type_qualified(OwnershipKind kind, TypeId underlying);
    [[nodiscard]] bool   qualified_composition_valid(TypeId underlying) const noexcept;
    // The FIRST use of `v` that lies OUTSIDE `defining` and its descendant regions (nullptr ⇒ none escape). The pointing
    // companion to `value_escapes_region` — the returned `Operation` carries the SourceLoc a CEIR-3z/CEIR-4 diagnostic
    // points at. Pure IR analysis (def-use since 1a; regions nest via Region::parent_op). Uses no Context state.
    [[nodiscard]] const Operation* first_escaping_use(const Value* v, const Region* defining) const noexcept;
    // True iff any USE of `v` lies OUTSIDE `defining` and its descendant regions (== `first_escaping_use(...) != null`).
    [[nodiscard]] bool   value_escapes_region(const Value* v, const Region* defining) const noexcept;
    // CEIR-3z borrowed-view escape check: walk the module in deterministic pre-order (block args before op results,
    // recursing op regions — the printer's SSA-numbering order) and return the FIRST `!qual<borrow,_>` value whose use
    // escapes its defining region. The module-walk that composes `value_escapes_region` into a POINTING diagnostic — the
    // allocator-outlives-borrowers scar, IR edition. No producer op yet; the gate + CEIR-4 verifiers are the consumers.
    [[nodiscard]] BorrowEscape find_borrowed_escape(const Module& m) const noexcept;

    // ── Source map / provenance (CEIR-1c, §111) ── register a source file → a stable `file_id` (dedup by path;
    // 0 = unknown). Every op carries provenance via `SourceLoc{file_id,line,col}` from day one (`Operation::set_loc`).
    [[nodiscard]] u32                    register_file(containers::StringView path);
    [[nodiscard]] containers::StringView file_path(u32 file_id) const noexcept;

    // ── Dialect registry + traits/interfaces (CEIR-1d, §6/§7/§101) ── OPEN-WORLD: register dialects/ops/interfaces
    // without editing any central enum; the core dispatches through these, NEVER a switch on op.kind.
    [[nodiscard]] Dialect*       register_dialect(containers::StringView name);
    [[nodiscard]] Dialect*       dialect(containers::StringView name) noexcept;
    [[nodiscard]] const OpInfo*  op_info(OpId kind) const noexcept;
    [[nodiscard]] const Dialect* dialect_of(OpId kind) const noexcept; // nullptr ⇒ unregistered (unknown) op
    [[nodiscard]] bool           has_trait(OpId kind, OpTrait t) const noexcept;
    [[nodiscard]] bool           op_has_trait(const Operation& op, OpTrait t) const noexcept;
    // Verify `op` against its kind's registered verifier; true if the kind has no verifier (opaque/unknown ⇒ valid).
    [[nodiscard]] bool           verify(const Operation& op) const;
    // The §26 effects declared for `kind` (CEIR-4a; empty for a registered effect-free op). ⛔ EMPTY ≠ UNKNOWN: an
    // analysis MUST check `op_info(kind) != nullptr` FIRST — an UNREGISTERED kind (unknown dialect, §6.11) declares no
    // effects here yet is MAXIMALLY EFFECTFUL (assume it reads/writes everything), never reorderable. A registered kind
    // with an empty span is genuinely effect-free (⇒ typically `OpTrait::Pure`).
    [[nodiscard]] containers::ConstSpan<EffectRecord> op_effects(OpId kind) const noexcept;
    // ── CEIR-8f capabilities (ADR-0116, U-§57) ── an op-kind's REQUIRED host-granted capabilities (arena-copied set;
    // ⛔ EMPTY≠UNKNOWN: an unregistered kind returns {} here, but program_capabilities treats it as `external.process`).
    [[nodiscard]] containers::ConstSpan<CapabilityId> op_capabilities(OpId kind) const noexcept;
    // ── CEIR-8f safety axes (ADR-0116 §2.2, U-§23) ── the op-kind's may-allocate/may-block/may-IO (+ derived
    // realtime_safe), FOLDED over its declared effects (a PROJECTION — no new state). ⛔ EMPTY≠UNKNOWN: an unregistered
    // kind is MAXIMALLY unsafe (may-allocate+block+IO), never accidentally realtime-safe.
    [[nodiscard]] SafetyBits op_safety(OpId kind) const noexcept;
    // Intern a capability NAME (FNV, intern-only — no verify/version) + reverse lookup (diagnostics).
    [[nodiscard]] CapabilityId          intern_capability(containers::StringView name);
    [[nodiscard]] containers::StringView capability_name(CapabilityId id) const noexcept;
    // The PROGRAM's required-capability set: the module-wide (pre-order, every private callee) UNION of its ops'
    // required sets, SORTED + deduped. An UNREGISTERED op contributes `external.process` (EMPTY≠UNKNOWN). Fills `out`.
    void program_capabilities(const Module& m, containers::Array<CapabilityId>& out) const;
    // true iff every `required` capability is in `granted` (the host-grant check; both sets are CapabilityId spans).
    [[nodiscard]] static bool capabilities_satisfied(containers::ConstSpan<CapabilityId> required,
                                                     containers::ConstSpan<CapabilityId> granted) noexcept;
    // The §27 determinism class declared for `kind` (CEIR-4b). ⛔ Same EMPTY≠UNKNOWN discipline as op_effects: an
    // UNREGISTERED kind returns Unspecified but is actually UNKNOWN — check `op_info(kind)!=nullptr` first.
    [[nodiscard]] DeterminismClass op_determinism(OpId kind) const noexcept;
    // The §15 evaluation domain declared for `kind` (CEIR-4c; the op-kind's domain affinity). ⛔ EMPTY≠UNKNOWN again.
    // NOTE: the CEIR-4c domain-legality walk does NOT yet consume this kind-domain — kind-domain × region-domain placement
    // checking is an op-verifier concern (CEIR-6-ish); the §15 partial-evaluation use is later still.
    [[nodiscard]] EvalDomain op_domain(OpId kind) const noexcept;

    // ── §27/§28 compiler mode + contract enforcement (CEIR-4b) ── the active mode is SESSION state, never serialized (it
    // does not change a module's content hash). `find_mode_violation` walks the module in pre-order and returns the FIRST
    // op that violates the active mode's contract (nullptr ⇒ none) — either its determinism class fails the mode (§27) OR
    // its per-instance numerics do (§28: a legal-but-forbidden knob like fast_math under Certified), OR its `numerics`
    // attr is CORRUPT (a violation in EVERY mode, Normal included — garbage isn't a constraint question). The pointing
    // enforcement primitive an optimization pass will call (no pass manager until CEIR-6; §27 replay/deterministic
    // alternatives are runtime/executor concerns, CEIR-5+).
    void                            set_compiler_mode(CompilerMode m) noexcept { m_compiler_mode = m; }
    [[nodiscard]] CompilerMode      compiler_mode() const noexcept { return m_compiler_mode; }
    [[nodiscard]] const Operation*  find_mode_violation(const Module& m) const noexcept;

    // ── §28 per-instance numerical semantics (CEIR-4b) ── carried as a single packed `numerics` int ATTRIBUTE (rides the
    // existing attr text/binary machinery — no new grammar). `set_numerics` writes it; `op_numerics` reads+validates it
    // (returns false iff the attr is present but CORRUPT — an out-of-range packed field; ABSENT ⇒ all-Inherit default, true).
    void                            set_numerics(Operation* op, const NumericalSemantics& n);
    [[nodiscard]] bool              op_numerics(const Operation& op, NumericalSemantics& out) const noexcept;

    // ── §15/§32 region execution tag + domain-legality verifier (CEIR-4c) ── a region's (§15 domain + §32 realtime)
    // rides a packed `region_exec` int ATTRIBUTE on the region-OWNING op (regions have no attr dict; module CONTENT, so
    // it survives serialization — unlike the session-only compiler mode). `set_region_exec` writes it on a region-owner;
    // `op_region_exec` reads+validates it (false ⇒ present-but-corrupt; absent ⇒ Unspecified default, true). ⛔ PRESENT-
    // EMPTY ≠ ABSENT: `set_region_exec(owner, {})` writes a PRESENT all-Unspecified tag, which the walk treats as an
    // explicit UNCONSTRAIN OVERRIDE for the subtree (innermost-wins); an ABSENT attr INHERITS the enclosing tag.
    void                            set_region_exec(Operation* region_owner, const RegionExec& r);
    [[nodiscard]] bool              op_region_exec(const Operation& op, RegionExec& out) const noexcept;
    // Walk the module and return the FIRST op that violates its EFFECTIVE region tag (§32 seeded rule: a FileIO/NetworkIO
    // — and now (CEIR-5c) an unmodeled `ExternalCall` — effect in an audio-real-time region). The tag is INNERMOST-wins (an
    // owner's tag replaces its parent's for its subtree; the module body is untagged/unconstrained). ⛔ EMPTY≠UNKNOWN: an
    // UNREGISTERED op in a tagged region is MAXIMALLY effectful ⇒ flagged (`unknown_kind`); a registered effect-free op is
    // legal. ⭐ CEIR-5c: a `func.call`'s effects are its CALLEE's (resolved through `m.symbols()` via the EffectsFn hook),
    // so a call to a PURE func is now legal in an audio region and a call to a FileIO func is flagged — the 4c "ExternalCall
    // stays legal-for-now" gap CLOSED. NOT noexcept (resolving a call allocates a scratch visited map). Separate from
    // `find_mode_violation` (mode = session state; the region tag = module content) — the 4z GATE TEST composes them.
    [[nodiscard]] DomainViolation   find_domain_violation(const Module& m) const;

    // ── §34 callee-derived effects (CEIR-5c, the CEIR-4a "EffectsFn" landing) ── `effective_effects` is the INSTANCE-level
    // effect set of one op: for most ops its static §26 records; for a `func.call` its callee's effects, resolved through
    // `table` and unioned TRANSITIVELY across the callee body (nested regions + further calls), with a recursion cycle
    // guard. ⛔ Lifted callee effects become AMBIENT `EffectRecord{family, None, 0, 0}` — a callee's operand/result target
    // names the CALLEE's position, meaningless at the call site, so the honest lift drops resource identity to whole-class
    // (inter-procedural aliasing is a far-later band). ⛔ DEGRADES to a full `ExternalCall` barrier if ANY transitively-
    // reached callee is unresolved OR any reached op is unregistered (the 4a "registered-empty reads as provably-none"
    // landmine, interprocedural). `collect_effective_mask` (one op's own contribution — hook or static) + `collect_region_
    // effective_mask` (the recursive callee-body walker) are the shared `u32`-bitmask core the hook re-enters.
    void collect_effective_mask(const Operation& op, const EffectQuery& q, u64& mask) const;   // u64 mask (CEIR-8c)
    void collect_region_effective_mask(const Region& r, const EffectQuery& q, u64& mask) const; // u64 mask (CEIR-8c)
    void effective_effects(const Operation& op, const SymbolTable& table, containers::Array<EffectRecord>& out) const;

    // ── §26/§116 effect-derived ordering hazards (CEIR-4d) ── compose the two ops' 4a effects into an ordering
    // constraint. `ops_hazard(before, after)` is a PURE, DIRECTIONAL query (the caller asserts `before` precedes `after`):
    // the strongest hazard (WAW>RAW>WAR) over every pair of their effects that share a resource + overlapping range with
    // ≥1 write; `None` ⇒ freely reorderable. ⛔ EMPTY≠UNKNOWN: an UNREGISTERED op reads+writes `Universe` (hazards
    // everything); a registered EFFECT-FREE (Pure) op touches nothing ⇒ `None` vs anything. ⛔ DISTINCT SSA Values are
    // assumed NON-ALIASING — ⛔ SUPERSEDED (CEIR-13d part 3, DONE): 12a minted `resource.view` and 12c built a view→root map,
    // and the hazard walk NOW consults it — `op_access_at` normalizes every captured resource through `resource_root`, so
    // `write(%buf)` vs `read(view(%buf))` is a REAL hazard (no longer a false-negative). ⛔ a view laundered through a region
    // yield / call result STILL escapes BOTH this walk and the 12c lifetime analysis (its `defining_op` is not a
    // `resource.view` — an unbound inter-op alias-model refinement). `collect_block_hazards` is the O(n²) all-pairs CORRECTNESS reference over ONE block's ops in LIST (authored-
    // linearization) order — the edges a scheduler must preserve; no transitive reduction (that is the scheduler's).
    // Cross-block / loop-carried hazards need the CFG (CEIR-5b) — not here. ⭐ CEIR-5c: the `table`-taking OVERLOADS use a
    // call's callee-DERIVED effects (a call to a pure func is reorderable; the no-table paths keep the conservative
    // `ExternalCall`-barrier baseline — the A/B pair). Lifted call effects are whole-class (ambient) as above.
    // CEIR-13d part 3 (§78/§116): the underlying resource `v` names — follow `resource.view` chains to the root resource so
    // `write(%buf)` vs `read(view(%buf))` is a REAL hazard (the 12c view→root map, on-demand via `defining_op`). Returns `v`
    // unchanged when it is not a view (or is a block arg). ⛔ a view LAUNDERED through a region yield / call result (its
    // defining op is not a `resource.view`) is NOT resolved — a deeper alias-model hole, still forwarded. Used by the 4d
    // access-gather so hazards + the crd-ceir-gpu barrier lowering both see through views; ⛔ resolves IDENTITY only, not the
    // view's byte RANGE (a disjoint-view refinement is a later slice — normalizing to the root is conservative-safe here).
    [[nodiscard]] const Value* resource_root(const Value* v) const noexcept;

    [[nodiscard]] HazardKind ops_hazard(const Operation& before, const Operation& after) const noexcept;
    [[nodiscard]] HazardKind ops_hazard(const Operation& before, const Operation& after, const SymbolTable& table) const;
    void collect_block_hazards(const Block& b, containers::Array<Hazard>& out) const;
    void collect_block_hazards(const Block& b, const SymbolTable& table, containers::Array<Hazard>& out) const;

    // ── CEIR-12c §78: the resource ALIAS/LIFETIME analysis (over the same one-block LIST-order scope as the hazards) ──
    // Enumerate the GRAPH-OWNED resources (resource.declare results; resource.import is EXCLUDED — never planned) of `b`
    // and compute each one's live range (ResourceLifetime). `last` follows resource.view chains to the root, extends to any
    // op with an ambient Memory/Universe effect at-or-after the declare (the 4d conservative rule), pins to block-end when
    // the resource is exported, and — the nested-region fix — an operand USE inside a nested region is attributed to the
    // CONTAINING op's position (the "pass"). Appends in declaration order. Allocates scratch (a Value*→root map); not
    // noexcept. ⛔ per-block, like collect_block_hazards — cross-block/loop-carried ranges need the CFG (a later refinement).
    void compute_block_lifetimes(const Block& b, containers::Array<ResourceLifetime>& out) const;

    // Two resources INTERFERE iff their [first, last] live ranges OVERLAP (they cannot share memory). A pure interval test.
    [[nodiscard]] static bool resources_interfere(const ResourceLifetime& a, const ResourceLifetime& b) noexcept;

    // Whether `a` and `b` MAY share one memory slot (the predicate the CEIR-12d planner colors): BOTH explicitly Transient
    // (Unspecified/Persistent/History never pool — the conservative direction), NEITHER exported, the SAME resource kind
    // AND the SAME NON-ZERO size_class bucket (the slot-SIZE scar — size_class 0/unspecified never pools), and
    // NON-interfering live ranges. ⛔ a de-facto-persistent (Unspecified) or unsized resource is refused, not aliased — a
    // wrong alias is a correctness bug, a refused one only a pessimization (12d relaxes per profile). Symmetric; a resource
    // never aliases itself (it always self-interferes).
    [[nodiscard]] static bool resources_may_alias(const ResourceLifetime& a, const ResourceLifetime& b) noexcept;

    // ── CEIR-12d §78/§162: the memory PLANNER — color a block's 12c live ranges into physical slots (`out` is CLEARED and
    // filled; the out-param house pattern of the hazard/lifetime collectors). `profile` selects the strategy (§78): Memory
    // aliases via resources_may_alias (greedy interval-coloring — provably minimal, the input is start-sorted by
    // construction), Latency dedicates a slot per resource (preserves parallelism). ⛔ every co-slotted pair satisfies
    // resources_may_alias (the plan↔predicate consistency invariant — holds by construction). Allocates scratch; not
    // noexcept. The plan is inspectable (per-slot occupancy + per-assignment SlotReason) — §162. ADR-0124 pins the contract.
    void plan_block_memory(const Block& b, PlanProfile profile, MemoryPlan& out) const;

    // ── CEIR-5a structured control flow: canonicalization ── the constant-condition `if` fold (the partial-eval seed).
    // If `if_op` is a `core.if` whose condition is a constant `arith.const`, splice the TAKEN region's single block into
    // the parent block before the `if` and erase the `if` — the non-taken branch vanishes. Returns true iff it folded.
    // ⛔ BAILS (false, NO mutation) on: not a `core.if`; a non-constant condition; a multi-block taken region; a missing
    // `core.yield` terminator; OR a `region_exec`-tagged `if` (CEIR-4c — inlining would silently DELETE the region's
    // domain/realtime constraint — the first place bands 4 and 5 touch). (Value forwarding — RAUW the if's results with
    // the yield's operands — arrives with the value-producing variants; CEIR-5a remaining.)
    [[nodiscard]] bool fold_constant_if(Operation* if_op);

    // ── CEIR-5b the §115 STRUCTURE-layer verifier ── one pre-order walk returning the FIRST structural defect: SSA
    // dominance (single-block def-before-use), capture visibility across nested regions (blocked by `IsolatedFromAbove`),
    // block-arg defs, terminator rules by RegionKind (an SsaCfg block must END with a Terminator), and the general
    // yield↔owner count contract (the 3f `parent_op` back-link's payoff). ⛔ SCOPE: the STRUCTURE layer of §115 ONLY —
    // the TYPE layer is CEIR-6, ownership/effects/domain are bands 3–4 predicates; and dominance is over the CURRENT
    // single-block regions — the multi-block dominator tree lands with the (not-yet-existing) cf branch dialect. Allocates
    // scratch visibility sets (hence not noexcept); does not mutate the module.
    [[nodiscard]] StructureError find_structure_error(const Module& m) const;

    // ── CEIR-6a §116 async-token-misuse verifier ── one pre-order walk returning the FIRST misused token: every token (a
    // result of a `TokenProducer` op) must be CONSUMED EXACTLY ONCE by a `TokenConsumer` op's operand slot. `join(t, t)`
    // counts as TWO consuming slots (MultiplyConsumed). Trait-keyed (open-world — the core never name-checks async); the
    // §116 taxonomy, a linear use-once MECHANISM (4d's read/write hazards cannot express "exactly once"). ⛔ Conscious
    // conservative edges (each documented + tested): a token consumed in BOTH `if` branches = 2 static uses = flagged
    // (no path-sensitivity substrate); a token yielded out of a region / passed to `func.call` / fed to arith = flagged
    // ConsumedByNonConsumer (⇒ tokens are region- AND function-CONFINED — §30 structured concurrency falls out free);
    // produce-outside / consume-inside a LOOP = 1 static use = passes (dynamic multiplicity needs execution — a gap).
    // Inherits the 5d register-to-verify contract (traits are registry state). Allocates scratch (hence not noexcept).
    [[nodiscard]] TokenMisuse find_token_misuse(const Module& m) const;

    // CEIR-12a §36: the resource-dialect type-system enforcement — a pre-order walk returning the FIRST resource.view /
    // resource.export / declare / import op whose typing is malformed (see ResourceMisuseKind), or {None}. Reads the
    // CEIR-3c resource TYPES (Buffer/Image/View/... via `type_of` + `view_combination_valid`); resource-ness rides the
    // type, not a trait. ⛔ Assumes structural validity (run `find_structure_error` first — the structural verify checks
    // op arity/attr-kinds; this walk checks the cross-op TYPE contract the doc-only TOML types cannot).
    [[nodiscard]] ResourceMisuse find_resource_misuse(const Module& m) const noexcept;

    // CEIR-12b §24/§25: the resource planning-INTENT attribute-vocabulary enforcement — a SEPARATE pre-order walk from
    // find_resource_misuse (12a's TYPE contract), returning the FIRST resource.declare / resource.export / resource.import
    // op whose §20 lifetime / §24 memory-domain / §25 residency / export-direction attr is malformed (see
    // ResourceIntentMisuseKind), or {None}. Enforces the CLOSED vocabularies (wrong VALUE or wrong KIND folds into one kind
    // per attr — the state-depth precedent); OPEN tags (streaming_priority/budget_class) are unchecked. ⛔ Kept separate so
    // 12a's contractual check order + its pinned negatives stay untouched (the widen-enum-audit scar).
    [[nodiscard]] ResourceIntentMisuse find_resource_intent_misuse(const Module& m) const noexcept;

    // CEIR-13a §42: the `ceir.compute` dispatch well-formedness check — a pre-order walk returning the FIRST
    // compute.dispatch / compute.dispatch_indirect op whose binding contract is malformed (see DispatchMisuseKind), or
    // {None}. Reads the CEIR-3c operand TYPES + the `access` string; binding KIND/SLOT are derived (type + position).
    // ⛔ Assumes structural validity (run the per-op verify first — it guarantees the required kernel/access attrs + the
    // min operand arity; this walk checks the cross-operand + access-string contract those cannot).
    [[nodiscard]] DispatchMisuse find_dispatch_misuse(const Module& m) const noexcept;

    // CEIR-13b §50: the `ceir.transfer` well-formedness check — a pre-order walk returning the FIRST transfer op whose
    // movement contract is malformed (see TransferMisuseKind), or {None}. Reads the CEIR-3c operand TYPES (transferable =
    // Buffer/Image or a View one-hop) + the clear `value` attr. ⛔ Assumes structural validity; robust to a raw
    // deserialized module for the `value` kind-fold (like find_dispatch_misuse).
    [[nodiscard]] TransferMisuse find_transfer_misuse(const Module& m) const noexcept;

    // Interfaces: intern a name, register/query an op-kind's implementation (an opaque function-table pointer).
    [[nodiscard]] InterfaceId    intern_interface(containers::StringView name); // CEIR-8e: an FNV of the name (== T::kId)
    [[nodiscard]] containers::StringView interface_name(InterfaceId id) const noexcept; // reverse lookup (diagnostics)
    void                         register_interface(OpId kind, InterfaceId iface, const void* impl);
    [[nodiscard]] const void*    get_interface(OpId kind, InterfaceId iface) const noexcept;

    // Factories (the fluent `ModuleBuilder` is CEIR-1g). Each returns a stable arena handle.
    [[nodiscard]] Module*    create_module(RegionKind body_kind = RegionKind::Graph);
    [[nodiscard]] Region*    create_region(RegionKind kind = RegionKind::Graph);
    [[nodiscard]] Block*     create_block(u32 num_args = 0U, TypeId arg_type = {});
    [[nodiscard]] Operation* create_operation(OpId kind, containers::ConstSpan<Value*> operands, u32 num_results,
                                              TypeId result_type = {}, u32 num_regions = 0U);

    // Retag a region's kind (Graph vs SsaCfg). For CONSTRUCTION / DESERIALIZATION only — `create_operation` makes its
    // regions Graph, so the binary loader (CEIR-1f) uses this to restore a region's serialized kind. Cheap (Context is
    // a friend of Region); do not use it to mutate a region mid-analysis.
    void set_region_kind(Region* r, RegionKind kind) noexcept;

    // Set an op's CEIR-8d stable id. For DESERIALIZATION only (the STID-chunk loader) — Context is a friend of Operation.
    // ⛔ Not for mutation mid-analysis: a stable id is one-time (see assign_stable_ids). 0 is rejected by the loader.
    // Uniqueness is the caller's contract (the decoder enforces it; assign_stable_ids never produces a duplicate).
    void set_stable_id(Operation* op, StableId id) noexcept;
    // Restore a module's CEIR-8d id high-water mark (the STID-chunk loader) — so a post-load edit never reuses a freed id.
    void set_stable_id_watermark(Module* m, u64 watermark) noexcept;

    // ── CEIR-8i (ADR-0119) transaction support ── the privileged inverse/rebuild atoms the `Transaction` recorder
    // (transaction.hpp) routes through so ALL raw mutation stays inside Context (never a second mutation implementation).
    // ⛔ TRANSACTION-ONLY (like set_stable_id's deserialization-only contract) — the authored-edit surface is `Transaction`,
    // not these. ⛔ Every restore PAYLOAD the caller passes (operand snapshot, attr dict) must be CONTEXT-ARENA-owned: a
    // rolled-back attr dict BECOMES live module state and outlives the Transaction.
    //
    // The inverse of Operation::erase — restore `op`'s operand count, re-thread `operands` into its (arena-preserved) Use
    // slots, re-link before `before` (nullptr ⇒ append) in `block`, clear the tombstone.
    void reinsert_erased_op(Operation* op, Block* block, Operation* before, containers::ConstSpan<Value*> operands) noexcept;
    // Repoint one Use at `value` (unlink from its current value's use-list, relink into `value`'s) — the forward+inverse
    // RAUW atom. `value` may be null (detach only).
    void detach_and_point_use(Use* u, Value* value) noexcept;
    // Walk `from`'s use-list, capture every Use* into `moved`, and repoint each at `to` — RAUW with a recorded moved-set
    // (the recorder pushes one detach_and_point_use inverse per captured use, so RAUW reuses the set_operand-style inverse).
    void rauw_recording(Value* from, Value* to, containers::Array<Use*>& moved);
    // The inverse of set_attr — restore `op`'s attribute dict to a prior (Context-arena) snapshot; uniform across the
    // overwrite-in-place and grow-by-rebuild branches of set_attr.
    void restore_attr_dict(Operation* op, NamedAttr* dict, u32 count) noexcept;
    // Rebuild `m`'s SymbolTable from its ops (the detail::register_symbol path the parser/decoder share — never a second
    // path), swapping in a fresh table ONLY on success. On the FIRST duplicate sym_name it mutates NOTHING, sets
    // `first_dup`, and returns false — the commit-verify hook (a transaction that introduces a duplicate symbol fails).
    [[nodiscard]] bool resync_symbols(Module& m, const Operation*& first_dup);
    // Resolve a CEIR-8d StableId to its op in `m` (pre-order; nullptr ⇒ none) — the "reference an op by stable id"
    // surface a Transaction uses so a reference survives an edit (never a pointer/pre-order that a concurrent edit voids).
    [[nodiscard]] const Operation* find_by_stable_id(const Module& m, StableId id) const noexcept;

    [[nodiscard]] memory::IAllocator*             allocator() const noexcept { return m_arena.parent(); }
    [[nodiscard]] memory::GrowableLinearAllocator& arena() noexcept { return m_arena; }

private:
    friend class Dialect; // Dialect::register_op registers an OpInfo on its owning Context
    // CEIR-8d recursion helpers for assign_stable_ids (Context is a friend of Operation, so these can read/set the id).
    void stable_id_scan_max(Region* r, u64& mx) const noexcept;   // find the current max stable id in pre-order
    void stable_id_assign_unset(Region* r, u64& next) const noexcept; // give every id-0 op the next id in pre-order

    struct OpName
    {
        u64                    hash = 0;
        containers::StringView name;
    };

    memory::GrowableLinearAllocator           m_arena;
    containers::Array<OpName>                  m_op_names;
    containers::Array<OpName>                  m_type_class_names; // CEIR-8a: TypeClassId → "dialect.class" (OpName reuse)
    containers::Array<OpName>                  m_attr_class_names; // CEIR-8b: AttrClassId → "dialect.attr" (OpName reuse)
    containers::Array<OpName>                  m_location_class_names; // CEIR-8c: LocationClassId → "dialect.location"
    containers::Array<AttrValue>               m_attr_values; // attribute-value intern table (AttrId.value = index+1)
    containers::Array<Type>                    m_types;        // type intern table (TypeId.value = index+1; 0 = none)
    struct Conformance { u32 concrete = 0; u32 trait = 0; };   // a registered (concrete satisfies trait) fact
    containers::Array<Conformance>             m_conformances; // live trait-conformance relation (CEIR-3b; not serialized)
    containers::Array<containers::StringView>  m_files;        // source map (file_id = index+1; each path arena-interned)
    containers::HashMap<containers::StringView, Dialect*, detail::StringViewHash> m_dialects; // name → dialect
    containers::HashMap<u64, OpInfo*>          m_op_infos;         // OpId.value → its ODS-lite descriptor
    containers::HashMap<u64, TypeClassInfo*>   m_type_classes;     // CEIR-8a: TypeClassId.value → its type-class descriptor
    containers::HashMap<u64, AttrClassInfo*>   m_attr_classes;     // CEIR-8b: AttrClassId.value → its attribute-class descriptor
    containers::HashMap<u64, LocationClassInfo*> m_location_classes; // CEIR-8c: LocationClassId.value → its location-class descriptor
    containers::Array<OpName>                  m_interface_names;  // CEIR-8e: InterfaceId (FNV) → name (reverse lookup)
    containers::Array<OpName>                  m_capability_names; // CEIR-8f: CapabilityId (FNV) → name (reverse lookup)
    CompilerMode                               m_compiler_mode = CompilerMode::Normal; // §27 active mode (session state)
};
} // namespace crd::ceir
