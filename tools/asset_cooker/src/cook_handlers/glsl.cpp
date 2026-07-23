#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/cook_io.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp> // ADR-0103: GLSL→SPIR-V owned by the Vulkan backend, not crd-shader
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/resources/crdr.hpp>

namespace crd::cooker
{
namespace
{

constexpr crd::u32 kGlslHandlerVersion = 1U;

[[nodiscard]] crd::gpu::ShaderStage stage_from_path(crd::containers::StringView path) noexcept
{
    if (path.size() >= 10U && path.substr(path.size() - 10U) == ".frag.glsl")
    {
        return crd::gpu::ShaderStage::Fragment;
    }
    if (path.size() >= 10U && path.substr(path.size() - 10U) == ".comp.glsl")
    {
        return crd::gpu::ShaderStage::Compute;
    }
    return crd::gpu::ShaderStage::Vertex; // .vert.glsl or fallback
}

[[nodiscard]] crd::u32 spirv_chunk_fourcc(crd::gpu::ShaderStage stage) noexcept
{
    switch (stage)
    {
        case crd::gpu::ShaderStage::Fragment:
            return crd::resources::kFourCC_SPVF;
        case crd::gpu::ShaderStage::Compute:
            return crd::resources::kFourCC_SPVC;
        case crd::gpu::ShaderStage::Vertex:
        default:
            return crd::resources::kFourCC_SPVV;
    }
}

CookResult glsl_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);

    crd::containers::Array<crd::u8> src_bytes(ctx.allocator);
    if (!ctx.io->read_source(src_bytes))
    {
        return result; // ok = false
    }
    crd::containers::String source_text(ctx.allocator);
    source_text.append(reinterpret_cast<const char*>(src_bytes.data()), src_bytes.size());

    const crd::gpu::ShaderStage stage = stage_from_path(ctx.source_path);

    // Extract file name for error messages.
    const auto slash = ctx.source_path.rfind('/');
    const crd::containers::StringView name =
        (slash != crd::containers::StringView::npos)
            ? ctx.source_path.substr(slash + 1U)
            : ctx.source_path;

    crd::gpu::ShaderCompileResult compiled = crd::gpu::compile_glsl_to_spirv(
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
    register_cook_handler(".vert.glsl", glsl_handler, kGlslHandlerVersion);
    register_cook_handler(".frag.glsl", glsl_handler, kGlslHandlerVersion);
    register_cook_handler(".comp.glsl", glsl_handler, kGlslHandlerVersion);
}

} // namespace crd::cooker
