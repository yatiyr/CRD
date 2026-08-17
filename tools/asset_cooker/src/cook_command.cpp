// cook_command.cpp — GEO-6 (D-007 row 71): the dependency-graph ASSET PROCESSOR (the Bevy-v2/O3DE consensus).
//
// Per source job: the cook database records the input edges CookIO saw last run (source · .meta · auxiliary files
// · id-stability sidecars, each with its content hash) and every product. The incremental decision re-hashes
// EXACTLY those edges — on a full match the handler NEVER RUNS and the cached artifacts serve (the ninja depfile
// model: new inputs can only appear if an old input changed, which forces the recook that discovers them).
// Undeclared dependencies are structurally impossible: handlers have no road to bytes except CookIO, so every
// byte a cook consumed IS an edge.
//
// Crash safety: `begin <src>` journals durably before a job's artifacts are written, `commit <src>` after; a
// killed run leaves a dangling begin and that source recooks unconditionally next run (its cache is distrusted).
// Artifacts, the database, and the PACK all publish via write-temp → rename — no consumer ever sees a torn file.

#include <crd/cooker/cook_command.hpp>
#include <crd/cooker/cook_db.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/cook_io.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace fs = crd::platform::fs;
using crd::resources::ManifestEntry;
using crd::resources::ResourceId;

namespace crd::cooker
{

namespace
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::GrowableTlsfAllocator g_cook_alloc;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// ── Path helpers ───────────────────────────────────────────────────────────

crd::containers::StringView path_extension(const fs::Path& p)
{
    const crd::containers::StringView sv    = p.generic();
    const auto                        slash = sv.rfind('/');
    const crd::usize fname_start = (slash != crd::containers::StringView::npos) ? slash + 1U : 0U;
    const crd::containers::StringView fname = sv.substr(fname_start);
    const auto                        dot   = fname.find('.');
    if (dot == crd::containers::StringView::npos) { return {}; }
    return sv.substr(fname_start + dot);
}

crd::containers::StringView path_filename(const fs::Path& p)
{
    const crd::containers::StringView sv    = p.generic();
    const auto                        slash = sv.rfind('/');
    if (slash == crd::containers::StringView::npos) { return sv; }
    return sv.substr(slash + 1U);
}

// strip the root prefix (+ leading '/') — the database speaks ROOT-RELATIVE paths
crd::containers::StringView root_relative(crd::containers::StringView path, crd::containers::StringView root)
{
    if (path.starts_with(root))
    {
        path = path.substr(root.size());
        if (!path.empty() && path.front() == '/') { path = path.substr(1U); }
    }
    return path;
}

// ── Meta file (.meta sidecar, the [id] section — the PROCESSOR owns main-asset identity) ──────────────────────

bool meta_read(const fs::Path& meta_path, ResourceId& out_id)
{
    crd::containers::String text(&g_cook_alloc);
    if (!fs::read_file_text(meta_path, text)) { return false; }
    const std::string_view sv(text.data(), text.size());
    const std::string_view key = "uuid = \"";
    auto                   pos = sv.find(key);
    if (pos == std::string_view::npos) { return false; }
    pos += key.size();
    const auto end = sv.find('"', pos);
    if (end == std::string_view::npos) { return false; }
    out_id = ResourceId::parse(sv.substr(pos, end - pos));
    return !out_id.is_null();
}

bool meta_write(const fs::Path& meta_path, const ResourceId& id)
{
    const auto              id_str = id.to_string(&g_cook_alloc);
    crd::containers::String content(&g_cook_alloc);
    content.append("[id]\n");
    content.append("uuid = \"");
    content.append(id_str.c_str());
    content.append("\"\n");
    return fs::write_file_text(meta_path, crd::containers::StringView(content.data(), content.size()));
}

// ── Cook cache paths ───────────────────────────────────────────────────────

fs::Path cache_dir(const fs::Path& root) { return root / ".cook_cache"; }

fs::Path cache_artifact_path(const fs::Path& root, const ResourceId& id)
{
    const auto              id_str = id.to_string(&g_cook_alloc);
    crd::containers::String name(&g_cook_alloc);
    name.append(id_str.c_str());
    name.append(".crdr");
    return cache_dir(root) / crd::containers::StringView(name.data(), name.size());
}

// write-temp → rename: an artifact file is either absent, the old version, or the new version — never torn
bool write_file_atomic(const fs::Path& path, crd::containers::ConstSpan<crd::u8> bytes)
{
    crd::containers::String tmp(&g_cook_alloc);
    tmp.append(path.generic().data(), path.generic().size());
    tmp.append(".tmp");
    const fs::Path tmp_path(crd::containers::StringView(tmp.data(), tmp.size()));
    if (!fs::write_file_binary(tmp_path, bytes)) { return false; }
    return fs::rename_file(tmp_path, path);
}

// ── Recursive directory scan ───────────────────────────────────────────────

void scan_recursive(const fs::Path& dir, crd::containers::Array<fs::Path>& out)
{
    crd::containers::Array<fs::Path> entries(&g_cook_alloc);
    fs::list_directory(dir, entries);

    for (crd::usize i = 0U; i < entries.size(); ++i)
    {
        const fs::Path&                   entry = entries[i];
        const crd::containers::StringView name  = path_filename(entry);

        if (fs::is_directory(entry))
        {
            if (name != ".cook_cache") { scan_recursive(entry, out); }
        }
        else if (fs::is_file(entry) && !name.ends_with(".meta"))
        {
            // Filter on filename suffix, not path_extension(): the latter uses first-dot semantics (so
            // `BoomBox.glb.meta` returns `.glb.meta`), which would let .meta sidecars leak back into the next
            // scan and grow `.meta.meta.meta...` chains across runs.
            out.push_back(entry);
        }
    }
}

// ── The incremental decision: do the recorded edges still hold? ────────────

[[nodiscard]] bool inputs_unchanged(const DbJob& rec, const fs::Path& root)
{
    for (crd::usize i = 0; i < rec.inputs.size(); ++i)
    {
        const DbInput& input = rec.inputs[i];
        const fs::Path abs   = root / crd::containers::StringView(input.path.data(), input.path.size());
        crd::containers::Array<crd::u8> bytes(&g_cook_alloc);
        const bool                      exists_now = fs::read_file_binary(abs, bytes);
        if (exists_now != input.existed) { return false; } // appearance/disappearance IS a change
        if (exists_now && cook_hash64(crd::containers::as_const_span(bytes)) != input.content_hash)
        {
            return false;
        }
    }
    return true;
}

// every product's cached artifact present AND hash-intact (a torn cache file must force a recook, not ship)
[[nodiscard]] bool products_intact(const DbJob& rec, const fs::Path& root)
{
    for (crd::usize p = 0; p < rec.products.size(); ++p)
    {
        crd::containers::Array<crd::u8> bytes(&g_cook_alloc);
        if (!fs::read_file_binary(cache_artifact_path(root, rec.products[p].id), bytes)) { return false; }
        if (cook_hash64(crd::containers::as_const_span(bytes)) != rec.products[p].artifact_hash) { return false; }
    }
    return true;
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

    CookDb db(&g_cook_alloc);
    db.load(root);

    // Recursive scan, sorted for determinism.
    crd::containers::Array<fs::Path> source_files(&g_cook_alloc);
    scan_recursive(root, source_files);
    std::sort(source_files.begin(), source_files.end(),
              [](const fs::Path& a, const fs::Path& b) { return a.generic() < b.generic(); });

    struct ArtifactInfo
    {
        ResourceId                      id;
        crd::containers::String         rel_path;
        crd::u32                        type_fourcc;
        crd::containers::Array<crd::u8> bytes;
    };

    crd::containers::Array<ArtifactInfo>            artifacts(&g_cook_alloc);
    crd::containers::Array<LogEntry>                log_entries(&g_cook_alloc);
    crd::containers::Array<crd::containers::String> live_sources(&g_cook_alloc);

    const crd::containers::StringView root_sv = root.generic();

    for (const fs::Path& src : source_files)
    {
        const crd::containers::StringView ext    = path_extension(src);
        const crd::containers::StringView rel_sv = root_relative(src.generic(), root_sv);

        // Handler lookup BEFORE touching the .meta sidecar: non-assets must not grow stale sidecars.
        const CookHandlerFn handler = find_cook_handler(ext);
        if (handler == nullptr)
        {
            LogEntry le;
            le.rel_path = crd::containers::String(rel_sv.data(), rel_sv.size(), &g_cook_alloc);
            le.uuid     = crd::containers::String(&g_cook_alloc);
            le.status   = "no_handler";
            log_entries.push_back(std::move(le));
            continue;
        }
        const crd::u32 handler_version = find_cook_handler_version(ext);

        live_sources.push_back(crd::containers::String(rel_sv.data(), rel_sv.size(), &g_cook_alloc));

        // Resolve the main-asset id (the processor owns identity; the .meta is ALSO a recorded input below).
        crd::containers::String meta_str(&g_cook_alloc);
        meta_str.append(src.generic().data(), src.generic().size());
        meta_str.append(".meta");
        const fs::Path meta_path(crd::containers::StringView(meta_str.data(), meta_str.size()));

        ResourceId asset_id;
        if (fs::is_file(meta_path))
        {
            if (!meta_read(meta_path, asset_id))
            {
                std::fprintf(stderr, "cook: malformed .meta for %s, regenerating\n", src.generic().data());
                asset_id = ResourceId::mint_random();
                (void)meta_write(meta_path, asset_id);
            }
        }
        else
        {
            asset_id = ResourceId::mint_random();
            if (!meta_write(meta_path, asset_id))
            {
                std::fprintf(stderr, "cook: failed to write .meta for %s\n", src.generic().data());
                return 1;
            }
        }

        // ── the incremental decision: skip WITHOUT running the handler ────────────────────────────────────────
        const DbJob* rec = db.find_job(rel_sv);
        if (rec != nullptr && !db.is_distrusted(rel_sv) && rec->handler_version == handler_version
            && rec->products.size() > 0U && inputs_unchanged(*rec, root) && products_intact(*rec, root))
        {
            for (crd::usize p = 0; p < rec->products.size(); ++p)
            {
                const DbProduct& prod = rec->products[p];
                ArtifactInfo     info{ResourceId{}, crd::containers::String(&g_cook_alloc), 0U,
                                      crd::containers::Array<crd::u8>(&g_cook_alloc)};
                info.id          = prod.id;
                info.rel_path    = crd::containers::String(prod.name.data(), prod.name.size(), &g_cook_alloc);
                info.type_fourcc = prod.type_fourcc;
                if (!fs::read_file_binary(cache_artifact_path(root, prod.id), info.bytes))
                {
                    std::fprintf(stderr, "cook: cache read failed for %s\n", prod.name.c_str());
                    return 1;
                }
                artifacts.push_back(std::move(info));

                LogEntry le;
                le.rel_path = crd::containers::String(prod.name.data(), prod.name.size(), &g_cook_alloc);
                le.uuid     = crd::containers::String(prod.id.to_string(&g_cook_alloc).c_str(), &g_cook_alloc);
                le.status   = "skipped";
                log_entries.push_back(std::move(le));
            }
            continue;
        }

        // ── cook: journal begin → run through CookIO → publish artifacts → record → commit ────────────────────
        if (!db.journal_begin(root, rel_sv))
        {
            std::fprintf(stderr, "cook: journal write failed\n");
            return 1;
        }

        CookIO io(src.generic(), crd::containers::StringView(meta_str.data(), meta_str.size()), &g_cook_alloc,
                  root_sv);
        {
            // force-record the source and .meta edges — their content governs the cook key even for handlers
            // that never look at them (the artifact embeds the .meta's uuid)
            crd::containers::Array<crd::u8> tmp_bytes(&g_cook_alloc);
            (void)io.read_source(tmp_bytes);
            crd::containers::String tmp_text(&g_cook_alloc);
            (void)io.read_meta(tmp_text);
        }

        CookContext ctx;
        ctx.source_path = src.generic();
        ctx.meta_path   = crd::containers::StringView(meta_str.data(), meta_str.size());
        ctx.id          = asset_id;
        ctx.allocator   = &g_cook_alloc;
        ctx.io          = &io;

        CookResult result = handler(ctx);
        if (!result.ok)
        {
            std::fprintf(stderr, "cook: handler failed for %s\n", src.generic().data());
            return 1; // the dangling `begin` distrusts this source next run
        }

        DbJob job(&g_cook_alloc);
        job.source          = crd::containers::String(rel_sv.data(), rel_sv.size(), &g_cook_alloc);
        job.handler_version = handler_version;
        for (crd::usize i = 0; i < io.inputs().size(); ++i)
        {
            const CookInput& edge = io.inputs()[i];
            DbInput          input(&g_cook_alloc);
            const crd::containers::StringView in_rel =
                root_relative(crd::containers::StringView(edge.path.data(), edge.path.size()), root_sv);
            input.path         = crd::containers::String(in_rel.data(), in_rel.size(), &g_cook_alloc);
            input.content_hash = edge.content_hash;
            input.existed      = edge.existed;
            job.inputs.push_back(std::move(input));
        }
        for (crd::usize d = 0; d < result.dependencies.size(); ++d)
        {
            job.runtime_deps.push_back(result.dependencies[d]);
        }

        // products: main first, then extras in handler order — the PACK preserves this order
        const auto publish = [&](const ResourceId& id, crd::containers::Array<crd::u8>&& bytes,
                                 crd::containers::StringView display_name) -> bool {
            if (!write_file_atomic(cache_artifact_path(root, id), crd::containers::as_const_span(bytes)))
            {
                std::fprintf(stderr, "cook: failed to write artifact cache for %.*s\n",
                             static_cast<int>(display_name.size()), display_name.data());
                return false;
            }
            crd::resources::CrdrFile crdr_file(&g_cook_alloc);
            if (crd::resources::crdr_read(crd::containers::as_const_span(bytes), crdr_file, &g_cook_alloc)
                != crd::resources::CrdrError::Ok)
            {
                std::fprintf(stderr, "cook: artifact parse failed for %.*s\n",
                             static_cast<int>(display_name.size()), display_name.data());
                return false;
            }

            DbProduct prod(&g_cook_alloc);
            prod.id            = id;
            prod.type_fourcc   = crdr_file.type_fourcc;
            prod.name          = crd::containers::String(display_name.data(), display_name.size(), &g_cook_alloc);
            prod.artifact_hash = cook_hash64(crd::containers::as_const_span(bytes));
            job.products.push_back(std::move(prod));

            ArtifactInfo info{ResourceId{}, crd::containers::String(&g_cook_alloc), 0U,
                              crd::containers::Array<crd::u8>(&g_cook_alloc)};
            info.id          = id;
            info.rel_path    = crd::containers::String(display_name.data(), display_name.size(), &g_cook_alloc);
            info.type_fourcc = crdr_file.type_fourcc;
            info.bytes       = std::move(bytes);
            artifacts.push_back(std::move(info));

            LogEntry le;
            le.rel_path = crd::containers::String(display_name.data(), display_name.size(), &g_cook_alloc);
            le.uuid     = crd::containers::String(id.to_string(&g_cook_alloc).c_str(), &g_cook_alloc);
            le.status   = "cooked";
            log_entries.push_back(std::move(le));
            return true;
        };

        if (!publish(asset_id, std::move(result.cooked_bytes), rel_sv)) { return 1; }
        for (crd::usize ei = 0; ei < result.extra_artifacts.size(); ++ei)
        {
            ExtraArtifact& extra = result.extra_artifacts[ei];
            if (!publish(extra.id, std::move(extra.cooked_bytes),
                         crd::containers::StringView(extra.name.data(), extra.name.size())))
            {
                return 1;
            }
        }

        db.upsert_job(std::move(job));
        if (!db.journal_commit(root, rel_sv))
        {
            std::fprintf(stderr, "cook: journal write failed\n");
            return 1;
        }
    }

    (void)db.prune_missing(live_sources);

    // ── Assemble PACK ──────────────────────────────────────────────────────
    //
    // pack_id derives from the root path — deterministic across runs (the byte-identical gate).
    const ResourceId pack_id = ResourceId::from_content(crd::containers::ConstSpan<crd::u8>(
        reinterpret_cast<const crd::u8*>(root_sv.data()), root_sv.size()));

    crd::containers::Array<crd::u8>  strp(&g_cook_alloc);
    crd::containers::Array<crd::u32> strp_offsets(&g_cook_alloc);
    strp_offsets.reserve(artifacts.size());
    for (const ArtifactInfo& info : artifacts)
    {
        strp_offsets.push_back(static_cast<crd::u32>(strp.size()));
        for (crd::usize j = 0U; j < info.rel_path.size(); ++j)
        {
            strp.push_back(static_cast<crd::u8>(info.rel_path.data()[j]));
        }
        strp.push_back(static_cast<crd::u8>('\0'));
    }

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

    // Pass 1: measure container size with dummy offsets; pass 2: final offsets.
    {
        crd::resources::CrdrWriter w1(&g_cook_alloc, pack_id, crd::resources::kFourCC_PACK);
        manifest_write(w1, crd::containers::as_const_span(entries), crd::containers::as_const_span(strp));
        const crd::containers::Array<crd::u8> crdr_pass1 = w1.finish();

        crd::usize blob_pos = crdr_pass1.size();
        for (crd::usize i = 0U; i < artifacts.size(); ++i)
        {
            entries[i].blob_offset = static_cast<crd::u64>(blob_pos);
            blob_pos += artifacts[i].bytes.size();
        }
    }

    crd::resources::CrdrWriter w2(&g_cook_alloc, pack_id, crd::resources::kFourCC_PACK);
    manifest_write(w2, crd::containers::as_const_span(entries), crd::containers::as_const_span(strp));
    crd::containers::Array<crd::u8> pack_bytes = w2.finish();

    for (const ArtifactInfo& info : artifacts)
    {
        for (crd::u8 byte : info.bytes) { pack_bytes.push_back(byte); }
    }

    if (!write_file_atomic(out_path, crd::containers::as_const_span(pack_bytes)))
    {
        std::fprintf(stderr, "cook: failed to write pack: %s\n", out_cstr);
        return 1;
    }

    // the run is complete: publish the database atomically (this also resets the journal)
    if (!db.save(root))
    {
        std::fprintf(stderr, "cook: failed to write cook database\n");
        return 1;
    }

    // Write cook.log.toml adjacent to out_path.
    const crd::containers::StringView out_sv     = out_path.generic();
    const auto                        out_slash  = out_sv.rfind('/');
    const crd::containers::StringView log_dir_sv = (out_slash != crd::containers::StringView::npos)
                                                       ? out_sv.substr(0U, out_slash + 1U)
                                                       : crd::containers::StringView("./");
    crd::containers::String log_path_str(&g_cook_alloc);
    log_path_str.append(log_dir_sv.data(), log_dir_sv.size());
    log_path_str.append("cook.log.toml");
    const fs::Path log_path(crd::containers::StringView(log_path_str.data(), log_path_str.size()));

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
