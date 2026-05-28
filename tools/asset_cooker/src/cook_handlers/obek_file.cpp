// Phase 3.0 v1o3 — `.obek.toml` → OBEK file-handler.
//
// Thin wrapper around the existing `ObekCooker::cook_inline` (declared in
// `crd/cooker/obek_cooker.hpp`). The cooker class itself is the real
// implementation — this file just plugs it into the asset cooker's
// extension dispatch table so a `.obek.toml` source file in
// `assets/source/` cooks alongside meshes, textures, and presets.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/obek_cooker.hpp>
#include <crd/platform/filesystem.hpp>

#include <cstdio>
#include <cstring>

namespace fs = crd::platform::fs;

namespace crd::cooker
{

namespace
{

constexpr crd::u32 kObekHandlerVersion = 1U;

// FNV-1a 64 over a byte range. Same constants as cook_command.cpp's
// helper; duplicated here to keep this file self-contained without
// pulling cook_command.hpp into the handler surface.
crd::u64 fnv1a64(const crd::u8* data, crd::usize size) noexcept
{
    constexpr crd::u64 fnv_offset64 = 14695981039346656037ULL;
    constexpr crd::u64 fnv_prime64  = 1099511628211ULL;

    crd::u64 hash = fnv_offset64;
    for (crd::usize i = 0U; i < size; ++i)
    {
        hash ^= static_cast<crd::u64>(data[i]);
        hash *= fnv_prime64;
    }
    return hash;
}

// File resolver — reads sibling .obek.toml files from disk. Used for
// `extends` / nested `obek =` references inside the cooker. We pass
// the source-dir as user_data so the resolver can build absolute
// paths from relative TOML strings.
struct ResolverUd
{
    fs::Path source_dir;
};

bool obek_file_resolver(crd::containers::StringView path,
                        crd::memory::IAllocator*    alloc,
                        crd::containers::String&    out_text,
                        void*                       ud_void)
{
    auto* ud = static_cast<ResolverUd*>(ud_void);
    if (ud == nullptr) return false;

    crd::containers::String full(alloc);
    full.append(ud->source_dir.generic().data(), ud->source_dir.generic().size());
    full.append("/");
    full.append(path.data(), path.size());
    const fs::Path full_path(crd::containers::StringView{full.data(), full.size()});
    return fs::read_file_text(full_path, out_text);
}

CookResult obek_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);

    crd::containers::String text(ctx.allocator);
    if (!fs::read_file_text(fs::Path(ctx.source_path), text))
        return result;

    // Resolve source directory so the file resolver can satisfy
    // sibling references.
    fs::Path source_dir;
    {
        const auto sp    = ctx.source_path;
        const auto slash = sp.rfind('/');
        if (slash != crd::containers::StringView::npos)
            source_dir = fs::Path(sp.substr(0U, slash));
    }

    ResolverUd ud{};
    ud.source_dir = source_dir;

    crd::cooker::ObekCookContext octx{};
    octx.id              = ctx.id;
    octx.allocator       = ctx.allocator;
    octx.obek_root_id    = fnv1a64(reinterpret_cast<const crd::u8*>(ctx.source_path.data()),
                                   ctx.source_path.size());
    octx.file_resolver   = obek_file_resolver;
    octx.file_resolver_ud = &ud;

    crd::containers::Array<CookError> errors(ctx.allocator);
    auto bytes = crd::cooker::obek_cooker_inline(
        crd::containers::StringView{text.data(), text.size()}, octx, &errors);

    if (!errors.empty() || bytes.empty())
    {
        for (const auto& e : errors)
        {
            std::fprintf(stderr,
                         "obek cook: %.*s:%u:%u: %s\n",
                         static_cast<int>(ctx.source_path.size()), ctx.source_path.data(),
                         e.line, e.column, e.message.c_str());
        }
        if (errors.empty())
        {
            std::fprintf(stderr, "obek cook: %.*s produced empty output\n",
                         static_cast<int>(ctx.source_path.size()), ctx.source_path.data());
        }
        return result;
    }

    // OBEK FourCC is 'OBEK' (declared in crd/scene/obek.hpp); use the
    // make_fourcc helper rather than including the header here.
    constexpr crd::u32 obek_four_cc =
        (static_cast<crd::u32>('O')      ) |
        (static_cast<crd::u32>('B') <<  8) |
        (static_cast<crd::u32>('E') << 16) |
        (static_cast<crd::u32>('K') << 24);

    result.type_fourcc     = obek_four_cc;
    result.cooked_bytes    = std::move(bytes);
    result.handler_version = kObekHandlerVersion;
    result.ok              = true;
    return result;
}

} // anonymous namespace

void register_obek_handler()
{
    register_cook_handler(".obek.toml", obek_handler);
}

} // namespace crd::cooker
