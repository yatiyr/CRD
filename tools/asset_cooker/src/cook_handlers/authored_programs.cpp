// authored_programs.cpp — REN-38 audit: THE AUTHORED-PROGRAM VOCABULARIES RIDE THE ASSET PIPELINE.
//
// ⛔ THE GAP THIS CLOSES. The REN-38 bands made every GPU program an authored asset — and the asset COOKER had
// no handler for ANY of the five formats. The legacy `.mat.toml`/GLSL handlers were deleted (correctly, C4) and
// nothing replaced them, so a `.crdm`/`.crdv`/`.crdl`/`.crdt`/`.frame.toml` in a source tree was invisible to
// the pipeline: never validated at cook time, never packed, never mountable. "Authoring text, runtime binary"
// stopped one layer short of the newest vocabulary in the engine.
//
// WHAT EACH HANDLER DOES: parse + VALIDATE with the owning cooker — a malformed asset FAILS THE COOK with the
// cooker's own named error, which is the entire point (a shipped pack must already be proven well-formed; a
// player's machine is not where a typo should surface). Frame graphs and techniques then cook to their REAL
// binary blobs. Material / vertex / lighting have no binary descriptor serializers yet, so their artifact is
// the VALIDATED SOURCE — stated openly: the binary forms are a named row, not a silent equivalence.

#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/cook_io.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/framecook/frame_asset.hpp>
#include <crd/lightcook/lighting_asset.hpp>
#include <crd/matcook/material_asset.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/techniquecook/technique_asset.hpp>
#include <crd/vertexcook/vertex_asset.hpp>

#include <cstdio> // the handler failure report — the cooker's stderr idiom (see preset.cpp)

namespace crd::cooker
{

namespace
{

constexpr crd::u32 kAuthoredHandlerVersion = 1U;

// Wrap `payload` as this artifact's cooked blob (the blob's own magic identifies the format inside).
[[nodiscard]] CookResult finish_blob(const CookContext& ctx, crd::containers::ConstSpan<crd::u8> payload)
{
    CookResult result(ctx.allocator);
    crd::resources::CrdrWriter writer(ctx.allocator, ctx.id, crd::resources::kFourCC_BLOB);
    writer.add_chunk_compressed(crd::resources::kFourCC_BLOB, payload);
    result.type_fourcc     = crd::resources::kFourCC_BLOB;
    result.cooked_bytes    = writer.finish();
    result.handler_version = kAuthoredHandlerVersion;
    result.ok              = true;
    return result;
}

[[nodiscard]] bool read_text(const CookContext& ctx, crd::containers::Array<crd::u8>& bytes)
{
    return ctx.io != nullptr && ctx.io->read_source(bytes);
}

[[nodiscard]] crd::containers::StringView view_of(const crd::containers::Array<crd::u8>& bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void report(const CookContext& ctx, const char* what, const crd::containers::String& where)
{
    std::fprintf(stderr, "authored cook: %.*s: %s at '%s'\n", static_cast<int>(ctx.source_path.size()),
                 ctx.source_path.data(), what, where.c_str());
}

CookResult frame_graph_handler(const CookContext& ctx)
{
    CookResult fail(ctx.allocator);
    crd::containers::Array<crd::u8> src(ctx.allocator);
    if (!read_text(ctx, src)) { return fail; }
    crd::framecook::FrameGraphDesc desc(ctx.allocator);
    crd::containers::String        where(ctx.allocator);
    const auto err = crd::framecook::parse_frame_toml(view_of(src), desc, &where);
    if (err != crd::framecook::FrameCookError::Ok)
    {
        report(ctx, crd::framecook::frame_cook_error_text(err), where);
        return fail;
    }
    const crd::containers::Array<crd::u8> blob = crd::framecook::cook_frame_graph(desc, ctx.allocator);
    if (blob.size() == 0U) { return fail; }
    return finish_blob(ctx, crd::containers::as_const_span(blob));
}

CookResult technique_handler(const CookContext& ctx)
{
    CookResult fail(ctx.allocator);
    crd::containers::Array<crd::u8> src(ctx.allocator);
    if (!read_text(ctx, src)) { return fail; }
    crd::techniquecook::TechniqueDesc desc(ctx.allocator);
    crd::containers::String           where(ctx.allocator);
    const auto err = crd::techniquecook::parse_technique_toml(view_of(src), desc, &where);
    if (err != crd::techniquecook::TechniqueCookError::Ok)
    {
        report(ctx, crd::techniquecook::technique_cook_error_text(err), where);
        return fail;
    }
    const crd::containers::Array<crd::u8> blob = crd::techniquecook::cook_technique(desc, ctx.allocator);
    if (blob.size() == 0U) { return fail; }
    return finish_blob(ctx, crd::containers::as_const_span(blob));
}

CookResult material_handler(const CookContext& ctx)
{
    CookResult fail(ctx.allocator);
    crd::containers::Array<crd::u8> src(ctx.allocator);
    if (!read_text(ctx, src)) { return fail; }
    crd::matcook::MaterialDesc desc(ctx.allocator);
    crd::containers::String    where(ctx.allocator);
    const auto err = crd::matcook::parse_material_toml(view_of(src), desc, &where);
    if (err != crd::matcook::MaterialCookError::Ok)
    {
        report(ctx, crd::matcook::material_cook_error_text(err), where);
        return fail;
    }
    return finish_blob(ctx, crd::containers::as_const_span(src)); // validated source (binary form = named row)
}

CookResult vertex_handler(const CookContext& ctx)
{
    CookResult fail(ctx.allocator);
    crd::containers::Array<crd::u8> src(ctx.allocator);
    if (!read_text(ctx, src)) { return fail; }
    crd::vertcook::VertexProgramDesc desc(ctx.allocator);
    crd::containers::String          where(ctx.allocator);
    const auto err = crd::vertcook::parse_vertex_toml(view_of(src), desc, &where);
    if (err != crd::vertcook::VertexCookError::Ok)
    {
        report(ctx, crd::vertcook::vertex_cook_error_text(err), where);
        return fail;
    }
    return finish_blob(ctx, crd::containers::as_const_span(src)); // validated source (binary form = named row)
}

CookResult lighting_handler(const CookContext& ctx)
{
    CookResult fail(ctx.allocator);
    crd::containers::Array<crd::u8> src(ctx.allocator);
    if (!read_text(ctx, src)) { return fail; }
    crd::lightcook::LightingDesc desc(ctx.allocator);
    crd::containers::String      where(ctx.allocator);
    const auto err = crd::lightcook::parse_lighting_toml(view_of(src), desc, &where);
    if (err != crd::lightcook::LightingCookError::Ok)
    {
        report(ctx, crd::lightcook::lighting_cook_error_text(err), where);
        return fail;
    }
    return finish_blob(ctx, crd::containers::as_const_span(src)); // validated source (binary form = named row)
}

} // namespace

void register_authored_program_handlers()
{
    register_cook_handler(".frame.toml", frame_graph_handler, kAuthoredHandlerVersion);
    register_cook_handler(".crdt", technique_handler, kAuthoredHandlerVersion);
    register_cook_handler(".crdm", material_handler, kAuthoredHandlerVersion);
    register_cook_handler(".crdv", vertex_handler, kAuthoredHandlerVersion);
    register_cook_handler(".crdl", lighting_handler, kAuthoredHandlerVersion);
}

} // namespace crd::cooker
