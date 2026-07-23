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
#include <crd/cooker/cook_io.hpp>
#include <crd/cooker/obek_cooker.hpp>

#include <cstdio>
#include <cstring>

namespace crd::cooker
{

namespace
{

constexpr crd::u32 kObekHandlerVersion = 1U;

// File resolver — satisfies `extends` / nested `obek =` sibling references THROUGH CookIO, so every referenced
// .obek.toml is a RECORDED dependency edge (touch a base obek → the derived one recooks).
bool obek_file_resolver(crd::containers::StringView path,
                        crd::memory::IAllocator*    alloc,
                        crd::containers::String&    out_text,
                        void*                       ud_void)
{
    auto* io = static_cast<CookIO*>(ud_void);
    if (io == nullptr) { return false; }

    crd::containers::Array<crd::u8> bytes(alloc);
    if (!io->read_input(path, bytes)) { return false; }
    out_text.clear();
    out_text.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

CookResult obek_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);

    crd::containers::Array<crd::u8> src_bytes(ctx.allocator);
    if (!ctx.io->read_source(src_bytes))
        return result;
    crd::containers::String text(ctx.allocator);
    text.append(reinterpret_cast<const char*>(src_bytes.data()), src_bytes.size());

    crd::cooker::ObekCookContext octx{};
    octx.id              = ctx.id;
    octx.allocator       = ctx.allocator;
    octx.obek_root_id    = cook_hash64(crd::containers::ConstSpan<crd::u8>(
        reinterpret_cast<const crd::u8*>(ctx.source_path.data()), ctx.source_path.size()));
    octx.file_resolver   = obek_file_resolver;
    octx.file_resolver_ud = ctx.io;

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
    register_cook_handler(".obek.toml", obek_handler, kObekHandlerVersion);
}

} // namespace crd::cooker
