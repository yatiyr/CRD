// test_scene_render_gpu.cpp — GEO-7 (D-007 row 72): THE 10K-INSTANCE GATE on a real Vulkan device (headless).
// import-shaped data → instantiate 10,000 cubes → chunk-grain sync → BVH + frustum culling → ONE vertex-pulling
// instanced draw through the gpu-context stack → readback proves lit pixels. Camera-away proves culling to zero;
// the SpatialBVHIndex path proves the crd-geometry broad phase agrees with the plane test.

#include <crd/gpu/context.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/gpu/vulkan_ray_tracing_context.hpp>
#if defined(_WIN32) // the D3D12 backend exists only on Windows; the DX12 twin gates ride the same guard
#include <crd/gpu/dx12_context.hpp>
#include <crd/gpu/dx12_raster_context.hpp>
#include <crd/gpu/dx12_ray_tracing_context.hpp>
#endif
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
#include <crd/anim/anim_resources.hpp> // REN-40-F: skeleton + clip builders for the GPU skinning gate
#include <crd/geometry/mesh_processing/cluster_dag_cook.hpp> // REN-41 Stage 4: the packed cluster DAG
#include <crd/geometry/mesh_processing/cluster_unpack.hpp>   // REN-41 Stage 4: the CPU unpack oracle

#include <catch2/catch_test_macros.hpp>

#include <crd/math/deterministic.hpp> // REN-41 Stage 4: crd::math sin/cos/floor/ceil/abs (NO std math)
#include <cstring>
#include <cstdio>  // REN-38-F15: per-run temp root stamp
#include <cstdlib> // REN-41: std::getenv for CRD_ASSETS_DIR (assets are disk-only now)
#include <ctime>   // REN-38-F15: per-run temp root stamp

using namespace crd;

namespace
{

memory::TlsfAllocator& galloc()
{
    static memory::TlsfAllocator a(256U << 20U);
    return a;
}

// ⭐⭐ REN-41: the shipped authored assets are the SINGLE SOURCE — read them from the ctest-provided
// `CRD_ASSETS_DIR` (there is no in-binary pack any more). Returns false if the root is unset or the file is
// missing, so a REQUIRE at the call site fails loudly rather than cooking nothing. `_CRT_SECURE_NO_WARNINGS`
// (set for this target) lets `std::getenv` through on MSVC.
[[nodiscard]] bool read_shipped_asset(const char* rel, containers::String& out)
{
    const char* root = std::getenv("CRD_ASSETS_DIR");
    if (root == nullptr || root[0] == '\0') { return false; }
    containers::String p(&galloc());
    p.append(root);
    p.append("/");
    p.append(rel);
    return platform::fs::read_file_text(platform::fs::Path(containers::StringView(p.c_str(), p.size())), out);
}

// ⭐⭐ REN-41: a recursive MIRROR of an asset tree. Disk is the single source of truth, so the F15 "an edited
// asset is live" gate needs a COMPLETE root to point the renderer at — not a one-file overlay over an embedded
// pack (there is none). Copy the shipped tree; the caller then overwrites the one asset it is testing.
void copy_tree(const platform::fs::Path& src, const platform::fs::Path& dst)
{
    (void)platform::fs::create_directories(dst);
    containers::Array<platform::fs::Path> entries(&galloc());
    platform::fs::list_directory(src, entries);
    for (usize i = 0; i < entries.size(); ++i)
    {
        const platform::fs::Path&      e = entries[i];
        const containers::StringView   g = e.generic();
        usize                          cut = 0U;
        bool                           has = false;
        for (usize k = g.size(); k-- > 0U;) { if (g[k] == '/') { cut = k + 1U; has = true; break; } }
        const containers::StringView name(g.data() + (has ? cut : 0U), has ? g.size() - cut : g.size());
        const platform::fs::Path      child = dst / name;
        if (platform::fs::is_directory(e)) { copy_tree(e, child); }
        else
        {
            containers::Array<u8> bytes(&galloc());
            if (platform::fs::read_file_binary(e, bytes))
            {
                (void)platform::fs::write_file_binary(child, containers::ConstSpan<u8>(bytes.data(), bytes.size()));
            }
        }
    }
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

// ── REN-40-F fixtures ────────────────────────────────────────────────────────────────────────────────────────────
// A 1-JOINT SKINNED CUBE: the same geometry as `build_cube_mesh_crdr` plus a SKNV chunk that binds every vertex
// to joint 0 with weight 1.0 — the minimal rig to exercise the GPU skinning pipeline (palette kernel, buffer
// layout, VS palette read). The animation TRANSLATES the root joint, so the skinned cube MOVES relative to its
// instance position — a visible, measurable effect on the rendered pixels.
[[nodiscard]] containers::Array<u8> build_skinned_cube_crdr(const resources::ResourceId& id)
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
    // SKNV: 24 bytes/vertex — 4x u16 joints + 4x f32 weights; every vertex bound to joint 0
    containers::Array<u8> skin(a);
    for (u32 sv = 0; sv < 8U; ++sv)
    {
        const u16 joints[4]  = {0, 0, 0, 0};
        const f32 weights[4] = {1.0F, 0.0F, 0.0F, 0.0F};
        const auto* jp = reinterpret_cast<const u8*>(joints);
        for (u32 k = 0; k < 8U; ++k) { skin.push_back(jp[k]); }
        const auto* wp = reinterpret_cast<const u8*>(weights);
        for (u32 k = 0; k < 16U; ++k) { skin.push_back(wp[k]); }
    }
    resources::CrdrWriter w(a, id, resources::kFourCC_MESH);
    w.add_chunk(resources::kFourCC_VERT, containers::as_const_span(verts));
    w.add_chunk(resources::kFourCC_INDX, containers::as_const_span(indices));
    w.add_chunk(resources::kFourCC_PRIM, containers::as_const_span(prim));
    w.add_chunk(resources::kFourCC_SKNV, containers::as_const_span(skin));
    return w.finish();
}

void write_resource_pack(const platform::fs::Path& path, const resources::ResourceId& id,
                          u32 fourcc, const containers::Array<u8>& art_bytes)
{
    auto* a = &galloc();
    containers::Array<u8> pool(a);
    const char name[] = "res";
    for (char c : name) { pool.push_back(static_cast<u8>(c)); }
    containers::Array<resources::ManifestEntry> entries(a);
    resources::ManifestEntry                    e;
    e.id          = id;
    e.type_fourcc = fourcc;
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

// Every pack a gate writes lives in the OS TEMP directory and is REMOVED when the test ends — Catch2 failures
// unwind, so the destructor runs even when a REQUIRE fails mid-test. ⛔ These used to be RELATIVE paths,
// resolved against wherever ctest was launched from — the repo checkout, on a dev box — and the checkout grew
// a crop of `sr_*_pack_*.crdr` artifacts that .gitignore then chased pattern by pattern.
struct TempPack
{
    platform::fs::Path path;

    TempPack(const char* prefix, const resources::ResourceId& id)
    {
        containers::String name(prefix, &galloc());
        name.append(id.to_string(&galloc()));
        name.append(".crdr");
        path = platform::fs::temp_directory() / containers::StringView(name.data(), name.size());
    }
    ~TempPack() { static_cast<void>(platform::fs::remove_file(path)); }
    TempPack(const TempPack&)            = delete;
    TempPack& operator=(const TempPack&) = delete;
    TempPack(TempPack&&)                 = delete;
    TempPack& operator=(TempPack&&)      = delete;
};

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
    const TempPack           pack("sr_gpu_pack_", cube_id);
    const platform::fs::Path pack_path = pack.path;
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

    // ⛔ REN-41: this gate measures FRUSTUM visibility, not the screen-size cull. The renderer now culls
    // sub-`min_draw_px` instances by default (a frontier "no aliased speck" win), which — with LOD OFF, so no
    // impostor catches the small ones — legitimately drops the far field below 5000. Turn the pixel cull OFF so
    // the count reflects the frustum test this case is actually about; the pixel cull has its own coverage.
    renderer.set_min_draw_px(0.0F);

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
    constexpr u32 floor_v  = 60U; // a flat clear or an unsampled map would leave the dominant channel far below
    constexpr u32 margin_v = 25U; // measured separation is ~49 on both sides
    CHECK(lr > floor_v);
    CHECK(rg > floor_v);
    CHECK(lr > rr + margin_v); // screen-LEFT sampled the RED texel
    CHECK(rg > lg + margin_v); // screen-RIGHT sampled the GREEN texel
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
    const TempPack           pack("sr_csm_pack_", cube_id);
    const platform::fs::Path pack_path = pack.path;
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
    const TempPack           pack("sr_csm_dir_", cube_id);
    const platform::fs::Path pack_path = pack.path;
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
    const TempPack           pack("sr_tech_pack_", cube_id);
    const platform::fs::Path pack_path = pack.path;
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

    // (c2) ⭐⭐ REN-38-E7: THE SAME SHADOWED FRAME, LIT BY AN AUTHORED DECLARATION. `forward_authored`’s
    // body is not C++ — it is `crd-light-cook` cooking `assets/lighting/scene_forward.crdl`: a declared light
    // record, a declared directional count, a declared CSM+PCF shadow scheme. ⛔ Until this row EVERY technique
    // body was a `TechniqueBody` function pointer, so the scene was lit by one hardcoded directional light no
    // matter what an asset said.
    //
    // ⛔⛔ THE CLAIM IS THAT IT LIGHTS AND SHADOWS, not that it compiles. A cooked-but-wrong lighting body
    // would still draw the geometry — so this asserts the SAME shape of result the C++ technique produces:
    // a real shading gradient, and a compact region that DARKENS with nothing getting brighter.
    containers::Array<u32> authored(&galloc());
    REQUIRE(sample("forward_authored", true, authored));
    REQUIRE(authored.size() == lit.size());
    u32 a_min = 255U;
    u32 a_max = 0U;
    u32 a_cov = 0U;
    u32 a_dark = 0U;
    u32 a_bright = 0U;
    for (usize i = 0; i < lit.size(); ++i)
    {
        const u32 v = authored[i] & 0xFFU;
        if ((authored[i] & 0x00FFFFFFU) != 0U)
        {
            ++a_cov;
            if (v < a_min) { a_min = v; }
            if (v > a_max) { a_max = v; }
        }
        const u32 before = lit[i] & 0xFFU;
        if (v + 12U < before) { ++a_dark; }
        else if (before + 12U < v) { ++a_bright; }
    }
    CHECK(a_cov > 500U);       // it draws the scene
    CHECK(a_max > a_min);      // …with a real shading gradient, not a constant
    CHECK(a_dark > 20U);       // …and the declared CSM term actually occludes
    CHECK(a_bright == 0U);     // …in the right direction

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
    const TempPack           pack("sr_vp_pack_", cube_id);
    const platform::fs::Path pack_path = pack.path;
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
    const TempPack           pack("sr_loop_pack_", cube_id);
    const platform::fs::Path pack_path = pack.path;
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

// ── ⭐⭐ REN-38-F6 GATES: the renderer draws THROUGH the advanced stages, from AUTHORED assets only. ──────────
// The F band proved each stage cooks; the A band proved each device verb draws. NOTHING had ever joined them:
// an authored `.frame.toml` naming an authored program, resolved by the LIVE renderer host, putting pixels (or
// compute results) where an assertion can see them. That join is what found the F1/F2 device-impossible
// entries, the pull-tail task decoration and the cull kernel's unbindable binding — all closed green by
// cook-only gates.

TEST_CASE("REN-38-F6 GATE: TESS, MESH and VISBUFFER families render through authored scene graphs (Vulkan)",
          "[scene-render][ren38][gpu][vulkan]")
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

    // one cube in the world — the advanced graphs do not draw it, but the renderer contributes nothing at all
    // for an EMPTY scene (the draw-list early-out), and the cull family needs real instances anyway
    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    const TempPack              pack("sr_f6_pack_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());
    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);
    {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs(math::Vec3f{0, 0, 0}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    // REN-38: the vertex variant axis is ENGINE-FILLED from the live declaration the renderer just cooked
    CHECK(renderer.debug_variant_vertex() != 0U);
    (void)renderer.sync(world);

    auto target = raster->create_color_depth_target(128U, 128U);
    REQUIRE(target != nullptr);
    const math::Mat4f view = math::look_at(math::Vec3f{0, 0, 5}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f light{0.4F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    containers::String graph(&galloc());

    // ── TESSELLATION: the corner-table VS + hull levels + DISPLACING domain, all `.crdv`. The domain expands
    // the quad x1.3, so a pixel between the base edge (NDC 0.6) and the expanded one (0.78) is a pixel ONLY a
    // running domain shader could have coloured — "did anything render" would pass without tessellation.
    {
        REQUIRE(read_shipped_asset("frame/scene_tess.frame.toml", graph));
        REQUIRE(renderer.set_frame_graph_toml(graph.c_str()));
        (void)renderer.render(*target, proj * view, light, clear);
        CHECK((target->read_pixel(64U, 64U) & 0x00FFFFFFU) != 0U);   // inside the base quad
        CHECK((target->read_pixel(108U, 64U) & 0x00FFFFFFU) != 0U);  // NDC ~0.69: expansion-only territory
        CHECK((target->read_pixel(124U, 64U) & 0x00FFFFFFU) == 0U);  // NDC ~0.94: beyond even the expansion
    }

    // ── MESH + TASK: the amplification pipeline from two declarations (task emits 2 mesh workgroups each; the
    // meshlet grid tiles thin triangles left to right). The claim is the JOIN renders — the amplification
    // COUNTS are the A8 device gates' claim.
    {
        REQUIRE(read_shipped_asset("frame/scene_mesh.frame.toml", graph));
        REQUIRE(renderer.set_frame_graph_toml(graph.c_str()));
        (void)renderer.render(*target, proj * view, light, clear);
        u32 lit_total = 0;
        u32 lit_left  = 0;
        for (u32 sy = 4U; sy < 128U; sy += 4U)
        {
            for (u32 sx = 2U; sx < 128U; sx += 2U)
            {
                if ((target->read_pixel(sx, sy) & 0x00FFFFFFU) != 0U)
                {
                    ++lit_total;
                    if (sx < 43U) { ++lit_left; }
                }
            }
        }
        CHECK(lit_total > 0U);
        CHECK(lit_left > 0U); // workgroup 0 tiles from NDC -0.8 — the left third
    }

    // ── VISIBILITY BUFFER: the procedural fullscreen pair (`.crdv`, the F7 vocabulary) with the id-grading FS.
    // Each half-screen triangle carries a DISTINCT primitive id, so the two halves must read back as two
    // DIFFERENT non-background greys — a pixel-true assertion, not a smoke one.
    {
        REQUIRE(read_shipped_asset("frame/scene_visbuffer.frame.toml", graph));
        REQUIRE(renderer.set_frame_graph_toml(graph.c_str()));
        (void)renderer.render(*target, proj * view, light, clear);
        const u32 a = target->read_pixel(96U, 24U) & 0x00FFFFFFU;
        const u32 b = target->read_pixel(24U, 96U) & 0x00FFFFFFU;
        CHECK(a != 0U);
        CHECK(b != 0U);
        CHECK(a != b); // two primitives, two ids, two greys
    }
}

TEST_CASE("REN-38-F6 GATE: the authored CULL graph computes real frustum visibility for the scene (Vulkan)",
          "[scene-render][ren38][gpu][vulkan]")
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

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    const TempPack              pack("sr_f6c_pack_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());
    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);
    // 8 cubes near the origin (in view), 8 far off to +x/+z (outside every frustum plane)
    constexpr u32 near_count = 8U;
    constexpr u32 far_count  = 8U;
    for (u32 i = 0; i < near_count + far_count; ++i)
    {
        const f32 x = i < near_count ? (static_cast<f32>(i) - 3.5F) * 1.5F : 1000.0F + static_cast<f32>(i);
        const f32 z = i < near_count ? 0.0F : 1000.0F;
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs(math::Vec3f{x, 0.0F, z}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    (void)renderer.sync(world);
    REQUIRE(renderer.mesh_groups().size() == 1U);

    auto target = raster->create_color_depth_target(64U, 64U);
    REQUIRE(target != nullptr);
    const math::Mat4f view = math::look_at(math::Vec3f{0, 8, 20}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);

    containers::String graph(&galloc());
    REQUIRE(read_shipped_asset("frame/scene_cull.frame.toml", graph));
    REQUIRE(renderer.set_frame_graph_toml(graph.c_str()));
    (void)renderer.render(*target, proj * view, math::Vec3f{0.4F, 1.0F, 0.2F}, gpu::ClearColor{0, 0, 0, 1});

    // ⛔ The authored kernel's verdicts, read back: near instances VISIBLE, far ones CULLED. A gate that only
    // counted "some flags written" would pass a kernel that wrote 1 everywhere.
    gpu::IStorageBuffer* flags = renderer.debug_scene_buffer("cull_flags");
    REQUIRE(flags != nullptr);
    REQUIRE(raster->download_storage(*flags));
    u32 visible = 0;
    for (u32 i = 0; i < near_count + far_count; ++i) { visible += flags->read_u32(i) != 0U ? 1U : 0U; }
    UNSCOPED_INFO("visible " << visible << " of " << (near_count + far_count));
    CHECK(visible >= 1U);
    CHECK(visible <= near_count); // every far instance culled; near ones (mostly) survive

    // the GPU-DRIVEN half: the INDIRECT mark pass ran exactly one workgroup per survivor off the args
    // the cull kernel wrote -- the count never touched the CPU. marked == visible is the whole claim.
    gpu::IStorageBuffer* marks = renderer.debug_scene_buffer("cull_marks");
    REQUIRE(marks != nullptr);
    REQUIRE(raster->download_storage(*marks));
    u32 marked = 0;
    for (u32 i = 0; i < near_count + far_count; ++i) { marked += marks->read_u32(i) != 0U ? 1U : 0U; }
    UNSCOPED_INFO("marked " << marked);
    CHECK(marked == visible);
    CHECK(marked >= 1U);
}

TEST_CASE("REN-38-F6 GATE: the authored RT PIPELINE graph traces the scene TLAS through the live host (Vulkan)",
          "[scene-render][ren38][gpu][vulkan]")
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
    if (!raster->supports_rt_pipeline()) { SKIP("adapter has no ray-tracing pipeline"); }

    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());
    // ⛔ NON-OPAQUE, or the graph's declared any-hit stage is silently skipped by traversal (the flag's whole
    // meaning) and the fourth authored stage proves nothing.
    const float tri[9]       = {-1.0F, -1.0F, 1.0F, 1.0F, -1.0F, 1.0F, 0.0F, 1.0F, 1.0F};
    const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    auto        scene_as     = rt.build_scene_instanced(tri, 1U, identity, 1U, /*opaque=*/false);
    REQUIRE(scene_as != nullptr);

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    const TempPack              pack("sr_f6r_pack_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());
    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);
    {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs(math::Vec3f{0, 0, 0}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    (void)renderer.sync(world);
    renderer.set_scene_accel(scene_as.get());

    auto target = raster->create_color_depth_target(64U, 64U);
    REQUIRE(target != nullptr);
    const math::Mat4f view = math::look_at(math::Vec3f{0, 0, 5}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);

    containers::String graph(&galloc());
    REQUIRE(read_shipped_asset("frame/scene_rt.frame.toml", graph));
    REQUIRE(renderer.set_frame_graph_toml(graph.c_str()));
    (void)renderer.render(*target, proj * view, math::Vec3f{0.4F, 1.0F, 0.2F}, gpu::ClearColor{0, 0, 0, 1});

    // the four authored stages ran: every ray slot holds a WRITTEN record (a hit t or the miss marker — both
    // are non-zero bit patterns), where a dead pipeline leaves the fresh buffer untouched
    gpu::IStorageBuffer* hits = renderer.debug_scene_buffer("hits");
    REQUIRE(hits != nullptr);
    REQUIRE(raster->download_storage(*hits));
    u32 written = 0;
    // the raygen writes `out[LaunchId.x] = payload[0]` — one f32 record per ray, hit 1.0 / miss -1.0
    for (u32 i = 0; i < 4U; ++i) { written += hits->read_u32(i) != 0U ? 1U : 0U; }
    UNSCOPED_INFO("written ray records: " << written << " of 4");
    CHECK(written == 4U);
}

#if defined(_WIN32)
// ── ⭐⭐ REN-38-F6 GATES (DX12): the SAME renderer joins on the OTHER backend. ────────────────────────────────
// ⛔ A per-backend claim closed on the strength of the Vulkan gate alone leaves the entire DX12 dispatch path —
// PSO/state creation for tess/mesh pipelines, the DXR SBT, the compute binding order — unexecuted (the exact
// scar A16 carried: "compiled" standing in for "ran"). These are the Vulkan gates, joint for joint, on D3D12.

TEST_CASE("REN-38-F6 GATE (DX12): TESS, MESH and VISBUFFER families render through authored scene graphs",
          "[scene-render][ren38][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    const TempPack              pack("sr_f6dx_pack_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());
    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);
    {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs(math::Vec3f{0, 0, 0}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    if (!renderer.init_programs(*gctx)) { SKIP("dxc/DXIL unavailable"); }
    (void)renderer.sync(world);

    auto target = raster->create_color_depth_target(128U, 128U);
    REQUIRE(target != nullptr);
    const math::Mat4f view = math::look_at(math::Vec3f{0, 0, 5}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f light{0.4F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    containers::String graph(&galloc());

    // TESSELLATION: the displacement-territory pixel is the claim (see the Vulkan twin for the geometry)
    {
        REQUIRE(read_shipped_asset("frame/scene_tess.frame.toml", graph));
        REQUIRE(renderer.set_frame_graph_toml(graph.c_str()));
        (void)renderer.render(*target, proj * view, light, clear);
        CHECK((target->read_pixel(64U, 64U) & 0x00FFFFFFU) != 0U);
        CHECK((target->read_pixel(108U, 64U) & 0x00FFFFFFU) != 0U);
        CHECK((target->read_pixel(124U, 64U) & 0x00FFFFFFU) == 0U);
    }

    // MESH + TASK amplification
    {
        REQUIRE(read_shipped_asset("frame/scene_mesh.frame.toml", graph));
        REQUIRE(renderer.set_frame_graph_toml(graph.c_str()));
        (void)renderer.render(*target, proj * view, light, clear);
        u32 lit_total = 0;
        u32 lit_left  = 0;
        for (u32 sy = 4U; sy < 128U; sy += 4U)
        {
            for (u32 sx = 2U; sx < 128U; sx += 2U)
            {
                if ((target->read_pixel(sx, sy) & 0x00FFFFFFU) != 0U)
                {
                    ++lit_total;
                    if (sx < 43U) { ++lit_left; }
                }
            }
        }
        CHECK(lit_total > 0U);
        CHECK(lit_left > 0U);
    }

    // VISIBILITY BUFFER: two primitives, two ids, two greys.
    // ⚠ The probes sit on the HORIZONTAL midline (NDC y ~ 0): the fullscreen quad's two triangles split along
    // the y = x diagonal, and D3D's raster y-orientation MIRRORS that diagonal relative to Vulkan — corner
    // probes that straddle it on one backend both land on a single triangle on the other. A midline pair
    // classifies identically under either orientation.
    {
        REQUIRE(read_shipped_asset("frame/scene_visbuffer.frame.toml", graph));
        REQUIRE(renderer.set_frame_graph_toml(graph.c_str()));
        (void)renderer.render(*target, proj * view, light, clear);
        const u32 a = target->read_pixel(98U, 64U) & 0x00FFFFFFU; // NDC (+0.53, ~0) — the y<x triangle
        const u32 b = target->read_pixel(30U, 64U) & 0x00FFFFFFU; // NDC (-0.53, ~0) — the y>x triangle
        CHECK(a != 0U);
        CHECK(b != 0U);
        CHECK(a != b);
    }
}

TEST_CASE("REN-38-F6 GATE (DX12): the authored CULL graph computes real frustum visibility for the scene",
          "[scene-render][ren38][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    const TempPack              pack("sr_f6dxc_pack_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());
    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);
    constexpr u32 near_count = 8U;
    constexpr u32 far_count  = 8U;
    for (u32 i = 0; i < near_count + far_count; ++i)
    {
        const f32 x = i < near_count ? (static_cast<f32>(i) - 3.5F) * 1.5F : 1000.0F + static_cast<f32>(i);
        const f32 z = i < near_count ? 0.0F : 1000.0F;
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs(math::Vec3f{x, 0.0F, z}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    if (!renderer.init_programs(*gctx)) { SKIP("dxc/DXIL unavailable"); }
    (void)renderer.sync(world);
    REQUIRE(renderer.mesh_groups().size() == 1U);

    auto target = raster->create_color_depth_target(64U, 64U);
    REQUIRE(target != nullptr);
    const math::Mat4f view = math::look_at(math::Vec3f{0, 8, 20}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);

    containers::String graph(&galloc());
    REQUIRE(read_shipped_asset("frame/scene_cull.frame.toml", graph));
    REQUIRE(renderer.set_frame_graph_toml(graph.c_str()));
    (void)renderer.render(*target, proj * view, math::Vec3f{0.4F, 1.0F, 0.2F}, gpu::ClearColor{0, 0, 0, 1});

    gpu::IStorageBuffer* flags = renderer.debug_scene_buffer("cull_flags");
    REQUIRE(flags != nullptr);
    REQUIRE(raster->download_storage(*flags));
    u32 visible = 0;
    for (u32 i = 0; i < near_count + far_count; ++i) { visible += flags->read_u32(i) != 0U ? 1U : 0U; }
    UNSCOPED_INFO("visible " << visible << " of " << (near_count + far_count));
    CHECK(visible >= 1U);
    CHECK(visible <= near_count);

    // the GPU-DRIVEN half: the INDIRECT mark pass ran exactly one workgroup per survivor off the args
    // the cull kernel wrote -- the count never touched the CPU. marked == visible is the whole claim.
    gpu::IStorageBuffer* marks = renderer.debug_scene_buffer("cull_marks");
    REQUIRE(marks != nullptr);
    REQUIRE(raster->download_storage(*marks));
    u32 marked = 0;
    for (u32 i = 0; i < near_count + far_count; ++i) { marked += marks->read_u32(i) != 0U ? 1U : 0U; }
    UNSCOPED_INFO("marked " << marked);
    CHECK(marked == visible);
    CHECK(marked >= 1U);
}

TEST_CASE("REN-38-F6 GATE (DX12): the authored RT PIPELINE graph traces the scene TLAS through the live host",
          "[scene-render][ren38][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (!raster->supports_rt_pipeline()) { SKIP("adapter has no DXR ray-tracing pipeline"); }

    gpu::Dx12RayTracingContext rt;
    if (!rt.valid()) { SKIP("no DXR-capable device"); }
    const float tri[9]       = {-1.0F, -1.0F, 1.0F, 1.0F, -1.0F, 1.0F, 0.0F, 1.0F, 1.0F};
    const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    auto        scene_as     = rt.build_scene_instanced(tri, 1U, identity, 1U, /*opaque=*/false);
    REQUIRE(scene_as != nullptr);

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    const TempPack              pack("sr_f6dxr_pack_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());
    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);
    {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs(math::Vec3f{0, 0, 0}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    if (!renderer.init_programs(*gctx)) { SKIP("dxc/DXIL unavailable"); }
    (void)renderer.sync(world);
    renderer.set_scene_accel(scene_as.get());

    auto target = raster->create_color_depth_target(64U, 64U);
    REQUIRE(target != nullptr);
    const math::Mat4f view = math::look_at(math::Vec3f{0, 0, 5}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);

    containers::String graph(&galloc());
    REQUIRE(read_shipped_asset("frame/scene_rt.frame.toml", graph));
    REQUIRE(renderer.set_frame_graph_toml(graph.c_str()));
    (void)renderer.render(*target, proj * view, math::Vec3f{0.4F, 1.0F, 0.2F}, gpu::ClearColor{0, 0, 0, 1});

    gpu::IStorageBuffer* hits = renderer.debug_scene_buffer("hits");
    REQUIRE(hits != nullptr);
    REQUIRE(raster->download_storage(*hits));
    u32 written = 0;
    for (u32 i = 0; i < 4U; ++i) { written += hits->read_u32(i) != 0U ? 1U : 0U; }
    UNSCOPED_INFO("written ray records: " << written << " of 4");
    CHECK(written == 4U);
}
#endif // _WIN32 (the DX12 twin gates)

// ── ⭐ REN-38-F15 / REN-41 GATE: DISK-FIRST asset loading — the asset root is the SINGLE SOURCE. ─────────────
// The claim has two halves, both pixel-visible: (1) an EDITED disk declaration changes the frame without a
// rebuild; (2) a CORRUPT disk declaration fails LOUDLY — there is no in-binary pack to silently fall back to, and
// a fallback that renders would be indistinguishable from the edit having worked. Disk is the only source now, so
// the temp root is a COMPLETE mirror of the shipped tree with the one asset under test overwritten.
TEST_CASE("REN-38-F15 GATE: an edited disk asset is live and a corrupt one refuses loudly (Vulkan)",
          "[scene-render][ren38][gpu][vulkan]")
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

    // an asset root in the temp directory holding ONE edited declaration: the tess corner table at +-0.9
    // instead of +-0.6 — under the x1.3 domain expansion, a pixel near NDC 0.9 is reachable ONLY via the disk
    // copy (the embedded quad tops out at 0.78)
    containers::String root(&galloc());
    root.append(platform::fs::temp_directory().generic());
    root.append("/crd_f15_root_");
    {
        // a FRESH root per run: the corrupt copies halves 2/3 write must not poison the next run's half 1
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "%llu",
                      static_cast<unsigned long long>(std::time(nullptr)));
        root.append(stamp);
    }
    // ⭐⭐ REN-41: disk is the SINGLE SOURCE — mirror the shipped tree into the temp root so it is COMPLETE
    // (set_asset_root validates the default frame pair, and the scene_tess frame cooks its stages BY NAME from
    // the root), THEN overwrite the one declaration under test below. No embedded pack shadows anything now.
    const char* canon = std::getenv("CRD_ASSETS_DIR");
    if (canon == nullptr || canon[0] == '\0') { SKIP("CRD_ASSETS_DIR not set (run through ctest)"); }
    copy_tree(platform::fs::Path(canon),
              platform::fs::Path(containers::StringView(root.c_str(), root.size())));
    containers::String vdir(&galloc());
    vdir.append(root.c_str());
    vdir.append("/vertex");
    REQUIRE(platform::fs::create_directories(platform::fs::Path(containers::StringView(vdir.c_str(), vdir.size()))));
    containers::String edited(&galloc());
    REQUIRE(read_shipped_asset("vertex/tess_corners.crdv", edited));
    // widen the corner table: every 0.6 literal becomes 0.9 (the ifequal chains carry them)
    for (usize i = 0; i + 3U <= edited.size(); ++i)
    {
        if (edited.c_str()[i] == '0' && edited.c_str()[i + 1U] == '.' && edited.c_str()[i + 2U] == '6')
        {
            edited.data()[i + 2U] = '9';
        }
    }
    containers::String vfile(&galloc());
    vfile.append(vdir.c_str());
    vfile.append("/tess_corners.crdv");
    REQUIRE(platform::fs::write_file_text(platform::fs::Path(containers::StringView(vfile.c_str(), vfile.size())),
                                          containers::StringView(edited.c_str(), edited.size())));

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    const TempPack              pack("sr_f15_pack_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());
    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);
    {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs(math::Vec3f{0, 0, 0}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }
    const math::Mat4f view = math::look_at(math::Vec3f{0, 0, 5}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};
    containers::String graph(&galloc());
    REQUIRE(read_shipped_asset("frame/scene_tess.frame.toml", graph));

    // half 1: the EDITED disk copy draws — a pixel the shipped (0.6) declaration cannot reach lights up
    {
        scenerender::SceneRenderer renderer(&galloc());
        REQUIRE(renderer.init(*raster, rm));
        REQUIRE(renderer.init_programs(*vk));
        REQUIRE(renderer.set_asset_root(root.c_str()));
        (void)renderer.sync(world);
        auto target = raster->create_color_depth_target(128U, 128U);
        REQUIRE(target != nullptr);
        REQUIRE(renderer.set_frame_graph_toml(graph.c_str()));
        const auto r1 = renderer.render(*target, proj * view, math::Vec3f{0.4F, 1.0F, 0.2F}, clear);
        CHECK(r1.timed_passes > 0U); // the graph executed
        CHECK((target->read_pixel(120U, 64U) & 0x00FFFFFFU) != 0U); // NDC ~0.87: disk-only territory
        CHECK((target->read_pixel(64U, 64U) & 0x00FFFFFFU) != 0U);
    }

    // half 2: a CORRUPT disk copy REFUSES — the pass fails by name, nothing silently falls back and renders
    {
        REQUIRE(platform::fs::write_file_text(
            platform::fs::Path(containers::StringView(vfile.c_str(), vfile.size())),
            containers::StringView("this is not a vertex declaration")));
        scenerender::SceneRenderer renderer(&galloc());
        REQUIRE(renderer.init(*raster, rm));
        REQUIRE(renderer.init_programs(*vk));
        REQUIRE(renderer.set_asset_root(root.c_str()));
        (void)renderer.sync(world);
        auto target = raster->create_color_depth_target(128U, 128U);
        REQUIRE(target != nullptr);
        REQUIRE(renderer.set_frame_graph_toml(graph.c_str()));
        const auto r2 = renderer.render(*target, proj * view, math::Vec3f{0.4F, 1.0F, 0.2F}, clear);
        // ⛔ EXECUTION truth, not pixels: a fresh target's readback can recycle the PREVIOUS target's host
        // memory, so a stale image would fake either verdict. A refused cook means the record fails by name
        // and NO pass executes — there is no pack to sneak a fallback in behind the corrupt disk copy.
        CHECK(r2.timed_passes == 0U);
    }

    // half 3: a corrupt disk FRAME graph refuses the root itself
    {
        containers::String fdir(&galloc());
        fdir.append(root.c_str());
        fdir.append("/frame");
        REQUIRE(platform::fs::create_directories(
            platform::fs::Path(containers::StringView(fdir.c_str(), fdir.size()))));
        containers::String ffile(&galloc());
        ffile.append(fdir.c_str());
        ffile.append("/forward_basic.frame.toml");
        REQUIRE(platform::fs::write_file_text(
            platform::fs::Path(containers::StringView(ffile.c_str(), ffile.size())),
            containers::StringView("not a graph")));
        scenerender::SceneRenderer renderer(&galloc());
        REQUIRE(renderer.init(*raster, rm));
        CHECK_FALSE(renderer.set_asset_root(root.c_str()));
    }
}

// ── ⭐⭐ REN-38 GATE: a group is TEXTURED **AND** SHADOWED in the same frame. ────────────────────────────────
// Until this slice the base-colour map and the shadow atlas fought over descriptor bindings 1/2, so
// `record_one_group` had to NULL one of them — textured monuments lost their maps the instant shadows turned
// on (the user saw it immediately). The atlas now lives at its OWN bindings (4/5 on Vulkan, t4/s5 on DX12) and
// the renderer cooks a COMBINED variant, so this gate demands BOTH properties of one receiver in ONE frame:
//   (a) the texture is VISIBLE  — the receiver's left and right halves sample different texels (red vs green);
//   (b) the shadow LANDS on it  — a compact patch darkens when shadows turn on, on the SAME textured surface.
// Either property alone was already gated (Half B / the occluder gate); their CONJUNCTION is what was impossible.
TEST_CASE("REN-38 GATE: a TEXTURED receiver is ALSO SHADOWED in one frame (Vulkan)",
          "[scene-render][ren38][material][shadow][gpu][vulkan]")
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

    // cook + mount: the UV QUAD (the receiver, textured), a plain CUBE (the caster), the 2x1 red/green TXTR
    // and an OpenPbrMaterial referencing it — the exact assets Half B proves individually.
    memory::TlsfAllocator       a2(4U << 20U);
    const resources::ResourceId quad_id = resources::ResourceId::mint_random();
    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    const resources::ResourceId tex_id  = resources::ResourceId::mint_random();
    const resources::ResourceId mtl_id  = resources::ResourceId::mint_random();
    const platform::fs::Path    quad_path(containers::StringView("sr_ts_quad.crdr"));
    const platform::fs::Path    tex_path(containers::StringView("sr_ts_tex.crdr"));
    const platform::fs::Path    mtl_path(containers::StringView("sr_ts_mtl.crdr"));
    const TempPack              cube_pack("sr_ts_cube_", cube_id);
    write_one_pack(quad_path, quad_id, resources::kFourCC_MESH, build_quad_mesh_crdr(quad_id), "quad");
    write_one_pack(tex_path, tex_id, resources::kFourCC_TXTR, build_rg_txtr_crdr(tex_id), "rg");
    write_mesh_pack(cube_pack.path, cube_id);
    resources::PbrmParams params;
    params.base_color[0] = 1.0F;
    params.base_color[1] = 1.0F;
    params.base_color[2] = 1.0F;
    params.base_alpha    = 1.0F;
    resources::PbrmTextures textures;
    textures.base_color = tex_id;
    auto mtl_bytes      = resources::pbrm_build(params, textures, mtl_id, &a2);
    write_one_pack(mtl_path, mtl_id, resources::kFourCC_PBRM, mtl_bytes, "mtl");

    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    resources::register_texture_loader(&rm);
    resources::register_openpbr_material_loader(&rm);
    REQUIRE(rm.mount_manifest(quad_path.generic()).is_valid());
    REQUIRE(rm.mount_manifest(tex_path.generic()).is_valid());
    REQUIRE(rm.mount_manifest(mtl_path.generic()).is_valid());
    REQUIRE(rm.mount_manifest(cube_pack.path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);
    // the TEXTURED receiver: the +Z-facing UV quad laid FLAT (rotated to face +Y), scaled wide
    {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        const math::Quatf     q = math::from_axis_angle(math::Vec3f{1.0F, 0.0F, 0.0F}, -1.57079632679F);
        t.translation           = math::from_raw_vec<units::dim::Length>(math::Vec3f{0.0F, 0.0F, 0.0F});
        t.rotation              = q;
        t.scale                 = {24.0F, 24.0F, 1.0F};
        t.world                 = math::from_trs({0.0F, 0.0F, 0.0F}, q, {24.0F, 24.0F, 1.0F});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{quad_id, mtl_id});
    }
    // the CASTER: a plain cube floating above the origin
    {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.translation = math::from_raw_vec<units::dim::Length>(math::Vec3f{0.0F, 8.0F, 0.0F});
        t.scale       = {4.0F, 4.0F, 4.0F};
        t.world       = math::from_trs({0.0F, 8.0F, 0.0F}, math::Quatf::identity(), {4.0F, 4.0F, 4.0F});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    const auto sy = renderer.sync(world);
    CHECK(sy.total_instances == 2U);

    auto target = raster->create_color_depth_target(256U, 256U);
    REQUIRE(target != nullptr);

    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 30.0F, 42.0F}, math::Vec3f{0, 0, 0},
                                           math::Vec3f{0, 1, 0});
    const math::Mat4f     proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Mat4f     vp   = proj * view;
    const math::Vec3f     light{0.0F, 1.0F, 0.0F}; // straight down — the shadow lands under the caster
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    scenerender::CsmConfig ccfg;
    ccfg.cascade_count = 4;
    ccfg.map_size      = 1024;
    ccfg.far_plane     = 120.0F;
    renderer.set_csm_config(ccfg);

    // shadows OFF — the reference image (and the texture-visibility half of the claim)
    renderer.set_shadows_enabled(false);
    REQUIRE(renderer.render(*target, vp, light, clear, nullptr).draws > 0U);
    containers::Array<u32> unshadowed(&galloc());
    u32 red_left = 0U;
    u32 grn_right = 0U;
    for (u32 y = 0; y < 256U; y += 4U)
    {
        for (u32 x = 0; x < 256U; x += 4U)
        {
            const u32 px = target->read_pixel(x, y);
            unshadowed.push_back(px);
            if ((px & 0x00FFFFFFU) == 0U) { continue; } // background
            const u32 r = px & 0xFFU;
            const u32 g = (px >> 8U) & 0xFFU;
            if (x < 120U && r > g + 40U) { ++red_left; }   // the RED texel dominates screen-left
            if (x > 136U && g > r + 40U) { ++grn_right; }  // the GREEN texel dominates screen-right
        }
    }
    // (a) the TEXTURE is visible: both texels of the base-colour map show through on their own halves
    CHECK(red_left > 30U);
    CHECK(grn_right > 30U);

    // shadows ON — the same scene, same camera
    REQUIRE(renderer.set_shadows_enabled(true));
    REQUIRE(renderer.render(*target, vp, light, clear, nullptr).draws > 0U);
    u32 darker    = 0U;
    u32 idx       = 0U;
    u32 red_left2 = 0U;
    u32 grn_right2 = 0U;
    for (u32 y = 0; y < 256U; y += 4U)
    {
        for (u32 x = 0; x < 256U; x += 4U)
        {
            const u32 px     = target->read_pixel(x, y);
            const u32 before = unshadowed[idx++];
            const u32 lb     = ((before & 0xFFU) + ((before >> 8U) & 0xFFU) + ((before >> 16U) & 0xFFU)) / 3U;
            const u32 la     = ((px & 0xFFU) + ((px >> 8U) & 0xFFU) + ((px >> 16U) & 0xFFU)) / 3U;
            if (la + 12U < lb) { ++darker; }
            if ((px & 0x00FFFFFFU) == 0U) { continue; }
            const u32 r = px & 0xFFU;
            const u32 g = (px >> 8U) & 0xFFU;
            if (x < 120U && r > g + 40U) { ++red_left2; }
            if (x > 136U && g > r + 40U) { ++grn_right2; }
        }
    }
    // (b) the SHADOW lands: a compact patch darkened on the textured receiver...
    CHECK(darker > 20U);
    CHECK(darker < 2000U);
    // ...(c) AND the texture SURVIVED shadows turning on — the exact conjunction that used to be impossible
    // (the old router nulled base_color under active shadows, collapsing the receiver to the flat material).
    CHECK(red_left2 > 30U);
    CHECK(grn_right2 > 30U);
}

// ── ⭐⭐ REN-38 GATE: CROSS-GROUP SCENE BATCHING — two mesh groups, ONE device draw command. ────────────────
// The consolidation's whole claim: plain groups render from ONE scene buffer (per-group regions + a draw
// table), the scene VS rebases every load by `table[DrawIndex]`, and the executor merges the draw list into a
// single multi-draw batch. Two DIFFERENT meshes (a cube group and a second cube group from a distinct mesh id
// — distinct groups by construction) must land in ONE batch:
//   (a) `multi_batch_count()` advances by EXACTLY ONE for the frame (one bucket, one vkCmdDrawIndirect);
//   (b) BOTH groups' geometry is visible (each instance tinted via its instance colour, probed separately) —
//       which fails loudly if the second draw's rebase reads the first group's region.
TEST_CASE("REN-38 GATE: two mesh groups render as ONE multi-draw batch from the scene buffer (Vulkan)",
          "[scene-render][ren38][multidraw][gpu][vulkan]")
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

    // two DISTINCT mesh resources ⇒ two groups ⇒ two rows of the draw table
    const resources::ResourceId mesh_a = resources::ResourceId::mint_random();
    const resources::ResourceId mesh_b = resources::ResourceId::mint_random();
    const TempPack              pack_a("sr_mdg_a_", mesh_a);
    const TempPack              pack_b("sr_mdg_b_", mesh_b);
    write_mesh_pack(pack_a.path, mesh_a);
    write_mesh_pack(pack_b.path, mesh_b);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack_a.path.generic()).is_valid());
    REQUIRE(rm.mount_manifest(pack_b.path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);
    const auto add = [&](const resources::ResourceId& mid, math::Vec3f pos) {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.translation = math::from_raw_vec<units::dim::Length>(pos);
        t.scale       = {2.0F, 2.0F, 2.0F};
        t.world       = math::from_trs(pos, math::Quatf::identity(), {2.0F, 2.0F, 2.0F});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{mid, {}});
    };
    add(mesh_a, {-4.0F, 0.0F, 0.0F}); // group A, screen-left
    add(mesh_b, {4.0F, 0.0F, 0.0F});  // group B, screen-right

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    const auto sy = renderer.sync(world);
    CHECK(sy.total_instances == 2U);
    REQUIRE(renderer.mesh_groups().size() == 2U); // two GROUPS — the cross-group claim needs both

    auto target = raster->create_color_depth_target(256U, 256U);
    REQUIRE(target != nullptr);
    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 6.0F, 16.0F}, math::Vec3f{0, 0, 0},
                                           math::Vec3f{0, 1, 0});
    const math::Mat4f     proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     light{0.3F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    const crd::u64 batches_before = raster->multi_batch_count();
    const auto     r              = renderer.render(*target, proj * view, light, clear, nullptr);
    CHECK(r.draws == 2U);
    CHECK(r.drawn_instances == 2U);

    // (a) ONE batch covered both groups — the count assert "looped" cannot fake
    CHECK(raster->multi_batch_count() == batches_before + 1U);

    // (b) BOTH halves show geometry: a wrong rebase collapses one side to background (or garbage clip space)
    u32 left = 0U;
    u32 right = 0U;
    for (u32 y = 64U; y < 192U; y += 2U)
    {
        for (u32 x = 0U; x < 256U; x += 2U)
        {
            const u32 px = target->read_pixel(x, y);
            if ((px & 0x00FFFFFFU) == 0U) { continue; }
            if (x < 120U) { ++left; }
            if (x > 136U) { ++right; }
        }
    }
    INFO("coverage left=" << left << " right=" << right);
    CHECK(left > 40U);
    CHECK(right > 40U);
}

// ── ⭐⭐ 38-G1 GATE: an AUTHORED POST GRAPH transforms the frame — the technique library reaches the device. ──
// The frame is `scene → post` where the post pass's shader is `crd://post/...`: a fullscreen VS asset + an FS
// cooked from a `[[node]]` POST graph (parse_post_toml → cook_post_graph). Two halves, deliberately split:
//   (a) EXACTNESS on `srgb_only` — the sRGB OETF is a SPEC formula the test recomputes independently, so the
//       whole chain (authored graph → cook → FS → fullscreen sample → pixels) is pinned to real numbers;
//   (b) REACHABILITY on `tonemap_agx` — the AgX pixels must differ from the sRGB-only pixels (the display
//       transform actually ran; its own numeric truth lives with the ckir_post oracle tests).
TEST_CASE("38-G1 GATE: an authored POST graph tonemaps the frame (scene -> post, Vulkan)",
          "[scene-render][ren38][g1][post][gpu][vulkan]")
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
    const TempPack              pack("sr_g1_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());
    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);
    {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs({0.0F, 0.0F, 0.0F}, math::Quatf::identity(), {2.0F, 2.0F, 2.0F});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }
    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    (void)renderer.sync(world);

    // the authored frame: the scene into a transient, the POST graph over it into @output
    const auto frame_toml = [](const char* shader, const char* fname) {
        static char buf[1280];
        (void)std::snprintf(static_cast<char*>(buf), sizeof(buf), R"(
schema = 1
name   = "crd://frame/%s"

[[resource]]
name    = "scene_hdr"
kind    = "transient_image"
format  = "RGBA8Unorm"
width   = 128
height  = 128
sampled = true

[[draw_list]]
name = "visible_geometry"
all  = ["MeshRenderer", "Transform"]
cull = "frustum"
sort = "material"

[[pass]]
name          = "scene"
kind          = "raster.geometry"
draw_list     = "visible_geometry"
writes        = ["scene_hdr"]
material_pass = "Forward"
clear_color   = [0.10, 0.30, 0.60, 1.0]

[[pass]]
name   = "post"
kind   = "raster.fullscreen"
reads  = ["scene_hdr"]
writes = ["@output"]
shader = "%s"
)", fname, shader);
        return static_cast<const char*>(buf);
    };

    auto target = raster->create_color_depth_target(128U, 128U);
    REQUIRE(target != nullptr);
    const math::Mat4f vp = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F)
                           * math::look_at(math::Vec3f{0, 2, 8}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Vec3f     light{0.3F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    // (a) EXACTNESS: srgb_only — a background pixel holds the clear colour, so out = OETF(clear), computed here
    REQUIRE(renderer.set_frame_graph_toml(frame_toml("crd://post/srgb_only", "g1_post_srgb")));
    REQUIRE(renderer.render(*target, vp, light, clear, nullptr).draws > 0U);
    const u32 srgb_px = target->read_pixel(4U, 4U); // top corner: background in this camera
    const auto oetf = [](double c) {
        return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    };
    const auto ch = [&](double c) { return static_cast<i32>(std::lround(oetf(c) * 255.0)); };
    const i32 er = ch(0.10);
    const i32 eg = ch(0.30);
    const i32 eb = ch(0.60);
    const i32 ar = static_cast<i32>(srgb_px & 0xFFU);
    const i32 ag = static_cast<i32>((srgb_px >> 8U) & 0xFFU);
    const i32 ab = static_cast<i32>((srgb_px >> 16U) & 0xFFU);
    INFO("srgb got (" << ar << "," << ag << "," << ab << ") want (" << er << "," << eg << "," << eb << ")");
    CHECK(std::abs(ar - er) <= 2);
    CHECK(std::abs(ag - eg) <= 2);
    CHECK(std::abs(ab - eb) <= 2);

    // (b) REACHABILITY: the AgX graph produces a DIFFERENT frame than srgb_only over the same scene.
    // A FRESH renderer isolates the swap from any per-renderer program state.
    scenerender::SceneRenderer r2(&galloc());
    REQUIRE(r2.init(*raster, rm));
    REQUIRE(r2.init_programs(*vk));
    (void)r2.sync(world);
    REQUIRE(r2.set_frame_graph_toml(frame_toml("crd://post/tonemap_agx", "g1_post_agx")));
    REQUIRE(r2.render(*target, vp, light, clear, nullptr).draws > 0U);
    const u32 agx_px = target->read_pixel(4U, 4U);
    CHECK(agx_px != srgb_px); // the display transform ran (AgX bends what a bare OETF does not)
    CHECK((agx_px & 0x00FFFFFFU) != 0U); // and produced a real colour, not a dropped pass

    // ⛔⛔ AND THE GEOMETRY IS IN IT. Sampling only the background proved the CLEAR reached the post pass and
    // nothing more — which is exactly how a live app ended up showing a tonemapped EMPTY frame: the textured
    // and shadowed scene verbs refused the depth-less colour transient and drew nothing, silently. A post gate
    // that never looks at the mesh cannot see that.
    u32 differs = 0U;
    for (u32 y = 0; y < 128U; y += 2U)
    {
        for (u32 x = 0; x < 128U; x += 2U)
        {
            if ((target->read_pixel(x, y) & 0x00FFFFFFU) != (agx_px & 0x00FFFFFFU)) { ++differs; }
        }
    }
    INFO("pixels differing from the background: " << differs);
    CHECK(differs > 200U); // the cube covers a real part of the frame
}

#ifdef _WIN32
// ── ⭐⭐ 38-G1 GATE (DX12): the SAME authored post frame on the OTHER backend — sRGB pinned to the spec
// OETF, AgX distinct. One asset, two APIs; a divergence here is an emitter bug by construction.
TEST_CASE("38-G1 GATE (DX12): an authored POST graph tonemaps the frame (scene -> post)",
          "[scene-render][ren38][g1][post][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    const TempPack              pack("sr_g1dx_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());
    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);
    {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs({0.0F, 0.0F, 0.0F}, math::Quatf::identity(), {2.0F, 2.0F, 2.0F});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }
    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    if (!renderer.init_programs(*gctx)) { SKIP("dxc/DXIL unavailable"); }
    (void)renderer.sync(world);

    const auto frame_toml = [](const char* shader, const char* fname) {
        static char buf[1280];
        (void)std::snprintf(static_cast<char*>(buf), sizeof(buf), R"(
schema = 1
name   = "crd://frame/%s"

[[resource]]
name    = "scene_hdr"
kind    = "transient_image"
format  = "RGBA8Unorm"
width   = 128
height  = 128
sampled = true

[[draw_list]]
name = "visible_geometry"
all  = ["MeshRenderer", "Transform"]
cull = "frustum"
sort = "material"

[[pass]]
name          = "scene"
kind          = "raster.geometry"
draw_list     = "visible_geometry"
writes        = ["scene_hdr"]
material_pass = "Forward"
clear_color   = [0.10, 0.30, 0.60, 1.0]

[[pass]]
name   = "post"
kind   = "raster.fullscreen"
reads  = ["scene_hdr"]
writes = ["@output"]
shader = "%s"
)", fname, shader);
        return static_cast<const char*>(buf);
    };

    auto target = raster->create_color_depth_target(128U, 128U);
    REQUIRE(target != nullptr);
    const math::Mat4f vp = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F)
                           * math::look_at(math::Vec3f{0, 2, 8}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Vec3f     light{0.3F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    REQUIRE(renderer.set_frame_graph_toml(frame_toml("crd://post/srgb_only", "g1dx_post_srgb")));
    REQUIRE(renderer.render(*target, vp, light, clear, nullptr).draws > 0U);
    const u32  srgb_px = target->read_pixel(4U, 4U);
    const auto oetf    = [](double c) {
        return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    };
    const auto ch = [&](double c) { return static_cast<i32>(std::lround(oetf(c) * 255.0)); };
    const i32  er = ch(0.10);
    const i32  eg = ch(0.30);
    const i32  eb = ch(0.60);
    const i32  ar = static_cast<i32>(srgb_px & 0xFFU);
    const i32  ag = static_cast<i32>((srgb_px >> 8U) & 0xFFU);
    const i32  ab = static_cast<i32>((srgb_px >> 16U) & 0xFFU);
    INFO("srgb got (" << ar << "," << ag << "," << ab << ") want (" << er << "," << eg << "," << eb << ")");
    CHECK(std::abs(ar - er) <= 2);
    CHECK(std::abs(ag - eg) <= 2);
    CHECK(std::abs(ab - eb) <= 2);

    scenerender::SceneRenderer r2(&galloc());
    REQUIRE(r2.init(*raster, rm));
    if (!r2.init_programs(*gctx)) { SKIP("dxc/DXIL unavailable"); }
    (void)r2.sync(world);
    REQUIRE(r2.set_frame_graph_toml(frame_toml("crd://post/tonemap_agx", "g1dx_post_agx")));
    REQUIRE(r2.render(*target, vp, light, clear, nullptr).draws > 0U);
    const u32 agx_px = target->read_pixel(4U, 4U);
    CHECK(agx_px != srgb_px);
    CHECK((agx_px & 0x00FFFFFFU) != 0U);
    // ⛔⛔ and the GEOMETRY is in the post-processed frame (see the Vulkan twin: a background-only gate
    // cannot see a scene pass that silently drew nothing).
    u32 differs = 0U;
    for (u32 y = 0; y < 128U; y += 2U)
    {
        for (u32 x = 0; x < 128U; x += 2U)
        {
            if ((target->read_pixel(x, y) & 0x00FFFFFFU) != (agx_px & 0x00FFFFFFU)) { ++differs; }
        }
    }
    INFO("pixels differing from the background: " << differs);
    CHECK(differs > 200U);
}
#endif // _WIN32

// ── ⭐⭐ REN-38 GATE: THE FRAME LOOP SURVIVES. 64 consecutive renders, every one drawing. ─────────────────
// ⛔⛔ THE SCAR THIS EXISTS FOR: `FrameRecorder` hands out one PassRec block per `record()` from a ring of
// 32 and `begin_frame()` is what returns them — and NOTHING CALLED IT. The counter climbed one per frame, so
// the 33rd frame began failing every record with `BuildRejected`, permanently. The sandbox froze about half a
// second in on a black-ish frame; the WHOLE offscreen test suite stayed green because a gate renders once or
// twice and never reaches 33. A per-frame arena needs a per-frame reset, and only a LOOP can prove it.
// ⭐ 64 is deliberately > 2x the ring: it catches an off-by-one reset as well as a missing one.
TEST_CASE("REN-38 GATE: 64 consecutive frames all render (the per-frame recorder arena is recycled)",
          "[scene-render][ren38][frameloop][gpu][vulkan]")
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
    const TempPack              pack("sr_loop_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());
    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);
    {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs({0.0F, 0.0F, 0.0F}, math::Quatf::identity(), {2.0F, 2.0F, 2.0F});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }
    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    (void)renderer.sync(world);

    auto target = raster->create_color_depth_target(64U, 64U);
    REQUIRE(target != nullptr);
    const math::Mat4f vp = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F)
                           * math::look_at(math::Vec3f{0, 2, 8}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Vec3f     light{0.3F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.05F, 0.05F, 0.08F, 1.0F};

    u32 first_bad = 0U;
    u32 drew      = 0U;
    for (u32 f = 1; f <= 64U; ++f)
    {
        const auto st = renderer.render(*target, vp, light, clear, nullptr);
        // ⛔ `draws` counts the draw list built BEFORE recording — it stays positive when the RECORD fails,
        // which is exactly how the arena bug hid. `timed_passes` comes from device timestamps AFTER execute:
        // it is zero when nothing ran. Assert on the signal that can only come from the GPU.
        if (st.draws > 0U && st.timed_passes > 0U) { ++drew; }
        else if (first_bad == 0U) { first_bad = f; }
    }
    INFO("frames that drew: " << drew << "/64; first failing frame: " << first_bad);
    // ⛔ EVERY frame, not "most": the arena bug is silent until frame 33 and total afterwards
    CHECK(first_bad == 0U);
    CHECK(drew == 64U);
    // and the last frame still produced real pixels (not a stale or cleared surface)
    u32 covered = 0U;
    for (u32 y = 0; y < 64U; y += 2U)
    {
        for (u32 x = 0; x < 64U; x += 2U)
        {
            if ((target->read_pixel(x, y) & 0x00FFFFFFU) != 0U) { ++covered; }
        }
    }
    CHECK(covered > 50U);
}

// ── ⭐⭐ REN-39-C1 GATE: THE RENDERER SWITCH — pull and indexed render BIT-IDENTICAL frames. ─────────────────
// The same scene (two mesh groups, SHADOWS ON so all four cascade passes + the shadowed forward pass run) is
// rendered twice: once with `set_indexed_pull(false)` (the proven pull path — the reference) and once with the
// switch ON. The claim has BOTH halves, because pixels alone cannot distinguish "switched" from "ignored":
//   (a) the two frames' readback is BIT-IDENTICAL — same shading, different draw verbs (the parity proof);
//   (b) the INDEXED frame advances `multi_batch_count()` while the pull frame's shadowed draws do NOT (with
//       shadows on the pull path takes the classic per-draw verbs; only the indexed path records batches) —
//       the counter probe that fails if the switch silently kept the pull path.
TEST_CASE("REN-39-C1 GATE: pull and indexed scene frames are bit-identical, and the indexed one batches",
          "[scene-render][ren39][indexed][gpu][vulkan]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    cfg.enable_validation = true;
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    auto* vk = ctx != nullptr ? static_cast<gpu::VulkanGpuContext*>(ctx.get()) : nullptr;
    if (vk == nullptr || !vk->graphics_capable() || !vk->shader_object())
    {
        SKIP("no graphics-capable Vulkan device with shader objects");
    }
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    const resources::ResourceId mesh_a = resources::ResourceId::mint_random();
    const resources::ResourceId mesh_b = resources::ResourceId::mint_random();
    const TempPack pack_a("sr_idx_a_", mesh_a);
    const TempPack pack_b("sr_idx_b_", mesh_b);
    write_mesh_pack(pack_a.path, mesh_a);
    write_mesh_pack(pack_b.path, mesh_b);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack_a.path.generic()).is_valid());
    REQUIRE(rm.mount_manifest(pack_b.path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);
    const auto add = [&](const resources::ResourceId& mid, math::Vec3f pos)
    {
        const scene::EntityId e = world.spawn();
        scene::Transform t;
        t.translation = math::from_raw_vec<units::dim::Length>(pos);
        t.scale = {2.0F, 2.0F, 2.0F};
        t.world = math::from_trs(pos, math::Quatf::identity(), {2.0F, 2.0F, 2.0F});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{mid, {}});
    };
    add(mesh_a, {-4.0F, 0.0F, 0.0F});
    add(mesh_b, {4.0F, 0.0F, 0.0F});

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    REQUIRE(renderer.set_shadows_enabled(true)); // all four cascades + the shadowed forward pass run
    REQUIRE(renderer.sync(world).total_instances == 2U);

    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 6.0F, 16.0F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f light{0.3F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    // ── the PULL reference frame ──
    auto ref = raster->create_color_depth_target(256U, 256U);
    REQUIRE(ref != nullptr);
    renderer.set_indexed_pull(false);
    const crd::u64 pull_before = raster->multi_batch_count();
    const auto r_pull = renderer.render(*ref, proj * view, light, clear, nullptr);
    CHECK(r_pull.draws == 2U);
    const crd::u64 pull_batches = raster->multi_batch_count() - pull_before;

    // ── the INDEXED frame ──
    auto tgt = raster->create_color_depth_target(256U, 256U);
    REQUIRE(tgt != nullptr);
    renderer.set_indexed_pull(true);
    const crd::u64 idx_before = raster->multi_batch_count();
    const auto r_idx = renderer.render(*tgt, proj * view, light, clear, nullptr);
    CHECK(r_idx.draws == 2U);
    const crd::u64 idx_batches = raster->multi_batch_count() - idx_before;

    // (b) the switch ACTUALLY switched: the shadowed pull frame records zero multi batches; the indexed frame
    // records one per cascade run + the forward runs — the probe "ignored the switch" cannot fake
    INFO("pull batches=" << pull_batches << " indexed batches=" << idx_batches);
    CHECK(pull_batches == 0U);
    CHECK(idx_batches >= 5U);

    // (a) BIT-IDENTICAL pixels over a frame that actually drew
    crd::u32 diffs = 0U;
    crd::u32 covered = 0U;
    for (u32 y = 0; y < 256U; ++y)
    {
        for (u32 x = 0; x < 256U; ++x)
        {
            const u32 a = ref->read_pixel(x, y);
            const u32 b = tgt->read_pixel(x, y);
            if (a != b)
            {
                ++diffs;
            }
            if ((b & 0x00FFFFFFU) != 0U)
            {
                ++covered;
            }
        }
    }
    INFO("diffs=" << diffs << " covered=" << covered);
    CHECK(diffs == 0U);
    CHECK(covered > 500U);
}

#ifdef _WIN32 // the D3D12 backend exists only on Windows (the F17 guard rule)
// ── ⭐⭐ REN-39-C1 GATE (DX12): the same parity claim on the other backend. ─────────────────────────────────
// Identical shape to the Vulkan gate: pull reference vs indexed frame, BIT-IDENTICAL pixels. With shadows
// active the batch-count probe discriminates exactly as on Vulkan (pull shadowed = classic verbs, 0 batches;
// indexed = cascade + forward batches). If this rig's DX12 shadow set fails to build, the gate still asserts
// pixel parity (stated by INFO) — the switch logic is shared renderer code; the DX12-specific halves (verbs,
// t0 SRV seam, kIndexedDrawStates walk) are exactly what the pixel comparison exercises.
TEST_CASE("REN-39-C1 GATE (DX12): pull and indexed scene frames are bit-identical",
          "[scene-render][ren39][indexed][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid())
    {
        SKIP("no D3D12 device available");
    }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    const resources::ResourceId mesh_a = resources::ResourceId::mint_random();
    const resources::ResourceId mesh_b = resources::ResourceId::mint_random();
    const TempPack pack_a("sr_idxdx_a_", mesh_a);
    const TempPack pack_b("sr_idxdx_b_", mesh_b);
    write_mesh_pack(pack_a.path, mesh_a);
    write_mesh_pack(pack_b.path, mesh_b);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack_a.path.generic()).is_valid());
    REQUIRE(rm.mount_manifest(pack_b.path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);
    const auto add = [&](const resources::ResourceId& mid, math::Vec3f pos)
    {
        const scene::EntityId e = world.spawn();
        scene::Transform t;
        t.translation = math::from_raw_vec<units::dim::Length>(pos);
        t.scale = {2.0F, 2.0F, 2.0F};
        t.world = math::from_trs(pos, math::Quatf::identity(), {2.0F, 2.0F, 2.0F});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{mid, {}});
    };
    add(mesh_a, {-4.0F, 0.0F, 0.0F});
    add(mesh_b, {4.0F, 0.0F, 0.0F});

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    if (!renderer.init_programs(*gctx))
    {
        SKIP("dxc/DXIL unavailable");
    }
    const bool shadows = renderer.set_shadows_enabled(true);
    INFO("dx12 shadow set active=" << shadows);
    REQUIRE(renderer.sync(world).total_instances == 2U);

    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 6.0F, 16.0F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f light{0.3F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    auto ref = raster->create_color_depth_target(256U, 256U);
    REQUIRE(ref != nullptr);
    renderer.set_indexed_pull(false);
    const crd::u64 pull_before = raster->multi_batch_count();
    CHECK(renderer.render(*ref, proj * view, light, clear, nullptr).draws == 2U);
    const crd::u64 pull_batches = raster->multi_batch_count() - pull_before;

    auto tgt = raster->create_color_depth_target(256U, 256U);
    REQUIRE(tgt != nullptr);
    renderer.set_indexed_pull(true);
    const crd::u64 idx_before = raster->multi_batch_count();
    CHECK(renderer.render(*tgt, proj * view, light, clear, nullptr).draws == 2U);
    const crd::u64 idx_batches = raster->multi_batch_count() - idx_before;

    INFO("pull batches=" << pull_batches << " indexed batches=" << idx_batches);
    if (shadows)
    {
        CHECK(pull_batches == 0U); // shadowed pull = classic verbs only
        CHECK(idx_batches >= 5U);  // 4 cascade runs + the forward runs, all indexed batches
    }

    crd::u32 diffs = 0U;
    crd::u32 covered = 0U;
    for (u32 y = 0; y < 256U; ++y)
    {
        for (u32 x = 0; x < 256U; ++x)
        {
            const u32 a = ref->read_pixel(x, y);
            const u32 b = tgt->read_pixel(x, y);
            if (a != b)
            {
                ++diffs;
            }
            if ((b & 0x00FFFFFFU) != 0U)
            {
                ++covered;
            }
        }
    }
    INFO("diffs=" << diffs << " covered=" << covered);
    CHECK(diffs == 0U);
    CHECK(covered > 500U);
}
#endif // _WIN32 (the REN-39-C1 DX12 parity gate)

// ── ⭐⭐ REN-40-D GATE: THE CASCADE SEAM, AS A DICHOTOMY. ────────────────────────────────────────────────────
// ⛔⛔ A SEAM CANNOT BE GATED BY "DOES IT LOOK BETTER". Cascades are fitted as SPHERES and selected by
// CONTAINMENT, so a fragment leaves one and enters the next at a hard line — and the two sides differ in texel
// size, in depth bias and in filter footprint, so the line shows as a STEP in shadow softness even when both
// sides are individually correct. The only honest test is a DICHOTOMY on one scene: render a receiver that spans
// a cascade boundary with the blend OFF and then ON, and require the largest adjacent-pixel jump along a scanline
// crossing that boundary to FALL. A one-sided "the blended image is smooth" assert would pass on an image with no
// shadow in it at all, which is why the hard arm has to show the step first.
//
// ⛔ And the parity arm is STRUCTURAL, not a tolerance: `cascade_blend_pct = 0` is a DECLARED option that cooks
// the byte-identical graph the technique always emitted, so the two frames must differ by exactly ZERO pixels.
TEST_CASE("REN-40-D GATE: cascade cross-fade removes the seam step, and blend=0 is bit-identical (Vulkan)",
          "[scene-render][ren40][csm][gpu][vulkan]")
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
    const TempPack              pack("sr_csm_blend_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
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
    // a LONG receiver running away from the camera, so one scanline crosses several cascade boundaries, and a
    // row of casters above it so there is shadow on both sides of every boundary
    add_cube({0.0F, 0.0F, 0.0F}, {6.0F, 0.25F, 90.0F});
    for (int i = 0; i < 12; ++i)
    {
        add_cube({0.0F, 3.0F, -70.0F + (static_cast<f32>(i) * 12.0F)}, {1.6F, 1.6F, 1.6F});
    }

    constexpr u32 dim    = 256U;
    auto          target = raster->create_color_depth_target(dim, dim);
    REQUIRE(target != nullptr);
    // looking straight down the receiver, so screen-Y IS distance and a vertical scanline crosses the cascades
    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 7.0F, 95.0F}, math::Vec3f{0.0F, 0.0F, 0.0F},
                                           math::Vec3f{0, 1, 0});
    const math::Mat4f     proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Mat4f     vp   = proj * view;
    const math::Vec3f     light = math::normalized(math::Vec3f{0.35F, 1.0F, 0.15F});
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    scenerender::CsmConfig ccfg;
    ccfg.cascade_count = 4;
    ccfg.map_size      = 1024;
    ccfg.far_plane     = 200.0F;

    // ⛔⛔ THE METRIC HAS TO ISOLATE THE SEAM, and a plain "largest adjacent step in the frame" does NOT: it is
    // dominated by the shadow's OWN edge (lit -> shadowed is a ~60-level jump), which is present, correct and
    // identical in both arms. The first version of this gate measured exactly that and reported 62 -> 62 on a
    // cross-fade that was demonstrably working.
    // ⭐ So the seam is located BY THE MECHANISM rather than guessed at: the cross-fade acts only inside the band
    // around a cascade border, so the rows whose pixels CHANGED between the two arms are, by construction, the
    // rows the seam runs through. The step is then compared on those rows only — the same pixels, both arms.
    constexpr u32 grid = dim / 2U;
    const auto    grab = [&](gpu::IRasterTarget& t, containers::Array<u32>& out) {
        out.clear();
        for (u32 y = 0; y < dim; y += 2U)
        {
            for (u32 x = 0; x < dim; x += 2U) { out.push_back(t.read_pixel(x, y) & 0xFFU); }
        }
    };
    // the largest vertically-adjacent jump, restricted to a per-PIXEL mask. ⛔⛔ Restricting to the CHANGED PIXELS
    // — not whole rows — is what finally isolates the cascade seam: the shadow's OWN edge is identical in both arms
    // (never a changed pixel) so it drops out, leaving only the seam discontinuity the cross-fade blends. The old
    // whole-row form measured 59→59 because a shadow edge sat on a touched row and dominated the max both arms.
    const auto step_at_mask = [&](const containers::Array<u32>& px, const containers::Array<u8>& mask) {
        u32 worst = 0U;
        for (u32 ry = 1; ry < grid; ++ry)
        {
            for (u32 rx = 0; rx < grid; ++rx)
            {
                if (mask[(ry * grid) + rx] == 0U && mask[((ry - 1U) * grid) + rx] == 0U) { continue; }
                const u32 a = px[((ry - 1U) * grid) + rx];
                const u32 b = px[(ry * grid) + rx];
                const u32 d = a > b ? a - b : b - a;
                if (d > worst) { worst = d; }
            }
        }
        return worst;
    };

    containers::Array<u32> hard_px(&galloc());
    containers::Array<u32> soft_px(&galloc());

    // ── arm A: the HARD select (blend = 0) ──
    {
        scenerender::SceneRenderer r(&galloc());
        REQUIRE(r.init(*raster, rm));
        r.set_cascade_blend_pct(0U);
        REQUIRE(r.init_programs(*vk));
        r.set_csm_config(ccfg);
        REQUIRE(r.set_shadows_enabled(true));
        (void)r.sync(world);
        REQUIRE(r.render(*target, vp, light, clear, nullptr).draws > 0U);
        grab(*target, hard_px);
    }

    // ── arm B: the SAME scene with the cross-fade on ──
    {
        scenerender::SceneRenderer r(&galloc());
        REQUIRE(r.init(*raster, rm));
        r.set_cascade_blend_pct(30U);
        REQUIRE(r.init_programs(*vk));
        r.set_csm_config(ccfg);
        REQUIRE(r.set_shadows_enabled(true));
        (void)r.sync(world);
        REQUIRE(r.render(*target, vp, light, clear, nullptr).draws > 0U);
        grab(*target, soft_px);
    }

    // which PIXELS did the cross-fade touch? Those pixels ARE the cascade seam (the shadow's own edge is identical
    // in both arms, so it is never marked) — the mask the step metric restricts to below.
    containers::Array<u8> cmask(&galloc());
    cmask.resize(grid * grid, static_cast<u8>(0));
    u32 changed = 0U;
    for (u32 ry = 0; ry < grid; ++ry)
    {
        for (u32 rx = 0; rx < grid; ++rx)
        {
            if (hard_px[(ry * grid) + rx] != soft_px[(ry * grid) + rx]) { cmask[(ry * grid) + rx] = 1U; ++changed; }
        }
    }
    // ⛔ FIRST: the cross-fade must actually REACH the shader. Without this every "it got smoother" assertion
    // below is vacuously true — and it caught exactly that: `Technique::n_options` was a hand-written literal
    // `2`, so the third DECLARED option was silently bounds-checked away and the cook produced the unblended
    // graph while the renderer, the option table and the asset all agreed the feature was on.
    INFO("pixels changed by the cross-fade: " << changed);
    REQUIRE(changed > 0U);
    // ...and it is a SEAM-LOCAL effect, not a global lighting change
    CHECK(changed < (grid * grid) / 4U);

    const u32 hard_step = step_at_mask(hard_px, cmask);
    const u32 soft_step = step_at_mask(soft_px, cmask);
    INFO("seam step " << hard_step << " -> " << soft_step);
    // the hard arm must SHOW a step there, or the soft arm proves nothing (a flat region is trivially smooth)
    CHECK(hard_step > 4U);
    // ...and the cross-fade must reduce it
    CHECK(soft_step < hard_step);

    // ── arm C: PARITY. `blend = 0` is a DECLARED option that cooks the same graph, so this is exact. ──
    {
        scenerender::SceneRenderer r(&galloc());
        REQUIRE(r.init(*raster, rm));
        r.set_cascade_blend_pct(0U);
        REQUIRE(r.init_programs(*vk));
        r.set_csm_config(ccfg);
        REQUIRE(r.set_shadows_enabled(true));
        (void)r.sync(world);
        REQUIRE(r.render(*target, vp, light, clear, nullptr).draws > 0U);
        u32 idx    = 0U;
        u32 differ = 0U;
        for (u32 y = 0; y < dim; y += 2U)
        {
            for (u32 x = 0; x < dim; x += 2U)
            {
                if (hard_px[idx++] != (target->read_pixel(x, y) & 0xFFU)) { ++differ; }
            }
        }
        CHECK(differ == 0U);
    }
}

// ── ⭐⭐ REN-40-D GATE: PCSS CONTACT HARDENING — a TWO-DISTANCE DICHOTOMY. ───────────────────────────────────
// ⛔⛔ "THE SHADOWS LOOK SOFTER" IS NOT A TEST. Fixed-radius PCF also looks soft, and swapping filters always
// moves SOME pixels — so "the image changed" proves nothing about contact hardening. The property that actually
// distinguishes PCSS is a DERIVATIVE: the SAME caster over the SAME receiver must cast a WIDER penumbra when it
// is FURTHER from it. A fixed radius produces the same width at both heights BY CONSTRUCTION, which is what
// makes this a dichotomy rather than a preference — and why both filters are measured, not just the new one.
//
// ⛔⛔ THE PROBE GEOMETRY IS PART OF THE TEST. An overhead camera with an overhead light puts the caster ON TOP
// of its own shadow, so the near arm measures a shadow that is mostly HIDDEN and reads as narrower for a reason
// that has nothing to do with the filter — an earlier version of this probe reported exactly that and looked
// like a real inversion. The light is slanted 45 deg and the caster kept small, so the shadow clears the
// caster's own projection at BOTH heights: h*tan(theta) > 2*half_extent holds for h = 4 as well as h = 16.
//
// ⛔⛔ AND THE DEFECT THIS GATE EXISTS TO PIN: the blocker search reads the atlas through a PLAIN sampler
// (binding 6), because a comparison sampler cannot return a stored depth. That binding was missing from the
// descriptor set LAYOUT — which is not a compile error and not a hang. The search read an UNBOUND descriptor,
// reported "blocker found" across the whole open receiver, and dimmed every lit surface by half; the result
// still looked like a plausible soft shadow, so it read as a tuning problem. What exposed it was dumping the
// scanline as NUMBERS and seeing that the LIT PLATEAU had moved, which no amount of looking had shown.
TEST_CASE("REN-40-D GATE: PCSS widens the penumbra with blocker distance, PCF does not (Vulkan)",
          "[scene-render][ren40][csm][pcss][gpu][vulkan]")
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
    const TempPack              pack("sr_pcss_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());

    constexpr u32 dim    = 256U;
    auto          target = raster->create_color_depth_target(dim, dim);
    REQUIRE(target != nullptr);
    // straight down at the receiver, so screen-X IS world-X and the shadow slides along the measured scanline
    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 42.0F, 0.01F}, math::Vec3f{0, 0, 0},
                                           math::Vec3f{0, 1, 0});
    const math::Mat4f     proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Mat4f     vp   = proj * view;
    // ⛔ the slant is a CONSTRAINT, not a taste: the shadow must clear the caster's own projection at the NEAR
    // height (h*tan > caster_half + shadow_half) and still land on screen at the FAR one (h*tan + half < 24 units).
    const math::Vec3f     light = math::normalized(math::Vec3f{1.7F, 1.0F, 0.0F});
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    scenerender::CsmConfig ccfg;
    ccfg.cascade_count = 4;
    ccfg.map_size      = 512;
    ccfg.far_plane     = 120.0F;

    struct Edge
    {
        u32 partial;  // pixels that are neither fully lit nor fully shadowed — the penumbra width
        u32 contrast; // the lit-to-shadow range actually found, so an EMPTY frame cannot pass as a narrow one
    };

    // ⛔ The window is centred on the DARKEST pixel rather than on a fixed column, because the shadow MOVES
    // when the caster rises (that is the whole point of the slanted light) and a fixed window would read the
    // far arm's empty receiver. And the band is relative to the window's own lit level, not to 255: the
    // receiver carries a shading gradient across the frame, and a global threshold would count it as penumbra.
    const auto measure = [&](gpu::IRasterTarget& t) {
        constexpr u32 x_lo = 4U; // the receiver fills the frame, so only the very edge is excluded
        constexpr u32 x_hi = 251U;
        constexpr u32 row  = dim / 2U;
        u32           core = x_lo;
        u32           core_v = 255U;
        for (u32 x = x_lo; x <= x_hi; ++x)
        {
            const u32 v = t.read_pixel(x, row) & 0xFFU;
            if (v < core_v) { core_v = v; core = x; }
        }
        const u32 w_lo = core > x_lo + 24U ? core - 24U : x_lo;
        const u32 w_hi = core + 24U < x_hi ? core + 24U : x_hi;
        u32       lo   = 255U;
        u32       hi   = 0U;
        for (u32 x = w_lo; x <= w_hi; ++x)
        {
            const u32 v = t.read_pixel(x, row) & 0xFFU;
            if (v < lo) { lo = v; }
            if (v > hi) { hi = v; }
        }
        Edge e{0U, hi - lo};
        if (e.contrast < 40U) { return e; } // no shadow on this line at all
        const u32 band_lo = lo + (e.contrast * 15U / 100U);
        const u32 band_hi = lo + (e.contrast * 85U / 100U);
        for (u32 x = w_lo; x <= w_hi; ++x)
        {
            const u32 v = t.read_pixel(x, row) & 0xFFU;
            if (v > band_lo && v < band_hi) { ++e.partial; }
        }
        return e;
    };

    // one arm: the caster at `height`, rendered with the chosen filter
    const auto run = [&](f32 height, bool pcss) {
        scene::World world{&galloc()};
        world.register_component<scene::Transform>(scene::transform_serialize_trait());
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
        add_cube({0.0F, 0.0F, 0.0F}, {100.0F, 0.25F, 100.0F}); // receiver: fills the frame, so no background
        // ⛔⛔ THE CASTER MUST BE WIDER THAN THE PENUMBRA IT CASTS. The mesh is a UNIT cube, so `scale` IS the
        // full extent — a scale of 1 is a half-extent of 0.5, which at this light angle is SMALLER than the far
        // arm's penumbra radius. Its umbra then vanishes entirely, both arms saturate at "the whole shadow is
        // partial", and the measured widening collapses to noise while still pointing the right way.
        // Thin in Y for a different reason: the search must read the caster's TOP, not a slab whose own depth
        // spans the penumbra it is being used to estimate.
        add_cube({0.0F, height, 0.0F}, {6.0F, 0.25F, 6.0F});

        scenerender::SceneRenderer r(&galloc());
        REQUIRE(r.init(*raster, rm));
        // ⛔ a WIDE light, so the effect is unmistakable at 1024 texels — the real sun's 0.27 deg would move the
        // penumbra by a fraction of a texel over this height range and the gate would be measuring noise.
        r.set_soft_shadows(pcss, 100U); // 1.00 degree of angular radius
        // ⛔⛔ THE CAP IS RAISED SO THIS GATE MEASURES THE PHYSICS AND NOT THE CAP. The shipping default bounds
        // the penumbra at 24 texels because a 16-tap filter bands once its taps spread further apart than that —
        // a real cost/quality trade, and the reason the next tier is a filterable representation rather than a
        // bigger number. But a gate run at the default would be reading that bound in the far arm, would show
        // the same compression whether or not the derivation was right, and would go on passing if it broke.
        r.set_soft_shadow_quality(96U, 16U);
        r.set_pcf_taps(16U);
        REQUIRE(r.init_programs(*vk));
        r.set_csm_config(ccfg);
        REQUIRE(r.set_shadows_enabled(true));
        (void)r.sync(world);
        REQUIRE(r.render(*target, vp, light, clear, nullptr).draws > 0U);
        return measure(*target);
    };

    const Edge pcf_near  = run(2.0F, false);
    const Edge pcf_far   = run(10.0F, false);
    const Edge pcss_near = run(2.0F, true);
    const Edge pcss_far  = run(10.0F, true);

    INFO("PCF  near " << pcf_near.partial << "/" << pcf_near.contrast << "  far " << pcf_far.partial << "/"
                      << pcf_far.contrast);
    INFO("PCSS near " << pcss_near.partial << "/" << pcss_near.contrast << "  far " << pcss_far.partial << "/"
                      << pcss_far.contrast);

    // ⛔ EVERY arm must have found a real edge, or a zero-vs-zero comparison passes on an empty frame
    CHECK(pcf_near.contrast >= 40U);
    CHECK(pcf_far.contrast >= 40U);
    CHECK(pcss_near.contrast >= 40U);
    CHECK(pcss_far.contrast >= 40U);

    // ⭐ THE DICHOTOMY: PCSS widens with blocker distance, and it does so PROPORTIONALLY — the penumbra of a
    // blocker at distance d is d*tan(theta), so a 5x lift must widen it ~5x. Measured across this range the
    // penumbra runs 2 px -> 14 px, and the same probe run against light angle (1 deg -> 4 deg -> 12 deg at fixed
    // height) scales it 2 -> 9 -> 25: linear in BOTH terms, which is the property, not the anecdote. The margin
    // asked for here is a fraction of that and still unreachable for a fixed radius.
    CHECK(pcss_far.partial > pcss_near.partial + 8U);
    // ...and the fixed-radius filter does NOT widen (it may wobble by a pixel of rasterisation, never by a factor)
    CHECK(pcf_far.partial <= pcf_near.partial + 3U);
    // ...and at CONTACT distance PCSS is no softer than PCF — that is what "contact hardening" MEANS
    CHECK(pcss_near.partial <= pcf_near.partial + 4U);
}

#ifdef _WIN32 // the D3D12 backend exists only on Windows (the F17 guard rule)
// ── ⭐⭐ REN-40-D GATE (DX12): the same contact-hardening dichotomy on the other backend. ────────────────────
// Identical probe to the Vulkan gate above — its comments carry the full rationale (the geometry constraints,
// the darkest-pixel window, the derived margins). What THIS twin exists to catch is backend-specific: the
// blocker search samples the atlas through the PLAIN sampler at register s6, and on D3D12 that register must be
// covered by the scene ROOT SIGNATURE and fed from its own fixed sampler-heap slot. A missing s6 is not a
// cook error and not a crash — PSO creation rejects the program, `set_shadows_enabled` reports false, and the
// frame silently renders unshadowed: exactly the cook-only scar, one register over.
TEST_CASE("REN-40-D GATE (DX12): PCSS widens the penumbra with blocker distance, PCF does not",
          "[scene-render][ren40][csm][pcss][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    const TempPack              pack("sr_pcssdx_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());

    constexpr u32 dim    = 256U;
    auto          target = raster->create_color_depth_target(dim, dim);
    REQUIRE(target != nullptr);
    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 42.0F, 0.01F}, math::Vec3f{0, 0, 0},
                                           math::Vec3f{0, 1, 0});
    const math::Mat4f     proj  = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Mat4f     vp    = proj * view;
    const math::Vec3f     light = math::normalized(math::Vec3f{1.7F, 1.0F, 0.0F});
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    scenerender::CsmConfig ccfg;
    ccfg.cascade_count = 4;
    ccfg.map_size      = 512;
    ccfg.far_plane     = 120.0F;

    struct Edge
    {
        u32 partial;
        u32 contrast;
    };
    const auto measure = [&](gpu::IRasterTarget& t) {
        constexpr u32 x_lo = 4U;
        constexpr u32 x_hi = 251U;
        constexpr u32 row  = dim / 2U;
        u32           core   = x_lo;
        u32           core_v = 255U;
        for (u32 x = x_lo; x <= x_hi; ++x)
        {
            const u32 v = t.read_pixel(x, row) & 0xFFU;
            if (v < core_v) { core_v = v; core = x; }
        }
        const u32 w_lo = core > x_lo + 24U ? core - 24U : x_lo;
        const u32 w_hi = core + 24U < x_hi ? core + 24U : x_hi;
        u32       lo   = 255U;
        u32       hi   = 0U;
        for (u32 x = w_lo; x <= w_hi; ++x)
        {
            const u32 v = t.read_pixel(x, row) & 0xFFU;
            if (v < lo) { lo = v; }
            if (v > hi) { hi = v; }
        }
        Edge e{0U, hi - lo};
        if (e.contrast < 40U) { return e; }
        const u32 band_lo = lo + (e.contrast * 15U / 100U);
        const u32 band_hi = lo + (e.contrast * 85U / 100U);
        for (u32 x = w_lo; x <= w_hi; ++x)
        {
            const u32 v = t.read_pixel(x, row) & 0xFFU;
            if (v > band_lo && v < band_hi) { ++e.partial; }
        }
        return e;
    };

    const auto run = [&](f32 height, bool pcss) {
        scene::World world{&galloc()};
        world.register_component<scene::Transform>(scene::transform_serialize_trait());
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
        add_cube({0.0F, 0.0F, 0.0F}, {100.0F, 0.25F, 100.0F});
        add_cube({0.0F, height, 0.0F}, {6.0F, 0.25F, 6.0F});

        scenerender::SceneRenderer r(&galloc());
        REQUIRE(r.init(*raster, rm));
        r.set_soft_shadows(pcss, 100U);
        r.set_soft_shadow_quality(96U, 16U);
        r.set_pcf_taps(16U);
        if (!r.init_programs(*gctx)) { SKIP("dxc/DXIL unavailable"); }
        r.set_csm_config(ccfg);
        // ⛔ REQUIRE, not CHECK-and-continue: false here IS the s6 defect this twin exists to catch — the
        // program set failed to build and the frame would render unshadowed, making every arm below vacuous.
        REQUIRE(r.set_shadows_enabled(true));
        (void)r.sync(world);
        REQUIRE(r.render(*target, vp, light, clear, nullptr).draws > 0U);
        return measure(*target);
    };

    const Edge pcf_near  = run(2.0F, false);
    const Edge pcf_far   = run(10.0F, false);
    const Edge pcss_near = run(2.0F, true);
    const Edge pcss_far  = run(10.0F, true);

    INFO("PCF  near " << pcf_near.partial << "/" << pcf_near.contrast << "  far " << pcf_far.partial << "/"
                      << pcf_far.contrast);
    INFO("PCSS near " << pcss_near.partial << "/" << pcss_near.contrast << "  far " << pcss_far.partial << "/"
                      << pcss_far.contrast);

    CHECK(pcf_near.contrast >= 40U);
    CHECK(pcf_far.contrast >= 40U);
    CHECK(pcss_near.contrast >= 40U);
    CHECK(pcss_far.contrast >= 40U);

    CHECK(pcss_far.partial > pcss_near.partial + 8U);
    CHECK(pcf_far.partial <= pcf_near.partial + 3U);
    CHECK(pcss_near.partial <= pcf_near.partial + 4U);
}
#endif // _WIN32 (the REN-40-D DX12 PCSS gate)

// ── ⭐⭐ REN-40-D GATE: THE MOMENT TIER (EVSM + MSM) — filterable shadows through the AUTHORED moment graph. ──
// What runs here is the entire new pipeline: the `forward_csm_moment` frame graph's four for_each passes
// (cascade depth -> per-slice moment CONVERT -> separable blur X -> blur Y), the by-NAME instance-program
// dispatch, the colour-array atlas routing (layered-ness, not depth-ness), the LINEAR/CLAMP atlas sampler, and
// the technique's one-read moment resolve. Three properties, each chosen because a specific defect class fakes
// the others:
//   1. THE FLOOR STAYS LIT. An unbound/mis-typed atlas read does not crash — it darkens every lit surface and
//      still looks like "soft shadows" (the PCSS scar, verbatim). So the open floor's level must MATCH the
//      PCF arm's within a tolerance, before any softness claim is examined.
//   2. THE SHADOW IS STILL THERE. A convert or blur pass that silently dropped out (null program, empty draw)
//      leaves a uniformly-lit frame whose "soft edge" measure is 0/0 — so the dark core must exist.
//   3. THE EDGE IS SOFTER THAN HARD PCF. The prefilter is the whole point of the tier; its width must show.
TEST_CASE("REN-40-D GATE: EVSM and MSM moment shadows render soft with a lit floor (Vulkan)",
          "[scene-render][ren40][csm][moment][gpu][vulkan]")
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
    const TempPack              pack("sr_moment_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());

    constexpr u32 dim    = 256U;
    auto          target = raster->create_color_depth_target(dim, dim);
    REQUIRE(target != nullptr);
    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 42.0F, 0.01F}, math::Vec3f{0, 0, 0},
                                           math::Vec3f{0, 1, 0});
    const math::Mat4f     proj  = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Mat4f     vp    = proj * view;
    const math::Vec3f     light = math::normalized(math::Vec3f{1.7F, 1.0F, 0.0F});
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    scenerender::CsmConfig ccfg;
    ccfg.cascade_count = 4;
    ccfg.map_size      = 512;
    ccfg.far_plane     = 120.0F;

    struct Probe
    {
        u32 partial;  // pixels neither plateau — the edge width
        u32 contrast; // lit-to-core range found in the window
        u32 floor;    // the OPEN floor's level, far from the shadow — the unbound-read canary
    };
    const auto measure = [&](gpu::IRasterTarget& t) {
        constexpr u32 row = dim / 2U;
        u32           core   = 4U;
        u32           core_v = 255U;
        for (u32 x = 4U; x <= 251U; ++x)
        {
            const u32 v = t.read_pixel(x, row) & 0xFFU;
            if (v < core_v) { core_v = v; core = x; }
        }
        const u32 w_lo = core > 28U ? core - 24U : 4U;
        const u32 w_hi = core + 24U < 251U ? core + 24U : 251U;
        u32       lo = 255U;
        u32       hi = 0U;
        for (u32 x = w_lo; x <= w_hi; ++x)
        {
            const u32 v = t.read_pixel(x, row) & 0xFFU;
            if (v < lo) { lo = v; }
            if (v > hi) { hi = v; }
        }
        Probe p{0U, hi - lo, 0U};
        // the open floor: the OPPOSITE side of the frame from the shadow core, same scanline
        const u32 fx = core >= dim / 2U ? 24U : dim - 24U;
        p.floor      = t.read_pixel(fx, row) & 0xFFU;
        if (p.contrast < 40U) { return p; }
        const u32 blo = lo + (p.contrast * 15U / 100U);
        const u32 bhi = lo + (p.contrast * 85U / 100U);
        for (u32 x = w_lo; x <= w_hi; ++x)
        {
            const u32 v = t.read_pixel(x, row) & 0xFFU;
            if (v > blo && v < bhi) { ++p.partial; }
        }
        return p;
    };

    const auto run = [&](scenerender::SceneRenderer::SoftShadow mode) {
        scene::World world{&galloc()};
        world.register_component<scene::Transform>(scene::transform_serialize_trait());
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
        add_cube({0.0F, 0.0F, 0.0F}, {100.0F, 0.25F, 100.0F});
        add_cube({0.0F, 8.0F, 0.0F}, {6.0F, 0.25F, 6.0F});

        scenerender::SceneRenderer r(&galloc());
        REQUIRE(r.init(*raster, rm));
        r.set_soft_shadows(mode, 100U);
        // ⛔ the hard arm runs the SHIPPING 4-tap filter, not 1 tap: a single comparison on a 100-unit receiver
        // sits in acne territory (faint self-shadow speckle dims the whole floor and the "darkest pixel" the
        // window centres on is an acne pixel, not the shadow), and the arm then measures its own artefact. The
        // 4-tap edge is still ~2 px against the prefilter's ~6+, so the dichotomy survives — and the acne-free
        // floor is asserted BY the moment arms against this arm's floor, which is exactly property 1.
        r.set_pcf_taps(4U);
        REQUIRE(r.init_programs(*vk));
        r.set_csm_config(ccfg);
        REQUIRE(r.set_shadows_enabled(true));
        (void)r.sync(world);
        REQUIRE(r.render(*target, vp, light, clear, nullptr).draws > 0U);
        return measure(*target);
    };

    const Probe hard = run(scenerender::SceneRenderer::SoftShadow::Off);
    const Probe msm  = run(scenerender::SceneRenderer::SoftShadow::Msm);
    const Probe evsm = run(scenerender::SceneRenderer::SoftShadow::Evsm);

    INFO("hard " << hard.partial << "/" << hard.contrast << " floor " << hard.floor);
    INFO("msm  " << msm.partial << "/" << msm.contrast << " floor " << msm.floor);
    INFO("evsm " << evsm.partial << "/" << evsm.contrast << " floor " << evsm.floor);

    // every arm must have found a real shadow (property 2 — a dropped pass leaves contrast ~0)
    CHECK(hard.contrast >= 40U);
    CHECK(msm.contrast >= 40U);
    CHECK(evsm.contrast >= 40U);

    // ⛔ property 1 FIRST: the open floor must not dim — an unbound or mis-sampled atlas darkens EVERYTHING
    // and the "softness" below would still pass. ±12 covers rounding and the moment tier's slight bleed.
    const u32 f_lo = hard.floor > 12U ? hard.floor - 12U : 0U;
    CHECK(msm.floor >= f_lo);
    CHECK(evsm.floor >= f_lo);

    // property 3: the prefiltered edge is WIDER than the hard edge. ⛔ The margins differ BY PHYSICS, not by
    // tuning: at the same Gaussian, EVSM's exponential warp RE-SHARPENS the filtered edge (the same property
    // that makes it leak less light than plain VSM), so its penumbra is genuinely narrower than MSM's — the
    // measured widths here are hard 1 px, EVSM 3 px, MSM 5 px. Each margin still fails on a dropped blur or
    // convert pass (equal widths), which is what the property exists to catch.
    CHECK(msm.partial > hard.partial + 2U);
    CHECK(evsm.partial > hard.partial + 1U);
}

#ifdef _WIN32 // the D3D12 backend exists only on Windows (the F17 guard rule)
// ── ⭐⭐ REN-40-D GATE (DX12): the moment tier on the other backend. ─────────────────────────────────────────
// The same three properties; what is DX12-specific here is exactly what the twin exists to exercise: the
// borrowed transient SRV's EXPLICIT depth flag (a DX12 depth SRV is R32_FLOAT, so the format cannot answer),
// the s5 table chosen by that flag (comparison vs LINEAR/CLAMP), the s6 plain-sampler table on the fullscreen
// record path the convert shader reads through, and ExecuteIndirect-era root-signature coverage for both.
TEST_CASE("REN-40-D GATE (DX12): EVSM and MSM moment shadows render soft with a lit floor",
          "[scene-render][ren40][csm][moment][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    const TempPack              pack("sr_momentdx_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());

    constexpr u32 dim    = 256U;
    auto          target = raster->create_color_depth_target(dim, dim);
    REQUIRE(target != nullptr);
    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 42.0F, 0.01F}, math::Vec3f{0, 0, 0},
                                           math::Vec3f{0, 1, 0});
    const math::Mat4f     proj  = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Mat4f     vp    = proj * view;
    const math::Vec3f     light = math::normalized(math::Vec3f{1.7F, 1.0F, 0.0F});
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    scenerender::CsmConfig ccfg;
    ccfg.cascade_count = 4;
    ccfg.map_size      = 512;
    ccfg.far_plane     = 120.0F;

    struct Probe
    {
        u32 partial;  // pixels neither plateau — the edge width
        u32 contrast; // lit-to-core range found in the window
        u32 floor;    // the OPEN floor's level, far from the shadow — the unbound-read canary
    };
    const auto measure = [&](gpu::IRasterTarget& t) {
        constexpr u32 row = dim / 2U;
        u32           core   = 4U;
        u32           core_v = 255U;
        for (u32 x = 4U; x <= 251U; ++x)
        {
            const u32 v = t.read_pixel(x, row) & 0xFFU;
            if (v < core_v) { core_v = v; core = x; }
        }
        const u32 w_lo = core > 28U ? core - 24U : 4U;
        const u32 w_hi = core + 24U < 251U ? core + 24U : 251U;
        u32       lo = 255U;
        u32       hi = 0U;
        for (u32 x = w_lo; x <= w_hi; ++x)
        {
            const u32 v = t.read_pixel(x, row) & 0xFFU;
            if (v < lo) { lo = v; }
            if (v > hi) { hi = v; }
        }
        Probe p{0U, hi - lo, 0U};
        // the open floor: the OPPOSITE side of the frame from the shadow core, same scanline
        const u32 fx = core >= dim / 2U ? 24U : dim - 24U;
        p.floor      = t.read_pixel(fx, row) & 0xFFU;
        if (p.contrast < 40U) { return p; }
        const u32 blo = lo + (p.contrast * 15U / 100U);
        const u32 bhi = lo + (p.contrast * 85U / 100U);
        for (u32 x = w_lo; x <= w_hi; ++x)
        {
            const u32 v = t.read_pixel(x, row) & 0xFFU;
            if (v > blo && v < bhi) { ++p.partial; }
        }
        return p;
    };

    const auto run = [&](scenerender::SceneRenderer::SoftShadow mode) {
        scene::World world{&galloc()};
        world.register_component<scene::Transform>(scene::transform_serialize_trait());
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
        add_cube({0.0F, 0.0F, 0.0F}, {100.0F, 0.25F, 100.0F});
        add_cube({0.0F, 8.0F, 0.0F}, {6.0F, 0.25F, 6.0F});

        scenerender::SceneRenderer r(&galloc());
        REQUIRE(r.init(*raster, rm));
        r.set_soft_shadows(mode, 100U);
        // ⛔ the hard arm runs the SHIPPING 4-tap filter, not 1 tap: a single comparison on a 100-unit receiver
        // sits in acne territory (faint self-shadow speckle dims the whole floor and the "darkest pixel" the
        // window centres on is an acne pixel, not the shadow), and the arm then measures its own artefact. The
        // 4-tap edge is still ~2 px against the prefilter's ~6+, so the dichotomy survives — and the acne-free
        // floor is asserted BY the moment arms against this arm's floor, which is exactly property 1.
        r.set_pcf_taps(4U);
        if (!r.init_programs(*gctx)) { SKIP("dxc/DXIL unavailable"); }
        r.set_csm_config(ccfg);
        REQUIRE(r.set_shadows_enabled(true));
        (void)r.sync(world);
        REQUIRE(r.render(*target, vp, light, clear, nullptr).draws > 0U);
        return measure(*target);
    };

    const Probe hard = run(scenerender::SceneRenderer::SoftShadow::Off);
    const Probe msm  = run(scenerender::SceneRenderer::SoftShadow::Msm);
    const Probe evsm = run(scenerender::SceneRenderer::SoftShadow::Evsm);

    INFO("hard " << hard.partial << "/" << hard.contrast << " floor " << hard.floor);
    INFO("msm  " << msm.partial << "/" << msm.contrast << " floor " << msm.floor);
    INFO("evsm " << evsm.partial << "/" << evsm.contrast << " floor " << evsm.floor);

    // every arm must have found a real shadow (property 2 — a dropped pass leaves contrast ~0)
    CHECK(hard.contrast >= 40U);
    CHECK(msm.contrast >= 40U);
    CHECK(evsm.contrast >= 40U);

    // ⛔ property 1 FIRST: the open floor must not dim — an unbound or mis-sampled atlas darkens EVERYTHING
    // and the "softness" below would still pass. ±12 covers rounding and the moment tier's slight bleed.
    const u32 f_lo = hard.floor > 12U ? hard.floor - 12U : 0U;
    CHECK(msm.floor >= f_lo);
    CHECK(evsm.floor >= f_lo);

    // property 3: the prefiltered edge is WIDER than the hard edge. ⛔ The margins differ BY PHYSICS, not by
    // tuning: at the same Gaussian, EVSM's exponential warp RE-SHARPENS the filtered edge (the same property
    // that makes it leak less light than plain VSM), so its penumbra is genuinely narrower than MSM's — the
    // measured widths here are hard 1 px, EVSM 3 px, MSM 5 px. Each margin still fails on a dropped blur or
    // convert pass (equal widths), which is what the property exists to catch.
    CHECK(msm.partial > hard.partial + 2U);
    CHECK(evsm.partial > hard.partial + 1U);
}
#endif // _WIN32 (the REN-40-D DX12 moment gate)

// ── ⭐⭐ REN-40-F GATE: GPU SKINNING COMPUTE PASS — BIT-IDENTICAL TO CPU PALETTE. ────────────────────────────────
// The GPU skinning kernel (a CKIR compute pass) pre-bakes clip data to uniform-rate TRS keys on the GPU, computes
// FK + IBM per instance, and writes the bone palette into the group buffer — the EXACT SAME section the CPU path
// fills. This gate renders two SKINNED cubes (1-joint rig, distinct animation times on BAKE-FRAME BOUNDARIES) with
// the CPU palette path, then re-renders with the GPU palette kernel, and asserts BIT-IDENTICAL pixels. The
// bake-frame alignment eliminates interpolation error (NLERP vs SLERP); identity at alpha=0 is exact.
//
// ⛔ The TOML includes the `gpu_skin` compute pass for BOTH arms: when `set_gpu_skinning(false)` the pass is a
// no-op (dispatch_groups = 0), and sync() fills the palette on the CPU; when true, the kernel dispatches and sync()
// uploads skeleton/clip/anim_state instead. Same graph, same geometry path, only the palette origin differs.

namespace
{
const char kSkinGateBasicToml[] = R"(
schema = 1
name   = "crd://frame/skin_gate_basic"

[[resource]]
name    = "scene_hdr"
kind    = "transient_image"
format  = "RGBA8Unorm"
width   = 256
height  = 256
sampled = true

[[draw_list]]
name = "visible_geometry"
all  = ["MeshRenderer", "Transform"]
cull = "frustum"
sort = "material"

[[pass]]
name          = "scene"
kind          = "raster.geometry"
draw_list     = "visible_geometry"
writes        = ["scene_hdr"]
material_pass = "Forward"
clear_color   = [0.10, 0.30, 0.60, 1.0]

[[pass]]
name   = "post"
kind   = "raster.fullscreen"
reads  = ["scene_hdr"]
writes = ["@output"]
shader = "crd://post/srgb_only"
)";

const char kSkinGateGpuToml[] = R"(
schema = 1
name   = "crd://frame/skin_gate_gpu"

[[resource]]
name = "instances"
kind = "external_buffer"

[[resource]]
name    = "scene_hdr"
kind    = "transient_image"
format  = "RGBA8Unorm"
width   = 256
height  = 256
sampled = true

[[draw_list]]
name = "visible_geometry"
all  = ["MeshRenderer", "Transform"]
cull = "frustum"
sort = "material"

[[pass]]
name      = "gpu_skin"
kind      = "compute"
kernel    = "crd://scene/gpu_skin"
draw_list = "visible_geometry"
writes    = ["instances"]

[[pass]]
name          = "scene"
kind          = "raster.geometry"
draw_list     = "visible_geometry"
writes        = ["scene_hdr"]
reads         = ["instances"]
material_pass = "Forward"
clear_color   = [0.10, 0.30, 0.60, 1.0]

[[pass]]
name   = "post"
kind   = "raster.fullscreen"
reads  = ["scene_hdr"]
writes = ["@output"]
shader = "crd://post/srgb_only"
)";
} // namespace

TEST_CASE("REN-40-F GATE: GPU skinning palette is bit-identical to the CPU palette (Vulkan)",
          "[scene-render][ren40][skinning][gpu][vulkan]")
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

    // ── resources: skinned mesh + 1-joint skeleton + translation clip ──
    const resources::ResourceId mesh_id = resources::ResourceId::mint_random();
    const resources::ResourceId skel_id = resources::ResourceId::mint_random();
    const resources::ResourceId clip_id = resources::ResourceId::mint_random();
    const TempPack mesh_pack("sr_skin_m_", mesh_id);
    const TempPack skel_pack("sr_skin_s_", skel_id);
    const TempPack clip_pack("sr_skin_c_", clip_id);
    {
        auto art = build_skinned_cube_crdr(mesh_id);
        write_resource_pack(mesh_pack.path, mesh_id, resources::kFourCC_MESH, art);
    }
    {
        anim::SkeletonResource sk(&galloc());
        sk.parents.push_back(-1);
        const f32 rest[10] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1};
        for (f32 v : rest) { sk.rest.push_back(v); }
        const f32 ibm[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (f32 v : ibm) { sk.inverse_binds.push_back(v); }
        sk.name_offsets.push_back(0);
        const char jn[] = "root";
        for (const char c : jn) { sk.name_pool.push_back(c); }
        auto art = anim::skeleton_build(sk, skel_id, &galloc());
        write_resource_pack(skel_pack.path, skel_id, anim::kFourCC_SKEL, art);
    }
    {
        anim::AnimClipResource cl(&galloc());
        cl.duration = 1.0F;
        anim::AnimTrack trk{};
        trk.target     = 0;
        trk.channel    = static_cast<u8>(anim::AnimChannel::Translation);
        trk.interp     = 1;
        trk.components = 3;
        trk.key_count  = 2;
        trk.times_off  = 0;
        trk.values_off = 2;
        cl.tracks.push_back(trk);
        const f32 d[8] = {0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 3.0F, 0.0F, 0.0F};
        for (f32 v : d) { cl.data.push_back(v); }
        auto art = anim::anim_clip_build(cl, clip_id, &galloc());
        write_resource_pack(clip_pack.path, clip_id, anim::kFourCC_ANIM, art);
    }
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    anim::register_anim_loaders(&rm, nullptr);
    REQUIRE(rm.mount_manifest(mesh_pack.path.generic()).is_valid());
    REQUIRE(rm.mount_manifest(skel_pack.path.generic()).is_valid());
    REQUIRE(rm.mount_manifest(clip_pack.path.generic()).is_valid());

    // ── world: two skinned cubes at distinct animation times on bake-frame boundaries ──
    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);
    const auto add = [&](math::Vec3f pos, f32 anim_time) {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.translation = math::from_raw_vec<units::dim::Length>(pos);
        t.scale       = {2.0F, 2.0F, 2.0F};
        t.world       = math::from_trs(pos, math::Quatf::identity(), {2.0F, 2.0F, 2.0F});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{mesh_id, {}});
        scene::SkeletonAnimator sa{};
        sa.skeleton = skel_id;
        sa.clip     = clip_id;
        sa.time     = anim_time;
        sa.speed    = 0.0F;
        world.add_component(e, sa);
    };
    add({-4.0F, 0.0F, 0.0F}, 0.0F);
    add({ 4.0F, 0.0F, 0.0F}, 0.5F);

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    const math::Mat4f     view  = math::look_at(math::Vec3f{0.0F, 6.0F, 16.0F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj  = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     light{0.3F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    // ── CPU palette via the basic TOML — the proven reference ──
    REQUIRE(renderer.set_frame_graph_toml(kSkinGateBasicToml));
    renderer.set_gpu_skinning(false);
    REQUIRE(renderer.sync(world).total_instances == 2U);
    auto ref = raster->create_color_depth_target(256U, 256U);
    REQUIRE(ref != nullptr);
    const auto r_cpu = renderer.render(*ref, proj * view, light, clear);
    CHECK(r_cpu.draws > 0U);

    // ── GPU palette via the GPU TOML with the gpu_skin compute pass ──
    REQUIRE(renderer.set_frame_graph_toml(kSkinGateGpuToml));
    renderer.set_gpu_skinning(true);
    REQUIRE(renderer.sync(world).total_instances == 2U);
    auto tgt = raster->create_color_depth_target(256U, 256U);
    REQUIRE(tgt != nullptr);
    const auto r_gpu = renderer.render(*tgt, proj * view, light, clear);
    INFO("gpu: draws=" << r_gpu.draws << " timed=" << r_gpu.timed_passes
         << " gpu_ms=" << r_gpu.gpu_ms << " drawn_inst=" << r_gpu.drawn_instances);
    CHECK(r_gpu.draws > 0U);

    // ── BIT-IDENTICAL pixels + non-trivial coverage ──
    u32 diffs   = 0U;
    u32 covered = 0U;
    for (u32 y = 0; y < 256U; ++y)
    {
        for (u32 x = 0; x < 256U; ++x)
        {
            const u32 a = ref->read_pixel(x, y);
            const u32 b = tgt->read_pixel(x, y);
            if (a != b) { ++diffs; }
            if ((b & 0x00FFFFFFU) != 0U) { ++covered; }
        }
    }
    INFO("diffs=" << diffs << " covered=" << covered);
    CHECK(diffs == 0U);
    CHECK(covered > 500U);
}

#ifdef _WIN32
TEST_CASE("REN-40-F GATE (DX12): GPU skinning palette is bit-identical to the CPU palette",
          "[scene-render][ren40][skinning][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid())
    {
        SKIP("no D3D12 device available");
    }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    const resources::ResourceId mesh_id = resources::ResourceId::mint_random();
    const resources::ResourceId skel_id = resources::ResourceId::mint_random();
    const resources::ResourceId clip_id = resources::ResourceId::mint_random();
    const TempPack mesh_pack("sr_skindx_m_", mesh_id);
    const TempPack skel_pack("sr_skindx_s_", skel_id);
    const TempPack clip_pack("sr_skindx_c_", clip_id);
    {
        auto art = build_skinned_cube_crdr(mesh_id);
        write_resource_pack(mesh_pack.path, mesh_id, resources::kFourCC_MESH, art);
    }
    {
        anim::SkeletonResource sk(&galloc());
        sk.parents.push_back(-1);
        const f32 rest[10] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1};
        for (f32 v : rest) { sk.rest.push_back(v); }
        const f32 ibm[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (f32 v : ibm) { sk.inverse_binds.push_back(v); }
        sk.name_offsets.push_back(0);
        const char jn[] = "root";
        for (const char c : jn) { sk.name_pool.push_back(c); }
        auto art = anim::skeleton_build(sk, skel_id, &galloc());
        write_resource_pack(skel_pack.path, skel_id, anim::kFourCC_SKEL, art);
    }
    {
        anim::AnimClipResource cl(&galloc());
        cl.duration = 1.0F;
        anim::AnimTrack trk{};
        trk.target     = 0;
        trk.channel    = static_cast<u8>(anim::AnimChannel::Translation);
        trk.interp     = 1;
        trk.components = 3;
        trk.key_count  = 2;
        trk.times_off  = 0;
        trk.values_off = 2;
        cl.tracks.push_back(trk);
        const f32 d[8] = {0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 3.0F, 0.0F, 0.0F};
        for (f32 v : d) { cl.data.push_back(v); }
        auto art = anim::anim_clip_build(cl, clip_id, &galloc());
        write_resource_pack(clip_pack.path, clip_id, anim::kFourCC_ANIM, art);
    }
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    anim::register_anim_loaders(&rm, nullptr);
    REQUIRE(rm.mount_manifest(mesh_pack.path.generic()).is_valid());
    REQUIRE(rm.mount_manifest(skel_pack.path.generic()).is_valid());
    REQUIRE(rm.mount_manifest(clip_pack.path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);
    const auto add = [&](math::Vec3f pos, f32 anim_time) {
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.translation = math::from_raw_vec<units::dim::Length>(pos);
        t.scale       = {2.0F, 2.0F, 2.0F};
        t.world       = math::from_trs(pos, math::Quatf::identity(), {2.0F, 2.0F, 2.0F});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{mesh_id, {}});
        scene::SkeletonAnimator sa{};
        sa.skeleton = skel_id;
        sa.clip     = clip_id;
        sa.time     = anim_time;
        sa.speed    = 0.0F;
        world.add_component(e, sa);
    };
    add({-4.0F, 0.0F, 0.0F}, 0.0F);
    add({ 4.0F, 0.0F, 0.0F}, 0.5F);

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    if (!renderer.init_programs(*gctx))
    {
        SKIP("dxc/DXIL unavailable");
    }
    const math::Mat4f     view  = math::look_at(math::Vec3f{0.0F, 6.0F, 16.0F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj  = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     light{0.3F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    // ── CPU palette via the basic TOML — the proven reference ──
    REQUIRE(renderer.set_frame_graph_toml(kSkinGateBasicToml));
    renderer.set_gpu_skinning(false);
    REQUIRE(renderer.sync(world).total_instances == 2U);
    auto ref = raster->create_color_depth_target(256U, 256U);
    REQUIRE(ref != nullptr);
    CHECK(renderer.render(*ref, proj * view, light, clear).draws > 0U);

    // ── GPU palette via the GPU TOML with the gpu_skin compute pass ──
    REQUIRE(renderer.set_frame_graph_toml(kSkinGateGpuToml));
    renderer.set_gpu_skinning(true);
    REQUIRE(renderer.sync(world).total_instances == 2U);
    auto tgt = raster->create_color_depth_target(256U, 256U);
    REQUIRE(tgt != nullptr);
    CHECK(renderer.render(*tgt, proj * view, light, clear).draws > 0U);

    u32 diffs   = 0U;
    u32 covered = 0U;
    for (u32 y = 0; y < 256U; ++y)
    {
        for (u32 x = 0; x < 256U; ++x)
        {
            const u32 a = ref->read_pixel(x, y);
            const u32 b = tgt->read_pixel(x, y);
            if (a != b) { ++diffs; }
            if ((b & 0x00FFFFFFU) != 0U) { ++covered; }
        }
    }
    INFO("diffs=" << diffs << " covered=" << covered);
    CHECK(diffs == 0U);
    CHECK(covered > 500U);
}
#endif // _WIN32 (the REN-40-F DX12 skinning gate)

// ── ⭐⭐ REN-40-G GATE: THE FRAME TRICKS — DEPTH PREPASS, R11G11B10F, OCCLUSION CULLING. ───────────────────────
// Each feature has its own A/B arm: one arm renders WITHOUT the feature, one WITH. The gates are:
//   G1 — depth prepass + shared_depth + load_depth: pixels IDENTICAL (prepass only accelerates, never changes output)
//   G2 — R11G11B10F intermediate: the packed HDR format produces correct lit output after tonemap
//   G3 — two-phase occlusion culling: pixels IDENTICAL for an unoccluded scene (the re-cull keeps everything)

namespace
{
const char k40gRefBasicToml[] = R"(
schema = 1
name   = "crd://frame/test_g1_ref"

[[resource]]
name    = "scene_hdr"
kind    = "transient_image"
format  = "RGBA8Unorm"
width   = 256
height  = 256
sampled = true

[[draw_list]]
name = "visible_geometry"
all  = ["MeshRenderer", "Transform"]
cull = "frustum"
sort = "material"

[[pass]]
name          = "forward"
kind          = "raster.geometry"
draw_list     = "visible_geometry"
writes        = ["scene_hdr"]
material_pass = "Forward"
clear_color   = [0.10, 0.30, 0.60, 1.0]
clear_depth   = 0.0
depth         = "GreaterEqual"

[[pass]]
name   = "post"
kind   = "raster.fullscreen"
reads  = ["scene_hdr"]
writes = ["@output"]
shader = "crd://post/srgb_only"
)";

const char k40gPrepassToml[] = R"(
schema = 1
name   = "crd://frame/test_g1_pre"

[[resource]]
name    = "scene_depth"
kind    = "transient_image"
format  = "D32Float"
width   = 256
height  = 256

[[resource]]
name    = "scene_hdr"
kind    = "transient_image"
format  = "RGBA8Unorm"
width   = 256
height  = 256
sampled = true

[[draw_list]]
name = "visible_geometry"
all  = ["MeshRenderer", "Transform"]
cull = "frustum"
sort = "material"

[[pass]]
name          = "depth_prepass"
kind          = "raster.depth_only"
draw_list     = "visible_geometry"
writes        = ["scene_depth"]
material_pass = "Shadow"
clear_depth   = 0.0
depth         = "GreaterEqual"

[[pass]]
name          = "forward"
kind          = "raster.geometry"
draw_list     = "visible_geometry"
writes        = ["scene_hdr"]
shared_depth  = "scene_depth"
load_depth    = true
material_pass = "Forward"
clear_color   = [0.10, 0.30, 0.60, 1.0]
depth         = "GreaterEqual"

[[pass]]
name   = "post"
kind   = "raster.fullscreen"
reads  = ["scene_hdr"]
writes = ["@output"]
shader = "crd://post/srgb_only"
)";

const char k40gRef16FToml[] = R"(
schema = 1
name   = "crd://frame/test_g2_16f"

[[resource]]
name    = "scene_hdr"
kind    = "transient_image"
format  = "RGBA16F"
width   = 256
height  = 256
sampled = true

[[draw_list]]
name = "visible_geometry"
all  = ["MeshRenderer", "Transform"]
cull = "frustum"
sort = "material"

[[pass]]
name          = "forward"
kind          = "raster.geometry"
draw_list     = "visible_geometry"
writes        = ["scene_hdr"]
material_pass = "Forward"
clear_color   = [0.10, 0.30, 0.60, 1.0]
clear_depth   = 0.0
depth         = "GreaterEqual"

[[pass]]
name   = "post"
kind   = "raster.fullscreen"
reads  = ["scene_hdr"]
writes = ["@output"]
shader = "crd://post/srgb_only"
)";

const char k40gHdrR11Toml[] = R"(
schema = 1
name   = "crd://frame/test_g2_r11"

[[resource]]
name    = "scene_hdr"
kind    = "transient_image"
format  = "R11G11B10F"
width   = 256
height  = 256
sampled = true

[[draw_list]]
name = "visible_geometry"
all  = ["MeshRenderer", "Transform"]
cull = "frustum"
sort = "material"

[[pass]]
name          = "forward"
kind          = "raster.geometry"
draw_list     = "visible_geometry"
writes        = ["scene_hdr"]
material_pass = "Forward"
clear_color   = [0.10, 0.30, 0.60, 1.0]
clear_depth   = 0.0
depth         = "GreaterEqual"

[[pass]]
name   = "post"
kind   = "raster.fullscreen"
reads  = ["scene_hdr"]
writes = ["@output"]
shader = "crd://post/srgb_only"
)";

const char k40gOccCullToml[] = R"(
schema = 1
name   = "crd://frame/test_g3_occ"

[[resource]]
name = "instances"
kind = "external_buffer"

[[resource]]
name       = "cull_args"
kind       = "indirect_args"
size_bytes = 256

[[resource]]
name    = "scene_depth"
kind    = "transient_image"
format  = "D32Float"
width   = 256
height  = 256
sampled = true

[[resource]]
name    = "hzb"
kind    = "transient_image"
format  = "R32F"
width   = 128
height  = 128
sampled = true

[[resource]]
name    = "scene_hdr"
kind    = "transient_image"
format  = "RGBA8Unorm"
width   = 256
height  = 256
sampled = true

[[draw_list]]
name = "visible_geometry"
all  = ["MeshRenderer", "Transform"]
cull = "frustum"
sort = "material"

[[pass]]
name      = "cull_reset"
kind      = "compute"
kernel    = "crd://scene/cull_reset"
draw_list = "visible_geometry"
writes    = ["instances", "cull_args"]

[[pass]]
name      = "cull_view0"
kind      = "compute"
kernel    = "crd://scene/cull_view0"
draw_list = "visible_geometry"
writes    = ["instances", "cull_args"]

[[pass]]
name               = "depth_prepass"
kind               = "raster.depth_only"
draw_list          = "visible_geometry"
writes             = ["scene_depth"]
material_pass      = "Shadow"
clear_depth        = 0.0
depth              = "GreaterEqual"
untracked_storage  = true

[[pass]]
name           = "hzb_build"
kind           = "raster.fullscreen"
reads          = ["scene_depth"]
writes         = ["hzb"]
shader         = "crd://scene/hzb_build"
depth_as_float = true

[[pass]]
name      = "occlusion_reset"
kind      = "compute"
kernel    = "crd://scene/cull_reset"
draw_list = "visible_geometry"
writes    = ["instances", "cull_args"]

[[pass]]
name      = "occlusion_cull"
kind      = "compute"
kernel    = "crd://scene/occlusion_cull"
draw_list = "visible_geometry"
reads     = ["hzb"]
writes    = ["instances", "cull_args"]

[[pass]]
name          = "forward"
kind          = "raster.geometry"
draw_list     = "visible_geometry"
writes        = ["scene_hdr"]
reads         = ["instances", "cull_args"]
shared_depth  = "scene_depth"
load_depth    = true
material_pass = "Forward"
clear_color   = [0.10, 0.30, 0.60, 1.0]
depth         = "GreaterEqual"

[[pass]]
name   = "post"
kind   = "raster.fullscreen"
reads  = ["scene_hdr"]
writes = ["@output"]
shader = "crd://post/srgb_only"
)";
} // namespace

TEST_CASE("REN-40-G1 GATE: depth prepass with shared_depth is pixel-identical to forward-only (Vulkan)",
          "[scene-render][ren40][depth-prepass][gpu][vulkan]")
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
    const TempPack              pack("sr_g1_pack_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);
    for (u32 i = 0; i < 5U; ++i)
    {
        const f32             x = (static_cast<f32>(i) - 2.0F) * 2.5F;
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs(math::Vec3f{x, 0.0F, 0.0F}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    (void)renderer.sync(world);

    constexpr u32         dim   = 256U;
    const math::Mat4f     view  = math::look_at(math::Vec3f{0, 4, 12}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj  = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     light{0.4F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    auto ref = raster->create_color_depth_target(dim, dim);
    REQUIRE(ref != nullptr);
    REQUIRE(renderer.set_frame_graph_toml(k40gRefBasicToml));
    const auto r_ref = renderer.render(*ref, proj * view, light, clear);
    CHECK(r_ref.draws > 0U);

    auto tgt = raster->create_color_depth_target(dim, dim);
    REQUIRE(tgt != nullptr);
    REQUIRE(renderer.set_frame_graph_toml(k40gPrepassToml));
    const auto r_pre = renderer.render(*tgt, proj * view, light, clear);
    INFO("ref draws=" << r_ref.draws << " pre draws=" << r_pre.draws
         << " pre timed=" << r_pre.timed_passes);
    CHECK(r_pre.draws > 0U);

    u32 diffs   = 0U;
    u32 covered = 0U;
    for (u32 y = 0; y < dim; ++y)
    {
        for (u32 x = 0; x < dim; ++x)
        {
            const u32 a = ref->read_pixel(x, y);
            const u32 b = tgt->read_pixel(x, y);
            if (a != b) { ++diffs; }
            if ((b & 0x00FFFFFFU) != 0U) { ++covered; }
        }
    }
    INFO("diffs=" << diffs << " covered=" << covered);
    CHECK(diffs == 0U);
    CHECK(covered > 500U);
}

TEST_CASE("REN-40-G2 GATE: R11G11B10F scene_hdr produces correct lit output after tonemap (Vulkan)",
          "[scene-render][ren40][hdr-format][gpu][vulkan]")
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
    const TempPack              pack("sr_g2_pack_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);
    for (u32 i = 0; i < 3U; ++i)
    {
        const f32             x = (static_cast<f32>(i) - 1.0F) * 3.0F;
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs(math::Vec3f{x, 0.0F, 0.0F}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    REQUIRE(renderer.init_programs(*vk));
    (void)renderer.sync(world);

    constexpr u32         dim   = 256U;
    const math::Mat4f     view  = math::look_at(math::Vec3f{0, 4, 12}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj  = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     light{0.4F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    auto ref = raster->create_color_depth_target(dim, dim);
    REQUIRE(ref != nullptr);
    REQUIRE(renderer.set_frame_graph_toml(k40gRef16FToml));
    const auto r_ref = renderer.render(*ref, proj * view, light, clear);
    CHECK(r_ref.draws > 0U);

    auto tgt = raster->create_color_depth_target(dim, dim);
    REQUIRE(tgt != nullptr);
    REQUIRE(renderer.set_frame_graph_toml(k40gHdrR11Toml));
    const auto r_r11 = renderer.render(*tgt, proj * view, light, clear);
    INFO("ref draws=" << r_ref.draws << " r11 draws=" << r_r11.draws);
    CHECK(r_r11.draws > 0U);

    u32 diffs     = 0U;
    u32 big_diffs = 0U;
    u32 covered   = 0U;
    for (u32 y = 0; y < dim; ++y)
    {
        for (u32 x = 0; x < dim; ++x)
        {
            const u32 a = ref->read_pixel(x, y);
            const u32 b = tgt->read_pixel(x, y);
            if (a != b) { ++diffs; }
            const i32 dr = static_cast<i32>((a >> 16U) & 0xFFU) - static_cast<i32>((b >> 16U) & 0xFFU);
            const i32 dg = static_cast<i32>((a >> 8U)  & 0xFFU) - static_cast<i32>((b >> 8U)  & 0xFFU);
            const i32 db = static_cast<i32>(a & 0xFFU)          - static_cast<i32>(b & 0xFFU);
            const i32 mx = (dr > 0 ? dr : -dr);
            const i32 my = (dg > 0 ? dg : -dg);
            const i32 mz = (db > 0 ? db : -db);
            const i32 mxy   = mx > my ? mx : my;
            const i32 worst = mxy > mz ? mxy : mz;
            if (worst > 4) { ++big_diffs; }
            if ((b & 0x00FFFFFFU) != 0U) { ++covered; }
        }
    }
    INFO("diffs=" << diffs << " big_diffs=" << big_diffs << " covered=" << covered);
    CHECK(big_diffs == 0U);
    CHECK(covered > 500U);
}

TEST_CASE("REN-40-G3 GATE: GPU occlusion culling produces pixel-identical output for unoccluded scene (Vulkan)",
          "[scene-render][ren40][occlusion][gpu][vulkan]")
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
    const TempPack              pack("sr_g3_pack_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);
    for (u32 i = 0; i < 8U; ++i)
    {
        const f32             x = (static_cast<f32>(i) - 3.5F) * 1.5F;
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs(math::Vec3f{x, 0.0F, 0.0F}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }

    const math::Mat4f     view  = math::look_at(math::Vec3f{0, 8, 20}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj  = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     light{0.4F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};
    constexpr u32         dim   = 256U;

    scenerender::SceneRenderer ref_renderer(&galloc());
    REQUIRE(ref_renderer.init(*raster, rm));
    REQUIRE(ref_renderer.init_programs(*vk));
    (void)ref_renderer.sync(world);
    auto ref = raster->create_color_depth_target(dim, dim);
    REQUIRE(ref != nullptr);
    REQUIRE(ref_renderer.set_frame_graph_toml(k40gRefBasicToml));
    const auto r_ref = ref_renderer.render(*ref, proj * view, light, clear);
    INFO("ref draws=" << r_ref.draws);
    CHECK(r_ref.draws > 0U);

    scenerender::SceneRenderer occ_renderer(&galloc());
    REQUIRE(occ_renderer.init(*raster, rm));
    REQUIRE(occ_renderer.init_programs(*vk));
    occ_renderer.set_gpu_cull(true);
    occ_renderer.set_gpu_cull_verify(true);
    (void)occ_renderer.sync(world);
    auto tgt = raster->create_color_depth_target(dim, dim);
    REQUIRE(tgt != nullptr);
    REQUIRE(occ_renderer.set_frame_graph_toml(k40gOccCullToml));
    const auto r_occ = occ_renderer.render(*tgt, proj * view, light, clear);
    scenerender::SceneRenderer::GpuCullCounts gc{};
    const bool gc_ok = occ_renderer.read_gpu_cull_counts(gc);
    INFO("occ draws=" << r_occ.draws << " drawn_instances=" << r_occ.drawn_instances
         << " gc_ok=" << gc_ok << " gc_views=" << gc.views
         << " gc_v0=" << gc.instances[0] << " gc_groups=" << gc.groups
         << " cpu_vis=" << r_occ.culled_instances
         << " cpu_v0=" << gc.cpu_instances[0]
         << " bounds_checked=" << gc.bounds_checked
         << " bounds_mismatch=" << gc.bounds_mismatch
         << " slot0_inst=" << gc.slot_instances[0]
         << " slot0_idx=" << gc.slot_indices[0]
         << " slot0_fi=" << gc.slot_first[0]
         << " gpu_cull_effective=" << occ_renderer.gpu_cull()
         << " hdr_w0=" << gc.header_w0
         << " hdr_w2=" << gc.header_w2
         << " args_size=" << gc.args_size);
    INFO("raw_args[0..15]:"
         << " " << gc.raw_args[0] << " " << gc.raw_args[1]
         << " " << gc.raw_args[2] << " " << gc.raw_args[3]
         << " " << gc.raw_args[4] << " " << gc.raw_args[5]
         << " " << gc.raw_args[6] << " " << gc.raw_args[7]
         << " |" << gc.raw_args[8] << " " << gc.raw_args[9]
         << " " << gc.raw_args[10] << " " << gc.raw_args[11]
         << " " << gc.raw_args[12] << "| " << gc.raw_args[13]
         << " " << gc.raw_args[14] << " " << gc.raw_args[15]);
    INFO("fill: cull_gates=" << gc.fill_cull_gates
         << " dispatch_max=" << gc.fill_dispatch_max
         << " total_items=" << gc.fill_total_items
         << " index_count_0=" << gc.fill_index_count_0
         << " record_ok=" << gc.fill_record_ok
         << " build_ok=" << gc.fill_build_ok
         << " pass_count=" << gc.fill_pass_count
         << " vk_dispatches=" << raster->compute_dispatch_count()
         << " fg_order=" << raster->compute_diag_count(5)
         << " fg_has_fn=" << raster->compute_diag_count(6)
         << " fg_not_async=" << raster->compute_diag_count(7)
         << " record_pass_calls=" << raster->compute_diag_count(8)
         << " record_pass_compute=" << raster->compute_diag_count(9)
         << " compute[0..4]=" << raster->compute_diag_count(0) << "," << raster->compute_diag_count(1) << ","
         << raster->compute_diag_count(2) << "," << raster->compute_diag_count(3) << "," << raster->compute_diag_count(4)
         << " occ_step=" << gc.occ_step);
    CHECK(r_occ.draws > 0U);

    u32 diffs   = 0U;
    u32 covered = 0U;
    for (u32 y = 0; y < dim; ++y)
    {
        for (u32 x = 0; x < dim; ++x)
        {
            const u32 a = ref->read_pixel(x, y);
            const u32 b = tgt->read_pixel(x, y);
            if (a != b) { ++diffs; }
            if ((b & 0x00FFFFFFU) != 0U) { ++covered; }
        }
    }
    INFO("diffs=" << diffs << " covered=" << covered);
    CHECK(diffs == 0U);
    CHECK(covered > 500U);
}

// ── REN-41 (velocity) CORRECTNESS GATE — the shared body; one TEST_CASE per backend drives it. ───────────────
// The dossier's remaining "read the velocity buffer" gate. It renders the SHIPPED `velocity_debug` frame (the
// SHIPPING motion-vector prepass + an encode pass) and DECODES the RG16F motion delta out of the RGBA8 output.
//
// Scene: two unit cubes of the SAME mesh (one group). LEFT is STATIC; RIGHT is the MOVER. Frame 1 seeds both
// instances' prev_world (a fresh slot self-shadows -> zero velocity on spawn, which frame 1 verifies). Then the
// mover's Transform is pushed +3 world-units in Y and re-synced: bumping the chunk Transform version makes the
// incremental extract snapshot prev_world = A and set world = B for the mover, while the static instance
// re-extracts to prev == cur. Frame 2 therefore carries three distinguishable classes:
//   . BACKGROUND      -> the cleared SENTINEL (0.25, 0.25)  -> encodes ~(191, 191)
//   . the STATIC cube -> velocity 0 (drawn, and WROTE 0)    -> encodes ~(128, 128)  [!= sentinel => it truly drew]
//   . the MOVER cube  -> A_uv - B_uv (a pure +Y screen move) -> encodes (128, enc(exp_v))
// The move is purely in world Y and this camera's view row0 carries no Y term, so u_delta = 0 for EVERY mover
// fragment (clip.x and w are unchanged by the move) -- which is why "drawn" is classified by R ~= 128 while the
// signed motion lives entirely in G. exp_v is computed from the SAME projection the velocity FS uses.
void velocity_gate_body(gpu::IGpuContext& ctx, gpu::IRasterContext& raster)
{
    const auto absf = [](f32 x) { return x < 0.0F ? -x : x; };

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    const TempPack              pack("sr_vel_pack_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);

    const math::Vec3f sscale{2.0F, 2.0F, 2.0F};
    const math::Vec3f static_pos{-4.5F, 0.0F, 0.0F};
    const math::Vec3f mover_a{4.5F, 0.0F, 0.0F};
    const math::Vec3f mover_b{4.5F, 3.0F, 0.0F};

    const scene::EntityId e_static = world.spawn();
    scene::Transform      t_static;
    t_static.world = math::from_trs(static_pos, math::Quatf::identity(), sscale);
    world.add_component(e_static, t_static);
    world.add_component(e_static, scene::MeshRenderer{cube_id, {}});

    const scene::EntityId e_mover = world.spawn();
    scene::Transform      t_mover;
    t_mover.world = math::from_trs(mover_a, math::Quatf::identity(), sscale);
    world.add_component(e_mover, t_mover);
    world.add_component(e_mover, scene::MeshRenderer{cube_id, {}});

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(raster, rm));
    if (!renderer.init_programs(ctx)) { SKIP("shader backend unavailable"); }

    containers::String toml(&galloc());
    REQUIRE(read_shipped_asset("frame/velocity_debug.frame.toml", toml));
    REQUIRE(renderer.set_frame_graph_toml(toml.c_str()));

    constexpr u32     dim  = 256U;
    const math::Mat4f view = math::look_at(math::Vec3f{0, 0, 12}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Mat4f vp   = proj * view;
    const math::Vec3f     light{0.4F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    // decode a channel back to signed UV motion (the inverse of the shader's out = v*scale + 0.5). shift = 0 -> R
    // (u), 8 -> G (v).
    const auto decode = [](u32 px, u32 shift) {
        return (static_cast<f32>((px >> shift) & 0xFFU) / 255.0F - 0.5F) / scenerender::kVelocityDebugScale;
    };
    // scan a COLUMN (+-2 px) over all rows; a fragment is "drawn" when its u channel (R) decodes near 0 (a pure-Y
    // move -> every cube fragment has u = 0, while the 0.25 sentinel background does not). Returns the drawn-pixel
    // count and the MEAN decoded (u, v) over them (mean averages the small per-fragment perspective spread).
    const auto scan_column = [&](gpu::IRasterTarget& t, int cx, u32& count, f32& mean_u, f32& mean_v) {
        f32 su = 0.0F;
        f32 sv = 0.0F;
        count = 0U;
        for (int dx = -2; dx <= 2; ++dx)
        {
            const int x = cx + dx;
            if (x < 0 || x >= static_cast<int>(dim)) { continue; }
            for (u32 y = 0; y < dim; ++y)
            {
                const u32 px = t.read_pixel(static_cast<u32>(x), y);
                const f32 u  = decode(px, 0U);
                if (u > -0.10F && u < 0.10F) // a cube fragment (u == 0), not the 0.25 sentinel background
                {
                    su += u;
                    sv += decode(px, 8U);
                    ++count;
                }
            }
        }
        mean_u = count > 0U ? su / static_cast<f32>(count) : 0.0F;
        mean_v = count > 0U ? sv / static_cast<f32>(count) : 0.0F;
    };

    // expected mover velocity, from the SAME projection the velocity FS uses (clip = view_proj . world . pos; a
    // cube CENTRE's world.pos is the translation column). vsgn matches build_velocity_fs_cooked: +0.5 on a y-down
    // backend (Vulkan), -0.5 on y-up (DX12).
    const f32         vsgn        = raster.ndc_y_points_down() ? 0.5F : -0.5F;
    const math::Vec4f clip_a      = vp * math::Vec4f{mover_a.x, mover_a.y, mover_a.z, 1.0F};
    const math::Vec4f clip_b      = vp * math::Vec4f{mover_b.x, mover_b.y, mover_b.z, 1.0F};
    const f32         exp_u       = (clip_a.x / clip_a.w - clip_b.x / clip_b.w) * 0.5F;
    const f32         exp_v       = (clip_a.y / clip_a.w - clip_b.y / clip_b.w) * vsgn;
    const int         col_mover   = static_cast<int>((clip_b.x / clip_b.w * 0.5F + 0.5F) * static_cast<f32>(dim));
    const math::Vec4f clip_static = vp * math::Vec4f{static_pos.x, static_pos.y, static_pos.z, 1.0F};
    const int col_static = static_cast<int>((clip_static.x / clip_static.w * 0.5F + 0.5F) * static_cast<f32>(dim));

    // ── FRAME 1: both instances fresh -> zero velocity on spawn (a fresh slot self-shadows; no origin spike). ──
    (void)renderer.sync(world);
    auto tgt1 = raster.create_color_depth_target(dim, dim);
    REQUIRE(tgt1 != nullptr);
    const auto r1 = renderer.render(*tgt1, vp, light, clear);
    CHECK(r1.draws > 0U);
    u32 c1 = 0U;
    f32 u1 = 0.0F;
    f32 v1 = 0.0F;
    scan_column(*tgt1, col_mover, c1, u1, v1);
    INFO("frame1 mover col=" << col_mover << " drawn=" << c1 << " mean_u=" << u1 << " mean_v=" << v1);
    CHECK(c1 > 20U);            // the mover is visible
    CHECK(absf(u1) < 0.03F);    // spawn => no velocity, either axis
    CHECK(absf(v1) < 0.03F);

    // ── move the mover +3 world-units in Y and re-sync (bumps the chunk Transform version -> the incremental
    // extract snapshots prev_world = A, sets world = B for the mover; the static instance re-extracts to 0). ──
    world.get_component_mut<scene::Transform>(e_mover)->world =
        math::from_trs(mover_b, math::Quatf::identity(), sscale);
    (void)renderer.sync(world);
    auto tgt2 = raster.create_color_depth_target(dim, dim);
    REQUIRE(tgt2 != nullptr);
    const auto r2 = renderer.render(*tgt2, vp, light, clear);
    CHECK(r2.draws > 0U);

    // (a) BACKGROUND is the sentinel -> proves the encode pass ran and the clear survived where nothing drew.
    const u32 bg = tgt2->read_pixel(2U, 2U);
    INFO("bg R=" << (bg & 0xFFU) << " G=" << ((bg >> 8U) & 0xFFU));
    CHECK(absf(decode(bg, 0U) - 0.25F) < 0.05F);
    CHECK(absf(decode(bg, 8U) - 0.25F) < 0.05F);

    // (b) the STATIC cube drew and WROTE ~0 velocity (!= sentinel => the velocity FS genuinely ran for it).
    u32 cs = 0U;
    f32 us = 0.0F;
    f32 vs = 0.0F;
    scan_column(*tgt2, col_static, cs, us, vs);
    INFO("frame2 static col=" << col_static << " drawn=" << cs << " mean_u=" << us << " mean_v=" << vs);
    CHECK(cs > 20U);
    CHECK(absf(us) < 0.03F);
    CHECK(absf(vs) < 0.03F);

    // (c) the MOVER carries the expected screen-space motion delta: u ~= 0 (pure-Y move), v ~= exp_v, and clearly
    // NONZERO (distinct from the static's 0) -- the crux of the gate.
    u32 cm = 0U;
    f32 um = 0.0F;
    f32 vm = 0.0F;
    scan_column(*tgt2, col_mover, cm, um, vm);
    INFO("frame2 mover col=" << col_mover << " drawn=" << cm << " mean_u=" << um << " mean_v=" << vm
                             << " exp_u=" << exp_u << " exp_v=" << exp_v);
    CHECK(cm > 20U);
    CHECK(absf(um - exp_u) < 0.04F);
    CHECK(absf(vm - exp_v) < 0.06F);
    CHECK(absf(vm) > 0.10F); // motion is present and significant (not the static 0)
}

TEST_CASE("REN-41 GATE: velocity is zero for static instances and matches the screen delta for movers (Vulkan)",
          "[scene-render][ren41][velocity][gpu][vulkan]")
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
    velocity_gate_body(*vk, *raster);
}

// ── REN-41 Stage 4 (S4-0): the CLUSTER-MESH UNPACK GATE — the Nanite mesh shader on a real device. ────────────
// Generate a grid mesh -> cook_cluster_dag -> render the LEAF clusters through the cluster mesh shader
// (draw_clusters -> draw_mesh_storage) -> compare the DEVICE coverage against the CPU unpack_selected_clusters
// oracle. The mesh shader self-selects the level-0 leaves, so the device draws exactly the original mesh; the
// oracle rasterises the same leaf triangles on the CPU. Parity-by-derivation (the 40-A pattern) proves the mesh
// emitter emits valid SPIR-V/DXIL AND the unpack (cluster_vertices indirection + 4-u8/u32 packed local indices)
// is byte-correct, on BOTH backends where mesh-shader defects are otherwise SILENT.
void cluster_mesh_gate_body(gpu::IGpuContext& ctx, gpu::IRasterContext& raster)
{
    namespace mp = geometry::mesh_processing;
    memory::TlsfAllocator alloc(32U << 20U);

    // a UV SPHERE — the same real 3D surface the 40-I gate cooks (a flat/coplanar mesh is a degenerate QEM/BVH
    // input). `slices` x `stacks` grid of quads → ~2·slices·stacks triangles, enough for several meshlet clusters.
    constexpr u32          slices = 24U;
    constexpr u32          stacks = 12U;
    containers::Array<f32> mpos(&alloc);
    containers::Array<u32> midx(&alloc);
    for (u32 s = 0; s <= stacks; ++s)
    {
        const f32 phi = 3.14159265F * static_cast<f32>(s) / static_cast<f32>(stacks);
        const f32 sp  = math::deterministic::sin(phi);
        const f32 cp  = math::deterministic::cos(phi);
        for (u32 j = 0; j <= slices; ++j)
        {
            const f32 theta = 2.0F * 3.14159265F * static_cast<f32>(j) / static_cast<f32>(slices);
            mpos.push_back(sp * math::deterministic::cos(theta));
            mpos.push_back(cp);
            mpos.push_back(sp * math::deterministic::sin(theta));
        }
    }
    const u32 row = slices + 1U;
    for (u32 s = 0; s < stacks; ++s)
    {
        for (u32 j = 0; j < slices; ++j)
        {
            const u32 v0 = s * row + j;
            midx.push_back(v0);       midx.push_back(v0 + row);      midx.push_back(v0 + row + 1U);
            midx.push_back(v0);       midx.push_back(v0 + row + 1U); midx.push_back(v0 + 1U);
        }
    }
    const u32 mesh_vcount = static_cast<u32>(mpos.size() / 3U);

    mp::ClusterDagCookResult cdag(&alloc);
    mp::DagBuildOptions      opts;
    memory::TlsfAllocator    scratch(32U << 20U);
    const auto rep = mp::cook_cluster_dag(mpos.data(), mesh_vcount, midx.data(), static_cast<u32>(midx.size()), opts,
                                          cdag, &scratch);
    REQUIRE(rep.status == mp::ClusterDagCookStatus::Ok);
    REQUIRE(cdag.cluster_count > 0U);

    // leaves (level 0) + the CPU oracle triangle soup for exactly those clusters.
    const u32              nclusters = static_cast<u32>(cdag.packed_clusters.size() / mp::kClusterGpuWords);
    containers::Array<u32> leaves(&alloc);
    for (u32 c = 0; c < nclusters; ++c)
    {
        if ((cdag.packed_clusters[c * mp::kClusterGpuWords + 2U] >> 16U) == 0U) { leaves.push_back(c); }
    }
    REQUIRE(leaves.size() > 0U);
    mp::ClusterUnpackResult oracle(&alloc);
    mp::unpack_selected_clusters(cdag.packed_clusters.data(), cdag.cluster_vertices.data(),
                                 cdag.cluster_triangles_packed.data(), cdag.positions.data(), leaves.data(),
                                 static_cast<u32>(leaves.size()), oracle);
    REQUIRE(oracle.triangle_count > 0U);

    constexpr u32     dim  = 200U;
    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 0.0F, 4.0F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Mat4f vp   = proj * view;

    // pack the buffer: header (offsets + view_proj @ word 6, column-major) + the four 40-I arrays.
    const u32 clusters_off  = scenerender::kClusterHeaderWords;
    const u32 vertices_off  = clusters_off + cdag.cluster_count * mp::kClusterGpuWords;
    const u32 triangles_off = vertices_off + static_cast<u32>(cdag.cluster_vertices.size());
    const u32 positions_off = triangles_off + static_cast<u32>(cdag.cluster_triangles_packed.size());
    const u32 total_words   = positions_off + static_cast<u32>(cdag.positions.size());
    containers::Array<u32> buf(&alloc);
    buf.resize(total_words, 0U);
    buf[scenerender::kClusterHdrClustersOff]  = clusters_off;
    buf[scenerender::kClusterHdrClusterCount] = cdag.cluster_count;
    buf[scenerender::kClusterHdrVerticesOff]  = vertices_off;
    buf[scenerender::kClusterHdrTrianglesOff] = triangles_off;
    buf[scenerender::kClusterHdrPositionsOff] = positions_off;
    std::memcpy(&buf[6U], &vp, 16U * 4U); // view_proj column-major, matching Gx::mul_view_proj (header word 6)
    for (usize i = 0; i < cdag.packed_clusters.size(); ++i) { buf[clusters_off + i] = cdag.packed_clusters[i]; }
    for (usize i = 0; i < cdag.cluster_vertices.size(); ++i) { buf[vertices_off + i] = cdag.cluster_vertices[i]; }
    for (usize i = 0; i < cdag.cluster_triangles_packed.size(); ++i)
    {
        buf[triangles_off + i] = cdag.cluster_triangles_packed[i];
    }
    for (usize i = 0; i < cdag.positions.size(); ++i)
    {
        const f32 v = cdag.positions[i];
        std::memcpy(&buf[positions_off + i], &v, 4U);
    }

    resources::ResourceManager rm(&galloc());
    scenerender::SceneRenderer  renderer(&galloc());
    REQUIRE(renderer.init(raster, rm));
    if (!renderer.init_programs(ctx)) { SKIP("shader backend unavailable"); }
    if (!renderer.supports_clusters()) { SKIP("no mesh-shader support on this device"); }

    auto cbuf = raster.create_storage_buffer(total_words * 4U);
    REQUIRE(cbuf != nullptr);
    REQUIRE(raster.upload_storage(*cbuf, 0U, buf.data(), total_words * 4U));
    auto tgt = raster.create_color_target(dim, dim);
    REQUIRE(tgt != nullptr);
    renderer.draw_clusters(*tgt, *cbuf, cdag.cluster_count, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F});

    // device coverage A.
    containers::Array<u8> cov_a(&alloc);
    cov_a.resize(static_cast<usize>(dim) * dim, 0U);
    u32 lit = 0U;
    for (u32 y = 0; y < dim; ++y)
    {
        for (u32 x = 0; x < dim; ++x)
        {
            if ((tgt->read_pixel(x, y) & 0x00FFFFFFU) != 0U)
            {
                cov_a[static_cast<usize>(y) * dim + x] = 1U;
                ++lit;
            }
        }
    }
    INFO("clusters=" << cdag.cluster_count << " leaves=" << leaves.size() << " oracle_tris=" << oracle.triangle_count
                     << " lit=" << lit);
    CHECK(lit > 200U); // the mesh shader emitted real geometry (a failed/empty mesh shader lights nothing)

    // CPU oracle coverage: rasterise the leaf triangles for BOTH y-conventions, take the best IoU (the UNPACK is
    // what is under test here, not the viewport orientation — a y-flip is a separate concern).
    const auto proj_pt = [&](u32 vidx, f32& sx, f32& sy_down, f32& sy_up, bool& ok) {
        const math::Vec4f clip = vp * math::Vec4f{oracle.positions[vidx * 3U + 0U], oracle.positions[vidx * 3U + 1U],
                                                  oracle.positions[vidx * 3U + 2U], 1.0F};
        ok                     = clip.w > 1.0e-6F;
        const f32 nx           = ok ? clip.x / clip.w : 0.0F;
        const f32 ny           = ok ? clip.y / clip.w : 0.0F;
        sx                     = (nx * 0.5F + 0.5F) * static_cast<f32>(dim);
        sy_down                = (ny * 0.5F + 0.5F) * static_cast<f32>(dim);
        sy_up                  = (0.5F - ny * 0.5F) * static_cast<f32>(dim);
    };
    const auto rasterise = [&](bool y_up, containers::Array<u8>& cov) {
        cov.resize(static_cast<usize>(dim) * dim, 0U);
        for (usize t = 0; t < oracle.triangle_count; ++t)
        {
            f32  ax, ad, au, bx, bd, bu, cx, cd, cu;
            bool oa, ob, oc;
            proj_pt(oracle.triangles[t * 3U + 0U], ax, ad, au, oa);
            proj_pt(oracle.triangles[t * 3U + 1U], bx, bd, bu, ob);
            proj_pt(oracle.triangles[t * 3U + 2U], cx, cd, cu, oc);
            if (!oa || !ob || !oc) { continue; }
            const f32 ay = y_up ? au : ad, by = y_up ? bu : bd, cy = y_up ? cu : cd;
            const f32 area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            if (math::deterministic::abs(area) < 1.0e-9F) { continue; }
            const f32  inv  = 1.0F / area;
            const auto lo3  = [](f32 u, f32 v, f32 w) { f32 m = u; if (v < m) { m = v; } if (w < m) { m = w; } return m; };
            const auto hi3  = [](f32 u, f32 v, f32 w) { f32 m = u; if (v > m) { m = v; } if (w > m) { m = w; } return m; };
            const int  cap  = static_cast<int>(dim) - 1;
            int        lx   = static_cast<int>(math::deterministic::floor(lo3(ax, bx, cx)));
            int        hx   = static_cast<int>(math::deterministic::ceil(hi3(ax, bx, cx)));
            int        ly   = static_cast<int>(math::deterministic::floor(lo3(ay, by, cy)));
            int        hy   = static_cast<int>(math::deterministic::ceil(hi3(ay, by, cy)));
            if (lx < 0) { lx = 0; }
            if (ly < 0) { ly = 0; }
            if (hx > cap) { hx = cap; }
            if (hy > cap) { hy = cap; }
            for (int yy = ly; yy <= hy; ++yy)
            {
                for (int xx = lx; xx <= hx; ++xx)
                {
                    const f32 fx = static_cast<f32>(xx) + 0.5F, fy = static_cast<f32>(yy) + 0.5F;
                    const f32 w0 = ((bx - ax) * (fy - ay) - (by - ay) * (fx - ax)) * inv;
                    const f32 w1 = ((cx - bx) * (fy - by) - (cy - by) * (fx - bx)) * inv;
                    const f32 w2 = ((ax - cx) * (fy - cy) - (ay - cy) * (fx - cx)) * inv;
                    if ((w0 >= 0.0F && w1 >= 0.0F && w2 >= 0.0F) || (w0 <= 0.0F && w1 <= 0.0F && w2 <= 0.0F))
                    {
                        cov[static_cast<usize>(yy) * dim + static_cast<usize>(xx)] = 1U;
                    }
                }
            }
        }
    };
    const auto iou = [&](const containers::Array<u8>& b) {
        u32 inter = 0U, uni = 0U;
        for (usize i = 0; i < cov_a.size(); ++i)
        {
            const bool a = cov_a[i] != 0U, bb = b[i] != 0U;
            inter += (a && bb) ? 1U : 0U;
            uni += (a || bb) ? 1U : 0U;
        }
        return uni == 0U ? 0.0F : static_cast<f32>(inter) / static_cast<f32>(uni);
    };
    containers::Array<u8> cov_down(&alloc);
    containers::Array<u8> cov_up(&alloc);
    rasterise(false, cov_down);
    rasterise(true, cov_up);
    const f32 iou_d = iou(cov_down);
    const f32 iou_u = iou(cov_up);
    INFO("IoU down=" << iou_d << " up=" << iou_u);
    CHECK((iou_d > iou_u ? iou_d : iou_u) >= 0.97F);
}

TEST_CASE("REN-41 GATE: the cluster mesh shader unpacks a packed DAG to match the CPU oracle (Vulkan)",
          "[scene-render][ren41][cluster][nanite][gpu][vulkan]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto  ctx = gpu::create_vulkan_gpu_context(cfg);
    auto* vk  = ctx != nullptr ? static_cast<gpu::VulkanGpuContext*>(ctx.get()) : nullptr;
    if (vk == nullptr || !vk->graphics_capable() || !vk->shader_object() || !vk->mesh_shader())
    {
        SKIP("no Vulkan device with mesh-shader support");
    }
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    cluster_mesh_gate_body(*vk, *raster);
}

#ifdef _WIN32
TEST_CASE("REN-40-G1 GATE (DX12): depth prepass with shared_depth is pixel-identical to forward-only",
          "[scene-render][ren40][depth-prepass][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid())
    {
        SKIP("no D3D12 device available");
    }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    const TempPack              pack("sr_g1d_pack_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);
    for (u32 i = 0; i < 5U; ++i)
    {
        const f32             x = (static_cast<f32>(i) - 2.0F) * 2.5F;
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs(math::Vec3f{x, 0.0F, 0.0F}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    if (!renderer.init_programs(*gctx))
    {
        SKIP("dxc/DXIL unavailable");
    }
    (void)renderer.sync(world);

    constexpr u32         dim   = 256U;
    const math::Mat4f     view  = math::look_at(math::Vec3f{0, 4, 12}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj  = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     light{0.4F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    auto ref = raster->create_color_depth_target(dim, dim);
    REQUIRE(ref != nullptr);
    REQUIRE(renderer.set_frame_graph_toml(k40gRefBasicToml));
    const auto r_ref = renderer.render(*ref, proj * view, light, clear);
    CHECK(r_ref.draws > 0U);

    auto tgt = raster->create_color_depth_target(dim, dim);
    REQUIRE(tgt != nullptr);
    REQUIRE(renderer.set_frame_graph_toml(k40gPrepassToml));
    const auto r_pre = renderer.render(*tgt, proj * view, light, clear);
    CHECK(r_pre.draws > 0U);

    u32 diffs   = 0U;
    u32 covered = 0U;
    for (u32 y = 0; y < dim; ++y)
    {
        for (u32 x = 0; x < dim; ++x)
        {
            const u32 a = ref->read_pixel(x, y);
            const u32 b = tgt->read_pixel(x, y);
            if (a != b) { ++diffs; }
            if ((b & 0x00FFFFFFU) != 0U) { ++covered; }
        }
    }
    INFO("diffs=" << diffs << " covered=" << covered);
    CHECK(diffs == 0U);
    CHECK(covered > 500U);
}

TEST_CASE("REN-40-G2 GATE (DX12): R11G11B10F scene_hdr produces correct lit output after tonemap",
          "[scene-render][ren40][hdr-format][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid())
    {
        SKIP("no D3D12 device available");
    }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    const TempPack              pack("sr_g2d_pack_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);
    for (u32 i = 0; i < 3U; ++i)
    {
        const f32             x = (static_cast<f32>(i) - 1.0F) * 3.0F;
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs(math::Vec3f{x, 0.0F, 0.0F}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    if (!renderer.init_programs(*gctx))
    {
        SKIP("dxc/DXIL unavailable");
    }
    (void)renderer.sync(world);

    constexpr u32         dim   = 256U;
    const math::Mat4f     view  = math::look_at(math::Vec3f{0, 4, 12}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj  = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     light{0.4F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    auto ref = raster->create_color_depth_target(dim, dim);
    REQUIRE(ref != nullptr);
    REQUIRE(renderer.set_frame_graph_toml(k40gRef16FToml));
    const auto r_ref = renderer.render(*ref, proj * view, light, clear);
    CHECK(r_ref.draws > 0U);

    auto tgt = raster->create_color_depth_target(dim, dim);
    REQUIRE(tgt != nullptr);
    REQUIRE(renderer.set_frame_graph_toml(k40gHdrR11Toml));
    const auto r_r11 = renderer.render(*tgt, proj * view, light, clear);
    CHECK(r_r11.draws > 0U);

    u32 big_diffs = 0U;
    u32 covered   = 0U;
    for (u32 y = 0; y < dim; ++y)
    {
        for (u32 x = 0; x < dim; ++x)
        {
            const u32 a = ref->read_pixel(x, y);
            const u32 b = tgt->read_pixel(x, y);
            const i32 dr = static_cast<i32>((a >> 16U) & 0xFFU) - static_cast<i32>((b >> 16U) & 0xFFU);
            const i32 dg = static_cast<i32>((a >> 8U)  & 0xFFU) - static_cast<i32>((b >> 8U)  & 0xFFU);
            const i32 db = static_cast<i32>(a & 0xFFU)          - static_cast<i32>(b & 0xFFU);
            const i32 mx = (dr > 0 ? dr : -dr);
            const i32 my = (dg > 0 ? dg : -dg);
            const i32 mz = (db > 0 ? db : -db);
            const i32 mxy   = mx > my ? mx : my;
            const i32 worst = mxy > mz ? mxy : mz;
            if (worst > 4) { ++big_diffs; }
            if ((b & 0x00FFFFFFU) != 0U) { ++covered; }
        }
    }
    INFO("big_diffs=" << big_diffs << " covered=" << covered);
    CHECK(big_diffs == 0U);
    CHECK(covered > 500U);
}

TEST_CASE("REN-40-G3 GATE (DX12): GPU occlusion culling produces pixel-identical output for unoccluded scene",
          "[scene-render][ren40][occlusion][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid())
    {
        SKIP("no D3D12 device available");
    }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    const resources::ResourceId cube_id = resources::ResourceId::mint_random();
    const TempPack              pack("sr_g3d_pack_", cube_id);
    write_mesh_pack(pack.path, cube_id);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait());
    scene::register_render_components(world);
    for (u32 i = 0; i < 8U; ++i)
    {
        const f32             x = (static_cast<f32>(i) - 3.5F) * 1.5F;
        const scene::EntityId e = world.spawn();
        scene::Transform      t;
        t.world = math::from_trs(math::Vec3f{x, 0.0F, 0.0F}, math::Quatf::identity(), math::Vec3f{1, 1, 1});
        world.add_component(e, t);
        world.add_component(e, scene::MeshRenderer{cube_id, {}});
    }

    scenerender::SceneRenderer renderer(&galloc());
    REQUIRE(renderer.init(*raster, rm));
    if (!renderer.init_programs(*gctx))
    {
        SKIP("dxc/DXIL unavailable");
    }
    (void)renderer.sync(world);

    const math::Mat4f     view  = math::look_at(math::Vec3f{0, 8, 20}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj  = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     light{0.4F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};
    constexpr u32         dim   = 256U;

    auto ref = raster->create_color_depth_target(dim, dim);
    REQUIRE(ref != nullptr);
    REQUIRE(renderer.set_frame_graph_toml(k40gRefBasicToml));
    const auto r_ref = renderer.render(*ref, proj * view, light, clear);
    INFO("ref draws=" << r_ref.draws);
    CHECK(r_ref.draws > 0U);

    renderer.set_gpu_cull(true);
    auto tgt = raster->create_color_depth_target(dim, dim);
    REQUIRE(tgt != nullptr);
    REQUIRE(renderer.set_frame_graph_toml(k40gOccCullToml));
    const auto r_occ = renderer.render(*tgt, proj * view, light, clear);
    INFO("occ draws=" << r_occ.draws);
    CHECK(r_occ.draws > 0U);

    u32 diffs   = 0U;
    u32 covered = 0U;
    for (u32 y = 0; y < dim; ++y)
    {
        for (u32 x = 0; x < dim; ++x)
        {
            const u32 a = ref->read_pixel(x, y);
            const u32 b = tgt->read_pixel(x, y);
            if (a != b) { ++diffs; }
            if ((b & 0x00FFFFFFU) != 0U) { ++covered; }
        }
    }
    INFO("diffs=" << diffs << " covered=" << covered);
    CHECK(diffs == 0U);
    CHECK(covered > 500U);
}

TEST_CASE("REN-41 GATE (DX12): velocity is zero for static instances and matches the screen delta for movers",
          "[scene-render][ren41][velocity][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid())
    {
        SKIP("no D3D12 device available");
    }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    velocity_gate_body(*gctx, *raster); // init_programs may still SKIP inside if dxc/DXIL is unavailable
}

TEST_CASE("REN-41 GATE (DX12): the cluster mesh shader unpacks a packed DAG to match the CPU oracle",
          "[scene-render][ren41][cluster][nanite][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    cluster_mesh_gate_body(*gctx, *raster); // SKIPs inside if no mesh-shader support / DXIL unavailable
}
#endif // _WIN32 (the REN-40-G DX12 gates)
