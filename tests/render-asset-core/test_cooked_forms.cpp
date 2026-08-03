// RAF-3 Gate 3 — the canonical cooked-asset envelope + generational runtime handle (device-free).
//
// Gates (mission §6 · D-007 RAF-3): deterministic cooked bytes; load WITHOUT the authoring parser (read straight
// from bytes); interface/content hash discrimination + invalidation; old-schema + wrong-type + malformed rejection;
// dependency-list round trip; generation replacement + stale-handle detection (the hot-reload safety property).
// (Missing-dependency rejection is gated on the DependencyGraph in test_render_asset_core.cpp.)
//
// ⛔ named allocator throughout; ASCII-only test names.

#include <crd/renderasset/renderasset.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using namespace crd::renderasset;

// ── Deterministic cooked bytes + load-without-parser round trip. ──
TEST_CASE("raf3 cooked header round-trips deterministically")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf3-header");

    const AssetId id = asset_id_of("engine://technique/scene");
    const AssetId deps[2] = {asset_id_of("engine://shader/a"), asset_id_of("engine://shader/b")};
    CookedHeader h;
    h.type = AssetType::Technique;
    h.schema = SchemaVersion{3U};
    h.iface = InterfaceHash{0x1234ULL};
    h.content = ContentHash{0x5678ULL};
    h.id = id;
    h.dependency_count = 2U;

    const usize sz = cooked_blob_header_size(2U);
    Array<u8> a(&alloc);
    a.resize(sz, u8{0});
    Array<u8> b(&alloc);
    b.resize(sz, u8{0});
    REQUIRE(write_cooked_header(a.data(), a.size(), h, deps) == sz);
    REQUIRE(write_cooked_header(b.data(), b.size(), h, deps) == sz);
    for (usize i = 0; i < sz; ++i)
    {
        REQUIRE(a[i] == b[i]); // byte-deterministic
    }

    // Load straight from the bytes — no authoring parser involved.
    DiagnosticList d(&alloc);
    CookedHeader out;
    Array<AssetId> out_deps(&alloc);
    REQUIRE(read_cooked_header(a.data(), a.size(), AssetType::Technique, SchemaVersion{3U}, out, out_deps, d));
    REQUIRE_FALSE(d.has_errors());
    REQUIRE(out.type == AssetType::Technique);
    REQUIRE(out.schema == SchemaVersion{3U});
    REQUIRE(out.iface == InterfaceHash{0x1234ULL});
    REQUIRE(out.content == ContentHash{0x5678ULL});
    REQUIRE(out.id == id);
    REQUIRE(out.dependency_count == 2U);
    REQUIRE(out_deps.size() == 2U);
    REQUIRE(out_deps[0] == deps[0]);
    REQUIRE(out_deps[1] == deps[1]);
}

// ── Interface / content hash discrimination. ──
TEST_CASE("raf3 interface and content hashes discriminate")
{
    const char* v1 = "stage-io-v1";
    const char* v2 = "stage-io-v2";
    REQUIRE(interface_hash_of(v1, 11U) != interface_hash_of(v2, 11U)); // a changed interface invalidates
    REQUIRE(interface_hash_of(v1, 11U) == interface_hash_of(v1, 11U)); // deterministic
    REQUIRE(content_hash_of(v1, 11U) == content_hash_of(v1, 11U));
    REQUIRE(content_hash_of(v1, 11U) != content_hash_of(v2, 11U));
}

// ── The loader rejects old-schema / wrong-type / malformed / truncated blobs. ──
TEST_CASE("raf3 cooked loader rejects bad blobs")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf3-reject");

    CookedHeader h;
    h.type = AssetType::Shader;
    h.schema = SchemaVersion{5U};
    h.id = asset_id_of("engine://shader/x");
    Array<u8> blob(&alloc);
    blob.resize(cooked_blob_header_size(0U), u8{0});
    REQUIRE(write_cooked_header(blob.data(), blob.size(), h, nullptr) == blob.size());

    CookedHeader out;
    Array<AssetId> deps(&alloc);
    {
        DiagnosticList d(&alloc);
        REQUIRE_FALSE(read_cooked_header(blob.data(), blob.size(), AssetType::Shader, SchemaVersion{6U}, out, deps, d));
        REQUIRE(d.contains(DiagCode::SchemaMismatch));
    }
    {
        DiagnosticList d(&alloc);
        REQUIRE_FALSE(read_cooked_header(blob.data(), blob.size(), AssetType::Material, SchemaVersion{5U}, out, deps, d));
        REQUIRE(d.contains(DiagCode::TypeMismatch));
    }
    {
        DiagnosticList d(&alloc);
        Array<u8> garbage(&alloc);
        garbage.resize(kCookedHeaderBytes, u8{0xAB});
        REQUIRE_FALSE(
            read_cooked_header(garbage.data(), garbage.size(), AssetType::Shader, SchemaVersion{5U}, out, deps, d));
        REQUIRE(d.contains(DiagCode::MalformedBlob));
    }
    {
        DiagnosticList d(&alloc);
        REQUIRE_FALSE(read_cooked_header(blob.data(), 8U, AssetType::Shader, SchemaVersion{5U}, out, deps, d));
        REQUIRE(d.contains(DiagCode::TruncatedBlob));
    }
}

// ── Generation replacement + stale-handle detection (hot-reload safety). ──
TEST_CASE("raf3 runtime slot generation detects staleness")
{
    int a1 = 1;
    int a2 = 2;
    RuntimeSlot<int> slot;
    const AssetId id = asset_id_of("engine://material/m");

    const RuntimeHandle<int> h1 = slot.install(&a1, id);
    REQUIRE(h1.valid());
    REQUIRE(slot.is_current(h1));
    REQUIRE(slot.generation().value == 1U);

    const RuntimeHandle<int> h2 = slot.install(&a2, id); // hot-reload replacement
    REQUIRE(slot.is_current(h2));
    REQUIRE_FALSE(slot.is_current(h1)); // the pre-replacement handle is detectably stale
    REQUIRE(slot.generation().value == 2U);
    REQUIRE(slot.current() == &a2);
}
