#include <crd/ceir/cook/program_cook.hpp>

#include <crd/ceir/func.hpp>  // find_recursion_violation (the §34 declared-policy verifier)
#include <crd/ceir/parse.hpp> // parse (the text frontend for cook_program_text)
#include <crd/ceir/binary.hpp>        // serialize / deserialize / stable_hash
#include <crd/renderasset/cooked.hpp> // CookedHeader + write/read_cooked_header (the render-asset cooked envelope)
#include <crd/renderasset/diagnostic.hpp>
#include <crd/renderasset/identity.hpp> // AssetId / AssetType
#include <crd/resources/crdr.hpp>       // CrdrWriter / crdr_read / crdr_find_chunk / kFourCC_*
#include <crd/resources/resource_id.hpp>

// CEIR-7a — the CEIR asset cook bridge. crd-ceir computes the semantic metadata (content/interface hash, deps); THIS
// module packages a verified module into a CRDR container using the render-asset CookedHeader envelope. crd-ceir gains
// NO edge to crd-resources / crd-render-asset-core (ADR-0109 I4/I5).

namespace crd::ceir::cook
{
namespace
{
// The cooked-CEIR blob layout version (bumped when the chunk set / header schema changes — a recook by design).
constexpr crd::u32 kCeirCookSchema = 1U;

void push_u32(containers::Array<crd::u8>& out, crd::u32 v)
{
    out.push_back(static_cast<crd::u8>(v & 0xFFU));
    out.push_back(static_cast<crd::u8>((v >> 8U) & 0xFFU));
    out.push_back(static_cast<crd::u8>((v >> 16U) & 0xFFU));
    out.push_back(static_cast<crd::u8>((v >> 24U) & 0xFFU));
}
void push_str(containers::Array<crd::u8>& out, containers::StringView s)
{
    push_u32(out, static_cast<crd::u32>(s.size()));
    for (crd::usize i = 0; i < s.size(); ++i) { out.push_back(static_cast<crd::u8>(s[i])); }
}
void push_list(containers::Array<crd::u8>& out, const containers::Array<containers::StringView>& list)
{
    push_u32(out, static_cast<crd::u32>(list.size()));
    for (crd::u32 i = 0; i < static_cast<crd::u32>(list.size()); ++i) { push_str(out, list[i]); }
}
// Serialize the §106 dependency record field-by-field LE (⛔ never a struct blast — the struct-padding-in-content-hash
// scar). ckir_refs are named-forward to CEIR-10 (no field emitted; a future schema bump adds it — a recook).
void serialize_deps(const DependencyRecord& dep, containers::Array<crd::u8>& out)
{
    push_list(out, dep.called_funcs);
    push_list(out, dep.intrinsics);
    push_list(out, dep.providers);
}

// A bounds-checked little-endian reader over the deps chunk (malformed input is REPORTED, never an OOB read).
struct Reader
{
    const crd::u8* p;
    crd::usize     n;
    crd::usize     off = 0;
    [[nodiscard]] bool u32(crd::u32& out) noexcept
    {
        if (off + 4U > n) { return false; }
        out = static_cast<crd::u32>(p[off]) | (static_cast<crd::u32>(p[off + 1U]) << 8U)
              | (static_cast<crd::u32>(p[off + 2U]) << 16U) | (static_cast<crd::u32>(p[off + 3U]) << 24U);
        off += 4U;
        return true;
    }
    [[nodiscard]] bool str(containers::StringView& out) noexcept
    {
        crd::u32 len = 0;
        if (!u32(len)) { return false; }
        if (off + len > n) { return false; }
        out = containers::StringView(reinterpret_cast<const char*>(p + off), len);
        off += len;
        return true;
    }
};
// Parse a dep list, INTERNING each string into `ctx` (stable storage for the ctx lifetime).
[[nodiscard]] bool parse_list(Context& ctx, Reader& rd, containers::Array<containers::StringView>& list)
{
    crd::u32 count = 0;
    if (!rd.u32(count)) { return false; }
    for (crd::u32 i = 0; i < count; ++i)
    {
        containers::StringView s;
        if (!rd.str(s)) { return false; }
        list.push_back(ctx.intern_symbol(s));
    }
    return true;
}
[[nodiscard]] containers::ConstSpan<crd::u8> span_of(const containers::Array<crd::u8>& a) noexcept
{
    return containers::ConstSpan<crd::u8>(a.data(), a.size());
}
} // namespace

containers::StringView cook_error_name(CookError e) noexcept
{
    switch (e) // ⛔ no default — a new CookError is a -Werror=switch compile error
    {
    case CookError::Ok: return containers::StringView("ok");
    case CookError::NoModuleBody: return containers::StringView("no-module-body");
    case CookError::ParseFailed: return containers::StringView("parse-failed");
    case CookError::UnregisteredOp: return containers::StringView("unregistered-op");
    case CookError::StructureError: return containers::StringView("structure-error");
    case CookError::DomainViolation: return containers::StringView("domain-violation");
    case CookError::TokenMisuse: return containers::StringView("token-misuse");
    case CookError::BorrowEscape: return containers::StringView("borrow-escape");
    case CookError::RecursionViolation: return containers::StringView("recursion-violation");
    }
    return containers::StringView("unknown");
}
containers::StringView read_error_name(ReadError e) noexcept
{
    switch (e)
    {
    case ReadError::Ok: return containers::StringView("ok");
    case ReadError::BadContainer: return containers::StringView("bad-container");
    case ReadError::WrongType: return containers::StringView("wrong-type");
    case ReadError::BadHeader: return containers::StringView("bad-header");
    case ReadError::MissingProgram: return containers::StringView("missing-program");
    case ReadError::ProgramDecodeFailed: return containers::StringView("program-decode-failed");
    case ReadError::BadDeps: return containers::StringView("bad-deps");
    }
    return containers::StringView("unknown");
}

CookResult cook_program(Context& ctx, const Module& module, crd::u64 asset_id, memory::IAllocator* alloc,
                        memory::IAllocator* scratch)
{
    CookResult r(alloc);
    if (module.body() == nullptr)
    {
        r.error = CookError::NoModuleBody;
        return r;
    }
    // 1. STRICT registration check — EMPTY≠UNKNOWN: an unregistered op kind would make the verifiers below pass
    // vacuously, so a cook must reject it FIRST (the 6a vacuous-pass shape, closed at the cook).
    if (const Operation* const u = find_unregistered_op(ctx, module))
    {
        r.error = CookError::UnregisteredOp;
        r.op    = u;
        return r;
    }
    // 2. source → VERIFIED: the §115 structure, §15/§32 domain, §116 token, and §19 borrow verifiers (the module-validity
    // set; the compiler-MODE check is session state, not a cook concern).
    if (const StructureError se = ctx.find_structure_error(module); se.kind != StructureErrorKind::None)
    {
        r.error = CookError::StructureError;
        r.op    = se.op;
        return r;
    }
    if (const DomainViolation dv = ctx.find_domain_violation(module); dv.op != nullptr)
    {
        r.error = CookError::DomainViolation;
        r.op    = dv.op;
        return r;
    }
    if (const TokenMisuse tm = ctx.find_token_misuse(module); tm.kind != TokenMisuseKind::None)
    {
        r.error = CookError::TokenMisuse;
        r.op    = tm.op;
        return r;
    }
    if (const BorrowEscape be = ctx.find_borrowed_escape(module); be.escaping_use != nullptr)
    {
        r.error = CookError::BorrowEscape;
        r.op    = be.escaping_use;
        return r;
    }
    // §34 declared-recursion-policy check (the declared-words-validated scar): a None-declared func on a call-graph cycle
    // is a cook error. Resolved over module.symbols() (guard null — an un-indexed module has no verifiable call graph).
    if (const SymbolTable* const syms = module.symbols(); syms != nullptr)
    {
        if (const func::RecursionViolation rv = func::find_recursion_violation(ctx, module, *syms);
            rv.kind != func::RecursionViolationKind::None)
        {
            r.error = CookError::RecursionViolation;
            r.op    = rv.func_op;
            return r;
        }
    }
    // 3. serialize + the two hashes + the dependency record (all crd-ceir semantic computations).
    const containers::Array<crd::u8> program = serialize(ctx, module, scratch);
    r.content_hash                           = stable_hash(ctx, module, scratch);
    r.interface_hash                         = interface_hash(ctx, module, scratch);
    const DependencyRecord dep               = collect_dependencies(ctx, module, scratch);

    // 4. the render-asset CookedHeader (in a 'META' chunk): the §107 interface hash + the content hash + the type/schema
    // a loader validates. dependency_count = 0 — the flat AssetId list carries CROSS-ASSET deps (CKIR kernel assets →
    // CEIR-10); the STRUCTURED §106 CEIR deps live in the 'CDEP' chunk below.
    crd::renderasset::CookedHeader hdr;
    hdr.type             = crd::renderasset::AssetType::Program;
    hdr.schema           = crd::renderasset::SchemaVersion{kCeirCookSchema};
    hdr.iface            = crd::renderasset::InterfaceHash{r.interface_hash};
    hdr.content          = crd::renderasset::ContentHash{r.content_hash};
    hdr.id               = crd::renderasset::AssetId{asset_id};
    hdr.dependency_count = 0U;
    const crd::usize          hsz = crd::renderasset::cooked_blob_header_size(0U);
    containers::Array<crd::u8> header(scratch);
    for (crd::usize i = 0; i < hsz; ++i) { header.push_back(0U); }
    (void)crd::renderasset::write_cooked_header(header.data(), hsz, hdr, nullptr);

    // 5. the structured §106 dependency chunk.
    containers::Array<crd::u8> deps(scratch);
    serialize_deps(dep, deps);

    // 6. assemble the CRDR container (type 'CEIR'): 'META' header · 'CEIR' program · 'CDEP' deps.
    crd::resources::CrdrWriter w(alloc, crd::resources::ResourceId{0U, asset_id}, crd::resources::kFourCC_CEIR);
    w.add_chunk(crd::resources::kFourCC_META, span_of(header));
    w.add_chunk(crd::resources::kFourCC_CEIR, span_of(program));
    w.add_chunk(crd::resources::kFourCC_CDEP, span_of(deps));
    r.blob = w.finish();
    return r;
}

CookResult cook_program_text(Context& ctx, containers::StringView source, crd::u64 asset_id, memory::IAllocator* alloc,
                            memory::IAllocator* scratch)
{
    const ParseResult pr = parse(ctx, source);
    if (!pr.ok || pr.module == nullptr)
    {
        CookResult r(alloc);
        r.error = CookError::ParseFailed;
        return r;
    }
    return cook_program(ctx, *pr.module, asset_id, alloc, scratch); // the SAME cook — no privileged path (§121)
}

ReadResult read_program(Context& ctx, containers::ConstSpan<crd::u8> blob, memory::IAllocator* alloc)
{
    ReadResult r(alloc);
    crd::resources::CrdrFile file(alloc);
    if (crd::resources::crdr_read(blob, file, alloc) != crd::resources::CrdrError::Ok)
    {
        r.error = ReadError::BadContainer;
        return r;
    }
    if (file.type_fourcc != crd::resources::kFourCC_CEIR)
    {
        r.error = ReadError::WrongType;
        return r;
    }
    // validate the CookedHeader ('META') — magic / type == Program / schema.
    const crd::resources::CrdrChunk* const meta = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_META);
    if (meta == nullptr)
    {
        r.error = ReadError::BadHeader;
        return r;
    }
    crd::renderasset::CookedHeader          hdr;
    containers::Array<crd::renderasset::AssetId> hdeps(alloc);
    crd::renderasset::DiagnosticList             diags(alloc);
    if (!crd::renderasset::read_cooked_header(meta->payload.data(), meta->payload.size(),
                                              crd::renderasset::AssetType::Program,
                                              crd::renderasset::SchemaVersion{kCeirCookSchema}, hdr, hdeps, diags))
    {
        r.error = ReadError::BadHeader;
        return r;
    }
    r.content_hash   = hdr.content.value;
    r.interface_hash = hdr.iface.value;
    // deserialize the 'CEIR' program chunk into a fresh module owned by ctx.
    const crd::resources::CrdrChunk* const prog = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_CEIR);
    if (prog == nullptr)
    {
        r.error = ReadError::MissingProgram;
        return r;
    }
    const ParseResult pr = deserialize(ctx, prog->payload);
    if (!pr.ok || pr.module == nullptr)
    {
        r.error = ReadError::ProgramDecodeFailed;
        return r;
    }
    r.module = pr.module;
    // parse the §106 dependency chunk — ⛔ REQUIRED (schema 1 always writes it): a MISSING or UNPARSEABLE 'CDEP' is a
    // malformed blob (BadDeps), never a silent "no dependencies" (the default-empty scar — under-invalidation at 7c).
    const crd::resources::CrdrChunk* const dc = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_CDEP);
    if (dc == nullptr)
    {
        r.error = ReadError::BadDeps;
        return r;
    }
    Reader rd{dc->payload.data(), dc->payload.size()};
    if (!(parse_list(ctx, rd, r.deps.called_funcs) && parse_list(ctx, rd, r.deps.intrinsics)
          && parse_list(ctx, rd, r.deps.providers)))
    {
        r.error = ReadError::BadDeps;
        return r;
    }
    return r;
}
} // namespace crd::ceir::cook
