#include <crd/cooker/cook_handler.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>

#include <cstring>

namespace fs = crd::platform::fs;

namespace crd::cooker
{
namespace
{

constexpr crd::u32 kMaterialHandlerVersion = 1U;

// Parse: key = "value"  (returns the quoted string without surrounding quotes)
// Returns empty view on failure.
[[nodiscard]] crd::containers::StringView parse_quoted_value(
    crd::containers::StringView line, crd::containers::StringView key) noexcept
{
    const auto key_pos = line.find(key);
    if (key_pos == crd::containers::StringView::npos)
    {
        return {};
    }
    const auto eq_pos = line.find('=', key_pos + key.size());
    if (eq_pos == crd::containers::StringView::npos)
    {
        return {};
    }
    const auto q_open = line.find('"', eq_pos + 1U);
    if (q_open == crd::containers::StringView::npos)
    {
        return {};
    }
    const auto q_close = line.find('"', q_open + 1U);
    if (q_close == crd::containers::StringView::npos)
    {
        return {};
    }
    return line.substr(q_open + 1U, q_close - q_open - 1U);
}

CookResult material_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);

    crd::containers::String text(ctx.allocator);
    if (!fs::read_file_text(fs::Path(ctx.source_path), text))
    {
        return result;
    }

    crd::resources::ResourceId vert_id;
    crd::resources::ResourceId frag_id;

    // Walk lines and parse vertex_shader / fragment_shader UUID keys.
    crd::containers::StringView sv(text.data(), text.size());
    while (!sv.empty())
    {
        const auto nl = sv.find('\n');
        const crd::containers::StringView line =
            (nl != crd::containers::StringView::npos) ? sv.substr(0U, nl) : sv;

        // Strip carriage return for Windows line endings.
        crd::containers::StringView clean = line;
        if (!clean.empty() && clean.back() == '\r')
        {
            clean = clean.substr(0U, clean.size() - 1U);
        }

        const crd::containers::StringView vert_val =
            parse_quoted_value(clean, "vertex_shader");
        if (!vert_val.empty())
        {
            vert_id = crd::resources::ResourceId::parse(vert_val);
        }

        const crd::containers::StringView frag_val =
            parse_quoted_value(clean, "fragment_shader");
        if (!frag_val.empty())
        {
            frag_id = crd::resources::ResourceId::parse(frag_val);
        }

        if (nl == crd::containers::StringView::npos)
        {
            break;
        }
        sv = sv.substr(nl + 1U);
    }

    if (vert_id.is_null() || frag_id.is_null())
    {
        return result; // missing keys → failure
    }

    // META chunk: 32 bytes (vertex UUID hi+lo, fragment UUID hi+lo)
    crd::u8 meta[32];
    std::memcpy(meta +  0, &vert_id.hi, 8);
    std::memcpy(meta +  8, &vert_id.lo, 8);
    std::memcpy(meta + 16, &frag_id.hi, 8);
    std::memcpy(meta + 24, &frag_id.lo, 8);

    crd::resources::CrdrWriter writer(ctx.allocator, ctx.id, crd::resources::kFourCC_MATR);
    writer.add_chunk(crd::resources::kFourCC_META,
                     crd::containers::ConstSpan<crd::u8>(meta, 32U));

    result.dependencies.push_back(vert_id);
    result.dependencies.push_back(frag_id);
    result.type_fourcc     = crd::resources::kFourCC_MATR;
    result.cooked_bytes    = writer.finish();
    result.handler_version = kMaterialHandlerVersion;
    result.ok              = true;
    return result;
}

} // anonymous namespace

void register_material_handler()
{
    register_cook_handler(".mat.toml", material_handler);
}

} // namespace crd::cooker
