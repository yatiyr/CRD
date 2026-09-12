#include <catch2/catch_test_macros.hpp>
#include <crd/cooker/cook_command.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <new>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>
#include <string_view>

using namespace crd::resources;

alignas(crd::memory::GrowableTlsfAllocator) static unsigned char s_hr_alloc_buf[sizeof(crd::memory::GrowableTlsfAllocator)];
static crd::memory::GrowableTlsfAllocator& s_hr_alloc = *::new (s_hr_alloc_buf) crd::memory::GrowableTlsfAllocator(); // never destroyed: static-destruction-order safe

// ── BlobResource + loader ─────────────────────────────────────────────────

struct HRBlobResource
{
    crd::containers::Array<crd::u8> bytes;
    explicit HRBlobResource(crd::memory::IAllocator* a) : bytes(a) {}
};

struct HRBlobLoader final : public ILoader
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
        if (chunk == nullptr) { return nullptr; }

        void* raw = m_alloc.allocate(sizeof(HRBlobResource), alignof(HRBlobResource));
        auto* res = new (raw) HRBlobResource(&m_alloc);
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
        auto* res = static_cast<HRBlobResource*>(payload);
        res->~HRBlobResource();
        m_alloc.deallocate(res);
    }
};

// Loader that always returns null (for failed-reload test).
struct HRFailLoader final : public ILoader
{
    [[nodiscard]] crd::u32 type_fourcc()    const noexcept override { return kFourCC_BLOB; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }
    [[nodiscard]] void* load(const LoadContext&) override { return nullptr; }
    void unload(void*) noexcept override {}
};

// ── Pack assembly helper ──────────────────────────────────────────────────

// Write a PACK to `path` containing one BLOB artifact (blob_bytes).
// Reuses the same pack-container id each call so the manifest CRDR size is
// stable across writes — offsets won't shift when only the blob content changes.
static void write_blob_pack(const crd::platform::fs::Path& path,
                             ResourceId                     artifact_id,
                             crd::containers::ConstSpan<crd::u8> blob_bytes)
{
    // Build artifact CRDR.
    CrdrWriter art(&s_hr_alloc, artifact_id, kFourCC_BLOB);
    art.add_chunk(kFourCC_BLOB, blob_bytes);
    auto art_bytes = art.finish();

    const ResourceId pack_id = ResourceId::mint_random();

    crd::containers::Array<crd::u8> pool(&s_hr_alloc);
    const char name[] = "hr_blob";
    for (char c : name) { pool.push_back(static_cast<crd::u8>(c)); }
    pool.push_back(0U);

    crd::containers::Array<ManifestEntry> entries(&s_hr_alloc);
    ManifestEntry e;
    e.id            = artifact_id;
    e.type_fourcc   = kFourCC_BLOB;
    e.flags         = 0U;
    e.blob_offset   = 0U;
    e.blob_size     = static_cast<crd::u64>(art_bytes.size());
    e.name_strp_idx = 0U;
    entries.push_back(e);

    // Pass 1: measure manifest CRDR size to compute blob_offset.
    {
        CrdrWriter p1(&s_hr_alloc, pack_id, kFourCC_PACK);
        manifest_write(p1, crd::containers::as_const_span(entries),
                       crd::containers::as_const_span(pool));
        auto b1 = p1.finish();
        entries[0].blob_offset = static_cast<crd::u64>(b1.size());
    }

    // Pass 2: assemble final pack.
    CrdrWriter p2(&s_hr_alloc, pack_id, kFourCC_PACK);
    manifest_write(p2, crd::containers::as_const_span(entries),
                   crd::containers::as_const_span(pool));
    auto pack_bytes = p2.finish();
    for (crd::u8 b : art_bytes) { pack_bytes.push_back(b); }

    REQUIRE(crd::platform::fs::write_file_binary(path, crd::containers::as_const_span(pack_bytes)));
}

// ── Tests ─────────────────────────────────────────────────────────────────

TEST_CASE("Hot-reload: payload swap and generation bump", "[resources][hot_reload]")
{
    const ResourceId artifact_id = ResourceId::mint_random();
    const auto tmp_id            = artifact_id.to_string(&s_hr_alloc);
    crd::containers::String tmp_name("hr_test_", &s_hr_alloc);
    tmp_name.append(tmp_id);
    tmp_name.append(".crdr");
    const crd::platform::fs::Path path(tmp_name);

    const crd::u8 v1_content[] = {0x10, 0x20, 0x30};
    write_blob_pack(path, artifact_id, crd::containers::ConstSpan<crd::u8>(v1_content, 3));

    ResourceManager rm(&s_hr_alloc);
    rm.register_loader(std::make_unique<HRBlobLoader>());
    const MountId mid = rm.mount_manifest(path.generic());
    REQUIRE(mid.is_valid());

    auto handle = rm.load_sync<HRBlobResource>(artifact_id);
    REQUIRE(handle.state() == LoadState::Ready);

    {
        const HRBlobResource* res = handle.get();
        REQUIRE(res != nullptr);
        REQUIRE(res->bytes.size() == 3U);
        CHECK(res->bytes[0] == 0x10);
        CHECK(res->bytes[1] == 0x20);
        CHECK(res->bytes[2] == 0x30);
    }
    CHECK(handle.generation() == 0U);

    // Overwrite the pack with V2 content.
    const crd::u8 v2_content[] = {0xAA, 0xBB, 0xCC};
    write_blob_pack(path, artifact_id, crd::containers::ConstSpan<crd::u8>(v2_content, 3));

    const crd::usize n = rm.reload_mount_now(mid);
    CHECK(n == 1U);
    CHECK(handle.generation() == 1U);

    {
        const HRBlobResource* res = handle.get();
        REQUIRE(res != nullptr);
        REQUIRE(res->bytes.size() == 3U);
        CHECK(res->bytes[0] == 0xAA);
        CHECK(res->bytes[1] == 0xBB);
        CHECK(res->bytes[2] == 0xCC);
    }

    // Drain deferred frees before the handle goes out of scope.
    (void)rm.poll_hot_reload(0U);

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("Hot-reload: failed reload preserves last-good payload", "[resources][hot_reload]")
{
    const ResourceId artifact_id = ResourceId::mint_random();
    const auto tmp_id            = artifact_id.to_string(&s_hr_alloc);
    crd::containers::String tmp_name("hr_fail_", &s_hr_alloc);
    tmp_name.append(tmp_id);
    tmp_name.append(".crdr");
    const crd::platform::fs::Path path(tmp_name);

    const crd::u8 v1_content[] = {0x01, 0x02, 0x03};
    write_blob_pack(path, artifact_id, crd::containers::ConstSpan<crd::u8>(v1_content, 3));

    ResourceManager rm(&s_hr_alloc);
    rm.register_loader(std::make_unique<HRBlobLoader>());
    const MountId mid = rm.mount_manifest(path.generic());
    REQUIRE(mid.is_valid());

    auto handle = rm.load_sync<HRBlobResource>(artifact_id);
    REQUIRE(handle.state() == LoadState::Ready);

    // Write garbage so crdr_read fails (bad magic).
    {
        const crd::u8 garbage[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
        const crd::containers::ConstSpan<crd::u8> gspan(garbage, 8);
        REQUIRE(crd::platform::fs::write_file_binary(path, gspan));
    }

    const crd::usize n = rm.reload_mount_now(mid);
    CHECK(n == 0U); // no resources reloaded

    // Payload and state must be unchanged.
    CHECK(handle.state() == LoadState::Ready);
    CHECK(handle.generation() == 0U);
    {
        const HRBlobResource* res = handle.get();
        REQUIRE(res != nullptr);
        CHECK(res->bytes[0] == 0x01);
    }

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("Hot-reload: subscribe_reload callback fires", "[resources][hot_reload]")
{
    const ResourceId artifact_id = ResourceId::mint_random();
    const auto tmp_id            = artifact_id.to_string(&s_hr_alloc);
    crd::containers::String tmp_name("hr_sub_", &s_hr_alloc);
    tmp_name.append(tmp_id);
    tmp_name.append(".crdr");
    const crd::platform::fs::Path path(tmp_name);

    const crd::u8 v1_content[] = {0xAA};
    write_blob_pack(path, artifact_id, crd::containers::ConstSpan<crd::u8>(v1_content, 1));

    ResourceManager rm(&s_hr_alloc);
    rm.register_loader(std::make_unique<HRBlobLoader>());
    const MountId mid = rm.mount_manifest(path.generic());
    REQUIRE(mid.is_valid());

    auto handle = rm.load_sync<HRBlobResource>(artifact_id);
    REQUIRE(handle.state() == LoadState::Ready);

    struct CallbackState
    {
        bool     fired        = false;
        crd::u32 generation   = 0U;
        ResourceId id{};
    } cb_state;

    const crd::u32 token = rm.subscribe_reload(
        artifact_id,
        [](ResourceId id, crd::u32 gen, void* user)
        {
            auto* s       = static_cast<CallbackState*>(user);
            s->fired      = true;
            s->generation = gen;
            s->id         = id;
        },
        &cb_state);
    (void)token;

    const crd::u8 v2_content[] = {0xBB};
    write_blob_pack(path, artifact_id, crd::containers::ConstSpan<crd::u8>(v2_content, 1));

    (void)rm.reload_mount_now(mid);

    CHECK(cb_state.fired);
    CHECK(cb_state.generation == 1U);
    CHECK(cb_state.id == artifact_id);

    (void)rm.poll_hot_reload(0U); // drain deferred frees
    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("GEO-6: hot-reload rides the subscription seam -- touch source -> incremental recook -> subscriber "
          "sees the NEW bytes",
          "[resources][hot_reload][geo6]")
{
    // the full loop the editor lives on: cook a root with the REAL processor, mount its PACK, subscribe; touch ONE
    // source; the incremental recook rewrites only that product; the existing reload seam swaps the payload and
    // fires the subscriber — GEO-6 adds ZERO new runtime machinery, the cheap recook IS the contribution.
    namespace fs = crd::platform::fs;
    crd::cooker::register_builtin_handlers();

    const auto  run_id = ResourceId::mint_random().to_string(&s_hr_alloc);
    crd::containers::String root_name("hr_geo6_", &s_hr_alloc);
    root_name.append(run_id);
    const fs::Path root(crd::containers::StringView(root_name.data(), root_name.size()));
    REQUIRE(fs::create_directories(root));
    const fs::Path pack_path = root / "pack.crdr";

    const crd::u8 v1[] = {1, 2, 3};
    REQUIRE(fs::write_file_binary(root / "v.bin", crd::containers::ConstSpan<crd::u8>(v1, sizeof(v1))));
    REQUIRE(crd::cooker::cmd_cook(root.generic().data(), pack_path.generic().data()) == 0);

    // the artifact id the processor minted into the sidecar
    ResourceId blob_id;
    {
        crd::containers::String meta_text(&s_hr_alloc);
        REQUIRE(fs::read_file_text(root / "v.bin.meta", meta_text));
        const std::string_view sv(meta_text.data(), meta_text.size());
        const auto             q = sv.find("uuid = \"");
        REQUIRE(q != std::string_view::npos);
        blob_id = ResourceId::parse(sv.substr(q + 8U, 36U));
        REQUIRE(!blob_id.is_null());
    }

    ResourceManager rm(&s_hr_alloc);
    rm.register_loader(std::make_unique<HRBlobLoader>());
    const MountId mid = rm.mount_manifest(pack_path.generic());
    REQUIRE(mid.is_valid());

    auto handle = rm.load_sync<HRBlobResource>(blob_id);
    REQUIRE(handle.state() == LoadState::Ready);
    REQUIRE(handle.get()->bytes.size() == 3U);
    CHECK(handle.get()->bytes[0] == 1U);

    struct CallbackState
    {
        crd::u32 fired = 0U;
    } cb_state;
    (void)rm.subscribe_reload(
        blob_id,
        [](ResourceId /*id*/, crd::u32 /*gen*/, void* user) { ++static_cast<CallbackState*>(user)->fired; },
        &cb_state);

    // touch the SOURCE and recook incrementally (only v.bin's job runs — the GEO-6 precision path)
    const crd::u8 v2[] = {9, 9, 9, 9};
    REQUIRE(fs::write_file_binary(root / "v.bin", crd::containers::ConstSpan<crd::u8>(v2, sizeof(v2))));
    REQUIRE(crd::cooker::cmd_cook(root.generic().data(), pack_path.generic().data()) == 0);

    const crd::usize swapped = rm.reload_mount_now(mid);
    CHECK(swapped == 1U);
    CHECK(cb_state.fired == 1U);
    CHECK(handle.generation() == 1U);
    REQUIRE(handle.get()->bytes.size() == 4U); // the subscriber-visible payload IS the recooked artifact
    CHECK(handle.get()->bytes[0] == 9U);

    (void)rm.poll_hot_reload(0U); // drain deferred frees
    (void)fs::remove_all(root);
}

TEST_CASE("Hot-reload: unsubscribe prevents callback", "[resources][hot_reload]")
{
    const ResourceId artifact_id = ResourceId::mint_random();
    const auto tmp_id            = artifact_id.to_string(&s_hr_alloc);
    crd::containers::String tmp_name("hr_unsub_", &s_hr_alloc);
    tmp_name.append(tmp_id);
    tmp_name.append(".crdr");
    const crd::platform::fs::Path path(tmp_name);

    const crd::u8 v1_content[] = {0x11};
    write_blob_pack(path, artifact_id, crd::containers::ConstSpan<crd::u8>(v1_content, 1));

    ResourceManager rm(&s_hr_alloc);
    rm.register_loader(std::make_unique<HRBlobLoader>());
    const MountId mid = rm.mount_manifest(path.generic());
    REQUIRE(mid.is_valid());

    auto handle = rm.load_sync<HRBlobResource>(artifact_id);
    REQUIRE(handle.state() == LoadState::Ready);

    bool fired = false;
    const crd::u32 token = rm.subscribe_reload(
        artifact_id,
        [](ResourceId /*id*/, crd::u32 /*gen*/, void* user)
        {
            *static_cast<bool*>(user) = true;
        },
        &fired);

    rm.unsubscribe_reload(artifact_id, token);

    const crd::u8 v2_content[] = {0x22};
    write_blob_pack(path, artifact_id, crd::containers::ConstSpan<crd::u8>(v2_content, 1));

    (void)rm.reload_mount_now(mid);

    CHECK_FALSE(fired);
    CHECK(handle.generation() == 1U); // reload still happened, just no callback

    (void)rm.poll_hot_reload(0U); // drain deferred frees
    (void)crd::platform::fs::remove_file(path);
}
