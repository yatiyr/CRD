#pragma once

// crd-ceir-cook — the CEIR RUNTIME PROGRAM + generation-safe handle (CEIR-7b, §105). A `RuntimeProgram` is the LOADED,
// immutable runtime form of a cooked 'CEIR' asset: the deserialized Module + its content/interface hashes. `load_program`
// reads a cooked blob and ⛔ VALIDATES the header's declared hashes against the payload (the declared-header-words-
// validated scar) before handing back a program. The generation-safe `RuntimeSlot`/`RuntimeHandle` come from
// crd-render-asset-core (RAF-3/RAF-11) — ⭐ CEIR is the FIRST consumer of that framework. The EXECUTABLE form
// (CompiledExecutionPlan / interpreter binding) is CEIR-8, NOT here.
//
// ⛔ Context lifetime (7c inbound pattern): each generation's Module should be deserialized into ITS OWN Context, so a
// hot-swap can free an old generation WHOLESALE (arenas free per-Context, never per-module) without disturbing live
// handles. 7b BORROWS a caller-owned Context (no ownership / deferred-destroy machinery — that is 7c's row); the
// two-Context generation test proves two generations coexist independently.

#include <crd/ceir/context.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/renderasset/cooked.hpp> // RuntimeSlot / RuntimeHandle / AssetId (the RAF generation-tagged handle)

namespace crd::ceir::cook
{
// The LOADED, immutable runtime form of a cooked 'CEIR' program (§105 RuntimeProgram). Borrows `module` (owned by the
// Context it was deserialized into). The hashes are the VALIDATED header words (a load PROVED they match the payload).
struct RuntimeProgram
{
    Module*  module         = nullptr;
    crd::u64 content_hash   = 0U; // == stable_hash(module) — verified at load
    crd::u64 interface_hash = 0U; // == interface_hash(module) — verified at load; a 7c swap-compatibility check compares THIS
    [[nodiscard]] bool valid() const noexcept { return module != nullptr; }
};

// A generation-tagged slot/handle over a RuntimeProgram — the RAF-11 hot-reload safety types, reused VERBATIM. ⭐ CEIR is
// the FIRST adopter of crd-render-asset-core's RuntimeSlot framework (RAF-3 built it for the render families to adopt).
using ProgramSlot   = crd::renderasset::RuntimeSlot<RuntimeProgram>;
using ProgramHandle = crd::renderasset::RuntimeHandle<RuntimeProgram>;

// Why a load FAILED.
// NOLINTNEXTLINE(performance-enum-size)
enum class LoadError : crd::u8
{
    Ok = 0,
    ReadFailed,            // read_program rejected the blob (bad container / header / program / deps)
    UnregisteredOp,        // an op kind is not registered — the CALLER must register the module's dialects before load
                           // (interface_hash's effect walk is registration-sensitive; a typed error, NOT a hash mismatch)
    ContentHashMismatch,   // recomputed stable_hash != the header's content hash — a CORRUPT or SPLICED blob
    InterfaceHashMismatch, // content matches but recomputed interface_hash != the header's — REGISTRY DRIFT (a dialect's
                           // declared effects changed between cook and load); the case a 7c swap-compat check must catch
};
[[nodiscard]] containers::StringView load_error_name(LoadError e) noexcept;

// The result of a load: on success `program.valid()`; on failure `error` names why.
struct LoadResult
{
    RuntimeProgram program;
    LoadError      error = LoadError::Ok;
    [[nodiscard]] bool ok() const noexcept { return error == LoadError::Ok; }
};

// LOAD a cooked blob into a RuntimeProgram, VALIDATING the header's declared hashes against the payload. ⛔ The CALLER
// must have registered the module's dialects (the register-to-verify contract) — an unregistered op is a typed
// `UnregisteredOp`, checked BEFORE the hash recompute (which needs registration). A CONTENT-hash match carries the
// cook-time verification FORWARD (no verifier re-run at load). The Module is deserialized into `ctx`.
[[nodiscard]] LoadResult load_program(Context& ctx, containers::ConstSpan<crd::u8> blob, memory::IAllocator* alloc,
                                      memory::IAllocator* scratch);
} // namespace crd::ceir::cook
