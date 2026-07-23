// test_threemf.cpp — GEO-5 pt 3 (D-007): the 3MF gates. Import: a printer-style model part (mm units → SI, sRGB
// displaycolor → linear, a transformed build item through the SHARED TRS decompose, requiredextensions REFUSED BY
// NAME). Export: the WATERTIGHT gate — a closed tetrahedron ships, a deliberately HOLED mesh is REFUSED — and the
// full OPC round-trip: model.xml → our ZIP writer → our ZIP reader → our parser → geometry value-exact.

#include <crd/assetio/threemf.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/resources/zip_archive.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
[[nodiscard]] crd::containers::ConstSpan<crd::u8> sv(const char* s)
{
    return {reinterpret_cast<const crd::u8*>(s), std::strlen(s)};
}

// a closed tetrahedron in the cooked 48-byte layout (4 verts, 4 faces, outward winding — WATERTIGHT)
void build_tetrahedron(crd::containers::Array<crd::u8>& verts, crd::containers::Array<crd::u8>& idx)
{
    const float p[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (const auto& v : p)
    {
        float rec[12] = {v[0], v[1], v[2], 0, 0, 1, 0, 0, 1, 0, 0, 1};
        const auto* b = reinterpret_cast<const crd::u8*>(rec);
        for (crd::u32 k = 0; k < 48U; ++k) { verts.push_back(b[k]); }
    }
    const crd::u32 faces[4][3] = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}};
    for (const auto& f : faces)
    {
        for (crd::u32 k = 0; k < 3U; ++k)
        {
            const auto* b = reinterpret_cast<const crd::u8*>(&f[k]);
            for (crd::u32 j = 0; j < 4U; ++j) { idx.push_back(b[j]); }
        }
    }
}
} // namespace

TEST_CASE("3mf: a printer-style model part imports -- units to SI, sRGB->linear, the shared TRS decompose",
          "[assetio][threemf]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    namespace aio = crd::assetio;

    const char* model = R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
 <resources>
  <basematerials id="5">
   <base name="ABS Red" displaycolor="#FF0000"/>
   <base name="PLA White" displaycolor="#FFFFFF80"/>
  </basematerials>
  <object id="2" type="model" name="bracket" pid="5" pindex="1">
   <mesh>
    <vertices>
     <vertex x="0" y="0" z="0"/>
     <vertex x="1000" y="0" z="0"/>
     <vertex x="0" y="1000" z="0"/>
    </vertices>
    <triangles>
     <triangle v1="0" v2="1" v3="2"/>
    </triangles>
   </mesh>
  </object>
 </resources>
 <build>
  <item objectid="2" transform="1 0 0 0 1 0 0 0 1 500 0 0"/>
 </build>
</model>)";

    aio::ImportedAsset ia(&alloc);
    REQUIRE(aio::parse_3mf_model(sv(model), &alloc, ia) == aio::ImportStatus::Ok);

    // geometry: mm → SI metres
    REQUIRE(ia.meshes.size() == 1U);
    const aio::ImportedMesh& im = ia.meshes[0];
    REQUIRE(im.positions.size() == 3U);
    CHECK(im.positions[1].x == 1.0F); // 1000 mm = 1 m
    CHECK(im.positions[2].y == 1.0F);
    REQUIRE(im.indices.size() == 3U);
    CHECK(im.material == 1); // pid 5 / pindex 1 -> the SECOND base material

    // materials: sRGB displaycolor decoded to LINEAR (0xFF -> 1.0 exact; 0x80 alpha -> 128/255 linear coverage)
    REQUIRE(ia.materials.size() == 2U);
    CHECK(ia.materials[0].base_color.x == 1.0F);
    CHECK(ia.materials[0].base_color.y == 0.0F);
    CHECK(ia.materials[1].base_alpha == 128.0F / 255.0F);

    // the build item: translation 500 mm -> 0.5 m through the SHARED decompose; identity rotation/scale
    REQUIRE(ia.nodes.size() == 1U);
    CHECK(ia.nodes[0].mesh == 0);
    CHECK(ia.nodes[0].translation.x == 0.5F);
    CHECK(ia.nodes[0].rotation.w == 1.0F);
    CHECK(ia.nodes[0].scale.x == 1.0F);
}

TEST_CASE("3mf: refusal classes -- requiredextensions, bad unit, OOB triangle", "[assetio][threemf]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    namespace aio = crd::assetio;
    aio::ImportedAsset ia(&alloc);

    CHECK(aio::parse_3mf_model(
              sv(R"(<model requiredextensions="b"><resources/><build/></model>)"), &alloc, ia)
          == aio::ImportStatus::Unsupported); // REQUIRED semantics we cannot honor — refused BY NAME
    CHECK(aio::parse_3mf_model(sv(R"(<model unit="parsec"><resources/></model>)"), &alloc, ia)
          == aio::ImportStatus::Malformed);
    CHECK(aio::parse_3mf_model(
              sv(R"(<model><resources><object id="1" type="model"><mesh><vertices><vertex x="0" y="0" z="0"/></vertices>)"
                 R"(<triangles><triangle v1="0" v2="1" v3="9"/></triangles></mesh></object></resources></model>)"),
              &alloc, ia)
          == aio::ImportStatus::Malformed); // an index past the vertex count
    CHECK(aio::parse_3mf_model(sv("<notmodel/>"), &alloc, ia) == aio::ImportStatus::NotRecognized);
}

TEST_CASE("3mf: extension awareness -- beamlattice refused BY NAME, slicestack dropped with a WARNING",
          "[assetio][threemf]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    namespace aio = crd::assetio;

    // a beam-lattice mesh is not triangle geometry: importing the bare vertices would silently lose the structure
    aio::ImportedAsset lattice(&alloc);
    CHECK(aio::parse_3mf_model(
              sv(R"(<model><resources><object id="1" type="model"><mesh>)"
                 R"(<vertices><vertex x="0" y="0" z="0"/><vertex x="1" y="0" z="0"/></vertices>)"
                 R"(<b:beamlattice minlength="0.1" radius="1"><b:beams><b:beam v1="0" v2="1"/></b:beams></b:beamlattice>)"
                 R"(</mesh></object></resources><build/></model>)"),
              &alloc, lattice)
          == aio::ImportStatus::Unsupported);

    // slice stacks are auxiliary print data: the triangle geometry imports COMPLETE, the drop is warned, never silent
    aio::ImportedAsset sliced(&alloc);
    REQUIRE(aio::parse_3mf_model(
                sv(R"(<model><resources>)"
                   R"(<s:slicestack id="7" zbottom="0"><s:slice ztop="0.1"/></s:slicestack>)"
                   R"(<object id="1" type="model"><mesh>)"
                   R"(<vertices><vertex x="0" y="0" z="0"/><vertex x="1" y="0" z="0"/><vertex x="0" y="1" z="0"/></vertices>)"
                   R"(<triangles><triangle v1="0" v2="1" v3="2"/></triangles>)"
                   R"(</mesh></object></resources><build><item objectid="1"/></build></model>)"),
                &alloc, sliced)
            == aio::ImportStatus::Ok);
    CHECK(sliced.meshes.size() == 1U);
    CHECK(sliced.meshes[0].indices.size() == 3U);
    CHECK(sliced.warning_count >= 1U); // the dropped slice stack is REPORTED
}

TEST_CASE("3mf: a REAL reference-producer file imports -- the lib3mf fixture through our ZIP + parser",
          "[assetio][threemf]")
{
    // tests/asset-io/data/lib3mf_box.3mf was authored by lib3mf 2.5.0 (the 3MF Consortium's REFERENCE library, the
    // one slicers embed): a 20 mm box, a base material, a +40 mm translated build item, production-extension UUID
    // attributes sprinkled in. The permanent "a real printer-toolchain 3MF imports" gate.
    crd::memory::TlsfAllocator alloc(8U << 20U);
    namespace aio = crd::assetio;
    namespace res = crd::resources;

    crd::containers::String path(&alloc);
    if (const char* root = std::getenv("CRD_ASSETIO_DATA"); root != nullptr && root[0] != '\0') { path.append(root); }
    else { path.append("tests/asset-io/data"); }
    path.append("/lib3mf_box.3mf");

    crd::containers::Array<crd::u8> bytes(&alloc);
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        REQUIRE(f != nullptr);
        (void)std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        (void)std::fseek(f, 0, SEEK_SET);
        REQUIRE(sz > 0);
        bytes.resize(static_cast<crd::usize>(sz));
        REQUIRE(std::fread(bytes.data(), 1U, bytes.size(), f) == bytes.size());
        (void)std::fclose(f);
    }

    res::ZipReader zr(&alloc);
    REQUIRE(zr.open(crd::containers::ConstSpan<crd::u8>(bytes.data(), bytes.size())) == res::ZipError::Ok);
    const crd::i64 part = zr.find(aio::k3mfModelPart);
    REQUIRE(part >= 0);
    crd::containers::Array<crd::u8> model_bytes(&alloc);
    REQUIRE(zr.extract(static_cast<crd::usize>(part), model_bytes) == res::ZipError::Ok);

    aio::ImportedAsset ia(&alloc);
    REQUIRE(aio::parse_3mf_model(crd::containers::ConstSpan<crd::u8>(model_bytes.data(), model_bytes.size()), &alloc,
                                 ia)
            == aio::ImportStatus::Ok);

    REQUIRE(ia.meshes.size() == 1U);
    CHECK(ia.meshes[0].positions.size() == 8U);
    CHECK(ia.meshes[0].indices.size() == 36U);
    crd::f32 max_x = 0.0F;
    for (crd::usize v = 0; v < ia.meshes[0].positions.size(); ++v)
    {
        if (ia.meshes[0].positions[v].x > max_x) { max_x = ia.meshes[0].positions[v].x; }
    }
    CHECK(max_x == 0.02F); // 20 mm -> SI metres

    REQUIRE(ia.materials.size() == 1U);
    CHECK(ia.materials[0].base_color.x == 0.0F); // #008080FF teal: red 0 exact; green/blue sRGB-decoded
    CHECK(ia.materials[0].base_color.y > 0.21F);
    CHECK(ia.materials[0].base_color.y < 0.22F);
    CHECK(ia.materials[0].base_alpha == 1.0F);

    REQUIRE(ia.nodes.size() == 1U);
    CHECK(ia.nodes[0].mesh == 0);
    CHECK(ia.nodes[0].translation.x == 0.04F); // the +40 mm build translation -> SI
}

TEST_CASE("3mf: a REAL slicer-authored file imports -- PrusaSlicer 2.9.6's own re-export of OUR tetra",
          "[assetio][threemf]")
{
    // tests/asset-io/data/prusaslicer_tetra.3mf: PrusaSlicer 2.9.6 loaded OUR exported tetra (after slicing it to
    // G-code) and re-exported it through ITS OWN 3MF writer. The genuine printer-toolchain article, closing the loop:
    // our writer -> the reference slicer -> its writer -> our parser.
    crd::memory::TlsfAllocator alloc(8U << 20U);
    namespace aio = crd::assetio;
    namespace res = crd::resources;

    crd::containers::String path(&alloc);
    if (const char* root = std::getenv("CRD_ASSETIO_DATA"); root != nullptr && root[0] != '\0') { path.append(root); }
    else { path.append("tests/asset-io/data"); }
    path.append("/prusaslicer_tetra.3mf");

    crd::containers::Array<crd::u8> bytes(&alloc);
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        REQUIRE(f != nullptr);
        (void)std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        (void)std::fseek(f, 0, SEEK_SET);
        REQUIRE(sz > 0);
        bytes.resize(static_cast<crd::usize>(sz));
        REQUIRE(std::fread(bytes.data(), 1U, bytes.size(), f) == bytes.size());
        (void)std::fclose(f);
    }

    res::ZipReader zr(&alloc);
    REQUIRE(zr.open(crd::containers::ConstSpan<crd::u8>(bytes.data(), bytes.size())) == res::ZipError::Ok);
    const crd::i64 part = zr.find(aio::k3mfModelPart);
    REQUIRE(part >= 0);
    crd::containers::Array<crd::u8> model_bytes(&alloc);
    REQUIRE(zr.extract(static_cast<crd::usize>(part), model_bytes) == res::ZipError::Ok);

    aio::ImportedAsset ia(&alloc);
    REQUIRE(aio::parse_3mf_model(crd::containers::ConstSpan<crd::u8>(model_bytes.data(), model_bytes.size()), &alloc,
                                 ia)
            == aio::ImportStatus::Ok);

    REQUIRE(ia.meshes.size() == 1U);
    REQUIRE(ia.meshes[0].positions.size() == 4U);
    REQUIRE(ia.meshes[0].indices.size() == 12U);
    // the slicer may recenter via mesh coordinates or a build transform — the EXTENT is invariant: the 1 m tetra legs
    crd::f32 min_x = ia.meshes[0].positions[0].x;
    crd::f32 max_x = min_x;
    for (crd::usize v = 1; v < ia.meshes[0].positions.size(); ++v)
    {
        const crd::f32 x = ia.meshes[0].positions[v].x;
        if (x < min_x) { min_x = x; }
        if (x > max_x) { max_x = x; }
    }
    CHECK(max_x - min_x > 0.999F); // mm-authored by the slicer -> SI metres
    CHECK(max_x - min_x < 1.001F);
}

TEST_CASE("3mf: the WATERTIGHT export gate + the full OPC round-trip through our ZIP", "[assetio][threemf][export]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    namespace aio = crd::assetio;
    namespace res = crd::resources;

    crd::containers::Array<crd::u8> verts(&alloc);
    crd::containers::Array<crd::u8> idx(&alloc);
    build_tetrahedron(verts, idx);

    // the deliberately HOLED mesh (one face removed) is REFUSED — the row's red gate
    crd::containers::String holed(&alloc);
    CHECK_FALSE(aio::threemf_write_model_xml(crd::containers::ConstSpan<crd::u8>(verts.data(), verts.size()),
                                             crd::containers::ConstSpan<crd::u8>(idx.data(), idx.size() - 12U),
                                             "holed", &alloc, holed));
    CHECK(holed.size() == 0U);

    // the closed tetrahedron ships
    crd::containers::String model_xml(&alloc);
    REQUIRE(aio::threemf_write_model_xml(crd::containers::ConstSpan<crd::u8>(verts.data(), verts.size()),
                                         crd::containers::ConstSpan<crd::u8>(idx.data(), idx.size()), "tetra", &alloc,
                                         model_xml));

    // wrap the three OPC parts with OUR ZIP writer → a complete .3mf
    res::ZipWriter zw(&alloc);
    REQUIRE(zw.add(aio::k3mfContentTypes, sv(aio::threemf_content_types_xml())));
    REQUIRE(zw.add(aio::k3mfRels, sv(aio::threemf_rels_xml())));
    REQUIRE(zw.add(aio::k3mfModelPart,
                   crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(model_xml.c_str()),
                                                       model_xml.size())));
    const auto threemf = zw.finish();
    REQUIRE(threemf.size() > 0U);

    // conformance hook: CRD_3MF_DUMP=<path> writes the EXACT product-path archive for reference-implementation
    // validation (lib3mf / a slicer) — the bytes the assertion below round-trips are the bytes the reference sees
    if (const char* dump = std::getenv("CRD_3MF_DUMP"); dump != nullptr && dump[0] != '\0')
    {
        std::FILE* f = std::fopen(dump, "wb");
        REQUIRE(f != nullptr);
        CHECK(std::fwrite(threemf.data(), 1U, threemf.size(), f) == threemf.size());
        (void)std::fclose(f);
    }

    // …and back: unzip with OUR reader, parse with OUR parser — geometry value-exact (metres → meter unit → metres)
    res::ZipReader zr(&alloc);
    REQUIRE(zr.open(crd::containers::ConstSpan<crd::u8>(threemf.data(), threemf.size())) == res::ZipError::Ok);
    const crd::i64 part = zr.find(aio::k3mfModelPart);
    REQUIRE(part >= 0);
    crd::containers::Array<crd::u8> extracted(&alloc);
    REQUIRE(zr.extract(static_cast<crd::usize>(part), extracted) == res::ZipError::Ok);

    aio::ImportedAsset back(&alloc);
    REQUIRE(aio::parse_3mf_model(crd::containers::ConstSpan<crd::u8>(extracted.data(), extracted.size()), &alloc, back)
            == aio::ImportStatus::Ok);
    REQUIRE(back.meshes.size() == 1U);
    REQUIRE(back.meshes[0].positions.size() == 4U);
    REQUIRE(back.meshes[0].indices.size() == 12U);
    CHECK(back.meshes[0].positions[1].x == 1.0F); // the tetra vertex survives metres→meter→metres exactly
    CHECK(back.meshes[0].positions[3].z == 1.0F);
    CHECK(back.meshes[0].indices[0] == 0U);
    CHECK(back.meshes[0].indices[11] == 3U);
    REQUIRE(back.nodes.size() == 1U);
    CHECK(back.nodes[0].mesh == 0);
}
