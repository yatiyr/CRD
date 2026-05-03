// smoke_resources.cpp — end-to-end smoke test for crd-resources v1c.
//
// Cooks a small blob artifact in-memory, writes a PACK file, mounts it,
// loads it synchronously, and verifies the payload bytes. Exits 0 on success.

#include <crd/containers/array.hpp>
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

// ── Pack assembly helper ──────────────────────────────────────────────────

static crd::platform::fs::Path assemble_pack(
    ResourceId artifact_id,
    crd::containers::ConstSpan<crd::u8> artifact_payload)
{
    // Build artifact CRDR.
    CrdrWriter art_writer(&g_alloc, artifact_id, kFourCC_BLOB);
    art_writer.add_chunk(kFourCC_BLOB, artifact_payload);
    auto art_bytes = art_writer.finish();

    const ResourceId pack_id = ResourceId::mint_random();

    // Build string pool + entry with dummy offset.
    crd::containers::Array<crd::u8>        pool(&g_alloc);
    const char kName[] = "smoke_blob";
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

    // Pass 2: assemble final pack.
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
    crd::containers::String tmp("smoke_resources_", &g_alloc);
    tmp.append(str_id);
    tmp.append(".crdr");

    const crd::platform::fs::Path path(tmp);
    const bool ok = crd::platform::fs::write_file_binary(
        path, crd::containers::as_const_span(pack_bytes));
    if (!ok)
    {
        std::fprintf(stderr, "smoke_resources: failed to write pack file\n");
        std::exit(1);
    }
    return path;
}

// ── main ─────────────────────────────────────────────────────────────────

int main()
{
    const crd::u8 kExpected[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    const auto expected_span = crd::containers::ConstSpan<crd::u8>(kExpected, 5);

    const ResourceId blob_id = ResourceId::mint_random();
    const auto pack_path = assemble_pack(blob_id, expected_span);

    ResourceManager rm(&g_alloc);
    rm.register_loader(std::make_unique<BlobResourceLoader>());

    const MountId mid = rm.mount_manifest(pack_path.generic());
    if (!mid.is_valid())
    {
        std::fprintf(stderr, "smoke_resources: mount_manifest failed\n");
        (void)crd::platform::fs::remove_file(pack_path);
        return 1;
    }

    auto handle = rm.load_sync<BlobResource>(blob_id);
    if (handle.state() != LoadState::Ready)
    {
        std::fprintf(stderr, "smoke_resources: load_sync failed (state=%d)\n",
                     static_cast<int>(handle.state()));
        (void)crd::platform::fs::remove_file(pack_path);
        return 1;
    }

    const BlobResource* res = handle.get();
    if (res == nullptr || res->bytes.size() != 5)
    {
        std::fprintf(stderr, "smoke_resources: payload missing or wrong size\n");
        (void)crd::platform::fs::remove_file(pack_path);
        return 1;
    }

    for (crd::usize i = 0; i < 5; ++i)
    {
        if (res->bytes[i] != kExpected[i])
        {
            std::fprintf(stderr, "smoke_resources: payload mismatch at byte %zu\n", i);
            (void)crd::platform::fs::remove_file(pack_path);
            return 1;
        }
    }

    (void)crd::platform::fs::remove_file(pack_path);
    std::printf("smoke_resources: OK — BlobResource loaded and verified\n");
    return 0;
}
