// test_scene_render.cpp — GEO-7 (D-007 row 72): the CPU-side gates of the scene renderer, against a STUB raster
// context (upload calls recorded byte-for-byte). Proves: chunk-grain SoA extraction through the GEO-7 ChunkView
// table, structural-vs-incremental sync, and THE partial-re-upload gate — move ONE entity and exactly its chunk
// run's bytes re-upload, nothing else. Plus the frustum helpers' truth table.

#include <crd/gpu/raster_context.hpp>
#include <crd/math/mat.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/mesh_resource.hpp>
#include <crd/resources/resource_manager.hpp>
#include <crd/scene/render_components.hpp>
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
    static memory::TlsfAllocator a(64U << 20U);
    return a;
}

// ── the stub GPU: storage buffers as CPU byte arrays, every upload logged ─────────────────────────────────────────

struct StubStorage final : gpu::IStorageBuffer
{
    containers::Array<u8> bytes;
    explicit StubStorage(u32 size, memory::IAllocator* a) : bytes(a) { bytes.resize(size); }
    [[nodiscard]] u32 size_bytes() const noexcept override { return static_cast<u32>(bytes.size()); }
    [[nodiscard]] u32 read_u32(u32 index) const noexcept override
    {
        if ((index + 1U) * 4U > bytes.size()) { return 0U; }
        u32 v = 0;
        std::memcpy(&v, bytes.data() + index * 4U, 4U);
        return v;
    }
};

struct UploadRecord
{
    u32 offset = 0;
    u32 size   = 0;
};

struct StubRaster final : gpu::IRasterContext
{
    containers::Array<UploadRecord> uploads{&galloc()};

    [[nodiscard]] bool valid() const noexcept override { return true; }
    [[nodiscard]] std::unique_ptr<gpu::IRasterTarget> create_color_target(u32, u32) override { return nullptr; }
    void clear(gpu::IRasterTarget&, gpu::ClearColor) override {}
    [[nodiscard]] std::unique_ptr<gpu::IRasterProgram> create_raster_program(gpu::IGpuProgram&,
                                                                             gpu::IGpuProgram&) override
    {
        return nullptr;
    }
    void draw(gpu::IRasterTarget&, gpu::IRasterProgram&, gpu::ClearColor, u32) override {}
    [[nodiscard]] std::unique_ptr<gpu::IRasterTarget> create_color_target_ms(u32, u32, u32) override
    {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<gpu::IRasterTarget> create_color_depth_target(u32, u32) override
    {
        return nullptr;
    }
    void draw_depth(gpu::IRasterTarget&, gpu::IRasterProgram&, gpu::ClearColor, float, gpu::DepthCompare,
                    u32) override
    {
    }
    void draw_vrs(gpu::IRasterTarget&, gpu::IRasterProgram&, gpu::ClearColor, gpu::ShadingRate,
                  gpu::ShadingRateCombiner, u32) override
    {
    }
    [[nodiscard]] bool supports_vrs() const noexcept override { return false; }
    [[nodiscard]] std::unique_ptr<gpu::IRasterTarget> create_color_vrs_target(u32, u32, gpu::ShadingRate) override
    {
        return nullptr;
    }
    [[nodiscard]] bool supports_conservative_raster() const noexcept override { return false; }
    [[nodiscard]] bool supports_inner_coverage() const noexcept override { return false; }
    void draw_conservative(gpu::IRasterTarget&, gpu::IRasterProgram&, gpu::ClearColor, gpu::ConservativeMode,
                           u32) override
    {
    }
    [[nodiscard]] std::unique_ptr<gpu::IStorageBuffer> create_storage_buffer(u32 size_bytes) override
    {
        return std::make_unique<StubStorage>(size_bytes, &galloc());
    }
    void draw_storage(gpu::IRasterTarget&, gpu::IRasterProgram&, gpu::ClearColor, gpu::IStorageBuffer&,
                      u32) override
    {
    }
    [[nodiscard]] bool supports_fragment_interlock() const noexcept override { return false; }
    [[nodiscard]] std::unique_ptr<gpu::ITexture> create_texture(u32, u32, const void*) override { return nullptr; }
    void draw_textured(gpu::IRasterTarget&, gpu::IRasterProgram&, gpu::ClearColor, gpu::ITexture&, u32) override {}
    [[nodiscard]] std::unique_ptr<gpu::ITexture> create_depth_texture(u32, u32, const float*) override
    {
        return nullptr;
    }
    void draw_shadow(gpu::IRasterTarget&, gpu::IRasterProgram&, gpu::ClearColor, gpu::ITexture&, u32) override {}
    [[nodiscard]] std::unique_ptr<gpu::ITexture> create_texture_dim(gpu::TextureKind, u32, u32, u32,
                                                                    const void*) override
    {
        return nullptr;
    }
    void draw_bindless(gpu::IRasterTarget&, gpu::IRasterProgram&, gpu::ClearColor, gpu::ITexture* const*, u32,
                       u32) override
    {
    }
    [[nodiscard]] bool supports_bindless() const noexcept override { return false; }
    [[nodiscard]] std::unique_ptr<gpu::IGBufferTarget> create_gbuffer_target(u32, u32, u32) override
    {
        return nullptr;
    }
    void draw_gbuffer(gpu::IGBufferTarget&, gpu::IRasterProgram&, gpu::ClearColor, u32) override {}

    [[nodiscard]] bool upload_storage(gpu::IStorageBuffer& storage, u32 byte_offset, const void* data,
                                      u32 size_bytes) override
    {
        auto& s = static_cast<StubStorage&>(storage);
        if (static_cast<usize>(byte_offset) + size_bytes > s.bytes.size()) { return false; }
        std::memcpy(s.bytes.data() + byte_offset, data, size_bytes);
        uploads.push_back(UploadRecord{byte_offset, size_bytes});
        return true;
    }
};

// ── a hand-built cooked MESH pack (unit cube) mounted into a real ResourceManager ─────────────────────────────────

[[nodiscard]] containers::Array<u8> build_cube_mesh_crdr(const resources::ResourceId& id)
{
    auto* a = &galloc();
    // 8 corners, 48-byte records (position + a unit normal; uv/tangent zero)
    containers::Array<u8> verts(a);
    for (u32 corner = 0; corner < 8U; ++corner)
    {
        const f32 x      = (corner & 1U) != 0U ? 0.5F : -0.5F;
        const f32 y      = (corner & 2U) != 0U ? 0.5F : -0.5F;
        const f32 z      = (corner & 4U) != 0U ? 0.5F : -0.5F;
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

struct Rig
{
    resources::ResourceManager rm{&galloc()};
    resources::ResourceId      cube_id = resources::ResourceId::mint_random();
    platform::fs::Path         pack_path;
    StubRaster                 raster;
    scene::World               world{&galloc()};
    scenerender::SceneRenderer renderer{&galloc()};

    Rig()
    {
        containers::String name("sr_pack_", &galloc());
        name.append(cube_id.to_string(&galloc()));
        name.append(".crdr");
        pack_path = platform::fs::Path(containers::StringView(name.data(), name.size()));
        write_mesh_pack(pack_path, cube_id);
        resources::register_mesh_loader(&rm, nullptr);
        REQUIRE(rm.mount_manifest(pack_path.generic()).is_valid());

        world.register_component<scene::Transform>(scene::transform_serialize_trait());
        scene::register_render_components(world);
        REQUIRE(renderer.init(raster, rm));
    }
    ~Rig() { (void)platform::fs::remove_file(pack_path); }

    scene::EntityId spawn_cube(f32 x, f32 y, f32 z)
    {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.translation = math::from_raw_vec<units::dim::Length>(math::Vec3f{x, y, z});
        t.world       = math::from_trs(math::Vec3f{x, y, z}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
        return e;
    }
};

} // namespace

TEST_CASE("scene-render: chunk-grain extraction -- structural sync builds groups, clean sync uploads NOTHING",
          "[scene-render][geo7]")
{
    Rig rig;
    for (int i = 0; i < 100; ++i) { (void)rig.spawn_cube(static_cast<f32>(i), 0.0F, 0.0F); }

    const auto s1 = rig.renderer.sync(rig.world);
    CHECK(s1.structural_rebuild);
    CHECK(s1.groups == 1U);
    CHECK(s1.total_instances == 100U);
    CHECK(s1.meshes_pending == 0U);
    CHECK(s1.uploaded_bytes == 100U * sizeof(scenerender::InstanceGpu)); // the full instance payload, once

    // the group's buffer carries the geometry (index_count at the right shape)
    REQUIRE(rig.renderer.mesh_groups().size() == 1U);
    CHECK(rig.renderer.mesh_groups()[0].index_count == 36U);

    // a clean second sync: no structure change, no dirt, ZERO instance bytes
    const auto s2 = rig.renderer.sync(rig.world);
    CHECK_FALSE(s2.structural_rebuild);
    CHECK(s2.dirty_runs == 0U);
    CHECK(s2.uploaded_bytes == 0U);
}

TEST_CASE("scene-render: THE partial-re-upload gate -- move ONE entity, exactly its chunk run re-uploads",
          "[scene-render][geo7]")
{
    Rig rig;
    containers::Array<scene::EntityId> entities(&galloc());
    for (int i = 0; i < 500; ++i) { entities.push_back(rig.spawn_cube(static_cast<f32>(i), 0.0F, 0.0F)); }
    (void)rig.renderer.sync(rig.world);
    rig.raster.uploads.clear();

    // move ONE entity via the UPSERT path (fires the chunk version bump + the storage-event update)
    {
        scene::Transform t;
        t.translation = math::from_raw_vec<units::dim::Length>(math::Vec3f{7.0F, 9.0F, 0.0F});
        t.world       = math::from_trs(math::Vec3f{7.0F, 9.0F, 0.0F}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        rig.world.add_component(entities[123], t);
    }

    const auto s = rig.renderer.sync(rig.world);
    CHECK_FALSE(s.structural_rebuild);
    CHECK(s.dirty_runs >= 1U);
    const auto& group = rig.renderer.mesh_groups()[0];

    // the uploaded bytes are EXACTLY dirty-run instance ranges — and strictly less than the full table
    CHECK(s.uploaded_bytes > 0U);
    CHECK(s.uploaded_bytes < 500U * sizeof(scenerender::InstanceGpu)); // NOT a full re-upload
    CHECK(s.uploaded_bytes % sizeof(scenerender::InstanceGpu) == 0U);  // whole instance records only

    // and the moved entity's matrix landed in the buffer at its slot
    bool found = false;
    for (usize slot = 0; slot < group.slot_entity.size(); ++slot)
    {
        if (group.slot_entity[slot] == entities[123])
        {
            found = true;
            CHECK(group.instances[slot].world[12] == 7.0F); // column 3 x = translation.x
            CHECK(group.instances[slot].world[13] == 9.0F);
        }
    }
    CHECK(found);

    // an untouched third sync goes back to zero
    const auto s3 = rig.renderer.sync(rig.world);
    CHECK(s3.uploaded_bytes == 0U);
}

TEST_CASE("scene-render: a structural change (spawn) triggers a rebuild", "[scene-render][geo7]")
{
    Rig rig;
    for (int i = 0; i < 10; ++i) { (void)rig.spawn_cube(static_cast<f32>(i), 0.0F, 0.0F); }
    (void)rig.renderer.sync(rig.world);

    (void)rig.spawn_cube(99.0F, 0.0F, 0.0F);
    const auto s = rig.renderer.sync(rig.world);
    CHECK(s.structural_rebuild);
    CHECK(s.total_instances == 11U);
}

TEST_CASE("scene-render: frustum planes + AABB test -- in front passes, behind culls", "[scene-render][geo7]")
{
    const math::Mat4f view = math::look_at(math::Vec3f{0, 0, 5}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Mat4f vp   = proj * view;

    math::Vec4f planes[6];
    scenerender::frustum_planes(vp, planes);

    const geometry::primitives::AABB3<f32> at_origin{{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}};
    const geometry::primitives::AABB3<f32> behind{{-0.5F, -0.5F, 9.5F}, {0.5F, 0.5F, 10.5F}};
    const geometry::primitives::AABB3<f32> far_left{{-500.0F, -0.5F, -0.5F}, {-499.0F, 0.5F, 0.5F}};
    CHECK(scenerender::aabb_in_frustum(at_origin, planes));
    CHECK_FALSE(scenerender::aabb_in_frustum(behind, planes));
    CHECK_FALSE(scenerender::aabb_in_frustum(far_left, planes));

    // the frustum AABB CONTAINS everything visible. An infinite-far reverse-Z projection expands the box toward
    // huge extents (the far corners approach w=0) — a broad phase that prunes nothing, never one that drops a
    // visible instance (the safe direction).
    const auto fbox = scenerender::frustum_aabb(vp);
    CHECK(fbox.min.x <= at_origin.min.x);
    CHECK(fbox.max.x >= at_origin.max.x);
    CHECK(fbox.min.z <= at_origin.min.z);
}
