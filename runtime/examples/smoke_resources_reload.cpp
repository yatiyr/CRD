// smoke_resources_reload.cpp — end-to-end smoke for crd-resources v1f hot-reload.
//
// Writes a PACK file with a BLOB artifact (V1), mounts it, loads it, verifies
// the payload, then overwrites the PACK with V2 content, calls poll_hot_reload(0)
// to trigger an immediate reload (debounce=0), and verifies the swapped payload.
// Exits 0 on success.

#include <crd/containers/array.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>

using namespace crd::resources;

static crd::memory::TlsfAllocator g_alloc{256ULL << 20};

// ── BlobResource ─────────────────────────────────────────────────────────

struct BlobResource
{
    crd::containers::Array<crd::u8> bytes;
    explicit BlobResource(crd::memory::IAllocator* a) : bytes(a) {}
};

struct BlobResourceLoader final : public ILoader
{
    crd::memory::TlsfAllocator m_alloc{256ULL << 20};

    [[nodiscard]] crd::u32 type_fourcc()    const noexcept override { return kFourCC_BLOB; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }

    [[nodiscard]] void* load(const LoadContext& ctx) override
    {
        CrdrFile file(&m_alloc);
        if (crdr_read(ctx.bytes, file, &m_alloc) != CrdrError::Ok) { return nullptr; }
        const CrdrChunk* chunk = crdr_find_chunk(file, kFourCC_BLOB);
        if (chunk == nullptr) { return nullptr; }

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
        if (payload == nullptr) { return; }
        auto* res = static_cast<BlobResource*>(payload);
        res->~BlobResource();
        m_alloc.deallocate(res);
    }
};

// ── Pack assembly ─────────────────────────────────────────────────────────

static void write_blob_pack(const crd::platform::fs::Path& path,
                             ResourceId                      artifact_id,
                             crd::containers::ConstSpan<crd::u8> blob_bytes)
{
    CrdrWriter art(&g_alloc, artifact_id, kFourCC_BLOB);
    art.add_chunk(kFourCC_BLOB, blob_bytes);
    auto art_bytes = art.finish();

    const ResourceId pack_id = ResourceId::mint_random();

    crd::containers::Array<crd::u8> pool(&g_alloc);
    const char name[] = "smoke_blob";
    for (char c : name) { pool.push_back(static_cast<crd::u8>(c)); }
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
        manifest_write(p1, crd::containers::as_const_span(entries), crd::containers::as_const_span(pool));
        auto b1 = p1.finish();
        entries[0].blob_offset = static_cast<crd::u64>(b1.size());
    }

    CrdrWriter p2(&g_alloc, pack_id, kFourCC_PACK);
    manifest_write(p2, crd::containers::as_const_span(entries), crd::containers::as_const_span(pool));
    auto pack_bytes = p2.finish();
    for (crd::u8 b : art_bytes) { pack_bytes.push_back(b); }

    if (!crd::platform::fs::write_file_binary(path, crd::containers::as_const_span(pack_bytes)))
    {
        std::fprintf(stderr, "smoke_resources_reload: failed to write pack\n");
        std::exit(1);
    }
}

// ── main ─────────────────────────────────────────────────────────────────

int main()
{
    // Write V1 pack.
    const ResourceId blob_id = ResourceId::mint_random();
    const auto id_str        = blob_id.to_string(&g_alloc);
    crd::containers::String pack_name("smoke_reload_", &g_alloc);
    pack_name.append(id_str);
    pack_name.append(".crdr");
    const crd::platform::fs::Path pack_path(pack_name);

    const crd::u8 v1_content[] = {0x10, 0x20, 0x30};
    write_blob_pack(pack_path, blob_id, crd::containers::ConstSpan<crd::u8>(v1_content, 3));

    ResourceManager rm(&g_alloc);
    rm.register_loader(std::make_unique<BlobResourceLoader>());

    const MountId mid = rm.mount_manifest(pack_path.generic());
    if (!mid.is_valid())
    {
        std::fprintf(stderr, "smoke_resources_reload: mount_manifest failed\n");
        (void)crd::platform::fs::remove_file(pack_path);
        return 1;
    }

    auto handle = rm.load_sync<BlobResource>(blob_id);
    if (handle.state() != LoadState::Ready)
    {
        std::fprintf(stderr, "smoke_resources_reload: initial load failed\n");
        (void)crd::platform::fs::remove_file(pack_path);
        return 1;
    }

    {
        const BlobResource* r = handle.get();
        if (r == nullptr || r->bytes.size() != 3 ||
            r->bytes[0] != 0x10 || r->bytes[1] != 0x20 || r->bytes[2] != 0x30)
        {
            std::fprintf(stderr, "smoke_resources_reload: V1 payload mismatch\n");
            (void)crd::platform::fs::remove_file(pack_path);
            return 1;
        }
    }
    std::printf("smoke_resources_reload: V1 verified\n");

    // Subscribe reload callback.
    struct ReloadResult { bool fired = false; crd::u32 gen = 0U; };
    ReloadResult reload_result;
    (void)rm.subscribe_reload(
        blob_id,
        [](ResourceId /*id*/, crd::u32 gen, void* user)
        {
            auto* r = static_cast<ReloadResult*>(user);
            r->fired = true;
            r->gen   = gen;
        },
        &reload_result);

    // Sleep so the OS reports a different mtime on the rewritten file.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // Overwrite with V2.
    const crd::u8 v2_content[] = {0xAA, 0xBB, 0xCC};
    write_blob_pack(pack_path, blob_id, crd::containers::ConstSpan<crd::u8>(v2_content, 3));

    // poll_hot_reload with debounce=0 — must detect the mtime change and reload.
    const crd::usize n = rm.poll_hot_reload(0U);
    if (n != 1U)
    {
        std::fprintf(stderr, "smoke_resources_reload: expected 1 reload, got %zu\n", n);
        (void)crd::platform::fs::remove_file(pack_path);
        return 1;
    }

    if (handle.generation() != 1U)
    {
        std::fprintf(stderr, "smoke_resources_reload: expected generation 1, got %u\n",
                     handle.generation());
        (void)crd::platform::fs::remove_file(pack_path);
        return 1;
    }

    {
        const BlobResource* r = handle.get();
        if (r == nullptr || r->bytes.size() != 3 ||
            r->bytes[0] != 0xAA || r->bytes[1] != 0xBB || r->bytes[2] != 0xCC)
        {
            std::fprintf(stderr, "smoke_resources_reload: V2 payload mismatch\n");
            (void)crd::platform::fs::remove_file(pack_path);
            return 1;
        }
    }
    std::printf("smoke_resources_reload: V2 verified (hot-reload OK)\n");

    if (!reload_result.fired || reload_result.gen != 1U)
    {
        std::fprintf(stderr, "smoke_resources_reload: callback not fired or wrong gen\n");
        (void)crd::platform::fs::remove_file(pack_path);
        return 1;
    }
    std::printf("smoke_resources_reload: callback fired with generation=%u\n", reload_result.gen);

    // Drain deferred frees.
    (void)rm.poll_hot_reload(0U);

    (void)crd::platform::fs::remove_file(pack_path);
    std::printf("smoke_resources_reload: OK\n");
    return 0;
}
