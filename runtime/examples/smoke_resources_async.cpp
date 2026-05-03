// smoke_resources_async.cpp — end-to-end smoke test for crd-resources v1d.
//
// Assembles a small PACK in memory, mounts it, loads a BlobResource
// asynchronously via load_async<T> + wait_ready(), and verifies the payload.
// Exits 0 on success.

#include <crd/containers/array.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace crd::resources;

static crd::memory::MallocAllocator g_alloc;

// ── Minimal BlobResource ──────────────────────────────────────────────────

struct BlobResource
{
    crd::containers::Array<crd::u8> bytes;
    explicit BlobResource(crd::memory::IAllocator* a) : bytes(a) {}
};

struct BlobResourceLoader final : public ILoader
{
    crd::memory::MallocAllocator m_alloc;

    [[nodiscard]] crd::u32 type_fourcc()    const noexcept override { return kFourCC_BLOB; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }

    [[nodiscard]] void* load(const LoadContext& ctx) override
    {
        CrdrFile file(&m_alloc);
        if (crdr_read(ctx.bytes, file, &m_alloc) != CrdrError::Ok) { return nullptr; }
        const CrdrChunk* chunk = crdr_find_chunk(file, kFourCC_BLOB);
        if (!chunk) { return nullptr; }

        void* raw = m_alloc.allocate(sizeof(BlobResource), alignof(BlobResource));
        auto* res = new (raw) BlobResource(&m_alloc);
        res->bytes.resize(chunk->payload.size());
        if (!chunk->payload.empty())
        {
            std::memcpy(res->bytes.data(), chunk->payload.data(), chunk->payload.size());
        }
        return res;
    }

    void unload(void* payload) noexcept override
    {
        if (!payload) { return; }
        auto* res = static_cast<BlobResource*>(payload);
        res->~BlobResource();
        m_alloc.deallocate(res);
    }
};

// ── Pack assembly helper (same two-pass pattern as smoke_resources.cpp) ───

static crd::platform::fs::Path assemble_pack(
    ResourceId artifact_id,
    crd::containers::ConstSpan<crd::u8> artifact_payload)
{
    CrdrWriter art_writer(&g_alloc, artifact_id, kFourCC_BLOB);
    art_writer.add_chunk(kFourCC_BLOB, artifact_payload);
    auto art_bytes = art_writer.finish();

    const ResourceId pack_id = ResourceId::mint_random();

    crd::containers::Array<crd::u8> pool(&g_alloc);
    const char kName[] = "smoke_async_blob";
    for (char c : kName) { pool.push_back(static_cast<crd::u8>(c)); }

    crd::containers::Array<ManifestEntry> entries(&g_alloc);
    ManifestEntry e;
    e.id            = artifact_id;
    e.type_fourcc   = kFourCC_BLOB;
    e.flags         = 0U;
    e.blob_offset   = 0U;
    e.blob_size     = static_cast<crd::u64>(art_bytes.size());
    e.name_strp_idx = 0U;
    entries.push_back(e);

    // Pass 1: measure CRDR section size.
    {
        CrdrWriter p1(&g_alloc, pack_id, kFourCC_PACK);
        manifest_write(p1,
                       crd::containers::as_const_span(entries),
                       crd::containers::as_const_span(pool));
        const auto b1 = p1.finish();
        entries[0].blob_offset = static_cast<crd::u64>(b1.size());
    }

    // Pass 2: assemble final pack with real offsets.
    CrdrWriter p2(&g_alloc, pack_id, kFourCC_PACK);
    manifest_write(p2,
                   crd::containers::as_const_span(entries),
                   crd::containers::as_const_span(pool));
    auto pack_bytes = p2.finish();
    for (crd::usize i = 0; i < art_bytes.size(); ++i)
    {
        pack_bytes.push_back(art_bytes[i]);
    }

    const auto str_id = pack_id.to_string(&g_alloc);
    crd::containers::String tmp("smoke_resources_async_", &g_alloc);
    tmp.append(str_id);
    tmp.append(".crdr");

    const crd::platform::fs::Path path(tmp);
    if (!crd::platform::fs::write_file_binary(path, crd::containers::as_const_span(pack_bytes)))
    {
        std::fprintf(stderr, "smoke_resources_async: failed to write pack file\n");
        std::exit(1);
    }
    return path;
}

// ── main ─────────────────────────────────────────────────────────────────

int main()
{
    crd::jobs::init(crd::jobs::Config{.num_threads = 2});

    const crd::u8 kExpected[] = {0x10, 0x20, 0x30, 0x40, 0x50};
    const auto expected_span = crd::containers::ConstSpan<crd::u8>(kExpected, 5);

    const ResourceId blob_id = ResourceId::mint_random();
    const auto pack_path = assemble_pack(blob_id, expected_span);

    ResourceManager rm(&g_alloc);
    rm.register_loader(std::make_unique<BlobResourceLoader>());

    const MountId mid = rm.mount_manifest(pack_path.generic());
    if (!mid.is_valid())
    {
        std::fprintf(stderr, "smoke_resources_async: mount_manifest failed\n");
        (void)crd::platform::fs::remove_file(pack_path);
        crd::jobs::shutdown();
        return 1;
    }

    // Async load.
    auto handle = rm.load_async<BlobResource>(blob_id);

    // Fiber-cooperative wait (or spin on main thread when not in a fiber).
    const LoadState ws = handle.wait_ready();
    if (ws != LoadState::Ready)
    {
        std::fprintf(stderr, "smoke_resources_async: wait_ready returned state=%d\n",
                     static_cast<int>(ws));
        (void)crd::platform::fs::remove_file(pack_path);
        crd::jobs::shutdown();
        return 1;
    }

    const BlobResource* res = handle.get();
    if (res == nullptr || res->bytes.size() != 5U)
    {
        std::fprintf(stderr, "smoke_resources_async: payload missing or wrong size\n");
        (void)crd::platform::fs::remove_file(pack_path);
        crd::jobs::shutdown();
        return 1;
    }

    for (crd::usize i = 0; i < 5U; ++i)
    {
        if (res->bytes[i] != kExpected[i])
        {
            std::fprintf(stderr, "smoke_resources_async: payload mismatch at byte %zu\n", i);
            (void)crd::platform::fs::remove_file(pack_path);
            crd::jobs::shutdown();
            return 1;
        }
    }

    // Drop the handle before shutting down (manager must drain in-flight before dtor).
    handle = ResourceHandle<BlobResource>{};

    (void)crd::platform::fs::remove_file(pack_path);

    crd::jobs::shutdown();
    std::printf("smoke_resources_async: OK — BlobResource loaded async and verified\n");
    return 0;
}
