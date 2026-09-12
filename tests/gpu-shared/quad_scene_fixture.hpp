#pragma once

// CEIR-31b-4-b-iii — a SHARED geometry-scene fixture: a UV quad mesh cooked through the real resource pipeline + the
// single-entry .crdr pack writer. Lifted so the frosted-glass hard-edge arms (orientation / far==tint / blur monotone /
// width∝radius) can put REAL geometry-pass content in scene_color — the flip question is specifically about a geometry
// pass's output read by the ui fullscreen chain, so a fullscreen-written backdrop would dodge it. Allocator-parametrized
// (no per-TU galloc) so any test TU can use it. `write_one_pack` REQUIREs its write, so include from a Catch2 TU.
//
// (test_scene_render_gpu.cpp carries its own older copies of these two helpers; migrating that TU onto this fixture is a
// ledgered follow-up — left untouched here to keep this slice's blast radius to the frosted-glass TU.)

#include <crd/containers/array.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/mesh_resource.hpp>
#include <crd/resources/resource_manager.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcpy/std::memset — the .crdr byte layout

namespace crd::tests
{
// A flat UV quad at z=0, normal +Z (toward a +Z camera so a frontal light lights it uniformly), spanning [-0.9,0.9]²,
// vertex layout [pos3, normal3, uv2, color4]. A Transform (scale/translate) positions it; scale (2,2,1) + translate
// +1.8 in Y puts its bottom edge at world y=0 (screen centre) so it covers the viewport's TOP half — a horizontal hard
// edge in scene_color (the axis the Y-flip question is about). Mirrors test_scene_render_gpu.cpp's build_quad_mesh_crdr.
[[nodiscard]] inline crd::containers::Array<crd::u8> build_quad_mesh_crdr(crd::memory::IAllocator* a,
                                                                         const crd::resources::ResourceId& id)
{
    const crd::f32 quad[4][12] = {{-0.9F, -0.9F, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1},
                                  {0.9F, -0.9F, 0, 0, 0, 1, 1, 0, 1, 0, 0, 1},
                                  {-0.9F, 0.9F, 0, 0, 0, 1, 0, 1, 1, 0, 0, 1},
                                  {0.9F, 0.9F, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1}};
    crd::containers::Array<crd::u8> verts(a);
    for (crd::u32 cnr = 0; cnr < 4U; ++cnr)
    {
        const auto* b = reinterpret_cast<const crd::u8*>(quad[cnr]);
        for (crd::u32 k = 0; k < 48U; ++k) { verts.push_back(b[k]); }
    }
    const crd::u32                  idx[6] = {0, 1, 2, 2, 1, 3};
    crd::containers::Array<crd::u8> indices(a);
    for (crd::u32 v : idx)
    {
        const auto* b = reinterpret_cast<const crd::u8*>(&v);
        for (crd::u32 k = 0; k < 4U; ++k) { indices.push_back(b[k]); }
    }
    crd::containers::Array<crd::u8> prim(a);
    prim.resize(4U + 32U);
    std::memset(prim.data(), 0, prim.size());
    const crd::u32 pc = 1U;
    const crd::u32 vc = 4U;
    const crd::u32 ic = 6U;
    std::memcpy(prim.data(), &pc, 4U);
    std::memcpy(prim.data() + 4U, &vc, 4U);
    std::memcpy(prim.data() + 8U, &ic, 4U);
    crd::resources::CrdrWriter w(a, id, crd::resources::kFourCC_MESH);
    w.add_chunk(crd::resources::kFourCC_VERT, crd::containers::as_const_span(verts));
    w.add_chunk(crd::resources::kFourCC_INDX, crd::containers::as_const_span(indices));
    w.add_chunk(crd::resources::kFourCC_PRIM, crd::containers::as_const_span(prim));
    return w.finish();
}

// Write ONE resource as a single-entry .crdr manifest pack to `path` (the two-pass offset fixup mirrors
// test_scene_render_gpu.cpp's write_one_pack). REQUIREs the disk write — call from a Catch2 TU.
inline void write_one_pack(crd::memory::IAllocator* a, const crd::platform::fs::Path& path,
                           const crd::resources::ResourceId& id, crd::u32 fourcc,
                           const crd::containers::Array<crd::u8>& art, const char* name)
{
    crd::containers::Array<crd::u8> pool(a);
    for (const char* p = name;; ++p)
    {
        pool.push_back(static_cast<crd::u8>(*p));
        if (*p == '\0') { break; }
    }
    crd::containers::Array<crd::resources::ManifestEntry> entries(a);
    crd::resources::ManifestEntry                         e;
    e.id          = id;
    e.type_fourcc = fourcc;
    e.blob_size   = static_cast<crd::u64>(art.size());
    entries.push_back(e);
    const crd::resources::ResourceId pack_id = crd::resources::ResourceId::mint_random();
    {
        crd::resources::CrdrWriter p1(a, pack_id, crd::resources::kFourCC_PACK);
        crd::resources::manifest_write(p1, crd::containers::as_const_span(entries), crd::containers::as_const_span(pool));
        auto b1                = p1.finish();
        entries[0].blob_offset = static_cast<crd::u64>(b1.size());
    }
    crd::resources::CrdrWriter p2(a, pack_id, crd::resources::kFourCC_PACK);
    crd::resources::manifest_write(p2, crd::containers::as_const_span(entries), crd::containers::as_const_span(pool));
    auto pack = p2.finish();
    for (crd::u8 b : art) { pack.push_back(b); }
    REQUIRE(crd::platform::fs::write_file_binary(path, crd::containers::as_const_span(pack)));
}
} // namespace crd::tests
