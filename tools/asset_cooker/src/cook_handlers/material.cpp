#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/cook_io.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>

// Mirror on-disk enum values from ADR-0048 — do not include renderer headers.
// Values must match MaterialDomain and PassType in material_domain.hpp / pass_type.hpp.
static constexpr crd::u8 kDomainSurface = 0U;

static constexpr crd::u8 kPassDepthPrepass = 0U;
// static constexpr crd::u8 kPassShadow    = 1U;  // reserved
static constexpr crd::u8 kPassForward   = 2U;

#include <cstring>

namespace crd::cooker
{
namespace
{

constexpr crd::u32 kMaterialHandlerVersion = 2U;

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

// Return true if the line contains the given section header (e.g. "[passes.forward]").
[[nodiscard]] bool is_section(crd::containers::StringView line,
                              crd::containers::StringView section) noexcept
{
    const auto p = line.find(section);
    return p != crd::containers::StringView::npos;
}

// Per-pass shader UUID pair parsed from the .mat.toml [passes.*] sections.
struct PassEntry
{
    crd::u8                    pass_type;
    crd::resources::ResourceId vert_id;
    crd::resources::ResourceId frag_id;
};

// Emit one PASS chunk entry (36 bytes) into `buf`.
void write_pass_entry(crd::u8*                          buf,
                      const PassEntry&                  e) noexcept
{
    buf[0] = e.pass_type;
    buf[1] = 0U;
    buf[2] = 0U;
    buf[3] = 0U;
    std::memcpy(buf +  4, &e.vert_id.hi, 8);
    std::memcpy(buf + 12, &e.vert_id.lo, 8);
    std::memcpy(buf + 20, &e.frag_id.hi, 8);
    std::memcpy(buf + 28, &e.frag_id.lo, 8);
}

CookResult material_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);

    crd::containers::Array<crd::u8> src_bytes(ctx.allocator);
    if (!ctx.io->read_source(src_bytes))
    {
        return result;
    }
    crd::containers::String text(ctx.allocator);
    text.append(reinterpret_cast<const char*>(src_bytes.data()), src_bytes.size());

    // ── Parse .mat.toml ────────────────────────────────────────────────────
    // Supported keys:
    //   vertex_shader   = "<uuid>"   (legacy flat format → [passes.forward])
    //   fragment_shader = "<uuid>"   (legacy flat format → [passes.forward])
    //
    //   [passes.forward]
    //     vertex_shader   = "<uuid>"
    //     fragment_shader = "<uuid>"
    //
    //   [passes.depth_prepass]
    //     vertex_shader   = "<uuid>"
    //     fragment_shader = "<uuid>"

    crd::resources::ResourceId fwd_vert;
    crd::resources::ResourceId fwd_frag;
    crd::resources::ResourceId depth_vert;
    crd::resources::ResourceId depth_frag;

    enum class Section : crd::u8 { None, Forward, DepthPrepass } cur_section = Section::None;

    crd::containers::StringView sv(text.data(), text.size());
    while (!sv.empty())
    {
        const auto nl = sv.find('\n');
        crd::containers::StringView line =
            (nl != crd::containers::StringView::npos) ? sv.substr(0U, nl) : sv;

        // Strip carriage return.
        if (!line.empty() && line.back() == '\r')
        {
            line = line.substr(0U, line.size() - 1U);
        }

        // Section headers.
        if (is_section(line, "[passes.forward]"))
        {
            cur_section = Section::Forward;
        }
        else if (is_section(line, "[passes.depth_prepass]"))
        {
            cur_section = Section::DepthPrepass;
        }
        else if (!line.empty() && line[0] == '[')
        {
            cur_section = Section::None;
        }

        // Key parsing.
        const auto vval = parse_quoted_value(line, "vertex_shader");
        if (!vval.empty())
        {
            const auto id = crd::resources::ResourceId::parse(vval);
            if (cur_section == Section::Forward || cur_section == Section::None)
            {
                fwd_vert = id;
            }
            else if (cur_section == Section::DepthPrepass)
            {
                depth_vert = id;
            }
        }

        const auto fval = parse_quoted_value(line, "fragment_shader");
        if (!fval.empty())
        {
            const auto id = crd::resources::ResourceId::parse(fval);
            if (cur_section == Section::Forward || cur_section == Section::None)
            {
                fwd_frag = id;
            }
            else if (cur_section == Section::DepthPrepass)
            {
                depth_frag = id;
            }
        }

        if (nl == crd::containers::StringView::npos)
        {
            break;
        }
        sv = sv.substr(nl + 1U);
    }

    // At minimum the Forward pass must have both shaders.
    if (fwd_vert.is_null() || fwd_frag.is_null())
    {
        return result;
    }

    // ── Collect valid pass entries ─────────────────────────────────────────
    crd::containers::Array<PassEntry> passes(ctx.allocator);

    passes.push_back({kPassForward, fwd_vert, fwd_frag});
    result.dependencies.push_back(fwd_vert);
    result.dependencies.push_back(fwd_frag);

    if (!depth_vert.is_null() && !depth_frag.is_null())
    {
        passes.push_back({kPassDepthPrepass, depth_vert, depth_frag});
        result.dependencies.push_back(depth_vert);
        result.dependencies.push_back(depth_frag);
    }

    // ── INFO chunk (4 bytes) ────────────────────────────────────────────────
    crd::u8 info[4] = {
        static_cast<crd::u8>(kMaterialHandlerVersion),
        kDomainSurface,
        0U, // flags
        0U, // pad
    };

    // ── PASS chunk ──────────────────────────────────────────────────────────
    // Header: count u32 (4 bytes)
    // Per entry: 36 bytes (pass_type u8, pad u8[3], vert_id u8[16], frag_id u8[16])
    const crd::u32 pass_count = static_cast<crd::u32>(passes.size());
    constexpr crd::usize pass_entry_size = 36U;
    const crd::usize pass_chunk_size = sizeof(crd::u32) + pass_count * pass_entry_size;

    crd::containers::Array<crd::u8> pass_bytes(ctx.allocator);
    pass_bytes.resize(pass_chunk_size);
    std::memcpy(pass_bytes.data(), &pass_count, sizeof(crd::u32));
    for (crd::u32 i = 0; i < pass_count; ++i)
    {
        write_pass_entry(pass_bytes.data() + sizeof(crd::u32) + i * pass_entry_size, passes[i]);
    }

    crd::resources::CrdrWriter writer(ctx.allocator, ctx.id, crd::resources::kFourCC_MATR);
    writer.add_chunk(crd::resources::kFourCC_INFO,
                     crd::containers::ConstSpan<crd::u8>(info, sizeof(info)));
    writer.add_chunk(crd::resources::kFourCC_PASS,
                     crd::containers::as_const_span(pass_bytes));

    result.type_fourcc     = crd::resources::kFourCC_MATR;
    result.cooked_bytes    = writer.finish();
    result.handler_version = kMaterialHandlerVersion;
    result.ok              = true;
    return result;
}

} // anonymous namespace

void register_material_handler()
{
    register_cook_handler(".mat.toml", material_handler, kMaterialHandlerVersion);
}

} // namespace crd::cooker
