#include <catch2/catch_test_macros.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>

using namespace crd::resources;

static crd::memory::MallocAllocator s_ev_alloc;

// ── EVBlobResource + loaders ───────────────────────────────────────────────

struct EVBlobResource
{
    crd::u32 value = 0;
};

struct EVBlobLoader final : public ILoader
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

        void* raw = m_alloc.allocate(sizeof(EVBlobResource), alignof(EVBlobResource));
        auto* res = new (raw) EVBlobResource();
        std::memcpy(&res->value, chunk->payload.data(), 4U);
        return res;
    }

    void unload(void* payload) noexcept override
    {
        if (payload == nullptr) { return; }
        auto* res = static_cast<EVBlobResource*>(payload);
        res->~EVBlobResource();
        m_alloc.deallocate(res);
    }
};

// ── Pack assembly helpers ──────────────────────────────────────────────────

// Write a single-resource BLOB pack to `path`.
static void ev_write_blob_pack(const crd::platform::fs::Path& path,
                                ResourceId                     artifact_id,
                                crd::u32                       value)
{
    const crd::u8 blob[4] = {
        static_cast<crd::u8>(value & 0xFFU),
        static_cast<crd::u8>((value >> 8U) & 0xFFU),
        static_cast<crd::u8>((value >> 16U) & 0xFFU),
        static_cast<crd::u8>((value >> 24U) & 0xFFU)
    };

    CrdrWriter art(&s_ev_alloc, artifact_id, kFourCC_BLOB);
    art.add_chunk(kFourCC_BLOB, crd::containers::ConstSpan<crd::u8>(blob, 4U));
    auto art_bytes = art.finish();

    const ResourceId pack_id = ResourceId::mint_random();

    crd::containers::Array<crd::u8> pool(&s_ev_alloc);
    const char name[] = "ev_blob";
    for (const char c : name) { pool.push_back(static_cast<crd::u8>(c)); }
    pool.push_back(0U);

    crd::containers::Array<ManifestEntry> entries(&s_ev_alloc);
    ManifestEntry e;
    e.id            = artifact_id;
    e.type_fourcc   = kFourCC_BLOB;
    e.flags         = 0U;
    e.blob_offset   = 0U;
    e.blob_size     = static_cast<crd::u64>(art_bytes.size());
    e.name_strp_idx = 0U;
    entries.push_back(e);

    // Pass 1: measure pack CRDR size → blob_offset.
    {
        CrdrWriter p1(&s_ev_alloc, pack_id, kFourCC_PACK);
        manifest_write(p1, crd::containers::as_const_span(entries),
                       crd::containers::as_const_span(pool));
        auto b1 = p1.finish();
        entries[0].blob_offset = static_cast<crd::u64>(b1.size());
    }

    // Pass 2: assemble final pack.
    CrdrWriter p2(&s_ev_alloc, pack_id, kFourCC_PACK);
    manifest_write(p2, crd::containers::as_const_span(entries),
                   crd::containers::as_const_span(pool));
    auto pack_bytes = p2.finish();
    for (crd::u8 b : art_bytes) { pack_bytes.push_back(b); }

    REQUIRE(crd::platform::fs::write_file_binary(path, crd::containers::as_const_span(pack_bytes)));
}

// ── Test utilities ─────────────────────────────────────────────────────────

struct EvPack
{
    ResourceId             id;
    crd::platform::fs::Path path;
    MountId                mid;
};

// Create a unique temp file path for a test pack.
static crd::containers::String ev_tmp_path(const char* prefix, const ResourceId& id)
{
    const auto id_str = id.to_string(&s_ev_alloc);
    crd::containers::String p(prefix, &s_ev_alloc);
    p.append(id_str);
    p.append(".crdr");
    return p;
}

// ── Tests ──────────────────────────────────────────────────────────────────

TEST_CASE("Eviction: budget enforced - excess resources evicted from A1in",
          "[resources][eviction][v1g]")
{
    // 4 resources, budget = 2 * blob_size.
    // Load all 4 sequentially (drop each handle before loading the next).
    // After all loads, memory_used must be <= budget.

    ResourceManager rm(&s_ev_alloc);
    rm.register_loader(std::make_unique<EVBlobLoader>());

    const crd::usize n_packs = 4;
    EvPack packs[n_packs];
    for (crd::usize i = 0; i < n_packs; ++i)
    {
        packs[i].id   = ResourceId::mint_random();
        packs[i].path = crd::platform::fs::Path(ev_tmp_path("ev_budget_", packs[i].id));
        ev_write_blob_pack(packs[i].path, packs[i].id, static_cast<crd::u32>(i + 1U));
        packs[i].mid = rm.mount_manifest(packs[i].path.generic());
        REQUIRE(packs[i].mid.is_valid());
    }

    const crd::u64 blob_sz = rm.find_entry(packs[0].id)->blob_size;
    rm.set_memory_budget(blob_sz * 2U);

    for (crd::usize i = 0; i < n_packs; ++i)
    {
        auto h = rm.load_sync<EVBlobResource>(packs[i].id);
        CHECK(h.state() == LoadState::Ready);
    } // each handle dropped at end of scope

    CHECK(rm.current_memory_use() <= blob_sz * 2U);

    for (crd::usize i = 0; i < n_packs; ++i)
    {
        (void)crd::platform::fs::remove_file(packs[i].path);
    }
}

TEST_CASE("Eviction: pinned entries survive eviction pressure",
          "[resources][eviction][v1g]")
{
    ResourceManager rm(&s_ev_alloc);
    rm.register_loader(std::make_unique<EVBlobLoader>());

    const crd::usize n_packs = 4;
    EvPack packs[n_packs];
    for (crd::usize i = 0; i < n_packs; ++i)
    {
        packs[i].id   = ResourceId::mint_random();
        packs[i].path = crd::platform::fs::Path(ev_tmp_path("ev_pin_", packs[i].id));
        ev_write_blob_pack(packs[i].path, packs[i].id, static_cast<crd::u32>(i + 10U));
        packs[i].mid = rm.mount_manifest(packs[i].path.generic());
        REQUIRE(packs[i].mid.is_valid());
    }

    const crd::u64 blob_sz = rm.find_entry(packs[0].id)->blob_size;

    // Pin resource 0 before loading. Budget = 2 blobs (room for 2, but resource 0 is pinned).
    rm.pin(packs[0].id);
    rm.set_memory_budget(blob_sz * 2U);

    // Load all 4, dropping each handle immediately.
    for (crd::usize i = 0; i < n_packs; ++i)
    {
        auto h = rm.load_sync<EVBlobResource>(packs[i].id);
        CHECK(h.state() == LoadState::Ready);
    }

    // Resource 0 must still be accessible (not evicted due to pin).
    {
        auto h0 = rm.load_sync<EVBlobResource>(packs[0].id);
        REQUIRE(h0.state() == LoadState::Ready);
        const EVBlobResource* res = h0.get();
        REQUIRE(res != nullptr);
        CHECK(res->value == 10U);
    }

    rm.unpin(packs[0].id);

    for (crd::usize i = 0; i < n_packs; ++i)
    {
        (void)crd::platform::fs::remove_file(packs[i].path);
    }
}

TEST_CASE("Eviction: re-issue after eviction increments generation",
          "[resources][eviction][v1g]")
{
    ResourceManager rm(&s_ev_alloc);
    rm.register_loader(std::make_unique<EVBlobLoader>());

    const ResourceId id   = ResourceId::mint_random();
    const auto       path = crd::platform::fs::Path(ev_tmp_path("ev_reissue_", id));
    ev_write_blob_pack(path, id, 42U);

    const MountId mid = rm.mount_manifest(path.generic());
    REQUIRE(mid.is_valid());

    // Load and immediately drop the handle (use_count → 0).
    {
        auto h = rm.load_sync<EVBlobResource>(id);
        REQUIRE(h.state() == LoadState::Ready);
        REQUIRE(h.generation() == 0U);
    }

    // Force eviction by setting an impossibly small budget.
    rm.set_memory_budget(0U);

    // Reload — block was evicted, so this is a re-issue (generation bumps).
    {
        auto h = rm.load_sync<EVBlobResource>(id);
        REQUIRE(h.state() == LoadState::Ready);
        CHECK(h.generation() == 1U);
        const EVBlobResource* res = h.get();
        REQUIRE(res != nullptr);
        CHECK(res->value == 42U);
    }

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("Eviction: 2Q - A1out ghost hit promotes resource to Am over A1in entries",
          "[resources][eviction][v1g]")
{
    // Uses 4 resources (A, B, C, D) and budget = 2 * blob_sz.
    // After A is evicted and re-loaded (A1out ghost hit → Am), loading D should
    // evict C (A1in) rather than A (Am), demonstrating that Am survives longer.

    ResourceManager rm(&s_ev_alloc);
    rm.register_loader(std::make_unique<EVBlobLoader>());

    const crd::usize n_packs = 4;
    EvPack packs[n_packs];
    for (crd::usize i = 0; i < n_packs; ++i)
    {
        packs[i].id   = ResourceId::mint_random();
        packs[i].path = crd::platform::fs::Path(ev_tmp_path("ev_2q_", packs[i].id));
        ev_write_blob_pack(packs[i].path, packs[i].id, static_cast<crd::u32>(i + 20U));
        packs[i].mid = rm.mount_manifest(packs[i].path.generic());
        REQUIRE(packs[i].mid.is_valid());
    }

    const crd::u64 blob_sz = rm.find_entry(packs[0].id)->blob_size;
    rm.set_memory_budget(blob_sz * 2U);

    // Load A (→ A1in). Drop handle.
    { auto h = rm.load_sync<EVBlobResource>(packs[0].id); REQUIRE(h.state() == LoadState::Ready); }
    // Load B (→ A1in). Drop handle.
    { auto h = rm.load_sync<EVBlobResource>(packs[1].id); REQUIRE(h.state() == LoadState::Ready); }
    // Load C (→ A1in, evicts A from A1in → A1out ghost). Drop handle.
    { auto h = rm.load_sync<EVBlobResource>(packs[2].id); REQUIRE(h.state() == LoadState::Ready); }

    // Re-load A: A1out ghost hit → A promoted to Am. B evicted from A1in to make room.
    // After this: A1in=[C], Am=[A].
    {
        auto h_a = rm.load_sync<EVBlobResource>(packs[0].id);
        REQUIRE(h_a.state() == LoadState::Ready);
        CHECK(h_a.generation() == 1U); // re-issued block bumps generation
    }

    // Load D (→ A1in, should evict C from A1in rather than A from Am).
    {
        auto h_d = rm.load_sync<EVBlobResource>(packs[3].id);
        REQUIRE(h_d.state() == LoadState::Ready);
    }

    // A must still be accessible (not evicted — it's in Am).
    {
        auto h_a2 = rm.load_sync<EVBlobResource>(packs[0].id);
        REQUIRE(h_a2.state() == LoadState::Ready);
        CHECK(h_a2.generation() == 1U); // same generation, not re-evicted
        const EVBlobResource* res = h_a2.get();
        REQUIRE(res != nullptr);
        CHECK(res->value == 20U);
    }

    for (crd::usize i = 0; i < n_packs; ++i)
    {
        (void)crd::platform::fs::remove_file(packs[i].path);
    }
}

TEST_CASE("Eviction: load_streamed delivers correct payload via AsyncFile",
          "[resources][eviction][v1g]")
{
    // crd::jobs is already initialised by the ResourcesJobsListener in test_resource_manager.cpp
    // (same test binary). Do not call jobs::init() here — it would double-init.

    const ResourceId id   = ResourceId::mint_random();
    const auto       path = crd::platform::fs::Path(ev_tmp_path("ev_stream_", id));
    ev_write_blob_pack(path, id, 0xDEADBEEFU);

    ResourceManager rm(&s_ev_alloc);
    rm.register_loader(std::make_unique<EVBlobLoader>());

    const MountId mid = rm.mount_manifest(path.generic());
    REQUIRE(mid.is_valid());

    auto handle = rm.load_streamed<EVBlobResource>(id);
    CHECK(handle.wait_ready() == LoadState::Ready);

    const EVBlobResource* res = handle.get();
    REQUIRE(res != nullptr);
    CHECK(res->value == 0xDEADBEEFU);
    CHECK(handle.generation() == 0U);

    (void)crd::platform::fs::remove_file(path);
}
