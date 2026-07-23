// texture.cpp — GEO-3 stage 2b: the standalone image cook handler on OUR OWN codec family (ldr_decode: PNG · JPEG ·
// TGA · BMP — engine/resources), stb_image RETIRED. Decode → `.meta` [cook] color-space options (srgb / normal_map;
// the texture_cook.hpp contract) → the shared mip/encode core (sRGB filtered in LINEAR space) → TXTR CRDR (ADR-0042).

#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/texture_cook.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/ldr_image.hpp>

#include <cstdio>

namespace fs = crd::platform::fs;

namespace crd::cooker
{
namespace
{

// v2: ldr_decode replaces stb_image; sRGB-aware linear-space mips replace the byte-space box filter.
constexpr crd::u32 kTextureHandlerVersion = 2U;

CookResult texture_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);

    crd::containers::Array<crd::u8> src_bytes(ctx.allocator);
    if (!fs::read_file_binary(fs::Path(ctx.source_path), src_bytes)) { return result; }

    crd::resources::LdrImage image(ctx.allocator);
    const crd::resources::LdrError err =
        crd::resources::ldr_decode(crd::containers::as_const_span(src_bytes), image, ctx.allocator);
    if (err != crd::resources::LdrError::Ok)
    {
        std::fprintf(stderr, "texture: decode failed (LdrError %u) for %.*s\n", static_cast<unsigned>(err),
                     static_cast<int>(ctx.source_path.size()), ctx.source_path.data());
        return result;
    }

    TextureCookOptions options; // default: sRGB color (the standalone .png/.jpg reality); .meta opts out for data
    if (!ctx.meta_path.empty())
    {
        crd::containers::String meta_text(ctx.allocator);
        if (fs::read_file_text(fs::Path(ctx.meta_path), meta_text))
        {
            options = parse_texture_cook_options(crd::containers::StringView(meta_text.data(), meta_text.size()));
        }
    }

    auto cooked = cook_texture_rgba(image, options, ctx.id, ctx.allocator);
    if (cooked.empty()) { return result; }

    result.type_fourcc     = crd::resources::kFourCC_TXTR;
    result.cooked_bytes    = static_cast<crd::containers::Array<crd::u8>&&>(cooked);
    result.handler_version = kTextureHandlerVersion;
    result.ok              = true;
    return result;
}

} // anonymous namespace

void register_texture_handler()
{
    register_cook_handler(".png", texture_handler);
    register_cook_handler(".jpg", texture_handler);
    register_cook_handler(".jpeg", texture_handler);
    register_cook_handler(".tga", texture_handler);
    register_cook_handler(".bmp", texture_handler);
}

} // namespace crd::cooker
