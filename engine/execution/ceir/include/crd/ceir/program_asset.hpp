#pragma once

// crd-ceir — the §105-§107 PROGRAM-AS-ASSET metadata (CEIR-7a): the INTERFACE HASH, the DEPENDENCY RECORD, and the
// strict cook-time registration check. ALL are PURE IR ANALYSIS (host-only — crd-ceir gains NO asset/CRDR/cook edge,
// ADR-0109 I5): interface_hash + collect_dependencies are semantic-identity computations, and ADR-0109 §5 makes the
// interface hash a crd-ceir concept (`KernelRef = {asset_id, interface_hash}`). The CRDR `'CEIR'` chunk + the ADR-0104
// cook cache live in the crd-ceir-cook BRIDGE. Mirrors the `stable_hash` (content hash) precedent in binary.hpp.

#include <crd/ceir/context.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::ceir
{
// §107 INTERFACE HASH — a DETERMINISTIC FNV-1a over the CANONICAL projection of a module's CALLER-VISIBLE contract: for
// each EXPORTED (Public) func symbol, SORTED BY NAME (⛔ never module-body order — a function reorder is an
// implementation edit, NOT an interface change), its {name · param types · result types · TRANSITIVE effective effect
// set · §20 state schema · §57 capability contract}. Types are encoded STRUCTURALLY (recursively, never a Context-local
// TypeId int) so the hash is cross-Context-stable, exactly like the binary blob. Effects are the 5c TRANSITIVE effective
// set over the body (a body edit that adds an effect VISIBLE TO CALLERS — e.g. Synchronization — IS an interface change
// per §107; one that does not is not). ⭐ CEIR-8d (ADR-0114 §2.7): §20 state cells are keyed by their StateEdge op's
// STABLE ID (sorted by id, the id VALUE hashed) — reorder-invariant, delete/re-add-sensitive; that projection is the
// 10a migration schema (see `collect_state_schema`). ⭐ CEIR-8f (ADR-0116 §2.1): the module-wide capability set IS folded
// in (a host must re-grant on a change). ⛔ Implementation-only bodies / private functions / constants are EXCLUDED, so
// an implementation-only edit leaves this hash UNCHANGED (the §107 hot-swap property). ⛔ CEIR-13c (forward RESOLVED, NOT
// folded): a program's KernelRef deps are DELIBERATELY excluded from this hash — WHICH kernel a program dispatches is
// IMPLEMENTATION, not caller-visible contract, so swapping kernel X for a contract-compatible Y must NOT invalidate the
// program's own callers (that is exactly the hot-swap property this hash provides). The program→kernel edge rides the §106
// DEPENDENCY record (`ckir_refs`) + the cook's declared-contract check instead. (A future §107 resource-I/O-SURFACE folding
// — the caller-visible resource contract — is a DISTINCT item, still unbuilt.) Field-by-field little-endian (the ⛔
// struct-padding-in-content-hash scar); version-sensitive like `stable_hash`.
[[nodiscard]] u64 interface_hash(Context& ctx, const Module& module, memory::IAllocator* scratch);

// §107 CONTRACT HASH (CEIR-10a) — the `interface_hash` projection MINUS the §20 state schema: the caller-visible
// signature (params · results · transitive effects, per exported func sorted by name) + the §57 capability contract.
// A hot-swap uses it to distinguish an UNMIGRATABLE contract change (signature/effects/caps — callers break ⇒ Reject)
// from a state-schema-ONLY change (a registered migration fn may cover it). Since `interface_hash` ≡ this projection PLUS
// the state schema, `contract_hash`-equal WITH `interface_hash`-differ ⇒ ONLY the state schema changed. PURE in-memory
// analysis — the cooked format is UNTOUCHED (no recook). Determinism/encoding identical to `interface_hash`.
[[nodiscard]] u64 contract_hash(Context& ctx, const Module& module, memory::IAllocator* scratch);

// A single §20 state cell in the module-wide migration SCHEMA (CEIR-10a): its 8d STABLE ID (the migration key — survives
// a hot-swap / round-trip, unlike an op pointer), its stored-value TYPE, and its §20 ring DEPTH.
struct StateCell
{
    u64    id;    // the StateEdge op's stable_id().value
    TypeId type;  // op->result(0)->type()
    u32    depth; // §20 ring depth (default 1)
};

// §108/§109 — the module-wide STATE-MIGRATION SCHEMA: every StateEdge cell's `(stable_id, type, depth)`, sorted by stable
// id. This is the SAME walk `interface_hash` folds into its projection (ONE source of truth). A 10a hot-swap compares two
// schemas (with the `contract_hash` split above) to decide CompatibleReuse / Migrate / Reject and to move cells by id.
// ⛔ Assigns stable ids on `module` first (idempotent). Allocated from `alloc`.
[[nodiscard]] containers::Array<StateCell> collect_state_schema(Context& ctx, const Module& module,
                                                                memory::IAllocator* alloc);

// One CKIR KernelRef dependency (CEIR-13c §85/§106): the kernel ASSET IDENTITY (`name` — the `[op.kernel_ref] symbol`
// attr's value) + the EXPECTED §107 interface hash the dispatch PINS (`interface_hash`, valid iff `pinned`). ⛔ `pinned`
// is a distinct bool, NOT `interface_hash == 0` (a real FNV hash can be 0 — the 8f phantom-id shape). The cook resolves
// `name` and, iff `pinned`, checks the resolved kernel's actual interface hash against `interface_hash` (declared-contract).
struct KernelRefDep
{
    containers::StringView name;
    u64                    interface_hash = 0U;
    bool                   pinned         = false;
};

// §106 DEPENDENCY RECORD — what a program asset REFERENCES. Extracted by an IR walk that is SCHEMA/REGISTRY-driven +
// symbol-driven, NEVER dialect-name-sniffing (the I6 open-world rule). All lists are DISTINCT + sorted (deterministic).
struct DependencyRecord
{
    containers::Array<containers::StringView> called_funcs; // distinct func.call callee symbol names
    containers::Array<containers::StringView> intrinsics;   // distinct op names of ops declared `[op.native]` (op_info.intrinsic)
    containers::Array<containers::StringView> providers;    // distinct native_provider names those intrinsics declare
    // ⭐ CEIR-13c (§64 forward LANDED): the distinct CKIR KernelRef asset deps of the program's dispatch ops — extracted
    // SCHEMA-DRIVEN from `op_info.kernel_ref_symbol` (the `[op.kernel_ref]` marker), NEVER by op-name-sniffing (I6). Sorted
    // by name; the first-seen pin is kept per name (the authoritative per-dispatch contract check re-walks the module, cook).
    // ⛔ "Which execution provider RUNS a region" is a §69 plan-time question (partitioner rows CEIR-21/26), NOT here.
    containers::Array<KernelRefDep>           ckir_refs;
    explicit DependencyRecord(memory::IAllocator* a) : called_funcs(a), intrinsics(a), providers(a), ckir_refs(a) {}
};
[[nodiscard]] DependencyRecord collect_dependencies(Context& ctx, const Module& module, memory::IAllocator* alloc);

// The strict COOK-TIME registration check (closes the 6a VACUOUS-PASS shape): the FIRST op in `module` (pre-order, incl.
// nested regions) whose kind has NO registered OpInfo (`op_info == nullptr`) — EMPTY≠UNKNOWN. A cook MUST reject an
// unregistered op, because the verifiers pass VACUOUSLY on a kind that declares no traits/effects. `nullptr` ⇒ every op
// kind is registered (the module is verifiable).
[[nodiscard]] const Operation* find_unregistered_op(const Context& ctx, const Module& module) noexcept;
} // namespace crd::ceir
