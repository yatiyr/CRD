#include <crd/cooker/cook_handler.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>

namespace fs = crd::platform::fs;

namespace crd::cooker
{

namespace
{

constexpr crd::u32 kBlobHandlerVersion = 1U;

CookResult blob_passthrough_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);

    crd::containers::Array<crd::u8> src_bytes(ctx.allocator);
    if (!fs::read_file_binary(fs::Path(ctx.source_path), src_bytes))
    {
        return result; // ok = false
    }

    crd::resources::CrdrWriter writer(
        ctx.allocator, ctx.id, crd::resources::kFourCC_BLOB);
    writer.add_chunk_compressed(
        crd::resources::kFourCC_BLOB,
        crd::containers::as_const_span(src_bytes));

    result.type_fourcc     = crd::resources::kFourCC_BLOB;
    result.cooked_bytes    = writer.finish();
    result.source_hash     = 0; // caller computes this
    result.options_hash    = 0;
    result.handler_version = kBlobHandlerVersion;
    result.ok              = true;
    return result;
}

} // anonymous namespace

// Forward declarations for other handler registration functions in this directory.
void register_glsl_handler();
void register_material_handler();

void register_builtin_handlers()
{
    register_cook_handler(".bin", blob_passthrough_handler);
    register_glsl_handler();
    register_material_handler();
}

} // namespace crd::cooker
