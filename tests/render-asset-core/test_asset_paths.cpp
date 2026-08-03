// RAF-9 Increment 1 — the PURE folder->extension map + on_disk_relative(AssetRef).
//
// Gate (device-free, no I/O, no device): engine://frame/forward_csm -> "frame/forward_csm.frame.toml"; the crd://
// alias yields the IDENTICAL relative path AND the IDENTICAL AssetId (crd:// folds to engine:// — load-bearing);
// app://x.id() != engine://x.id() (no shadowing); each shipped folder maps to its extension; an unknown folder ->
// on_disk_relative returns false, leaving `out` untouched.
//
// ⛔ named allocator throughout (no hidden default malloc); ASCII-only test names.

#include <crd/renderasset/renderasset.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::containers::String;
using crd::containers::StringView;
using namespace crd::renderasset;

namespace
{
// True if `raw` parses valid and its on-disk relative path equals `expected`.
bool rel_is(StringView raw, const char* expected, crd::memory::IAllocator* alloc)
{
    DiagnosticList diags(alloc);
    AssetRef       ref = AssetRef::parse(raw, diags, alloc);
    if (!ref.valid())
    {
        return false;
    }
    String out(alloc);
    if (!on_disk_relative(ref, out))
    {
        return false;
    }
    return StringView(out.data(), out.size()) == StringView(expected);
}

// True if `raw` parses valid but on_disk_relative REFUSES it (unknown folder) leaving `out` untouched.
bool rel_refused(StringView raw, crd::memory::IAllocator* alloc)
{
    DiagnosticList diags(alloc);
    AssetRef       ref = AssetRef::parse(raw, diags, alloc);
    if (!ref.valid())
    {
        return false;
    }
    String out(alloc);
    const bool ok = on_disk_relative(ref, out);
    return !ok && out.size() == 0U;
}
} // namespace

TEST_CASE("raf9 on_disk_relative maps a canonical id to its on-disk file")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf9-relpath");
    CHECK(rel_is("engine://frame/forward_csm", "frame/forward_csm.frame.toml", &alloc));
}

TEST_CASE("raf9 crd alias yields the identical relative path AND AssetId as engine")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf9-alias");
    DiagnosticList             diags(&alloc);
    const AssetRef             eng = AssetRef::parse("engine://frame/forward_csm", diags, &alloc);
    const AssetRef             crd = AssetRef::parse("crd://frame/forward_csm", diags, &alloc);
    REQUIRE(eng.valid());
    REQUIRE(crd.valid());
    // ⛔ load-bearing: crd:// folds to engine:// -> SAME AssetId, so a registry keyed by parsed id matches the
    // still-crd:// live frames before the increment-4 rewrite, and the rewrite is pixel-neutral.
    CHECK(eng.id() == crd.id());
    CHECK(rel_is("crd://frame/forward_csm", "frame/forward_csm.frame.toml", &alloc));
}

TEST_CASE("raf9 app does not shadow engine (distinct AssetId)")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf9-noshadow");
    DiagnosticList             diags(&alloc);
    const AssetRef             eng = AssetRef::parse("engine://frame/forward_csm", diags, &alloc);
    const AssetRef             app = AssetRef::parse("app://frame/forward_csm", diags, &alloc);
    REQUIRE(eng.valid());
    REQUIRE(app.valid());
    CHECK(eng.id() != app.id());
}

TEST_CASE("raf9 every shipped folder maps to its extension")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf9-folders");
    CHECK(rel_is("engine://frame/x", "frame/x.frame.toml", &alloc));
    CHECK(rel_is("engine://vertex/scene", "vertex/scene.crdv", &alloc));
    CHECK(rel_is("engine://material/scene", "material/scene.crdm", &alloc));
    CHECK(rel_is("engine://post/tonemap_agx", "post/tonemap_agx.crdp", &alloc));
    CHECK(rel_is("engine://lighting/scene_forward", "lighting/scene_forward.crdl", &alloc));
    CHECK(rel_is("engine://technique/forward_csm", "technique/forward_csm.crdt", &alloc));
    CHECK(rel_is("engine://lod/scene_default", "lod/scene_default.crdlod", &alloc));
    // a nested name keeps its subfolders and takes the FIRST-segment folder's extension.
    CHECK(rel_is("engine://vertex/scene_rt_raygen", "vertex/scene_rt_raygen.crdv", &alloc));
}

TEST_CASE("raf9 an unknown folder is refused (no on-disk shape)")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf9-unknown");
    CHECK(rel_refused("engine://weird/x", &alloc));
    CHECK(rel_refused("engine://scene/mesh", &alloc)); // a PROGRAM id, not a file folder -> no extension
}
