#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/cooker/cook_command.hpp>
#include <crd/cooker/cook_db.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>

#include <cstdio>
#include <cstring>

using namespace crd::resources;

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
static crd::memory::MallocAllocator g_alloc;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// ── manifest_dump sub-command ─────────────────────────────────────────────

static int cmd_manifest_dump(const char* pack_path)
{
    const crd::platform::fs::Path fs_path(pack_path);
    if (!crd::platform::fs::is_file(fs_path))
    {
        std::fprintf(stderr, "asset_cooker: file not found: %s\n", pack_path);
        return 1;
    }

    crd::containers::Array<crd::u8> file_bytes(&g_alloc);
    if (!crd::platform::fs::read_file_binary(fs_path, file_bytes))
    {
        std::fprintf(stderr, "asset_cooker: failed to read: %s\n", pack_path);
        return 1;
    }

    CrdrFile crdr_file(&g_alloc);
    const CrdrError err =
        crdr_read(crd::containers::as_const_span(file_bytes), crdr_file, &g_alloc);

    if (err != CrdrError::Ok)
    {
        const char* err_str = "unknown";
        switch (err)
        {
            case CrdrError::BadMagic:        err_str = "bad magic";       break;
            case CrdrError::BadVersion:      err_str = "bad version";     break;
            case CrdrError::Truncated:       err_str = "truncated";       break;
            case CrdrError::InvalidChunk:    err_str = "invalid chunk";   break;
            case CrdrError::DecompressFailed:err_str = "decompress failed"; break;
            case CrdrError::Ok:              break;
        }
        std::fprintf(stderr, "asset_cooker: CRDR parse error (%s): %s\n", err_str, pack_path);
        return 1;
    }

    if (crdr_file.type_fourcc != kFourCC_PACK)
    {
        char fourcc_str[5];
        fourcc_to_str(crdr_file.type_fourcc, fourcc_str);
        std::fprintf(stderr,
                     "asset_cooker: not a PACK container (type='%s'): %s\n",
                     fourcc_str, pack_path);
        return 1;
    }

    const auto pack_id_str = crdr_file.id.to_string(&g_alloc);
    std::printf("Pack: %s\n", pack_id_str.c_str());
    std::printf("File: %s\n", pack_path);

    const CrdrChunk* mfst_chunk = crdr_find_chunk(crdr_file, kFourCC_MFST);
    if (mfst_chunk == nullptr)
    {
        std::printf("Entries: 0 (no MFST chunk)\n");
        return 0;
    }

    crd::containers::ConstSpan<crd::u8> strp_data{};
    const CrdrChunk* strp_chunk = crdr_find_chunk(crdr_file, kFourCC_STRP);
    if (strp_chunk != nullptr)
    {
        strp_data = strp_chunk->payload;
    }

    crd::containers::Array<ManifestEntry> entries(&g_alloc);
    if (!manifest_read_entries(mfst_chunk->payload, entries, &g_alloc))
    {
        std::fprintf(stderr, "asset_cooker: malformed MFST chunk in: %s\n", pack_path);
        return 1;
    }

    std::printf("Entries: %zu\n", entries.size());
    for (crd::usize i = 0U; i < entries.size(); ++i)
    {
        const ManifestEntry& e = entries[i];
        const auto id_str = e.id.to_string(&g_alloc);

        char type_str[5];
        fourcc_to_str(e.type_fourcc, type_str);

        // Retrieve debug name from string pool.
        const char* name = "(unnamed)";
        crd::containers::String name_storage(&g_alloc);
        if (e.name_strp_idx < static_cast<crd::u32>(strp_data.size()))
        {
            const auto* begin = reinterpret_cast<const char*>(strp_data.data() + e.name_strp_idx);
            const auto* limit = reinterpret_cast<const char*>(strp_data.data() + strp_data.size());
            const char* end = begin;
            while (end < limit && *end != '\0')
            {
                ++end;
            }
            name_storage = crd::containers::String(
                begin, static_cast<crd::usize>(end - begin), &g_alloc);
            if (!name_storage.empty())
            {
                name = name_storage.c_str();
            }
        }

        std::printf("  [%3zu] uuid=%-36s  type=%-4s  offset=%10llu  size=%10llu  name=\"%s\"\n",
                    i,
                    id_str.c_str(),
                    type_str,
                    static_cast<unsigned long long>(e.blob_offset),
                    static_cast<unsigned long long>(e.blob_size),
                    name);
    }

    return 0;
}

// ── graph / why sub-commands (GEO-6: the queryable dependency graph) ──────

static int cmd_graph(const char* root_cstr)
{
    const crd::platform::fs::Path root(root_cstr);
    crd::cooker::CookDb           db(&g_alloc);
    db.load(root);

    if (db.jobs().size() == 0U)
    {
        std::printf("graph: no cook database under %s (run `cook` first)\n", root_cstr);
        return 1;
    }
    for (crd::usize j = 0; j < db.jobs().size(); ++j)
    {
        const crd::cooker::DbJob& job = db.jobs()[j];
        std::printf("job %s (handler v%u)\n", job.source.c_str(), job.handler_version);
        for (crd::usize i = 0; i < job.inputs.size(); ++i)
        {
            const crd::cooker::DbInput& in = job.inputs[i];
            std::printf("  <- %s %s(%016llx)\n", in.path.c_str(), in.existed ? "" : "[ABSENT] ",
                        static_cast<unsigned long long>(in.content_hash));
        }
        for (crd::usize p = 0; p < job.products.size(); ++p)
        {
            const crd::cooker::DbProduct& prod = job.products[p];
            char type_str[5];
            crd::resources::fourcc_to_str(prod.type_fourcc, type_str);
            const auto id_str = prod.id.to_string(&g_alloc);
            std::printf("  -> %s  type=%s  uuid=%s\n", prod.name.c_str(), type_str, id_str.c_str());
        }
        for (crd::usize d = 0; d < job.runtime_deps.size(); ++d)
        {
            const auto id_str = job.runtime_deps[d].to_string(&g_alloc);
            std::printf("  ~> runtime dep %s\n", id_str.c_str());
        }
    }
    return 0;
}

static int cmd_why(const char* root_cstr, const char* what)
{
    const crd::platform::fs::Path root(root_cstr);
    crd::cooker::CookDb           db(&g_alloc);
    db.load(root);

    // uuid? → the producing job
    const crd::resources::ResourceId id = crd::resources::ResourceId::parse(what);
    if (!id.is_null())
    {
        const crd::cooker::DbJob* producer = db.find_producer(id);
        if (producer == nullptr)
        {
            std::printf("why: no job produced %s\n", what);
            return 1;
        }
        std::printf("%s is produced by job %s\n", what, producer->source.c_str());
        return 0;
    }

    // otherwise: a root-relative path → every job consuming it (the reverse edge)
    crd::containers::Array<const crd::cooker::DbJob*> consumers(&g_alloc);
    db.jobs_consuming(crd::containers::StringView(what), consumers);
    if (consumers.size() == 0U)
    {
        std::printf("why: nothing consumes %s\n", what);
        return 1;
    }
    std::printf("%s is an input of %zu job(s):\n", what, consumers.size());
    for (crd::usize i = 0; i < consumers.size(); ++i)
    {
        std::printf("  %s\n", consumers[i]->source.c_str());
    }
    return 0;
}

// ── Entry point ────────────────────────────────────────────────────────────

static void print_usage(const char* argv0)
{
    std::printf(
        "Cerid asset cooker\n"
        "Usage:\n"
        "  %s manifest_dump <pack.crdr>                 Print all entries in a PACK file\n"
        "  %s cook --root <src_dir> --out <pack.crdr>   Cook source assets into a pack\n"
        "  %s graph --root <src_dir>                    Dump the dependency graph (source -> inputs -> products)\n"
        "  %s why --root <src_dir> <path|uuid>          Reverse query: who consumes a file / who produced a uuid\n",
        argv0, argv0, argv0, argv0);
}

int main(int argc, char* argv[])
{
    if (argc < 2)  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
    {
        print_usage(argv[0]);
        return 1;
    }

    const char* subcmd = argv[1];

    if (std::strcmp(subcmd, "manifest_dump") == 0)
    {
        if (argc < 3)  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
        {
            std::fprintf(stderr, "Usage: %s manifest_dump <pack.crdr>\n", argv[0]);
            return 1;
        }
        return cmd_manifest_dump(argv[2]);
    }

    if (std::strcmp(subcmd, "cook") == 0)
    {
        const char* root_arg = nullptr;
        const char* out_arg  = nullptr;
        for (int i = 2; i < argc - 1; ++i)  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
        {
            if (std::strcmp(argv[i], "--root") == 0)
            {
                root_arg = argv[i + 1];
            }
            else if (std::strcmp(argv[i], "--out") == 0)
            {
                out_arg = argv[i + 1];
            }
        }
        if (root_arg == nullptr || out_arg == nullptr)
        {
            std::fprintf(stderr,
                         "Usage: %s cook --root <src_dir> --out <pack.crdr>\n",
                         argv[0]);
            return 1;
        }
        crd::cooker::register_builtin_handlers();
        return crd::cooker::cmd_cook(root_arg, out_arg);
    }

    if (std::strcmp(subcmd, "graph") == 0)
    {
        const char* root_arg = nullptr;
        for (int i = 2; i < argc - 1; ++i)
        {
            if (std::strcmp(argv[i], "--root") == 0) { root_arg = argv[i + 1]; }
        }
        if (root_arg == nullptr)
        {
            std::fprintf(stderr, "Usage: %s graph --root <src_dir>\n", argv[0]);
            return 1;
        }
        return cmd_graph(root_arg);
    }

    if (std::strcmp(subcmd, "why") == 0)
    {
        const char* root_arg = nullptr;
        const char* what     = nullptr;
        for (int i = 2; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--root") == 0 && i + 1 < argc)
            {
                root_arg = argv[i + 1];
                ++i;
            }
            else { what = argv[i]; }
        }
        if (root_arg == nullptr || what == nullptr)
        {
            std::fprintf(stderr, "Usage: %s why --root <src_dir> <path|uuid>\n", argv[0]);
            return 1;
        }
        return cmd_why(root_arg, what);
    }

    std::fprintf(stderr, "asset_cooker: unknown sub-command '%s'\n", subcmd);
    print_usage(argv[0]);
    return 1;
}
