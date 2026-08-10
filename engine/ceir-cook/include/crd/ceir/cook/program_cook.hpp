#pragma once

// crd-ceir-cook — the CEIR asset COOK (CEIR-7a; ADR-0109 §4.2 bridge). `cook_program` turns a VERIFIED crd::ceir::Module
// into a self-describing CRDR blob: the render-asset `CookedHeader` (in a 'META' chunk) carrying the CONTENT hash
// (`stable_hash` — the cook-cache key) and the §107 INTERFACE hash (so an implementation-only edit hot-swaps without
// invalidating callers), the CEIR binary program (a 'CEIR' chunk), and the §106 dependency record (a 'CDEP' chunk).
// `read_program` round-trips it back into a fresh Module + validated header. ⛔ This header names NO crd-resources /
// crd-render-asset-core type — the CRDR container + CookedHeader are PRIVATE impl (crd-ceir stays asset-free; I4/I5).

#include <crd/ceir/context.hpp>
#include <crd/ceir/program_asset.hpp> // DependencyRecord
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::ceir::cook
{
// Why a cook FAILED (a cook REPORTS bad input, never asserts). `Ok` ⇒ the blob is valid + self-describing.
// NOLINTNEXTLINE(performance-enum-size)
enum class CookError : crd::u8
{
    Ok = 0,
    NoModuleBody,       // the module has no body region (nothing to cook)
    ParseFailed,        // cook_program_text: the source text did not parse into a module
    UnregisteredOp,     // an op whose kind has NO registered dialect — its verifiers would pass VACUOUSLY (find_unregistered_op)
    StructureError,     // find_structure_error flagged a §115 structural defect
    DomainViolation,    // find_domain_violation flagged a §15/§32 domain/realtime illegality
    TokenMisuse,        // find_token_misuse flagged a §116 async use-once defect
    BorrowEscape,       // find_borrowed_escape flagged a §19 borrow escaping its region
    RecursionViolation, // find_recursion_violation flagged a §34 declared-recursion-policy breach
    // CEIR-13c §85/§107 — the CKIR KernelRef declared-contract check (only when a resolver is supplied):
    KernelUnresolved,        // a dispatch's @kernel identity did NOT resolve (the resolver returned false) — existence is
                             // ALWAYS checked; `op` points at the dispatch.
    KernelInterfaceMismatch, // a dispatch PINS an expected §107 interface hash (kernel_interface) that != the resolved
                             // kernel's actual interface hash — the declared-contract violation; `op` points at the dispatch.
};
[[nodiscard]] containers::StringView cook_error_name(CookError e) noexcept;

// CEIR-13c §85 — resolve a CKIR KernelRef asset IDENTITY (`name`, the dispatch's @kernel symbol) to its ACTUAL §107
// interface hash. Returns false iff the name does NOT resolve (→ KernelUnresolved); on true, `out_interface` is the
// resolved kernel's interface hash (compared to a dispatch's pin, iff pinned). ⛔ fn-ptr + `user` (the RunHooks precedent —
// no std::function). In production this is backed by the ADR-0104 cook cache; a test supplies a table-backed mock.
using KernelResolveFn = bool (*)(containers::StringView name, void* user, crd::u64& out_interface);

// The result of a cook: on success `blob` is the self-describing CRDR bytes + the two hashes; on failure `error` names
// why and `op` points at the offender (nullptr when ok / NoModuleBody).
struct CookResult
{
    containers::Array<crd::u8> blob;
    crd::u64                   content_hash   = 0U; // stable_hash — the cook-cache key
    crd::u64                   interface_hash = 0U; // §107 — invalidates dependents ONLY on an interface change
    CookError                  error          = CookError::Ok;
    const Operation*           op             = nullptr;
    explicit CookResult(memory::IAllocator* a) : blob(a) {}
    [[nodiscard]] bool ok() const noexcept { return error == CookError::Ok; }
};

// COOK `module` (asset id `asset_id`) → a verified, self-describing CRDR blob. ⛔ Runs the STRICT registration check
// (EMPTY≠UNKNOWN) + the §115/§15/§116/§19 verifiers FIRST (source → VERIFIED) — a cook that stamped "verified" on an
// unregistered op would have verified nothing. `scratch` backs transient work; `blob` is allocated from `alloc`.
// ⛔ CEIR-13c: when `resolve != nullptr`, the CKIR KernelRef declared-contract check runs (every dispatch's @kernel must
// resolve → KernelUnresolved; a pinned interface hash must match → KernelInterfaceMismatch; `op` points at the dispatch).
// `resolve == nullptr` DEFERS resolution to a later phase — the CDEP chunk still persists the refs (a documented split).
[[nodiscard]] CookResult cook_program(Context& ctx, const Module& module, crd::u64 asset_id, memory::IAllocator* alloc,
                                      memory::IAllocator* scratch, KernelResolveFn resolve = nullptr, void* user = nullptr);

// COOK from SOURCE TEXT (§105 ProgramSourceAsset): parse `source` (→ `CookError::ParseFailed` on a parse error) then cook
// the resulting module. ⛔ The text and builder paths are the SAME cook — a program cooked from text and the identical
// program built in C++ produce byte-identical content + interface hashes (the no-privileged-path property, §121).
[[nodiscard]] CookResult cook_program_text(Context& ctx, containers::StringView source, crd::u64 asset_id,
                                           memory::IAllocator* alloc, memory::IAllocator* scratch,
                                           KernelResolveFn resolve = nullptr, void* user = nullptr);

// Why a read FAILED.
// NOLINTNEXTLINE(performance-enum-size)
enum class ReadError : crd::u8
{
    Ok = 0,
    BadContainer,        // not a valid CRDR container
    WrongType,           // the container / CookedHeader type is not a CEIR program
    BadHeader,           // the 'META' CookedHeader is missing / malformed / wrong schema
    MissingProgram,      // no 'CEIR' program chunk
    ProgramDecodeFailed, // the 'CEIR' blob did not deserialize into a Module
    BadDeps,             // the 'CDEP' dependency chunk is missing (schema 1 always writes it) or unparseable
};
[[nodiscard]] containers::StringView read_error_name(ReadError e) noexcept;

// The result of reading a cooked blob back.
struct ReadResult
{
    Module*          module         = nullptr; // the round-tripped module (owned by the passed Context)
    crd::u64         content_hash   = 0U;      // as recorded in the CookedHeader
    crd::u64         interface_hash = 0U;
    DependencyRecord deps;                      // the §106 record (strings interned into the passed Context)
    ReadError        error = ReadError::Ok;
    explicit ReadResult(memory::IAllocator* a) : deps(a) {}
    [[nodiscard]] bool ok() const noexcept { return error == ReadError::Ok; }
};

// READ a cooked blob: validate the CRDR container + CookedHeader (magic/type/schema), deserialize the 'CEIR' chunk into a
// fresh Module owned by `ctx`, and parse the §106 dependency chunk (strings interned into `ctx`). ⛔ The CALLER must
// re-register the module's dialects before RECOMPUTING hashes on the result (traits are registry state — the
// register-to-verify contract; the header hashes are readable without registration).
[[nodiscard]] ReadResult read_program(Context& ctx, containers::ConstSpan<crd::u8> blob, memory::IAllocator* alloc);
} // namespace crd::ceir::cook
