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
// set · §20 state schema}. Types are encoded STRUCTURALLY (recursively, never a Context-local TypeId int) so the hash is
// cross-Context-stable, exactly like the binary blob. Effects are the 5c TRANSITIVE effective set over the body (a body
// edit that adds an effect VISIBLE TO CALLERS — e.g. Synchronization — IS an interface change per §107; one that does
// not is not). State cells stay in BODY order — layout order IS the migration schema (§108/7c). ⛔ Implementation-only
// bodies / private functions / constants are EXCLUDED, so an implementation-only edit leaves this hash UNCHANGED (the
// §107 hot-swap property). NAMED-FORWARD: §107's capability contract + resource contract are unbuilt (capabilities have
// no owning row yet — flagged; resources are CEIR-9) — folding them in later changes the hash (a recook, honest). Field-
// by-field little-endian (the ⛔ struct-padding-in-content-hash scar); version-sensitive like `stable_hash`.
[[nodiscard]] u64 interface_hash(Context& ctx, const Module& module, memory::IAllocator* scratch);

// §106 DEPENDENCY RECORD — what a program asset REFERENCES. Extracted by an IR walk that is SCHEMA/REGISTRY-driven +
// symbol-driven, NEVER dialect-name-sniffing (the I6 open-world rule). All lists are DISTINCT + sorted (deterministic).
struct DependencyRecord
{
    containers::Array<containers::StringView> called_funcs; // distinct func.call callee symbol names
    containers::Array<containers::StringView> intrinsics;   // distinct op names of ops declared `[op.native]` (op_info.intrinsic)
    containers::Array<containers::StringView> providers;    // distinct native_provider names those intrinsics declare
    // ⛔ ckir_refs (KernelRef asset dependencies) are NAMED-FORWARD to CEIR-10 (CKIR integration) — the schema slot
    // exists but no CKIR-referencing op is built yet. ⛔ "Which execution provider RUNS a region" is a §69 plan-time
    // question (routed to the partitioner rows CEIR-21/26), NOT inferred here by prefix-matching an op name.
    explicit DependencyRecord(memory::IAllocator* a) : called_funcs(a), intrinsics(a), providers(a) {}
};
[[nodiscard]] DependencyRecord collect_dependencies(Context& ctx, const Module& module, memory::IAllocator* alloc);

// The strict COOK-TIME registration check (closes the 6a VACUOUS-PASS shape): the FIRST op in `module` (pre-order, incl.
// nested regions) whose kind has NO registered OpInfo (`op_info == nullptr`) — EMPTY≠UNKNOWN. A cook MUST reject an
// unregistered op, because the verifiers pass VACUOUSLY on a kind that declares no traits/effects. `nullptr` ⇒ every op
// kind is registered (the module is verifiable).
[[nodiscard]] const Operation* find_unregistered_op(const Context& ctx, const Module& module) noexcept;
} // namespace crd::ceir
