#include <crd/cooker/cook_command.hpp>
#include <crd/cooker/cook_db.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/cook_io.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <new>
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

alignas(crd::memory::GrowableTlsfAllocator) unsigned char g_alloc_buf[sizeof(crd::memory::GrowableTlsfAllocator)];
crd::memory::GrowableTlsfAllocator& g_alloc = *::new (g_alloc_buf) crd::memory::GrowableTlsfAllocator(); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

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
    CookIO ctx_io(ctx.source_path, ctx.meta_path, &g_alloc); // GEO-6: the only road to bytes
    ctx.io          = &ctx_io;

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
    constexpr int file_count = 10;
    for (int i = 0; i < file_count; ++i)
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
    REQUIRE(entries_1.size() == static_cast<crd::usize>(file_count));

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
    REQUIRE(skipped_count == file_count);

    REQUIRE(fs::remove_all(tmp));
}

// ─────────────────────────────────────────────────────────────────────────────
// GEO-6 (D-007 row 71): the dependency-graph asset processor gates
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

// true when the cook.log.toml entry for `rel` carries exactly `expected` as its status
[[nodiscard]] bool log_status_is(const fs::Path& tmp, const char* rel, const char* expected)
{
    crd::containers::String log_text(&g_alloc);
    if (!fs::read_file_text(tmp / "cook.log.toml", log_text)) { return false; }
    const std::string_view sv(log_text.data(), log_text.size());
    crd::containers::String needle(&g_alloc);
    needle.append("path = \"");
    needle.append(rel);
    needle.push_back('"');
    const auto p = sv.find(std::string_view(needle.data(), needle.size()));
    if (p == std::string_view::npos) { return false; }
    const std::string_view key = "status = \"";
    const auto             s   = sv.find(key, p);
    if (s == std::string_view::npos) { return false; }
    const auto b = s + key.size();
    const auto e = sv.find('"', b);
    return sv.substr(b, e - b) == std::string_view(expected);
}

void write_bytes(const fs::Path& p, const crd::u8* data, crd::usize n)
{
    REQUIRE(fs::write_file_binary(p, crd::containers::ConstSpan<crd::u8>(data, n)));
}

// a minimal .gltf whose ONLY buffer is the external "geo.bin" (36 bytes = 3 raw float3 positions, non-indexed)
constexpr const char* kExtGltf =
    "{\"asset\":{\"version\":\"2.0\"},"
    "\"buffers\":[{\"byteLength\":36,\"uri\":\"geo.bin\"}],"
    "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
    "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
    "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}]}";

void write_ext_gltf(const fs::Path& tmp)
{
    REQUIRE(fs::write_file_text(tmp / "tri.gltf", crd::containers::StringView(kExtGltf, std::strlen(kExtGltf))));
    const crd::f32 pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    write_bytes(tmp / "geo.bin", reinterpret_cast<const crd::u8*>(pos), sizeof(pos));
}

} // namespace

TEST_CASE("GEO-6: precision recook -- touch ONE file, exactly its dependents recook (handlers NOT run on skips)",
          "[resources][cook][geo6]")
{
    register_builtin_handlers();
    const auto tmp = make_temp_dir();
    REQUIRE(fs::create_directories(tmp));
    const auto pack_path = tmp / "out.crdr";

    const crd::u8 a1[] = {1, 2, 3};
    const crd::u8 b1[] = {4, 5, 6};
    write_bytes(tmp / "a.bin", a1, sizeof(a1));
    write_bytes(tmp / "b.bin", b1, sizeof(b1));
    write_ext_gltf(tmp);

    // run 1: everything cooks
    REQUIRE(cmd_cook(tmp.generic().data(), pack_path.generic().data()) == 0);
    CHECK(log_status_is(tmp, "a.bin", "cooked"));
    CHECK(log_status_is(tmp, "tri.gltf", "cooked"));

    // run 2: NOTHING changed -> every job skips (the handler never runs — the true incremental)
    crd::containers::Array<crd::u8> pack_run1(&g_alloc);
    REQUIRE(fs::read_file_binary(pack_path, pack_run1));
    REQUIRE(cmd_cook(tmp.generic().data(), pack_path.generic().data()) == 0);
    CHECK(log_status_is(tmp, "a.bin", "skipped"));
    CHECK(log_status_is(tmp, "b.bin", "skipped"));
    CHECK(log_status_is(tmp, "tri.gltf", "skipped"));
    CHECK(log_status_is(tmp, "geo.bin", "skipped"));
    crd::containers::Array<crd::u8> pack_run2(&g_alloc);
    REQUIRE(fs::read_file_binary(pack_path, pack_run2));
    REQUIRE(pack_run1.size() == pack_run2.size());
    CHECK(std::memcmp(pack_run1.data(), pack_run2.data(), pack_run1.size()) == 0);

    // run 3: touch ONLY b.bin -> exactly b.bin recooks
    const crd::u8 b2[] = {7, 8, 9, 10};
    write_bytes(tmp / "b.bin", b2, sizeof(b2));
    REQUIRE(cmd_cook(tmp.generic().data(), pack_path.generic().data()) == 0);
    CHECK(log_status_is(tmp, "a.bin", "skipped"));
    CHECK(log_status_is(tmp, "b.bin", "cooked"));
    CHECK(log_status_is(tmp, "tri.gltf", "skipped"));
    CHECK(log_status_is(tmp, "geo.bin", "skipped"));

    // run 4: touch geo.bin -> BOTH its own blob job AND tri.gltf recook (the recorded dependency edge —
    // the O3DE staleness lesson made structural; the old cook served a stale mesh here)
    const crd::f32 pos2[9] = {0, 0, 0, 2, 0, 0, 0, 2, 0};
    write_bytes(tmp / "geo.bin", reinterpret_cast<const crd::u8*>(pos2), sizeof(pos2));
    REQUIRE(cmd_cook(tmp.generic().data(), pack_path.generic().data()) == 0);
    CHECK(log_status_is(tmp, "geo.bin", "cooked"));
    CHECK(log_status_is(tmp, "tri.gltf", "cooked"));
    CHECK(log_status_is(tmp, "a.bin", "skipped"));
    CHECK(log_status_is(tmp, "b.bin", "skipped"));
    crd::containers::Array<crd::u8> pack_run4(&g_alloc);
    REQUIRE(fs::read_file_binary(pack_path, pack_run4));

    // BYTE-IDENTICAL vs a from-scratch cook: wipe the cache (sidecar .metas keep the ids) and recook everything
    REQUIRE(fs::remove_all(tmp / ".cook_cache"));
    REQUIRE(cmd_cook(tmp.generic().data(), pack_path.generic().data()) == 0);
    crd::containers::Array<crd::u8> pack_scratch(&g_alloc);
    REQUIRE(fs::read_file_binary(pack_path, pack_scratch));
    REQUIRE(pack_run4.size() == pack_scratch.size());
    CHECK(std::memcmp(pack_run4.data(), pack_scratch.data(), pack_run4.size()) == 0);

    REQUIRE(fs::remove_all(tmp));
}

TEST_CASE("GEO-6: the .meta sidecar is a cook input -- touching it recooks even a handler that never reads it",
          "[resources][cook][geo6]")
{
    register_builtin_handlers();
    const auto tmp = make_temp_dir();
    REQUIRE(fs::create_directories(tmp));
    const auto pack_path = tmp / "out.crdr";

    const crd::u8 v[] = {42};
    write_bytes(tmp / "v.bin", v, sizeof(v));
    REQUIRE(cmd_cook(tmp.generic().data(), pack_path.generic().data()) == 0);
    REQUIRE(cmd_cook(tmp.generic().data(), pack_path.generic().data()) == 0);
    REQUIRE(log_status_is(tmp, "v.bin", "skipped"));

    // append a comment to v.bin.meta — the id is unchanged but the FILE is (the force-recorded meta edge)
    crd::containers::String meta_text(&g_alloc);
    REQUIRE(fs::read_file_text(tmp / "v.bin.meta", meta_text));
    meta_text.append("# a comment\n");
    REQUIRE(fs::write_file_text(tmp / "v.bin.meta",
                                crd::containers::StringView(meta_text.data(), meta_text.size())));
    REQUIRE(cmd_cook(tmp.generic().data(), pack_path.generic().data()) == 0);
    CHECK(log_status_is(tmp, "v.bin", "cooked"));

    REQUIRE(fs::remove_all(tmp));
}

TEST_CASE("GEO-6: kill-resume + torn-cache refusal -- a killed run's jobs recook; a corrupt artifact never ships",
          "[resources][cook][geo6]")
{
    register_builtin_handlers();
    const auto tmp = make_temp_dir();
    REQUIRE(fs::create_directories(tmp));
    const auto pack_path = tmp / "out.crdr";

    const crd::u8 a[] = {1, 1};
    const crd::u8 b[] = {2, 2};
    write_bytes(tmp / "a.bin", a, sizeof(a));
    write_bytes(tmp / "b.bin", b, sizeof(b));
    REQUIRE(cmd_cook(tmp.generic().data(), pack_path.generic().data()) == 0);
    crd::containers::Array<crd::u8> pack_good(&g_alloc);
    REQUIRE(fs::read_file_binary(pack_path, pack_good));

    // simulate a run KILLED mid-job on b.bin: a dangling journal `begin` + a torn cached artifact
    {
        crd::cooker::CookDb db(&g_alloc);
        REQUIRE(db.journal_begin(tmp, crd::containers::StringView("b.bin")));

        ResourceId b_id;
        crd::containers::String meta_text(&g_alloc);
        REQUIRE(fs::read_file_text(tmp / "b.bin.meta", meta_text));
        const std::string_view sv(meta_text.data(), meta_text.size());
        const auto             q = sv.find("uuid = \"");
        REQUIRE(q != std::string_view::npos);
        b_id = ResourceId::parse(sv.substr(q + 8U, 36U));
        REQUIRE(!b_id.is_null());
        crd::containers::String art(&g_alloc);
        art.append(b_id.to_string(&g_alloc).c_str());
        art.append(".crdr");
        const crd::u8 torn[] = {0xDE, 0xAD};
        write_bytes(tmp / ".cook_cache" / crd::containers::StringView(art.data(), art.size()), torn, sizeof(torn));
    }

    // resume: b.bin is distrusted and recooks; a.bin still skips; the pack matches the pre-kill build exactly
    REQUIRE(cmd_cook(tmp.generic().data(), pack_path.generic().data()) == 0);
    CHECK(log_status_is(tmp, "a.bin", "skipped"));
    CHECK(log_status_is(tmp, "b.bin", "cooked"));
    crd::containers::Array<crd::u8> pack_resumed(&g_alloc);
    REQUIRE(fs::read_file_binary(pack_path, pack_resumed));
    REQUIRE(pack_good.size() == pack_resumed.size());
    CHECK(std::memcmp(pack_good.data(), pack_resumed.data(), pack_good.size()) == 0);

    // a torn cache file WITHOUT a journal entry: the artifact-hash check refuses it and recooks (belt + braces)
    {
        ResourceId a_id;
        crd::containers::String meta_text(&g_alloc);
        REQUIRE(fs::read_file_text(tmp / "a.bin.meta", meta_text));
        const std::string_view sv(meta_text.data(), meta_text.size());
        const auto             q = sv.find("uuid = \"");
        a_id = ResourceId::parse(sv.substr(q + 8U, 36U));
        crd::containers::String art(&g_alloc);
        art.append(a_id.to_string(&g_alloc).c_str());
        art.append(".crdr");
        const crd::u8 torn[] = {0xBA, 0xD0};
        write_bytes(tmp / ".cook_cache" / crd::containers::StringView(art.data(), art.size()), torn, sizeof(torn));
    }
    REQUIRE(cmd_cook(tmp.generic().data(), pack_path.generic().data()) == 0);
    CHECK(log_status_is(tmp, "a.bin", "cooked"));
    crd::containers::Array<crd::u8> pack_healed(&g_alloc);
    REQUIRE(fs::read_file_binary(pack_path, pack_healed));
    REQUIRE(pack_good.size() == pack_healed.size());
    CHECK(std::memcmp(pack_good.data(), pack_healed.data(), pack_good.size()) == 0);

    REQUIRE(fs::remove_all(tmp));
}

TEST_CASE("GEO-6: the dependency graph is QUERYABLE -- forward and reverse edges from the cook database",
          "[resources][cook][geo6]")
{
    register_builtin_handlers();
    const auto tmp = make_temp_dir();
    REQUIRE(fs::create_directories(tmp));
    const auto pack_path = tmp / "out.crdr";
    write_ext_gltf(tmp);
    REQUIRE(cmd_cook(tmp.generic().data(), pack_path.generic().data()) == 0);

    crd::cooker::CookDb db(&g_alloc);
    db.load(tmp);

    // forward: the tri.gltf job exists and RECORDS geo.bin as an input edge
    const crd::cooker::DbJob* job = db.find_job(crd::containers::StringView("tri.gltf"));
    REQUIRE(job != nullptr);
    bool saw_bin = false;
    bool saw_src = false;
    for (crd::usize i = 0; i < job->inputs.size(); ++i)
    {
        const std::string_view p(job->inputs[i].path.data(), job->inputs[i].path.size());
        if (p == "geo.bin") { saw_bin = job->inputs[i].existed; }
        if (p == "tri.gltf") { saw_src = job->inputs[i].existed; }
    }
    CHECK(saw_bin);
    CHECK(saw_src);
    REQUIRE(job->products.size() >= 1U);

    // reverse: who consumes geo.bin? BOTH its own blob job and the glTF job
    crd::containers::Array<const crd::cooker::DbJob*> consumers(&g_alloc);
    db.jobs_consuming(crd::containers::StringView("geo.bin"), consumers);
    REQUIRE(consumers.size() == 2U);

    // reverse: which job produced the glTF's main artifact?
    const crd::cooker::DbJob* producer = db.find_producer(job->products[0].id);
    REQUIRE(producer == job);

    REQUIRE(fs::remove_all(tmp));
}
