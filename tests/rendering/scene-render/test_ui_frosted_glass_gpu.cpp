// test_ui_frosted_glass_gpu.cpp — CEIR-31b-4-b: the on-device numeric gates for the §141 frosted-glass chain
// (assets/frame/ui_frosted_glass.frame.toml). Its OWN translation unit: the frosted-glass device arms grow across
// 31b-4-b-i..iv, and test_scene_render_gpu.cpp is already a ~7.8k-line TU that MSVC codegen ICE'd (C1001) when this
// was appended to it — a dedicated TU is both the fix and the right home. Headless Vulkan + a DX12 twin (same body).
//
// 31b-4-b-i (this file's first gate): arms (a)+(i) of the device ledger, PLUS the 0-draw clear arm. (a) the panel MASK
// is actually READ by the composite; (i) the mask pass is `reads=[]` (the FIRST 0-texture-read fullscreen pass engine-
// wide) and records clean. 31b-4-b-ii-1 rides the SAME shared arm: (d) cache-hit H==H — render A twice with an identical
// spec-set and assert both a byte-identical framebuffer AND a stable ensure_ui_program (kind, spec-set) cache count.
// ⛔ Landing the b-i arm exposed THREE renderer defects in one family — an authored frame over an
// EMPTY world (the ui panel needs no scene geometry) had never been rendered end-to-end before: (1) render() short-
// circuited the whole frame on an empty draw list (scene_renderer.cpp:6799); (2) the recorder rejected a clear-only
// geometry pass as UnresolvedProgram — its program comes from the first draw, absent when empty (frame_runtime.cpp:874);
// (3) the command-lowering encoder folds a scope's LoadOp::Clear into its FIRST draw verb, so a 0-draw pass cleared
// NOTHING (command_lowering.hpp + IRasterContext::clear_scope). All three are fixed; this gate's record_ok/build_ok
// REQUIREs + the uniform-backdrop CHECK pin them.

#include <crd/gpu/context.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/gpu/vulkan_validation_capture.hpp>
#if defined(_WIN32) // the D3D12 backend exists only on Windows; the DX12 twin rides the same guard as the other gates
#include <crd/gpu/dx12_context.hpp>
#include <crd/gpu/dx12_raster_context.hpp>
#endif
#include <crd/math/mat.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>             // CEIR-31b-4-b-iii: ResourceId / kFourCC_* / CrdrWriter for the quad pack
#include <crd/resources/mesh_resource.hpp>    // register_mesh_loader
#include <crd/resources/openpbr_material.hpp> // PbrmParams / pbrm_build / register_openpbr_material_loader (the quad's surface)
#include <crd/resources/resource_manager.hpp>
#include <crd/scene/render_components.hpp>
#include <crd/scene/spatial_bvh_index.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>
#include <crd/scenerender/scene_renderer.hpp>

#include "../../gpu/gpu-shared/quad_scene_fixture.hpp"   // CEIR-31b-4-b-iii: build_quad_mesh_crdr + write_one_pack (the geometry hard-edge)
#include "../../gpu/gpu-shared/ui_tint_noise_oracle.hpp" // CEIR-31b-4-b-ii-2 arm (e): ref_hash + noise_from_hash_f32, shared with the eval gate

#include <catch2/catch_test_macros.hpp>

#include <cmath>   // std::lround — the oracle's UNORM8 quantization (match the store's round-to-nearest)
#include <cstdlib> // std::getenv for CRD_ASSETS_DIR (assets are disk-only)

using namespace crd;

namespace
{
memory::TlsfAllocator& galloc()
{
    static memory::TlsfAllocator a(64U << 20U);
    return a;
}

// The shipped authored asset, read from the ctest-provided CRD_ASSETS_DIR (the gates-run-configs rule). False if the
// root is unset or the file is missing, so a REQUIRE at the call site fails loudly. (_CRT_SECURE_NO_WARNINGS lets getenv through.)
[[nodiscard]] bool read_shipped_asset(const char* rel, containers::String& out)
{
    const char* root = std::getenv("CRD_ASSETS_DIR");
    if (root == nullptr || root[0] == '\0') { return false; }
    containers::String p(&galloc());
    p.append(root);
    p.append("/");
    p.append(rel);
    if (!platform::fs::read_file_text(platform::fs::Path(containers::StringView(p.c_str(), p.size())), out)) { return false; }
    // ⛔ CEIR-31b-4-b-iv-g-2: normalize CRLF→LF so the multi-line patch_replace needles (the 3-line `derive_spec_2_*` blocks)
    // match regardless of the checkout's line endings. read_file_text is BINARY (no strip), and .gitattributes has NO
    // `*.toml eol=lf` rule (only `* text=auto`), so a Windows checkout with core.autocrlf rewrites the asset CRLF — which
    // would break every step-override arm (the single-line patches were immune; a needle that spans lines is not).
    usize w = 0;
    for (usize r = 0; r < out.size(); ++r)
    {
        if (out.data()[r] != '\r') { out.data()[w++] = out.data()[r]; }
    }
    out.resize(w);
    return true;
}

// A same-width in-place byte patch of the FIRST occurrence of `needle` → `repl` (no std::string — the no-std-in-tests
// rule). Returns false if not found. needle.size() must equal repl.size() (a length change would need a resize).
[[nodiscard]] bool patch_same_width(containers::String& buf, const char* needle, const char* repl)
{
    const usize nlen = containers::StringView(needle).size();
    for (usize i = 0; i + nlen <= buf.size(); ++i)
    {
        bool m = true;
        for (usize j = 0; j < nlen; ++j)
        {
            if (buf.data()[i + j] != needle[j]) { m = false; break; }
        }
        if (m)
        {
            for (usize j = 0; j < nlen; ++j) { buf.data()[i + j] = repl[j]; }
            return true;
        }
    }
    return false;
}

// A VARIABLE-width in-place replace of the FIRST occurrence of `needle` → `repl` (their lengths may differ). Rebuilds the
// String (no std::string — the no-std-in-tests rule). CEIR-31b-4-b-iv-g-2: swaps a 3-line `derive_spec_2_{read,axis,op}`
// block for a one-line `spec_2 = X` literal so the explicit-step arms keep working once the baked literal is gone. Returns
// false if `needle` is not found. ⛔ blur_v1 and blur_v2 share the identical blur_a/axis-y block, so callers patch in FILE
// ORDER (h1,v1,h2,v2) — v1's replace consumes the first, v2's the second (as the old identical-literal patching did).
[[nodiscard]] bool patch_replace(containers::String& buf, const char* needle, const char* repl)
{
    const usize nlen = containers::StringView(needle).size();
    for (usize i = 0; i + nlen <= buf.size(); ++i)
    {
        bool m = true;
        for (usize j = 0; j < nlen; ++j)
        {
            if (buf.data()[i + j] != needle[j]) { m = false; break; }
        }
        if (m)
        {
            containers::String out(&galloc());
            out.append(buf.data(), i);                                // the prefix [0, i)
            out.append(repl);                                         // the replacement (null-terminated)
            out.append(buf.data() + i + nlen, buf.size() - i - nlen); // the suffix [i+nlen, end)
            buf = out;
            return true;
        }
    }
    return false;
}

// Read one pixel's RGB (an RGBA8 @output target packs one byte per channel, red in the low byte — both backends).
struct Rgb
{
    int r;
    int g;
    int b;
};
[[nodiscard]] Rgb pixel_rgb(gpu::IRasterTarget& rt, u32 x, u32 y)
{
    const u32 p = rt.read_pixel(x, y);
    return Rgb{static_cast<int>(p & 0xFFU), static_cast<int>((p >> 8U) & 0xFFU), static_cast<int>((p >> 16U) & 0xFFU)};
}

// FNV-1a 64 over EVERY RGBA8 pixel of the target — a whole-framebuffer fingerprint for arm (d). Two renders of the
// same graph/spec-set must produce the identical fingerprint (a byte-identical framebuffer, not just a matching centre
// sample); a differing hash means the cached program or the render is non-deterministic — a real finding, not noise.
[[nodiscard]] u64 fb_hash(gpu::IRasterTarget& rt, u32 w, u32 h)
{
    u64 hsh = 1469598103934665603ULL; // FNV-1a 64-bit offset basis
    for (u32 y = 0U; y < h; ++y)
    {
        for (u32 x = 0U; x < w; ++x) { hsh = (hsh ^ static_cast<u64>(rt.read_pixel(x, y))) * 1099511628211ULL; }
    }
    return hsh;
}

// ── ⭐⭐ THE SHARED ARM (both backends). The authored §141 ui_frosted_glass frame RENDERS on a real device, and the panel
// MASK is actually READ by the composite. ⛔ ISOLATION: no geometry — the scene pass CLEARS scene_color to a BRIGHT
// orange (patched from the shipped dark clear), so the backdrop is a uniform bright orange in BOTH renders; the ONLY
// thing that changes is the mask rect (4 spec-consts). Render A: rect [0.25,0.75] COVERS the centre → composite outputs
// EFFECT (tint × blurred backdrop). Render B: rect shrunk to [0.25,0.40] EXCLUDES the centre → composite outputs SCENE
// (raw orange). The centre MUST change (arm a: mask_rect WRITES, the composite READS bindless layer 2's `.r`, the spec-
// consts APPLY). B's centre AND corner are both raw backdrop → a bright, UNIFORM orange proves the 0-draw clear landed
// (the empty-world clear defect). Cross-backend memcmp is NOT required (tint_noise's hash is per-backend); the tint
// margin (~10% on r,g of a bright backdrop) is the signal. The Vulkan caller wraps this in a ValidationCapture (arm i).
void run_frosted_glass_mask_arm(gpu::IRasterContext& raster, gpu::IGpuContext& gpu_ctx)
{
    if (!raster.supports_bindless()) { SKIP("device does not support bindless texture arrays (the composite read heap)"); }

    containers::String graph(&galloc());
    REQUIRE(read_shipped_asset("frame/ui_frosted_glass.frame.toml", graph));
    // BRIGHT backdrop so the tint makes a clear margin: the scene pass clears scene_color to orange (same-width patch of
    // the shipped dark clear; "0.09, 0.10, 0.13" appears ONLY in the scene clear_color — the mask clears to 0,0,0,0).
    REQUIRE(patch_same_width(graph, "0.09, 0.10, 0.13", "0.80, 0.40, 0.20"));

    // an EMPTY world — no geometry needed: the uniform bright clear IS the backdrop, the ui fullscreen chain runs on it,
    // and effect≠scene comes from the tint, not from spatial content (that is 31b-4-b-iii's hard edge).
    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);

    resources::ResourceManager rm(&galloc());
    scenerender::SceneRenderer  renderer(&galloc());
    REQUIRE(renderer.init(raster, rm));
    if (!renderer.init_programs(gpu_ctx)) { SKIP("the device's shader toolchain (shaderc / dxc) is unavailable"); }
    REQUIRE(renderer.set_frame_graph_toml(graph.c_str())); // render A: rect [0.25,0.75] covers the centre
    (void)renderer.sync(world);                            // 0 instances — the scene pass clears, draws nothing

    auto target = raster.create_color_depth_target(64U, 64U);
    REQUIRE(target != nullptr);
    const math::Mat4f view = math::look_at(math::Vec3f{0.0F, 0.0F, 2.2F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f lightdir{0.0F, 0.0F, 1.0F};
    // ⛔ render()'s `clear` arg is IGNORED for an authored frame (SceneHost `(void)m_clear`); the scene clear is the
    // toml clear_color we patched to orange. Passed here for signature completeness only.
    const gpu::ClearColor clear{0.80F, 0.40F, 0.20F, 1.0F};

    (void)renderer.render(*target, proj * view, lightdir, clear, nullptr);
    const Rgb a = pixel_rgb(*target, 32U, 32U);
    // ⛔ DISCRIMINATOR (the three empty-world defects): a frame REJECTED at record() (target never written → black) or
    // one that never entered the record path both leave a black frame a pixel CHECK cannot tell from a real device bug.
    // render() sets fill_record_ok = 1 on a recorded frame, 100 + FrameExecError on a record rejection, 200 for no graph,
    // 0 if the record path was never reached (the :6799 short-circuit). read_gpu_cull_counts fills these unconditionally
    // (its bool return is false only because the empty scene has no cull GROUPS — expected). A bad frame aborts HERE.
    scenerender::SceneRenderer::GpuCullCounts diag{};
    (void)renderer.read_gpu_cull_counts(diag);
    UNSCOPED_INFO("31b-4-b-i A: record_ok=" << diag.fill_record_ok << " (0=not-reached, 1=ok, 100+ferr=rejected, 200=no-graph)"
                                            << "  build_ok=" << diag.fill_build_ok);
    REQUIRE(diag.fill_record_ok == 1U); // ⛔ the empty-world frame RECORDS (defects at render:6799 + record:874 fixed)
    REQUIRE(diag.fill_build_ok == 1U);

    // ── ⭐⭐ CEIR-31b-4-b-ii-1 (arm d): CACHE-HIT H==H on device. Render A a SECOND time with the IDENTICAL graph and
    // spec-set — no re-sync, no patch, nothing changed. The frame MUST be byte-identical (a deterministic device path
    // over the PERSISTENT ensure_ui_program cache), AND that (kind, spec-set) table MUST hold the EXACT expected count.
    // ⛔ IDENTITY not category (feedback_gate_assertions_check_identity_not_category): the exact count, derived from
    // ui_frosted_glass.frame.toml's raster.fullscreen passes by distinct (kind, spec-set) — backdrop_fetch(1) + blur
    // H{1,0,step} + blur V{0,1,step} (h1/h2 share H, v1/v2 share V ⇒ 2) + tint_noise(1) + mask_rect(1) + composite(1)
    // = 6. A `> 0` check would MISS the very bug arm (d) exists for: a mis-keyed cache that fails to dedup h2→h1 cooks
    // a 7th/8th variant on render 1, stable on render 2, H==H — and passes `> 0`. Pinning 6 catches it, AND prints the
    // real count on mismatch (the visibility a green ctest run hides). H==H then proves the RE-render is deterministic;
    // the count stability across the two renders proves render 2 actually HIT the cache rather than re-cooking.
    constexpr u32 expected_ui_variants = 6U;
    const u32     vc0                   = renderer.ui_variant_count();
    const u64     hash_a1               = fb_hash(*target, 64U, 64U);
    (void)renderer.render(*target, proj * view, lightdir, clear, nullptr); // render A again — the pure cache-hit path
    const u64 hash_a2 = fb_hash(*target, 64U, 64U);
    UNSCOPED_INFO("31b-4-b-ii-1: ui_variants=" << vc0 << " (expect " << expected_ui_variants << ")  hashA1=" << hash_a1
                                               << "  hashA2=" << hash_a2);
    CHECK(vc0 == expected_ui_variants);        // ⛔ the EXACT (kind, spec-set) variant count — the dedup tooth
    CHECK(hash_a2 == hash_a1);                 // ⛔ cache-hit is byte-identical: a deterministic device render
    CHECK(renderer.ui_variant_count() == vc0); // ⛔ render 2 did NOT re-cook — the (kind, spec-set) key HIT

    // MOVE the rect: shrink x1,y1 0.75 → 0.40 so [0.25,0.40] no longer covers the centre (uv ~0.5). spec_2/spec_3 = 0.75
    // appear ONLY in the mask pass (the blur passes DERIVE spec_2 and have no spec_3), so each needle is unique.
    REQUIRE(patch_same_width(graph, "spec_2 = 0.75", "spec_2 = 0.40"));
    REQUIRE(patch_same_width(graph, "spec_3 = 0.75", "spec_3 = 0.40"));
    REQUIRE(renderer.set_frame_graph_toml(graph.c_str())); // render B: rect [0.25,0.40] excludes the centre
    (void)renderer.sync(world);
    (void)renderer.render(*target, proj * view, lightdir, clear, nullptr);
    const Rgb b = pixel_rgb(*target, 32U, 32U);
    // ⛔ THE 0-DRAW CLEAR ARM: the CORNER (uv~0.04) is ALSO outside the mask rect [0.25,0.40], so B outputs scene_color
    // there too. With ZERO geometry the scene pass records no draw; the ONLY thing that fills scene_color is the pass's
    // LoadOp::Clear — and the lowering encoder folds that clear into its first draw verb, so a 0-draw pass cleared
    // NOTHING until IRasterContext::clear_scope (CEIR-31b-4-b-i). A bright, UNIFORM backdrop (corner==centre) is the
    // proof the empty-world clear now lands: black-everywhere was the pre-fix symptom, a spatial split would be a leak.
    const Rgb corner = pixel_rgb(*target, 2U, 2U);

    // ⭐ THE CLAIM (arm a): the SAME centre pixel differs between A (effect: tinted-blurred) and B (scene: raw orange) —
    // only the mask changed, so the composite MUST be reading the mask. B is the raw bright backdrop (a positive control).
    const int d     = (a.r > b.r ? a.r - b.r : b.r - a.r) + (a.g > b.g ? a.g - b.g : b.g - a.g)
                  + (a.b > b.b ? a.b - b.b : b.b - a.b);
    const int cornd = (corner.r > b.r ? corner.r - b.r : b.r - corner.r) + (corner.g > b.g ? corner.g - b.g : b.g - corner.g)
                      + (corner.b > b.b ? corner.b - b.b : b.b - corner.b);
    UNSCOPED_INFO("31b-4-b-i: centre A(effect)=" << a.r << "," << a.g << "," << a.b << "  B(scene)=" << b.r << "," << b.g
                                                 << "," << b.b << "  |sum diff|=" << d << "  B corner=" << corner.r << ","
                                                 << corner.g << "," << corner.b);
    CHECK(b.r > 40);   // B is the raw bright orange backdrop — a bright, non-black centre (the empty-world clear landed)
    CHECK(cornd <= 8); // ⛔ the 0-draw clear is UNIFORM (corner == centre): scene_color filled everywhere, not a spatial leak
    CHECK(d >= 10);    // ⛔ the mask flips effect↔scene at the centre — the tint margin, well above the amp-0.03 noise
}

// ── ⭐⭐ CEIR-31b-4-b-ii-2 arm (e): the tint_noise integer hash computes CORRECTLY on the GPU (both backends). 31b-1a-iii
// proved eval==C++ (the scalar build_hash_scalar oracle); THIS proves GPU==eval on a real device. ⛔ NOT through the
// frosted-glass chain (blur/composite/mask smear the noise) — a DEDICATED probe frame (ui_tint_noise_probe.frame.toml)
// runs ONE raster.fullscreen tint_noise over a cleared backdrop with tint=0 (spec_0..3) + amp=1 (spec_4), so @output is
// the RAW noise: splat(noise(hash((u32)FragCoord.xy))). The noise keys on FragCoord (window-space, top-origin on BOTH
// backends) and tint=0 removes the UV-dependent backdrop term, so the output is Y-FLIP-INVARIANT and BACKEND-AGNOSTIC —
// which is why ONE oracle serves both backends, and both matching it transitively proves cross-backend agreement (a free
// tooth). @output is R8G8B8A8_UNORM (LINEAR, verified on both backends), so the readback byte is round(noise·255): the
// oracle computes `(u32)(noise·255 + 0.5)` and (⛔ match-the-hardware) noise itself in FLOAT, the SAME precision as the
// shader's float(uint) (feedback_oracle_must_round_every_elementary_op). Per pixel: within ±1 LSB (UNORM8 tie handling)
// AND a MAJORITY exact — a wrong hash (arithmetic shift, swapped multiplier, mis-wired x/y) gives ~0 exact, not ~4090.
void run_tint_noise_hash_arm(gpu::IRasterContext& raster, gpu::IGpuContext& gpu_ctx)
{
    containers::String graph(&galloc());
    REQUIRE(read_shipped_asset("frame/ui_tint_noise_probe.frame.toml", graph));

    scene::World world{&galloc()};
    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);

    resources::ResourceManager rm(&galloc());
    scenerender::SceneRenderer  renderer(&galloc());
    REQUIRE(renderer.init(raster, rm));
    if (!renderer.init_programs(gpu_ctx)) { SKIP("the device's shader toolchain (shaderc / dxc) is unavailable"); }
    REQUIRE(renderer.set_frame_graph_toml(graph.c_str()));
    (void)renderer.sync(world); // 0 instances — the probe has no geometry pass, only clear + raster.fullscreen

    auto target = raster.create_color_depth_target(64U, 64U);
    REQUIRE(target != nullptr);
    // the frame has no geometry pass, so the camera matrices are unused by the fullscreen chain; passed for the signature.
    const math::Mat4f     view = math::look_at(math::Vec3f{0.0F, 0.0F, 2.2F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     lightdir{0.0F, 0.0F, 1.0F};
    const gpu::ClearColor clear{0.0F, 0.0F, 0.0F, 1.0F};
    (void)renderer.render(*target, proj * view, lightdir, clear, nullptr);

    // ⛔ DISCRIMINATOR (same as arm a/i): a frame rejected at record() leaves a black target a pixel check cannot tell
    // from a wrong hash. record_ok==1 confirms the empty-world probe RECORDED before we trust its pixels.
    scenerender::SceneRenderer::GpuCullCounts diag{};
    (void)renderer.read_gpu_cull_counts(diag);
    REQUIRE(diag.fill_record_ok == 1U);
    REQUIRE(diag.fill_build_ok == 1U);

    // the numeric hash check: @output.r == the lifted oracle for EVERY pixel (±1 LSB), a majority EXACT, and r==g==b.
    // ⛔ WHAT THE DISAGREEMENTS ARE (diagnosed by dumping every mismatch, 2026-09-06): ~3% of pixels (3971/4096 exact,
    // IDENTICAL on Vulkan and DX12) read back 1 LSB below the ideal round — EVERY one has noise·255 landing just above a
    // .5 boundary (168.52, 243.54, 112.53, …) and the hardware converts it DOWN. The noise VALUE is bit-exact (the hash
    // is correct); the store's f32→UNORM8 conversion is permitted a tolerance of 0.6 ULP relative to the ideal product
    // (D3D11.3 functional spec §3.2.3.1; the Vulkan conversion is likewise not bit-mandated), so a conforming byte is
    // within 0.5 (half-quantum) + 0.6 (tolerance) = 1.1 of the ideal REAL product. The gate is thus characterized by the
    // SPEC constant, not a tuned fraction band (which would false-fail a converter biased the other way):
    int exact          = 0;
    int within1        = 0; // |readback - ideal_round| <= 1: adjacency to the ideal byte (the quantum)
    int splat_coherent = 0; // r==g==b: the noise is a scalar splatted to all channels
    int out_of_spec    = 0; // |noise·255 - readback| >= 1.1: beyond the 0.6-ULP converter tolerance — a genuine error
    for (u32 y = 0U; y < 64U; ++y)
    {
        for (u32 x = 0U; x < 64U; ++x)
        {
            const Rgb    px       = pixel_rgb(*target, x, y);
            const float  noise    = crd::tests::noise_from_hash_f32(crd::tests::ref_hash(x, y)); // FLOAT: == the shader's noise
            const double noise255 = static_cast<double>(noise) * 255.0; // the IDEAL real product the converter approximates
            const int    expected = static_cast<int>(std::lround(noise255));
            const int    d        = px.r >= expected ? px.r - expected : expected - px.r;
            if (px.r == expected) { ++exact; }
            if (d <= 1) { ++within1; }
            if (px.r == px.g && px.g == px.b) { ++splat_coherent; }
            // ⛔ a readback beyond 0.5 + 0.6 ULP of the ideal real product is OUT OF SPEC — a genuine error: a wrong hash
            // (arithmetic shift / swapped multiplier / mis-wired x/y) reads ~random bytes far from noise·255, and a
            // systematic ±1 offset pushes ~40% of pixels past 1.1. A conforming converter tie (here ≤0.52) never fires.
            if (std::fabs(noise255 - static_cast<double>(px.r)) >= 1.1) { ++out_of_spec; }
        }
    }
    UNSCOPED_INFO("31b-4-b-ii-2: exact=" << exact << "/4096  within1=" << within1 << "/4096  out_of_spec=" << out_of_spec
                                         << "  splat=" << splat_coherent << "/4096");
    // ⛔ within1==4096: EVERY pixel is adjacent to the ideal byte, so the GPU computed the authored hash (a wrong hash
    // gives ~random bytes, within ±1 of the oracle at only ~3/256 of pixels ⇒ within1 ≈ 48).
    CHECK(within1 == 4096);
    // ⛔ and every byte is within the 0.6-ULP UNORM8 converter tolerance of the ideal real value — NOT a hash error and
    // NOT a systematic ±1 offset (which within1==4096 alone would miss). The spec constant 1.1, not a tuned band.
    CHECK(out_of_spec == 0);
    // a SANE converter also matches EXACTLY on the vast majority; the tolerance gate deliberately admits a pathological
    // (but conforming) converter, so this companion floor flags a driver regression the 1.1 band would let through.
    CHECK(exact >= 3072);
    // both backends matching the SAME (backend-agnostic) oracle transitively proves cross-backend hash agreement.
    CHECK(splat_coherent == 4096);
}

// ── ⭐ THE SHARED HARD-EDGE GEOMETRY FIXTURE (CEIR-31b-4-b-iii). Cook a flat `base`-grey quad + its OpenPBR material through
// the real resource pipeline, spawn it covering the viewport's TOP half (scale 2,2,1 · translate +1.8 Y ⇒ its bottom edge
// sits at world y=0 = screen centre), and init the renderer + programs. A geometry-pass hard edge in scene_color is the
// shape the ui chain's orientation (b-iii-1) AND blur-numeric (b-iii-2) arms both read — a fullscreen-written backdrop would
// be ui-convention and dodge the flip question. Writes two .crdr packs the caller removes. Returns false ONLY when the
// device toolchain is unavailable (the caller SKIPs); every CPU-side step is REQUIREd.
[[nodiscard]] bool build_hard_edge_scene(gpu::IRasterContext& raster, gpu::IGpuContext& gpu_ctx, float base,
                                         resources::ResourceManager& rm, scene::World& world,
                                         scenerender::SceneRenderer& renderer, const platform::fs::Path& mesh_path,
                                         const platform::fs::Path& mtl_path)
{
    memory::TlsfAllocator       cook(4U << 20U);
    const resources::ResourceId mesh_id = resources::ResourceId::mint_random();
    const resources::ResourceId mtl_id  = resources::ResourceId::mint_random();
    crd::tests::write_one_pack(&galloc(), mesh_path, mesh_id, resources::kFourCC_MESH,
                               crd::tests::build_quad_mesh_crdr(&galloc(), mesh_id), "quad");
    resources::PbrmParams params;
    params.base_color[0] = base;
    params.base_color[1] = base;
    params.base_color[2] = base;
    params.base_alpha    = 1.0F;
    resources::PbrmTextures textures; // no texture slots — the surface's base colour comes from params
    auto                    mtl_bytes = resources::pbrm_build(params, textures, mtl_id, &cook);
    crd::tests::write_one_pack(&galloc(), mtl_path, mtl_id, resources::kFourCC_PBRM, mtl_bytes, "mtl");

    resources::register_mesh_loader(&rm, nullptr);
    resources::register_openpbr_material_loader(&rm);
    REQUIRE(rm.mount_manifest(mesh_path.generic()).is_valid());
    REQUIRE(rm.mount_manifest(mtl_path.generic()).is_valid());

    world.register_component<scene::Transform>(scene::transform_serialize_trait(), scene::SpatialBVH{});
    scene::register_render_components(world);
    const scene::EntityId ent = world.spawn();
    scene::Transform      t;
    t.translation = math::from_raw_vec<units::dim::Length>(math::Vec3f{0.0F, 1.8F, 0.0F});
    t.world       = math::from_trs(math::Vec3f{0.0F, 1.8F, 0.0F}, math::Quatf::identity(), math::Vec3f{2.0F, 2.0F, 1.0F});
    world.add_component(ent, t);
    world.add_component(ent, scene::MeshRenderer{mesh_id, mtl_id});

    REQUIRE(renderer.init(raster, rm));
    return renderer.init_programs(gpu_ctx);
}

// Render the shipped frosted-glass frame into `target` with this arm's patches: a same-width `clear_str` scene clear; the
// mask rect FULL (coverage 1 ⇒ composite outputs the effect) or zero-area (⇒ outputs scene_color directly, ONE hop);
// optionally tint=1/amp=0 (⇒ the effect reproduces the backdrop — the far==tint numeric arm); optionally a fixed blur step.
// ⛔ THE STEP (CEIR-31b-4-b-iv-g-2): the four blur passes DERIVE spec_2 from their read extent, so the SHIPPED frame is
// resolution-correct at every size (blur_step_repl == nullptr ⇒ leave it derived). An arm that wants a FIXED step passes a
// `spec_2 = X` literal that replaces the 3-line derive block (variable-width patch_replace): "spec_2 = 0.031250000" (1/32 =
// 1 texel/tap at 64²) for the numeric/width arms, "spec_2 = 0.007812500" (1/128, near-no-op) for the width ratio, iter-2 via
// blur_step_iter2, or a per-AXIS (blur_step_h / blur_step_v) pair for the g-2 axis gate. Returns false on any patch/set/record miss.
[[nodiscard]] bool render_frosted_frame(scenerender::SceneRenderer& renderer, scene::World& world,
                                        gpu::IRasterTarget& target, const math::Mat4f& vp, const math::Vec3f& lightdir,
                                        const char* clear_str, const gpu::ClearColor& clear, bool mask_full,
                                        bool tint_one_amp_zero, const char* blur_step_repl, const char* blur_step_iter2 = nullptr,
                                        const char* blur_step_h = nullptr, const char* blur_step_v = nullptr)
{
    containers::String graph(&galloc());
    if (!read_shipped_asset("frame/ui_frosted_glass.frame.toml", graph)) { return false; }
    if (!patch_same_width(graph, "0.09, 0.10, 0.13", clear_str)) { return false; }        // a same-width scene clear
    if (!patch_same_width(graph, "spec_0 = 0.25", "spec_0 = 0.00")) { return false; }     // mask x0 = 0
    if (!patch_same_width(graph, "spec_1 = 0.25", "spec_1 = 0.00")) { return false; }     // mask y0 = 0
    if (!patch_same_width(graph, "spec_2 = 0.75", mask_full ? "spec_2 = 1.00" : "spec_2 = 0.00")) { return false; }
    if (!patch_same_width(graph, "spec_3 = 0.75", mask_full ? "spec_3 = 1.00" : "spec_3 = 0.00")) { return false; }
    if (tint_one_amp_zero)
    {
        if (!patch_same_width(graph, "spec_0 = 0.90", "spec_0 = 1.00")) { return false; } // tint.r = 1 (reproduce backdrop)
        if (!patch_same_width(graph, "spec_1 = 0.90", "spec_1 = 1.00")) { return false; } // tint.g = 1
        if (!patch_same_width(graph, "spec_4 = 0.03", "spec_4 = 0.00")) { return false; } // amp = 0 (no noise)
    }
    // CEIR-31b-4-b-iv-g-2: the four blur passes are EXTENT-DERIVED now, so an arm that wants a FIXED step swaps each 3-line
    // `derive_spec_2_{read,axis,op}` block for a one-line `spec_2 = X` literal (variable-width patch_replace). nullptr for a
    // pass ⇒ leave it DERIVED (the shipped, resolution-correct step). TWO override axes: (blur_step_repl, blur_step_iter2)
    // map per ITERATION (h1,v1 / h2,v2 — the ping-pong arm's step1/step2); (blur_step_h, blur_step_v) map per AXIS (h1,h2 /
    // v1,v2 — the g-2 axis gate), taking PRECEDENCE when set. Needles are the exact authored blocks; blur_v1/v2 share the
    // blur_a/axis-y block, so the FILE-ORDER h1,v1,h2,v2 patching resolves the pair (v1's replace first, v2's second).
    {
        const char* const s2   = blur_step_iter2 != nullptr ? blur_step_iter2 : blur_step_repl;
        const char* const r_h1 = blur_step_h != nullptr ? blur_step_h : blur_step_repl;
        const char* const r_v1 = blur_step_v != nullptr ? blur_step_v : blur_step_repl;
        const char* const r_h2 = blur_step_h != nullptr ? blur_step_h : s2;
        const char* const r_v2 = blur_step_v != nullptr ? blur_step_v : s2;
        const char* const derive_h1 = "derive_spec_2_read = \"blur_src\"\nderive_spec_2_axis = \"x\"\nderive_spec_2_op = \"inv\"";
        const char* const derive_v1 = "derive_spec_2_read = \"blur_a\"\nderive_spec_2_axis = \"y\"\nderive_spec_2_op = \"inv\"";
        const char* const derive_h2 = "derive_spec_2_read = \"blur_b\"\nderive_spec_2_axis = \"x\"\nderive_spec_2_op = \"inv\"";
        const char* const derive_v2 = "derive_spec_2_read = \"blur_a\"\nderive_spec_2_axis = \"y\"\nderive_spec_2_op = \"inv\"";
        if (r_h1 != nullptr && !patch_replace(graph, derive_h1, r_h1)) { return false; } // blur_h1 (reads blur_src, axis x)
        if (r_v1 != nullptr && !patch_replace(graph, derive_v1, r_v1)) { return false; } // blur_v1 (reads blur_a, axis y — 1st)
        if (r_h2 != nullptr && !patch_replace(graph, derive_h2, r_h2)) { return false; } // blur_h2 (reads blur_b, axis x)
        if (r_v2 != nullptr && !patch_replace(graph, derive_v2, r_v2)) { return false; } // blur_v2 (reads blur_a, axis y — 2nd)
    }
    if (!renderer.set_frame_graph_toml(graph.c_str())) { return false; }
    (void)renderer.sync(world);
    (void)renderer.render(target, vp, lightdir, clear, nullptr);
    scenerender::SceneRenderer::GpuCullCounts diag{};
    (void)renderer.read_gpu_cull_counts(diag);
    return diag.fill_record_ok == 1U && diag.fill_build_ok == 1U;
}

// ── ⭐⭐ CEIR-31b-4-b-iii-1: the ui chain PRESERVES the scene's orientation (the Y-flip ruling). A geometry hard-edge
// scene — a bright quad over the viewport's TOP half, a dark clear below — writes scene_color through raster.geometry,
// the REAL shape the flip question is about (a fullscreen-written backdrop would be ui-convention and dodge it). The
// composite reading scene_color DIRECTLY (mask coverage 0 ⇒ mix(scene,effect,0)=scene, ONE ui-VS hop) must land the
// bright half on the SAME rows as the FULL effect chain (mask coverage 1 ⇒ SEVEN hops: copy→fetch→blur×4→tint→composite).
// sign(top−bottom) agreeing between the two IS "scene-orientation == effect-orientation == @output-orientation" verbatim.
// ⭐ ABSOLUTE orientation inherits tonemap: ensure_ui_program cooks vertex/post_fullscreen.crdv with the IDENTICAL no-flip
// call as ensure_post_program_named (tonemap), shipped visually-correct reading the same geometry-output class — so NO
// !ndc_y_points_down() is needed (post_fullscreen.crdv derives uv from the clip position, so a flip WOULD mirror uv; the
// no-flip default is the correct, tonemap-inherited choice). Both backends — the flip arg is backend-conditional.
void run_hard_edge_orientation_arm(gpu::IRasterContext& raster, gpu::IGpuContext& gpu_ctx)
{
    if (!raster.supports_bindless()) { SKIP("device does not support bindless texture arrays (the composite read heap)"); }

    const platform::fs::Path   mesh_path(containers::StringView("sr_b3iii_mesh.crdr"));
    const platform::fs::Path   mtl_path(containers::StringView("sr_b3iii_mtl.crdr"));
    resources::ResourceManager rm(&galloc());
    scene::World               world{&galloc()};
    scenerender::SceneRenderer renderer(&galloc());
    if (!build_hard_edge_scene(raster, gpu_ctx, 0.8F, rm, world, renderer, mesh_path, mtl_path))
    {
        SKIP("the device's shader toolchain (shaderc / dxc) is unavailable");
    }

    auto target = raster.create_color_depth_target(64U, 64U);
    REQUIRE(target != nullptr);
    const math::Mat4f     view = math::look_at(math::Vec3f{0.0F, 0.0F, 2.2F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     lightdir{0.0F, 0.0F, 1.0F};
    const gpu::ClearColor clear{0.05F, 0.05F, 0.05F, 1.0F};

    // eff = the 7-hop pure effect (mask FULL); scn = the 1-hop scene read (mask empty). Orientation is blur-invariant, so
    // the DERIVED step (1/32 at 64² — a REAL blur now, no longer the baked no-op) is fine here: rows 8/56 are plateau, far
    // outside the ±8px blur support around the row-32 edge, so the top-vs-bottom sign the arm reads is unaffected.
    REQUIRE(render_frosted_frame(renderer, world, *target, proj * view, lightdir, "0.05, 0.05, 0.05", clear, true, false,
                                 nullptr));
    const int eff = pixel_rgb(*target, 32U, 8U).r - pixel_rgb(*target, 32U, 56U).r; // pure EFFECT top-bottom (7 ui-VS hops)
    REQUIRE(render_frosted_frame(renderer, world, *target, proj * view, lightdir, "0.05, 0.05, 0.05", clear, false, false,
                                 nullptr));
    const int scn = pixel_rgb(*target, 32U, 8U).r - pixel_rgb(*target, 32U, 56U).r; // pure SCENE top-bottom (1 ui-VS hop)
    UNSCOPED_INFO("31b-4-b-iii-1: effect top-bottom=" << eff << "  scene top-bottom=" << scn);
    // ⛔ a REAL edge, not a flat frame: the quad half must be clearly distinct from the dark clear in BOTH reads (else the
    // sign comparison is vacuous — a quad that never rendered leaves scene_color uniform and both signs ~0).
    CHECK((eff > 20 || eff < -20));
    // ⛔ ABSOLUTE upright, not merely a real edge: the quad sits at world +Y (scale 2,2,1 · translate +1.8Y ⇒ its bottom
    // edge is at world y=0 = screen centre, covering the TOP half), and look_at(up={0,1,0}) maps world +Y to the TOP rows
    // on BOTH backends (each backend's projection is built for its own ndc_y convention so the geometry pass renders
    // upright). So the 1-hop scene read MUST have top(row 8) brighter than bottom(row 56) ⇒ scn>0 is a PREDICTED value,
    // not just a sign to compare. scn<0 on either backend would mean look_at's up mapped to screen-bottom for the read —
    // the flip this arm rules out. Combined with |eff|>20 and (eff>0)==(scn>0) below, this forces eff>0 too ⇒ upright chain.
    CHECK(scn > 20);
    // ⛔ THE RULING: the 7-hop effect chain and the 1-hop scene read put the bright half on the SAME side ⇒ the ui chain
    // is orientation-consistent (scene==effect==@output). A pass that flipped differently would invert one sign vs the other.
    CHECK((eff > 0) == (scn > 0));

    (void)platform::fs::remove_file(mesh_path);
    (void)platform::fs::remove_file(mtl_path);
}

// ── ⭐⭐ CEIR-31b-4-b-iii-2: the ui blur is a REAL, well-behaved low-pass — arms (h) far==scene + (b) monotone, on the SAME
// geometry hard-edge scene now that orientation is ruled (b-iii-1). With tint=1/amp=0 the effect reproduces the tinted
// backdrop, so FAR from the blurred edge (uniform region, Sum(weights)~1 => blur is a no-op) the @output == the scene's OWN
// value — captured through the composite's 1-hop scene read, the geometry pass's ACTUAL output (not a hand-computed BRDF).
// ACROSS the edge the column is a monotone ramp strictly bounded by [clear, quad] — a Gaussian (and the half-res linear
// resample) never rings. ⛔ the real 1/32 step (not the baked 1/512, sub-texel at 64x64) is what makes the blur act.
void run_hard_edge_numeric_arm(gpu::IRasterContext& raster, gpu::IGpuContext& gpu_ctx)
{
    if (!raster.supports_bindless()) { SKIP("device does not support bindless texture arrays (the composite read heap)"); }

    const platform::fs::Path   mesh_path(containers::StringView("sr_b3iii2_mesh.crdr"));
    const platform::fs::Path   mtl_path(containers::StringView("sr_b3iii2_mtl.crdr"));
    resources::ResourceManager rm(&galloc());
    scene::World               world{&galloc()};
    scenerender::SceneRenderer renderer(&galloc());
    if (!build_hard_edge_scene(raster, gpu_ctx, 0.8F, rm, world, renderer, mesh_path, mtl_path))
    {
        SKIP("the device's shader toolchain (shaderc / dxc) is unavailable");
    }

    auto target = raster.create_color_depth_target(64U, 64U);
    REQUIRE(target != nullptr);
    const math::Mat4f     view = math::look_at(math::Vec3f{0.0F, 0.0F, 2.2F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     lightdir{0.0F, 0.0F, 1.0F};
    const gpu::ClearColor clear{0.30F, 0.30F, 0.30F, 1.0F};

    // the ORACLE for (h)+(b): the geometry pass's OWN PER-ROW output via the composite's 1-hop scene read (mask empty),
    // captured as a COLUMN before the effect overwrites the target. The quad is NOT flat — it carries a mild view-dependent
    // (Fresnel) gradient — so a single reference is wrong; the effect must reproduce the column ROW-BY-ROW where the blur
    // is a no-op, and stay inside the scene's LOCAL envelope where it acts.
    REQUIRE(render_frosted_frame(renderer, world, *target, proj * view, lightdir, "0.30, 0.30, 0.30", clear, false, false,
                                 nullptr));
    int scene_col[64];
    for (u32 y = 0U; y < 64U; ++y) { scene_col[y] = pixel_rgb(*target, 32U, y).r; }
    // ⛔ NO TONEMAP in this frame ⇒ scene_color goes straight to UNORM8. A SATURATED quad (255) would make (h) vacuous
    // (both paths clamp), and the two sides must be well separated or (b) is empty (the UNORM8-store gate doctrine).
    REQUIRE(scene_col[16] <= 250);               // quad plateau (row 16) unsaturated
    REQUIRE(scene_col[16] - scene_col[48] > 20); // a REAL edge: quad (row 16) clearly brighter than clear (row 48)

    // the EFFECT: mask FULL ⇒ pure effect; tint=1/amp=0 ⇒ tint x backdrop, no noise; the REAL 1/32 blur step.
    REQUIRE(render_frosted_frame(renderer, world, *target, proj * view, lightdir, "0.30, 0.30, 0.30", clear, true, true,
                                 "spec_2 = 0.031250000"));
    int eff_col[64];
    for (u32 y = 0U; y < 64U; ++y) { eff_col[y] = pixel_rgb(*target, 32U, y).r; }

    // (h) FAR from the blurred edge (rows 0-20 = quad, 44-63 = clear, outside the ±8 px ramp at row ~32) AND at the BORDERS
    // (rows 0 and 63) the effect is a FAITHFUL PER-ROW reproduction of the scene: tint=1 ⇒ effect[y] == scene[y] within the
    // UNORM8 store band (<=1 LSB), the Fresnel gradient reproduced and all. ⛔ the border tooth (rows 0/63) is exactly what
    // the clamp-address fix rescued — before it the Repeat-default sampler WRAPPED opposite-edge content across the screen
    // edge (the border-bleed defect this arm found + solved). [[feedback_unorm8_store_tolerance_gate_with_spec_band_not_exact]].
    for (u32 y = 0U; y <= 20U; ++y)
    {
        const int d = eff_col[y] - scene_col[y];
        CHECK(d <= 1);
        CHECK(d >= -1);
    }
    for (u32 y = 44U; y <= 63U; ++y)
    {
        const int d = eff_col[y] - scene_col[y];
        CHECK(d <= 1);
        CHECK(d >= -1);
    }

    // (b) the blur is a LOW-PASS. THE anti-ringing tooth on a NON-FLAT input: every row stays inside the LOCAL [min,max] of
    // the scene over the blur's ±8 px support (a flat [clear,quad] range would be wrong — the quad has a gradient, and a
    // scene-derived envelope catches an overshoot a flat range would hide). ±1 LSB slack for quantisation.
    for (u32 y = 0U; y < 64U; ++y)
    {
        const u32 y0 = y >= 8U ? y - 8U : 0U;
        const u32 y1 = y + 8U <= 63U ? y + 8U : 63U;
        int       lo = 255;
        int       hi = 0;
        for (u32 k = y0; k <= y1; ++k)
        {
            lo = scene_col[k] < lo ? scene_col[k] : lo;
            hi = scene_col[k] > hi ? scene_col[k] : hi;
        }
        CHECK(eff_col[y] >= lo - 1); // no undershoot below the local scene minimum
        CHECK(eff_col[y] <= hi + 1); // no ringing above the local scene maximum
    }
    // MONOTONE across the ramp (rows 24-40, where the step dominates the ~0.2-LSB/row quad gradient): non-increasing.
    for (u32 y = 25U; y <= 40U; ++y) { CHECK(eff_col[y] <= eff_col[y - 1] + 1); }
    // the blur actually MIXED the two sides at the edge (strictly between the plateaus, well clear of both) — a real ramp.
    CHECK(eff_col[32] > scene_col[48] + 10); // above the clear plateau
    CHECK(eff_col[32] < scene_col[16] - 10); // below the quad plateau
    CHECK(eff_col[16] - eff_col[48] > 20);   // a real end-to-end drop (quad plateau → clear plateau)

    (void)platform::fs::remove_file(mesh_path);
    (void)platform::fs::remove_file(mtl_path);
}

// ── ⭐⭐ CEIR-31b-4-b-iii-3: the blur's spatial extent SCALES with its step spec-const (arm (c) width∝radius). At 64² the
// baked kernel radius (R=4, 9 taps) is fixed, so the STEP is the clean knob — the same tint=1/amp=0 hard-edge effect at a
// WIDE step (1/32, 1 texel/tap) vs a NARROW step (1/128, ¼ texel/tap, near-no-op) must give a WIDER ramp. Proves the kernel
// READS spec_2: a kernel that baked a fixed tap spacing would give IDENTICAL widths. width = rows where the effect deviates
// from the scene by >1 LSB (the ramp — where the blur acts; the plateaus match per-row, per b-iii-2).
void run_width_scaling_arm(gpu::IRasterContext& raster, gpu::IGpuContext& gpu_ctx)
{
    if (!raster.supports_bindless()) { SKIP("device does not support bindless texture arrays (the composite read heap)"); }

    const platform::fs::Path   mesh_path(containers::StringView("sr_b3iii3_mesh.crdr"));
    const platform::fs::Path   mtl_path(containers::StringView("sr_b3iii3_mtl.crdr"));
    resources::ResourceManager rm(&galloc());
    scene::World               world{&galloc()};
    scenerender::SceneRenderer renderer(&galloc());
    if (!build_hard_edge_scene(raster, gpu_ctx, 0.8F, rm, world, renderer, mesh_path, mtl_path))
    {
        SKIP("the device's shader toolchain (shaderc / dxc) is unavailable");
    }

    auto target = raster.create_color_depth_target(64U, 64U);
    REQUIRE(target != nullptr);
    const math::Mat4f     view = math::look_at(math::Vec3f{0.0F, 0.0F, 2.2F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     lightdir{0.0F, 0.0F, 1.0F};
    const gpu::ClearColor clear{0.30F, 0.30F, 0.30F, 1.0F};

    // the scene column is the per-row reference (mask empty ⇒ 1-hop scene passthrough).
    REQUIRE(render_frosted_frame(renderer, world, *target, proj * view, lightdir, "0.30, 0.30, 0.30", clear, false, false,
                                 nullptr));
    int scene_col[64];
    for (u32 y = 0U; y < 64U; ++y) { scene_col[y] = pixel_rgb(*target, 32U, y).r; }
    REQUIRE(scene_col[16] - scene_col[48] > 20); // a real edge, else width is meaningless

    // ramp WIDTH = rows whose effect deviates from the scene by >1 LSB (the blur-affected rows) at a given step. ⛔ count ONLY
    // the edge neighbourhood [12,52] (±20 around the row-32 edge): the plateaus there are clean (|effect-scene|<=1, per
    // b-iii-2) so they contribute 0, and the borders (rows 0-11 / 53-63) are EXCLUDED so a resample border-smear cannot leak
    // into width_narrow (a border regression fails the b-iii-2 border tooth INDEPENDENTLY, not this width delta).
    const auto ramp_width = [&](const char* step_repl) -> int {
        REQUIRE(render_frosted_frame(renderer, world, *target, proj * view, lightdir, "0.30, 0.30, 0.30", clear, true, true,
                                     step_repl));
        int w = 0;
        for (u32 y = 12U; y <= 52U; ++y)
        {
            const int d = pixel_rgb(*target, 32U, y).r - scene_col[y];
            if (d > 1 || d < -1) { ++w; }
        }
        return w;
    };

    const int width_wide   = ramp_width("spec_2 = 0.031250000"); // 1/32  = 1   texel/tap → a real ±8px blur; MEASURED width 16
    const int width_narrow = ramp_width("spec_2 = 0.007812500"); // 1/128 = ¼ texel/tap → near-no-op; MEASURED width 6 = the
                                                                 // RESAMPLE FLOOR (a hard edge never round-trips half-res
                                                                 // perfectly). Both values IDENTICAL on Vulkan AND DX12.
    // ⛔ width SCALES with step (measured 16 vs 6, a +10 blur contribution over the resample floor) ⇒ the blur kernel READS
    // spec_2: a baked-spacing kernel would give width_wide == width_narrow (difference 0). The 4× step ratio (1/32 : 1/128)
    // is the census's "radius 4 vs 1" mapped onto the STEP knob (the kernel radius R=4 is baked in the 9 taps).
    CHECK(width_wide > width_narrow + 8); // the blur contribution (measured 10) is substantial, far above a baked kernel's 0
    CHECK(width_narrow >= 2);             // the resample floor exists (a hard edge never round-trips half-res perfectly)
    CHECK(width_narrow < width_wide);     // survives a loosened +8 margin

    (void)platform::fs::remove_file(mesh_path);
    (void)platform::fs::remove_file(mtl_path);
}

// ── ⭐⭐ CEIR-31b-4-b-iv (f): the 2-iteration ping-pong CHAINS — iter-2 CONSUMES iter-1's output (the `reads=` wiring is
// honored; a stale or no-op iter-2 gives identical widths). The blur runs blur_src→[h1]→blur_a→[v1]→blur_b→[h2]→blur_a→
// [v2]→blur_b, reusing blur_a/blur_b. ⛔ THIS IS NOT AN ALIASER-WAR GATE: that reuse is WAR-ordered by the DATA DEPENDENCY
// (h2 reads blur_b ← v1, so h2 cannot run before v1's read of blur_a completes), not by the transient aliaser — the
// aliaser-WAR scar ([[feedback_frame_graph_war_needs_resource_lifetime]]) is the NO-data-edge case (a transient freed +
// re-allocated to an unrelated pass) and is out of this gate's discrimination. PROOF: the 2-iter blur is WIDER than a 1-iter
// blur (two σ=R/3 Gaussians convolve to σ√2) — measured by DISABLING iter-2 (h2/v2 at a 0-step = identity) vs the full
// chain, the same [12,52] ramp-width metric as b-iii-3.
void run_ping_pong_arm(gpu::IRasterContext& raster, gpu::IGpuContext& gpu_ctx)
{
    if (!raster.supports_bindless()) { SKIP("device does not support bindless texture arrays (the composite read heap)"); }

    const platform::fs::Path   mesh_path(containers::StringView("sr_b4f_mesh.crdr"));
    const platform::fs::Path   mtl_path(containers::StringView("sr_b4f_mtl.crdr"));
    resources::ResourceManager rm(&galloc());
    scene::World               world{&galloc()};
    scenerender::SceneRenderer renderer(&galloc());
    if (!build_hard_edge_scene(raster, gpu_ctx, 0.8F, rm, world, renderer, mesh_path, mtl_path))
    {
        SKIP("the device's shader toolchain (shaderc / dxc) is unavailable");
    }

    auto target = raster.create_color_depth_target(64U, 64U);
    REQUIRE(target != nullptr);
    const math::Mat4f     view = math::look_at(math::Vec3f{0.0F, 0.0F, 2.2F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     lightdir{0.0F, 0.0F, 1.0F};
    const gpu::ClearColor clear{0.30F, 0.30F, 0.30F, 1.0F};

    REQUIRE(render_frosted_frame(renderer, world, *target, proj * view, lightdir, "0.30, 0.30, 0.30", clear, false, false,
                                 nullptr));
    int scene_col[64];
    for (u32 y = 0U; y < 64U; ++y) { scene_col[y] = pixel_rgb(*target, 32U, y).r; }
    REQUIRE(scene_col[16] - scene_col[48] > 20); // a real edge

    // ramp width over the edge neighbourhood [12,52] (as b-iii-3), given per-iteration steps (step1 = h1/v1, step2 = h2/v2).
    const auto ramp_width = [&](const char* step1, const char* step2) -> int {
        REQUIRE(render_frosted_frame(renderer, world, *target, proj * view, lightdir, "0.30, 0.30, 0.30", clear, true, true,
                                     step1, step2));
        int w = 0;
        for (u32 y = 12U; y <= 52U; ++y)
        {
            const int d = pixel_rgb(*target, 32U, y).r - scene_col[y];
            if (d > 1 || d < -1) { ++w; }
        }
        return w;
    };

    // both iterations at the real 1/32 step vs iter-2 DISABLED (h2/v2 at a 0-step = identity ⇒ a single H+V pass).
    const int width_2iter = ramp_width("spec_2 = 0.031250000", nullptr);                // full chain — MEASURED width 16
    const int width_1iter = ramp_width("spec_2 = 0.031250000", "spec_2 = 0.000000000"); // iter-2 identity — MEASURED width 12
    // ⛔ the 2nd iteration CONTRIBUTES (measured +4) ⇒ the ping-pong CHAINS — iter-2 CONSUMES iter-1's output through the
    // reused blur_a/blur_b (the `reads=` wiring is honored). A no-op / stale iter-2 would give width_2iter == width_1iter
    // (difference 0), far below the +2 gate. Both values IDENTICAL on Vulkan AND DX12. Why +4: each width includes the ~6-row
    // resample floor from b-iii-3 (a half-res tap can't resolve sub-2-row detail); the BLUR contribution over that floor is
    // 16−6 = 10 (2-iter) vs 12−6 = 6 (1-iter), i.e. two σ=R/3 Gaussians convolve to σ√2 (10/6 ≈ 1.67, above √2 — the floor is
    // not purely additive). Those over-floor numbers (10 vs 6) are the b-iv-g extent-derivation targets.
    CHECK(width_2iter > width_1iter + 2);
    CHECK(width_1iter >= 2);           // a real single-pass blur exists (not a degenerate 0-width)
    CHECK(width_1iter < width_2iter);  // iter-2 never narrows

    (void)platform::fs::remove_file(mesh_path);
    (void)platform::fs::remove_file(mtl_path);
}

// ── ⭐⭐ CEIR-31b-4-b-iv (g-1): the FALSIFIER — a baked step LITERAL does not travel across resolution ⇒ g-2 must DERIVE it.
// BEFORE g-2 the shipped frame BAKED spec_2 = 1/512 (0.001953125) sized for PROOF_RES 1024: at 1024² the half-res transient
// is 512 texels so 1/512 UV = 1 texel/tap (the correct blur). At THIS arm's 128² the half-res transient is 64 texels, so that
// same baked 1/512 is 1/8 texel/tap = SUB-TEXEL ⇒ the 9-tap Gaussian collapses to a no-op ⇒ the ramp falls to the RESAMPLE
// FLOOR: a baked literal would render an UN-BLURRED frosted panel at every resolution but 1024 (what g-2 fixes by DERIVING
// the step; this arm now patches that baked literal in explicitly as `width_literal` to keep the falsifier). The step 1/64 (= 1
// texel/tap on the 64-texel half-res) restores a real wide blur — and because "1 texel/tap" is the SAME 4-half-res-texel
// radius upsampled by the SAME 2× at BOTH resolutions, its full-res width matches the 64² arm's 16 (resolution-INVARIANT in
// full-res rows — the property a fixed-UV literal CANNOT have, and exactly what g-2's derivation restores).
// ⛔ THE PIN (width_shipped == width_correct), FLIPPED at g-2: the shipped frame now DERIVES spec_2 from the read extent, so at
// 128² it resolves to 1/64 (the CORRECT step) and width_shipped == width_correct == 16. Before g-2 the shipped frame baked
// 1/512, this pin read width_shipped == width_literal == 6, and flipping it once the derivation landed is the planted red→green
// that PROVES the step now travels across resolution — the tree stays green at every tick boundary. The explicit-step arms keep
// patching through the variable-width patch_replace (the 3-line derive block ⇄ a `spec_2 = X` literal, in render_frosted_frame).
void run_step_travels_arm(gpu::IRasterContext& raster, gpu::IGpuContext& gpu_ctx)
{
    if (!raster.supports_bindless()) { SKIP("device does not support bindless texture arrays (the composite read heap)"); }

    const platform::fs::Path   mesh_path(containers::StringView("sr_b4g1_mesh.crdr"));
    const platform::fs::Path   mtl_path(containers::StringView("sr_b4g1_mtl.crdr"));
    resources::ResourceManager rm(&galloc());
    scene::World               world{&galloc()};
    scenerender::SceneRenderer renderer(&galloc());
    if (!build_hard_edge_scene(raster, gpu_ctx, 0.8F, rm, world, renderer, mesh_path, mtl_path))
    {
        SKIP("the device's shader toolchain (shaderc / dxc) is unavailable");
    }

    // ⛔ 128² — a DIFFERENT resolution from every other arm's 64², so the baked 1/512 (sized for 1024) is provably wrong here.
    auto target = raster.create_color_depth_target(128U, 128U);
    REQUIRE(target != nullptr);
    const math::Mat4f     view = math::look_at(math::Vec3f{0.0F, 0.0F, 2.2F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj = math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
    const math::Vec3f     lightdir{0.0F, 0.0F, 1.0F};
    const gpu::ClearColor clear{0.30F, 0.30F, 0.30F, 1.0F};

    // per-row scene reference at 128² (mask empty ⇒ 1-hop scene passthrough); the edge sits at the vertical centre = row 64.
    REQUIRE(render_frosted_frame(renderer, world, *target, proj * view, lightdir, "0.30, 0.30, 0.30", clear, false, false,
                                 nullptr));
    int scene_col[128];
    for (u32 y = 0U; y < 128U; ++y) { scene_col[y] = pixel_rgb(*target, 64U, y).r; }
    REQUIRE(scene_col[32] - scene_col[96] > 20); // a real edge at 128² (top plateau row 32, bottom plateau row 96)

    // ramp WIDTH over the edge neighbourhood [24,104] (±40 around the row-64 edge; borders excluded, per b-iii-3 [12,52] ×2).
    const auto ramp_width = [&](const char* step_repl) -> int {
        REQUIRE(render_frosted_frame(renderer, world, *target, proj * view, lightdir, "0.30, 0.30, 0.30", clear, true, true,
                                     step_repl));
        int w = 0;
        for (u32 y = 24U; y <= 104U; ++y)
        {
            const int d = pixel_rgb(*target, 64U, y).r - scene_col[y];
            if (d > 1 || d < -1) { ++w; }
        }
        return w;
    };

    const int width_shipped = ramp_width(nullptr);                // the shipped step DERIVED (128² half-res 64 → 1/64) → real blur
    const int width_literal = ramp_width("spec_2 = 0.001953125"); // the OLD baked 1/512 literal patched in explicitly → collapses
    const int width_correct = ramp_width("spec_2 = 0.015625000"); // 1/64 = 1 texel/tap on the 64-texel half-res → real blur
    // MEASURED (identical Vulkan & DX12, bit-deterministic): shipped 16, literal 6, correct 16. ⭐ correct == 16 is the SAME
    // full-res width as the 64² arm's 1/32 (b-iii-3 width_wide = 16) — resolution-INVARIANT in full-res rows, because "1
    // texel/tap" is the same 4-half-res-texel radius × the same 2× upsample at both resolutions. The BAKED literal collapses
    // to the 6-row resample floor (== the 64² floor). ⭐ g-2 LANDED: the shipped frame DERIVES the step now, so width_shipped
    // travelled from 6 (the g-1 baked value) to 16 (correct) at 128² — the planted red→green that proves the step adapts.
    CHECK(width_correct > width_literal + 8); // ⛔ the falsifier (measured 16 > 6+8): the OLD baked literal is FAR from correct
    CHECK(width_literal >= 2);                // the 128² resample floor exists (measured 6; a hard edge never round-trips half-res)
    CHECK(width_shipped == width_correct);    // ⛔ THE PIN, FLIPPED at g-2 (measured 16 == 16): the shipped DERIVED step is correct

    (void)platform::fs::remove_file(mesh_path);
    (void)platform::fs::remove_file(mtl_path);
}

// ── ⭐⭐ CEIR-31b-4-b-iv (g-2): the derivation is AXIS-CORRECT on device — a NON-SQUARE 128×64 render where the H step (1/64,
// off the read's X extent) and the V step (1/32, off its Y extent) genuinely DIFFER (a square render, every other arm, cannot
// see the axis). Byte-identity is the gate (fb_hash, the b-ii-1 precedent): 1/64 and 1/32 are EXACT in f32, so the DERIVED
// SpecSet keys the SAME cached specialized program as the explicit-literal SpecSet ⇒ the SAME pixels. So fb_hash(shipped
// DERIVE) == fb_hash(H=1/64 + V=1/32 literals); and the SWAPPED-axis literals (H=1/32 + V=1/64) produce a DIFFERENT hash —
// proving the seat honours the AXIS, not merely "some step". An axis-blind resolver fails the equality; a baked one fails both.
void run_derive_step_arm(gpu::IRasterContext& raster, gpu::IGpuContext& gpu_ctx)
{
    if (!raster.supports_bindless()) { SKIP("device does not support bindless texture arrays (the composite read heap)"); }

    const platform::fs::Path   mesh_path(containers::StringView("sr_b4g2_mesh.crdr"));
    const platform::fs::Path   mtl_path(containers::StringView("sr_b4g2_mtl.crdr"));
    resources::ResourceManager rm(&galloc());
    scene::World               world{&galloc()};
    scenerender::SceneRenderer renderer(&galloc());
    if (!build_hard_edge_scene(raster, gpu_ctx, 0.8F, rm, world, renderer, mesh_path, mtl_path))
    {
        SKIP("the device's shader toolchain (shaderc / dxc) is unavailable");
    }

    // ⛔ NON-SQUARE 128×64: the blur transients (scale 0.5) are 64 (x) × 32 (y), so the H step (1/64) and the V step (1/32)
    // DIFFER — the axis split. aspect 2.0 keeps the hard-edge quad undistorted (both compared renders share it, so the
    // equality is unaffected either way; the horizontal edge makes the V-step change visible for the swapped-axis tooth).
    auto target = raster.create_color_depth_target(128U, 64U);
    REQUIRE(target != nullptr);
    const math::Mat4f     view = math::look_at(math::Vec3f{0.0F, 0.0F, 2.2F}, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    const math::Mat4f     proj = math::perspective_reverse_z(1.0472F, 2.0F, 0.1F); // 128/64 = 2.0 aspect
    const math::Vec3f     lightdir{0.0F, 0.0F, 1.0F};
    const gpu::ClearColor clear{0.30F, 0.30F, 0.30F, 1.0F};

    // shipped = the frame DERIVES the step; correct = the SAME values (H=1/64, V=1/32) as explicit per-axis literals;
    // swapped = the axes exchanged (H=1/32, V=1/64). tint=1/amp=0 removes the noise so the hash is a pure function of the blur.
    REQUIRE(render_frosted_frame(renderer, world, *target, proj * view, lightdir, "0.30, 0.30, 0.30", clear, true, true, nullptr));
    const u64 hash_shipped = fb_hash(*target, 128U, 64U);
    REQUIRE(render_frosted_frame(renderer, world, *target, proj * view, lightdir, "0.30, 0.30, 0.30", clear, true, true,
                                 nullptr, nullptr, "spec_2 = 0.015625000", "spec_2 = 0.031250000")); // H = 1/64, V = 1/32
    const u64 hash_correct = fb_hash(*target, 128U, 64U);
    REQUIRE(render_frosted_frame(renderer, world, *target, proj * view, lightdir, "0.30, 0.30, 0.30", clear, true, true,
                                 nullptr, nullptr, "spec_2 = 0.031250000", "spec_2 = 0.015625000")); // H = 1/32, V = 1/64 (SWAPPED)
    const u64 hash_swapped = fb_hash(*target, 128U, 64U);
    UNSCOPED_INFO("31b-4-b-iv-g-2: shipped=" << hash_shipped << " correct=" << hash_correct << " swapped=" << hash_swapped);
    CHECK(hash_shipped == hash_correct); // ⛔ the DERIVED step == the explicit H=1/64,V=1/32 literals, BYTE-IDENTICAL framebuffer
    CHECK(hash_shipped != hash_swapped); // ⛔ exchanging the axes CHANGES the image ⇒ the seat honours the axis, not just magnitude

    (void)platform::fs::remove_file(mesh_path);
    (void)platform::fs::remove_file(mtl_path);
}
} // namespace

// ── ⭐⭐ CEIR-31b-4-b-i GATE (Vulkan). See run_frosted_glass_mask_arm. Arm (i) rides here: the reads=[] mask pass records
// with NO Vulkan validation error (a dropped/mis-bound 0-read pass fires the capture).
TEST_CASE("CEIR-31b-4-b-i GATE (Vulkan): the frosted-glass composite reads the panel mask on device",
          "[scene-render][ceir31b][gpu][vulkan]")
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

    gpu::ValidationCapture capture(*vk);
    run_frosted_glass_mask_arm(*raster, *vk);
    // ⛔ arm (i): the reads=[] mask pass (+ the whole chain) records with NO validation error across both renders.
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == gpu::ValidationSeverity::Info) { continue; }
            WARN("[31b-4-b-i] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ CEIR-31b-4-b-ii-2 GATE (Vulkan): the tint_noise integer hash computes CORRECTLY on the device. See
// run_tint_noise_hash_arm — GPU==eval (31b-1a-iii proved eval==C++). ValidationCapture confirms the single-read
// fullscreen binds its backdrop cleanly (a dropped/mis-bound texture would fire the capture).
TEST_CASE("CEIR-31b-4-b-ii-2 GATE (Vulkan): the tint_noise integer hash computes on device",
          "[scene-render][ceir31b][gpu][vulkan]")
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

    gpu::ValidationCapture capture(*vk);
    run_tint_noise_hash_arm(*raster, *vk);
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == gpu::ValidationSeverity::Info) { continue; }
            WARN("[31b-4-b-ii-2] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ CEIR-31b-4-b-iii-1 GATE (Vulkan): the ui chain preserves the scene's orientation. See run_hard_edge_orientation_arm.
TEST_CASE("CEIR-31b-4-b-iii-1 GATE (Vulkan): the ui chain preserves scene orientation on a geometry hard-edge",
          "[scene-render][ceir31b][gpu][vulkan]")
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

    gpu::ValidationCapture capture(*vk);
    run_hard_edge_orientation_arm(*raster, *vk);
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == gpu::ValidationSeverity::Info) { continue; }
            WARN("[31b-4-b-iii-1] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ CEIR-31b-4-b-iii-2 GATE (Vulkan): the ui blur is a monotone, non-ringing low-pass; far from the edge the effect
// reproduces the scene value (tint=1/amp=0). See run_hard_edge_numeric_arm.
TEST_CASE("CEIR-31b-4-b-iii-2 GATE (Vulkan): the ui blur is a monotone low-pass, far==scene on a geometry hard-edge",
          "[scene-render][ceir31b][gpu][vulkan]")
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

    gpu::ValidationCapture capture(*vk);
    run_hard_edge_numeric_arm(*raster, *vk);
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == gpu::ValidationSeverity::Info) { continue; }
            WARN("[31b-4-b-iii-2] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ CEIR-31b-4-b-iii-3 GATE (Vulkan): the blur's ramp width scales with its step spec-const. See run_width_scaling_arm.
TEST_CASE("CEIR-31b-4-b-iii-3 GATE (Vulkan): the ui blur ramp width scales with its step spec-const",
          "[scene-render][ceir31b][gpu][vulkan]")
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

    gpu::ValidationCapture capture(*vk);
    run_width_scaling_arm(*raster, *vk);
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == gpu::ValidationSeverity::Info) { continue; }
            WARN("[31b-4-b-iii-3] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ CEIR-31b-4-b-iv-f GATE (Vulkan): the 2-iteration blur ping-pong chains (iter-2 widens iter-1). See run_ping_pong_arm.
TEST_CASE("CEIR-31b-4-b-iv-f GATE (Vulkan): the 2-iter blur ping-pong chains through the reused transients",
          "[scene-render][ceir31b][gpu][vulkan]")
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

    gpu::ValidationCapture capture(*vk);
    run_ping_pong_arm(*raster, *vk);
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == gpu::ValidationSeverity::Info) { continue; }
            WARN("[31b-4-b-iv-f] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ CEIR-31b-4-b-iv-g-1 GATE (Vulkan): the baked step literal does NOT travel to 128² (the falsifier). See run_step_travels_arm.
TEST_CASE("CEIR-31b-4-b-iv-g-1 GATE (Vulkan): the baked blur step literal does not travel across resolution",
          "[scene-render][ceir31b][gpu][vulkan]")
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

    gpu::ValidationCapture capture(*vk);
    run_step_travels_arm(*raster, *vk);
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == gpu::ValidationSeverity::Info) { continue; }
            WARN("[31b-4-b-iv-g-1] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ CEIR-31b-4-b-iv-g-2 GATE (Vulkan): the derivation is resolution- AND axis-correct on device. See run_derive_step_arm
// — the shipped DERIVED step byte-matches the explicit H=1/64,V=1/32 literals and the SWAPPED axes differ. ValidationCapture
// confirms the non-square chain records cleanly across all three renders (a mis-resolved spec would fire the capture).
TEST_CASE("CEIR-31b-4-b-iv-g-2 GATE (Vulkan): the derived blur step is resolution- and axis-correct on device",
          "[scene-render][ceir31b][gpu][vulkan]")
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

    gpu::ValidationCapture capture(*vk);
    run_derive_step_arm(*raster, *vk);
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == gpu::ValidationSeverity::Info) { continue; }
            WARN("[31b-4-b-iv-g-2] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

#if defined(_WIN32)
// ── ⭐⭐ CEIR-31b-4-b-i GATE (DX12): the SAME authored frame + the SAME empty-world 0-draw clear on the OTHER backend.
// ⛔ The clear_scope fix has a per-backend implementation (Vulkan vkCmdBeginRendering+End, DX12 OMSetRenderTargets +
// ClearRenderTargetView/ClearDepthStencilView); the Vulkan gate alone leaves the DX12 0-draw clear UNEXECUTED (the
// "compiled ≠ ran" scar). No ValidationCapture (that is a Vulkan facility); a correct render + no crash is the DX12 proof.
TEST_CASE("CEIR-31b-4-b-i GATE (DX12): the frosted-glass composite reads the panel mask on device",
          "[scene-render][ceir31b][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    run_frosted_glass_mask_arm(*raster, *gctx);
}

// ── ⭐⭐ CEIR-31b-4-b-ii-2 GATE (DX12): the SAME hash-numeric probe on the OTHER backend. The hash is an integer function
// of integer coords ⇒ both backends match the SAME oracle, so this + the Vulkan gate transitively prove cross-backend
// agreement. ⛔ the emitter must lower the U32 `>>` to an unsigned shift on DXIL too; the Vulkan gate alone leaves that
// unproven on DX12 (the "compiled ≠ ran" scar).
TEST_CASE("CEIR-31b-4-b-ii-2 GATE (DX12): the tint_noise integer hash computes on device",
          "[scene-render][ceir31b][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    run_tint_noise_hash_arm(*raster, *gctx);
}

// ── ⭐⭐ CEIR-31b-4-b-iii-1 GATE (DX12): the SAME orientation ruling on the OTHER backend (the flip arg is backend-conditional).
TEST_CASE("CEIR-31b-4-b-iii-1 GATE (DX12): the ui chain preserves scene orientation on a geometry hard-edge",
          "[scene-render][ceir31b][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    run_hard_edge_orientation_arm(*raster, *gctx);
}

// ── ⭐⭐ CEIR-31b-4-b-iii-2 GATE (DX12): the SAME blur-numeric arms on the OTHER backend. The blur weights + the far==scene
// reproduction are backend-agnostic (a separable Gaussian of a uniform region), so a DX12 divergence would be a real emitter
// or resample defect the Vulkan gate alone leaves unproven (the "compiled != ran" scar).
TEST_CASE("CEIR-31b-4-b-iii-2 GATE (DX12): the ui blur is a monotone low-pass, far==scene on a geometry hard-edge",
          "[scene-render][ceir31b][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    run_hard_edge_numeric_arm(*raster, *gctx);
}

// ── ⭐⭐ CEIR-31b-4-b-iii-3 GATE (DX12): the SAME width∝step ruling on the OTHER backend (the separable blur is backend-agnostic).
TEST_CASE("CEIR-31b-4-b-iii-3 GATE (DX12): the ui blur ramp width scales with its step spec-const",
          "[scene-render][ceir31b][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    run_width_scaling_arm(*raster, *gctx);
}

// ── ⭐⭐ CEIR-31b-4-b-iv-f GATE (DX12): the SAME ping-pong chaining on the OTHER backend (the transient reuse + the frame
// graph's read/write wiring are per-backend; the Vulkan gate alone leaves the DX12 ping-pong unproven — the "compiled != ran" scar).
TEST_CASE("CEIR-31b-4-b-iv-f GATE (DX12): the 2-iter blur ping-pong chains through the reused transients",
          "[scene-render][ceir31b][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    run_ping_pong_arm(*raster, *gctx);
}

// ── ⭐⭐ CEIR-31b-4-b-iv-g-1 GATE (DX12): the SAME resolution-falsifier on the OTHER backend (the resample + blur step are
// per-backend; a baked literal that happened to travel on one backend would still be caught here). See run_step_travels_arm.
TEST_CASE("CEIR-31b-4-b-iv-g-1 GATE (DX12): the baked blur step literal does not travel across resolution",
          "[scene-render][ceir31b][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    run_step_travels_arm(*raster, *gctx);
}

// ── ⭐⭐ CEIR-31b-4-b-iv-g-2 GATE (DX12): the SAME axis-correct derivation on the OTHER backend. The extent → 1/extent
// resolution happens in the backend-neutral record seat, but the specialized program + the resample are per-backend; the
// Vulkan gate alone leaves the DX12 derivation unproven (the "compiled != ran" scar). See run_derive_step_arm.
TEST_CASE("CEIR-31b-4-b-iv-g-2 GATE (DX12): the derived blur step is resolution- and axis-correct on device",
          "[scene-render][ceir31b][gpu][dx12]")
{
    auto gctx = gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    run_derive_step_arm(*raster, *gctx);
}
#endif // _WIN32
