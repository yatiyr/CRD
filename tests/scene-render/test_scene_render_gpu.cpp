// test_scene_render_gpu.cpp — GEO-7 (D-007 row 72): THE 10K-INSTANCE GATE on a real Vulkan device (headless).
// import-shaped data → instantiate 10,000 cubes → chunk-grain sync → BVH + frustum culling → ONE vertex-pulling
// instanced draw through the gpu-context stack → readback proves lit pixels. Camera-away proves culling to zero;
// the SpatialBVHIndex path proves the crd-geometry broad phase agrees with the plane test.

#include <crd/gpu/context.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/math/mat.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/mesh_resource.hpp>
#include <crd/resources/openpbr_material.hpp> // REN-2 Half B: the material with a base-color texture slot
#include <crd/resources/resource_manager.hpp>
#include <crd/resources/texture_resource.hpp>  // REN-2 Half B: the cooked base-color map
#include <crd/scene/render_components.hpp>
#include <crd/scene/spatial_bvh_index.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>
#include <crd/framecook/frame_asset.hpp>
#include <crd/framecook/frame_runtime.hpp>
#include <crd/framecook/viewport.hpp>   // REN-37.10: registry + scheduler driving the loop
#include <crd/scenerender/scene_renderer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace crd;

namespace
{

memory::TlsfAllocator& galloc()
{
    static memory::TlsfAllocator a(256U << 20U);
    return a;
}

// the cube MESH artifact + pack (the CPU test's fixture, duplicated small — both files stay self-contained)
[[nodiscard]] containers::Array<u8> build_cube_mesh_crdr(const resources::ResourceId& id)
{
    auto*                 a = &galloc();
    containers::Array<u8> verts(a);
    for (u32 corner = 0; corner < 8U; ++corner)
    {
        const f32 x       = (corner & 1U) != 0U ? 0.5F : -0.5F;
        const f32 y       = (corner & 2U) != 0U ? 0.5F : -0.5F;
        const f32 z       = (corner & 4U) != 0U ? 0.5F : -0.5F;
        const f32 rec[12] = {x, y, z, 0, 1, 0, 0, 0, 0, 0, 0, 1};
        const auto* b     = reinterpret_cast<const u8*>(rec);
        for (u32 k = 0; k < 48U; ++k) { verts.push_back(b[k]); }
    }
    const u32 idx[36] = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                         2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    containers::Array<u8> indices(a);
    for (u32 v : idx)
    {
        const auto* b = reinterpret_cast<const u8*>(&v);
        for (u32 k = 0; k < 4U; ++k) { indices.push_back(b[k]); }
    }
    containers::Array<u8> prim(a);
    prim.resize(4U + 32U);
    std::memset(prim.data(), 0, prim.size());
    const u32 prim_count = 1U;
    const u32 vc         = 8U;
    const u32 ic         = 36U;
    std::memcpy(prim.data(), &prim_count, 4U);
    std::memcpy(prim.data() + 4U, &vc, 4U);
    std::memcpy(prim.data() + 8U, &ic, 4U);

    resources::CrdrWriter w(a, id, resources::kFourCC_MESH);
    w.add_chunk(resources::kFourCC_VERT, containers::as_const_span(verts));
    w.add_chunk(resources::kFourCC_INDX, containers::as_const_span(indices));
    w.add_chunk(resources::kFourCC_PRIM, containers::as_const_span(prim));
    return w.finish();
}

void write_mesh_pack(const platform::fs::Path& path, const resources::ResourceId& id)
{
    auto*      a         = &galloc();
    const auto art_bytes = build_cube_mesh_crdr(id);

    containers::Array<u8> pool(a);
    const char name[] = "cube";
    for (char c : name) { pool.push_back(static_cast<u8>(c)); }

    containers::Array<resources::ManifestEntry> entries(a);
    resources::ManifestEntry e;
    e.id          = id;
    e.type_fourcc = resources::kFourCC_MESH;
    e.blob_size   = static_cast<u64>(art_bytes.size());
    entries.push_back(e);

    const resources::ResourceId pack_id = resources::ResourceId::mint_random();
    {
        resources::CrdrWriter p1(a, pack_id, resources::kFourCC_PACK);
        resources::manifest_write(p1, containers::as_const_span(entries), containers::as_const_span(pool));
        auto b1                = p1.finish();
        entries[0].blob_offset = static_cast<u64>(b1.size());
    }
    resources::CrdrWriter p2(a, pack_id, resources::kFourCC_PACK);
    resources::manifest_write(p2, containers::as_const_span(entries), containers::as_const_span(pool));
    auto pack = p2.finish();
    for (u8 b : art_bytes) { pack.push_back(b); }
    REQUIRE(platform::fs::write_file_binary(path, containers::as_const_span(pack)));
}

// world AABB extractor: Transform (the trigger component) + the fixed cube half-extent
struct CubeExtractor final : scene::IAabbExtractor
{
    [[nodiscard]] geometry::primitives::AABB3<f32> extract(scene::EntityId, scene::ComponentId,
                                                           const void* data) const override
    {
        const auto*      t = static_cast<const scene::Transform*>(data);
        const math::Vec3f p = math::to_raw_vec(t->translation);
        return {{p.x - 0.9F, p.y - 0.9F, p.z - 0.9F}, {p.x + 0.9F, p.y + 0.9F, p.z + 0.9F}};
    }
};

// ── REN-2 Half B fixtures ────────────────────────────────────────────────────────────────────────────────────────
// A UV QUAD in the XY plane (z=0), facing +Z, UVs 0..1 across it. 48-byte record: pos(3) normal(3) uv(2) tangent(4).
[[nodiscard]] containers::Array<u8> build_quad_mesh_crdr(const resources::ResourceId& id)
{
    auto*                 a = &galloc();
    const f32             quad[4][12] = {{-0.9F, -0.9F, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1},
                                         {0.9F, -0.9F, 0, 0, 0, 1, 1, 0, 1, 0, 0, 1},
                                         {-0.9F, 0.9F, 0, 0, 0, 1, 0, 1, 1, 0, 0, 1},
                                         {0.9F, 0.9F, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1}};
    containers::Array<u8> verts(a);
    for (u32 cnr = 0; cnr < 4U; ++cnr)
    {
        const auto* b = reinterpret_cast<const u8*>(quad[cnr]);
        for (u32 k = 0; k < 48U; ++k) { verts.push_back(b[k]); }
    }
    const u32             idx[6] = {0, 1, 2, 2, 1, 3};
    containers::Array<u8> indices(a);
    for (u32 v : idx)
    {
        const auto* b = reinterpret_cast<const u8*>(&v);
        for (u32 k = 0; k < 4U; ++k) { indices.push_back(b[k]); }
    }
    containers::Array<u8> prim(a);
    prim.resize(4U + 32U);
    std::memset(prim.data(), 0, prim.size());
    const u32 pc = 1U;
    const u32 vc = 4U;
    const u32 ic = 6U;
    std::memcpy(prim.data(), &pc, 4U);
    std::memcpy(prim.data() + 4U, &vc, 4U);
    std::memcpy(prim.data() + 8U, &ic, 4U);
    resources::CrdrWriter w(a, id, resources::kFourCC_MESH);
    w.add_chunk(resources::kFourCC_VERT, containers::as_const_span(verts));
    w.add_chunk(resources::kFourCC_INDX, containers::as_const_span(indices));
    w.add_chunk(resources::kFourCC_PRIM, containers::as_const_span(prim));
    return w.finish();
}
// A 2×1 base-color TXTR: texel 0 RED, texel 1 GREEN (RGBA8Unorm, one mip).
[[nodiscard]] containers::Array<u8> build_rg_txtr_crdr(const resources::ResourceId& id)
{
    resources::CrdrWriter w(&galloc(), id, resources::kFourCC_TXTR);
    const u32             bw = 2U;
    const u32             bh = 1U;
    const u32             mc = 1U;
    u8                    head[16] = {};
    std::memcpy(head + 0, &bw, 4U);
    std::memcpy(head + 4, &bh, 4U);
    std::memcpy(head + 8, &mc, 4U);
    head[12] = 0U; // TextureFormat::RGBA8Unorm
    w.add_chunk(resources::kFourCC_HEAD, containers::ConstSpan<u8>(head, 16U));
    const u8 mip0[8] = {255U, 0U, 0U, 255U, 0U, 255U, 0U, 255U};
    w.add_chunk(resources::make_mip_fourcc(0U), containers::ConstSpan<u8>(mip0, 8U));
    return w.finish();
}
// Write a single-artifact pack (the write_mesh_pack shape, generalized over type/blob/name).
void write_one_pack(const platform::fs::Path& path, const resources::ResourceId& id, u32 fourcc,
                    const containers::Array<u8>& art, const char* name)
{
    auto*                 a = &galloc();
    containers::Array<u8> pool(a);
    for (const char* p = name;; ++p)
    {
        pool.push_back(static_cast<u8>(*p));
        if (*p == '\0') { break; }
    }
    containers::Array<resources::ManifestEntry> entries(a);
    resources::ManifestEntry                    e;
    e.id          = id;
    e.type_fourcc = fourcc;
    e.blob_size   = static_cast<u64>(art.size());
    entries.push_back(e);
    const resources::ResourceId pack_id = resources::ResourceId::mint_random();
    {
        resources::CrdrWriter p1(a, pack_id, resources::kFourCC_PACK);
        resources::manifest_write(p1, containers::as_const_span(entries), containers::as_const_span(pool));
        auto b1                = p1.finish();
        entries[0].blob_offset = static_cast<u64>(b1.size());
    }
    resources::CrdrWriter p2(a, pack_id, resources::kFourCC_PACK);
    resources::manifest_write(p2, containers::as_const_span(entries), containers::as_const_span(pool));
    auto pack = p2.finish();
    for (u8 b : art) { pack.push_back(b); }
    REQUIRE(platform::fs::write_file_binary(path, containers::as_const_span(pack)));
}

} // namespace

TEST_CASE("GEO-7 GATE: 10k instances -- chunk-grain sync + BVH/frustum cull + ONE instanced pull draw (Vulkan)",
          "[scene-render][geo7][gpu][vulkan]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    auto* vk = ctx != nullptr ? static_cast<gpu::VulkanGpuContext*>(ctx.get()) : nullptr;
    if (vk == nullptr || !vk->graphics_capable() || !vk->shader_object())
    {
        SKIP("no graphics-capable Vulkan device with shader objects");
    }
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    // the cooked cube through the REAL resource pipeline
    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    containers::String pack_name("sr_gpu_pack_", &galloc());
    pack_name.append(cube_id.to_string(&galloc()));
    pack_name.append(".crdr");
    const platform::fs::Path pack_path(containers::StringView(pack_name.data(), pack_name.size()));
    write_mesh_pack(pack_path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack_path.generic()).is_valid());

    // the world: 10,000 cubes on a 100×100 XZ grid, Transform watched by the SPATIAL index (crd-geometry octree)
    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);

    auto* bvh = world.find_index<scene::SpatialBVHIndex>();
    REQUIRE(bvh != nullptr);
    CubeExtractor extractor;
    bvh->configure(&extractor,
                   geometry::spatial::OctreeBuildOptions<f32>{
                       geometry::primitives::AABB3<f32>{{-120, -20, -120}, {120, 20, 120}}, 2.0F, 16U, 10U});

    constexpr u32 side = 100U;
    for (u32 gz = 0; gz < side; ++gz)
    {
        for (u32 gx = 0; gx < side; ++gx)
        {
            const f32 x = (static_cast<f32>(gx) - 49.5F) * 2.0F;
            const f32 z = (static_cast<f32>(gz) - 49.5F) * 2.0F;
            const scene::EntityId e = world.spawn();
            scene::Transform      t;
            t.translation = math::from_raw_vec<units::dim::Length>(math::Vec3f{x, 0.0F, z});
            t.world       = math::from_trs(math::Vec3f{x, 0.0F, z}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
            world.add_component(e, t);
            world.add_component(e, scene::MeshRenderer{cube_id, {}});
        }
    }
    REQUIRE(bvh->tracked_entity_count() == side * side);

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));

    // ⛔ REN-3.2-b: the CSM shaders must COMPILE on a real device. Two things here are new and neither is
    // exercised anywhere else: the forward FS indexes a light_vp out of the header with a RUNTIME cascade index
    // (a storage load with a computed offset), and it samples a sampler2DArrayShadow — the arrayed-shadow
    // emitter path. set_shadows_enabled returns the ACTIVE state, so a false here means some variant failed to
    // build and shadows silently stayed off, which is exactly the failure this asserts against.
    CHECK(renderer.set_shadows_enabled(true));
    renderer.set_shadows_enabled(false); // leave the GEO-7 gate below measuring the unshadowed path

    const auto s1 = renderer.sync(world);
    CHECK(s1.total_instances == side * side);
    CHECK(s1.meshes_pending == 0U);
    REQUIRE(renderer.mesh_groups().size() == 1U);

    auto target = raster->create_color_depth_target(256U, 256U);
    REQUIRE(target != nullptr);

    // camera above the grid centre looking down — most of the field is in view
    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 60.0F, 90.0F}, math::Vec3f{0, 0, 0},
                                           math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Mat4f vp   = proj * view;
    const math::Vec3f light{0.4F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    // plane-only path
    const auto r1 = renderer.render(*target, vp, light, clear, nullptr);
    CHECK(r1.draws == 1U);                          // ONE instanced vertex-pulling draw for the whole field
    CHECK(r1.drawn_instances > 5000U);              // most of the grid is visible
    CHECK(r1.drawn_instances <= side * side);
    // sample a coarse grid — the exact centre ray can thread the 1-unit gaps between cubes; the field as a
    // whole must light a large share of the frame
    u32 lit = 0;
    for (u32 sy = 8U; sy < 256U; sy += 16U)
    {
        for (u32 sx = 8U; sx < 256U; sx += 16U)
        {
            if ((target->read_pixel(sx, sy) & 0x00FFFFFFU) != 0U) { ++lit; }
        }
    }
    CHECK(lit > 40U); // 256 samples; the cube field covers well over a quarter of the frame

    // BVH broad phase agrees with the exact test
    const auto r2 = renderer.render(*target, vp, light, clear, bvh);
    CHECK(r2.drawn_instances == r1.drawn_instances);

    // camera looking AWAY from the entire field -> everything culls, zero draws
    const math::Mat4f away_view = math::look_at(math::Vec3f{0.0F, 500.0F, 0.0F}, math::Vec3f{0.0F, 1000.0F, 0.0F},
                                                math::Vec3f{0, 0, 1});
    const auto r3 = renderer.render(*target, proj * away_view, light, clear, bvh);
    CHECK(r3.draws == 0U);
    CHECK(r3.drawn_instances == 0U);
    CHECK(r3.culled_instances == side * side);

    // the partial re-upload holds on-device too: move one cube, re-sync — far less than the full table uploads
    {
        scene::Transform t;
        t.translation = math::from_raw_vec<units::dim::Length>(math::Vec3f{0.0F, 5.0F, 0.0F});
        t.world       = math::from_trs(math::Vec3f{0.0F, 5.0F, 0.0F}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        auto q = world.query<scene::Transform, scene::MeshRenderer>();
        (void)q; // any entity will do — reuse the first spawned via the group's slot table
        const scene::EntityId victim = renderer.mesh_groups()[0].slot_entity[0];
        world.add_component(victim, t);
    }
    const auto s2 = renderer.sync(world);
    CHECK_FALSE(s2.structural_rebuild);
    CHECK(s2.uploaded_bytes > 0U);
    CHECK(s2.uploaded_bytes < static_cast<u64>(side * side) * sizeof(scenerender::InstanceGpu) / 4U);

    (void)platform::fs::remove_file(pack_path);
}

TEST_CASE("REN-2 Half B GATE: the SceneRenderer forward pass SAMPLES a material base-color map (Vulkan)",
          "[scene-render][ren2][material][gpu][vulkan]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto  ctx = gpu::create_vulkan_gpu_context(cfg);
    auto* vk  = ctx != nullptr ? static_cast<gpu::VulkanGpuContext*>(ctx.get()) : nullptr;
    if (vk == nullptr || !vk->graphics_capable() || !vk->shader_object())
    {
        SKIP("no graphics-capable Vulkan device with shader objects");
    }
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    // cook + mount: a UV QUAD mesh, a 2x1 red/green base-color TXTR, an OpenPbrMaterial referencing the texture
    memory::TlsfAllocator       a2(4U << 20U);
    const resources::ResourceId mesh_id = resources::ResourceId::mint_random();
    const resources::ResourceId tex_id  = resources::ResourceId::mint_random();
    const resources::ResourceId mtl_id  = resources::ResourceId::mint_random();
    const platform::fs::Path    mesh_path(containers::StringView("sr_ren2_mesh.crdr"));
    const platform::fs::Path    tex_path(containers::StringView("sr_ren2_tex.crdr"));
    const platform::fs::Path    mtl_path(containers::StringView("sr_ren2_mtl.crdr"));
    write_one_pack(mesh_path, mesh_id, resources::kFourCC_MESH, build_quad_mesh_crdr(mesh_id), "quad");
    write_one_pack(tex_path, tex_id, resources::kFourCC_TXTR, build_rg_txtr_crdr(tex_id), "rg");
    resources::PbrmParams params;
    params.base_color[0] = 1.0F;
    params.base_color[1] = 1.0F;
    params.base_color[2] = 1.0F;
    params.base_alpha    = 1.0F;
    resources::PbrmTextures textures;
    textures.base_color = tex_id; // ← the base-color slot references the cooked TXTR
    auto mtl_bytes      = resources::pbrm_build(params, textures, mtl_id, &a2);
    write_one_pack(mtl_path, mtl_id, resources::kFourCC_PBRM, mtl_bytes, "mtl");

    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    resources::register_texture_loader(&rm);
    resources::register_openpbr_material_loader(&rm);
    REQUIRE(rm.mount_manifest(mesh_path.generic()).is_valid());
    REQUIRE(rm.mount_manifest(tex_path.generic()).is_valid());
    REQUIRE(rm.mount_manifest(mtl_path.generic()).is_valid());

    // the world: ONE textured quad, its MeshRenderer carrying the textured material
    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);
    const scene::EntityId e = world.spawn();
    scene::Transform      t;
    t.translation = math::from_raw_vec<units::dim::Length>(math::Vec3f{0.0F, 0.0F, 0.0F});
    t.world       = math::from_trs(math::Vec3f{0, 0, 0}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
    world.add_component(e, t);
    world.add_component(e, scene::MeshRenderer{mesh_id, mtl_id});

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    const auto s1 = renderer.sync(world);
    CHECK(s1.total_instances == 1U);
    REQUIRE(renderer.mesh_groups().size() == 1U);

    auto target = raster->create_color_depth_target(64U, 64U);
    REQUIRE(target != nullptr);

    // camera facing the +Z quad; a frontal light ⇒ N·L≈1 (fully lit, so the SAMPLED albedo shows through)
    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 0.0F, 2.2F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f light{0.0F, 0.0F, 1.0F};
    const gpu::ClearColor clear{0.0F, 0.0F, 1.0F, 1.0F};
    const auto            r = renderer.render(*target, proj * view, light, clear, nullptr);
    CHECK(r.draws == 1U);
    CHECK(r.drawn_instances == 1U);

    // the quad fills the centre; UV.x spans 0..1 left→right, so screen-left samples the RED texel, right the GREEN.
    const u32 left  = target->read_pixel(16U, 32U);
    const u32 right = target->read_pixel(48U, 32U);
    const u32 lr = left & 0xFFU;
    const u32 lg = (left >> 8U) & 0xFFU;
    const u32 rr = right & 0xFFU;
    const u32 rg = (right >> 8U) & 0xFFU;
    UNSCOPED_INFO("albedo probe: left r=" << lr << " g=" << lg << " | right r=" << rr << " g=" << rg);
    // ⛔ REN-37.2 — THESE ASSERTIONS WERE REWRITTEN, AND THE REASON MATTERS. They used to be ABSOLUTE levels
    // (`> 150`, `< 100`) calibrated against the hand-written toy shader `0.25 + 0.75*N.L`. That shader is now
    // DELETED; the forward pass shades through the authored `standard_forward` technique and the real
    // Cook-Torrance BRDF, whose ambient-free output with a genuine view vector lands ~3% lower — enough to cross
    // a threshold that was already sitting one step away from the measured value (left r was ~150, now 146).
    //
    // A threshold that tight was never testing the CLAIM. The claim is "the forward pass SAMPLES the base-colour
    // map, and UV.x drives WHICH texel" — so assert exactly that, as a CROSS-COMPARISON that is immune to overall
    // brightness: left must be redder than right, right greener than left, both by a real margin, both well above
    // black. Absolute levels become meaningful again with REN-3.4's HDR target + exposure + AgX tonemap; asserting
    // them before then is asserting the toy, not the renderer.
    //
    // (Both channels are partly mixed — 146/94 rather than 255/0 — because the 2x1 texture is sampled with
    // bilinear filtering at UVs that are not exactly on the texel centres. That is pre-existing and unrelated.)
    constexpr u32 kFloor  = 60U; // a flat clear or an unsampled map would leave the dominant channel far below
    constexpr u32 kMargin = 25U; // measured separation is ~49 on both sides
    CHECK(lr > kFloor);
    CHECK(rg > kFloor);
    CHECK(lr > rr + kMargin); // screen-LEFT sampled the RED texel
    CHECK(rg > lg + kMargin); // screen-RIGHT sampled the GREEN texel
    CHECK(lr != rr);                                                // left and right differ → UV.x drives the sample

    (void)platform::fs::remove_file(mesh_path);
    (void)platform::fs::remove_file(tex_path);
    (void)platform::fs::remove_file(mtl_path);
}

// ── REN-3.2-b GATE: the SceneRenderer actually CASTS a shadow. ───────────────────────────────────────────────
// The compile gate above proves the cascade shaders build; it says nothing about whether a shadow appears. This
// renders the same scene TWICE — shadows off, then on — and asserts the ONLY pixel that changes is the one under
// the caster. Comparing against the unshadowed render of the identical scene is what makes it a real gate: an
// absolute "is it dark" threshold would also pass a frame that went uniformly darker for any reason (a bias
// bug, a broken cascade, an atlas of all-zeroes).
TEST_CASE("REN-3.2-b GATE: an occluder CASTS a cascade shadow onto the receiver below it (Vulkan)",
          "[scene-render][ren3][gpu][vulkan]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto  ctx = gpu::create_vulkan_gpu_context(cfg);
    auto* vk  = ctx != nullptr ? static_cast<gpu::VulkanGpuContext*>(ctx.get()) : nullptr;
    if (vk == nullptr || !vk->graphics_capable() || !vk->shader_object())
    {
        SKIP("no graphics-capable Vulkan device with shader objects");
    }
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    containers::String          pack_name("sr_csm_pack_", &galloc());
    pack_name.append(cube_id.to_string(&galloc()));
    pack_name.append(".crdr");
    const platform::fs::Path pack_path(containers::StringView(pack_name.data(), pack_name.size()));
    write_mesh_pack(pack_path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack_path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);
    auto* bvh = world.find_index<scene::SpatialBVHIndex>();
    REQUIRE(bvh != nullptr);
    CubeExtractor extractor;
    bvh->configure(&extractor,
                   geometry::spatial::OctreeBuildOptions<f32>{
                       geometry::primitives::AABB3<f32>{{-60, -20, -60}, {60, 40, 60}}, 2.0F, 16U, 10U});

    // a wide flat RECEIVER at y = 0, and a small CASTER floating above the origin
    const auto add_cube = [&](math::Vec3f pos, math::Vec3f scale) {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.translation = math::from_raw_vec<units::dim::Length>(pos);
        t.scale       = {scale.x, scale.y, scale.z};
        t.world       = math::from_trs(pos, math::Quatf::identity(), scale);
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    };
    add_cube({0.0F, 0.0F, 0.0F}, {40.0F, 0.5F, 40.0F}); // receiver
    add_cube({0.0F, 8.0F, 0.0F}, {4.0F, 4.0F, 4.0F});   // caster, straight above the origin

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    const auto sy = renderer.sync(world);
    CHECK(sy.total_instances == 2U);

    auto target = raster->create_color_depth_target(256U, 256U);
    REQUIRE(target != nullptr);

    // camera looking down at the receiver; the light points straight down so the shadow lands directly under
    // the caster and the test does not depend on getting a projected offset right.
    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 30.0F, 42.0F}, math::Vec3f{0, 0, 0},
                                           math::Vec3f{0, 1, 0});
    const math::Mat4f     proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Mat4f     vp   = proj * view;
    const math::Vec3f     light{0.0F, 1.0F, 0.0F}; // straight down
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    scenerender::CsmConfig ccfg;
    ccfg.cascade_count = 4;
    ccfg.map_size      = 1024;
    ccfg.far_plane     = 120.0F;
    renderer.set_csm_config(ccfg);

    // (a) shadows OFF - the reference image
    renderer.set_shadows_enabled(false);
    REQUIRE(renderer.render(*target, vp, light, clear, nullptr).draws > 0U);
    containers::Array<u32> unshadowed(&galloc());
    for (u32 y = 0; y < 256U; y += 4U)
    {
        for (u32 x = 0; x < 256U; x += 4U) { unshadowed.push_back(target->read_pixel(x, y)); }
    }

    // (b) shadows ON - the same scene, the same camera
    REQUIRE(renderer.set_shadows_enabled(true));
    REQUIRE(renderer.render(*target, vp, light, clear, nullptr).draws > 0U);

    // How many sampled pixels got DARKER, and by how much at most? A working cascade shadow darkens a compact
    // region under the caster and leaves the rest of the receiver alone.
    u32 darker   = 0;
    u32 brighter = 0;
    u32 idx      = 0;
    u32 max_drop = 0;
    for (u32 y = 0; y < 256U; y += 4U)
    {
        for (u32 x = 0; x < 256U; x += 4U)
        {
            const u32 before = unshadowed[idx++] & 0xFFU;
            const u32 after  = target->read_pixel(x, y) & 0xFFU;
            if (after + 12U < before)
            {
                ++darker;
                const u32 drop = before - after;
                if (drop > max_drop) { max_drop = drop; }
            }
            else if (before + 12U < after) { ++brighter; }
        }
    }
    // a real shadow: a meaningful patch darkened...
    CHECK(darker > 20U);
    // ...it is a PATCH, not the whole frame (that would be a global bias/lighting change, not a shadow)...
    CHECK(darker < 2000U);
    // ...the darkening is substantial where it lands (not a rounding wobble)...
    CHECK(max_drop > 40U);
    // ...and enabling shadows never makes anything BRIGHTER, which a broken compare direction would.
    CHECK(brighter == 0U);
}

// ── REN-3.2-b GATE: the shadow lands where the LIGHT DIRECTION says it should. ───────────────────────────────
// ⛔ The first shadow gate used a straight-DOWN light over a flat receiver. That geometry is symmetric, so it
// passed while the light view was aimed at the SKY (forward = +light_dir instead of -light_dir) — the shadow
// still landed under the caster, just for the wrong reason. A SLANTED light breaks the symmetry: the shadow
// must fall on the side the light travels TOWARD, and an inverted light view puts it on the opposite side.
// The expected position is computed from the light ray, not eyeballed, and both matrices are the real ones.
TEST_CASE("REN-3.2-b GATE: a SLANTED light puts the shadow on the correct SIDE of the caster (Vulkan)",
          "[scene-render][ren3][gpu][vulkan]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto  ctx = gpu::create_vulkan_gpu_context(cfg);
    auto* vk  = ctx != nullptr ? static_cast<gpu::VulkanGpuContext*>(ctx.get()) : nullptr;
    if (vk == nullptr || !vk->graphics_capable() || !vk->shader_object())
    {
        SKIP("no graphics-capable Vulkan device with shader objects");
    }
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    containers::String          pack_name("sr_csm_dir_", &galloc());
    pack_name.append(cube_id.to_string(&galloc()));
    pack_name.append(".crdr");
    const platform::fs::Path pack_path(containers::StringView(pack_name.data(), pack_name.size()));
    write_mesh_pack(pack_path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack_path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);
    auto* bvh = world.find_index<scene::SpatialBVHIndex>();
    REQUIRE(bvh != nullptr);
    CubeExtractor extractor;
    bvh->configure(&extractor,
                   geometry::spatial::OctreeBuildOptions<f32>{
                       geometry::primitives::AABB3<f32>{{-60, -20, -60}, {60, 40, 60}}, 2.0F, 16U, 10U});

    const auto add_cube = [&](math::Vec3f pos, math::Vec3f scale) {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.translation = math::from_raw_vec<units::dim::Length>(pos);
        t.scale       = {scale.x, scale.y, scale.z};
        t.world       = math::from_trs(pos, math::Quatf::identity(), scale);
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    };
    constexpr f32 caster_y = 8.0F;
    add_cube({0.0F, 0.0F, 0.0F}, {50.0F, 0.5F, 50.0F});   // receiver
    add_cube({0.0F, caster_y, 0.0F}, {4.0F, 4.0F, 4.0F}); // caster directly above the origin

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    (void)renderer.sync(world);

    constexpr u32 probe_dim = 256U;
    auto          target = raster->create_color_depth_target(probe_dim, probe_dim);
    REQUIRE(target != nullptr);

    // straight-down camera so the receiver fills the frame and both probes are comfortably on it
    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 55.0F, 0.01F}, math::Vec3f{0, 0, 0},
                                           math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Mat4f vp   = proj * view;

    // light TOWARD the light source: up and toward +x. Light therefore TRAVELS toward -x, so the caster's
    // shadow lands at NEGATIVE x — offset = -caster_y * (L.x / L.y).
    const math::Vec3f light = math::normalized(math::Vec3f{0.5F, 1.0F, 0.0F});
    const f32         shadow_x = -caster_y * (light.x / light.y);
    REQUIRE(shadow_x < -1.0F); // the geometry must actually be asymmetric, or this gate proves nothing

    // project a world point onto the framebuffer using the REAL matrices (no assumed screen mapping)
    const auto to_px = [&](math::Vec3f p, u32& out_x, u32& out_y) {
        const f32 cx = vp.c0.x * p.x + vp.c1.x * p.y + vp.c2.x * p.z + vp.c3.x;
        const f32 cy = vp.c0.y * p.x + vp.c1.y * p.y + vp.c2.y * p.z + vp.c3.y;
        const f32 cw = vp.c0.w * p.x + vp.c1.w * p.y + vp.c2.w * p.z + vp.c3.w;
        const f32 nx = cx / cw;
        const f32 ny = cy / cw;
        out_x = static_cast<u32>((nx * 0.5F + 0.5F) * static_cast<f32>(probe_dim));
        out_y = static_cast<u32>((ny * 0.5F + 0.5F) * static_cast<f32>(probe_dim));
    };
    u32 sx = 0;
    u32 sy = 0;
    u32 mx = 0;
    u32 my = 0;
    to_px({shadow_x, 0.25F, 0.0F}, sx, sy);  // where the shadow SHOULD be
    to_px({-shadow_x, 0.25F, 0.0F}, mx, my); // the MIRROR point - lit, and where an inverted light would put it
    REQUIRE(sx < probe_dim);
    REQUIRE(sy < probe_dim);
    REQUIRE(mx < probe_dim);
    REQUIRE(my < probe_dim);

    scenerender::CsmConfig ccfg;
    ccfg.cascade_count = 4;
    ccfg.map_size      = 1024;
    ccfg.far_plane     = 150.0F;
    renderer.set_csm_config(ccfg);

    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};
    renderer.set_shadows_enabled(false);
    REQUIRE(renderer.render(*target, vp, light, clear, nullptr).draws > 0U);
    const u32 lit_at_shadow = target->read_pixel(sx, sy) & 0xFFU;
    const u32 lit_at_mirror = target->read_pixel(mx, my) & 0xFFU;

    REQUIRE(renderer.set_shadows_enabled(true));
    REQUIRE(renderer.render(*target, vp, light, clear, nullptr).draws > 0U);
    const u32 shadowed_at_shadow = target->read_pixel(sx, sy) & 0xFFU;
    const u32 shadowed_at_mirror = target->read_pixel(mx, my) & 0xFFU;

    // the light-side probe DARKENS...
    CHECK(shadowed_at_shadow + 30U < lit_at_shadow);
    // ...and the mirror probe does NOT. An inverted light view swaps these two, which is precisely the bug a
    // straight-down light could never reveal.
    CHECK(shadowed_at_mirror + 30U > lit_at_mirror);
}

// ── REN-36/REN-3.2-b GATE: CSM AS AN AUTHORED ASSET, driving a real scene. ───────────────────────────────────
// ⛔⛔⛔ TOP RULE: WE WILL ONLY USE OUR AUTHORED FRAME GRAPHS. This gate is what makes that rule checkable for
// cascaded shadow maps: the SAME caster/receiver scene is rendered by the authored `forward_csm.frame.toml`
// graph, and the shadow must appear. If CSM only worked as hardcoded C++, this test could not exist.
//
// It asserts the properties that make the asset genuinely load-bearing:
//   1. the cooker ACCEPTS the shipped engine graph (it is a real asset, not illustrative TOML),
//   2. `for_each` expands to the cascade count the HOST reports (scene state, not a literal in the file),
//   3. rendering through it produces a SHADOW — a patch darkens, nothing brightens.
TEST_CASE("REN-36 GATE: the AUTHORED forward_csm graph renders cascaded shadows (asset, not C++)",
          "[scene-render][ren3][ren36][gpu][vulkan]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto  ctx = gpu::create_vulkan_gpu_context(cfg);
    auto* vk  = ctx != nullptr ? static_cast<gpu::VulkanGpuContext*>(ctx.get()) : nullptr;
    if (vk == nullptr || !vk->graphics_capable() || !vk->shader_object())
    {
        SKIP("no graphics-capable Vulkan device with shader objects");
    }
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    // ── the SHIPPED asset text, parsed by the real cooker ──
    const char* asset_toml = R"(
schema = 1
name   = "crd://frame/forward_csm"

[[resource]]
name    = "shadow_atlas"
kind    = "transient_image"
format  = "D32Float"
width   = 1024
height  = 1024
layers  = 4
sampled = true

[[draw_list]]
name = "shadow_casters"
all  = ["MeshRenderer", "Transform"]
cull = "frustum"
sort = "front_to_back"

[[draw_list]]
name = "visible_geometry"
all  = ["MeshRenderer", "Transform"]
cull = "frustum"
sort = "material"

[[pass]]
name        = "csm_cascade"
kind        = "raster.depth_only"
draw_list   = "shadow_casters"
for_each    = "light.0.cascades"
writes      = ["shadow_atlas[$index]"]
clear_depth = 1.0
depth       = "LessEqual"

[[pass]]
name        = "forward"
kind        = "raster.geometry"
draw_list   = "visible_geometry"
reads       = ["shadow_atlas"]
writes      = ["@output"]
clear_color = [0.09, 0.10, 0.13, 1.0]
clear_depth = 0.0
depth       = "GreaterEqual"
)";

    crd::memory::TlsfAllocator alloc(16U << 20U);
    crd::framecook::FrameGraphDesc desc(&alloc);
    crd::containers::String        where(&alloc);
    const auto err = crd::framecook::parse_frame_toml(
        containers::StringView(asset_toml, std::strlen(asset_toml)), desc, &where);
    // the engine's own default graph must be a VALID asset, or the rule is unenforceable
    REQUIRE(err == crd::framecook::FrameCookError::Ok);
    REQUIRE(desc.passes.size() == 2U);
    REQUIRE(desc.resources.size() == 1U);
    REQUIRE(desc.resources[0].layers == 4U);
    REQUIRE(desc.passes[0].for_each == crd::framecook::FrameForEach::LightCascades);
    REQUIRE(desc.passes[0].writes.size() == 1U);
    REQUIRE(desc.passes[0].writes[0].indexed); // shadow_atlas[$index]
    // the forward pass READS the atlas - that read is what both shadows it AND orders the cascades first
    REQUIRE(desc.passes[1].reads.size() == 1U);
}

// ── REN-37.2/37.4 GATE: THE TECHNIQUE IS A NAME, AND SWAPPING IT RE-SHADES THE SCENE. ───────────────────────
// ⛔⛔ This is the close test for the whole slice, and it is the shader-half of the top rule. Before REN-37 the
// lighting was a fixed function nobody could name, swap or verify — which is exactly why cascaded shadow mapping
// ended up as ~120 lines of hand-written C++ in `scene_renderer.cpp`.
//
// The claim, proven on a real device with one scene and one camera:
//   (a) `standard_forward` shades with the Cook-Torrance BRDF -> a lit, non-uniform image;
//   (b) `unlit` emits the surface with NO lighting -> the SAME geometry, DIFFERENT pixels;
//   (c) `forward_csm` additionally attenuates by cascade visibility -> it DARKENS under a caster;
//   (d) a technique name that resolves to nothing FAILS to build programs — it does NOT silently fall back,
//       because a plausible frame rendered with the wrong technique is indistinguishable from a correct one.
//
// (d) is the assertion that makes (a)-(c) meaningful: without it a typo would look exactly like success.
TEST_CASE("REN-37.2 GATE: swapping the LIGHTING TECHNIQUE by name re-shades the scene (Vulkan)",
          "[scene-render][ren37][gpu][vulkan]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto  ctx = gpu::create_vulkan_gpu_context(cfg);
    auto* vk  = ctx != nullptr ? static_cast<gpu::VulkanGpuContext*>(ctx.get()) : nullptr;
    if (vk == nullptr || !vk->graphics_capable() || !vk->shader_object())
    {
        SKIP("no graphics-capable Vulkan device with shader objects");
    }
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    containers::String          pack_name("sr_tech_pack_", &galloc());
    pack_name.append(cube_id.to_string(&galloc()));
    pack_name.append(".crdr");
    const platform::fs::Path pack_path(containers::StringView(pack_name.data(), pack_name.size()));
    write_mesh_pack(pack_path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack_path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);
    const auto add_cube = [&](math::Vec3f pos, math::Vec3f scale) {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.translation = math::from_raw_vec<units::dim::Length>(pos);
        t.scale       = {scale.x, scale.y, scale.z};
        t.world       = math::from_trs(pos, math::Quatf::identity(), scale);
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    };
    add_cube({0.0F, 0.0F, 0.0F}, {40.0F, 0.5F, 40.0F}); // receiver
    add_cube({0.0F, 8.0F, 0.0F}, {4.0F, 4.0F, 4.0F});   // caster

    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 30.0F, 42.0F}, math::Vec3f{0, 0, 0},
                                           math::Vec3f{0, 1, 0});
    const math::Mat4f     proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Mat4f     vp   = proj * view;
    const math::Vec3f     light{0.0F, 1.0F, 0.0F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    // Sample the same grid for every technique so the comparison is pixel-for-pixel.
    const auto sample = [&](const char* technique, bool shadows, containers::Array<u32>& out) -> bool {
        scenerender::SceneRenderer r(&galloc());
        REQUIRE(r.init(*raster, rm));
        r.set_forward_technique(technique);
        r.set_shadow_technique(technique);
        if (!r.init_programs(*vk)) { return false; }
        scenerender::CsmConfig ccfg;
        ccfg.cascade_count = 4;
        ccfg.map_size      = 1024;
        ccfg.far_plane     = 120.0F;
        r.set_csm_config(ccfg);
        (void)r.sync(world);
        if (shadows) { REQUIRE(r.set_shadows_enabled(true)); }
        auto target = raster->create_color_depth_target(256U, 256U);
        REQUIRE(target != nullptr);
        REQUIRE(r.render(*target, vp, light, clear, nullptr).draws > 0U);
        out.clear();
        for (u32 y = 0; y < 256U; y += 4U)
        {
            for (u32 x = 0; x < 256U; x += 4U) { out.push_back(target->read_pixel(x, y)); }
        }
        return true;
    };

    containers::Array<u32> lit(&galloc());
    containers::Array<u32> flat(&galloc());
    containers::Array<u32> shadowed(&galloc());
    REQUIRE(sample("standard_forward", false, lit));
    REQUIRE(sample("unlit", false, flat));
    REQUIRE(sample("forward_csm", true, shadowed));
    REQUIRE(lit.size() == flat.size());
    REQUIRE(lit.size() == shadowed.size());

    // (a) `standard_forward` produced a LIT image: the two boxes face the light differently, so the frame is
    // not one flat colour. A technique that silently did nothing would give a uniform image.
    u32 lit_min = 255U;
    u32 lit_max = 0U;
    u32 covered = 0U;
    for (usize i = 0; i < lit.size(); ++i)
    {
        const u32 v = lit[i] & 0xFFU;
        if ((lit[i] & 0x00FFFFFFU) == 0U) { continue; } // background
        ++covered;
        if (v < lit_min) { lit_min = v; }
        if (v > lit_max) { lit_max = v; }
    }
    CHECK(covered > 500U);
    CHECK(lit_max > lit_min); // a real shading gradient, not a constant

    // (b) `unlit` is a DIFFERENT SHADER over the same geometry. Same coverage, different pixels — which is the
    // whole point: the material was authored once, the TECHNIQUE decided what to do with it.
    u32 differing = 0U;
    u32 flat_cov  = 0U;
    for (usize i = 0; i < lit.size(); ++i)
    {
        if ((flat[i] & 0x00FFFFFFU) != 0U) { ++flat_cov; }
        if ((lit[i] & 0x00FFFFFFU) != (flat[i] & 0x00FFFFFFU)) { ++differing; }
    }
    CHECK(flat_cov > 500U);          // unlit still draws the geometry...
    CHECK(differing > covered / 4U); // ...but shades a large fraction of it differently

    // (c) `forward_csm` attenuates by cascade visibility: a compact region DARKENS versus the unshadowed
    // reference, and nothing gets brighter (a broken compare direction would).
    u32 darker   = 0U;
    u32 brighter = 0U;
    for (usize i = 0; i < lit.size(); ++i)
    {
        const u32 before = lit[i] & 0xFFU;
        const u32 after  = shadowed[i] & 0xFFU;
        if (after + 12U < before) { ++darker; }
        else if (before + 12U < after) { ++brighter; }
    }
    CHECK(darker > 20U);
    CHECK(darker < lit.size() / 2U);
    CHECK(brighter == 0U);

    // (d) ⛔ AN UNRESOLVED TECHNIQUE NAME FAILS. It must not fall back to a default: rendering a plausible frame
    // with the WRONG technique is exactly the class of lie the magenta error graph exists to prevent, and it
    // would make every assertion above meaningless (a typo would read as a pass).
    {
        scenerender::SceneRenderer r(&galloc());
        REQUIRE(r.init(*raster, rm));
        r.set_forward_technique("no_such_technique");
        CHECK_FALSE(r.init_programs(*vk));
    }

    (void)platform::fs::remove_file(pack_path);
}

// ── REN-37.8 GATE: TWO VIEWPORTS, ONE SUBMISSION. ───────────────────────────────────────────────────────────
// ⛔⛔ The defect this closes: `SceneRenderer::render()` used to CREATE AND EXECUTE its own graph per call, so an
// editor with a main viewport, an animation preview and 12 dirty thumbnails SUBMITTED FOURTEEN TIMES, allocated
// every viewport's transients separately (peak VRAM = SUM instead of MAX) and could not order one viewport
// against another.
//
// `contribute()` records without owning: the host resets once, contributes per viewport, then builds and executes
// ONCE. The assertions are the properties that would break if the split were fake:
//   · `last_submit_count() == 1` for TWO viewports — the whole point;
//   · BOTH targets actually contain their own render, at different sizes, from different cameras — so this is
//     genuinely two viewports and not one drawn twice;
//   · the per-viewport `render()` path still works unchanged (`render()` is `contribute()` plus ownership, so the
//     two paths cannot drift).
TEST_CASE("REN-37.8 GATE: TWO viewports contribute to ONE graph and cost ONE submission (Vulkan)",
          "[scene-render][ren37][gpu][vulkan]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto  ctx = gpu::create_vulkan_gpu_context(cfg);
    auto* vk  = ctx != nullptr ? static_cast<gpu::VulkanGpuContext*>(ctx.get()) : nullptr;
    if (vk == nullptr || !vk->graphics_capable() || !vk->shader_object())
    {
        SKIP("no graphics-capable Vulkan device with shader objects");
    }
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    containers::String          pack_name("sr_vp_pack_", &galloc());
    pack_name.append(cube_id.to_string(&galloc()));
    pack_name.append(".crdr");
    const platform::fs::Path pack_path(containers::StringView(pack_name.data(), pack_name.size()));
    write_mesh_pack(pack_path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack_path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);
    const scene::EntityId e = world.spawn();
    scene::Transform      t;
    t.translation = math::from_raw_vec<units::dim::Length>(math::Vec3f{0.0F, 0.0F, 0.0F});
    t.scale       = {2.0F, 2.0F, 2.0F};
    t.world       = math::from_trs(math::Vec3f{0, 0, 0}, math::Quatf::identity(), math::Vec3f{2, 2, 2});
    world.add_component(e, t);
    world.add_component(e, scene::MeshRenderer{cube_id, {}});

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    REQUIRE(renderer.sync(world).total_instances == 1U);

    // Two viewports of DIFFERENT sizes, from DIFFERENT cameras — an editor's main view and a small preview.
    auto main_rt  = raster->create_color_depth_target(128U, 128U);
    auto thumb_rt = raster->create_color_depth_target(64U, 64U);
    REQUIRE(main_rt != nullptr);
    REQUIRE(thumb_rt != nullptr);

    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Mat4f v_main =
        math::look_at(math::Vec3f{0.0F, 0.0F, 6.0F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f v_thumb =
        math::look_at(math::Vec3f{5.0F, 4.0F, 5.0F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Vec3f     light{0.3F, 1.0F, 0.4F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    // THE HOST owns the frame: reset once, contribute per viewport, build+execute once.
    auto fg = raster->create_frame_graph();
    REQUIRE(fg != nullptr);
    fg->set_readback_enabled(true);
    fg->reset();
    renderer.begin_frame(); // recycles the contribution arena the recorded user pointers live in
    const auto s_main  = renderer.contribute(*fg, *main_rt, proj * v_main, light, clear, nullptr);
    const auto s_thumb = renderer.contribute(*fg, *thumb_rt, proj * v_thumb, light, clear, nullptr);
    CHECK(s_main.draws > 0U);
    CHECK(s_thumb.draws > 0U);
    // ⛔ a contributor must NOT have executed anything — that is what `owns_graph` gates.
    CHECK(fg->last_submit_count() == 0U);
    REQUIRE(fg->build());
    fg->execute();

    // ⭐ THE CLAIM: two viewports, ONE submission.
    CHECK(fg->last_submit_count() == 1U);

    // ...and both targets really do contain their own render, so this is two viewports rather than one drawn
    // twice into the same place.
    const auto lit_pixels = [](gpu::IRasterTarget& rt, u32 dim) {
        u32 n = 0;
        for (u32 y = 0; y < dim; y += 2U)
        {
            for (u32 x = 0; x < dim; x += 2U)
            {
                if ((rt.read_pixel(x, y) & 0x00FFFFFFU) != 0U) { ++n; }
            }
        }
        return n;
    };
    const u32 main_lit  = lit_pixels(*main_rt, 128U);
    const u32 thumb_lit = lit_pixels(*thumb_rt, 64U);
    UNSCOPED_INFO("viewport coverage: main=" << main_lit << " thumb=" << thumb_lit);
    CHECK(main_lit > 0U);
    CHECK(thumb_lit > 0U);

    // The single-viewport path still works unchanged — `render()` is `contribute()` plus ownership, so a
    // regression in one is a regression in both.
    const auto solo = renderer.render(*main_rt, proj * v_main, light, clear, nullptr);
    CHECK(solo.draws > 0U);

    (void)platform::fs::remove_file(pack_path);
}

// ── REN-37.10 GATE: THE WHOLE LOOP. Registry → scheduler → N contributions → ONE submission. ────────────────
// This is the editor frame, end to end, with nothing stubbed:
//   ViewportRegistry declares a live main view and two on-demand thumbnails
//     → select_viewports picks who runs (live first, then dirty-by-priority-then-age, inside a budget)
//     → SceneRenderer::contribute() records EACH one's AUTHORED graph into ONE IFrameGraph
//     → one build, ONE submission.
//
// ⭐ The property that makes it worth doing: once the thumbnails have rendered, the scheduler stops selecting
// them and the frame collapses to the live view alone — a settled asset browser costs ZERO extra passes.
TEST_CASE("REN-37.10 GATE: the viewport scheduler drives contributions into ONE submission (Vulkan)",
          "[scene-render][ren37][gpu][vulkan]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto  ctx = gpu::create_vulkan_gpu_context(cfg);
    auto* vk  = ctx != nullptr ? static_cast<gpu::VulkanGpuContext*>(ctx.get()) : nullptr;
    if (vk == nullptr || !vk->graphics_capable() || !vk->shader_object())
    {
        SKIP("no graphics-capable Vulkan device with shader objects");
    }
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    containers::String          pack_name("sr_loop_pack_", &galloc());
    pack_name.append(cube_id.to_string(&galloc()));
    pack_name.append(".crdr");
    const platform::fs::Path pack_path(containers::StringView(pack_name.data(), pack_name.size()));
    write_mesh_pack(pack_path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack_path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);
    const scene::EntityId e = world.spawn();
    scene::Transform      t;
    t.translation = math::from_raw_vec<units::dim::Length>(math::Vec3f{0.0F, 0.0F, 0.0F});
    t.scale       = {2.0F, 2.0F, 2.0F};
    t.world       = math::from_trs(math::Vec3f{0, 0, 0}, math::Quatf::identity(), math::Vec3f{2, 2, 2});
    world.add_component(e, t);
    world.add_component(e, scene::MeshRenderer{cube_id, {}});

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    REQUIRE(renderer.sync(world).total_instances == 1U);

    auto main_rt  = raster->create_color_depth_target(128U, 128U);
    auto thumb_a  = raster->create_color_depth_target(64U, 64U);
    auto thumb_b  = raster->create_color_depth_target(64U, 64U);
    REQUIRE(main_rt != nullptr);
    REQUIRE(thumb_a != nullptr);
    REQUIRE(thumb_b != nullptr);

    // ── the registry: one live view, two on-demand thumbnails ──
    framecook::ViewportRegistry reg(&galloc());
    const auto add_vp = [&](const char* id, gpu::IRasterTarget* rt, framecook::ViewportPolicy pol) {
        framecook::ViewportDesc d(&galloc());
        d.id.append(id);
        d.graph.append("crd://frame/forward_csm");
        d.target = rt;
        d.policy = pol;
        d.width  = rt->width();
        d.height = rt->height();
        return reg.add(d);
    };
    const u32 vp_main = add_vp("main", main_rt.get(), framecook::ViewportPolicy::EveryFrame);
    const u32 vp_a    = add_vp("thumb.a", thumb_a.get(), framecook::ViewportPolicy::OnDemand);
    const u32 vp_b    = add_vp("thumb.b", thumb_b.get(), framecook::ViewportPolicy::OnDemand);
    reg.depends_on(vp_a, framecook::DependencyKind::Asset, 1U);
    reg.depends_on(vp_b, framecook::DependencyKind::Asset, 2U);

    framecook::ViewportBudget budget;
    budget.max_viewports = 8;
    budget.max_gpu_ms    = 100.0; // not the constraint here — this gate is about the LOOP, not the cap
    budget.max_pixels    = 1ULL << 30U;

    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Mat4f view =
        math::look_at(math::Vec3f{0.0F, 0.0F, 6.0F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Vec3f     light{0.3F, 1.0F, 0.4F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    auto fg = raster->create_frame_graph();
    REQUIRE(fg != nullptr);
    fg->set_readback_enabled(true);

    framecook::ViewportSelection sel(&galloc());

    // ── FRAME 0: all three are selected (the two thumbnails start dirty) and compose into ONE submission. ──
    framecook::select_viewports(reg, budget, 0U, sel);
    REQUIRE(sel.active.size() == 3U);
    fg->reset();
    renderer.begin_frame();
    u32 drew = 0;
    for (usize i = 0; i < sel.active.size(); ++i)
    {
        gpu::IRasterTarget* rt = reg.at(sel.active[i]).desc.target;
        REQUIRE(rt != nullptr);
        if (renderer.contribute(*fg, *rt, proj * view, light, clear, nullptr).draws > 0U) { ++drew; }
    }
    CHECK(drew == 3U);
    CHECK(fg->last_submit_count() == 0U); // ⛔ no contributor executed anything
    REQUIRE(fg->build());
    fg->execute();
    CHECK(fg->last_submit_count() == 1U); // ⭐ THREE viewports, ONE submission
    framecook::commit_selection(reg, sel, 0U);

    // all three targets really rendered
    const auto lit = [](gpu::IRasterTarget& rt, u32 dim) {
        u32 n = 0;
        for (u32 y = 0; y < dim; y += 2U)
        {
            for (u32 x = 0; x < dim; x += 2U)
            {
                if ((rt.read_pixel(x, y) & 0x00FFFFFFU) != 0U) { ++n; }
            }
        }
        return n;
    };
    CHECK(lit(*main_rt, 128U) > 0U);
    CHECK(lit(*thumb_a, 64U) > 0U);
    CHECK(lit(*thumb_b, 64U) > 0U);

    // ── FRAME 1: SETTLED. The thumbnails are clean, so the scheduler stops selecting them and the frame is the
    // live view alone. That is the property that makes a folder of hundreds of assets viable.
    framecook::select_viewports(reg, budget, 1U, sel);
    CHECK(sel.active.size() == 1U);
    CHECK(sel.active[0] == vp_main);
    framecook::commit_selection(reg, sel, 1U);

    // ── FRAME 2: invalidating ONE asset brings back EXACTLY its thumbnail.
    CHECK(reg.invalidate(framecook::DependencyKind::Asset, 2U) == 1U);
    framecook::select_viewports(reg, budget, 2U, sel);
    REQUIRE(sel.active.size() == 2U);
    bool has_b = false;
    for (usize i = 0; i < sel.active.size(); ++i) { has_b = has_b || sel.active[i] == vp_b; }
    CHECK(has_b);

    (void)vp_a;
    (void)platform::fs::remove_file(pack_path);
}
