// tests/cooker/test_scene_decompose.cpp — GEO-3 stage 3 gates: the glTF scene DECOMPOSE. A hand-built mm-authored GLB
// (node hierarchy + mesh + camera + spot light) cooks through the REAL wave1 handler into a SCEN extra artifact built
// by the REAL SceneArtifactBuilder; the artifact loads through the REAL SceneLoader and INSTANTIATES into a runtime
// World — verifying SI-scaled node transforms, BAKED hierarchical world matrices, MeshRenderer ResourceId wiring to
// the cooked mesh artifact, exact camera/light components, zero skipped components/relations, and sidecar-id
// stability across recooks.

#include <catch2/catch_test_macros.hpp>

#include <crd/cooker/cook_handler.hpp>
#include <crd/math/mat.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/openpbr_material.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/scene/relation.hpp>
#include <crd/scene/render_components.hpp>
#include <crd/scene/scene_resource.hpp>
#include <crd/scene/serialize.hpp>
#include <crd/scene/system.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/transform_propagation.hpp>
#include <crd/scene/world.hpp>

#include <cmath>
#include <cstring>
#include <memory>

namespace fs = crd::platform::fs;

namespace crd::cooker
{
void register_wave1_mesh_handler();
} // namespace crd::cooker

namespace
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::MallocAllocator g_alloc;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

crd::cooker::CookHandlerFn glb_handler()
{
    static bool registered = false;
    if (!registered)
    {
        crd::cooker::register_wave1_mesh_handler();
        registered = true;
    }
    return crd::cooker::find_cook_handler(crd::containers::StringView(".glb"));
}

void push_bytes(crd::containers::Array<crd::u8>& b, const void* src, crd::usize n)
{
    const crd::u8* s = static_cast<const crd::u8*>(src);
    for (crd::usize i = 0; i < n; ++i) { b.push_back(s[i]); }
}
void push_u32(crd::containers::Array<crd::u8>& b, crd::u32 v) { push_bytes(b, &v, 4); }
void push_f32(crd::containers::Array<crd::u8>& b, crd::f32 v) { push_bytes(b, &v, 4); }

// the runtime-side registration — must match the cook-side temp World bit-for-bit
void setup_runtime_world(crd::scene::World& w)
{
    w.register_component<crd::scene::Transform>(crd::scene::transform_serialize_trait());
    w.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
    w.register_builtin_relations();
    w.register_system(std::make_unique<crd::scene::TransformPropagation>());
    crd::scene::register_render_components(w);
}

} // namespace

TEST_CASE("cooker: glTF scene DECOMPOSES into a SCEN artifact -- SI transforms, baked hierarchy, resource refs",
          "[cooker][wave1][scene][geo]")
{
    // a mm-authored GLB: root "rig" at (1000,0,0) mm with three children — "part" at (0,500,0) mm carrying the
    // triangle mesh, a spot-light node, and a camera node. .meta position_scale = 0.001 → SI metres everywhere.
    crd::containers::Array<crd::u8> bin(&g_alloc);
    const crd::f32 pos[9] = {0, 0, 0, 1000, 0, 0, 0, 1000, 0}; // mm-scale triangle
    for (crd::f32 v : pos) { push_f32(bin, v); }

    const char* json = R"({
      "asset": {"version": "2.0"},
      "scene": 0,
      "scenes": [{"nodes": [0]}],
      "buffers": [{"byteLength": 36}],
      "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
      "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}],
      "cameras": [{"type": "perspective", "perspective": {"yfov": 0.9, "znear": 0.5}}],
      "extensions": {"KHR_lights_punctual": {"lights": [
        {"type": "spot", "color": [1.0, 0.25, 0.5], "intensity": 30.0, "range": 8.0,
         "spot": {"innerConeAngle": 0.1, "outerConeAngle": 0.5}}
      ]}},
      "nodes": [
        {"name": "rig", "translation": [1000, 0, 0], "children": [1, 2, 3]},
        {"name": "part", "translation": [0, 500, 0], "mesh": 0},
        {"name": "key", "extensions": {"KHR_lights_punctual": {"light": 0}}},
        {"name": "cam", "camera": 0}
      ],
      "materials": [{"name": "steel",
        "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.25, 0.125, 1.0], "roughnessFactor": 0.3}}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "material": 0}]}]
    })";

    crd::containers::Array<crd::u8> glb(&g_alloc);
    const crd::u32 jlen = static_cast<crd::u32>(std::strlen(json));
    const crd::u32 jpad = (4U - (jlen % 4U)) % 4U;
    const crd::u32 blen = static_cast<crd::u32>(bin.size());
    const crd::u32 bpad = (4U - (blen % 4U)) % 4U;
    push_u32(glb, 0x46546C67U);
    push_u32(glb, 2U);
    push_u32(glb, 12U + 8U + jlen + jpad + 8U + blen + bpad);
    push_u32(glb, jlen + jpad);
    push_u32(glb, 0x4E4F534AU);
    push_bytes(glb, json, jlen);
    for (crd::u32 i = 0; i < jpad; ++i) { glb.push_back(' '); }
    push_u32(glb, blen + bpad);
    push_u32(glb, 0x004E4942U);
    for (crd::usize i = 0; i < bin.size(); ++i) { glb.push_back(bin[i]); }
    for (crd::u32 i = 0; i < bpad; ++i) { glb.push_back(0); }

    const char* src_path  = "scen_rig.glb";
    const char* meta_path = "scen_rig.glb.meta";
    REQUIRE(fs::write_file_binary(fs::Path(crd::containers::StringView(src_path)), crd::containers::as_const_span(glb)));
    {
        const char* meta = "[id]\nuuid = \"0123456789abcdef0123456789abcdef\"\n[cook]\nposition_scale = 0.001\n";
        REQUIRE(fs::write_file_text(fs::Path(crd::containers::StringView(meta_path)),
                                    crd::containers::StringView(meta, std::strlen(meta))));
    }

    crd::cooker::CookHandlerFn handler = glb_handler();
    REQUIRE(handler != nullptr);
    crd::cooker::CookContext ctx;
    ctx.source_path = crd::containers::StringView(src_path);
    ctx.meta_path   = crd::containers::StringView(meta_path);
    ctx.id          = crd::resources::ResourceId::mint_random();
    ctx.allocator   = &g_alloc;

    const crd::cooker::CookResult result = handler(ctx);
    REQUIRE(result.ok);
    CHECK(result.type_fourcc == crd::resources::kFourCC_MESH); // the mesh is still the main artifact

    // two extras: the AUTHORED material (stage 4) + the SCEN artifact (no images in this GLB)
    REQUIRE(result.extra_artifacts.size() == 2U);
    const crd::cooker::ExtraArtifact& mtl  = result.extra_artifacts[0];
    const crd::cooker::ExtraArtifact& scen = result.extra_artifacts[1];
    CHECK(mtl.type_fourcc == crd::resources::kFourCC_PBRM);
    CHECK(scen.type_fourcc == crd::scene::kFourCC_SCEN);

    // load through the REAL SceneLoader → instantiate into a runtime World
    crd::scene::SceneLoader        loader;
    crd::resources::LoadContext    lctx;
    lctx.id        = scen.id;
    lctx.bytes     = crd::containers::as_const_span(scen.cooked_bytes);
    lctx.manager   = nullptr;
    lctx.allocator = &g_alloc;
    void* payload  = loader.load(lctx);
    REQUIRE(payload != nullptr);
    auto* res = static_cast<crd::scene::SceneResource*>(payload);

    crd::scene::World world{&g_alloc};
    setup_runtime_world(world);
    const crd::scene::SceneInstantiation inst = world.instantiate_scene(*res);
    CHECK(inst.components_skipped == 0U); // every cooked component type is registered — nothing dropped
    CHECK(inst.relations_skipped == 0U);
    REQUIRE(inst.entities.size() == 4U); // rig · part · key · cam (node spawn order = file order)

    // SI scale reached the NODE transforms: rig.x = 1000 mm → 1.0 m (local), part.y = 500 mm → 0.5 m
    const auto* rig_t = world.get_component<crd::scene::Transform>(inst.entities[0]);
    REQUIRE(rig_t != nullptr);
    CHECK(crd::math::to_raw_vec(rig_t->translation).x == 1.0F);
    const auto* part_t = world.get_component<crd::scene::Transform>(inst.entities[1]);
    REQUIRE(part_t != nullptr);
    CHECK(crd::math::to_raw_vec(part_t->translation).y == 0.5F);

    // the BAKED hierarchy: part.world = rig.world · part.local ⇒ world translation (1.0, 0.5, 0) — proves the
    // ChildOf relation cooked AND TransformPropagation ran pre-serialize (the v1l baked-path convention)
    CHECK(part_t->world.c3.x == 1.0F);
    CHECK(part_t->world.c3.y == 0.5F);
    CHECK(part_t->world.c3.z == 0.0F);

    // the drawable references the cooked MESH artifact AND its AUTHORED material, both by ResourceId (stage 4)
    const auto* mr = world.get_component<crd::scene::MeshRenderer>(inst.entities[1]);
    REQUIRE(mr != nullptr);
    CHECK(mr->mesh == ctx.id);
    CHECK(mr->material == mtl.id);

    { // the authored material carries the glTF surface verbatim
        crd::resources::OpenPbrMaterialLoader mloader;
        crd::resources::LoadContext           mctx;
        mctx.id        = mtl.id;
        mctx.bytes     = crd::containers::as_const_span(mtl.cooked_bytes);
        mctx.manager   = nullptr;
        mctx.allocator = &g_alloc;
        void* mp = mloader.load(mctx);
        REQUIRE(mp != nullptr);
        auto* pbrm = static_cast<crd::resources::OpenPbrMaterial*>(mp);
        CHECK(pbrm->params.base_color[0] == 0.5F);
        CHECK(pbrm->params.base_color[2] == 0.125F);
        CHECK(pbrm->params.roughness == 0.3F);
        CHECK(pbrm->textures.base_color.is_null()); // untextured material — every slot honestly unbound
        mloader.unload(mp);
    }

    // the punctual light, exact
    const auto* light = world.get_component<crd::scene::SceneLight>(inst.entities[2]);
    REQUIRE(light != nullptr);
    CHECK(light->type == 2U);
    CHECK(light->color.y == 0.25F);
    CHECK(light->intensity == 30.0F);
    CHECK(light->range == 8.0F);
    CHECK(light->inner_cone_rad == 0.1F);
    CHECK(light->outer_cone_rad == 0.5F);

    // the camera, exact
    const auto* cam = world.get_component<crd::scene::SceneCamera>(inst.entities[3]);
    REQUIRE(cam != nullptr);
    CHECK(cam->is_ortho == 0U);
    CHECK(cam->yfov_rad == 0.9F);
    CHECK(cam->znear == 0.5F);
    CHECK(cam->zfar == 0.0F); // absent zfar = infinite

    loader.unload(payload);

    // recook → the material AND scene sidecars replay the SAME ids (the stability contract)
    const crd::cooker::CookResult again = handler(ctx);
    REQUIRE(again.ok);
    REQUIRE(again.extra_artifacts.size() == 2U);
    CHECK(again.extra_artifacts[0].id == mtl.id);
    CHECK(again.extra_artifacts[1].id == scen.id);

    (void)fs::remove_file(fs::Path(crd::containers::StringView(src_path)));
    (void)fs::remove_file(fs::Path(crd::containers::StringView(meta_path)));
    (void)fs::remove_file(fs::Path(crd::containers::StringView("scen_rig.glb.mtl.0_steel.meta")));
    (void)fs::remove_file(fs::Path(crd::containers::StringView("scen_rig.glb.scen.meta")));
}
