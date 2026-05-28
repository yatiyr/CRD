#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <crd/resources/resource_manager.hpp>

#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <new>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_handle.hpp>

#include <cstring>

using namespace crd::resources;

alignas(crd::memory::GrowableTlsfAllocator) static unsigned char s_alloc_buf[sizeof(crd::memory::GrowableTlsfAllocator)];
static crd::memory::GrowableTlsfAllocator& s_alloc = *::new (s_alloc_buf) crd::memory::GrowableTlsfAllocator(); // never destroyed: static-destruction-order safe

// Initialise / shut down the job system around the full test run.
// Using a listener avoids calling jobs::init() during catch_discover_tests' listing phase.
struct ResourcesJobsListener final : Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override
    {
        crd::jobs::init(crd::jobs::Config{.num_threads = 2});
    }

    void testRunEnded(Catch::TestRunStats const&) override
    {
        crd::jobs::shutdown();
    }
};
CATCH_REGISTER_LISTENER(ResourcesJobsListener)

// ── Helpers: write a minimal PACK to a temp file ───────────────────────────

static crd::platform::fs::Path write_test_pack(
    crd::containers::ConstSpan<ManifestEntry>    entries,
    crd::containers::ConstSpan<crd::u8>          string_pool)
{
    const ResourceId pack_id = ResourceId::mint_random();
    CrdrWriter writer(&s_alloc, pack_id, kFourCC_PACK);
    manifest_write(writer, entries, string_pool);
    const auto blob = writer.finish();

    // Write to a temp file in the current directory.
    const auto str_id = pack_id.to_string(&s_alloc);
    crd::containers::String tmp_name("test_pack_", &s_alloc);
    tmp_name.append(str_id);
    tmp_name.append(".crdr");

    const crd::platform::fs::Path path(tmp_name);
    const bool ok = crd::platform::fs::write_file_binary(path, crd::containers::as_const_span(blob));
    REQUIRE(ok);
    return path;
}

// ── Minimal no-op loader ──────────────────────────────────────────────────

struct BlobLoader final : public ILoader
{
    [[nodiscard]] crd::u32 type_fourcc()    const noexcept override { return kFourCC_BLOB; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }
    [[nodiscard]] void*    load(const LoadContext&) override { return nullptr; } // no-op in v1a
    void                   unload(void*) noexcept override {}
};

// ── Tests ─────────────────────────────────────────────────────────────────

TEST_CASE("ResourceManager registers a loader", "[resources][manager]")
{
    ResourceManager rm(&s_alloc);
    rm.register_loader(std::make_unique<BlobLoader>());
    CHECK(rm.loader_count() == 1U);
}

TEST_CASE("ResourceManager mount + unmount", "[resources][manager]")
{
    // Build a 3-entry pack.
    crd::containers::Array<crd::u8> string_pool(&s_alloc);
    crd::containers::Array<ManifestEntry> entries(&s_alloc);
    for (int i = 0; i < 3; ++i)
    {
        const crd::u32 name_offset = static_cast<crd::u32>(string_pool.size());
        const char* names[] = {"alpha", "bravo", "charlie"};
        for (const char* p = names[i]; *p; ++p)
        {
            string_pool.push_back(static_cast<crd::u8>(*p));
        }
        string_pool.push_back(0U);

        ManifestEntry e;
        e.id            = ResourceId::mint_random();
        e.type_fourcc   = kFourCC_BLOB;
        e.flags         = 0U;
        e.blob_offset   = 0U;
        e.blob_size     = 0U;
        e.name_strp_idx = name_offset;
        entries.push_back(e);
    }

    const crd::platform::fs::Path path =
        write_test_pack(crd::containers::as_const_span(entries),
                        crd::containers::as_const_span(string_pool));

    ResourceManager rm(&s_alloc);
    const MountId mid = rm.mount_manifest(path.generic());
    REQUIRE(mid.is_valid());
    CHECK(rm.mount_count() == 1U);
    CHECK(rm.entry_count() == 3U);

    // All three entries should be findable.
    for (const ManifestEntry& e : entries)
    {
        const MountEntry* found = rm.find_entry(e.id);
        REQUIRE(found != nullptr);
        CHECK(found->id == e.id);
        CHECK(found->type_fourcc == kFourCC_BLOB);
    }

    rm.unmount(mid);
    CHECK(rm.mount_count() == 0U);
    CHECK(rm.entry_count() == 0U);

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("ResourceManager mount missing file returns invalid MountId", "[resources][manager]")
{
    ResourceManager rm(&s_alloc);
    const MountId mid = rm.mount_manifest("nonexistent_file_xyz.crdr");
    CHECK_FALSE(mid.is_valid());
}

TEST_CASE("ResourceManager mount collision logs Warn and newest wins", "[resources][manager]")
{
    // Shared ResourceId in two different packs.
    const ResourceId shared_id = ResourceId::mint_random();

    auto make_single_entry_pack = [&](crd::containers::StringView name,
                                      crd::u32 flags) -> crd::platform::fs::Path
    {
        crd::containers::Array<crd::u8> pool(&s_alloc);
        for (char c : name)
        {
            pool.push_back(static_cast<crd::u8>(c));
        }
        pool.push_back(0U);

        crd::containers::Array<ManifestEntry> entries(&s_alloc);
        ManifestEntry e;
        e.id            = shared_id;
        e.type_fourcc   = kFourCC_BLOB;
        e.flags         = flags;
        e.blob_offset   = 0U;
        e.blob_size     = 0U;
        e.name_strp_idx = 0U;
        entries.push_back(e);
        return write_test_pack(crd::containers::as_const_span(entries),
                               crd::containers::as_const_span(pool));
    };

    const auto path_a = make_single_entry_pack("first",  0U);
    const auto path_b = make_single_entry_pack("second", 1U); // flags=1 distinguishes second pack

    ResourceManager rm(&s_alloc);
    const MountId mid_a = rm.mount_manifest(path_a.generic());
    REQUIRE(mid_a.is_valid());

    // Second mount causes a collision warning; newest wins.
    const MountId mid_b = rm.mount_manifest(path_b.generic());
    REQUIRE(mid_b.is_valid());

    CHECK(rm.entry_count() == 1U); // still one logical entry

    const MountEntry* found = rm.find_entry(shared_id);
    REQUIRE(found != nullptr);
    CHECK(found->flags == 1U); // second mount's value

    (void)crd::platform::fs::remove_file(path_a);
    (void)crd::platform::fs::remove_file(path_b);
}

TEST_CASE("ResourceManager unmount invalid id is a no-op", "[resources][manager]")
{
    ResourceManager rm(&s_alloc);
    rm.unmount(MountId{0U}); // should not crash
    rm.unmount(MountId{99U}); // non-existent; should not crash
    CHECK(rm.mount_count() == 0U);
}

TEST_CASE("ResourceManager find_entry returns nullptr for unknown id", "[resources][manager]")
{
    ResourceManager rm(&s_alloc);
    CHECK(rm.find_entry(ResourceId::mint_random()) == nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════
// v1c — load_sync helpers and loaders
// ═══════════════════════════════════════════════════════════════════════════

// Payload type for BlobResource loader.
struct BlobResource
{
    crd::containers::Array<crd::u8> bytes;
    explicit BlobResource(crd::memory::IAllocator* a) : bytes(a) {}
};

// Fully-functional loader: parses the CRDR artifact and copies the BLOB chunk.
struct BlobResourceLoader final : public ILoader
{
    // Concurrent async loads share one loader instance; wrapper serializes the heap.
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_alloc{&m_inner};

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

// Loader that always fails (hard failure — no placeholder).
struct HardFailLoader final : public ILoader
{
    crd::u32 m_fourcc;
    explicit HardFailLoader(crd::u32 fc) : m_fourcc(fc) {}

    [[nodiscard]] crd::u32 type_fourcc()    const noexcept override { return m_fourcc; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }
    [[nodiscard]] void*    load(const LoadContext&) override { return nullptr; }
    void                   unload(void*) noexcept override {}
};

// Loader that fails load() but provides a placeholder.
struct PlaceholderLoader final : public ILoader
{
    crd::u32 m_fourcc;
    explicit PlaceholderLoader(crd::u32 fc) : m_fourcc(fc) {}

    [[nodiscard]] crd::u32 type_fourcc()    const noexcept override { return m_fourcc; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }
    [[nodiscard]] void*    load(const LoadContext&) override { return nullptr; }

    [[nodiscard]] void* load_placeholder(const LoadContext&) override
    {
        static bool s_placeholder = true;
        return &s_placeholder;
    }

    void unload(void*) noexcept override {}
};

// Chained loader: reads a dep_id from the artifact's BLOB chunk (first 16 bytes)
// and calls load_sync<BlobResource> on the dep before returning its own payload.
struct ChainedPayload { bool ok = true; };

struct ChainedLoader final : public ILoader
{
    crd::u32 m_fourcc;
    // Concurrent async loads share one loader instance; wrapper serializes the heap.
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_alloc{&m_inner};

    explicit ChainedLoader(crd::u32 fc) : m_fourcc(fc) {}

    [[nodiscard]] crd::u32 type_fourcc()    const noexcept override { return m_fourcc; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }

    [[nodiscard]] void* load(const LoadContext& ctx) override
    {
        CrdrFile file(&m_alloc);
        if (crdr_read(ctx.bytes, file, &m_alloc) != CrdrError::Ok) { return nullptr; }
        const CrdrChunk* chunk = crdr_find_chunk(file, kFourCC_BLOB);
        if (!chunk || chunk->payload.size() < 16) { return nullptr; }

        // Read dep ResourceId from first 16 bytes of blob.
        ResourceId dep_id;
        std::memcpy(&dep_id.hi, chunk->payload.data(),     8);
        std::memcpy(&dep_id.lo, chunk->payload.data() + 8, 8);

        if (!dep_id.is_null())
        {
            // Load dependency; if it fails, we fail too.
            auto dep_handle = ctx.manager->load_sync<BlobResource>(dep_id);
            if (dep_handle.state() != LoadState::Ready)
            {
                return nullptr;
            }
        }

        void* raw = m_alloc.allocate(sizeof(ChainedPayload), alignof(ChainedPayload));
        return new (raw) ChainedPayload{true};
    }

    void unload(void* payload) noexcept override
    {
        if (payload) { m_alloc.deallocate(payload); }
    }
};

// ── Helper: write a PACK file with actual artifact bodies ──────────────────

struct TestArtifact
{
    ResourceId                      id;
    crd::u32                        type_fourcc;
    crd::containers::Array<crd::u8> crdr_bytes;
    crd::containers::String         name;

    explicit TestArtifact(crd::memory::IAllocator* a)
        : crdr_bytes(a), name(a)
    {
    }
};

// Build a blob artifact CRDR (type='BLOB' with one BLOB chunk).
static crd::containers::Array<crd::u8> make_blob_artifact(
    ResourceId id, crd::containers::ConstSpan<crd::u8> payload)
{
    CrdrWriter writer(&s_alloc, id, kFourCC_BLOB);
    writer.add_chunk(kFourCC_BLOB, payload);
    return writer.finish();
}

// Write a PACK file that contains real artifact bodies.
// Two-pass assembly mirrors what the cooker does:
//   Pass 1: measure CRDR section size with dummy blob_offsets.
//   Pass 2: compute real offsets, reassemble, append artifact bytes.
static crd::platform::fs::Path write_pack_with_artifacts(
    crd::containers::Array<TestArtifact>& arts)
{
    const ResourceId pack_id = ResourceId::mint_random();

    // Build string pool and initial entries with dummy offsets.
    crd::containers::Array<crd::u8>        string_pool(&s_alloc);
    crd::containers::Array<ManifestEntry>  entries(&s_alloc);
    for (const TestArtifact& art : arts)
    {
        const crd::u32 name_off = static_cast<crd::u32>(string_pool.size());
        for (crd::usize i = 0; i < art.name.size(); ++i)
        {
            string_pool.push_back(static_cast<crd::u8>(art.name.data()[i]));
        }
        string_pool.push_back(0U);

        ManifestEntry e;
        e.id             = art.id;
        e.type_fourcc    = art.type_fourcc;
        e.flags          = 0U;
        e.blob_offset    = 0U; // dummy — fixed in pass 2
        e.blob_size      = static_cast<crd::u64>(art.crdr_bytes.size());
        e.name_strp_idx  = name_off;
        entries.push_back(e);
    }

    // Pass 1: measure CRDR section size.
    {
        CrdrWriter pass1(&s_alloc, pack_id, kFourCC_PACK);
        manifest_write(pass1,
                       crd::containers::as_const_span(entries),
                       crd::containers::as_const_span(string_pool));
        const auto p1 = pass1.finish();
        const crd::u64 crdr_size = static_cast<crd::u64>(p1.size());

        // Fix blob_offsets.
        crd::u64 body_pos = crdr_size;
        for (crd::usize i = 0; i < arts.size(); ++i)
        {
            entries[i].blob_offset = body_pos;
            body_pos += arts[i].crdr_bytes.size();
        }
    }

    // Pass 2: reassemble with real offsets.
    CrdrWriter pass2(&s_alloc, pack_id, kFourCC_PACK);
    manifest_write(pass2,
                   crd::containers::as_const_span(entries),
                   crd::containers::as_const_span(string_pool));
    auto pack_bytes = pass2.finish();

    // Append artifact bytes.
    for (const TestArtifact& art : arts)
    {
        for (crd::usize i = 0; i < art.crdr_bytes.size(); ++i)
        {
            pack_bytes.push_back(art.crdr_bytes[i]);
        }
    }

    // Write to a temp file.
    const auto str_id = pack_id.to_string(&s_alloc);
    crd::containers::String tmp("test_pack_body_", &s_alloc);
    tmp.append(str_id);
    tmp.append(".crdr");
    const crd::platform::fs::Path path(tmp);
    const bool ok = crd::platform::fs::write_file_binary(
        path, crd::containers::as_const_span(pack_bytes));
    REQUIRE(ok);
    return path;
}

// ── v1c tests ──────────────────────────────────────────────────────────────

TEST_CASE("load_sync: BlobResource round-trip", "[resources][manager][v1c]")
{
    // Build a 5-byte blob artifact.
    const ResourceId blob_id = ResourceId::mint_random();
    const crd::u8 payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
    const auto payload_span = crd::containers::ConstSpan<crd::u8>(payload, 5);

    crd::containers::Array<TestArtifact> arts(&s_alloc);
    arts.push_back(TestArtifact(&s_alloc));
    arts[0].id          = blob_id;
    arts[0].type_fourcc = kFourCC_BLOB;
    arts[0].crdr_bytes  = make_blob_artifact(blob_id, payload_span);
    arts[0].name        = crd::containers::String("blob0", &s_alloc);

    const auto path = write_pack_with_artifacts(arts);

    ResourceManager rm(&s_alloc);
    rm.register_loader(std::make_unique<BlobResourceLoader>());
    const MountId mid = rm.mount_manifest(path.generic());
    REQUIRE(mid.is_valid());

    auto handle = rm.load_sync<BlobResource>(blob_id);
    CHECK(handle.state() == LoadState::Ready);
    CHECK(handle.is_ready());
    CHECK(handle.id() == blob_id);

    const BlobResource* res = handle.get();
    REQUIRE(res != nullptr);
    REQUIRE(res->bytes.size() == 5U);
    CHECK(res->bytes[0] == 0xDE);
    CHECK(res->bytes[4] == 0x42);

    CHECK(rm.handle_count() == 1U);

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("load_sync: 1000 copies refcount stays stable", "[resources][manager][v1c]")
{
    const ResourceId blob_id = ResourceId::mint_random();
    const crd::u8 byte_val = 0xAB;
    const auto span = crd::containers::ConstSpan<crd::u8>(&byte_val, 1);

    crd::containers::Array<TestArtifact> arts(&s_alloc);
    arts.push_back(TestArtifact(&s_alloc));
    arts[0].id          = blob_id;
    arts[0].type_fourcc = kFourCC_BLOB;
    arts[0].crdr_bytes  = make_blob_artifact(blob_id, span);
    arts[0].name        = crd::containers::String("blob_copies", &s_alloc);

    const auto path = write_pack_with_artifacts(arts);

    ResourceManager rm(&s_alloc);
    rm.register_loader(std::make_unique<BlobResourceLoader>());
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    {
        auto original = rm.load_sync<BlobResource>(blob_id);
        REQUIRE(original.is_ready());

        // Create 999 copies.
        crd::containers::Array<ResourceHandle<BlobResource>> copies(&s_alloc);
        for (int i = 0; i < 999; ++i)
        {
            copies.push_back(original);
        }
        CHECK(original.is_ready());

        // All copies dropped at end of scope.
    }

    // Block stays in handle table in v1c (no eviction).
    CHECK(rm.handle_count() == 1U);

    // Second load_sync returns the cached block.
    auto second = rm.load_sync<BlobResource>(blob_id);
    CHECK(second.is_ready());

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("load_sync: second call returns cached block", "[resources][manager][v1c]")
{
    const ResourceId blob_id = ResourceId::mint_random();
    const crd::u8 byte_val = 0x11;
    const auto span = crd::containers::ConstSpan<crd::u8>(&byte_val, 1);

    crd::containers::Array<TestArtifact> arts(&s_alloc);
    arts.push_back(TestArtifact(&s_alloc));
    arts[0].id          = blob_id;
    arts[0].type_fourcc = kFourCC_BLOB;
    arts[0].crdr_bytes  = make_blob_artifact(blob_id, span);
    arts[0].name        = crd::containers::String("blob_cache", &s_alloc);

    const auto path = write_pack_with_artifacts(arts);

    ResourceManager rm(&s_alloc);
    rm.register_loader(std::make_unique<BlobResourceLoader>());
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto h1 = rm.load_sync<BlobResource>(blob_id);
    auto h2 = rm.load_sync<BlobResource>(blob_id);

    REQUIRE(h1.is_ready());
    REQUIRE(h2.is_ready());
    // Both handles point to the same payload.
    CHECK(h1.get() == h2.get());
    CHECK(rm.handle_count() == 1U);

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("load_sync: unknown id returns Failed state", "[resources][manager][v1c]")
{
    ResourceManager rm(&s_alloc);
    auto handle = rm.load_sync<BlobResource>(ResourceId::mint_random());
    CHECK(handle.state() == LoadState::Failed);
    CHECK(handle.get() == nullptr);
}

TEST_CASE("load_sync: hard failure loader returns Failed state", "[resources][manager][v1c]")
{
    constexpr crd::u32 kTestFourCC = make_fourcc('T', 'S', 'T', 'F');

    const ResourceId test_id = ResourceId::mint_random();
    const crd::u8 dummy = 0;
    const auto span = crd::containers::ConstSpan<crd::u8>(&dummy, 1);

    crd::containers::Array<TestArtifact> arts(&s_alloc);
    arts.push_back(TestArtifact(&s_alloc));
    arts[0].id          = test_id;
    arts[0].type_fourcc = kTestFourCC;
    {
        CrdrWriter w(&s_alloc, test_id, kTestFourCC);
        w.add_chunk(kFourCC_BLOB, span);
        arts[0].crdr_bytes = w.finish();
    }
    arts[0].name = crd::containers::String("fail_test", &s_alloc);

    const auto path = write_pack_with_artifacts(arts);

    ResourceManager rm(&s_alloc);
    rm.register_loader(std::make_unique<HardFailLoader>(kTestFourCC));
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto handle = rm.load_sync<BlobResource>(test_id);
    CHECK(handle.state() == LoadState::Failed);
    CHECK(handle.get() == nullptr);
    CHECK(rm.handle_count() == 0U); // failed blocks not cached

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("load_sync: placeholder loader returns Placeholder state", "[resources][manager][v1c]")
{
    constexpr crd::u32 kTestFourCC = make_fourcc('T', 'S', 'T', 'P');

    const ResourceId test_id = ResourceId::mint_random();
    const crd::u8 dummy = 0;
    const auto span = crd::containers::ConstSpan<crd::u8>(&dummy, 1);

    crd::containers::Array<TestArtifact> arts(&s_alloc);
    arts.push_back(TestArtifact(&s_alloc));
    arts[0].id          = test_id;
    arts[0].type_fourcc = kTestFourCC;
    {
        CrdrWriter w(&s_alloc, test_id, kTestFourCC);
        w.add_chunk(kFourCC_BLOB, span);
        arts[0].crdr_bytes = w.finish();
    }
    arts[0].name = crd::containers::String("placeholder_test", &s_alloc);

    const auto path = write_pack_with_artifacts(arts);

    ResourceManager rm(&s_alloc);
    rm.register_loader(std::make_unique<PlaceholderLoader>(kTestFourCC));
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto handle = rm.load_sync<BlobResource>(test_id);
    CHECK(handle.state() == LoadState::Placeholder);
    CHECK(handle.get() != nullptr); // placeholder is non-null

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("load_sync: transitive dependency loads both resources", "[resources][manager][v1c]")
{
    constexpr crd::u32 kChainCC = make_fourcc('C', 'H', 'N', 'K');

    const ResourceId dep_id   = ResourceId::mint_random();
    const ResourceId owner_id = ResourceId::mint_random();

    // Dep artifact: a simple blob.
    const crd::u8 dep_byte = 0xDD;
    const auto dep_span = crd::containers::ConstSpan<crd::u8>(&dep_byte, 1);

    // Owner artifact: CRDR with type=CHNK; BLOB chunk contains dep_id (16 bytes).
    crd::containers::Array<crd::u8> owner_blob_payload(&s_alloc);
    owner_blob_payload.resize(16);
    std::memcpy(owner_blob_payload.data(),     &dep_id.hi, 8);
    std::memcpy(owner_blob_payload.data() + 8, &dep_id.lo, 8);

    crd::containers::Array<TestArtifact> arts(&s_alloc);

    arts.push_back(TestArtifact(&s_alloc));
    arts[0].id          = dep_id;
    arts[0].type_fourcc = kFourCC_BLOB;
    arts[0].crdr_bytes  = make_blob_artifact(dep_id, dep_span);
    arts[0].name        = crd::containers::String("dep", &s_alloc);

    arts.push_back(TestArtifact(&s_alloc));
    arts[1].id          = owner_id;
    arts[1].type_fourcc = kChainCC;
    {
        CrdrWriter w(&s_alloc, owner_id, kChainCC);
        w.add_chunk(kFourCC_BLOB, crd::containers::as_const_span(owner_blob_payload));
        arts[1].crdr_bytes = w.finish();
    }
    arts[1].name = crd::containers::String("owner", &s_alloc);

    const auto path = write_pack_with_artifacts(arts);

    ResourceManager rm(&s_alloc);
    rm.register_loader(std::make_unique<BlobResourceLoader>());
    rm.register_loader(std::make_unique<ChainedLoader>(kChainCC));
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto owner_handle = rm.load_sync<ChainedPayload>(owner_id);
    CHECK(owner_handle.state() == LoadState::Ready);

    // Dep should have been loaded transitively.
    auto dep_handle = rm.load_sync<BlobResource>(dep_id);
    CHECK(dep_handle.state() == LoadState::Ready);
    CHECK(rm.handle_count() == 2U);

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("load_sync: cycle detection makes both resources Failed", "[resources][manager][v1c]")
{
    constexpr crd::u32 kChainCC = make_fourcc('C', 'Y', 'C', 'L');

    const ResourceId id_a = ResourceId::mint_random();
    const ResourceId id_b = ResourceId::mint_random();

    auto make_chain_artifact = [&](ResourceId self_id, ResourceId dep_id) -> crd::containers::Array<crd::u8>
    {
        crd::containers::Array<crd::u8> blob(&s_alloc);
        blob.resize(16);
        std::memcpy(blob.data(),     &dep_id.hi, 8);
        std::memcpy(blob.data() + 8, &dep_id.lo, 8);

        CrdrWriter w(&s_alloc, self_id, kChainCC);
        w.add_chunk(kFourCC_BLOB, crd::containers::as_const_span(blob));
        return w.finish();
    };

    crd::containers::Array<TestArtifact> arts(&s_alloc);

    arts.push_back(TestArtifact(&s_alloc));
    arts[0].id          = id_a;
    arts[0].type_fourcc = kChainCC;
    arts[0].crdr_bytes  = make_chain_artifact(id_a, id_b); // A → B
    arts[0].name        = crd::containers::String("cycle_a", &s_alloc);

    arts.push_back(TestArtifact(&s_alloc));
    arts[1].id          = id_b;
    arts[1].type_fourcc = kChainCC;
    arts[1].crdr_bytes  = make_chain_artifact(id_b, id_a); // B → A (cycle!)
    arts[1].name        = crd::containers::String("cycle_b", &s_alloc);

    const auto path = write_pack_with_artifacts(arts);

    ResourceManager rm(&s_alloc);
    rm.register_loader(std::make_unique<ChainedLoader>(kChainCC));
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    // Loading A triggers B which tries to load A → cycle.
    auto handle_a = rm.load_sync<ChainedPayload>(id_a);
    CHECK(handle_a.state() == LoadState::Failed);
    CHECK(handle_a.get() == nullptr);

    // Neither resource should be permanently cached.
    CHECK(rm.handle_count() == 0U);

    (void)crd::platform::fs::remove_file(path);
}

// ═══════════════════════════════════════════════════════════════════════════
// v1d — load_async / wait_ready
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("load_async: BlobResource round-trip", "[resources][manager][v1d]")
{
    const ResourceId blob_id = ResourceId::mint_random();
    const crd::u8 payload[] = {0x0A, 0x0B, 0x0C, 0x0D};
    const auto payload_span  = crd::containers::ConstSpan<crd::u8>(payload, 4);

    crd::containers::Array<TestArtifact> arts(&s_alloc);
    arts.push_back(TestArtifact(&s_alloc));
    arts[0].id          = blob_id;
    arts[0].type_fourcc = kFourCC_BLOB;
    arts[0].crdr_bytes  = make_blob_artifact(blob_id, payload_span);
    arts[0].name        = crd::containers::String("async_blob", &s_alloc);
    const auto path = write_pack_with_artifacts(arts);

    ResourceManager rm(&s_alloc);
    rm.register_loader(std::make_unique<BlobResourceLoader>());
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto handle = rm.load_async<BlobResource>(blob_id);
    CHECK(handle.wait_ready() == LoadState::Ready);
    CHECK(handle.state() == LoadState::Ready);

    const BlobResource* res = handle.get();
    REQUIRE(res != nullptr);
    REQUIRE(res->bytes.size() == 4U);
    CHECK(res->bytes[0] == 0x0A);
    CHECK(res->bytes[3] == 0x0D);
    CHECK(rm.handle_count() == 1U);
    CHECK(rm.in_flight_count() == 0U);

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("load_async: coalesced requests share one block", "[resources][manager][v1d]")
{
    const ResourceId blob_id = ResourceId::mint_random();
    const crd::u8 payload[] = {0xFF, 0xEE};
    const auto payload_span  = crd::containers::ConstSpan<crd::u8>(payload, 2);

    crd::containers::Array<TestArtifact> arts(&s_alloc);
    arts.push_back(TestArtifact(&s_alloc));
    arts[0].id          = blob_id;
    arts[0].type_fourcc = kFourCC_BLOB;
    arts[0].crdr_bytes  = make_blob_artifact(blob_id, payload_span);
    arts[0].name        = crd::containers::String("coalesce_blob", &s_alloc);
    const auto path = write_pack_with_artifacts(arts);

    ResourceManager rm(&s_alloc);
    rm.register_loader(std::make_unique<BlobResourceLoader>());
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    // Submit two async loads for the same id before either completes.
    auto h1 = rm.load_async<BlobResource>(blob_id);
    auto h2 = rm.load_async<BlobResource>(blob_id);

    CHECK(h1.wait_ready() == LoadState::Ready);
    CHECK(h2.wait_ready() == LoadState::Ready);

    // Same underlying data implies the same block was reused.
    CHECK(h1.get() == h2.get());
    CHECK(rm.handle_count() == 1U);
    CHECK(rm.in_flight_count() == 0U);

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("load_async: unknown id returns Failed block immediately", "[resources][manager][v1d]")
{
    ResourceManager rm(&s_alloc);
    const ResourceId unknown = ResourceId::mint_random();

    auto handle = rm.load_async<BlobResource>(unknown);
    CHECK(handle.state() == LoadState::Failed);
    CHECK(handle.get() == nullptr);
    CHECK(rm.handle_count() == 0U);
}

TEST_CASE("load_async: four concurrent loads all reach Ready", "[resources][manager][v1d]")
{
    constexpr crd::usize kCount = 4U;
    const crd::u8 payload[] = {0xCA, 0xFE};
    const auto payload_span  = crd::containers::ConstSpan<crd::u8>(payload, 2);

    ResourceId ids[kCount];
    crd::containers::Array<TestArtifact> arts(&s_alloc);
    for (crd::usize i = 0; i < kCount; ++i)
    {
        ids[i] = ResourceId::mint_random();
        arts.push_back(TestArtifact(&s_alloc));
        arts[i].id          = ids[i];
        arts[i].type_fourcc = kFourCC_BLOB;
        arts[i].crdr_bytes  = make_blob_artifact(ids[i], payload_span);
        arts[i].name        = crd::containers::String("concurrent_blob", &s_alloc);
    }
    const auto path = write_pack_with_artifacts(arts);

    ResourceManager rm(&s_alloc);
    rm.register_loader(std::make_unique<BlobResourceLoader>());
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    // Submit all 4 async loads before waiting on any.
    ResourceHandle<BlobResource> handles[kCount];
    for (crd::usize i = 0; i < kCount; ++i)
    {
        handles[i] = rm.load_async<BlobResource>(ids[i]);
    }

    for (crd::usize i = 0; i < kCount; ++i)
    {
        CHECK(handles[i].wait_ready() == LoadState::Ready);
        CHECK(handles[i].get() != nullptr);
    }

    CHECK(rm.handle_count() == kCount);
    CHECK(rm.in_flight_count() == 0U);

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("load_async: wait_ready on sync-loaded handle returns Ready immediately", "[resources][manager][v1d]")
{
    const ResourceId blob_id = ResourceId::mint_random();
    const crd::u8 payload[] = {0x42};
    const auto payload_span  = crd::containers::ConstSpan<crd::u8>(payload, 1);

    crd::containers::Array<TestArtifact> arts(&s_alloc);
    arts.push_back(TestArtifact(&s_alloc));
    arts[0].id          = blob_id;
    arts[0].type_fourcc = kFourCC_BLOB;
    arts[0].crdr_bytes  = make_blob_artifact(blob_id, payload_span);
    arts[0].name        = crd::containers::String("sync_then_wait", &s_alloc);
    const auto path = write_pack_with_artifacts(arts);

    ResourceManager rm(&s_alloc);
    rm.register_loader(std::make_unique<BlobResourceLoader>());
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    // Synchronous load — wait_ready must return Ready without spinning.
    auto handle = rm.load_sync<BlobResource>(blob_id);
    REQUIRE(handle.state() == LoadState::Ready);

    const LoadState ws = handle.wait_ready();
    CHECK(ws == LoadState::Ready);
    CHECK(handle.get() != nullptr);

    (void)crd::platform::fs::remove_file(path);
}
