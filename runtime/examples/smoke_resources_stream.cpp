// smoke_resources_stream.cpp — end-to-end smoke for crd-resources v1g load_streamed.
//
// Writes a PACK file with a BLOB artifact, mounts it, calls load_streamed<>() which
// reads the artifact via crd::platform::AsyncFile inside a job fiber, then verifies
// that wait_ready() returns Ready with the correct payload value.
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

// ── BlobResource ─────────────────────────────────────────────────────────

struct StreamBlobResource
{
    crd::u32 value = 0;
};

struct StreamBlobLoader final : public ILoader
{
    crd::memory::MallocAllocator m_alloc;

    [[nodiscard]] crd::u32 type_fourcc()    const noexcept override { return kFourCC_BLOB; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }

    [[nodiscard]] void* load(const LoadContext& ctx) override
    {
        CrdrFile file(&m_alloc);
        if (crdr_read(ctx.bytes, file, &m_alloc) != CrdrError::Ok) { return nullptr; }
        const CrdrChunk* chunk = crdr_find_chunk(file, kFourCC_BLOB);
        if (chunk == nullptr || chunk->payload.size() < 4U) { return nullptr; }

        void* raw = m_alloc.allocate(sizeof(StreamBlobResource), alignof(StreamBlobResource));
        auto* res = new (raw) StreamBlobResource();
        std::memcpy(&res->value, chunk->payload.data(), 4U);
        return res;
    }

    void unload(void* payload) noexcept override
    {
        if (payload == nullptr) { return; }
        auto* res = static_cast<StreamBlobResource*>(payload);
        res->~StreamBlobResource();
        m_alloc.deallocate(res);
    }
};

// ── Pack assembly ─────────────────────────────────────────────────────────

static void write_blob_pack(const crd::platform::fs::Path& path,
                             ResourceId                      artifact_id,
                             crd::u32                        value)
{
    const crd::u8 blob[4] = {
        static_cast<crd::u8>(value & 0xFFU),
        static_cast<crd::u8>((value >> 8U) & 0xFFU),
        static_cast<crd::u8>((value >> 16U) & 0xFFU),
        static_cast<crd::u8>((value >> 24U) & 0xFFU)
    };

    CrdrWriter art(&g_alloc, artifact_id, kFourCC_BLOB);
    art.add_chunk(kFourCC_BLOB, crd::containers::ConstSpan<crd::u8>(blob, 4U));
    auto art_bytes = art.finish();

    const ResourceId pack_id = ResourceId::mint_random();

    crd::containers::Array<crd::u8> pool(&g_alloc);
    const char name[] = "stream_blob";
    for (const char c : name) { pool.push_back(static_cast<crd::u8>(c)); }
    pool.push_back(0U);

    crd::containers::Array<ManifestEntry> entries(&g_alloc);
    ManifestEntry e;
    e.id            = artifact_id;
    e.type_fourcc   = kFourCC_BLOB;
    e.flags         = 0U;
    e.blob_offset   = 0U;
    e.blob_size     = static_cast<crd::u64>(art_bytes.size());
    e.name_strp_idx = 0U;
    entries.push_back(e);

    {
        CrdrWriter p1(&g_alloc, pack_id, kFourCC_PACK);
        manifest_write(p1, crd::containers::as_const_span(entries),
                       crd::containers::as_const_span(pool));
        auto b1 = p1.finish();
        entries[0].blob_offset = static_cast<crd::u64>(b1.size());
    }

    CrdrWriter p2(&g_alloc, pack_id, kFourCC_PACK);
    manifest_write(p2, crd::containers::as_const_span(entries),
                   crd::containers::as_const_span(pool));
    auto pack_bytes = p2.finish();
    for (const crd::u8 b : art_bytes) { pack_bytes.push_back(b); }

    if (!crd::platform::fs::write_file_binary(path, crd::containers::as_const_span(pack_bytes)))
    {
        std::fprintf(stderr, "smoke_resources_stream: failed to write pack file\n");
        std::exit(1);
    }
}

// ── main ─────────────────────────────────────────────────────────────────

int main()
{
    // Initialise jobs — required for AsyncFile::read_async to submit work.
    crd::jobs::init();

    const ResourceId blob_id = ResourceId::mint_random();
    const auto       id_str  = blob_id.to_string(&g_alloc);
    crd::containers::String pack_name("smoke_stream_", &g_alloc);
    pack_name.append(id_str);
    pack_name.append(".crdr");
    const crd::platform::fs::Path pack_path(pack_name);

    constexpr crd::u32 kExpectedValue = 0xCAFEBABEU;
    write_blob_pack(pack_path, blob_id, kExpectedValue);

    ResourceManager rm(&g_alloc);
    rm.register_loader(std::make_unique<StreamBlobLoader>());

    const MountId mid = rm.mount_manifest(pack_path.generic());
    if (!mid.is_valid())
    {
        std::fprintf(stderr, "smoke_resources_stream: mount_manifest failed\n");
        crd::jobs::shutdown();
        (void)crd::platform::fs::remove_file(pack_path);
        return 1;
    }

    auto handle = rm.load_streamed<StreamBlobResource>(blob_id);

    const LoadState s = handle.wait_ready();
    if (s != LoadState::Ready)
    {
        std::fprintf(stderr, "smoke_resources_stream: load_streamed did not reach Ready (state=%d)\n",
                     static_cast<int>(s));
        crd::jobs::shutdown();
        (void)crd::platform::fs::remove_file(pack_path);
        return 1;
    }

    const StreamBlobResource* res = handle.get();
    if (res == nullptr || res->value != kExpectedValue)
    {
        std::fprintf(stderr, "smoke_resources_stream: payload mismatch (expected 0x%08X, got 0x%08X)\n",
                     kExpectedValue, res ? res->value : 0U);
        crd::jobs::shutdown();
        (void)crd::platform::fs::remove_file(pack_path);
        return 1;
    }

    std::printf("smoke_resources_stream: OK — payload=0x%08X, gen=%u\n",
                res->value, handle.generation());

    crd::jobs::shutdown();
    (void)crd::platform::fs::remove_file(pack_path);
    return 0;
}
