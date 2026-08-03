// test_scene_render.cpp — GEO-7 (D-007 row 72): the CPU-side gates of the scene renderer, against a STUB raster
// context (upload calls recorded byte-for-byte). Proves: chunk-grain SoA extraction through the GEO-7 ChunkView
// table, structural-vs-incremental sync, and THE partial-re-upload gate — move ONE entity and exactly its chunk
// run's bytes re-upload, nothing else. Plus the frustum helpers' truth table.

#include <crd/framecook/frame_asset.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/lightcook/lighting_asset.hpp>
#include <crd/matcook/material_asset.hpp>
#include <crd/vertexcook/vertex_asset.hpp>
#include <crd/kir/ckir.hpp> // REN-41 velocity: cook the velocity VS assets (exercise the prev:clip chain)
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

#include <cstdlib> // std::getenv - the drift gate reads the ctest-provided asset root
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
        // ⛔ In the OS temp dir, never a RELATIVE path — that resolves to wherever ctest was launched from,
        // which on a dev box is the repo checkout, and the checkout grows artifact crops .gitignore then chases.
        pack_path = platform::fs::temp_directory() / containers::StringView(name.data(), name.size());
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
    // the full instance payload, once — PLUS the per-instance world AABBs beside it.
    // ⭐⭐ REN-40-A: the GPU cull tests a BOX, so 6 floats per instance now ride the SAME dirty grain as the
    // transform they describe. ⛔ Bounds that lagged their instances would cull against LAST frame's positions —
    // geometry popping for a frame, which reads as a culling bug and is really a staleness bug. Counted here
    // explicitly rather than loosened to `> 0`: this gate's whole job is that a sync uploads EXACTLY what changed.
    constexpr crd::usize bounds_bytes = 6U * sizeof(crd::f32);
    // ⭐⭐ REN-40-C2: and the per-instance LOD OVERRIDE (2 words — the screen bias, then the level clamp), which
    // rides the SAME grain for the SAME reason: an override that lagged its entity would select against the
    // previous frame's policy. ⛔ Still counted EXACTLY rather than loosened to `> 0`. This assertion is the one
    // that caught the section being added at all (11200 vs 10400), which is precisely its job — a per-instance
    // section that appeared without anyone noticing is a per-frame cost nobody budgeted.
    constexpr crd::usize lod_override_bytes = 2U * sizeof(crd::u32);
    CHECK(s1.uploaded_bytes == 100U * (sizeof(scenerender::InstanceGpu) + bounds_bytes + lod_override_bytes));

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

// ── ⭐⭐ REN-41 audit: THE SHIPPED-ASSET COOK GATE — every authored default asset PARSES and validates. ───────
// Disk is the SINGLE SOURCE now (the in-binary pack was retired), so the old "disk == embedded pack" drift gate
// has nothing left to diff against. What still matters — and what an app authoring its own pipelines leans on —
// is that every shipped default declaration COOKS: it parses through its vocabulary, and a frame graph also
// passes the SAME structural validation the renderer runs before installing one (reads/writes resolve, no
// cycles, capability tiers well-formed). A broken default fails here by NAME rather than at first render.
//
// ⭐⭐ RAF-9 Increment 2: an engine default frame loads BY CANONICAL ENGINE:// ID through the public API — the Gate-9
// selector (no relative path, no embedded TOML). `crd://` folds to `engine://` (the identical asset resolves); a
// MISSING id REFUSES with a clear error and keeps the previous frame; a NON-frame id is rejected by type. Device-free:
// `set_frame_graph` parses + validates only (no GPU). `CRD_ASSETS_DIR` (ctest env) mounts the engine:// root.
TEST_CASE("RAF-9: an engine default frame loads by canonical engine:// id", "[scene-render][raf9]")
{
    const char* root = std::getenv("CRD_ASSETS_DIR");
    if (root == nullptr || root[0] == '\0') { SKIP("CRD_ASSETS_DIR not set (run through ctest)"); }
    memory::TlsfAllocator      alloc(16U << 20U, nullptr, "raf9-id-load");
    scenerender::SceneRenderer r(&alloc);
    REQUIRE(r.set_asset_root(root));
    CHECK(r.set_frame_graph("engine://frame/forward_csm_agx"));          // selected by canonical id
    CHECK(r.set_frame_graph("crd://frame/forward_csm_agx"));             // the alias folds to engine:// -> same asset
    CHECK_FALSE(r.set_frame_graph("engine://frame/this_default_absent")); // missing -> clear refusal, not a fallback
    CHECK_FALSE(r.set_frame_graph("engine://scene/mesh"));               // a program id, not a frame -> rejected by type
    CHECK(r.set_frame_graph_asset("frame/forward_csm_agx.frame.toml"));  // the deprecated relative wrapper still installs
    // ⭐ the PUBLIC program registry — an app registers its OWN programs by canonical id the SAME way the engine does
    // (the RAF-10 seam). A malformed id is refused.
    CHECK(r.register_raster_program("app://scene/my_shader",
                                    [](void*) -> gpu::IRasterProgram* { return nullptr; }, nullptr));
    CHECK(r.register_kernel_program("app://scene/my_kernel", [](void*) -> gpu::IGpuProgram* { return nullptr; },
                                    nullptr));
    CHECK_FALSE(r.register_raster_program("no-scheme-here", [](void*) -> gpu::IRasterProgram* { return nullptr; },
                                          nullptr));
}

// `CRD_ASSETS_DIR` is set by ctest (the guard lives in ctest, like every cross-config guard); without it the
// gate SKIPS rather than passing on nothing.
TEST_CASE("REN-41: every shipped authored default asset cooks (parses + validates)", "[scene-render][ren38][ren41]")
{
    const char* root = std::getenv("CRD_ASSETS_DIR");
    if (root == nullptr || root[0] == '\0') { SKIP("CRD_ASSETS_DIR not set (run through ctest)"); }
    memory::TlsfAllocator alloc(16U << 20U, nullptr, "ren41-cook");

    const auto read_shipped = [&](const char* rel, containers::String& out) {
        containers::String p(&alloc);
        p.append(root);
        p.append("/");
        p.append(rel);
        return platform::fs::read_file_text(platform::fs::Path(containers::StringView(p.c_str(), p.size())), out);
    };

    // frame graphs: parse through the frame vocabulary AND pass the structural validation the renderer runs.
    const auto check_frame = [&](const char* rel) {
        INFO(rel);
        containers::String shipped(&alloc);
        REQUIRE(read_shipped(rel, shipped));
        framecook::FrameGraphDesc a(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(shipped.c_str(), shipped.size()), a, &where)
                == framecook::FrameCookError::Ok);
        CHECK(framecook::validate_frame_graph(a, &where) == framecook::FrameCookError::Ok);
    };
    check_frame("frame/forward_csm.frame.toml");
    check_frame("frame/forward_basic.frame.toml");
    // the post-chain forward frames (the tonemap arms) — now TAA-carrying defaults
    check_frame("frame/forward_csm_agx.frame.toml");
    check_frame("frame/forward_csm_srgb.frame.toml");
    // the SHADOWS-OFF tiers the tonemapped pair steps down to (their `fallback` targets)
    check_frame("frame/forward_agx.frame.toml");
    check_frame("frame/forward_srgb.frame.toml");
    // ⭐⭐ REN-41: the device-cull 1M frames + their TAA — shipped defaults, so they cook here too
    check_frame("frame/forward_csm_gpu.frame.toml");
    check_frame("frame/forward_csm_gpu_srgb.frame.toml");
    // ⭐⭐ REN-41: the motion-vector debug view (the velocity correctness gate's readback surface)
    check_frame("frame/velocity_debug.frame.toml");
    // REN-38-F6: the advanced-family scene graphs
    check_frame("frame/scene_tess.frame.toml");
    check_frame("frame/scene_mesh.frame.toml");
    check_frame("frame/scene_visbuffer.frame.toml");
    check_frame("frame/scene_cull.frame.toml");
    check_frame("frame/scene_rt.frame.toml");

    const auto check_material = [&](const char* rel) {
        INFO(rel);
        containers::String shipped(&alloc);
        REQUIRE(read_shipped(rel, shipped));
        matcook::MaterialDesc a(&alloc);
        containers::String    where(&alloc);
        CHECK(matcook::parse_material_toml(containers::StringView(shipped.c_str(), shipped.size()), a, &where)
              == matcook::MaterialCookError::Ok);
    };
    check_material("material/scene.crdm");
    check_material("material/scene_textured.crdm");

    const auto check_vertex = [&](const char* rel) {
        INFO(rel);
        containers::String shipped(&alloc);
        REQUIRE(read_shipped(rel, shipped));
        vertcook::VertexProgramDesc a(&alloc);
        containers::String          where(&alloc);
        const auto rc = vertcook::parse_vertex_toml(containers::StringView(shipped.c_str(), shipped.size()), a, &where);
        INFO("rc=" << static_cast<int>(rc) << " where=" << where.c_str());
        CHECK(rc == vertcook::VertexCookError::Ok);
    };
    check_vertex("vertex/scene.crdv");
    check_vertex("vertex/scene_skinned.crdv");
    check_vertex("vertex/velocity.crdv");         // REN-41 velocity: matched motion-vector VS (non-skinned)
    check_vertex("vertex/velocity_skinned.crdv"); // REN-41 velocity: matched motion-vector VS (skinned)
    check_vertex("vertex/shadow.crdv");
    // ⭐⭐ REN-41 velocity: the velocity VS assets must not just PARSE — the prev:clip chain (re-skin with the
    // previous palette + previous transform → current view_proj) must COOK to a valid CKIR entry. Parse alone
    // never builds that graph; this does.
    const auto cook_ok = [&](const char* rel) {
        INFO(rel);
        containers::String shipped(&alloc);
        REQUIRE(read_shipped(rel, shipped));
        vertcook::VertexProgramDesc a(&alloc);
        containers::String          where(&alloc);
        REQUIRE(vertcook::parse_vertex_toml(containers::StringView(shipped.c_str(), shipped.size()), a, &where)
                == vertcook::VertexCookError::Ok);
        crd::kir::KGraph g(&alloc);
        crd::kir::KEntry ve;
        CHECK(vertcook::cook_vertex_program(a, g, ve, nullptr));
    };
    cook_ok("vertex/velocity.crdv");
    cook_ok("vertex/velocity_skinned.crdv");
    // REN-38-F6: the advanced-stage declarations
    check_vertex("vertex/tess_corners.crdv");
    check_vertex("vertex/tess_hull.crdv");
    check_vertex("vertex/tess_domain.crdv");
    check_vertex("vertex/scene_meshlet.crdv");
    check_vertex("vertex/scene_task.crdv");
    check_vertex("vertex/visbuffer_fullscreen.crdv");
    check_vertex("vertex/scene_cull.crdv");
    check_vertex("vertex/scene_cull_mark.crdv");
    check_vertex("vertex/scene_rt_raygen.crdv");
    check_vertex("vertex/scene_rt_miss.crdv");
    check_vertex("vertex/scene_rt_closest_hit.crdv");
    check_vertex("vertex/scene_rt_any_hit.crdv");
    check_vertex("vertex/post_fullscreen.crdv"); // 38-G1: the post fullscreen pair
    check_material("material/flat.crdm");

    // ── 38-G1: the POST graphs — parsed through THEIR face. ──
    const auto check_post = [&](const char* rel) {
        INFO(rel);
        containers::String shipped(&alloc);
        REQUIRE(read_shipped(rel, shipped));
        matcook::MaterialDesc a(&alloc);
        containers::String    where(&alloc);
        CHECK(matcook::parse_post_toml(containers::StringView(shipped.c_str(), shipped.size()), a, &where)
              == matcook::MaterialCookError::Ok);
    };
    check_post("post/tonemap_agx.crdp");
    check_post("post/srgb_only.crdp");

    {
        INFO("lighting/scene_forward.crdl");
        containers::String shipped(&alloc);
        REQUIRE(read_shipped("lighting/scene_forward.crdl", shipped));
        lightcook::LightingDesc a(&alloc);
        containers::String      where(&alloc);
        CHECK(lightcook::parse_lighting_toml(containers::StringView(shipped.c_str(), shipped.size()), a, &where)
              == lightcook::LightingCookError::Ok);
    }
}

// ⛔⛔ REN-38 (2026-07-27): `component_id_by_name` resolves an AUTHORED component name against the registry's
// `typeid(T).name()`, which is decorated DIFFERENTLY PER ABI. The original matcher only handled the MSVC spelling
// ("struct crd::scene::MeshRenderer" — the decorated name ENDS with the identifier), so on gcc/clang, where the
// Itanium ABI produces "N3crd5scene12MeshRendererE" with LENGTH-PREFIXED components and a trailing 'E', NOTHING
// ever matched. Every authored draw-list component filter then rejected every group SILENTLY: the renderer's
// draw list resolved empty and the frame drew nothing, on every Linux and macOS build. It presented as a Vulkan
// bug (it surfaced first on llvmpipe) and was a name-mangling bug. This gate pins BOTH spellings so the class
// cannot come back on whichever compiler the author happens not to be using.
TEST_CASE("REN-36.3-b: component_id_by_name matches BOTH ABI decorations (MSVC and Itanium)", "[scene][ecs]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    crd::scene::World          world{&alloc};
    world.register_component<crd::scene::Transform>(crd::scene::transform_serialize_trait());
    crd::scene::register_render_components(world);

    // the names an ASSET writes — resolved on whatever compiler built this
    CHECK_FALSE(world.component_id_by_name(crd::containers::StringView("Transform")).is_null());
    CHECK_FALSE(world.component_id_by_name(crd::containers::StringView("MeshRenderer")).is_null());
    // ⛔ and a filter that names something unregistered must stay NULL (the caller reports it; a null that
    // matched everything is the silently-ignored-filter shape this row exists to prevent)
    CHECK(world.component_id_by_name(crd::containers::StringView("NoSuchComponent")).is_null());
    // ⛔ a SUFFIX of a real name must not match — "Renderer" is not "MeshRenderer"
    CHECK(world.component_id_by_name(crd::containers::StringView("Renderer")).is_null());

    // the ABI-decoration matcher itself, against both real spellings, so this gate fails on the compiler that
    // does NOT produce the local decoration too
    CHECK(crd::scene::World::decorated_names(crd::containers::StringView("struct crd::scene::MeshRenderer"),
                                             crd::containers::StringView("MeshRenderer")));
    CHECK(crd::scene::World::decorated_names(crd::containers::StringView("N3crd5scene12MeshRendererE"),
                                             crd::containers::StringView("MeshRenderer")));
    CHECK(crd::scene::World::decorated_names(crd::containers::StringView("N3crd5scene9TransformE"),
                                             crd::containers::StringView("Transform")));
    // negatives under BOTH decorations
    CHECK_FALSE(crd::scene::World::decorated_names(crd::containers::StringView("N3crd5scene12MeshRendererE"),
                                                   crd::containers::StringView("Renderer")));
    CHECK_FALSE(crd::scene::World::decorated_names(crd::containers::StringView("struct crd::scene::MeshRenderer"),
                                                   crd::containers::StringView("Renderer")));
}

// ── ⭐⭐ REN-40-B GATE: THE EXTRACT WALK IS O(chunks), NOT O(entities). ──────────────────────────────────────
// ⛔ This is asserted by COUNTING, not by TIMING. The claim is asymptotic ("a static frame costs nothing"), and
// a millisecond threshold cannot express that: it is noisy, machine-specific, and on a debug build it passes or
// fails by luck. The four counters in `SyncStats` make the same claim exact.
//
// What each arm forbids, concretely — every one of these was TRUE of the code this slice replaced, and together
// they were 171 ms of a 337 ms frame at one million instances:
//   · `signature_bytes < total_instances` — the structure signature used to hash `EntityId[n]` AND
//     `MeshRenderer[n]` byte by byte for every chunk, EVERY FRAME: 40 bytes per entity, ~40 MB at 1M, to answer
//     a question that only changes when something spawns or despawns. A per-entity signature cannot consume
//     fewer than 8 bytes per entity, so this inequality is unreachable for any implementation that reads one.
//   · `entities_extracted == 0` on a static frame — nothing is re-read when nothing moved.
//   · `runs_visited == 0` on a static frame — finding a moved chunk's runs used to be a scan over every run of
//     every group (O(chunks x runs), quadratic in the scene); it is now one hash probe, so a frame with no
//     dirty chunk examines no runs at all.
//   · a ONE-entity move re-extracts ONE chunk — the cost tracks what changed, not what exists.
TEST_CASE("REN-40-B GATE: the extract is O(chunks) -- a static frame re-extracts NOTHING and hashes no entity",
          "[scene-render][ren40][geo7]")
{
    Rig rig;
    constexpr int n_total = 20000;
    containers::Array<scene::EntityId> entities(&galloc());
    for (int i = 0; i < n_total; ++i)
    {
        entities.push_back(rig.spawn_cube(static_cast<f32>(i % 100), 0.0F, static_cast<f32>(i) * 0.01F));
    }

    // ── arm 1: the structural frame pays for everything, once ──
    const auto s1 = rig.renderer.sync(rig.world);
    CHECK(s1.structural_rebuild);
    CHECK(s1.total_instances == static_cast<u32>(n_total));
    CHECK(s1.entities_extracted == static_cast<u64>(n_total)); // a rebuild DOES touch every entity — that is its job
    CHECK(s1.chunks_visited > 1U);                        // the scene must really span many chunks...
    CHECK(s1.chunks_visited < static_cast<u32>(n_total) / 8U); // ...with many entities in each, or the gate is weak

    // ── arm 2: a STATIC frame. This is the whole slice. ──
    const auto s2 = rig.renderer.sync(rig.world);
    CHECK_FALSE(s2.structural_rebuild);
    CHECK(s2.chunks_visited == s1.chunks_visited); // the walk still SEES every chunk (that cost is irreducible)
    CHECK(s2.chunks_reextracted == 0U);            // ...and re-extracts none of them
    CHECK(s2.entities_extracted == 0U);            // ⛔ not one entity read
    CHECK(s2.runs_visited == 0U);                  // ⛔ not one run scanned
    CHECK(s2.dirty_runs == 0U);
    CHECK(s2.uploaded_bytes == 0U);
    // ⛔ THE DISCRIMINATOR: the signature is O(1) per CHUNK. Any per-entity signature fails both of these.
    CHECK(s2.signature_bytes <= 64U * static_cast<u64>(s2.chunks_visited));
    CHECK(s2.signature_bytes < static_cast<u64>(s2.total_instances));

    // ── arm 3: move exactly ONE entity — exactly ONE chunk re-extracts ──
    {
        scene::Transform t;
        t.translation = math::from_raw_vec<units::dim::Length>(math::Vec3f{3.0F, 5.0F, 7.0F});
        t.world       = math::from_trs(math::Vec3f{3.0F, 5.0F, 7.0F}, math::Quatf::identity(),
                                       math::Vec3f{1, 1, 1});
        rig.world.add_component(entities[n_total / 2], t);
    }
    const auto s3 = rig.renderer.sync(rig.world);
    CHECK_FALSE(s3.structural_rebuild);
    CHECK(s3.chunks_reextracted == 1U);
    CHECK(s3.entities_extracted > 0U);
    // one chunk's worth, not the world's — the chunk holds `kN / chunks_visited` entities on average
    CHECK(s3.entities_extracted < static_cast<u64>(n_total) / 4U);
    CHECK(s3.runs_visited >= 1U);
    CHECK(s3.dirty_runs >= 1U);
    CHECK(s3.uploaded_bytes > 0U);
    CHECK(s3.uploaded_bytes < static_cast<u64>(n_total) * sizeof(scenerender::InstanceGpu) / 4U);

    // and the move actually landed (a gate that measured only counters could pass on a renderer that did nothing)
    bool found = false;
    const auto& group = rig.renderer.mesh_groups()[0];
    for (usize slot = 0; slot < group.slot_entity.size(); ++slot)
    {
        if (group.slot_entity[slot] == entities[n_total / 2])
        {
            found = true;
            CHECK(group.instances[slot].world[12] == 3.0F);
            CHECK(group.instances[slot].world[13] == 5.0F);
            CHECK(group.instances[slot].world[14] == 7.0F);
        }
    }
    CHECK(found);

    // ── arm 4: a SPAWN is still structural, and the signature still catches it ──
    (void)rig.spawn_cube(1.0F, 2.0F, 3.0F);
    const auto s4 = rig.renderer.sync(rig.world);
    CHECK(s4.structural_rebuild);
    CHECK(s4.total_instances == static_cast<u32>(n_total) + 1U);
}

// ⭐⭐ REN-40-B GATE (the SCALING arm): quadruple the scene and the per-frame extract cost must NOT quadruple.
// ⛔ A single-size measurement cannot tell O(chunks) from O(entities) — both are "small" at one size. Two sizes
// can: the signature bytes and the re-extracted entity count of a STATIC frame must stay flat in the scene's
// size, while `chunks_visited` is allowed to grow (that is the irreducible walk).
TEST_CASE("REN-40-B GATE: 4x the instances does not cost 4x the static-frame extract",
          "[scene-render][ren40][geo7]")
{
    const auto static_frame = [](int n, u32& chunks, u64& sig_bytes, u64& reextracted) {
        Rig rig;
        for (int i = 0; i < n; ++i)
        {
            (void)rig.spawn_cube(static_cast<f32>(i % 100), 0.0F, static_cast<f32>(i) * 0.01F);
        }
        (void)rig.renderer.sync(rig.world);       // the structural frame
        const auto s = rig.renderer.sync(rig.world); // the static one — what a real frame costs
        chunks       = s.chunks_visited;
        sig_bytes    = s.signature_bytes;
        reextracted  = s.entities_extracted;
    };

    u32 c1 = 0;
    u32 c4 = 0;
    u64 b1 = 0;
    u64 b4 = 0;
    u64 r1 = 0;
    u64 r4 = 0;
    static_frame(8000, c1, b1, r1);
    static_frame(32000, c4, b4, r4);

    CHECK(c4 > c1);      // the scene really did get bigger
    CHECK(r1 == 0U);     // and a static frame re-extracts nothing at EITHER size
    CHECK(r4 == 0U);
    // The signature cost tracks CHUNKS, so it grows with the same ~4x the chunk count does — but it stays a
    // fixed number of bytes PER CHUNK, which is the property. An O(entities) signature would put b4 at
    // 32000 * 40 = 1.28 MB; this bounds it three orders of magnitude below that.
    CHECK(b1 == 40U * static_cast<u64>(c1)); // 5 u64 per chunk, exactly — the same constant at both sizes
    CHECK(b4 == 40U * static_cast<u64>(c4));
    CHECK(b4 < 40U * 32000U / 50U); // three orders below what a per-entity signature would consume
}
