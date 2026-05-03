#include <crd/cooker/cook_command.hpp>
#include <crd/cooker/cook_handler.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace fs = crd::platform::fs;
using crd::resources::ResourceId;
using crd::resources::ManifestEntry;

namespace crd::cooker
{

namespace
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::MallocAllocator g_cook_alloc;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// ── FNV-1a 64-bit hash ─────────────────────────────────────────────────────

constexpr crd::u64 kFnvOffset64 = 14695981039346656037ULL;
constexpr crd::u64 kFnvPrime64  = 1099511628211ULL;

crd::u64 fnv1a64(const crd::u8* data, crd::usize size) noexcept
{
    crd::u64 hash = kFnvOffset64;
    for (crd::usize i = 0U; i < size; ++i)
    {
        hash ^= static_cast<crd::u64>(data[i]);
        hash *= kFnvPrime64;
    }
    return hash;
}

// ── Path helpers ───────────────────────────────────────────────────────────

crd::containers::StringView path_extension(const fs::Path& p)
{
    const crd::containers::StringView sv    = p.generic();
    const auto                        dot   = sv.rfind('.');
    const auto                        slash = sv.rfind('/');
    if (dot == crd::containers::StringView::npos)
    {
        return {};
    }
    if (slash != crd::containers::StringView::npos && dot < slash)
    {
        return {};
    }
    return sv.substr(dot);
}

crd::containers::StringView path_filename(const fs::Path& p)
{
    const crd::containers::StringView sv    = p.generic();
    const auto                        slash = sv.rfind('/');
    if (slash == crd::containers::StringView::npos)
    {
        return sv;
    }
    return sv.substr(slash + 1U);
}

// ── Meta file (.meta sidecar) ─────────────────────────────────────────────
//
// Format written by this tool:
//   [id]
//   uuid = "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"

bool meta_read(const fs::Path& meta_path, ResourceId& out_id)
{
    crd::containers::String text(&g_cook_alloc);
    if (!fs::read_file_text(meta_path, text))
    {
        return false;
    }
    const std::string_view sv(text.data(), text.size());
    const std::string_view key = "uuid = \"";
    auto pos = sv.find(key);
    if (pos == std::string_view::npos)
    {
        return false;
    }
    pos += key.size();
    const auto end = sv.find('"', pos);
    if (end == std::string_view::npos)
    {
        return false;
    }
    out_id = ResourceId::parse(sv.substr(pos, end - pos));
    return !out_id.is_null();
}

bool meta_write(const fs::Path& meta_path, const ResourceId& id)
{
    const auto id_str = id.to_string(&g_cook_alloc);
    crd::containers::String content(&g_cook_alloc);
    content.append("[id]\n");
    content.append("uuid = \"");
    content.append(id_str.c_str());
    content.append("\"\n");
    return fs::write_file_text(
        meta_path, crd::containers::StringView(content.data(), content.size()));
}

// ── Cook cache ─────────────────────────────────────────────────────────────

fs::Path cache_dir(const fs::Path& root)
{
    return root / ".cook_cache";
}

fs::Path cache_key_path(const fs::Path& root, const ResourceId& id)
{
    const auto id_str = id.to_string(&g_cook_alloc);
    crd::containers::String name(&g_cook_alloc);
    name.append(id_str.c_str());
    name.append(".key");
    return cache_dir(root) / crd::containers::StringView(name.data(), name.size());
}

fs::Path cache_artifact_path(const fs::Path& root, const ResourceId& id)
{
    const auto id_str = id.to_string(&g_cook_alloc);
    crd::containers::String name(&g_cook_alloc);
    name.append(id_str.c_str());
    name.append(".crdr");
    return cache_dir(root) / crd::containers::StringView(name.data(), name.size());
}

bool cache_read_key(const fs::Path& key_path, crd::u64& out_key)
{
    crd::containers::Array<crd::u8> bytes(&g_cook_alloc);
    if (!fs::read_file_binary(key_path, bytes) || bytes.size() != sizeof(crd::u64))
    {
        return false;
    }
    std::memcpy(&out_key, bytes.data(), sizeof(crd::u64));
    return true;
}

bool cache_write_key(const fs::Path& key_path, crd::u64 cook_key)
{
    crd::u8 buf[sizeof(crd::u64)];
    std::memcpy(buf, &cook_key, sizeof(crd::u64));
    return fs::write_file_binary(
        key_path, crd::containers::ConstSpan<crd::u8>(buf, sizeof(buf)));
}

// ── Recursive directory scan ───────────────────────────────────────────────

void scan_recursive(const fs::Path& dir, crd::containers::Array<fs::Path>& out)
{
    crd::containers::Array<fs::Path> entries(&g_cook_alloc);
    fs::list_directory(dir, entries);

    for (crd::usize i = 0U; i < entries.size(); ++i)
    {
        const fs::Path&                  entry = entries[i];
        const crd::containers::StringView name  = path_filename(entry);

        if (fs::is_directory(entry))
        {
            if (name != ".cook_cache")
            {
                scan_recursive(entry, out);
            }
        }
        else if (fs::is_file(entry) && path_extension(entry) != ".meta")
        {
            out.push_back(entry);
        }
    }
}

// ── Log entry ──────────────────────────────────────────────────────────────

struct LogEntry
{
    crd::containers::String rel_path;
    crd::containers::String uuid;
    const char*             status; // "cooked", "skipped", "no_handler"
};

} // anonymous namespace

// ── cmd_cook ───────────────────────────────────────────────────────────────

int cmd_cook(const char* root_cstr, const char* out_cstr)
{
    const fs::Path root(root_cstr);
    const fs::Path out_path(out_cstr);

    if (!fs::is_directory(root))
    {
        std::fprintf(stderr, "cook: root directory not found: %s\n", root_cstr);
        return 1;
    }

    const fs::Path cache = cache_dir(root);
    if (!fs::create_directories(cache))
    {
        std::fprintf(stderr, "cook: failed to create cache dir: %s\n", cache.generic().data());
        return 1;
    }

    // Recursive scan, sorted for determinism.
    crd::containers::Array<fs::Path> source_files(&g_cook_alloc);
    scan_recursive(root, source_files);
    std::sort(source_files.begin(), source_files.end(),
              [](const fs::Path& a, const fs::Path& b)
              {
                  return a.generic() < b.generic();
              });

    struct ArtifactInfo
    {
        ResourceId                      id;
        crd::containers::String         rel_path;
        crd::u32                        type_fourcc;
        crd::containers::Array<crd::u8> bytes;
        bool                            from_cache;
    };

    crd::containers::Array<ArtifactInfo> artifacts(&g_cook_alloc);
    crd::containers::Array<LogEntry>     log_entries(&g_cook_alloc);
    artifacts.reserve(source_files.size());
    log_entries.reserve(source_files.size());

    const crd::containers::StringView root_sv = root.generic();

    for (const fs::Path& src : source_files)
    {
        const crd::containers::StringView ext = path_extension(src);

        // Derive relative path (strip root prefix + leading slash).
        crd::containers::StringView rel_sv = src.generic();
        if (rel_sv.starts_with(root_sv))
        {
            rel_sv = rel_sv.substr(root_sv.size());
            if (!rel_sv.empty() && rel_sv.front() == '/')
            {
                rel_sv = rel_sv.substr(1U);
            }
        }

        // Resolve .meta sidecar.
        crd::containers::String meta_str(&g_cook_alloc);
        meta_str.append(src.generic().data(), src.generic().size());
        meta_str.append(".meta");
        const fs::Path meta_path(
            crd::containers::StringView(meta_str.data(), meta_str.size()));

        ResourceId asset_id;
        if (fs::is_file(meta_path))
        {
            if (!meta_read(meta_path, asset_id))
            {
                std::fprintf(stderr, "cook: malformed .meta for %s, regenerating\n",
                             src.generic().data());
                asset_id = ResourceId::mint_random();
                (void)meta_write(meta_path, asset_id);
            }
        }
        else
        {
            asset_id = ResourceId::mint_random();
            if (!meta_write(meta_path, asset_id))
            {
                std::fprintf(stderr, "cook: failed to write .meta for %s\n",
                             src.generic().data());
                return 1;
            }
        }

        const auto id_str = asset_id.to_string(&g_cook_alloc);

        const CookHandlerFn handler = find_cook_handler(ext);
        if (handler == nullptr)
        {
            LogEntry le;
            le.rel_path = crd::containers::String(rel_sv.data(), rel_sv.size(), &g_cook_alloc);
            le.uuid     = crd::containers::String(id_str.c_str(), &g_cook_alloc);
            le.status   = "no_handler";
            log_entries.push_back(std::move(le));
            continue;
        }

        // Read source bytes for hash.
        crd::containers::Array<crd::u8> src_bytes(&g_cook_alloc);
        if (!fs::read_file_binary(src, src_bytes))
        {
            std::fprintf(stderr, "cook: failed to read: %s\n", src.generic().data());
            return 1;
        }
        const crd::u64 source_hash = fnv1a64(src_bytes.data(), src_bytes.size());

        const fs::Path key_file      = cache_key_path(root, asset_id);
        const fs::Path artifact_file = cache_artifact_path(root, asset_id);

        crd::u64 cached_key = 0;
        const bool has_cached_key = cache_read_key(key_file, cached_key);

        bool                            from_cache = false;
        crd::containers::Array<crd::u8> artifact_bytes(&g_cook_alloc);

        // Always run the handler to get handler_version for cook_key computation.
        // If the computed cook_key matches the cached key, we use the cached artifact.
        CookContext ctx;
        ctx.source_path = src.generic();
        ctx.meta_path   = meta_path.generic();
        ctx.id          = asset_id;
        ctx.allocator   = &g_cook_alloc;

        CookResult result = handler(ctx);
        if (!result.ok)
        {
            std::fprintf(stderr, "cook: handler failed for %s\n", src.generic().data());
            return 1;
        }

        const crd::u64 cook_key =
            source_hash ^ static_cast<crd::u64>(result.handler_version);

        if (has_cached_key && cook_key == cached_key && fs::is_file(artifact_file))
        {
            // Cache hit: reuse cached artifact.
            if (fs::read_file_binary(artifact_file, artifact_bytes))
            {
                from_cache = true;
            }
        }

        if (!from_cache)
        {
            artifact_bytes = std::move(result.cooked_bytes);
            (void)cache_write_key(key_file, cook_key);
            if (!fs::write_file_binary(artifact_file,
                    crd::containers::as_const_span(artifact_bytes)))
            {
                std::fprintf(stderr, "cook: failed to write artifact cache: %s\n",
                             artifact_file.generic().data());
                return 1;
            }
        }

        // Parse artifact to get type_fourcc.
        crd::resources::CrdrFile crdr_file(&g_cook_alloc);
        const crd::resources::CrdrError parse_err =
            crd::resources::crdr_read(
                crd::containers::as_const_span(artifact_bytes), crdr_file, &g_cook_alloc);
        if (parse_err != crd::resources::CrdrError::Ok)
        {
            std::fprintf(stderr, "cook: artifact parse failed for %s\n",
                         src.generic().data());
            return 1;
        }

        ArtifactInfo info;
        info.id          = asset_id;
        info.rel_path    = crd::containers::String(rel_sv.data(), rel_sv.size(), &g_cook_alloc);
        info.type_fourcc = crdr_file.type_fourcc;
        info.bytes       = std::move(artifact_bytes);
        info.from_cache  = from_cache;
        artifacts.push_back(std::move(info));

        {
            LogEntry le;
            le.rel_path = crd::containers::String(rel_sv.data(), rel_sv.size(), &g_cook_alloc);
            le.uuid     = crd::containers::String(id_str.c_str(), &g_cook_alloc);
            le.status   = from_cache ? "skipped" : "cooked";
            log_entries.push_back(std::move(le));
        }
    }

    // Assemble PACK.
    //
    // pack_id is derived from the root path so the PACK container UUID is
    // deterministic across runs (required for byte-identical second-run check).
    const ResourceId pack_id = ResourceId::from_content(
        crd::containers::ConstSpan<crd::u8>(
            reinterpret_cast<const crd::u8*>(root_sv.data()), root_sv.size()));

    // Build string pool.
    crd::containers::Array<crd::u8>  strp(&g_cook_alloc);
    crd::containers::Array<crd::u32> strp_offsets(&g_cook_alloc);
    strp_offsets.reserve(artifacts.size());
    for (const ArtifactInfo& info : artifacts)
    {
        strp_offsets.push_back(static_cast<crd::u32>(strp.size()));
        const char*     rp      = info.rel_path.data();
        const crd::usize rp_size = info.rel_path.size();
        for (crd::usize j = 0U; j < rp_size; ++j)
        {
            strp.push_back(static_cast<crd::u8>(rp[j]));
        }
        strp.push_back(static_cast<crd::u8>('\0'));
    }

    // Build manifest entries with placeholder offsets.
    crd::containers::Array<ManifestEntry> entries(&g_cook_alloc);
    entries.reserve(artifacts.size());
    for (crd::usize i = 0U; i < artifacts.size(); ++i)
    {
        ManifestEntry e;
        e.id            = artifacts[i].id;
        e.type_fourcc   = artifacts[i].type_fourcc;
        e.flags         = 0;
        e.blob_offset   = 0;
        e.blob_size     = artifacts[i].bytes.size();
        e.name_strp_idx = strp_offsets[i];
        entries.push_back(e);
    }

    // Pass 1: measure CRDR container size with dummy offsets.
    {
        crd::resources::CrdrWriter w1(&g_cook_alloc, pack_id, crd::resources::kFourCC_PACK);
        manifest_write(
            w1,
            crd::containers::as_const_span(entries),
            crd::containers::as_const_span(strp));
        const crd::containers::Array<crd::u8> crdr_pass1 = w1.finish();

        // Compute real blob_offsets.
        crd::usize blob_pos = crdr_pass1.size();
        for (crd::usize i = 0U; i < artifacts.size(); ++i)
        {
            entries[i].blob_offset = static_cast<crd::u64>(blob_pos);
            blob_pos += artifacts[i].bytes.size();
        }
    }

    // Pass 2: build final CRDR with correct offsets.
    crd::resources::CrdrWriter w2(&g_cook_alloc, pack_id, crd::resources::kFourCC_PACK);
    manifest_write(
        w2,
        crd::containers::as_const_span(entries),
        crd::containers::as_const_span(strp));
    crd::containers::Array<crd::u8> pack_bytes = w2.finish();

    for (const ArtifactInfo& info : artifacts)
    {
        for (crd::u8 byte : info.bytes)
        {
            pack_bytes.push_back(byte);
        }
    }

    if (!fs::write_file_binary(out_path, crd::containers::as_const_span(pack_bytes)))
    {
        std::fprintf(stderr, "cook: failed to write pack: %s\n", out_cstr);
        return 1;
    }

    // Write cook.log.toml adjacent to out_path.
    const crd::containers::StringView out_sv    = out_path.generic();
    const auto                        out_slash  = out_sv.rfind('/');
    const crd::containers::StringView log_dir_sv = (out_slash != crd::containers::StringView::npos)
                                                       ? out_sv.substr(0U, out_slash + 1U)
                                                       : crd::containers::StringView("./");
    crd::containers::String log_path_str(&g_cook_alloc);
    log_path_str.append(log_dir_sv.data(), log_dir_sv.size());
    log_path_str.append("cook.log.toml");
    const fs::Path log_path(
        crd::containers::StringView(log_path_str.data(), log_path_str.size()));

    crd::containers::String log(&g_cook_alloc);
    log.append("[cook]\n");
    log.append("root = \"");
    log.append(root.generic().data(), root.generic().size());
    log.append("\"\n");
    log.append("out = \"");
    log.append(out_path.generic().data(), out_path.generic().size());
    log.append("\"\n\n");

    for (const LogEntry& le : log_entries)
    {
        log.append("[[entries]]\n");
        log.append("path = \"");
        log.append(le.rel_path.c_str());
        log.append("\"\n");
        log.append("uuid = \"");
        log.append(le.uuid.c_str());
        log.append("\"\n");
        log.append("status = \"");
        log.append(le.status);
        log.append("\"\n\n");
    }

    if (!fs::write_file_text(log_path, crd::containers::StringView(log.data(), log.size())))
    {
        std::fprintf(stderr, "cook: failed to write cook.log.toml\n");
        return 1;
    }

    std::printf("cook: wrote %zu artifacts to %s\n", artifacts.size(), out_cstr);
    return 0;
}

} // namespace crd::cooker
