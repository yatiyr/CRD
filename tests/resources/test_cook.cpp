#include <crd/cooker/cook_command.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/platform/threading.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <string>

namespace fs = crd::platform::fs;
using namespace crd::resources;
using namespace crd::cooker;

namespace
{

crd::memory::MallocAllocator g_alloc; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

[[nodiscard]] fs::Path make_temp_dir()
{
    const auto base  = fs::current_working_dir() / ".crd_cook_test_tmp";
    const auto stamp = static_cast<crd::u64>(
        std::chrono::system_clock::now().time_since_epoch().count());
    crd::containers::String leaf("cook_");
    leaf.append(std::to_string(stamp));
    leaf.push_back('_');
    leaf.append(std::to_string(crd::platform::threading::current_thread_id()));
    return base / leaf.c_str();
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: Handler registry
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Cook registry: register and find handler", "[resources][cook][registry]")
{
    register_builtin_handlers();

    const CookHandlerFn bin_handler = find_cook_handler(".bin");
    REQUIRE(bin_handler != nullptr);

    const CookHandlerFn unknown = find_cook_handler(".xyz");
    REQUIRE(unknown == nullptr);

    // Calling register_builtin_handlers() again must be idempotent.
    register_builtin_handlers();
    REQUIRE(find_cook_handler(".bin") == bin_handler);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: .bin handler round-trip
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Cook .bin handler: round-trip", "[resources][cook][blob]")
{
    register_builtin_handlers();

    const auto tmp = make_temp_dir();
    REQUIRE(fs::create_directories(tmp));

    const auto src_path = tmp / "data.bin";
    const crd::u8 src_data[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };
    REQUIRE(fs::write_file_binary(src_path,
        crd::containers::ConstSpan<crd::u8>(src_data, sizeof(src_data))));

    const ResourceId id = ResourceId::mint_random();
    CookContext ctx;
    ctx.source_path = src_path.generic();
    ctx.meta_path   = {};
    ctx.id          = id;
    ctx.allocator   = &g_alloc;

    const CookHandlerFn handler = find_cook_handler(".bin");
    REQUIRE(handler != nullptr);

    CookResult result = handler(ctx);
    REQUIRE(result.ok);
    REQUIRE(result.type_fourcc == kFourCC_BLOB);
    REQUIRE(!result.cooked_bytes.empty());
    REQUIRE(result.handler_version == 1U);

    // Parse the cooked CRDR and verify it contains the original bytes.
    CrdrFile crdr_file(&g_alloc);
    const CrdrError err = crdr_read(
        crd::containers::as_const_span(result.cooked_bytes), crdr_file, &g_alloc);
    REQUIRE(err == CrdrError::Ok);
    REQUIRE(crdr_file.type_fourcc == kFourCC_BLOB);

    const CrdrChunk* blob = crdr_find_chunk(crdr_file, kFourCC_BLOB);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->uncompressed_size == sizeof(src_data));
    REQUIRE(blob->payload.size() == sizeof(src_data));
    for (crd::usize i = 0U; i < sizeof(src_data); ++i)
    {
        REQUIRE(blob->payload[i] == src_data[i]);
    }

    REQUIRE(fs::remove_all(tmp));
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: zstd compressed chunk round-trip
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("CrdrWriter: add_chunk_compressed round-trip", "[resources][cook][zstd]")
{
    const ResourceId id = ResourceId::mint_random();
    crd::containers::Array<crd::u8> payload(&g_alloc);
    // 128 bytes of repeated data: compressible.
    payload.resize(128U);
    for (crd::usize i = 0U; i < 128U; ++i)
    {
        payload[i] = static_cast<crd::u8>(i % 16U);
    }

    CrdrWriter writer(&g_alloc, id, kFourCC_BLOB);
    writer.add_chunk_compressed(
        kFourCC_BLOB, crd::containers::as_const_span(payload));
    const crd::containers::Array<crd::u8> bytes = writer.finish();

    CrdrFile out(&g_alloc);
    const CrdrError err = crdr_read(crd::containers::as_const_span(bytes), out, &g_alloc);
    REQUIRE(err == CrdrError::Ok);

    const CrdrChunk* chunk = crdr_find_chunk(out, kFourCC_BLOB);
    REQUIRE(chunk != nullptr);
    REQUIRE(chunk->uncompressed_size == 128U);
    REQUIRE(chunk->payload.size() == 128U);
    for (crd::usize i = 0U; i < 128U; ++i)
    {
        REQUIRE(chunk->payload[i] == static_cast<crd::u8>(i % 16U));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: Full cook integration
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("cmd_cook: 10 .bin files, byte-identical second run, skipped log entries",
          "[resources][cook][integration]")
{
    register_builtin_handlers();

    const auto tmp = make_temp_dir();
    REQUIRE(fs::create_directories(tmp));

    const auto pack_path = tmp / "output.crdr";

    // Create 10 .bin source files.
    constexpr int kFileCount = 10;
    for (int i = 0; i < kFileCount; ++i)
    {
        crd::containers::String name("file");
        name.append(std::to_string(i));
        name.append(".bin");
        const auto path = tmp / crd::containers::StringView(name.data(), name.size());
        const crd::u8 data[] = {
            static_cast<crd::u8>(i),
            static_cast<crd::u8>(i + 1),
            static_cast<crd::u8>(i + 2)
        };
        REQUIRE(fs::write_file_binary(
            path, crd::containers::ConstSpan<crd::u8>(data, sizeof(data))));
    }

    // First cook run.
    REQUIRE(cmd_cook(tmp.generic().data(), pack_path.generic().data()) == 0);

    // Read the pack file.
    crd::containers::Array<crd::u8> pack_bytes_1(&g_alloc);
    REQUIRE(fs::read_file_binary(pack_path, pack_bytes_1));

    // Parse and verify 10 entries.
    CrdrFile pack_1(&g_alloc);
    REQUIRE(crdr_read(crd::containers::as_const_span(pack_bytes_1), pack_1, &g_alloc) == CrdrError::Ok);
    REQUIRE(pack_1.type_fourcc == kFourCC_PACK);

    const CrdrChunk* mfst = crdr_find_chunk(pack_1, kFourCC_MFST);
    REQUIRE(mfst != nullptr);

    crd::containers::Array<ManifestEntry> entries_1(&g_alloc);
    REQUIRE(manifest_read_entries(mfst->payload, entries_1, &g_alloc));
    REQUIRE(entries_1.size() == static_cast<crd::usize>(kFileCount));

    // Second cook run.
    REQUIRE(cmd_cook(tmp.generic().data(), pack_path.generic().data()) == 0);

    // Read the pack file from the second run.
    crd::containers::Array<crd::u8> pack_bytes_2(&g_alloc);
    REQUIRE(fs::read_file_binary(pack_path, pack_bytes_2));

    // Byte-identical to first run.
    REQUIRE(pack_bytes_1.size() == pack_bytes_2.size());
    REQUIRE(std::memcmp(pack_bytes_1.data(), pack_bytes_2.data(), pack_bytes_1.size()) == 0);

    // Read cook.log.toml and verify "skipped" entries.
    const auto log_path = tmp / "cook.log.toml";
    crd::containers::String log_text(&g_alloc);
    REQUIRE(fs::read_file_text(log_path, log_text));

    // Count occurrences of status = "skipped".
    const std::string_view log_sv(log_text.data(), log_text.size());
    const std::string_view skipped_marker = "status = \"skipped\"";
    int skipped_count = 0;
    auto pos = log_sv.find(skipped_marker);
    while (pos != std::string_view::npos)
    {
        ++skipped_count;
        pos = log_sv.find(skipped_marker, pos + skipped_marker.size());
    }
    REQUIRE(skipped_count == kFileCount);

    REQUIRE(fs::remove_all(tmp));
}
