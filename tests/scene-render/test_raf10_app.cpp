// test_raf10_app.cpp — RAF-10 (D-007): AN APPLICATION-CUSTOM RENDERER, PROVEN END TO END.
//
// ⛔⛔⛔ THE GATE THIS EXISTS TO PASS. A small application — this file plus its `app_assets/` tree — customises the
// renderer in every way an app is meant to, on BOTH backends, WITHOUT editing a line of engine rendering code:
//   1. use the engine default graph unchanged                    (engine://frame/forward_basic)
//   2. include an engine graph as a subgraph                     (app_custom includes forward_basic)
//   3. inject a custom pass at a declared anchor                 (app_custom injects `app_grade`)
//   4. replace the display transform with an app one            (app://post/app_grade via register_post_asset)
//   5. supply an app material                                    (app://material/app_scene via set_scene_material)
//   6. supply an app shading technique                           (`app_tint` via define_technique)
//   7. select a fully app-authored frame graph                  (app://frame/app_authored)
//   8. register + use one small custom C++ pass executor        (`app://executor/grade` -> app_grade_executor)
//   9. use explicit capability fallback                          (app_gated requires an unmodelled cap -> app_basic)
//  10. run on both backends where available                     (the Vulkan gate + its DX12 twin)
//
// ⛔ GATE-10 (the boundary): this file includes ONLY PUBLIC engine headers (`<crd/scenerender/…>`, `<crd/gpu/…>`,
// `<crd/rendergraph/…>`, `<crd/renderpass/…>`, `<crd/kir/…>`, `<crd/scene/…>`, `<crd/resources/…>`, `<crd/math/…>`).
// It names NO engine-private renderer method, adds NO backend virtual, edits NO central pass enum, hard-codes NO
// backend slot, embeds NO frame asset as a C++ string, and takes NO privileged engine-only path. Every customisation
// goes through a public seam. That the compiler accepts this translation unit against the public surface IS the proof.

#include <crd/gpu/command_model.hpp>            // the custom executor's canonical command model (public)
#include <crd/gpu/context.hpp>
#include <crd/gpu/program.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#if defined(_WIN32) // the D3D12 backend exists only on Windows (the F17 guard rule) — the DX12 twin rides the guard
#include <crd/gpu/dx12_context.hpp>
#include <crd/gpu/dx12_raster_context.hpp>
#endif
#include <crd/kir/ckir_technique.hpp>           // the app shading technique (public CKIR authoring surface)
#include <crd/math/mat.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/rendergraph/frame_graph.hpp>      // RecordContext / PassRecordFn (public)
#include <crd/renderpass/executor_registry.hpp> // PassPayload / pass_param_id (public)
#include <crd/resources/crdr.hpp>
#include <crd/resources/mesh_resource.hpp>
#include <crd/resources/resource_manager.hpp>
#include <crd/scene/render_components.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>
#include <crd/scenerender/scene_renderer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib> // std::getenv (asset roots ride ctest ENVIRONMENT)
#include <cstring>

using namespace crd;

namespace
{

memory::TlsfAllocator& galloc()
{
    static memory::TlsfAllocator a(256U << 20U);
    return a;
}

// ── the cube mesh fixture (self-contained, mirrors the other scene-render GPU gates) ─────────────────────────────
[[nodiscard]] containers::Array<u8> build_cube_mesh_crdr(const resources::ResourceId& id)
{
    auto*                 a = &galloc();
    containers::Array<u8> verts(a);
    for (u32 corner = 0; corner < 8U; ++corner)
    {
        const f32   x       = (corner & 1U) != 0U ? 0.5F : -0.5F;
        const f32   y       = (corner & 2U) != 0U ? 0.5F : -0.5F;
        const f32   z       = (corner & 4U) != 0U ? 0.5F : -0.5F;
        const f32   rec[12] = {x, y, z, 0, 1, 0, 0, 0, 0, 0, 0, 1};
        const auto* b       = reinterpret_cast<const u8*>(rec);
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
    const char            name[] = "cube";
    for (char c : name) { pool.push_back(static_cast<u8>(c)); }
    containers::Array<resources::ManifestEntry> entries(a);
    resources::ManifestEntry                    e;
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

// ── ⭐⭐ RAF-10 requirement 6: THE APP'S OWN SHADING TECHNIQUE. ───────────────────────────────────────────────────
// Built with the SAME public CKIR authoring API the engine uses for its own techniques (`scene_authored_technique`).
// It reuses the engine's proven directional-light response, then applies the app's signature warm tint — a distinct,
// named technique the app supplies through `define_technique` and selects by name via `set_forward_technique`.
int body_app_tint(kir::KGraph& g, const kir::technique::TechniqueContext& tc, void* /*user*/)
{
    namespace ktech = kir::technique;
    const int lit    = kir::lighting::directional_light(g, tc.fixed[ktech::kTiBaseColor], tc.fixed[ktech::kTiMetallic],
                                                        tc.fixed[ktech::kTiRoughness], tc.fixed[ktech::kTiNormal],
                                                        tc.fixed[ktech::kTiViewDir], tc.fixed[ktech::kTiLightDir],
                                                        tc.fixed[ktech::kTiLightColor]);
    const int shaded = g.binary(kir::KOp::Add, lit, tc.fixed[ktech::kTiEmissive]);
    const auto s1    = kir::make_shape({1});
    const int  tint  = g.vec3(g.constant(1.15, s1, kir::DType::F32), g.constant(0.92, s1, kir::DType::F32),
                              g.constant(0.78, s1, kir::DType::F32));
    return kir::nodes::clamp01(g, g.binary(kir::KOp::Mul, shaded, tint));
}

[[nodiscard]] kir::technique::Technique make_app_tint_technique()
{
    kir::technique::Technique t;
    t.name       = "app_tint";
    t.body       = &body_app_tint;
    t.bindings   = nullptr; // no pass-frequency bindings (no shadow atlas) — like the engine's `standard_forward`
    t.n_bindings = 0;
    t.options    = nullptr;
    t.n_options  = 0;
    return t;
}

// ── ⭐⭐ RAF-10 requirement 8: THE APP'S OWN CUSTOM C++ PASS EXECUTOR. ────────────────────────────────────────────
// A `kind = "custom"` frame pass names `app://executor/grade`; the engine resolves THIS record fn in the SAME
// executor table a builtin uses and drives it with the resolved (payload, context, encoder). It touches only the
// slots the pass declared: it reads `input0` (the HDR scene the included engine subgraph rendered) and writes
// `color` (the display output), drawing a fullscreen triangle with the app's grade program (requirement 4). No new
// FramePassKind, no backend virtual — the id is the extension point.
void app_grade_executor(const renderpass::PassPayload& /*payload*/, rendergraph::RecordContext& ctx,
                        gpu::ICommandEncoder& encoder)
{
    gpu::IRasterTarget* color = ctx.color_target(renderpass::pass_param_id(containers::StringView("color")));
    if (!ctx.ok() || color == nullptr) { return; }
    gpu::ITexture* scene = ctx.texture(renderpass::pass_param_id(containers::StringView("input0")));
    if (scene == nullptr) { return; }
    gpu::RenderingDesc rd;
    rd.width  = color->width();
    rd.height = color->height();
    rd.color.push_back(gpu::ColorAttachmentDesc{color, gpu::LoadOp::Clear, gpu::StoreOp::Store,
                                                gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, gpu::BlendMode::Opaque});
    gpu::RasterDrawPacket p;
    p.program                        = ctx.programs().raster;
    p.command                        = gpu::RasterCommandKind::Draw;
    p.geometry.kind                  = gpu::GeometryKind::None;
    p.geometry.vertex_or_index_count = 3U; // the fullscreen triangle
    p.bindings.push_back(gpu::ResourceBinding{renderasset::BindingFrequency::Material,
                                              renderasset::BindingKind::SampledTexture, 0U, nullptr, scene});
    encoder.begin_rendering(rd);
    encoder.draw(p);
    encoder.end_rendering();
}

// GEOMETRY coverage: texels that differ from the corner (0,0) — which, with a centred cube, is always the background
// clear. This measures "the scene actually drew", independent of what colour each frame clears to.
[[nodiscard]] u32 geometry_pixels(gpu::IRasterTarget& t, u32 w, u32 h)
{
    const u32 bg = t.read_pixel(0U, 0U);
    u32       n  = 0U;
    for (u32 y = 0; y < h; ++y)
    {
        for (u32 x = 0; x < w; ++x)
        {
            if (t.read_pixel(x, y) != bg) { ++n; }
        }
    }
    return n;
}
[[nodiscard]] u32 diff_pixels(gpu::IRasterTarget& a, gpu::IRasterTarget& b, u32 w, u32 h)
{
    u32 d = 0U;
    for (u32 y = 0; y < h; ++y)
    {
        for (u32 x = 0; x < w; ++x)
        {
            if (a.read_pixel(x, y) != b.read_pixel(x, y)) { ++d; }
        }
    }
    return d;
}

// ── THE SHARED BODY: the whole application, run against a caller-supplied backend. ────────────────────────────────
// Vulkan and DX12 both drive this — the customisations are backend-neutral public API, so the SAME app code proves
// the SAME ten requirements on each device (requirement 10).
void run_raf10_app(gpu::IGpuContext& gctx, gpu::IRasterContext& raster, const char* engine_root, const char* app_root)
{
    constexpr u32 width = 256U;
    constexpr u32 height = 256U;

    // a single lit cube — enough that every frame draws a covered, comparable image
    const resources::ResourceId mesh = resources::ResourceId::mint_random();
    const TempPack              pack("sr_raf10_", mesh);
    write_mesh_pack(pack.path, mesh);
    resources::ResourceManager rm(&galloc());
    resources::register_mesh_loader(&rm, nullptr);
    REQUIRE(rm.mount_manifest(pack.path.generic()).is_valid());

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);
    const scene::EntityId e = world.spawn();
    scene::Transform      t;
    t.translation = math::from_raw_vec<units::dim::Length>(math::Vec3f{0.0F, 0.0F, 0.0F});
    t.scale       = {2.0F, 2.0F, 2.0F};
    t.world       = math::from_trs({0.0F, 0.0F, 0.0F}, math::Quatf::identity(), {2.0F, 2.0F, 2.0F});
    world.add_component(e, t);
    world.add_component(e, scene::MeshRenderer{mesh, {}});

    scenerender::SceneRenderer renderer(&galloc());

    // ── every customisation is a PUBLIC seam call, no engine edit ──
    REQUIRE(renderer.set_asset_root(engine_root));          // engine:// == the shipped assets/ tree
    REQUIRE(renderer.set_app_asset_root(app_root));         // app://   == this app's own tree (requirement: the mount)
    REQUIRE(renderer.init(raster, rm));
    // (5) the app's material, (6) the app's technique — supplied + selected before programs are cooked
    REQUIRE(renderer.set_scene_material("app://material/app_scene", "app://material/app_scene"));
    REQUIRE(renderer.define_technique(make_app_tint_technique()));
    renderer.set_forward_technique("app_tint");
    // (4) the app's display transform program, (8) the app's custom pass executor
    REQUIRE(renderer.register_post_asset("app://post/app_grade", "app://post/app_grade"));
    REQUIRE(renderer.register_pass_executor("app://executor/grade", &app_grade_executor));
    REQUIRE(renderer.init_programs(gctx));
    REQUIRE(renderer.sync(world).total_instances == 1U);

    const math::Mat4f     view = math::look_at(math::Vec3f{0.0F, 6.0F, 16.0F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     light{0.3F, 1.0F, 0.2F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};

    const auto render_into = [&](const char* frame_id, gpu::IRasterTarget& target) {
        const bool installed = renderer.set_frame_graph(frame_id);
        REQUIRE(installed);
        return renderer.render(target, proj * view, light, clear, nullptr);
    };

    auto eng_tgt   = raster.create_color_depth_target(width, height); // (1) engine default, unchanged
    auto app_tgt   = raster.create_color_depth_target(width, height); // (5,6,7) fully app-authored
    auto cust_tgt  = raster.create_color_depth_target(width, height); // (2,3,4,8) composed + injected custom pass
    auto gated_tgt = raster.create_color_depth_target(width, height); // (9) capability step-down
    auto basic_tgt = raster.create_color_depth_target(width, height); // (9) the fallback rendered directly, for comparison
    REQUIRE(eng_tgt != nullptr);
    REQUIRE(app_tgt != nullptr);
    REQUIRE(cust_tgt != nullptr);
    REQUIRE(gated_tgt != nullptr);
    REQUIRE(basic_tgt != nullptr);

    // (1) THE ENGINE DEFAULT GRAPH, UNCHANGED — selected by canonical id, rendered as the engine ships it.
    render_into("engine://frame/forward_basic", *eng_tgt);
    const u32 eng_geo = geometry_pixels(*eng_tgt, width, height);
    INFO("engine default geometry=" << eng_geo);
    CHECK(eng_geo > 500U);

    // (5,6,7) A FULLY APP-AUTHORED GRAPH with the app's OWN material + technique.
    render_into("app://frame/app_authored", *app_tgt);
    const u32 app_geo = geometry_pixels(*app_tgt, width, height);
    INFO("app_authored geometry=" << app_geo);
    CHECK(app_geo > 500U);
    // the app material + app technique change the shading, so the app frame is NOT the engine default
    CHECK(diff_pixels(*eng_tgt, *app_tgt, width, height) > 0U);

    // (2,3,4,8) THE COMPOSED GRAPH: an included engine subgraph renders the scene, an injected CUSTOM C++ pass
    // applies the app's display transform. It draws (geometry present) and — being tonemapped — differs from the
    // ungraded app-authored frame.
    render_into("app://frame/app_custom", *cust_tgt);
    const u32 cust_geo = geometry_pixels(*cust_tgt, width, height);
    INFO("app_custom geometry=" << cust_geo);
    CHECK(cust_geo > 500U);
    CHECK(diff_pixels(*cust_tgt, *app_tgt, width, height) > 0U); // the grade changed the image

    // (9) CAPABILITY STEP-DOWN: app_gated requires an unmodelled capability, so it must render its declared fallback
    // (app_basic), never its own (green) body. It must equal app_basic rendered directly.
    render_into("app://frame/app_gated", *gated_tgt);
    render_into("app://frame/app_basic", *basic_tgt);
    INFO("gated vs basic diff=" << diff_pixels(*gated_tgt, *basic_tgt, width, height));
    CHECK(diff_pixels(*gated_tgt, *basic_tgt, width, height) == 0U); // the step-down landed exactly on the fallback
    CHECK(geometry_pixels(*gated_tgt, width, height) > 500U);        // and it actually rendered (the fallback), not nothing

    // (9, explicit) the public capability seam reports the REAL device state — selection is inspectable, not a guess
    CHECK(renderer.capability("bindless") == raster.supports_bindless());
    CHECK(renderer.capability("app_wants_feature_x") == false); // an unmodelled capability is unsupported, by rule
}

} // namespace

// ── ⭐⭐ RAF-10 GATE (Vulkan): the application customises the renderer ten ways and renders on the primary backend. ─
TEST_CASE("RAF-10 GATE: an application customises the renderer without engine edits (Vulkan)",
          "[scene-render][raf10][gpu][vulkan]")
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
    const char* eroot = std::getenv("CRD_ASSETS_DIR");
    const char* aroot = std::getenv("CRD_APP_ASSETS_DIR");
    if (eroot == nullptr || eroot[0] == '\0') { SKIP("CRD_ASSETS_DIR not set (run through ctest)"); }
    if (aroot == nullptr || aroot[0] == '\0') { aroot = CRD_RAF10_APP_ASSETS_DIR; } // compiled fallback (see CMakeLists SCAR)
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    run_raf10_app(*vk, *raster, eroot, aroot);
}

#if defined(_WIN32)
// ── ⭐⭐ RAF-10 GATE (DX12): the SAME application, unchanged, on the OTHER backend (requirement 10). ───────────────
TEST_CASE("RAF-10 GATE (DX12): an application customises the renderer without engine edits",
          "[scene-render][raf10][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr) { SKIP("no D3D12 device"); }
    auto raster = gpu::create_dx12_raster_context();
    if (raster == nullptr) { SKIP("no D3D12 raster context"); }
    const char* eroot = std::getenv("CRD_ASSETS_DIR");
    const char* aroot = std::getenv("CRD_APP_ASSETS_DIR");
    if (eroot == nullptr || eroot[0] == '\0') { SKIP("CRD_ASSETS_DIR not set (run through ctest)"); }
    if (aroot == nullptr || aroot[0] == '\0') { aroot = CRD_RAF10_APP_ASSETS_DIR; } // compiled fallback (see CMakeLists SCAR)
    run_raf10_app(*gctx, *raster, eroot, aroot);
}
#endif
