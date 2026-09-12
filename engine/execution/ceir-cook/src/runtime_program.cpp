#include <crd/ceir/cook/runtime_program.hpp>

#include <crd/ceir/binary.hpp>            // stable_hash (recompute the content hash)
#include <crd/ceir/cook/program_cook.hpp> // read_program
#include <crd/ceir/program_asset.hpp>     // interface_hash / find_unregistered_op

// CEIR-7b — the CEIR runtime program loader. The generation-safe slot/handle are crd-render-asset-core's RuntimeSlot
// (reused verbatim); this file adds the loaded RuntimeProgram + a load path that VALIDATES the cooked header's declared
// hashes against the payload (the declared-header-words-validated scar) — read_program (7a) does NOT recompute them.

namespace crd::ceir::cook
{
containers::StringView load_error_name(LoadError e) noexcept
{
    switch (e) // ⛔ no default — a new LoadError is a -Werror=switch compile error
    {
    case LoadError::Ok: return containers::StringView("ok");
    case LoadError::ReadFailed: return containers::StringView("read-failed");
    case LoadError::UnregisteredOp: return containers::StringView("unregistered-op");
    case LoadError::ContentHashMismatch: return containers::StringView("content-hash-mismatch");
    case LoadError::InterfaceHashMismatch: return containers::StringView("interface-hash-mismatch");
    }
    return containers::StringView("unknown");
}

LoadResult load_program(Context& ctx, containers::ConstSpan<crd::u8> blob, memory::IAllocator* alloc,
                       memory::IAllocator* scratch)
{
    LoadResult r;
    const ReadResult read = read_program(ctx, blob, alloc);
    if (!read.ok() || read.module == nullptr)
    {
        r.error = LoadError::ReadFailed;
        return r;
    }
    // ⛔ registration check FIRST — interface_hash's effect walk is registration-sensitive, so an unregistered op is a
    // typed caller-must-register error, NOT a spurious hash mismatch (the register-to-verify contract).
    if (find_unregistered_op(ctx, *read.module) != nullptr)
    {
        r.error = LoadError::UnregisteredOp;
        return r;
    }
    // ⛔ VALIDATE the declared header hashes against the payload. Content first: a mismatch = a CORRUPT/SPLICED blob.
    if (stable_hash(ctx, *read.module, scratch) != read.content_hash)
    {
        r.error = LoadError::ContentHashMismatch;
        return r;
    }
    // Interface with content matching = REGISTRY DRIFT (a dialect's declared effects changed between cook and load) — the
    // discriminator a 7c swap-compatibility comparison relies on. (Content match carries cook-time verification forward.)
    if (interface_hash(ctx, *read.module, scratch) != read.interface_hash)
    {
        r.error = LoadError::InterfaceHashMismatch;
        return r;
    }
    r.program = RuntimeProgram{read.module, read.content_hash, read.interface_hash};
    return r;
}
} // namespace crd::ceir::cook
