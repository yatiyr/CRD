#include <crd/cooker/cook_handler.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/shader/compile.hpp>
#include <crd/shader/types.hpp>

namespace fs = crd::platform::fs;

namespace crd::cooker
{
namespace
{

constexpr crd::u32 kGlslHandlerVersion = 1U;

[[nodiscard]] crd::shader::Stage stage_from_path(crd::containers::StringView path) noexcept
{
    if (path.size() >= 10U && path.substr(path.size() - 10U) == ".frag.glsl")
    {
        return crd::shader::Stage::Fragment;
    }
    if (path.size() >= 10U && path.substr(path.size() - 10U) == ".comp.glsl")
    {
        return crd::shader::Stage::Compute;
    }
    return crd::shader::Stage::Vertex; // .vert.glsl or fallback
}

[[nodiscard]] crd::u32 spirv_chunk_fourcc(crd::shader::Stage stage) noexcept
{
    switch (stage)
    {
        case crd::shader::Stage::Fragment:
            return crd::resources::kFourCC_SPVF;
        case crd::shader::Stage::Compute:
            return crd::resources::kFourCC_SPVC;
        case crd::shader::Stage::Vertex:
        default:
            return crd::resources::kFourCC_SPVV;
    }
}

CookResult glsl_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);

    crd::containers::String source_text(ctx.allocator);
    if (!fs::read_file_text(fs::Path(ctx.source_path), source_text))
    {
        return result; // ok = false
    }

    const crd::shader::Stage stage = stage_from_path(ctx.source_path);

    // Extract file name for error messages.
    const auto slash = ctx.source_path.rfind('/');
    const crd::containers::StringView name =
        (slash != crd::containers::StringView::npos)
            ? ctx.source_path.substr(slash + 1U)
            : ctx.source_path;

    crd::shader::CompileResult compiled = crd::shader::compile_glsl(
        stage,
        crd::containers::StringView(source_text.data(), source_text.size()),
        name,
        ctx.allocator);

    if (!compiled.ok)
    {
        return result; // ok = false; caller prints stderr
    }

    crd::resources::CrdrWriter writer(ctx.allocator, ctx.id, crd::resources::kFourCC_SHDR);
    writer.add_chunk(
        spirv_chunk_fourcc(stage),
        crd::containers::as_const_span(compiled.spirv));

    result.type_fourcc     = crd::resources::kFourCC_SHDR;
    result.cooked_bytes    = writer.finish();
    result.handler_version = kGlslHandlerVersion;
    result.ok              = true;
    return result;
}

} // anonymous namespace

void register_glsl_handler()
{
    register_cook_handler(".vert.glsl", glsl_handler);
    register_cook_handler(".frag.glsl", glsl_handler);
    register_cook_handler(".comp.glsl", glsl_handler);
}

} // namespace crd::cooker
