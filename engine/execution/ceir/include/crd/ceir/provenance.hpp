#pragma once

// crd-ceir — authored-source and lowering PROVENANCE (DIAG.8a, ADR-0133 DG13). Every executable op can name the
// authored source positions and nodes it came from, through optimization, binary serialization and plan compilation.
//
// ⛔ A SIDE TABLE, never op content: origins live in the Context (keyed by op) and in the skippable 'ORIG' binary
// chunk. They are NOT part of `stable_hash`, CSE structural equality or the printed text, so reformatting a source file
// (every line moves) or re-authoring the same program elsewhere leaves the program's content identity untouched. The
// CEIR-1a `Operation::loc` stays what it was (a builder-declared location that IS content); it is the fallback when an
// op has no recorded origins.
//
// ⛔ MANY-TO-MANY: an op synthesized by a pass (a fold, a CSE survivor) carries the union of the origins of every op it
// replaced, resolved to those ops' stable ids at the moment they were replaced. Unknown attribution is reported as a
// typed gap (`ProvenanceGap`), never as a silent zero location.

#include <crd/ceir/id.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::ceir
{
class Context;
class Operation;

// What an origin's `node` identifies.
// NOLINTNEXTLINE(performance-enum-size)
enum class OriginSpace : u8
{
    CarrierOp = 0, // the op carrying this record (a parser position); `node` is unused, resolves to its stable id
    CeirOp    = 1, // another CEIR op, by stable id (an op a pass replaced); an invalid `node` = identity unknown
    ChirNode  = 2, // a CHIR source node, by its CHIR-side stable id (a lowering origin)
};
inline constexpr u8 kOriginSpaceCount = 3U;

// One authored origin: a source position (0 line = no position) plus the authored node identity.
struct Origin
{
    SourceLoc   loc{};
    StableId    node{};
    OriginSpace space = OriginSpace::CarrierOp;
};

[[nodiscard]] constexpr bool origin_equal(const Origin& a, const Origin& b) noexcept
{
    return a.loc.file_id == b.loc.file_id && a.loc.line == b.loc.line && a.loc.col == b.loc.col && a.node == b.node &&
           a.space == b.space;
}

// Why a provenance query could not name an authored source position.
// NOLINTNEXTLINE(performance-enum-size)
enum class ProvenanceGap : u8
{
    None = 0,         // at least one origin has a source position
    NoOperation,      // there is no op to attribute (e.g. a compile error with no offending op, or a fault-free run)
    NoSourceLocation, // the op exists but neither it nor any origin recorded a source position (a builder-built op)
};
[[nodiscard]] containers::StringView provenance_gap_name(ProvenanceGap g) noexcept;

// A resolved provenance view. `origins` has no CarrierOp entries (each became a CeirOp entry naming the op itself).
struct Provenance
{
    StableId                      op{};    // the attributed op's own stable id (invalid = unassigned or no op)
    containers::ConstSpan<Origin> origins; // the authored origins; empty when none were recorded
    ProvenanceGap                 gap = ProvenanceGap::NoOperation;

    // The closest-known source position: the first origin carrying a line, or nullptr when `gap != None`.
    [[nodiscard]] const Origin* primary() const noexcept
    {
        for (usize i = 0; i < origins.size(); ++i)
        {
            if (origins[i].loc.line != 0U)
            {
                return &origins[i];
            }
        }
        return nullptr;
    }
};

// Resolve `op`'s provenance from `ctx`: its recorded origins, else its builder-declared `loc()`, else a typed gap.
// `storage` backs the returned span (cleared first); keep it alive while the view is used. A null `op` → NoOperation.
[[nodiscard]] Provenance resolve_provenance(const Context& ctx, const Operation* op,
                                            containers::Array<Origin>& storage);

// Render `p` for a human or agent: "<file>:<line>:<col> op#<id>", then " from <origin>, <origin>" when more than one
// origin exists, or "<gap name> op#<id>" when no position is known, followed by " from <origin>, ..." for every
// position-less origin it has (e.g. a graph-authored "chir#<id>", the closest-known origin). A zero file id renders
// "<unknown>".
[[nodiscard]] containers::String render_provenance(const Context& ctx, const Provenance& p, memory::IAllocator* out);

// What a Context knows about an op kind's ADR-0110 native binding. EMPTY != UNKNOWN: a kind that is not registered in
// the Context (its dialect was never registered there, e.g. after a load into a bare Context) is `Unregistered`, which
// says the binding cannot be known there; it is never reported as a non-intrinsic op.
// NOLINTNEXTLINE(performance-enum-size)
enum class NativeKind : u8
{
    NoOperation = 0, // there is no op (a null op)
    Unregistered,    // the op's kind is not registered in this Context
    NotIntrinsic,    // a registered op that declares no `[op.native]` binding
    Intrinsic,       // a registered intrinsic: `provider` names its declared native provider
};
[[nodiscard]] containers::StringView native_kind_name(NativeKind k) noexcept;

// An op's native binding, so a failure at an intrinsic names the native provider that implements it. Views borrow the
// Context (the interned op name) and the dialect registration (the provider name).
struct NativeBinding
{
    NativeKind             kind = NativeKind::NoOperation;
    containers::StringView op_name;  // "dialect.op" ("" for a null op)
    containers::StringView provider; // the declared native provider; non-empty exactly when kind == Intrinsic
};
[[nodiscard]] NativeBinding native_binding(const Context& ctx, const Operation* op) noexcept;

// Render the op a failure is blamed on, for a human or agent: "<op name> " then " native <provider>" for an intrinsic
// (or " native unregistered" when the Context cannot know), then " at " and `render_provenance` of the op. A null op
// renders the NoOperation gap name alone.
[[nodiscard]] containers::String render_op_site(const Context& ctx, const Operation* op, memory::IAllocator* out);
} // namespace crd::ceir
