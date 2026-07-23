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
#include <crd/resources/resource_manager.hpp>
#include <crd/scene/render_components.hpp>
#include <crd/scene/spatial_bvh_index.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>
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
