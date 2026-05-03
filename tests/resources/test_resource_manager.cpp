#include <catch2/catch_test_macros.hpp>
#include <crd/resources/resource_manager.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>

#include <cstring>

using namespace crd::resources;

static crd::memory::MallocAllocator s_alloc;

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
