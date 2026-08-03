// scene_renderer.cpp — GEO-7 (D-007 row 72): chunk-grain extract → cull → partial upload → vertex-pulling
// instanced submission. See scene_renderer.hpp for the pipeline + data contract.

#include <crd/scenerender/scene_renderer.hpp>

#include <crd/lod/impostor_atlas.hpp>
#include <crd/lod/lod_asset.hpp>
#include <crd/lod/lod_chain.hpp>

// REN-38-D5: every vertex program this renderer runs is COOKED FROM A `.crdv`.
#include <crd/vertexcook/vertex_asset.hpp>
// REN-38-C4: the scene SURFACE is cooked from an authored `.crdm`, not a C++ builder.
#include <crd/matcook/material_asset.hpp>
// REN-38-E7: the LIGHTING is an authored declaration cooked into the technique body.
#include <crd/lightcook/lighting_asset.hpp>

#include <crd/scenerender/csm.hpp> // REN-3.2-b: stabilized cascade fitting
#include <crd/math/cmath.hpp>      // crd::math::sqrt — the caster screen-size metric's row norms

#include <crd/anim/pose.hpp>
#include <crd/geometry/primitives/transform.hpp>
#include <crd/gpu/context.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_cook.hpp>     // REN-37.1: build_fs_for_pass - the material VARIANT cook path
#include <crd/kir/ckir_lighting.hpp> // REN-3.2-b: csm_select_cascade
#include <crd/kir/ckir_material.hpp> // REN-37.1: the OpenPBR surface slab a material outputs
#include <crd/kir/ckir_technique.hpp> // REN-37.2: the LIGHTING TECHNIQUE, named + swappable by an asset
#include <crd/kir/ckir_variant.hpp>   // REN-37.7: the DECLARED variant matrix + content-hash dedup
#include <crd/framecook/frame_asset.hpp>   // REN-37.10: the AUTHORED frame graph this renderer HOSTS
#include <crd/framecook/frame_runtime.hpp> // REN-37.10: FrameRecorder + IFrameGraphHost
#include <crd/log/log.hpp>              // REN-37.10: a graph that fails to record must SAY so
#include <crd/platform/filesystem.hpp>  // REN-38-F15: disk-first asset resolution (a file shadows the pack)
#include <crd/resources/openpbr_material.hpp>
#include <crd/resources/resource_manager.hpp>
#include <crd/resources/texture_resource.hpp> // REN-2 Half B: the cooked base-color map the forward pass samples
#include <crd/scene/query.hpp>
#include <crd/scene/render_components.hpp>
#include <crd/scene/spatial_bvh_index.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>

#include <chrono> // REN-8: CPU wall-clock of render(), to compare against the frame graph's GPU timestamps
#include <cmath>
#include <cstring>

namespace crd::scenerender
{

CRD_DEFINE_LOG_CHANNEL(g_log_scenerender, "SceneRender", crd::log::LogLevel::Info)

namespace
{

[[nodiscard]] constexpr crd::u32 clamp_u32(crd::u32 v, crd::u32 lo, crd::u32 hi) noexcept
{
    if (v < lo) { return lo; }
    return v > hi ? hi : v;
}

// ── the CKIR forward pass ──────────────────────────────────────────────────────────────────────────────────────────
// The RET-6 builder scaffolding, self-contained (crd-scene-render does not depend on crd-draw): the u32 storage
// buffer at set 0 / binding 0 (KOp::StorageLoad), floats as bit patterns via int_bits_to_float.
struct Gx
{
    crd::kir::KGraph& g;
    crd::kir::Shape   sh;

    explicit Gx(crd::kir::KGraph& graph) : g(graph), sh(crd::kir::make_shape({1})) {}

    [[nodiscard]] int kf(double v) { return g.constant(v, sh, crd::kir::DType::F32); }
    [[nodiscard]] int ku(crd::u32 v) { return g.constant(static_cast<double>(v), sh, crd::kir::DType::U32); }
    [[nodiscard]] int add(int a, int b) { return g.binary(crd::kir::KOp::Add, a, b); }
    [[nodiscard]] int sub(int a, int b) { return g.binary(crd::kir::KOp::Sub, a, b); }
    [[nodiscard]] int mul(int a, int b) { return g.binary(crd::kir::KOp::Mul, a, b); }
    [[nodiscard]] int dvd(int a, int b) { return g.binary(crd::kir::KOp::Div, a, b); }
    [[nodiscard]] int mx(int a, int b) { return g.binary(crd::kir::KOp::Max, a, b); }
    [[nodiscard]] int loadu(int idx) { return g.storage_load(idx); }
    [[nodiscard]] int loadf(int idx) { return g.int_bits_to_float(g.cast(loadu(idx), crd::kir::DType::I32)); }
    [[nodiscard]] int hdru(crd::u32 word) { return loadu(ku(word)); }
    [[nodiscard]] int hdrf(crd::u32 word) { return loadf(ku(word)); }

    // clip = M (header words `base`.. `base`+15, column-major) · vec4(p, 1)
    void mul_mat4(crd::u32 base, int px, int py, int pz, int out[4])
    {
        const int v[4] = {px, py, pz, kf(1.0)};
        for (crd::u32 i = 0; i < 4U; ++i)
        {
            int acc = mul(hdrf(base + 0U * 4U + i), v[0]);
            acc     = add(acc, mul(hdrf(base + 1U * 4U + i), v[1]));
            acc     = add(acc, mul(hdrf(base + 2U * 4U + i), v[2]));
            acc     = add(acc, mul(hdrf(base + 3U * 4U + i), v[3]));
            out[i]  = acc;
        }
    }
    void mul_view_proj(int px, int py, int pz, int out[4]) { mul_mat4(6U, px, py, pz, out); }
    // REN-3.2-b: the same multiply against cascade `c`'s light_vp — the ONLY difference between the scene VS and
    // the shadow VS. The cascade index is a COMPILE-TIME constant (one shader variant per cascade), which is why
    // a single storage binding can serve all of them: the variant selects the header slice, not a uniform.
    void mul_light_vp(crd::u32 cascade, int px, int py, int pz, int out[4])
    {
        mul_mat4(kHdrCsmLightVp + cascade * 16U, px, py, pz, out);
    }
};


// ── REN-37.1: THE MATERIAL AS A SURFACE, cooked per PASS. ───────────────────────────────────────────────────
// ⛔ Until now this renderer HAND-WROTE its fragment shaders (`build_scene_fs`, `_textured`, `_shadowed`) and
// never touched `build_fs_for_pass` — the principled material-variant cook path, which was referenced only by
// two test files. That is the root cause the REN-37 design names: with no variant pipeline to extend, "add
// shadows" meant "hand-write another FS", which meant "hand-write the passes that feed it", which is how CSM
// ended up as C++ in violation of the top rule.
//
// The split (ADR-0102): the MATERIAL outputs an OpenPBR SurfaceData and computes NO lighting; the RENDER PATH
// applies the lighting. `build_fs_for_pass` is the join: it builds the surface ONCE and routes it per pass —
// depth-only for Shadow/DepthPrepass, MRT pack for GBuffer, `shade_forward` (real Cook-Torrance) for Forward.
//
// ⭐ THE FREE COLLAPSE, now actually exercised: for PassType::Shadow the surface is never consumed, so B7
// `lower_entry` DCEs the ENTIRE surface computation and every opaque material cooks to the same empty FS.
// The shadow shader is no longer written by hand at all - it FALLS OUT of the material.

// What the scene's surface builder needs beyond the shared SurfaceInputs: the per-instance tint varying (and,
// when the group has one, the base-colour map). Passed through `MaterialTemplate::user` so the shared
// `SurfaceInputs` struct stays the engine-wide contract rather than growing a renderer-specific field.
struct SceneSurfaceCtx
{
    int  tint     = -1; // vec4 per-instance colour varying
    int  uv       = -1; // vec2 texture coordinate varying
    bool textured = false;
    const crd::containers::String* disk_text = nullptr;
};

// ── ⭐⭐ REN-38-C4: THE SCENE MATERIAL IS AN AUTHORED `.crdm`. ────────────────────────
// ⛔⛔ `scene_build_surface` USED TO LIVE HERE — a C++ `MaterialTemplate::build_surface` function pointer, which
// is what made "invent a material" mean "edit and recompile the engine". It is GONE. The surface is now a node
// graph cooked by `crd-material-cook`, and THE DELETION IS THE PROOF the C band landed.
//
// ⭐ The material reads its inputs BY VARYING LOCATION through the geometric nodes — `geomcolor(1)` is the
// per-instance tint, `normal(0)` the interpolated world normal, `texcoord(3)` the uv — which is the same
// contract 38-D4 checks the vertex program against. The material declares what it reads instead of being handed
// it, so a material that wants the TANGENT frame at location 4 needs no engine change either.

// The `MaterialTemplate` adapter: cook the authored `.crdm` for this variant. ⛔ A cook failure returns a
// NEGATIVE node, never a substitute surface — a material silently replaced by another renders a plausible
// object that is not the one anybody authored.
int scene_build_surface(crd::kir::KGraph& g, int struct_id, const crd::kir::cook::SurfaceInputs& /*in*/, void* user)
{
    auto* c = static_cast<SceneSurfaceCtx*>(user);
    if (c == nullptr || c->disk_text == nullptr) { return -1; }
    crd::matcook::MaterialDesc desc(crd::memory::default_allocator());
    crd::containers::String    where(crd::memory::default_allocator());
    if (crd::matcook::parse_material_toml(crd::containers::StringView(c->disk_text->c_str()), desc, &where)
        != crd::matcook::MaterialCookError::Ok)
    {
        return -1;
    }
    return crd::matcook::cook_material(desc, g, struct_id);
}

int flat_build_surface(crd::kir::KGraph& g, int struct_id, const crd::kir::cook::SurfaceInputs& /*in*/,
                       void* user)
{
    if (user == nullptr) { return -1; }
    crd::matcook::MaterialDesc desc(crd::memory::default_allocator());
    crd::containers::String    where(crd::memory::default_allocator());
    const char* text = static_cast<const crd::containers::String*>(user)->c_str();
    if (crd::matcook::parse_material_toml(crd::containers::StringView(text), desc, &where)
        != crd::matcook::MaterialCookError::Ok)
    {
        return -1;
    }
    return crd::matcook::cook_material(desc, g, struct_id);
}

// ── REN-37.2/37.3: WHAT THE RENDERER COOKS, and the BINDING CONTRACT's runtime half ─────────────────────────
// A cooked scene fragment program is fully described by: which PASS, whether the group has a base-colour map,
// which TECHNIQUE shades it, and that technique's option values. Everything else is derived.
struct SceneShaderConfig
{
    crd::kir::cook::PassType              pass      = crd::kir::cook::PassType::Forward;
    bool                                  textured  = false;
    const crd::kir::technique::Technique* tech      = nullptr; // null ⇒ depth-only passes need none
    crd::u32                              map_size  = 2048U;   // shadow atlas edge, for the PCF texel step
    crd::u32                              cascades  = kMaxCascades;
    crd::u32                              pcf_taps  = 4U;
    // ⭐⭐ REN-40-D: the cascade blend, in percent of a cascade's footprint. 0 = the historical hard select.
    crd::u32                              blend_pct = 0U;
    // ⭐⭐ REN-40-D: 0 = fixed-radius PCF, 1 = PCSS; and the light's angular RADIUS in hundredths of a degree.
    crd::u32                              soft_mode = 0U;
    crd::u32                              light_angle_x100 = 27U;
    crd::u32                              soft_max_texels  = 24U;
    crd::u32                              soft_search_taps = 8U;
    crd::u32                              fade_pct         = 30U;
    // 38-G1 (user-directed): the DISK-resolved material text (F15). Null = the embedded default. Set by
    // `cook_fs`, which is the one place with access to the asset resolution.
    const crd::containers::String*        material_text = nullptr;
    // ⭐⭐ REN-39-C1: cook the READ-ONLY twin — the FS half of an INDEXED program pair. The flag rides the
    // KEntry into the emitters (DX12 t0 SRV / GLSL `readonly`) AND into the content hash, so the twin dedups
    // separately from its u0 sibling instead of colliding with it.
    bool storage_read_only = false;
    // ⭐⭐ REN-39-D1: the backend's CLIP-SPACE Y direction, folded into the `csm_light_vp` binding below so the
    // TECHNIQUE never has to know it (see IRasterContext::ndc_y_points_down). It changes the emitted GRAPH, so
    // the content hash separates the two cooks automatically — no extra hash term needed.
    bool flip_clip_y = false;
    // The DEPTH-PREPASS twist on PassType::Shadow: inject the SAME Bayer dither discard the forward FS carries,
    // so a cross-dissolving instance writes prepass depth for exactly the pixels its forward draw will keep.
    // ⛔ Only the prepass sets this — the cascade shadow FS must stay undithered, because the shadow VS
    // deliberately does not emit the fade varying the discard reads.
    bool dither_depth = false;
    // ⭐⭐ REN-40-C4: the dither cross-dissolve band. When > 0 the FS reads a flat fade varying at location 4
    // and discards pixels via a 4x4 Bayer threshold.
    crd::f32 dither_band = 0.0F;
};

// Resolve the technique's DECLARED bindings into node ids, in ABI order.
//
// ⭐ THIS FUNCTION IS THE SEAM. The technique declares WHAT it needs, by name and type; the renderer declares
// WHERE that lives in ITS data layout. Neither knows the other's internals, so the same technique ports to a
// renderer with a completely different buffer layout unchanged — and a renderer that cannot supply a declared
// binding FAILS BY NAME here rather than compiling a shader that reads garbage.
//
// ⛔ Today the pass-frequency values live in the group's storage-buffer HEADER because that is this renderer's
// actual binding model. When real per-frequency uniform blocks land (ADR-0102's set-frequency model), ONLY THIS
// FUNCTION CHANGES — the techniques, the assets and the contract are untouched. That is the whole reason the
// binding is declared by name+frequency instead of by set+slot.
[[nodiscard]] bool resolve_scene_bindings(crd::kir::KGraph& g, const crd::kir::technique::Technique& t,
                                          const SceneShaderConfig& cfg,
                                          crd::containers::Array<crd::i32>& out)
{
    namespace kir = crd::kir;
    namespace tq  = crd::kir::technique;
    Gx c(g);
    out.clear();
    // ⭐ REN-40-D: the atlas image node, remembered so the PCSS binding can REUSE it (see below).
    crd::i32 atlas_tex = -1;
    for (int i = 0; i < t.n_bindings; ++i)
    {
        const tq::TechniqueBinding& b = t.bindings[i];
        const char*                 n = b.name;
        if (tq::detail::tech_name_eq(n, "shadow_atlas"))
        {
            // texture + COMPARISON sampler — two nodes, as `bind_type_node_count` states.
            // ⛔⛔ REN-38: bindings 4/5, NOT 1/2. The material base-colour map lives at 1/2, and when the atlas
            // shared those slots a group could be textured OR shadowed but never both — the REN-3.2-b regression
            // the user saw the moment shadows turned on. A frame singleton gets its own fixed binding.
            // ⭐⭐ REN-40-D: under `soft_mode >= 2` the SAME bindings hold the MOMENT atlas — a COLOUR array
            // through a LINEAR sampler. The shadow-ness of these nodes must match what the executor binds
            // (chosen from the resource format), or the cooked shader declares a comparison sampler over a
            // colour image; deciding it from the same soft_mode option the technique body reads is what keeps
            // the writer and the reader of this seam in agreement by construction.
            const bool moments = cfg.soft_mode >= 2U;
            atlas_tex = g.texture(0, 4, kir::DType::F32, kir::TexDim::Tex2D, /*arrayed=*/true, /*ms=*/false,
                                  /*shadow=*/!moments);
            out.push_back(atlas_tex);
            out.push_back(g.sampler(0, 5, /*shadow=*/!moments));
        }
        else if (tq::detail::tech_name_eq(n, "shadow_atlas_depth"))
        {
            // ⭐⭐ REN-40-D (PCSS): THE SAME IMAGE, A SECOND SAMPLER. ⛔⛔ THE TEXTURE NODE IS REUSED, NOT REMADE.
            // The emitters are SEPARABLE (`uniform texture2DArray tex_0_4` + `uniform sampler samp_0_5`), and the
            // shadow-ness lives in the SAMPLER — which is exactly Vulkan's model and exactly why one image can
            // serve both. But creating a SECOND texture node at the same (set, binding) emits a SECOND
            // declaration of `tex_0_4`, and the shader then fails to compile on a redefinition — the graph builds,
            // the program does not, and `set_shadows_enabled` merely returns false.
            // ⛔ A blocker search needs the STORED DEPTH, which a comparison sampler cannot return: the
            // read-the-depth overload does not exist in GLSL. Hence one image, two samplers, both declared.
            if (atlas_tex < 0) { return false; } // `shadow_atlas` is declared first; this depends on it
            out.push_back(atlas_tex);
            out.push_back(g.sampler(0, 6, /*shadow=*/false));
        }
        else if (tq::detail::tech_name_eq(n, "csm_light_vp"))
        {
            // The header stores each cascade's light_vp COLUMN-MAJOR: column j at word base + j*4. `g.mat4` takes
            // four vec4 COLUMNS, so the layouts line up directly — no transpose, and no place for one to hide.
            //
            // ⛔⛔ REN-39-D1 — THE CLIP-Y CONVENTION LIVES HERE, NOT IN THE TECHNIQUE. The atlas is RASTERIZED
            // through this same matrix, and the two APIs disagree about which way +Y runs down a render target
            // (Vulkan down, D3D12 up). So on D3D12 the atlas is stored vertically MIRRORED relative to Vulkan,
            // while the technique's portable `v = ndc.y*0.5 + 0.5` is identical on both — every shadow lookup
            // read the wrong row. It is invisible on screen (a colour target and the fullscreen pass that
            // consumes it flip TOGETHER) and it survived a full audit of the fit, the culling, the per-slice
            // DSVs, the barriers, the indexed draw path and both emitters, because none of those was wrong.
            //
            // Negating the matrix's Y ROW here flips `lp.y` for the FS ONLY — the shadow VS keeps reading the
            // raw header matrix, so what is RASTERIZED is untouched and only the LOOKUP is corrected. The
            // technique stays one portable formula, which is the whole point of the binding seam.
            const bool fy = cfg.flip_clip_y;
            for (crd::u32 ci = 0; ci < b.count; ++ci)
            {
                const crd::u32 base = kHdrCsmLightVp + (ci * 16U);
                int            col[4];
                for (crd::u32 j = 0; j < 4U; ++j)
                {
                    const int y = c.hdrf(base + (j * 4U) + 1U);
                    col[j]      = g.vec4(c.hdrf(base + (j * 4U) + 0U), fy ? c.sub(c.kf(0.0), y) : y,
                                         c.hdrf(base + (j * 4U) + 2U), c.hdrf(base + (j * 4U) + 3U));
                }
                out.push_back(g.mat4(col[0], col[1], col[2], col[3]));
            }
        }
        else if (tq::detail::tech_name_eq(n, "csm_map_size"))
        {
            out.push_back(c.kf(static_cast<double>(cfg.map_size)));
        }
        else
        {
            return false; // a declared binding this renderer cannot supply — REPORTED, never silently skipped
        }
    }
    return static_cast<int>(out.size()) == t.binding_node_count();
}

// Cook the scene FS for `cfg.pass` from the material, shaded by `cfg.tech`. The Forward variant gets the REAL
// Cook-Torrance BRDF through the NAMED technique — which is what makes "swap the lighting by editing an asset"
// true rather than aspirational.
[[nodiscard]] bool build_scene_fs_cooked(crd::kir::KGraph& g, crd::kir::KEntry& fe, const SceneShaderConfig& cfg)
{
    namespace kir = crd::kir;
    namespace ck  = crd::kir::cook;
    namespace tq  = crd::kir::technique;
    namespace mat = crd::kir::material;
    const auto sh = kir::make_shape({1});
    const auto k  = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    Gx         c(g);

    SceneSurfaceCtx ctx;
    ctx.disk_text = cfg.material_text; // 38-G1: a shipped .crdm SHADOWS the embedded default
    ck::SurfaceInputs in;
    // The varyings the cooked VS supplies. A depth-only variant reads none of them - lowering removes the
    // interpolant fetches along with the surface, which is exactly the collapse described above.
    in.world_normal = g.stage_in(kir::KType::vec(kir::DType::F32, 3), 0, kir::Interp::Smooth);
    ctx.tint        = g.stage_in(kir::KType::vec(kir::DType::F32, 4), 1, kir::Interp::Flat);
    const int wpos4 = g.stage_in(kir::KType::vec(kir::DType::F32, 4), 2, kir::Interp::Smooth);
    in.world_pos    = g.vec3(g.swizzle(wpos4, 0), g.swizzle(wpos4, 1), g.swizzle(wpos4, 2));
    ctx.uv          = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 3, kir::Interp::Smooth);
    ctx.textured    = cfg.textured;
    // ⭐ REN-37.3: the REAL view direction, from the FRAME-frequency camera position the header now carries
    // (words 96..98). It used to be the placeholder constant (0,1,0), which degenerates `NoV` and with it the
    // Smith visibility term, `env_brdf_approx` and the energy compensation — the whole specular chain.
    const int eye   = g.vec3(c.hdrf(kHdrCameraPos + 0U), c.hdrf(kHdrCameraPos + 1U), c.hdrf(kHdrCameraPos + 2U));
    in.view_dir     = g.normalize(g.binary(kir::KOp::Sub, eye, in.world_pos));

    // ⛔ CONVENTION MISMATCH, and it renders pure BLACK if you get it wrong. The header stores the direction
    // TOWARD the light (what dot(N, L) wants directly), but `lighting::directional_light` takes the direction
    // light TRAVELS and negates it internally. Passing the header value straight through flips L away from the
    // light, so N.L <= 0 everywhere and the whole scene is black. Negate here, once, at the boundary.
    // (Same class of bug as the CSM shadow-camera inversion — two libraries, two conventions, no type to catch it.)
    const int ldir  = g.vec3(c.sub(k(0.0), c.hdrf(22U)), c.sub(k(0.0), c.hdrf(23U)), c.sub(k(0.0), c.hdrf(24U)));
    const int lcol  = g.vec3(k(1.0), k(1.0), k(1.0));

    const ck::MaterialTemplate tmpl{&scene_build_surface, &ctx};
    const ck::VariantOptions   opts{mat::AlphaMode::Opaque, 0.5};
    // B7 lowering is ON: the const-folder used to eat `StorageLoad` (a memory read with a literal index) and
    // rendered this variant black. Fixed at the root in ckir.hpp and pinned by the `[kir][lower][b7]` gate.
    // ⭐⭐ REN-40-C4: when dither is active the FS must inject a Bayer threshold discard AFTER the technique
    // outputs are built but BEFORE lowering (lowering needs the FragCoord/discard_cond in the graph to emit
    // them). So `do_lower` is false when dither is active; we lower manually after the injection.
    // The dithered DEPTH PREPASS (dither_depth) carries the same Bayer discard as the forward FS, so a
    // cross-dissolving instance writes prepass depth for exactly the pixels its forward draw keeps.
    const bool dither_active = cfg.dither_band > 0.0F
                               && (cfg.pass == ck::PassType::Forward
                                   || (cfg.dither_depth && cfg.pass == ck::PassType::Shadow));
    if (cfg.pass != ck::PassType::Forward || cfg.tech == nullptr)
    {
        // Depth-only / G-buffer: no technique is invoked at all, which is exactly why every opaque material
        // collapses to the SAME shadow program under lowering.
        const tq::Technique none = tq::standard_forward();
        if (!tq::build_fs_for_pass(tmpl, none, cfg.pass, opts, in, g, fe, ldir, lcol, nullptr, 0, nullptr, 0,
                                   /*do_lower=*/!dither_active))
        {
            return false;
        }
        if (!dither_active) { return true; }
        // fall through to the shared Bayer injection + manual lowering below
    }
    else
    {
    crd::containers::Array<crd::i32> binds(g.serial_nodes().allocator());
    if (!resolve_scene_bindings(g, *cfg.tech, cfg, binds)) { return false; }
    const crd::f64 option_values[8] = {static_cast<crd::f64>(cfg.cascades),   static_cast<crd::f64>(cfg.pcf_taps),
                                       static_cast<crd::f64>(cfg.blend_pct),  static_cast<crd::f64>(cfg.soft_mode),
                                       static_cast<crd::f64>(cfg.light_angle_x100),
                                       static_cast<crd::f64>(cfg.soft_max_texels),
                                       static_cast<crd::f64>(cfg.soft_search_taps),
                                       static_cast<crd::f64>(cfg.fade_pct)};
    if (!tq::build_fs_for_pass(tmpl, *cfg.tech, cfg.pass, opts, in, g, fe, ldir, lcol, binds.data(),
                               static_cast<int>(binds.size()), option_values, cfg.tech->n_options,
                               /*do_lower=*/!dither_active))
    {
        return false;
    }
    }
    // ── ⭐⭐⭐ REN-41 (Stage 3): TEMPORAL-STOCHASTIC DITHER — interleaved-gradient noise (Jimenez) whose sample
    // point is shifted EVERY FRAME by an R2 low-discrepancy (Roberts) offset, compared against the flat fade alpha
    // from the VS (location 4). ⛔ The old 4×4 Bayer was SPATIALLY uniform but TEMPORALLY FROZEN — identical every
    // frame — so TAA saw one static checkerboard and the LOD pop merely CRAWLED across it. A per-frame pattern lets
    // TAA AVERAGE the two levels into a genuinely seamless cross-dissolve (the UE5 "dithered LOD + TAA" result).
    // `fade < noise → discard`; at fade = 1.0 nothing drops, at fade = 0.0 everything does — a smooth ramp between.
    if (dither_active)
    {
        const int fade = g.stage_in(kir::KType::make_scalar(kir::DType::F32), 4, kir::Interp::Flat);
        const int fc   = g.builtin(kir::KBuiltin::FragCoord);
        const int fcx  = g.swizzle(fc, 0); // FragCoord.x (float pixel centre)
        const int fcy  = g.swizzle(fc, 1); // FragCoord.y
        const auto fract = [&](int v) { return c.sub(v, g.unary(kir::KOp::Floor, v)); };
        // the frame seed, masked to 1024 so `float(frame)` stays exact and the R2 offset cycles (>>128× the TAA
        // convergence window, so a repeat is never observable).
        const int frame_f = g.cast(g.binary(kir::KOp::BitAnd, c.hdru(kHdrFrameIndex), c.ku(1023U)), kir::DType::F32);
        // R2 (Roberts) per-frame pixel offset — the additive-recurrence low-discrepancy sequence, which spreads
        // the samples far more evenly across frames than a golden-ratio scalar would.
        const int ox = c.mul(fract(c.mul(frame_f, k(0.75487766624))), k(64.0));
        const int oy = c.mul(fract(c.mul(frame_f, k(0.56984029099))), k(64.0));
        const int px = c.add(fcx, ox);
        const int py = c.add(fcy, oy);
        // interleaved-gradient noise: frac(52.9829189 · frac(0.06711056·x + 0.00583715·y)) ∈ [0,1)
        const int d     = c.add(c.mul(px, k(0.06711056)), c.mul(py, k(0.00583715)));
        const int noise = fract(c.mul(k(52.9829189), fract(d)));
        fe.discard_cond = g.binary(kir::KOp::CmpLt, fade, noise);
        kir::lower::lower_entry(g, fe);
    }
    return true;
}

// ── ⭐⭐ REN-41 (velocity, path A): the MOTION-VECTOR fragment shader. ──────────────────────────────────────
// The velocity prepass folds a colour write into the depth prepass. It reads the two CLIP interpolants the
// matched velocity VS emits — `prev_clip` (loc 5: the object re-skinned/re-transformed by the PREVIOUS pose,
// then projected by the CURRENT view_proj) and `cur_clip` (loc 6) — and writes the OBJECT-motion delta in
// current-camera UV space. Because BOTH clips use the current JITTERED view_proj, the camera motion AND the TAA
// jitter cancel in the delta, so a static instance stores exactly ZERO and the resolve reconstructs the camera
// reprojection separately (`prev_uv = R_reproject(uv,depth) + velocity`). ⛔ It carries the SAME temporal LOD
// dither the forward FS carries (fade at loc 4, the IGN threshold) so the prepass writes velocity + depth for
// EXACTLY the pixels the forward draw keeps — the DEPTH-ONLY≠forward scar in its MRT form. ⛔ The v axis flips
// with the backend NDC-Y convention (REN-39-D1): the sign MUST match taa_resolve's ndc.y↔v mapping
// (`ndc_y_points_down ? +1 : −1`, i.e. `flip_clip_y ? −1 : +1`) or the reprojection slides in Y every frame.
[[nodiscard]] bool build_velocity_fs_cooked(crd::kir::KGraph& g, crd::kir::KEntry& fe, const SceneShaderConfig& cfg)
{
    namespace kir = crd::kir;
    Gx         c(g);
    const auto k = [&](double v) { return c.kf(v); };

    const int prev = g.stage_in(kir::KType::vec(kir::DType::F32, 4), 5, kir::Interp::Smooth);
    const int cur  = g.stage_in(kir::KType::vec(kir::DType::F32, 4), 6, kir::Interp::Smooth);
    // perspective divide → NDC. The delta is PURE object motion: both clips are projected by the CURRENT
    // view_proj, so the camera transform (and its baked TAA jitter) is identical in both and cancels.
    const int prev_ndc_x = c.dvd(g.swizzle(prev, 0), g.swizzle(prev, 3));
    const int prev_ndc_y = c.dvd(g.swizzle(prev, 1), g.swizzle(prev, 3));
    const int cur_ndc_x  = c.dvd(g.swizzle(cur, 0), g.swizzle(cur, 3));
    const int cur_ndc_y  = c.dvd(g.swizzle(cur, 1), g.swizzle(cur, 3));
    // ndc → uv delta. u = ndc.x*0.5 + 0.5 on both backends (the +0.5 cancels in the delta); v uses the
    // backend sign so the result lands in the SAME uv space the resolve adds it to.
    const int    u_delta = c.mul(c.sub(prev_ndc_x, cur_ndc_x), k(0.5));
    const double vsgn    = cfg.flip_clip_y ? -0.5 : 0.5;
    const int    v_delta = c.mul(c.sub(prev_ndc_y, cur_ndc_y), k(vsgn));
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    // RG16F attachment: the two motion components; b/a are written 0 and dropped by the two-channel target.
    fe.out[0] = {g.vec4(u_delta, v_delta, k(0.0), k(0.0)), 0};

    // ⭐⭐⭐ REN-41 (Stage 3): the SAME temporal-stochastic dither the forward FS applies — identical block to
    // build_scene_fs_cooked, so the prepass depth (and velocity) survive for exactly the pixels the forward keeps.
    if (cfg.dither_band > 0.0F)
    {
        const int  fade  = g.stage_in(kir::KType::make_scalar(kir::DType::F32), 4, kir::Interp::Flat);
        const int  fc    = g.builtin(kir::KBuiltin::FragCoord);
        const int  fcx   = g.swizzle(fc, 0);
        const int  fcy   = g.swizzle(fc, 1);
        const auto fract = [&](int v) { return c.sub(v, g.unary(kir::KOp::Floor, v)); };
        const int  frame_f =
            g.cast(g.binary(kir::KOp::BitAnd, c.hdru(kHdrFrameIndex), c.ku(1023U)), kir::DType::F32);
        const int ox    = c.mul(fract(c.mul(frame_f, k(0.75487766624))), k(64.0));
        const int oy    = c.mul(fract(c.mul(frame_f, k(0.56984029099))), k(64.0));
        const int px    = c.add(fcx, ox);
        const int py    = c.add(fcy, oy);
        const int d     = c.add(c.mul(px, k(0.06711056)), c.mul(py, k(0.00583715)));
        const int noise = fract(c.mul(k(52.9829189), fract(d)));
        fe.discard_cond = g.binary(kir::KOp::CmpLt, fade, noise);
    }
    kir::lower::lower_entry(g, fe);
    return true;
}

// ── ⭐⭐ REN-38-D5: THE VERTEX PROGRAMS ARE AUTHORED. ─────────────────────────────
// ⛔⛔ `build_scene_vs_shadowed` / `_skinned` / `build_shadow_vs` USED TO LIVE HERE — ~200 lines of C++ with the
// vertex-pull layout compiled in: the header word map as bare integers, the 12-word vertex record, the instance
// record, four-influence linear-blend skinning, and a hardcoded varying set. They are GONE. Each is now a `.crdv`
// cooked by `crd-vertex-cook`, and THE DELETION IS THE PROOF the D band landed: if the asset could not express
// these programs, this code would have had to stay.
//

namespace
{
// ⛔ A cook failure must FAIL, never fall back: a vertex program silently replaced by another reads the same
// buffer with a different layout, and the mesh draws as noise rather than not at all.
// `out_desc` (optional) receives the parsed declaration — the REN-38 live varying contract verifies every
// cooked fragment program's read set against it at program-creation time.
[[nodiscard]] bool cook_vs(crd::memory::IAllocator* alloc, const char* body, const char* extra,
                           crd::kir::KGraph& g, crd::kir::KEntry& ve,
                           crd::vertcook::VertexProgramDesc* out_desc = nullptr)
{
    crd::containers::String toml(alloc);
    toml.append(body);
    if (extra != nullptr) { toml.append(extra); }
    crd::vertcook::VertexProgramDesc local(alloc);
    crd::vertcook::VertexProgramDesc& desc = out_desc != nullptr ? *out_desc : local;
    crd::containers::String          where(alloc);
    if (crd::vertcook::parse_vertex_toml(crd::containers::StringView(toml.c_str(), toml.size()), desc, &where)
        != crd::vertcook::VertexCookError::Ok)
    {
        return false;
    }
    return crd::vertcook::cook_vertex_program(desc, g, ve);
}
} // namespace

// ⭐⭐ REN-37.4: the hand-written CASCADED-SHADOW fragment shader USED TO LIVE HERE - ~120 lines of C++ that
// selected a cascade, projected, PCF-filtered and attenuated. It is GONE. The identical shading is now the
// authored `forward_csm` TECHNIQUE (`ckir_technique.hpp` + `assets/technique/forward_csm.crdt`), reached by a
// frame-graph pass that says `technique = "forward_csm"`. THE DELETION IS THE PROOF the slice landed: if the
// technique path could not express CSM, this code would have had to stay.

// ⭐ REN-38-D5: the SHADOW vertex program was `build_shadow_vs(g, ve, cascade)` — the scene pull path with a
// different matrix, hand-written. It is now the SAME declaration with `transform = "light_vp"` and a cascade,
// and no varyings at all (a shadow pass writes depth and nothing else).

// ⭐ REN-37.1: the shadow FS is no longer HAND-WRITTEN — it is COOKED FROM THE MATERIAL at PassType::Shadow.
// `build_fs_for_pass` sets n_out = 0 and never consumes the surface, so B7 `lower_entry` DCEs the entire surface
// computation (texture fetches, parameters, interpolant reads) and every opaque material collapses to the SAME
// empty program, deduped by content hash. This is the "free collapse" the REN-37 design predicts, exercised for
// real: what other engines hand-engineer as depth-only permutations falls out of lowering because we compose in
// an IR rather than in text.
// ⛔ REN-38-C4: the `build_shadow_fs` wrapper is GONE too — it was a one-line alias for `cook_fs` with
// `PassType::Shadow`, and once the surface came from a `.crdm` it had no caller at all. A dead wrapper that
// still compiles is the kind of redundancy that later reads as a second path.

// ⭐ REN-38-D5: the SKINNED vertex program was ~90 lines that unpacked a 6-word skin record and blended four
// palette matrices by hand. It is now the shared declaration plus a `[skin]` block.


// ⭐ REN-37.2: the hand-written TEXTURED scene VS + FS used to live here. Both are GONE. The textured variant
// is now the SAME cooked material at `PassType::Forward` with `textured = true` - the base-colour map rides the
// material SURFACE (`scene_build_surface`), where it belongs, instead of a second hand-written shader that had
// its own varying layout and its own toy lighting. One VS (`build_scene_vs_shadowed`) now feeds every fragment
// variant, which is what made the layouts agree in the first place.

// ── hashing / small helpers ────────────────────────────────────────────────────────────────────────────────────────

constexpr crd::u64 kFnvOffset = 14695981039346656037ULL;
constexpr crd::u64 kFnvPrime  = 1099511628211ULL;

} // namespace

// ── REN-1: what ONE culled group draws. ─────────────────────────────────────────────────────────────────────
// ⛔ Defined HERE, above `Impl`, because the CONTRIBUTION ARENA stores the per-viewport list: the authored graph
// resolves its draw lists through the host during recording, and on the multi-viewport path that recording spans
// several calls before anything executes. A local would not survive it.
struct SceneDraw
{
    crd::gpu::IRasterProgram* program    = nullptr;
    crd::gpu::IStorageBuffer* buffer     = nullptr;
    crd::gpu::ITexture*       base_color = nullptr; // REN-2 Half B: the material albedo map (null ⇒ the flat draw)
    crd::u32                  vertex_count = 0;
    // ⭐⭐ REN-38: this draw's program rebases every load by `table[DrawIndex]` — the executor must record it
    // through the multi verb (which pushes the row) even alone; a classic verb would leave the push stale.
    bool                      rebased = false;
    // ⭐⭐ REN-39-C1: the INDEXED-PULL fields (0 = the classic pull draw). `first_index` is the ABSOLUTE u32
    // word of the group's index section (private buffer: `indices_off`; consolidated: `region_base +
    // indices_off`) — the executor binds the index view at offset 0, one convention for both layouts.
    crd::u32 index_count = 0U;
    crd::u32 instance_count = 0U;
    // ⭐⭐ REN-40-A: this draw's GPU-written command (per group), the byte offset of THIS VIEW's command inside
    // it, and the cull dispatch width for the group's instance count.
    crd::gpu::IStorageBuffer* cull_args = nullptr;
    crd::u32                  cull_args_offset = 0U;
    crd::u32                  cull_groups = 0U;
    crd::u32 first_index = 0U;
    // ⭐⭐ REN-40-C2: which LOD level this item draws. 0 everywhere until selection ships, and 0 is exactly the
    // historical behaviour — the row is inert at one slot.
    crd::u32 lod_slot = 0U;
    // The DEPTH-ONLY twin for non-instance depth passes (the depth prepass) — see DrawItem::program_depth.
    crd::gpu::IRasterProgram* program_depth = nullptr;
    // ⭐⭐ REN-41 (velocity): the MOTION-VECTOR twin the velocity MRT prepass draws this group with — same
    // per-group (skinned vs rigid) selection as program_depth, but writing the motion vector + depth. Null when
    // velocity programs did not cook (motion vectors off) — the executor then falls back to the pass program.
    crd::gpu::IRasterProgram* program_velocity = nullptr;
};

// ── the Impl ───────────────────────────────────────────────────────────────────────────────────────────────────────

struct SceneRenderer::Impl
{
    crd::memory::IAllocator*         alloc  = nullptr;
    crd::gpu::IGpuContext*           ctx    = nullptr;
    crd::gpu::IRasterContext*        raster = nullptr;
    crd::resources::ResourceManager* rm     = nullptr;

    std::unique_ptr<crd::gpu::IGpuProgram>    vs;
    // ⛔ BORROWED from `fs_programs`, which owns every fragment program. Two owners of one program would
    // double-free, and a second identical program would be pure waste — that is what the cache exists to stop.
    crd::gpu::IGpuProgram*                    fs = nullptr;
    std::unique_ptr<crd::gpu::IRasterProgram> program;
    // GEO-8: the skinned program pair + the skeleton/clip handle caches + palette scratch
    std::unique_ptr<crd::gpu::IGpuProgram>    vs_skinned;
    std::unique_ptr<crd::gpu::IRasterProgram> program_skinned;
    // REN-40-F: skinned variants that compose with textures and shadows
    std::unique_ptr<crd::gpu::IRasterProgram> program_skinned_textured;
    std::unique_ptr<crd::gpu::IRasterProgram> program_skinned_shadowed;
    std::unique_ptr<crd::gpu::IRasterProgram> program_skinned_textured_shadowed;
    // REN-2 Half B: the TEXTURED program (samples the material base-color map); the per-material GPU-texture cache
    // is declared next to material_color below (ctor init order). REN-37.2 removed its separate VERTEX program —
    // the textured variant is the same cooked material with `textured = true`, over the one shared VS.
    crd::gpu::IGpuProgram*                    fs_textured = nullptr; // borrowed from `fs_programs`
    // REN-38: the COMBINED textured+shadowed variant — base colour at bindings 1/2 AND the atlas at 4/5.
    crd::gpu::IGpuProgram*                    fs_textured_shadowed = nullptr; // borrowed from `fs_programs`
    std::unique_ptr<crd::gpu::IRasterProgram> program_textured_shadowed;
    // ⭐⭐ REN-38 (scene-buffer consolidation): the REBASED scene VS (rebase_table = kSceneDrawTableOff) over
    // the same flat FS, and THE one scene buffer its draws read. Plain groups under a shadow-free frame render
    // through this pair as ONE multi-draw batch; skinned/textured groups keep their private buffers (their
    // verbs bind per-draw state a batch cannot share yet, and their VSs are not rebased).
    [[nodiscard]] bool scene_ok() const noexcept { return raster != nullptr; }
    std::unique_ptr<crd::gpu::IGpuProgram>    vs_rebased; // the DrawIndex-rebased scene VS (owned)
    std::unique_ptr<crd::gpu::IRasterProgram> program_rebased;
    std::unique_ptr<crd::gpu::IStorageBuffer> scene_buf;
    crd::u32                                  scene_buf_words = 0U;
    bool                                      scene_geom_valid = false; // regions hold geometry (re-upload on rebuild)
    std::unique_ptr<crd::gpu::IRasterProgram> program_textured;
    crd::containers::HashMap<crd::resources::ResourceId, crd::resources::ResourceHandle<crd::anim::SkeletonResource>>
        skeleton_cache{nullptr};
    crd::containers::HashMap<crd::resources::ResourceId, crd::resources::ResourceHandle<crd::anim::AnimClipResource>>
        clip_cache{nullptr};
    crd::containers::Array<crd::anim::JointPose>  pose_scratch{nullptr};
    crd::containers::Array<crd::math::Mat4f>      world_scratch{nullptr};
    crd::containers::Array<crd::math::Mat4f>      palette_scratch{nullptr};
    crd::containers::Array<crd::f32>              palette_staging{nullptr};
    crd::containers::Array<crd::u32>              anim_state_staging{nullptr}; // REN-40-F: per-instance (clip_local_off, time_bits)

    crd::u64 structure_sig = 0;
    bool     has_structure = false;

    // ── ⭐⭐ REN-40-B: THE CHUNK INDEX — why a static frame now costs nothing. ────────────────────────────────
    // The extract was already chunk-grain and dirty-aware for the UPLOAD, but DISCOVERING the dirt was
    // O(entities) twice over, and at 1M instances that was 171 ms of a 337 ms frame:
    //   (1) the structure signature hashed `EntityId[n]` + `MeshRenderer[n]` BYTE BY BYTE for every chunk —
    //       40 MB of FNV per frame at 1M, to answer a question that only ever changes on spawn/despawn; and
    //   (2) deciding whether a chunk was stale scanned EVERY group's EVERY run looking for its key, so the
    //       walk was O(chunks x runs) — quadratic in the scene.
    // Both are replaced by an index keyed on the chunk (its entity-array pointer, the same key `ChunkRun`
    // always used): one O(1) probe gives the chunk's recorded Transform version and the exact span of runs it
    // owns. A chunk whose version has not moved is then skipped WITHOUT TOUCHING ONE ENTITY, so a frame in
    // which nothing moved costs O(chunks) — a few thousand compares — instead of O(entities).
    // ⛔ The signature still exists and is still exact; it is just built from O(1) facts per chunk (key, count,
    // first/last entity id, the MeshRenderer chunk version) instead of from every byte. Insert/move bump the
    // destination chunk's `version_counter` (archetype_chunk_storage.cpp), and a swap-remove changes the count,
    // so every structural edit still moves it — including the spawn-and-despawn-in-one-chunk case a count-only
    // signature would miss, because the spawn bumps the renderer version.
    struct ChunkEntry
    {
        crd::u64 tversion   = 0; // Transform chunk-version at last extract
        crd::u32 first_run  = 0; // [first_run, first_run + run_count) into `runs`
        crd::u32 run_count  = 0;
        crd::u32 entity_count = 0; // guards the incremental write against a structure that moved under us
    };
    // One run = the contiguous slot range ONE chunk contributed to ONE group. This is the same grain
    // `MeshGroup::runs` carried; it moves here so a chunk can reach its runs in O(1) and so the upload walks a
    // DIRTY LIST rather than every run of every group.
    struct RunEntry
    {
        crd::u32 group = 0;
        crd::u32 first = 0;
        crd::u32 count = 0;
    };
    crd::containers::HashMap<const crd::scene::EntityId*, crd::u32> chunk_index; // chunk key -> `chunks` slot
    crd::containers::Array<ChunkEntry> chunks;
    crd::containers::Array<RunEntry>   runs;
    crd::containers::Array<crd::u32>   dirty_runs; // indices into `runs`, cleared every sync
    // ⛔ Reused across frames: `upload_bounds_range` used to allocate a fresh Array per call, which on a
    // structural frame at 1M is a 24 MB allocation inside the hot loop.
    crd::containers::Array<crd::f32>   bounds_staging{nullptr};

    crd::containers::HashMap<crd::resources::ResourceId, crd::u32> group_of_mesh;
    // material colour cache (linear RGBA); resolved on rebuild
    crd::containers::HashMap<crd::resources::ResourceId, crd::math::Vec4f> material_color;
    // REN-2 Half B: the per-material GPU base-color texture cache (owns the uploaded ITexture)
    crd::containers::HashMap<crd::resources::ResourceId, std::unique_ptr<crd::gpu::ITexture>> material_texture{nullptr};
    // entity → (group << 32 | slot) — the BVH candidate → instance bridge, rebuilt with the structure
    crd::containers::HashMap<crd::scene::EntityId, crd::u64> entity_slot;

    // REN-1: the persistent frame graph (created lazily, reset per frame) — the whole scene's groups compose in
    // ONE submission. Null on a backend without it (DX12 until its port) ⇒ the synchronous per-draw fallback.
    std::unique_ptr<crd::gpu::IFrameGraph> frame_graph;
    bool                                   readback = true; // REN-8: on by default so gates keep read_pixel
    // REN-3.2-b: the frame's stabilized cascades + the per-cascade shadow programs (one variant each, so the
    // cascade index is a compile-time constant in the VS).
    CsmConfig                              csm{};
    CsmCascades                            cascades{};
    crd::math::Mat4f                       prev_light_vp[kMaxCascades]{}; // REN-40-E: previous frame's matrices for cache detection
    crd::u32                               csm_frame = 0;                // REN-40-E2: frame counter for round-robin far cascade scheduling
    crd::u32                               frame_index = 0;              // ⭐⭐⭐ REN-41 (Stage 3): monotonic per-frame counter for the TEMPORAL LOD dither (header word kHdrFrameIndex)
    [[nodiscard]] bool cascade_scheduled(crd::u32 index) const noexcept
    {
        if (index < 2U || !round_robin_far) { return true; }
        return (csm_frame & 1U) == (index & 1U);
    }
    // ⛔ Two separate facts, deliberately not merged. `shadow_programs_ok` says the cascade shaders COMPILED;
    // `shadows_requested` says the caller actually wants shadows. Rendering cascades costs a full extra pass
    // over the draw list PER CASCADE, so turning them on merely because the shaders built would make every
    // existing consumer pay 4x the geometry cost for an atlas nothing samples yet. Off until asked.
    bool                                   shadow_programs_ok = false;
    bool                                   shadows_requested  = false;
    [[nodiscard]] bool shadows_active() const noexcept { return shadow_programs_ok && shadows_requested; }
    std::unique_ptr<crd::gpu::IGpuProgram> shadow_vs[kMaxCascades];
    crd::gpu::IGpuProgram*                 shadow_fs = nullptr; // borrowed from `fs_programs`
    std::unique_ptr<crd::gpu::IRasterProgram> shadow_prog[kMaxCascades];
    // the forward program that SAMPLES the atlas (cascade select + PCF + slope-scaled bias)
    // ⛔ `vs_shadowed` is a BORROWED pointer into `vs` since REN-37.2 unified the varying layout — one vertex
    // program feeds every fragment variant. It is deliberately NOT a unique_ptr any more: two owners of the same
    // program would double-free, and a second identical VS would be pure waste.
    crd::gpu::IGpuProgram*                    vs_shadowed = nullptr;
    crd::gpu::IGpuProgram*                    fs_shadowed = nullptr; // borrowed from `fs_programs`
    std::unique_ptr<crd::gpu::IRasterProgram> program_shadowed;
    // ── ⭐⭐ REN-39-C1: the INDEXED program set — the SAME declarations cooked `indexed = true` over the
    // READ-ONLY fragment twins, one per variant the draw-list can pick. ⛔ ALL-OR-NOTHING: `use_indexed` is
    // finalized at the end of init_programs and honoured only when EVERY twin whose base cooked also cooked —
    // a cascade pass draws every item with ONE instance program, so a frame may never mix addressing modes.
    // The parity gate flips `use_indexed` to prove pull and indexed render bit-identical frames.
    bool use_indexed = true;
    // ⭐⭐ REN-40-A: the GPU-driven cull switch (default OFF — see set_gpu_cull) plus the device-side command
    // buffer it produces. ⛔ ONE args buffer for the whole frame, laid out [view][group] at the BACKEND'S
    // command stride, so a draw finds its command at `(view * n_groups + group) * stride` and the two APIs'
    // differing layouts never leak into the renderer's arithmetic.
    bool                                      gpu_cull_on = false;
    // REN-40-A: run the CPU cull TOO, only so its verdict can be compared (a gate mode — see the header).
    bool                                      gpu_cull_verify = false;
    crd::u32                                  fill_diag_cull_gates = 0U;
    crd::u32                                  fill_diag_dispatch_max = 0U;
    crd::u32                                  fill_diag_total_items = 0U;
    crd::u32                                  fill_diag_index_count_0 = 0U;
    crd::u32                                  fill_diag_record_ok = 0U;
    crd::u32                                  fill_diag_build_ok = 0U;
    crd::u32                                  fill_diag_pass_count = 0U;
    crd::u32                                  fill_diag_occ_step = 0U;
    // ⭐⭐ REN-40-F: GPU skinning — the compute kernel computes the palette on the device.
    bool                                      gpu_skinning_on = false;
    crd::gpu::IGpuProgram*                    kern_skin_compute = nullptr;
    // ⭐⭐ REN-41 (velocity, skinned): the device palette-snapshot kernel — copies palette_off → prev_palette_off
    // per skinned instance BEFORE gpu_skin overwrites the palette, so the velocity prepass reads last frame's pose.
    // Gated on kHdrGpuSkinActive so it no-ops under CPU skinning (where the renderer CPU-uploads prev_palette).
    crd::gpu::IGpuProgram*                    kern_palette_snapshot = nullptr;
    // ⭐⭐ REN-40-C2: the authored LOD policy and whether chains are built at all.
    // ⛔ DEFAULT OFF, like every other performance switch here, so the A/B runs on
    // ONE build (the readback-A/B rule).
    bool                                      lod_enabled = false;
    // ⭐⭐ REN-40-C2: how many LOD levels the visible-list and command layouts reserve PER VIEW. Fixed for
    // the renderer's lifetime from the installed policy, because it is baked into every cooked cull kernel
    // and every buffer size — a value that could change per frame would silently invalidate both.
    // ⛔ 1 (no policy) is the historical layout byte for byte.
    crd::u32                                  lod_slots   = 1U;
    // One-shot: LOD was asked for but the draw path cannot carry a per-draw slot (see the draw builder).
    bool                                      lod_unaddressable_reported = false;
    crd::lod::LodPolicy                       lod_policy{};
    std::unique_ptr<crd::gpu::IStorageBuffer> cull_args;
    crd::u32                                  cull_args_groups = 0U;
    std::unique_ptr<crd::gpu::IGpuProgram> vs_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_idx;
    std::unique_ptr<crd::gpu::IGpuProgram> vs_rebased_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_rebased_idx;
    std::unique_ptr<crd::gpu::IGpuProgram> vs_skinned_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_skinned_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_skinned_textured_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_skinned_shadowed_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_skinned_textured_shadowed_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_textured_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_shadowed_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_textured_shadowed_idx;
    // The DEPTH PREPASS programs — camera VS + a depth-only FS (Bayer-dithered when the LOD dither is on, so
    // the prepass depth matches the pixels the forward draw keeps). See DrawItem::program_depth for the fault
    // the forward-program fallback caused.
    std::unique_ptr<crd::gpu::IRasterProgram> program_prepass_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_prepass_skinned_idx;
    // ⭐⭐ REN-41 (velocity): the MOTION-VECTOR program twins — the velocity VS assets paired with the velocity FS,
    // cooked `indexed = true` exactly like the depth-prepass twins. OPTIONAL: a cook failure leaves these null and
    // the TAA resolve keeps its neighbourhood clamp (movers fall back, everything else reprojects from the camera).
    // The VS programs are OWNED here because the raster programs borrow them (the vs_idx precedent).
    std::unique_ptr<crd::gpu::IGpuProgram>    vs_velocity_idx;
    std::unique_ptr<crd::gpu::IGpuProgram>    vs_skinned_velocity_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_velocity_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_skinned_velocity_idx;
    std::unique_ptr<crd::gpu::IGpuProgram> shadow_vs_idx[kMaxCascades];
    std::unique_ptr<crd::gpu::IRasterProgram> shadow_prog_idx[kMaxCascades];
    SceneRenderer::FramePassFn             overlay_fn   = nullptr; // the grid/gizmo/debug pass, in OUR graph
    void*                                  overlay_user = nullptr;
    crd::gpu::FgImage                      overlay_img{}; // REN-39: the image the woven overlay pass declared
    // REN-37.8: when non-null, `render()` RECORDS into this caller-owned graph and does not reset/build/execute.
    // Borrowed for the duration of one `contribute()` call and cleared immediately after — it must never outlive
    // the caller's graph.
    crd::gpu::IFrameGraph*                 external_fg  = nullptr;

    // ⛔⛔ THE CONTRIBUTION ARENA, and why it has to exist. `add_pass(...).execute(fn, user)` stores the USER
    // POINTER and dereferences it at `execute()` time. While `render()` both recorded AND executed, that pointer
    // could safely be a local — the stack frame was still alive. The moment recording is split from execution
    // (REN-37.8), every such local DANGLES: the host executes after all contributions have returned.
    //
    // So the per-contribution draw list and its state live HERE, at FIXED ADDRESSES. Fixed, not an Array: a
    // growing Array would REALLOCATE and dangle every pointer already handed to the graph — the exact scar
    // REN-36.3 paid for with its expansion table (`recs` reserved to its exact total before any `&recs[i]` is
    // taken). A stated cap that is CHECKED beats a dynamic buffer that silently moves.
    // RESERVED TO ITS EXACT TOTAL in the constructor, so no `push_back` can ever reallocate it and move an entry
    // the graph already points at. (Reserving exactly is also how REN-36.3's expansion table stays safe.)
    static constexpr crd::u32 kMaxContributions = 32U;
    crd::containers::Array<crd::containers::Array<SceneDraw>> contrib_draws;
    crd::u32                                                  contrib_used = 0U;

    // ⭐⭐ REN-37.10: THE AUTHORED GRAPH THIS RENDERER HOSTS. Not a graph it BUILDS — one it is handed.
    // `frame` is the shadowed default; `fallback` is the shadows-off tier its `fallback` names. Both start as the
    // embedded built-in pack and are replaced wholesale by `set_frame_graph()`, which is what makes changing the
    // cascade count, the atlas format, the pass order, or the technique an ASSET EDIT rather than a rebuild.
    crd::framecook::FrameGraphDesc  frame;
    crd::framecook::FrameGraphDesc  fallback;
    // REN-38-F6: true once `set_frame_graph_toml` installed an explicit graph — the shadows-tier step-down
    // then no longer applies (it is the built-in forward pair's capability contract, not the frame's).
    bool                            frame_overridden = false;
    crd::framecook::FrameRecorder   recorder;
    bool                            frame_ok = false;
    // REN-36.3-b: the World this renderer last synced. Borrowed — the caller owns it, and it is only read during
    // recording, inside the same frame that called `sync()`. Null before the first sync (a stub-raster test),
    // which the query path treats as "no filters apply" rather than as a failure.
    crd::scene::World*              world = nullptr;
    // The group's entity list, for the draw-list filter. Returns null for an out-of-range group.
    // 38-G1: the GROUP behind draw-list row `i` — index-parallel with the draw list by the same construction
    // `groups_view` uses (culled groups are skipped in both). Null when the row has no group.
    crd::containers::Array<MeshGroup*> draw_groups{crd::memory::default_allocator()};
    crd::containers::Array<SceneDraw>  impostor_draws{crd::memory::default_allocator()};
    [[nodiscard]] const MeshGroup* group_of_draw(crd::u32 i) const noexcept
    {
        return i < draw_groups.size() ? draw_groups[i] : nullptr;
    }
    [[nodiscard]] const crd::containers::Array<crd::scene::EntityId>* group_entities(crd::usize g) const noexcept
    {
        return g < groups_view.size() ? &groups_view[g] : nullptr;
    }
    // The per-group entity lists mirrored for the filter, rebuilt each frame alongside the draw list. ⛔ Mirrored
    // rather than pointing into `MeshGroup::slot_entity`, because the draw list is CULLED: draw index i is not
    // group index i once an empty group is skipped.
    crd::containers::Array<crd::containers::Array<crd::scene::EntityId>> groups_view;

    // ── REN-37.2: the TECHNIQUE library + which techniques this renderer shades with. ──
    // Both are NAMES, not code paths. Point them at a different `.crdt` and the whole scene re-shades; that is
    // the top rule reaching the fragment shader, and it is why `set_forward_technique` is a one-line setter
    // rather than a new renderer.
    // ⭐ REN-37.7 IN THE RENDERER: the fragment-program cache, keyed by the CONTENT HASH of the LOWERED graph.
    // Every FS this renderer needs goes through `cook_fs`, so two variants that lower to the same IR share ONE
    // device program. That is where the free collapse becomes real work saved rather than a claim: the Shadow
    // variant never consumes the surface, so lowering DCEs all of it and every material lands on one program.
    // `variants_cooked` vs `fs_programs.size()` is the collapse, REPORTED — a dedup that silently stopped
    // happening would show up as cook time and memory with nothing pointing at it.
    crd::containers::Array<crd::u64>                               fs_hashes;
    crd::containers::Array<std::unique_ptr<crd::gpu::IGpuProgram>> fs_programs;
    crd::u32                                                       variants_cooked = 0U;

    // ── REN-39 (the shadows-off black frame): FALLBACK GRAPHS RESOLVE BY NAME. ──
    // A frame's `fallback` names an ASSET (`crd://frame/<x>` → `frame/<x>.frame.toml`, disk-first like every
    // other default); until now the host ignored the name and always answered the parsed `forward_basic`, so a
    // tonemapped frame could never step down to its tonemapped shadows-off tier. Parsed once, cached by name.
    crd::containers::Array<crd::containers::String>                         fb_frame_names;
    crd::containers::Array<std::unique_ptr<crd::framecook::FrameGraphDesc>> fb_frame_descs;
    crd::u64                                                                stepdown_logged = 0U;

    [[nodiscard]] const crd::framecook::FrameGraphDesc* resolve_frame_asset(crd::containers::StringView crd_name)
    {
        for (crd::usize i = 0; i < fb_frame_names.size(); ++i)
        {
            const crd::containers::String& n = fb_frame_names[i];
            if (crd::containers::StringView(n.c_str(), n.size()) == crd_name) { return fb_frame_descs[i].get(); }
        }
        constexpr crd::containers::StringView prefix("crd://frame/");
        if (crd_name.size() <= prefix.size()
            || std::memcmp(crd_name.data(), prefix.data(), prefix.size()) != 0)
        {
            return nullptr;
        }
        crd::containers::String rel(alloc);
        rel.append("frame/");
        rel.append(crd_name.data() + prefix.size(), crd_name.size() - prefix.size());
        rel.append(".frame.toml");
        crd::containers::String text(alloc);
        if (!asset_text(rel.c_str(), text)) { return nullptr; }
        auto d = std::make_unique<crd::framecook::FrameGraphDesc>(alloc);
        crd::containers::String where(alloc);
        if (crd::framecook::parse_frame_toml(crd::containers::StringView(text.c_str(), text.size()), *d, &where)
            != crd::framecook::FrameCookError::Ok)
        {
            CRD_LOG_ERROR(g_log_scenerender, "fallback frame '{}' failed to parse at '{}'", rel.c_str(),
                          where.c_str());
            return nullptr;
        }
        const crd::framecook::FrameGraphDesc* raw = d.get();
        crd::containers::String               name_copy(alloc);
        name_copy.append(crd_name.data(), crd_name.size());
        fb_frame_names.push_back(static_cast<crd::containers::String&&>(name_copy));
        fb_frame_descs.push_back(std::move(d));
        return raw;
    }

    // Cook `cfg`, hash the lowered graph, and return the cached program when one already matches. Null on a cook
    // or compile failure — never a silently-wrong program.
    // ⭐⭐ REN-38 audit (the LIVE half of 38-D4): `reqs`/`n_reqs` (optional, cap `req_cap`) receive the varying
    // read set of the cooked fragment graph — location, width, interpolation — so `init_programs` verifies every
    // (VS, FS) pair against the DECLARED `.crdv` at program-creation time. The skinned-VS scar (two of four
    // varyings emitted, shading from undefined interpolants) was found by this check in a TEST; running it on
    // the live path is what makes it impossible to reintroduce through ANY future asset edit.
    [[nodiscard]] crd::gpu::IGpuProgram* cook_fs(const SceneShaderConfig& cfg,
                                                 crd::vertcook::VaryingRequirement* reqs = nullptr,
                                                 crd::u32 req_cap = 0U, crd::u32* n_reqs = nullptr)
    {
        if (ctx == nullptr) { return nullptr; }
        // ⭐⭐ 38-G1 (user-directed): THE DEFAULTS ARE ASSETS. The scene material resolves DISK-FIRST exactly
        // like the frames and vertex programs — a shipped `assets/material/scene[_textured].crdm` SHADOWS the
        // embedded copy. ⛔ Until this line the constants were used directly and a user's override was silently
        // ignored; the files existed, were drift-gated, and did nothing. Depth-only passes cook no surface.
        SceneShaderConfig       rcfg = cfg;
        // ⭐⭐ REN-39-D1: the backend's clip-Y convention is stamped HERE, at the ONE place every scene FS is
        // cooked, so no call site can forget it and no authored technique has to know it exists.
        rcfg.flip_clip_y = raster != nullptr && !raster->ndc_y_points_down();
        // ⭐⭐ REN-40-C4: dither band stamped here (same discipline as flip_clip_y — no call site can forget).
        rcfg.dither_band = lod_enabled ? lod_policy.dither_band : 0.0F;
        crd::containers::String mat_text(alloc);
        if (asset_text(cfg.textured ? "material/scene_textured.crdm" : "material/scene.crdm", mat_text))
        {
            rcfg.material_text = &mat_text;
        }
        crd::kir::KGraph g(alloc);
        crd::kir::KEntry e;
        if (!build_scene_fs_cooked(g, e, rcfg)) { return nullptr; }
        // REN-39-C1: the read-only promise is entry state — it feeds the emitters AND the content hash below,
        // so the indexed pair's FS twin is a distinct deduped program, never a collision with its u0 sibling.
        e.storage_read_only = rcfg.storage_read_only;
        // ⭐⭐⭐ REN-41 (VARYING CONTRACT): a fragment that pairs with the SCENE vertex shader — the forward surface
        // and the dither-discard depth prepass — must declare EVERY varying the VS emits, so inject an (unreachable)
        // StageIn for each scene varying this FS does not already read. DXIL packs the PS input signature by REGISTER
        // (declaration order + component packing), so a fragment reading a non-contiguous subset (e.g. flat forward
        // reads normal/worldpos + the LOD-dither fade but skips uv) desyncs from the VS and the PSO fails to link.
        // Injecting the gap makes the two signatures identical by construction — future scene shaders inherit it
        // for free. The shadow depth-only FS reads nothing and pairs with the position-only shadow VS, so it is
        // excluded here. The injected nodes are unreachable: the GLSL path (Vulkan) and the reach-based varying
        // requirements ignore them; only the DXIL PSIn declares them (see the ckir_hlsl PSIn note).
        if ((rcfg.pass == crd::kir::cook::PassType::Forward || rcfg.dither_depth) && n_scene_varyings > 0U)
        {
            bool have_loc[32] = {};
            for (int i = 0; i < g.size(); ++i)
            {
                if (g.node(i).op == crd::kir::KOp::StageIn)
                {
                    const auto loc = static_cast<crd::u32>(g.node(i).iidx);
                    if (loc < 32U) { have_loc[loc] = true; }
                }
            }
            for (crd::u32 v = 0; v < n_scene_varyings; ++v)
            {
                const auto& sv = scene_varyings[v];
                if (sv.location < 32U && !have_loc[sv.location])
                {
                    (void)g.stage_in(sv.type, static_cast<int>(sv.location), sv.interp);
                }
            }
        }
        if (reqs != nullptr && n_reqs != nullptr
            && !crd::vertcook::fs_varying_requirements(g, e, reqs, req_cap, n_reqs, alloc))
        {
            return nullptr; // a fragment program that disagrees with itself about a location — never bind it
        }
        ++variants_cooked;
        const crd::u64 h = crd::kir::technique::graph_content_hash(g, e, alloc);
        for (crd::usize i = 0; i < fs_hashes.size(); ++i)
        {
            if (fs_hashes[i] == h) { return fs_programs[i].get(); }
        }
        std::unique_ptr<crd::gpu::IGpuProgram> p = ctx->create_program(g, e);
        if (p == nullptr) { return nullptr; }
        crd::gpu::IGpuProgram* raw = p.get();
        fs_hashes.push_back(h);
        fs_programs.push_back(std::move(p));
        return raw;
    }

    // ⭐⭐ REN-41 (velocity): cook the MOTION-VECTOR fragment shader — the same clip-Y and dither-band stamping as
    // `cook_fs`, but the velocity graph (no material, no technique), deduped into the shared `fs_programs`
    // keep-alive so both velocity VS twins can borrow it. ⛔ The dither_band MUST match what the velocity VS is
    // cooked with (both take it from the same lod policy) so the FS reads the exact `fade`@4 the VS emits. Null on
    // a cook/compile failure — the caller then leaves the velocity programs null (motion vectors simply off).
    [[nodiscard]] crd::gpu::IGpuProgram* cook_velocity_fs()
    {
        if (ctx == nullptr) { return nullptr; }
        SceneShaderConfig rcfg;
        rcfg.storage_read_only = true; // pairs with the indexed (read-only) velocity VS
        rcfg.flip_clip_y       = raster != nullptr && !raster->ndc_y_points_down();
        rcfg.dither_band       = (lod_enabled && lod_slots > 1U) ? lod_policy.dither_band : 0.0F;
        crd::kir::KGraph g(alloc);
        crd::kir::KEntry e;
        if (!build_velocity_fs_cooked(g, e, rcfg)) { return nullptr; }
        e.storage_read_only = rcfg.storage_read_only;
        const crd::u64 h    = crd::kir::technique::graph_content_hash(g, e, alloc);
        for (crd::usize i = 0; i < fs_hashes.size(); ++i)
        {
            if (fs_hashes[i] == h) { return fs_programs[i].get(); }
        }
        std::unique_ptr<crd::gpu::IGpuProgram> p = ctx->create_program(g, e);
        if (p == nullptr) { return nullptr; }
        crd::gpu::IGpuProgram* raw = p.get();
        fs_hashes.push_back(h);
        fs_programs.push_back(std::move(p));
        return raw;
    }

    crd::kir::technique::TechniqueLibrary  techniques{nullptr};
    const char*                            forward_technique = "standard_forward";
    const char*                            shadow_technique  = "forward_csm";
    crd::u32                               pcf_taps          = 4U; // the `pcf_taps` option value (1|2|4|8|16)
    crd::u32                               blend_pct         = 15U; // REN-40-D `cascade_blend_pct` (0..50)
    crd::u32                               soft_mode         = 0U; // REN-40-D 0 = PCF, 1 = PCSS
    crd::u32                               light_angle_x100  = 27U; // the sun's angular radius, x100 degrees
    crd::u32                               soft_max_texels   = 24U; // REN-40-D the penumbra cap, in texels
    crd::u32                               soft_search_taps  = 8U;  // REN-40-D taps in the blocker search disc
    crd::u32                               fade_pct          = 30U; // shadow distance fade, % of last cascade
    // Shadow-caster screen-size cull: instances below this many camera-projected pixels never enter a cascade's
    // caster list (UE5's "Min Screen Radius For Shadows"; 0 = off). Applied by BOTH cull paths — the CPU
    // cascade loop and the device cull kernels — from this one number, so the two can never disagree.
    crd::f32                               shadow_caster_min_px = 16.0F;
    // Minimum camera-projected height below which an instance does not DRAW at all (camera + impostor lists).
    // Sub-pixel geometry is pure aliasing energy — the far half of a million-instance field shimmers with
    // triangles smaller than a pixel unless something says stop. Under dither the kernel DISSOLVES instances
    // over the octave above this (255 at 2×, 0 at the threshold), so the far field thins smoothly to ground
    // instead of ending at a popping line. 0 = off.
    crd::f32                               min_draw_px          = 3.0F;
    // REN-40-E2 round-robin far cascades — OFF by default (stale-matrix strobe under camera motion; see the
    // header note). The exact-match cache (40-E1) keeps the static-scene win either way.
    bool                                   round_robin_far      = false;

    // ── ⭐⭐ REN-38-F6: the ADVANCED-STAGE programs, cooked from the authored declarations above. ──
    // Lazy (a renderer that never runs an advanced graph pays nothing) and CACHED (a program is cooked once).
    // Every failure returns null, which the executor reports BY NAME — never a silently-skipped pass.
    crd::containers::Array<std::unique_ptr<crd::gpu::IGpuProgram>> adv_stages; // keep-alive for linked programs
    std::unique_ptr<crd::gpu::IRasterProgram> prog_tess;
    std::unique_ptr<crd::gpu::IRasterProgram> prog_mesh;
    std::unique_ptr<crd::gpu::IRasterProgram> prog_visbuffer;
    crd::gpu::IGpuProgram*                    kern_cull = nullptr; // borrowed from adv_stages
    crd::gpu::IGpuProgram*                    kern_cull_mark = nullptr; // borrowed from adv_stages
    // ⭐⭐ REN-40-A: the GPU-DRIVEN pair — the compacting cull and its command RESET, both authored assets.
    // ⛔ ONE variant PER VIEW: `view_index` picks which visible list the kernel fills and `draw_arg_off` picks
    // which command it accumulates into, and BOTH are cook-time constants — so five views are five programs from
    // one authored asset, exactly as the four cascade shadow VS variants already are.
    crd::gpu::IGpuProgram*                    kern_cull_view[1U + kMaxCascades]{};
    crd::gpu::IGpuProgram*                    kern_cull_reset     = nullptr; // borrowed from adv_stages
    crd::gpu::IGpuProgram*                    kern_occlusion_cull = nullptr; // REN-40-G3: camera re-cull with HZB
    crd::gpu::IGpuProgram*                    kern_rt[4] = {nullptr, nullptr, nullptr, nullptr}; // rg/ms/ch/ah
    crd::gpu::IGpuProgram*                    flat_fs   = nullptr; // borrowed from adv_stages
    // The scene TLAS, provided by whoever owns the geometry's device form (B4: the graph names it, the HOST
    // resolves it — the asset format stays free of engine types).
    crd::gpu::IAccelerationStructure*         scene_accel = nullptr;
    // REN-38: the vertex axis of the VariantKey, folded from the LIVE .crdv (the D5-correction close)
    crd::u32                                  vertex_variant = 0U;
    // ⭐⭐⭐ REN-41 (VARYING CONTRACT): the scene VS's FULL interpolant layout (location + type + interp), captured
    // once from the cooked scene VS. Every scene fragment that pairs with this VS declares this WHOLE set on the
    // DX12 side (`cook_fs` injects the varyings it doesn't read) so the DXIL PS input signature packs identically
    // to the VS output signature — the fix that made `--backend dx12 --lod` link. See ckir_hlsl PSIn note.
    struct SceneVarying { crd::u32 location; crd::kir::KType type; crd::kir::Interp interp; };
    SceneVarying                              scene_varyings[crd::vertcook::kMaxVaryings]{};
    crd::u32                                  n_scene_varyings = 0U;
    // Host-resolved external buffers for the compute/RT graphs ("cull_flags", "hits").
    std::unique_ptr<crd::gpu::IStorageBuffer> buf_cull_flags;
    std::unique_ptr<crd::gpu::IStorageBuffer> buf_cull_marks;
    std::unique_ptr<crd::gpu::IStorageBuffer> buf_hits;

    // ── ⭐ REN-38-F15 / REN-41: the ASSET ROOT is the SINGLE SOURCE. ──
    // Every authored asset this renderer cooks is read from a file under the installed root (`set_asset_root`, or
    // the `CRD_ASSETS_DIR` default `init` honours) — that is what makes the `assets/` directory live rather than
    // documentation. There is no in-binary pack to fall back to: a name that resolves to no file returns false and
    // the cook fails loudly downstream, which is the point (a silent fallback that renders is indistinguishable
    // from the edit having worked).
    crd::containers::String asset_root; // empty = no assets resolve (init defaults it from CRD_ASSETS_DIR)
    [[nodiscard]] bool asset_text(const char* name, crd::containers::String& out)
    {
        if (asset_root.size() > 0U)
        {
            crd::containers::String p(alloc);
            p.append(asset_root.c_str());
            p.append("/");
            p.append(name);
            const crd::platform::fs::Path path(crd::containers::StringView(p.c_str(), p.size()));
            if (crd::platform::fs::exists(path))
            {
                return crd::platform::fs::read_file_text(path, out);
            }
        }
        return false;
    }

    // Cook one authored stage BY ASSET NAME (disk-first) and create its device program.
    // ⭐⭐⭐ REN-41 (NDC±Y): a FULLSCREEN-RESAMPLE VS builds its clip position DIRECTLY (no view_proj), so the
    // backend's clip-Y convention is never applied to it — and a pass that samples an RTT by a UV tied to that
    // clip Y comes out vertically MIRRORED on a y-up backend (DX12) while it is correct on Vulkan. The scene
    // geometry is unaffected (it goes through view_proj and the overlay proves it is upright on DX12); only the
    // resample flips. `flip_clip_y` negates the emitted clip-Y (leaving the UV, which is tied to TEXTURE space),
    // which makes the fullscreen resample an identity on BOTH backends. The caller passes `!ndc_y_points_down()`.
    [[nodiscard]] crd::gpu::IGpuProgram* cook_stage_named(const char* asset_name, bool flip_clip_y = false)
    {
        if (ctx == nullptr) { return nullptr; }
        crd::containers::String t(alloc);
        if (!asset_text(asset_name, t)) { return nullptr; }
        crd::kir::KGraph g(alloc);
        crd::kir::KEntry e;
        if (!cook_vs(alloc, t.c_str(), nullptr, g, e)) { return nullptr; }
        if (flip_clip_y && e.position >= 0)
        {
            const int px  = g.swizzle(e.position, 0);
            const int py  = g.swizzle(e.position, 1);
            const int pz  = g.swizzle(e.position, 2);
            const int pw  = g.swizzle(e.position, 3);
            e.position    = g.vec4(px, g.unary(crd::kir::KOp::Neg, py), pz, pw);
        }
        std::unique_ptr<crd::gpu::IGpuProgram> p = ctx->create_program(g, e);
        if (p == nullptr) { return nullptr; }
        crd::gpu::IGpuProgram* raw = p.get();
        adv_stages.push_back(std::move(p));
        return raw;
    }

    // ⛔ 38-G1: `cook_stage(pre, body)` — the variant that took EMBEDDED text — is GONE. Every stage cooks by
    // NAME through `cook_stage_named`, so the disk copy always wins. A dead text-taking wrapper is exactly the
    // second path the build_shadow_fs deletion note warns about.

    // The FLAT fragment program: the authored flat material through the `unlit` technique, with CONSTANT surface
    // inputs — so the cooked graph contains NO StageIn at all and the 38-D4 contract holds against any VS.
    [[nodiscard]] crd::gpu::IGpuProgram* ensure_flat_fs()
    {
        if (flat_fs != nullptr) { return flat_fs; }
        if (ctx == nullptr) { return nullptr; }
        namespace kir = crd::kir;
        namespace ck  = crd::kir::cook;
        namespace tq  = crd::kir::technique;
        kir::KGraph g(alloc);
        kir::KEntry e;
        const auto  sh = kir::make_shape({1});
        const auto  k  = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
        ck::SurfaceInputs in;
        in.world_normal = g.vec3(k(0.0), k(0.0), k(1.0));
        in.world_pos    = g.vec3(k(0.0), k(0.0), k(0.0));
        in.view_dir     = g.vec3(k(0.0), k(0.0), k(1.0));
        // F15: the flat material resolves disk-first like every stage declaration
        crd::containers::String flat_text(alloc);
        const bool              have_disk = asset_root.size() > 0U && asset_text("material/flat.crdm", flat_text);
        const ck::MaterialTemplate tmpl{&flat_build_surface, have_disk ? &flat_text : nullptr};
        const ck::VariantOptions   opts{crd::kir::material::AlphaMode::Opaque, 0.5};
        const tq::Technique        un   = tq::unlit();
        const int                  ldir = g.vec3(k(0.0), k(0.0), k(1.0));
        const int                  lcol = g.vec3(k(1.0), k(1.0), k(1.0));
        if (!tq::build_fs_for_pass(tmpl, un, ck::PassType::Forward, opts, in, g, e, ldir, lcol, nullptr, 0,
                                   nullptr, 0))
        {
            return nullptr;
        }
        std::unique_ptr<crd::gpu::IGpuProgram> p = ctx->create_program(g, e);
        if (p == nullptr) { return nullptr; }
        flat_fs = p.get();
        adv_stages.push_back(std::move(p));
        return flat_fs;
    }

    // ⛔ The visibility-buffer FS is NOT a material — the pass writes an ID, not a surface. This three-node
    // entry is the fixed contract of the pass KIND, the way the empty depth-only FS is the fixed contract of a
    // shadow pass; the authorable halves (the fullscreen VS, the graph, the resolve material) stay assets.
    [[nodiscard]] crd::gpu::IGpuProgram* ensure_visbuffer_fs()
    {
        if (ctx == nullptr) { return nullptr; }
        namespace kir = crd::kir;
        kir::KGraph g(alloc);
        kir::KEntry e;
        // The id as a GRADED GREY — (primId + 1) · 0.25 — so the F6 gate can assert per-primitive pixels on the
        // ordinary colour output (the pixel-blind-smoke scar): left triangle 0.25, right 0.5, clear elsewhere.
        // The full R32Uint id-buffer path is the A11 device gate's claim; this join renders THROUGH it.
        const auto sh   = kir::make_shape({1});
        const int  prim = g.cast(g.builtin(kir::KBuiltin::PrimitiveId), kir::DType::F32);
        const int  grey = g.binary(kir::KOp::Mul,
                                   g.binary(kir::KOp::Add, prim, g.constant(1.0, sh, kir::DType::F32)),
                                   g.constant(0.25, sh, kir::DType::F32));
        e.stage  = kir::KStage::Fragment;
        e.n_out  = 1;
        e.out[0] = {g.vec4(grey, grey, grey, g.constant(1.0, sh, kir::DType::F32)), 0};
        std::unique_ptr<crd::gpu::IGpuProgram> p = ctx->create_program(g, e);
        if (p == nullptr) { return nullptr; }
        crd::gpu::IGpuProgram* raw = p.get();
        adv_stages.push_back(std::move(p));
        return raw;
    }

    [[nodiscard]] crd::gpu::IRasterProgram* ensure_tess_program()
    {
        if (prog_tess != nullptr) { return prog_tess.get(); }
        if (raster == nullptr) { return nullptr; }
        crd::gpu::IGpuProgram* tvs = cook_stage_named("vertex/tess_corners.crdv");
        crd::gpu::IGpuProgram* ths = cook_stage_named("vertex/tess_hull.crdv");
        crd::gpu::IGpuProgram* tds = cook_stage_named("vertex/tess_domain.crdv");
        crd::gpu::IGpuProgram* tfs = ensure_flat_fs();
        if (tvs == nullptr || ths == nullptr || tds == nullptr || tfs == nullptr) { return nullptr; }
        prog_tess = raster->create_tess_program(*tvs, *ths, *tds, *tfs);
        return prog_tess.get();
    }

    [[nodiscard]] crd::gpu::IRasterProgram* ensure_mesh_program()
    {
        if (prog_mesh != nullptr) { return prog_mesh.get(); }
        if (raster == nullptr) { return nullptr; }
        crd::gpu::IGpuProgram* mtk = cook_stage_named("vertex/scene_task.crdv");
        crd::gpu::IGpuProgram* mms = cook_stage_named("vertex/scene_meshlet.crdv");
        crd::gpu::IGpuProgram* mfs = ensure_flat_fs();
        if (mtk == nullptr || mms == nullptr || mfs == nullptr) { return nullptr; }
        prog_mesh = raster->create_task_mesh_program(*mtk, *mms, *mfs);
        return prog_mesh.get();
    }

    // ── ⭐⭐ REN-41 Stage 4 (S4-0): the CLUSTER MESH shader — Nanite cluster unpack on the device. ───────────────
    // A MESH-ONLY program (no task/amplification yet — S4-2 adds GPU-driven selection): `draw_mesh_storage`
    // dispatches ONE workgroup per packed cluster, and workgroup `WorkgroupIndex` unpacks THAT cluster from the
    // 40-I packed buffer (10 u32/cluster) into its ≤128 triangles. Thread `LocalInvocationIndex` = tid writes
    // vertex tid (`positions[cluster_vertices[vertex_offset+tid]]` × view_proj) and primitive tid (three LOCAL
    // indices from the 4-u8-per-u32 packed-triangle stream). ⛔ FIXED 128/128 meshlets + DEGENERATE-PRIMITIVE
    // culling: SetMeshOutputs stays the compile-time MAX and every out-of-range OR non-leaf thread emits a
    // zero-area triangle (0,0,0) the rasteriser drops — so the whole body lives in the value graph and needs NO
    // mesh-emitter change. S4-0 draws the LEAF clusters (level 0) = the original mesh, gated pixel-for-pixel
    // against `unpack_selected_clusters`. This is a C++ CKIR builder like `gpu_skin` (40-F) — one specialised
    // engine-internal shader, no user variants — not an authored `.crdv`.
    std::unique_ptr<crd::gpu::IRasterProgram> prog_cluster_mesh;
    [[nodiscard]] crd::gpu::IRasterProgram* ensure_cluster_mesh_program()
    {
        if (prog_cluster_mesh != nullptr) { return prog_cluster_mesh.get(); }
        if (ctx == nullptr || raster == nullptr) { return nullptr; }
        crd::gpu::IGpuProgram* mfs = ensure_flat_fs();
        if (mfs == nullptr) { return nullptr; }
        namespace kir = crd::kir;
        using KOp     = kir::KOp;
        kir::KGraph g(alloc);
        Gx         gx(g);
        const auto ku   = [&](crd::u32 v) { return gx.ku(v); };
        const auto lu   = [&](int idx) { return gx.loadu(idx); };
        const auto adu  = [&](int a, int b) { return gx.add(a, b); };
        const auto mlu  = [&](int a, int b) { return gx.mul(a, b); };
        const auto shr  = [&](int a, int s) { return g.binary(KOp::Shr, a, s); };
        const auto andk = [&](int a, crd::u32 m) { return g.binary(KOp::BitAnd, a, ku(m)); };

        const int cidx    = g.builtin(kir::KBuiltin::WorkgroupIndex);        // this workgroup's cluster
        const int tid      = g.builtin(kir::KBuiltin::LocalInvocationIndex); // this thread's output slot
        const int clu_off  = gx.hdru(crd::scenerender::kClusterHdrClustersOff);
        const int vtx_off  = gx.hdru(crd::scenerender::kClusterHdrVerticesOff);
        const int tri_off  = gx.hdru(crd::scenerender::kClusterHdrTrianglesOff);
        const int pos_off  = gx.hdru(crd::scenerender::kClusterHdrPositionsOff);
        const int cbase    = adu(clu_off, mlu(cidx, ku(10U))); // 10 u32/cluster (kClusterGpuWords, 40-I layout)
        const int c_voff   = lu(adu(cbase, ku(0U)));           // vertex_offset (into cluster_vertices)
        const int c_toff   = lu(adu(cbase, ku(1U)));           // triangle BYTE offset (into the packed u8 stream)
        const int c_pack   = lu(adu(cbase, ku(2U)));           // vertex_count:8 | triangle_count:8 | level:16
        const int vcount   = andk(c_pack, 0xFFU);
        const int tcount   = andk(shr(c_pack, ku(8U)), 0xFFU);
        const int level    = shr(c_pack, ku(16U));
        const int is_leaf  = g.binary(KOp::CmpEq, level, ku(0U)); // S4-0: draw only the level-0 leaves

        // per-vertex clip position — clamp the read index INTO the cluster so tail threads read a valid vertex.
        const int rv    = g.binary(KOp::Min, tid, g.binary(KOp::Sub, vcount, ku(1U)));
        const int gvi   = lu(adu(adu(vtx_off, c_voff), rv)); // global vertex index
        const int p3    = mlu(gvi, ku(3U));
        const int px    = gx.loadf(adu(pos_off, p3));
        const int py    = gx.loadf(adu(pos_off, adu(p3, ku(1U))));
        const int pz    = gx.loadf(adu(pos_off, adu(p3, ku(2U))));
        int       clip[4];
        gx.mul_view_proj(px, py, pz, clip);

        // per-primitive local index triple — three u8s at byte (c_toff + tid*3): word = byte>>2, shift = (byte&3)*8.
        const auto local_idx = [&](int byte) {
            const int w  = lu(adu(tri_off, shr(byte, ku(2U))));
            const int sh = mlu(andk(byte, 3U), ku(8U));
            return g.binary(KOp::BitAnd, shr(w, sh), ku(0xFFU));
        };
        const int b0   = adu(c_toff, mlu(tid, ku(3U)));
        const int tri  = g.vec3(local_idx(b0), local_idx(adu(b0, ku(1U))), local_idx(adu(b0, ku(2U))));
        const int deg  = g.vec3(ku(0U), ku(0U), ku(0U)); // degenerate (zero-area) triangle → culled
        const int keep = g.binary(KOp::CmpLt, tid, tcount);
        const int prim = g.select(is_leaf, g.select(keep, tri, deg), deg);

        kir::KEntry me;
        me.stage           = kir::KStage::Mesh;
        me.mesh_vertices   = crd::scenerender::kClusterMaxVerts;
        me.mesh_primitives = crd::scenerender::kClusterMaxPrims;
        me.position        = g.vec4(clip[0], clip[1], clip[2], clip[3]);
        me.mesh_prim       = prim;
        kir::lower::lower_entry(g, me);
        std::unique_ptr<crd::gpu::IGpuProgram> mesh = ctx->create_program(g, me);
        if (mesh == nullptr) { return nullptr; }
        prog_cluster_mesh = raster->create_mesh_program(*mesh, *mfs);
        adv_stages.push_back(std::move(mesh));
        return prog_cluster_mesh.get();
    }

    [[nodiscard]] crd::gpu::IRasterProgram* ensure_visbuffer_program()
    {
        if (prog_visbuffer != nullptr) { return prog_visbuffer.get(); }
        if (raster == nullptr) { return nullptr; }
        crd::gpu::IGpuProgram* vvs = cook_stage_named("vertex/visbuffer_fullscreen.crdv");
        crd::gpu::IGpuProgram* vfs = ensure_visbuffer_fs();
        if (vvs == nullptr || vfs == nullptr) { return nullptr; }
        prog_visbuffer = raster->create_raster_program(*vvs, *vfs);
        return prog_visbuffer.get();
    }

    // ── ⭐⭐ 38-G1b: an AUTHORED POST program — the fullscreen VS + a post GRAPH cooked as its FS. The graph
    // text comes through the POST face (`parse_post_toml` + `cook_post_graph` — tonemap ops legal, surface
    // readers refused), and the FS entry is minimal by design: one colour out, alpha forced to 1 (a display
    // transform emits an opaque frame). Cached per name; two names ship embedded (agx · srgb_only).
    [[nodiscard]] crd::gpu::IRasterProgram* ensure_post_program(crd::containers::StringView name)
    {
        const auto name_is = [&](const char* b) {
            const crd::usize bl = std::strlen(b);
            return name.size() == bl && std::memcmp(name.data(), b, bl) == 0;
        };
        const bool is_agx  = name_is("crd://post/tonemap_agx");
        const bool is_srgb = name_is("crd://post/srgb_only");
        if (!is_agx && !is_srgb) { return nullptr; }
        std::unique_ptr<crd::gpu::IRasterProgram>& slot = is_agx ? prog_post_agx : prog_post_srgb;
        if (slot != nullptr) { return slot.get(); }
        if (raster == nullptr || ctx == nullptr) { return nullptr; }
        crd::gpu::IGpuProgram* pvs = cook_stage_named("vertex/post_fullscreen.crdv");
        if (pvs == nullptr) { return nullptr; }
        // ⛔ DISK-FIRST, like every asset here: a shipped `assets/post/*.crdp` SHADOWS the embedded copy, and
        // the drift gate keeps the two canonically identical — ONE declaration, two homes.
        crd::containers::String ptext(alloc);
        if (!asset_text(is_agx ? "post/tonemap_agx.crdp" : "post/srgb_only.crdp", ptext)) { return nullptr; }
        crd::matcook::MaterialDesc pdesc(alloc);
        crd::containers::String    where(alloc);
        if (crd::matcook::parse_post_toml(crd::containers::StringView(ptext.c_str(), ptext.size()), pdesc, &where)
            != crd::matcook::MaterialCookError::Ok)
        {
            return nullptr;
        }
        crd::kir::KGraph fg2(alloc);
        const int        out = crd::matcook::cook_post_graph(pdesc, fg2, &where);
        if (out < 0) { return nullptr; }
        const auto sh1  = crd::kir::make_shape({1});
        const int  onef = fg2.constant(1.0, sh1, crd::kir::DType::F32);
        crd::kir::KEntry fe;
        fe.stage  = crd::kir::KStage::Fragment;
        fe.n_out  = 1;
        fe.out[0] = {fg2.vec4(fg2.vec_comp(out, 0), fg2.vec_comp(out, 1), fg2.vec_comp(out, 2), onef), 0};
        std::unique_ptr<crd::gpu::IGpuProgram> pfs = ctx->create_program(fg2, fe);
        if (pfs == nullptr) { return nullptr; }
        slot = raster->create_raster_program(*pvs, *pfs);
        adv_stages.push_back(std::move(pfs));
        return slot.get();
    }
    std::unique_ptr<crd::gpu::IRasterProgram> prog_post_agx;
    std::unique_ptr<crd::gpu::IRasterProgram> prog_post_srgb;

    // ── ⭐⭐ REN-40-D: the MOMENT-ATLAS program family (crd://shadow/moment_*). ──────────────────────────────
    // Fullscreen technique-library shaders, cooked per CASCADE because the layer is baked into each instance's
    // FS (a fullscreen pass has no per-draw channel — the same rule the per-cascade shadow VS follows), and the
    // blur additionally bakes 1/map_size (there is no header binding on a fullscreen draw to read it from).
    // ── ⭐⭐ REN-41 (TAA): the temporal-AA RESOLVE program + its per-frame constants buffer. ─────────────────
    // A fullscreen pass reads scene_hdr[0] + scene_depth[1] + history[2] (the bindless heap at binding 16) and
    // the reproject matrix from the constants buffer (binding 0), reprojects last frame's history by depth,
    // clamps it to the current neighbourhood, and blends. `taa_constants` is the ONLY per-frame matrix a
    // fullscreen shader can receive (draw_bindless_storage binds b0); the renderer uploads R each frame.
    std::unique_ptr<crd::gpu::IRasterProgram> prog_taa;
    std::unique_ptr<crd::gpu::IStorageBuffer>  taa_constants;
    crd::math::Mat4f                           taa_reproj{};         // R = prev_unjit_vp · inv(cur_jit_vp), caller-set
    bool                                       taa_has_history = false; // false until a prev frame exists
    crd::f32                                   taa_feedback    = 0.9F;   // history blend weight
    [[nodiscard]] crd::gpu::IRasterProgram* ensure_taa_program()
    {
        if (prog_taa != nullptr) { return prog_taa.get(); }
        if (ctx == nullptr || raster == nullptr) { return nullptr; }
        crd::gpu::IGpuProgram* pvs = cook_stage_named("vertex/post_fullscreen.crdv", !raster->ndc_y_points_down());
        if (pvs == nullptr) { return nullptr; }
        namespace kir = crd::kir;
        using DType   = kir::DType;
        using KOp     = kir::KOp;
        kir::KGraph fg(alloc);
        const int  uv   = fg.stage_in(kir::KType::vec(DType::F32, 2), 0, kir::Interp::Smooth);
        const int  tex  = fg.texture(0, 16, DType::F32, kir::TexDim::Tex2D, false, false, false, 4); // +velocity (idx 3)
        const int  samp = fg.sampler(0, 2, false);
        // ⛔ NO buffer_decl: a reachable `storage_load` auto-declares the binding-0 `StorageBuf { uint data[]; }
        // sbuf` (ckir_glsl); an explicit buffer_decl would emit a SECOND buffer at binding 0 and the shader would
        // not compile. The renderer's `taa_constants` binds to that binding-0 slot via draw_bindless_storage.
        const auto sh   = kir::make_shape({1});
        const auto fk   = [&](double v) { return fg.constant(v, sh, DType::F32); };
        const auto ku   = [&](crd::u32 v) { return fg.constant(static_cast<double>(v), sh, DType::U32); };
        const auto add  = [&](int a, int b) { return fg.binary(KOp::Add, a, b); };
        const auto sub  = [&](int a, int b) { return fg.binary(KOp::Sub, a, b); };
        const auto mul  = [&](int a, int b) { return fg.binary(KOp::Mul, a, b); };
        const auto dvd  = [&](int a, int b) { return fg.binary(KOp::Div, a, b); };
        const auto sw   = [&](int v, int c) { return fg.swizzle(v, c); };
        // the constants buffer (binding 0): R (reproject matrix, column-major words 0..15), inv_res.xy (16,17),
        // feedback (18), has_history (19). storage_load reads binding 0 as u32 → reinterpret as f32.
        const auto rload = [&](crd::u32 i) { return fg.int_bits_to_float(fg.cast(fg.storage_load(ku(i)), DType::I32)); };
        const int  inv_rx = rload(16U);
        const int  inv_ry = rload(17U);
        const int  feedbk = rload(18U);
        const int  has_h  = rload(19U);

        // ── sample the CURRENT colour + a 3×3 neighbourhood, tracking the per-channel min/max box (the variance
        // clamp that rejects ghosting: reprojected history is clamped into this box). ──
        const int cur = fg.tex_sample_at(tex, samp, uv, ku(0U));
        int cmin = cur;
        int cmax = cur;
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0) { continue; }
                const int ouv = fg.vec2(add(sw(uv, 0), mul(fk(static_cast<double>(dx)), inv_rx)),
                                        add(sw(uv, 1), mul(fk(static_cast<double>(dy)), inv_ry)));
                const int s   = fg.tex_sample_at(tex, samp, ouv, ku(0U));
                cmin = fg.binary(KOp::Min, cmin, s);
                cmax = fg.binary(KOp::Max, cmax, s);
            }
        }

        // ── reproject: reconstruct clip from (uv, depth) and multiply by R = prev_vp·inv(cur_vp). The
        // perspective-divide makes the implicit w cancel, so v = (ndc.x, ndc.y, depth, 1) is exact. ──
        // ⭐⭐⭐ REN-41 (NDC±Y): the uv→ndc.y and ndc.y→prev_uv.v conversions carry the BACKEND'S clip-Y SIGN. The
        // fullscreen VS is clip-Y-flipped on a y-up backend (DX12) so the resample is an identity, which means a
        // texel's true ndc.y is `1 − 2·v` there, not `2·v − 1`. Reconstructing with the wrong sign leaves the
        // history reprojection MIRRORED in Y — samples slide against the current frame as the camera pans (the
        // "textures piling on top of each other"). `sgn = +1` on a y-down backend (Vulkan, unchanged), `−1` on
        // y-up (DX12), applied to BOTH the ndc.y we feed R and the prev_uv.v we read history at.
        const double sgn = raster->ndc_y_points_down() ? 1.0 : -1.0;
        const int depth = sw(fg.tex_sample_at(tex, samp, uv, ku(1U)), 0);
        const int ndcx  = sub(mul(sw(uv, 0), fk(2.0)), fk(1.0));
        const int ndcy  = mul(fk(sgn), sub(mul(sw(uv, 1), fk(2.0)), fk(1.0)));
        const int vv[4] = {ndcx, ndcy, depth, fk(1.0)};
        int prow[4];
        for (int row = 0; row < 4; ++row)
        {
            int acc = mul(rload(static_cast<crd::u32>(0 * 4 + row)), vv[0]);
            acc     = add(acc, mul(rload(static_cast<crd::u32>(1 * 4 + row)), vv[1]));
            acc     = add(acc, mul(rload(static_cast<crd::u32>(2 * 4 + row)), vv[2]));
            acc     = add(acc, mul(rload(static_cast<crd::u32>(3 * 4 + row)), vv[3]));
            prow[row] = acc;
        }
        const int iw      = dvd(fk(1.0), fg.binary(KOp::Max, prow[3], fk(1.0e-6)));
        const int puvx    = add(mul(mul(prow[0], iw), fk(0.5)), fk(0.5));
        const int puvy    = add(mul(mul(mul(prow[1], iw), fk(0.5)), fk(sgn)), fk(0.5)); // REN-41 NDC±Y: 0.5 + sgn·0.5·ndc.y'
        // ⭐⭐ REN-41 (velocity): add the PER-OBJECT motion delta (bindless index 3, in current-camera UV space).
        // The R matrix above reprojects the CAMERA motion assuming a static surface; velocity carries the object's
        // own screen displacement. A static instance stores 0 → prev_uv is the exact camera reproject, unchanged
        // from before this row (so every static pixel — the 1M majority — is byte-identical). A mover fetches the
        // history texel its surface actually came from, so the neighbourhood clamp no longer has to reject it.
        const int velv    = fg.tex_sample_at(tex, samp, uv, ku(3U));
        const int prev_uv = fg.vec2(add(puvx, sw(velv, 0)), add(puvy, sw(velv, 1)));

        // ── history sample. ⛔ NOT bilinear: resampling the history bilinearly every frame is a low-pass that
        // compounds into the "TAA blur". A 5-tap CATMULL-ROM reconstruction (Karis) is sharp — it has negative
        // lobes that restore high frequency — and costs 5 bilinear taps. Weights are per-axis; the 5 taps drop
        // the 4 corners and the result is renormalised by the tap-weight sum so edges neither darken nor bloom. ──
        const int res_x = dvd(fk(1.0), fg.binary(KOp::Max, inv_rx, fk(1.0e-9)));
        const int res_y = dvd(fk(1.0), fg.binary(KOp::Max, inv_ry, fk(1.0e-9)));
        const auto hist_catmull = [&](int puv) {
            const int spx = mul(sw(puv, 0), res_x);
            const int spy = mul(sw(puv, 1), res_y);
            const int t1x = add(fg.unary(KOp::Floor, sub(spx, fk(0.5))), fk(0.5));
            const int t1y = add(fg.unary(KOp::Floor, sub(spy, fk(0.5))), fk(0.5));
            const int fx  = sub(spx, t1x);
            const int fy  = sub(spy, t1y);
            // Catmull-Rom weights {w0,w1,w2,w3} for a fractional offset t (all scalar ops)
            const auto w0 = [&](int t) { return mul(t, add(fk(-0.5), mul(t, add(fk(1.0), mul(fk(-0.5), t))))); };
            const auto w1 = [&](int t) { return add(fk(1.0), mul(mul(t, t), add(fk(-2.5), mul(fk(1.5), t)))); };
            const auto w2 = [&](int t) { return mul(t, add(fk(0.5), mul(t, add(fk(2.0), mul(fk(-1.5), t))))); };
            const auto w3 = [&](int t) { return mul(mul(t, t), add(fk(-0.5), mul(fk(0.5), t))); };
            const int w0x  = w0(fx);
            const int w1x  = w1(fx);
            const int w2x  = w2(fx);
            const int w3x  = w3(fx);
            const int w0y  = w0(fy);
            const int w1y  = w1(fy);
            const int w2y  = w2(fy);
            const int w3y  = w3(fy);
            const int w12x = add(w1x, w2x);
            const int w12y = add(w1y, w2y);
            const int t0x  = mul(sub(t1x, fk(1.0)), inv_rx);
            const int t3x  = mul(add(t1x, fk(2.0)), inv_rx);
            const int t12x = mul(add(t1x, dvd(w2x, w12x)), inv_rx);
            const int t0y  = mul(sub(t1y, fk(1.0)), inv_ry);
            const int t3y  = mul(add(t1y, fk(2.0)), inv_ry);
            const int t12y = mul(add(t1y, dvd(w2y, w12y)), inv_ry);
            const auto smp = [&](int ux, int uy) { return fg.tex_sample_at(tex, samp, fg.vec2(ux, uy), ku(2U)); };
            const int wa   = mul(w12x, w0y);
            const int wb   = mul(w0x, w12y);
            const int wc   = mul(w12x, w12y);
            const int wd   = mul(w3x, w12y);
            const int we   = mul(w12x, w3y);
            int acc = mul(smp(t12x, t0y), wa);       // vec4 * scalar, vec first
            acc     = add(acc, mul(smp(t0x, t12y), wb));
            acc     = add(acc, mul(smp(t12x, t12y), wc));
            acc     = add(acc, mul(smp(t3x, t12y), wd));
            acc     = add(acc, mul(smp(t12x, t3y), we));
            const int wsum = add(add(add(add(wa, wb), wc), wd), we);
            return mul(acc, dvd(fk(1.0), fg.binary(KOp::Max, wsum, fk(1.0e-5)))); // renormalise
        };
        const int hist  = hist_catmull(prev_uv);
        const int hclmp = fg.ternary(KOp::Clamp, hist, cmin, cmax);
        // in-bounds test: 0 ≤ puv ≤ 1 on both axes
        const auto in01 = [&](int c) { return mul(fg.binary(KOp::Step, fk(0.0), c), fg.binary(KOp::Step, c, fk(1.0))); };
        int gate = mul(in01(puvx), in01(puvy));
        gate     = mul(gate, has_h);                                   // no history on frame 0 ⇒ current
        const int w = mul(gate, feedbk);
        const int outc = add(cur, mul(sub(hclmp, cur), w));            // mix(cur, hclmp, w)
        const int outv = fg.vec4(sw(outc, 0), sw(outc, 1), sw(outc, 2), fk(1.0));

        kir::KEntry fe;
        fe.stage  = kir::KStage::Fragment;
        fe.n_out  = 1;
        fe.out[0] = {outv, 0};
        // ⛔ LOWER before create_program: the resolve uses high-level ops (Clamp/Step/Min/Max/Div) the backend
        // emitters expect already lowered (the impostor FS does the same). The trivial passthrough skipped it and
        // still cooked; this full body does not.
        kir::lower::lower_entry(fg, fe);
        std::unique_ptr<crd::gpu::IGpuProgram> pfs = ctx->create_program(fg, fe);
        if (pfs == nullptr) { return nullptr; }
        prog_taa = raster->create_raster_program(*pvs, *pfs);
        adv_stages.push_back(std::move(pfs));
        return prog_taa.get();
    }
    [[nodiscard]] crd::gpu::IStorageBuffer* ensure_taa_constants()
    {
        if (taa_constants == nullptr && raster != nullptr)
        {
            taa_constants = raster->create_storage_buffer(24U * 4U); // 16 (R) + 8 scalar/pad
        }
        return taa_constants.get();
    }

    // kind: 0 = convert, 1 = blur_x, 2 = blur_y.
    std::unique_ptr<crd::gpu::IRasterProgram> moment_prog[3][kMaxCascades];
    [[nodiscard]] crd::gpu::IRasterProgram* ensure_moment_program(crd::u32 kind, crd::u32 index)
    {
        if (kind >= 3U || index >= kMaxCascades) { return nullptr; }
        std::unique_ptr<crd::gpu::IRasterProgram>& slot = moment_prog[kind][index];
        if (slot != nullptr) { return slot.get(); }
        if (raster == nullptr || ctx == nullptr || soft_mode < 2U) { return nullptr; }
        crd::gpu::IGpuProgram* pvs = cook_stage_named("vertex/post_fullscreen.crdv");
        if (pvs == nullptr) { return nullptr; }
        crd::kir::KGraph fg(alloc);
        int              out = -1;
        if (kind == 0U)
        {
            out = crd::kir::technique::body_moment_convert(fg, static_cast<int>(soft_mode), index);
        }
        else
        {
            // ⛔ 1/map_size from the SAME CsmConfig the cascade passes render with — cook AFTER set_csm_config,
            // which the setter contract already requires for every shadow program.
            const double inv = 1.0 / static_cast<double>(csm.map_size > 0U ? csm.map_size : 2048U);
            out = crd::kir::technique::body_moment_blur(fg, kind == 1U, index, inv);
        }
        if (out < 0) { return nullptr; }
        crd::kir::KEntry fe;
        fe.stage  = crd::kir::KStage::Fragment;
        fe.n_out  = 1;
        fe.out[0] = {out, 0};
        std::unique_ptr<crd::gpu::IGpuProgram> pfs = ctx->create_program(fg, fe);
        if (pfs == nullptr) { return nullptr; }
        slot = raster->create_raster_program(*pvs, *pfs);
        adv_stages.push_back(std::move(pfs));
        return slot.get();
    }

    // ── ⭐ REN-40-G3: HZB BUILD program — half-res MIN reduction of scene depth. ────────────────────────────
    // textureGather fetches the 2×2 bilinear footprint → min of 4 → output. The fullscreen VS is shared.
    std::unique_ptr<crd::gpu::IRasterProgram> prog_hzb;
    [[nodiscard]] crd::gpu::IRasterProgram* ensure_hzb_program()
    {
        if (prog_hzb != nullptr) { return prog_hzb.get(); }
        if (raster == nullptr || ctx == nullptr) { return nullptr; }
        crd::gpu::IGpuProgram* pvs = cook_stage_named("vertex/post_fullscreen.crdv");
        if (pvs == nullptr) { return nullptr; }
        crd::kir::KGraph fg(alloc);
        const int        out = crd::kir::technique::body_hzb_build(fg);
        if (out < 0) { return nullptr; }
        const auto sh1  = crd::kir::make_shape({1});
        const int  onef = fg.constant(1.0, sh1, crd::kir::DType::F32);
        crd::kir::KEntry fe;
        fe.stage  = crd::kir::KStage::Fragment;
        fe.n_out  = 1;
        fe.out[0] = {fg.vec4(out, out, out, onef), 0};
        std::unique_ptr<crd::gpu::IGpuProgram> pfs = ctx->create_program(fg, fe);
        if (pfs == nullptr) { return nullptr; }
        prog_hzb = raster->create_raster_program(*pvs, *pfs);
        adv_stages.push_back(std::move(pfs));
        return prog_hzb.get();
    }

    // ── ⭐⭐ REN-41 (velocity): the MOTION-VECTOR DEBUG VIEW (crd://scene/velocity_debug). ────────────────────
    // A fullscreen pass that samples the `velocity` buffer (bindless index 0 — the pass's SOLE read) and encodes
    // the signed UV motion delta into a viewable RGBA8 image: `out.rg = velocity.xy * kVelocityDebugScale + 0.5`
    // (0 motion → mid-grey; either sign visible), `out.b = 0.5`, `out.a = 1`. This is the readback surface the
    // REN-41 velocity CORRECTNESS gate decodes — a static instance encodes ~0.5 while a mover encodes 0.5 + its
    // expected screen delta — and a usable motion-vector overlay for any app authoring a `velocity_debug` frame.
    // The fullscreen VS carries the same clip-Y flip as the TAA resolve so the resample is an IDENTITY on a y-up
    // backend (DX12): output pixel == velocity-buffer pixel == the geometry's screen pixel, on both backends.
    std::unique_ptr<crd::gpu::IRasterProgram> prog_velocity_debug;
    [[nodiscard]] crd::gpu::IRasterProgram* ensure_velocity_debug_program()
    {
        if (prog_velocity_debug != nullptr) { return prog_velocity_debug.get(); }
        if (raster == nullptr || ctx == nullptr) { return nullptr; }
        crd::gpu::IGpuProgram* pvs = cook_stage_named("vertex/post_fullscreen.crdv", !raster->ndc_y_points_down());
        if (pvs == nullptr) { return nullptr; }
        namespace kir = crd::kir;
        using DType   = kir::DType;
        using KOp     = kir::KOp;
        kir::KGraph fg(alloc);
        const auto sh   = kir::make_shape({1});
        const auto fk   = [&](double v) { return fg.constant(v, sh, DType::F32); };
        const int  uv   = fg.stage_in(kir::KType::vec(DType::F32, 2), 0, kir::Interp::Smooth);
        // ⛔ A fullscreen pass that declares exactly ONE read is bound by the executor's single-texture path
        // (`draw_textured`) — the texture at set 0 / binding 1, the sampler at binding 2 — NOT the bindless heap at
        // binding 16 the multi-read TAA resolve uses. Sample it the way `body_hzb_build` (the other 1-read
        // fullscreen) does, or the FS reads an unbound descriptor and the encode comes out black.
        const int  tex  = fg.texture(0, 1, DType::F32, kir::TexDim::Tex2D, false, false, false);
        const int  samp = fg.sampler(0, 2, false);
        const int  v    = fg.tex_sample(tex, samp, uv);
        const int  scl  = fk(static_cast<double>(crd::scenerender::kVelocityDebugScale));
        const int  half = fk(0.5);
        const int  ex = fg.binary(KOp::Add, fg.binary(KOp::Mul, fg.swizzle(v, 0), scl), half);
        const int  ey = fg.binary(KOp::Add, fg.binary(KOp::Mul, fg.swizzle(v, 1), scl), half);
        kir::KEntry fe;
        fe.stage  = kir::KStage::Fragment;
        fe.n_out  = 1;
        fe.out[0] = {fg.vec4(ex, ey, half, fk(1.0)), 0};
        std::unique_ptr<crd::gpu::IGpuProgram> pfs = ctx->create_program(fg, fe);
        if (pfs == nullptr) { return nullptr; }
        prog_velocity_debug = raster->create_raster_program(*pvs, *pfs);
        adv_stages.push_back(std::move(pfs));
        return prog_velocity_debug.get();
    }

    // ── ⭐⭐ REN-40-C5: the IMPOSTOR BILLBOARD program — camera-facing quad from the bounding sphere. ─────────
    // VertexIndex (0-5 from the identity IB) selects the quad corner; InstanceIndex indexes the impostor slot's
    // visible list. The VS reads the world AABB from the bounds section, builds a camera-facing quad, and
    // outputs octahedral UVs for the atlas lookup in the FS. The FS is a placeholder (C5.5 replaces it).
    std::unique_ptr<crd::gpu::IRasterProgram> prog_impostor;
    [[nodiscard]] crd::gpu::IRasterProgram* ensure_impostor_program()
    {
        if (prog_impostor != nullptr) { return prog_impostor.get(); }
        if (ctx == nullptr || raster == nullptr) { return nullptr; }

        namespace kir = crd::kir;
        using DType   = kir::DType;
        using KOp     = kir::KOp;
        const auto sh = kir::make_shape({1});

        // ── VS: the billboard vertex program ──────────────────────────────────
        kir::KGraph vg(alloc);
        kir::KEntry ve;

        const auto kf  = [&](double v) { return vg.constant(v, sh, DType::F32); };
        const auto ku  = [&](crd::u32 v) { return vg.constant(static_cast<double>(v), sh, DType::U32); };
        const auto vadd = [&](int a, int b) { return vg.binary(KOp::Add, a, b); };
        const auto vsub = [&](int a, int b) { return vg.binary(KOp::Sub, a, b); };
        const auto vmul = [&](int a, int b) { return vg.binary(KOp::Mul, a, b); };
        const auto vdiv = [&](int a, int b) { return vg.binary(KOp::Div, a, b); };
        int sbase = -1;
        const auto sloadu = [&](int idx) {
            return vg.storage_load(sbase >= 0 ? vadd(sbase, idx) : idx);
        };
        const auto sloadf = [&](int idx) {
            return vg.int_bits_to_float(vg.cast(sloadu(idx), DType::I32));
        };
        const auto shdru = [&](crd::u32 w) { return sloadu(ku(w)); };
        const auto shdrf = [&](crd::u32 w) { return sloadf(ku(w)); };

        // rebase from the IMPOSTOR draw table (separate from the mesh table — see kImpostorTableOff)
        const int di  = vg.cast(vg.builtin(kir::KBuiltin::DrawIndex), DType::U32);
        const int row = vmul(di, ku(kSceneDrawRowWords));
        sbase         = sloadu(vadd(ku(kImpostorTableOff), row));
        const int row_slot = sloadu(vadd(vadd(ku(kImpostorTableOff), row), ku(1U)));

        // corner from VertexIndex (0-5, the identity IB)
        // Two CCW triangles: (-1,-1),(1,-1),(1,1), (-1,-1),(1,1),(-1,1)
        const int vid = vg.cast(vg.builtin(kir::KBuiltin::VertexIndex), DType::U32);
        const int eq0 = vg.binary(KOp::CmpEq, vid, ku(0U));
        const int eq1 = vg.binary(KOp::CmpEq, vid, ku(1U));
        const int eq2 = vg.binary(KOp::CmpEq, vid, ku(2U));
        const int eq3 = vg.binary(KOp::CmpEq, vid, ku(3U));
        const int eq4 = vg.binary(KOp::CmpEq, vid, ku(4U));
        const int cx  = vg.select(eq0, kf(-1.0), vg.select(eq1, kf(1.0), vg.select(eq2, kf(1.0),
                        vg.select(eq3, kf(-1.0), vg.select(eq4, kf(1.0), kf(-1.0))))));
        const int cy  = vg.select(eq0, kf(-1.0), vg.select(eq1, kf(-1.0), vg.select(eq2, kf(1.0),
                        vg.select(eq3, kf(-1.0), vg.select(eq4, kf(1.0), kf(1.0))))));

        // InstanceIndex → visible list → instance_id
        const int ii  = vg.cast(vg.builtin(kir::KBuiltin::InstanceIndex), DType::U32);
        const int cap = shdru(kHdrInstanceCapacity);
        const int vis_base_val = vadd(shdru(5U), vmul(cap, row_slot));
        const int raw_vis = sloadu(vadd(vis_base_val, ii));

        const bool has_dither = lod_policy.dither_band > 0.0F && lod_slots > 1U;
        const int instance_id = has_dither
            ? vg.binary(KOp::BitAnd, raw_vis, ku(0x00FFFFFFU))
            : raw_vis;

        // world AABB (6 floats at bounds_off + instance_id * 6)
        const int aabb = vadd(shdru(kHdrBoundsOff), vmul(instance_id, ku(6U)));
        const int bmin_x = sloadf(vadd(aabb, ku(0U)));
        const int bmin_y = sloadf(vadd(aabb, ku(1U)));
        const int bmin_z = sloadf(vadd(aabb, ku(2U)));
        const int bmax_x = sloadf(vadd(aabb, ku(3U)));
        const int bmax_y = sloadf(vadd(aabb, ku(4U)));
        const int bmax_z = sloadf(vadd(aabb, ku(5U)));

        // center and bounding-sphere radius
        const int half  = kf(0.5);
        const int cen_x = vmul(vadd(bmin_x, bmax_x), half);
        const int cen_y = vmul(vadd(bmin_y, bmax_y), half);
        const int cen_z = vmul(vadd(bmin_z, bmax_z), half);
        const int ext_x = vmul(vsub(bmax_x, bmin_x), half);
        const int ext_y = vmul(vsub(bmax_y, bmin_y), half);
        const int ext_z = vmul(vsub(bmax_z, bmin_z), half);
        const int r2    = vadd(vadd(vmul(ext_x, ext_x), vmul(ext_y, ext_y)), vmul(ext_z, ext_z));
        const int rad   = vg.unary(KOp::Sqrt, vadd(r2, kf(1.0e-12)));

        // camera position (header word 96)
        const int cam_x = shdrf(kHdrCameraPos);
        const int cam_y = shdrf(kHdrCameraPos + 1U);
        const int cam_z = shdrf(kHdrCameraPos + 2U);

        // view direction (centre → camera), normalised
        const int vdx   = vsub(cam_x, cen_x);
        const int vdy   = vsub(cam_y, cen_y);
        const int vdz   = vsub(cam_z, cen_z);
        const int vdl   = vg.unary(KOp::Sqrt, vadd(vadd(vmul(vdx, vdx), vmul(vdy, vdy)),
                                                    vadd(vmul(vdz, vdz), kf(1.0e-12))));
        const int fw_x  = vdiv(vdx, vdl);
        const int fw_y  = vdiv(vdy, vdl);
        const int fw_z  = vdiv(vdz, vdl);

        // billboard axes: right = normalize(cross((0,1,0), fwd)) = normalize(fwd_z, 0, -fwd_x)
        const int rx_raw = fw_z;
        const int rz_raw = vg.unary(KOp::Neg, fw_x);
        const int rl     = vg.unary(KOp::Sqrt, vadd(vadd(vmul(rx_raw, rx_raw), vmul(rz_raw, rz_raw)), kf(1.0e-12)));
        const int rx     = vdiv(rx_raw, rl);
        const int rz     = vdiv(rz_raw, rl);
        // up = cross(fwd, right) — right.y=0 by construction, so:
        //   up.x = fw_y*rz,  up.y = fw_z*rx - fw_x*rz,  up.z = -fw_y*rx
        const int ux = vmul(fw_y, rz);
        const int uy = vsub(vmul(fw_z, rx), vmul(fw_x, rz));
        const int uz = vg.unary(KOp::Neg, vmul(fw_y, rx));

        // world pos = centre + cx*radius*right + cy*radius*up
        const int cr  = vmul(cx, rad);
        const int cu  = vmul(cy, rad);
        const int wx  = vadd(cen_x, vadd(vmul(cr, rx), vmul(cu, ux)));
        const int wy  = vadd(cen_y, vmul(cu, uy));
        const int wz  = vadd(cen_z, vadd(vmul(cr, rz), vmul(cu, uz)));

        // clip = view_proj * (wx, wy, wz, 1)
        int vp[16];
        for (crd::u32 e = 0; e < 16U; ++e) { vp[e] = shdrf(6U + e); }
        int clip[4];
        {
            const int v_arr[4] = {wx, wy, wz, kf(1.0)};
            for (crd::u32 i = 0; i < 4U; ++i)
            {
                int acc = vmul(vp[0U * 4U + i], v_arr[0]);
                acc     = vadd(acc, vmul(vp[1U * 4U + i], v_arr[1]));
                acc     = vadd(acc, vmul(vp[2U * 4U + i], v_arr[2]));
                acc     = vadd(acc, vmul(vp[3U * 4U + i], v_arr[3]));
                clip[i] = acc;
            }
        }

        // octahedral UV: oct_encode(view direction) → [0,1]²
        const int abs_x  = vg.unary(KOp::Abs, fw_x);
        const int abs_y  = vg.unary(KOp::Abs, fw_y);
        const int abs_z  = vg.unary(KOp::Abs, fw_z);
        const int s_inv  = vdiv(kf(1.0), vadd(vadd(abs_x, abs_y), vadd(abs_z, kf(1.0e-12))));
        const int px     = vmul(fw_x, s_inv);
        const int py     = vmul(fw_y, s_inv);
        const int z_neg  = vg.binary(KOp::CmpLt, fw_z, kf(0.0));
        const int fold_x = vmul(vsub(kf(1.0), vg.unary(KOp::Abs, py)), vg.unary(KOp::Sign, px));
        const int fold_y = vmul(vsub(kf(1.0), vg.unary(KOp::Abs, px)), vg.unary(KOp::Sign, py));
        const int ox     = vg.select(z_neg, fold_x, px);
        const int oy     = vg.select(z_neg, fold_y, py);
        const int uv_x   = vmul(vadd(ox, kf(1.0)), half);
        const int uv_y   = vmul(vadd(oy, kf(1.0)), half);

        // ⭐⭐ REN-40-C5.5: per-vertex ATLAS PIXEL COORDINATES — the varying that makes the impostor
        // TEXTURED. oct_uv is constant across the quad (one view direction per instance), so passing it
        // as a smooth varying would sample the same texel at every pixel. Instead, each quad corner maps
        // to the corresponding corner of its tile: smooth interpolation then gives each fragment its own
        // texel address in the atlas.
        const int dims    = shdru(kHdrAtlasDims);
        const int grid_u  = vg.binary(KOp::Shr, dims, ku(16U));
        const int tile_u  = vg.binary(KOp::BitAnd, dims, ku(0xFFFFU));
        const int grid_f  = vg.cast(grid_u, DType::F32);
        const int tile_f  = vg.cast(tile_u, DType::F32);
        const int grid_m1 = vsub(grid_f, kf(1.0));
        const int tile_col = vg.ternary(KOp::Clamp, vg.unary(KOp::Floor, vmul(uv_x, grid_f)),
                                        kf(0.0), grid_m1);
        const int tile_row = vg.ternary(KOp::Clamp, vg.unary(KOp::Floor, vmul(uv_y, grid_f)),
                                        kf(0.0), grid_m1);
        const int quad_u_v = vmul(vadd(cx, kf(1.0)), half);
        const int quad_v_v = vmul(vadd(cy, kf(1.0)), half);
        const int apx_x   = vmul(vadd(tile_col, quad_u_v), tile_f);
        const int apx_y   = vmul(vadd(tile_row, quad_v_v), tile_f);

        // tint from instance colour (4 floats at instances_off + id*20 + 16)
        const int ibase = vadd(shdru(4U), vmul(instance_id, ku(kInstanceWords)));
        const int tint_r = sloadf(vadd(ibase, ku(16U)));
        const int tint_g = sloadf(vadd(ibase, ku(17U)));
        const int tint_b = sloadf(vadd(ibase, ku(18U)));
        const int tint_a = sloadf(vadd(ibase, ku(19U)));

        int fade = kf(1.0);
        if (has_dither)
        {
            const int m255 = ku(0xFFU);
            fade = vdiv(vg.cast(vg.binary(KOp::BitAnd, vg.binary(KOp::Shr, raw_vis, ku(24U)), m255), DType::F32),
                        kf(255.0));
        }

        // atlas base: absolute buffer word offset of the first atlas texel
        const int atlas_abs   = vadd(sbase, shdru(kHdrAtlasOff));
        const int atlas_abs_f = vg.int_bits_to_float(vg.cast(atlas_abs, DType::I32));

        ve.stage    = kir::KStage::Vertex;
        ve.position = vg.vec4(clip[0], clip[1], clip[2], clip[3]);
        ve.out[0]   = {vg.vec2(apx_x, apx_y), 0, kir::Interp::Smooth};
        ve.out[1]   = {vg.vec4(tint_r, tint_g, tint_b, tint_a), 1, kir::Interp::Flat};
        if (has_dither)
        {
            ve.n_out  = 4;
            ve.out[2] = {fade, 2, kir::Interp::Flat};
            ve.out[3] = {atlas_abs_f, 3, kir::Interp::Flat};
        }
        else
        {
            ve.n_out  = 3;
            ve.out[2] = {atlas_abs_f, 2, kir::Interp::Flat};
        }

        auto vs_prog = ctx->create_program(vg, ve);
        if (vs_prog == nullptr) { return nullptr; }

        // ── FS: atlas sample + coverage discard + Bayer dither ────────────────
        kir::KGraph fg(alloc);
        kir::KEntry fe;
        fe.stage = kir::KStage::Fragment;
        const int si_uv    = fg.stage_in(kir::KType::vec(DType::F32, 2), 0, kir::Interp::Smooth);
        const int si_tint  = fg.stage_in(kir::KType::vec(DType::F32, 4), 1, kir::Interp::Flat);
        const int si_fade  = has_dither ? fg.stage_in(kir::KType::make_scalar(DType::F32), 2, kir::Interp::Flat) : -1;
        const int si_abase = fg.stage_in(kir::KType::make_scalar(DType::F32), has_dither ? 3 : 2, kir::Interp::Flat);
        const auto fk  = [&](double v) { return fg.constant(v, sh, DType::F32); };
        const auto fku = [&](crd::u32 v) { return fg.constant(static_cast<double>(v), sh, DType::U32); };
        const auto fki = [&](int v) { return fg.constant(static_cast<double>(v), sh, DType::I32); };
        const auto fadd = [&](int a, int b) { return fg.binary(KOp::Add, a, b); };
        const auto fmul = [&](int a, int b) { return fg.binary(KOp::Mul, a, b); };

        // recover atlas_base as u32
        const int abase = fg.cast(fg.float_bits_to_int(si_abase), DType::U32);

        // ── ⭐⭐ REN-41: TRILINEAR MIP sampling of the prefiltered octahedral atlas. An impostor is drawn only
        // when the object is tiny on screen, so its `tile`-texel view is always heavily MINIFIED — point/bilinear
        // sampling of a large source into a few pixels is exactly the far-field shimmer. The atlas is now a mip
        // pyramid (impostor_atlas.hpp); the FS picks the level whose texel matches the screen via `fwidth` of the
        // atlas coordinate (the same quantity hardware mip selection uses) and blends two levels trilinearly.
        // ⛔ grid/tile are COOK-TIME constants here, so the per-level offsets unroll to a select chain.
        const crd::u32 grid = lod_policy.impostor_grid;
        const crd::u32 tile = lod_policy.impostor_tile;
        const crd::u32 mips = crd::lod::impostor_num_mips(tile);
        const int gt_f  = fk(static_cast<double>(grid * tile)); // level-0 atlas edge, texels
        const int r255  = fk(1.0 / 255.0);

        // level-0 atlas coords (smooth-interpolated from the VS quad corners → per-fragment)
        const int apx_x_f = fg.swizzle(si_uv, 0);
        const int apx_y_f = fg.swizzle(si_uv, 1);
        // mip = log2(level-0 texels per screen pixel), clamped to the pyramid; trilinear over floor/ceil.
        const int fw    = fg.binary(KOp::Max, fg.unary(KOp::Fwidth, apx_x_f), fg.unary(KOp::Fwidth, apx_y_f));
        const int mip_f = fg.ternary(KOp::Clamp, fg.unary(KOp::Log2, fg.binary(KOp::Max, fw, fk(1.0e-6))),
                                     fk(0.0), fk(static_cast<double>(mips - 1U)));
        const int m0_f  = fg.unary(KOp::Floor, mip_f);
        const int mfrac = fg.binary(KOp::Sub, mip_f, m0_f);
        const int m1_f  = fg.binary(KOp::Min, fadd(m0_f, fk(1.0)), fk(static_cast<double>(mips - 1U)));

        const auto chan = [&](int rgba8, crd::u32 ch) {
            const int shifted = ch == 0U ? rgba8 : fg.binary(KOp::Shr, rgba8, fku(ch * 8U));
            return fmul(fg.cast(fg.binary(KOp::BitAnd, shifted, fku(0xFFU)), DType::F32), r255);
        };
        const auto mix = [&](int a, int b, int t) { return fadd(a, fmul(fg.binary(KOp::Sub, b, a), t)); };

        // bilinear sample of level `mlevel_f` (a float holding an integer level index 0..mips-1). Returns the
        // four channels in `out4`. The level's texel offset comes from a select chain over the cook-time
        // constants; its dimension is `grid*tile / 2^m` (exact, tile is a power of two) and the level-0 coords
        // scale by the same `2^-m`.
        const auto sample_level = [&](int mlevel_f, int out4[4]) {
            const int inv   = fg.unary(KOp::Exp2, fg.unary(KOp::Neg, mlevel_f)); // 2^-m
            const int dimf  = fmul(gt_f, inv);                                    // texels per side at this level
            const int dm1   = fg.binary(KOp::Sub, dimf, fk(1.0));
            const int mi_u  = fg.cast(fadd(mlevel_f, fk(0.5)), DType::U32);       // the level as a u32
            int       off   = fku(crd::lod::impostor_level_offset(grid, tile, mips - 1U));
            for (crd::u32 k = mips; k-- > 0U;)
            {
                off = fg.select(fg.binary(KOp::CmpEq, mi_u, fku(k)),
                                fku(crd::lod::impostor_level_offset(grid, tile, k)), off);
            }
            const int abs_off = fg.binary(KOp::Add, abase, off);
            const int lx  = fmul(apx_x_f, inv);
            const int ly  = fmul(apx_y_f, inv);
            const int x0f = fg.ternary(KOp::Clamp, fg.unary(KOp::Floor, fg.binary(KOp::Sub, lx, fk(0.5))), fk(0.0), dm1);
            const int y0f = fg.ternary(KOp::Clamp, fg.unary(KOp::Floor, fg.binary(KOp::Sub, ly, fk(0.5))), fk(0.0), dm1);
            const int x1f = fg.ternary(KOp::Clamp, fadd(x0f, fk(1.0)), fk(0.0), dm1);
            const int y1f = fg.ternary(KOp::Clamp, fadd(y0f, fk(1.0)), fk(0.0), dm1);
            const int wx  = fg.ternary(KOp::Clamp, fg.binary(KOp::Sub, fg.binary(KOp::Sub, lx, fk(0.5)), x0f), fk(0.0), fk(1.0));
            const int wy  = fg.ternary(KOp::Clamp, fg.binary(KOp::Sub, fg.binary(KOp::Sub, ly, fk(0.5)), y0f), fk(0.0), fk(1.0));
            const int dimu = fg.cast(dimf, DType::U32);
            const int x0 = fg.cast(x0f, DType::U32);
            const int y0 = fg.cast(y0f, DType::U32);
            const int x1 = fg.cast(x1f, DType::U32);
            const int y1 = fg.cast(y1f, DType::U32);
            const auto texel = [&](int x, int y) {
                return fg.storage_load(fg.binary(KOp::Add, abs_off,
                                                 fg.binary(KOp::Add, fg.binary(KOp::Mul, y, dimu), x)));
            };
            const int t00 = texel(x0, y0);
            const int t10 = texel(x1, y0);
            const int t01 = texel(x0, y1);
            const int t11 = texel(x1, y1);
            for (crd::u32 ch = 0; ch < 4U; ++ch)
            {
                const int top = mix(chan(t00, ch), chan(t10, ch), wx);
                const int bot = mix(chan(t01, ch), chan(t11, ch), wx);
                out4[ch] = mix(top, bot, wy);
            }
        };
        int lo4[4];
        int hi4[4];
        sample_level(m0_f, lo4);
        sample_level(m1_f, hi4);
        const int tr = mix(lo4[0], hi4[0], mfrac);
        const int tg = mix(lo4[1], hi4[1], mfrac);
        const int tb = mix(lo4[2], hi4[2], mfrac);
        const int ta = mix(lo4[3], hi4[3], mfrac);

        // coverage discard: atlas alpha < 0.5 → kill (transparent atlas texel)
        const int cov_kill = fg.binary(KOp::CmpLt, ta, fk(0.5));

        // tint modulation: albedo × per-instance colour
        const int out_r = fmul(tr, fg.swizzle(si_tint, 0));
        const int out_g = fmul(tg, fg.swizzle(si_tint, 1));
        const int out_b = fmul(tb, fg.swizzle(si_tint, 2));
        const int out_a = fg.swizzle(si_tint, 3);

        fe.out[0] = {fg.vec4(out_r, out_g, out_b, out_a), 0};
        fe.n_out  = 1;

        // dither discard: 4×4 Bayer threshold against fade (same pattern as C4.3)
        if (has_dither)
        {
            const int fc  = fg.builtin(kir::KBuiltin::FragCoord);
            const int fcx = fg.cast(fg.swizzle(fc, 0), DType::I32);
            const int fcy = fg.cast(fg.swizzle(fc, 1), DType::I32);
            const int ix  = fg.binary(KOp::BitAnd, fcx, fki(3));
            const int iy  = fg.binary(KOp::BitAnd, fcy, fki(3));
            const auto brow = [&](double v0, double v1, double v2, double v3) {
                const int r01 = fg.select(fg.binary(KOp::CmpEq, ix, fki(0)), fk(v0/16.0), fk(v1/16.0));
                const int r23 = fg.select(fg.binary(KOp::CmpEq, ix, fki(2)), fk(v2/16.0), fk(v3/16.0));
                return fg.select(fg.binary(KOp::CmpLt, ix, fki(2)), r01, r23);
            };
            const int b0 = brow( 0,  8,  2, 10);
            const int b1 = brow(12,  4, 14,  6);
            const int b2 = brow( 3, 11,  1,  9);
            const int b3 = brow(15,  7, 13,  5);
            const int br01 = fg.select(fg.binary(KOp::CmpEq, iy, fki(0)), b0, b1);
            const int br23 = fg.select(fg.binary(KOp::CmpEq, iy, fki(2)), b2, b3);
            const int threshold = fg.select(fg.binary(KOp::CmpLt, iy, fki(2)), br01, br23);
            const int dither_kill = fg.binary(KOp::CmpLt, si_fade, threshold);
            fe.discard_cond = fg.select(cov_kill, cov_kill, dither_kill);
        }
        else
        {
            fe.discard_cond = cov_kill;
        }

        kir::lower::lower_entry(fg, fe);
        auto fs_prog = ctx->create_program(fg, fe);
        if (fs_prog == nullptr) { return nullptr; }

        prog_impostor = raster->create_raster_program(*vs_prog, *fs_prog);
        adv_stages.push_back(std::move(vs_prog));
        adv_stages.push_back(std::move(fs_prog));
        return prog_impostor.get();
    }

    // ⭐⭐ REN-40-A: cook a CULL asset with THIS BACKEND'S indirect-command layout STAMPED onto the parsed desc.
    // ⛔ The asset carries the Vulkan form (20-byte command, args at 0) as its written default; D3D12 needs 24
    // with the args at 4 behind a DrawIndex root constant. Stamping the DESC — never editing the text — is the
    // same discipline the per-cascade shadow variant uses: the variant is the renderer's pass semantics, the
    // vocabulary is the asset's. A kernel that assumed one layout would write garbage commands on the other
    // backend, which is the clip-space-Y failure shape again.
    [[nodiscard]] crd::gpu::IGpuProgram* cook_cull_stage_named(const char* asset_name, crd::u32 view,
                                                              bool occlusion = false, crd::u32* step = nullptr)
    {
        const auto set = [&](crd::u32 v) { if (step != nullptr) { *step = v; } };
        set(1U);
        if (ctx == nullptr || raster == nullptr) { CRD_LOG_ERROR(g_log_scenerender, "cull cook '{}': ctx or raster null", asset_name); return nullptr; }
        crd::containers::String t(alloc);
        if (!asset_text(asset_name, t)) { set(2U); CRD_LOG_ERROR(g_log_scenerender, "cull cook '{}': asset_text failed", asset_name); return nullptr; }
        set(3U);
        crd::vertcook::VertexProgramDesc desc(alloc);
        crd::containers::String          where(alloc);
        if (crd::vertcook::parse_vertex_toml(crd::containers::StringView(t.c_str(), t.size()), desc, &where)
            != crd::vertcook::VertexCookError::Ok)
        {
            set(4U); CRD_LOG_ERROR(g_log_scenerender, "cull cook '{}': parse failed at '{}'", asset_name, where.c_str()); return nullptr;
        }
        desc.cull.draw_stride  = raster->indirect_command_stride();
        // ⛔ view v's command sits `v * stride` along, so its 5 args start at `arg_off + v * stride`. Baking the
        // VIEW into the offset is what lets one authored asset produce every view's kernel.
        // ⛔ The commands start AFTER the params block (`kCullArgsHeaderWords`), whose word 0 carries the group's
        // buffer base. Every consumer of an args offset adds the same constant: the cook stamp here, the draw
        // items in `fill()`, the cascade expansion, and the counts readback.
        // ⭐⭐ REN-40-C2: a VIEW now owns `lod_slots` consecutive commands, so its first one is `view * slots`
        // along and the kernel walks the rest by the slot it selects.
        desc.cull.draw_arg_off = (kCullArgsHeaderWords * 4U) + raster->indirect_command_arg_offset()
                                 + view * lod_slots * raster->indirect_command_stride();
        desc.cull.base_word    = 0U; // params word 0
        // ⭐⭐ REN-40-C2: the LOD vocabulary, stamped from the ENGINE's header words — never read from the asset
        // text, for the reason the `bounds_off = 104` scar records: a declared word the host does not validate
        // renders a plausible frame off the wrong data. These are validated below alongside the rest.
        desc.cull.lod_slots         = lod_slots;
        desc.cull.lod_count_word    = kHdrLodCount;
        desc.cull.lod_table_word    = kHdrLodTable;
        desc.cull.lod_height_word   = kHdrLodHeight;
        desc.cull.pixel_height_word = kCullArgsPixelHeight + view;
        desc.cull.draw_arg_within   = raster->indirect_command_arg_offset();
        desc.cull.base_row_word     = kCullArgsBaseRow;
        desc.cull.lod_override_off  = kHdrLodOverrideOff;
        desc.cull.dither_band      = lod_enabled ? lod_policy.dither_band : 0.0F;
        // the screen-size culls — ONE mechanism, two thresholds. Both are measured in CAMERA pixels (an
        // instance's relevance is its size ON SCREEN): the camera dispatch culls sub-pixel geometry (pure
        // aliasing energy — nothing smaller than ~a pixel can contribute an image, only shimmer), a cascade
        // dispatch culls casters the atlas cannot resolve into anything but a flickering dot.
        desc.cull.caster_min_px         = view == 0U ? min_draw_px : shadow_caster_min_px;
        desc.cull.cam_pixel_height_word = kCullArgsPixelHeight; // view 0's — the CAMERA's height
        desc.cull.hzb_size_word         = kCullArgsHzbSize;     // conservative occlusion needs the texel dims
        // ⛔⛔ THE DECLARED HEADER WORDS MUST BE THE ENGINE'S HEADER WORDS. An asset naming word 104 for the
        // bounds section (the light record's first word) produced a kernel that tested boxes built from a light
        // colour: it rendered, it reported plausible counts, and the AABBs on the device were bit-identical to
        // the CPU's — because they were never read. A cook-time REFUSAL is the only place this is cheap to catch.
        // ⛔ `bounds_off` only matters to a COMPACTING kernel — the reset never reads a box, and demanding the
        // word from an asset that has no use for it would be a false failure (the first run of this check was
        // exactly that).
        const bool bounds_ok = !desc.cull.compact || desc.cull.bounds_off == kHdrBoundsOff;
        if (!bounds_ok || desc.cull.capacity_word != kHdrInstanceCapacity
            || desc.header.instance_count != kHdrInstanceCount || desc.header.view_proj != 6U
            || desc.header.light_vp != kHdrCsmLightVp || desc.header.visible_off != 5U
            || desc.header.index_off != 2U)
        {
            CRD_LOG_ERROR(g_log_scenerender,
                          "cull asset '{}' declares header words the engine does not use: bounds_off={} (want {}), "
                          "capacity_word={} (want {}), instance_count={} (want {}), view_proj={} (want 6), "
                          "light_vp={} (want {}), visible_off={} (want 5)",
                          asset_name, desc.cull.bounds_off, kHdrBoundsOff, desc.cull.capacity_word,
                          kHdrInstanceCapacity, desc.header.instance_count, kHdrInstanceCount,
                          desc.header.view_proj, desc.header.light_vp, kHdrCsmLightVp, desc.header.visible_off);
            return nullptr;
        }
        desc.cull.view_index   = view;
        desc.cull.views        = 1U + kMaxCascades;
        // ⛔⛔ AND ITS OWN FRUSTUM. View 0 is the camera (`frustum_off == 0` → `header.view_proj`); view c+1 is
        // cascade c, whose clip matrix sits at `light_vp + c*16` — the SAME words `frustum_planes(cascades.light_vp[c])`
        // feeds the CPU cull, which is what makes the two paths comparable at all. Leaving this at 0 made every
        // cascade dispatch cull against the CAMERA: four identical lists, an atlas covering the wrong volume, and a
        // frame with NO SHADOWS while every count matched.
        desc.cull.frustum_off  = view == 0U ? 0U : desc.header.light_vp + ((view - 1U) * 16U);
        desc.cull.occlusion    = occlusion;
        crd::kir::KGraph g(alloc);
        crd::kir::KEntry e;
        if (!crd::vertcook::cook_vertex_program(desc, g, e)) { set(5U); CRD_LOG_ERROR(g_log_scenerender, "cull cook '{}': cook_vertex_program failed (lod_slots={} dither={})", asset_name, desc.cull.lod_slots, desc.cull.dither_band); return nullptr; }
        set(6U);
        std::unique_ptr<crd::gpu::IGpuProgram> p = ctx->create_program(g, e);
        if (p == nullptr) { set(7U); CRD_LOG_ERROR(g_log_scenerender, "cull cook '{}': create_program failed", asset_name); return nullptr; }
        set(8U);
        crd::gpu::IGpuProgram* raw = p.get();
        adv_stages.push_back(std::move(p));
        return raw;
    }

    [[nodiscard]] crd::gpu::IGpuProgram* ensure_cull_view_kernel(crd::u32 view)
    {
        if (view > kMaxCascades) { return nullptr; }
        if (kern_cull_view[view] != nullptr) { return kern_cull_view[view]; }
        kern_cull_view[view] = cook_cull_stage_named("vertex/scene_cull_compact.crdv", view);
        return kern_cull_view[view];
    }

    // The RESET lays down every view's constants in one dispatch, so its own `view` stamp is 0.
    [[nodiscard]] crd::gpu::IGpuProgram* ensure_cull_reset_kernel()
    {
        if (kern_cull_reset != nullptr) { return kern_cull_reset; }
        kern_cull_reset = cook_cull_stage_named("vertex/scene_cull_reset.crdv", 0U);
        return kern_cull_reset;
    }

    // REN-40-G3: the OCCLUSION RE-CULL — same compacting kernel, camera view only, with the HZB test ON.
    [[nodiscard]] crd::gpu::IGpuProgram* ensure_occlusion_cull_kernel()
    {
        if (kern_occlusion_cull != nullptr) { return kern_occlusion_cull; }
        kern_occlusion_cull = cook_cull_stage_named("vertex/scene_cull_compact.crdv", 0U, true, &fill_diag_occ_step);
        return kern_occlusion_cull;
    }

    [[nodiscard]] crd::gpu::IGpuProgram* ensure_cull_kernel()
    {
        if (kern_cull != nullptr) { return kern_cull; }
        kern_cull = cook_stage_named("vertex/scene_cull.crdv");
        return kern_cull;
    }

    // The MARK kernel of the GPU-driven chain: the passthrough cull variant, dispatched INDIRECTLY with the
    // args the frustum cull wrote — workgroup 1, so each surviving slot stamps marks[wgi] = 1 and the CPU
    // never learns the count.
    [[nodiscard]] crd::gpu::IGpuProgram* ensure_cull_mark_kernel()
    {
        if (kern_cull_mark != nullptr) { return kern_cull_mark; }
        kern_cull_mark = cook_stage_named("vertex/scene_cull_mark.crdv");
        return kern_cull_mark;
    }

    // ⭐⭐ REN-40-F: the GPU SKINNING compute kernel — one thread per instance, sequential FK + IBM over joints.
    // Pre-baked uniform-rate TRS clip data is sampled via NLERP (rotation) and Mix (T/S), then composed through
    // the parent chain (topological order guarantees parents[j] < j, so a single forward pass suffices). Two
    // passes: (1) write WORLD matrices to the palette section, (2) multiply by IBM and overwrite.
    [[nodiscard]] crd::gpu::IGpuProgram* ensure_skin_compute_kernel()
    {
        if (kern_skin_compute != nullptr) { return kern_skin_compute; }
        if (ctx == nullptr) { return nullptr; }

        using namespace crd::kir;
        KGraph g(alloc);
        const Shape sh1 = make_shape({1});
        const auto ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
        const auto kf  = [&](crd::f32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::F32); };
        const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
        const auto sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
        const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

        const int buf = g.buffer_decl(DType::U32, 0, 0, true);
        const auto loadu = [&](int idx) { return g.buffer_load(buf, idx); };
        const auto loadf = [&](int idx) { return g.int_bits_to_float(g.cast(loadu(idx), DType::I32)); };
        const auto storeu = [&](int idx, int val) { g.stmt_buffer_store(buf, idx, val); };
        const auto storef = [&](int idx, int val) { storeu(idx, g.cast(g.float_bits_to_int(val), DType::U32)); };

        const auto load_col = [&](int base) -> int {
            return g.vec4(loadf(base), loadf(add(base, ku(1U))),
                          loadf(add(base, ku(2U))), loadf(add(base, ku(3U))));
        };
        const auto store_col = [&](int base, int col) {
            storef(base,               g.swizzle(col, 0));
            storef(add(base, ku(1U)),  g.swizzle(col, 1));
            storef(add(base, ku(2U)),  g.swizzle(col, 2));
            storef(add(base, ku(3U)),  g.swizzle(col, 3));
        };

        const int gid      = g.builtin(KBuiltin::GlobalInvocationId);
        const int instance = g.swizzle(gid, 0);
        const int mark     = g.kernel_stmt_mark();

        const int inst_count  = loadu(ku(kHdrInstanceCount));
        const int skel_off    = loadu(ku(kHdrSkelOff));
        const int clip_off    = loadu(ku(kHdrClipOff));
        const int anim_off    = loadu(ku(kHdrAnimStateOff));
        const int palette_off = loadu(ku(26U));
        const int jc          = loadu(ku(27U));

        const int in_range = g.binary(KOp::CmpLt, instance, inst_count);
        const int if_valid = g.stmt_if_begin(in_range);

        const int as_base    = add(anim_off, mul(instance, ku(2U)));
        const int clip_local = loadu(as_base);
        const int time       = g.int_bits_to_float(g.cast(loadu(add(as_base, ku(1U))), DType::I32));

        const int cb  = add(clip_off, clip_local);
        const int fc  = loadu(add(cb, ku(1U)));
        const int fps = g.int_bits_to_float(g.cast(loadu(add(cb, ku(3U))), DType::I32));

        const int t_frames = mul(time, fps);
        const int floor_t  = g.unary(KOp::Floor, t_frames);
        const int frame0   = g.binary(KOp::Mod, g.cast(floor_t, DType::U32), fc);
        const int alpha    = sub(t_frames, floor_t);
        const int frame1   = g.binary(KOp::Mod, add(frame0, ku(1U)), fc);
        const int data_base = add(cb, ku(4U));

        const int jc16     = mul(jc, ku(16U));
        const int pal_base = add(palette_off, mul(instance, jc16));
        const int jc10     = mul(jc, ku(10U));

        // ── PASS 1: sample clip + FK → store WORLD matrices to palette ──
        const int for1 = g.stmt_for_begin(jc);
        const int j    = g.kernel_loop_var(for1);
        {
            const int off0 = add(data_base, add(mul(frame0, jc10), mul(j, ku(10U))));
            const int off1 = add(data_base, add(mul(frame1, jc10), mul(j, ku(10U))));

            const int tr0 = g.vec3(loadf(off0), loadf(add(off0, ku(1U))), loadf(add(off0, ku(2U))));
            const int rot0 = g.vec4(loadf(add(off0, ku(3U))), loadf(add(off0, ku(4U))),
                                    loadf(add(off0, ku(5U))), loadf(add(off0, ku(6U))));
            const int scl0 = g.vec3(loadf(add(off0, ku(7U))), loadf(add(off0, ku(8U))), loadf(add(off0, ku(9U))));

            const int tr1 = g.vec3(loadf(off1), loadf(add(off1, ku(1U))), loadf(add(off1, ku(2U))));
            const int rot1 = g.vec4(loadf(add(off1, ku(3U))), loadf(add(off1, ku(4U))),
                                    loadf(add(off1, ku(5U))), loadf(add(off1, ku(6U))));
            const int scl1 = g.vec3(loadf(add(off1, ku(7U))), loadf(add(off1, ku(8U))), loadf(add(off1, ku(9U))));

            const int alpha3 = g.splat(alpha, 3);
            const int tr  = g.ternary(KOp::Mix, tr0, tr1, alpha3);
            const int rot = g.nlerp(rot0, rot1, alpha);
            const int scl = g.ternary(KOp::Mix, scl0, scl1, alpha3);

            const int zero = kf(0.0F);
            const int one  = kf(1.0F);
            const int sx  = g.swizzle(scl, 0);
            const int sy  = g.swizzle(scl, 1);
            const int sz  = g.swizzle(scl, 2);
            const int rc0 = g.quat_rotate(rot, g.vec3(sx, zero, zero));
            const int rc1 = g.quat_rotate(rot, g.vec3(zero, sy, zero));
            const int rc2 = g.quat_rotate(rot, g.vec3(zero, zero, sz));
            const int lc0 = g.vec_concat(rc0, zero);
            const int lc1 = g.vec_concat(rc1, zero);
            const int lc2 = g.vec_concat(rc2, zero);
            const int lc3 = g.vec_concat(tr, one);

            const int parent_raw = loadu(add(skel_off, j));
            const int pal_j      = add(pal_base, mul(j, ku(16U)));

            // Store local matrix first (correct for root joints; non-root overwrites below).
            // This also temps lc0-lc3 and pal_j in the for-loop scope so they're visible
            // to the conditional if-body that follows.
            store_col(pal_j, lc0);
            store_col(add(pal_j, ku(4U)), lc1);
            store_col(add(pal_j, ku(8U)), lc2);
            store_col(add(pal_j, ku(12U)), lc3);

            const int is_non_root = g.binary(KOp::CmpNe, parent_raw, ku(0xFFFFFFFFU));
            const int if_nr = g.stmt_if_begin(is_non_root);
            {
                const int pw_base = add(pal_base, mul(parent_raw, ku(16U)));
                const int pc0 = load_col(pw_base);
                const int pc1 = load_col(add(pw_base, ku(4U)));
                const int pc2 = load_col(add(pw_base, ku(8U)));
                const int pc3 = load_col(add(pw_base, ku(12U)));
                const int pw  = g.mat4(pc0, pc1, pc2, pc3);
                const int wc0 = g.mat_mul_vec(pw, lc0);
                const int wc1 = g.mat_mul_vec(pw, lc1);
                const int wc2 = g.mat_mul_vec(pw, lc2);
                const int wc3 = g.mat_mul_vec(pw, lc3);
                store_col(pal_j, wc0);
                store_col(add(pal_j, ku(4U)), wc1);
                store_col(add(pal_j, ku(8U)), wc2);
                store_col(add(pal_j, ku(12U)), wc3);
            }
            g.stmt_if_end(if_nr);
        }
        g.stmt_for_end(for1);

        // ── PASS 2: world × IBM → overwrite palette with final skin palette ──
        // ⛔ Fresh address nodes — the emitter temps arithmetic inside the enclosing for-body, so nodes
        //    shared with pass-1's for-body would reference an out-of-scope temp name.
        const int jc16_p2     = mul(jc, ku(16U));
        const int pal_base_p2 = add(palette_off, mul(instance, jc16_p2));
        const int for2 = g.stmt_for_begin(jc);
        const int j2   = g.kernel_loop_var(for2);
        {
            const int pal_j2  = add(pal_base_p2, mul(j2, ku(16U)));
            const int wc0 = load_col(pal_j2);
            const int wc1 = load_col(add(pal_j2, ku(4U)));
            const int wc2 = load_col(add(pal_j2, ku(8U)));
            const int wc3 = load_col(add(pal_j2, ku(12U)));
            const int w   = g.mat4(wc0, wc1, wc2, wc3);

            const int ibm_base = add(skel_off, add(jc, mul(j2, ku(16U))));
            const int ic0 = load_col(ibm_base);
            const int ic1 = load_col(add(ibm_base, ku(4U)));
            const int ic2 = load_col(add(ibm_base, ku(8U)));
            const int ic3 = load_col(add(ibm_base, ku(12U)));
            const int pc0 = g.mat_mul_vec(w, ic0);
            const int pc1 = g.mat_mul_vec(w, ic1);
            const int pc2 = g.mat_mul_vec(w, ic2);
            const int pc3 = g.mat_mul_vec(w, ic3);
            store_col(pal_j2, pc0);
            store_col(add(pal_j2, ku(4U)), pc1);
            store_col(add(pal_j2, ku(8U)), pc2);
            store_col(add(pal_j2, ku(12U)), pc3);
        }
        g.stmt_for_end(for2);

        g.stmt_if_end(if_valid);

        KEntry e{};
        e.stage             = KStage::Compute;
        e.local_size[0]     = 64U;
        e.kernel_body_begin = mark;
        e.kernel_body_count = g.stmt_count() - mark;

        std::unique_ptr<crd::gpu::IGpuProgram> p = ctx->create_program(g, e);
        if (p == nullptr) { return nullptr; }
        kern_skin_compute = p.get();
        adv_stages.push_back(std::move(p));
        return kern_skin_compute;
    }

    // ⭐⭐ REN-41 (velocity, skinned): the DEVICE PALETTE-SNAPSHOT kernel. Copies palette_off → prev_palette_off per
    // skinned instance so the velocity prepass can deform each vertex by LAST frame's pose. Runs as a SEPARATE pass
    // BEFORE gpu_skin (folding it into gpu_skin would read-after-write the palette it overwrites — the RAW inline-load
    // scar). ⛔ Gated on kHdrGpuSkinActive: under CPU skinning the renderer already CPU-uploads prev_palette, and an
    // unconditional device copy would set prev == cur (the current pose), silently zeroing skinned motion. src/dst are
    // DISJOINT regions (palette vs prev_palette), so there is no read-after-write within the kernel.
    [[nodiscard]] crd::gpu::IGpuProgram* ensure_palette_snapshot_kernel()
    {
        if (kern_palette_snapshot != nullptr) { return kern_palette_snapshot; }
        if (ctx == nullptr) { return nullptr; }

        using namespace crd::kir;
        KGraph      g(alloc);
        const Shape sh1 = make_shape({1});
        const auto  ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
        const auto  add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
        const auto  mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

        const int  buf    = g.buffer_decl(DType::U32, 0, 0, true);
        const auto loadu  = [&](int idx) { return g.buffer_load(buf, idx); };
        const auto storeu = [&](int idx, int val) { g.stmt_buffer_store(buf, idx, val); };

        const int gid      = g.builtin(KBuiltin::GlobalInvocationId);
        const int instance = g.swizzle(gid, 0);
        const int mark     = g.kernel_stmt_mark();

        const int active    = loadu(ku(kHdrGpuSkinActive));
        const int inst_ct   = loadu(ku(kHdrInstanceCount));
        const int pal_off   = loadu(ku(26U)); // palette_off
        const int prev_off  = loadu(ku(kHdrPrevPaletteOff));
        const int jc        = loadu(ku(27U)); // joint_count

        const int if_active = g.stmt_if_begin(g.binary(KOp::CmpNe, active, ku(0U)));
        {
            const int if_range = g.stmt_if_begin(g.binary(KOp::CmpLt, instance, inst_ct));
            {
                const int words = mul(jc, ku(16U)); // 4×4 matrix per joint; a non-skinned group has jc==0 → no-op
                const int src   = add(pal_off, mul(instance, words));
                const int dst   = add(prev_off, mul(instance, words));
                const int forw  = g.stmt_for_begin(words);
                const int w     = g.kernel_loop_var(forw);
                {
                    storeu(add(dst, w), loadu(add(src, w)));
                }
                g.stmt_for_end(forw);
            }
            g.stmt_if_end(if_range);
        }
        g.stmt_if_end(if_active);

        KEntry e{};
        e.stage             = KStage::Compute;
        e.local_size[0]     = 64U;
        e.kernel_body_begin = mark;
        e.kernel_body_count = g.stmt_count() - mark;

        std::unique_ptr<crd::gpu::IGpuProgram> p = ctx->create_program(g, e);
        if (p == nullptr) { return nullptr; }
        kern_palette_snapshot = p.get();
        adv_stages.push_back(std::move(p));
        return kern_palette_snapshot;
    }

    [[nodiscard]] crd::gpu::IGpuProgram* ensure_rt_kernel(crd::u32 which) // 0 rg · 1 ms · 2 ch · 3 ah
    {
        if (which >= 4U) { return nullptr; }
        if (kern_rt[which] != nullptr) { return kern_rt[which]; }
        static constexpr const char* kRtAsset[4] = {"vertex/scene_rt_raygen.crdv", "vertex/scene_rt_miss.crdv",
                                                    "vertex/scene_rt_closest_hit.crdv",
                                                    "vertex/scene_rt_any_hit.crdv"};
        kern_rt[which] = cook_stage_named(kRtAsset[which]);
        return kern_rt[which];
    }

    [[nodiscard]] crd::gpu::IStorageBuffer* ensure_scratch(std::unique_ptr<crd::gpu::IStorageBuffer>& slot)
    {
        if (slot == nullptr && raster != nullptr)
        {
            slot = raster->create_storage_buffer(4096U);
            if (slot != nullptr)
            {
                // fresh device memory is NOT guaranteed zero, and the gates read these buffers back
                crd::u8 zeros[4096] = {};
                (void)raster->upload_storage(*slot, 0U, static_cast<const void*>(zeros), 4096U);
            }
        }
        return slot.get();
    }

    explicit Impl(crd::memory::IAllocator* a)
        : alloc(a), skeleton_cache(a), clip_cache(a), pose_scratch(a), world_scratch(a), palette_scratch(a),
          palette_staging(a), anim_state_staging(a), chunk_index(a), chunks(a), runs(a), dirty_runs(a), bounds_staging(a),
          group_of_mesh(a), material_color(a), material_texture(a), entity_slot(a),
          contrib_draws(a), frame(a), fallback(a), recorder(a), groups_view(a), fs_hashes(a), fs_programs(a),
          fb_frame_names(a), fb_frame_descs(a), techniques(a), adv_stages(a)
    {
        frame_ok = false; // populated by set_asset_root()
        contrib_draws.reserve(kMaxContributions);

        for (crd::u32 i = 0; i < kMaxContributions; ++i)
        {
            contrib_draws.push_back(crd::containers::Array<SceneDraw>(a));
        }
    }

    [[nodiscard]] crd::math::Vec4f resolve_color(const crd::resources::ResourceId& material)
    {
        if (material.is_null()) { return {0.8F, 0.8F, 0.8F, 1.0F}; }
        if (const crd::math::Vec4f* cached = material_color.find(material)) { return *cached; }
        crd::math::Vec4f color{0.8F, 0.8F, 0.8F, 1.0F};
        auto handle = rm->load_sync<crd::resources::OpenPbrMaterial>(material);
        if (handle.state() == crd::resources::LoadState::Ready && handle.get() != nullptr)
        {
            const crd::resources::PbrmParams& p = handle.get()->params;
            color = {p.base_color[0], p.base_color[1], p.base_color[2], p.base_alpha};
        }
        material_color.insert(material, color);
        return color;
    }

    // REN-2 Half B: resolve a material's base-color (albedo) map to a GPU texture — load the OpenPbrMaterial, follow
    // its `textures.base_color` ResourceId to the cooked TextureResource, and upload its mip chain VERBATIM via
    // create_texture_from_mips (the RET-3 seam). Cached per material; nullptr if the material has no base-color map
    // (⇒ the flat-colour path). The cache OWNS the texture; the returned pointer is borrowed (stable for the cache's life).
    [[nodiscard]] crd::gpu::ITexture* resolve_base_color_texture(const crd::resources::ResourceId& material)
    {
        if (material.is_null()) { return nullptr; }
        if (auto* cached = material_texture.find(material)) { return cached->get(); }
        std::unique_ptr<crd::gpu::ITexture> owned;
        auto mh = rm->load_sync<crd::resources::OpenPbrMaterial>(material);
        if (mh.state() == crd::resources::LoadState::Ready && mh.get() != nullptr)
        {
            const crd::resources::ResourceId& bc = mh.get()->textures.base_color;
            if (!bc.is_null())
            {
                auto th = rm->load_sync<crd::resources::TextureResource>(bc);
                if (th.state() == crd::resources::LoadState::Ready && th.get() != nullptr && th.get()->mip_count > 0U)
                {
                    const crd::resources::TextureResource& t = *th.get();
                    crd::containers::Array<const void*>     mip_ptrs(alloc);
                    for (crd::u32 i = 0; i < t.mip_count; ++i) { mip_ptrs.push_back(t.mips[i].pixels.data()); }
                    const bool srgb = t.format == crd::resources::TextureFormat::RGBA8UnormSrgb;
                    owned = raster->create_texture_from_mips(t.mips[0].width, t.mips[0].height, t.mip_count,
                                                             mip_ptrs.data(), srgb);
                }
            }
        }
        crd::gpu::ITexture* result = owned.get();
        material_texture.insert(material, std::move(owned));
        return result;
    }
};

SceneRenderer::SceneRenderer(crd::memory::IAllocator* alloc)
    : m_impl(std::make_unique<Impl>(alloc)), m_groups(alloc)
{
}

SceneRenderer::~SceneRenderer() = default;

// C4996-safe env read (the engine's `mf_getenv` pattern): the renderer honours `CRD_ASSETS_DIR` only to LOCATE
// its shipped default assets when the host has not pinned a root — a FIXED, documented dev/quick-start knob over
// a fixed name, never a user-controlled path fed to a shell, so the deprecated-getenv warning is silenced here.
[[nodiscard]] static const char* renderer_getenv(const char* name) noexcept
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

bool SceneRenderer::init(crd::gpu::IRasterContext& raster, crd::resources::ResourceManager& rm)
{
    m_impl->raster = &raster;
    m_impl->rm     = &rm;
    // ⭐⭐ REN-41: the renderer's DEFAULT assets ship as FILES (there is no in-binary pack). If the host has not
    // pinned a root, honour the documented `CRD_ASSETS_DIR` convention so the shipped defaults resolve without
    // ceremony — the tests and the quick-start path. An application that authors its OWN pipelines calls
    // `set_asset_root` and that wins. Best-effort: a missing or unparseable root just leaves it empty, and
    // `init_programs` then reports the first asset it cannot find rather than failing here.
    if (m_impl->asset_root.size() == 0U)
    {
        if (const char* aroot = renderer_getenv("CRD_ASSETS_DIR"); aroot != nullptr && aroot[0] != '\0')
        {
            (void)set_asset_root(aroot);
        }
    }
    return raster.valid();
}

bool SceneRenderer::set_shadows_enabled(bool on) noexcept
{
    m_impl->shadows_requested = on;
    return m_impl->shadows_active();
}

void SceneRenderer::set_csm_config(const CsmConfig& cfg) noexcept { m_impl->csm = cfg; }

void SceneRenderer::set_forward_technique(const char* name) noexcept
{
    if (name != nullptr) { m_impl->forward_technique = name; }
}
void SceneRenderer::set_shadow_technique(const char* name) noexcept
{
    if (name != nullptr) { m_impl->shadow_technique = name; }
}
void SceneRenderer::set_pcf_taps(crd::u32 taps) noexcept { m_impl->pcf_taps = taps; }
// ⭐⭐ REN-40-D: how wide the cascade cross-fade is, in PERCENT of a cascade's footprint. ⛔ 0 cooks the
// byte-identical hard-select graph this technique has always emitted, which is what makes the parity arm a
// property of the COOK rather than a tolerance in a test. Call before `init_programs`.
// ⭐⭐ REN-40-D: the SOFTNESS MODEL. `pcss` searches the map for what is actually blocking each fragment and
// sets the filter radius from how far away it is, so a contact point stays sharp and the same shadow softens
// with distance from its caster — the single cue the eye uses to read contact. ⛔ `angle_x100` is the light's
// angular RADIUS in hundredths of a degree (the sun is ~27), because a directional light has no size, it has an
// angular diameter — and a penumbra derived from an angle is correct at every cascade scale for free.
void SceneRenderer::set_soft_shadows(bool pcss, crd::u32 angle_x100) noexcept
{
    set_soft_shadows(pcss ? SoftShadow::Pcss : SoftShadow::Off, angle_x100);
}

// ⭐⭐ REN-40-D: the full axis. ⛔ Evsm/Msm swap the DEFAULT frame graph for the moment tier (and back) — but
// NEVER an explicitly installed one: an explicit graph is the caller's contract, and silently editing it from a
// quality setter is exactly the two-homes drift the built-in pack exists to prevent.
void SceneRenderer::set_soft_shadows(SoftShadow mode, crd::u32 angle_x100) noexcept
{
    m_impl->soft_mode        = static_cast<crd::u32>(mode);
    m_impl->light_angle_x100 = clamp_u32(angle_x100, 1U, 2000U);
    if (!m_impl->frame_overridden)
    {
        crd::containers::String text(m_impl->alloc);
        if (m_impl->asset_text(m_impl->soft_mode >= 2U ? "frame/forward_csm_moment.frame.toml"
                                                        : "frame/forward_csm.frame.toml", text))
        {
            crd::framecook::FrameGraphDesc d(m_impl->alloc);
            if (crd::framecook::parse_frame_toml(crd::containers::StringView(text.c_str(), text.size()), d)
                == crd::framecook::FrameCookError::Ok)
            {
                m_impl->frame = std::move(d);
            }
        }
    }
}

// ⛔ Clamped to the DECLARED ranges here rather than trusted, and `search_taps` SNAPS to a table size instead of
// being rounded: the disc tables are normalised per count, so an unlisted value has no table and silently falling
// back to a prefix of a larger one would shrink the search radius by sqrt(n/16) — a quieter, worse version of the
// ring bug this option exists to keep fixed.
void SceneRenderer::set_soft_shadow_quality(crd::u32 max_texels, crd::u32 search_taps) noexcept
{
    m_impl->soft_max_texels = clamp_u32(max_texels, 1U, 256U);
    // ⛔ SNAPS to a table size rather than rounding: the disc tables are normalised per count, so an unlisted
    // value has no table, and quietly taking a prefix of a larger one would shrink the search by sqrt(n/16).
    crd::u32 snapped = 4U;
    if (search_taps >= 16U) { snapped = 16U; }
    else if (search_taps >= 8U) { snapped = 8U; }
    m_impl->soft_search_taps = snapped;
}

void SceneRenderer::set_cascade_blend_pct(crd::u32 pct) noexcept
{
    m_impl->blend_pct = pct > 50U ? 50U : pct;
}
void SceneRenderer::set_shadow_fade_pct(crd::u32 pct) noexcept
{
    m_impl->fade_pct = pct > 50U ? 50U : pct;
}
void SceneRenderer::set_shadow_caster_min_px(crd::f32 px) noexcept
{
    m_impl->shadow_caster_min_px = px > 0.0F ? px : 0.0F;
}
void SceneRenderer::set_min_draw_px(crd::f32 px) noexcept
{
    m_impl->min_draw_px = px > 0.0F ? px : 0.0F;
}
void SceneRenderer::set_cascade_round_robin(bool on) noexcept { m_impl->round_robin_far = on; }

// ⭐⭐ REN-41 (TAA): the caller supplies R = prev_UNJITTERED_vp · inv(cur_JITTERED_vp) (it owns both projections)
// and whether a previous frame exists. The renderer uploads it for the resolve pass; identity + has_history=false
// on the first frame makes the resolve fall back to the current sample. See the upload note in render().
void SceneRenderer::set_taa_reproj(const crd::math::Mat4f& reproj, bool has_history) noexcept
{
    m_impl->taa_reproj      = reproj;
    m_impl->taa_has_history = has_history;
}

void SceneRenderer::begin_frame() noexcept { m_impl->contrib_used = 0U; }

RenderStats SceneRenderer::contribute(crd::gpu::IFrameGraph& fg, crd::gpu::IRasterTarget& target,
                                      const crd::math::Mat4f& view_proj, const crd::math::Vec3f& light_dir,
                                      crd::gpu::ClearColor clear, const crd::scene::SpatialBVHIndex* bvh)
{
    // ⛔ The borrow is scoped to exactly this call. Leaving `external_fg` set would make the NEXT `render()` record
    // into a graph its caller no longer owns — a use-after-free that renders correctly right up until it does not.
    m_impl->external_fg = &fg;
    const RenderStats s = render(target, view_proj, light_dir, clear, bvh);
    m_impl->external_fg = nullptr;
    return s;
}

const CsmCascades& SceneRenderer::cascades() const noexcept { return m_impl->cascades; }

void SceneRenderer::set_overlay_pass(FramePassFn fn, void* user) noexcept
{
    Impl& impl        = *m_impl;
    impl.overlay_fn   = fn;
    impl.overlay_user = user;
}

void SceneRenderer::set_readback_enabled(bool on) noexcept
{
    Impl& impl    = *m_impl;
    impl.readback = on;
    // the graph may not exist yet (it is created lazily on the first render) — `render()` re-applies the flag
    if (impl.frame_graph != nullptr) { impl.frame_graph->set_readback_enabled(on); }
}

// ── ⭐⭐ REN-38-F6: the ADVANCED-GRAPH seams. ────────────────────────────────────────────────────────────────
// ⛔ A parse or validation failure REFUSES the graph and keeps the previous one — it never half-installs. The
// caller gates on the return value; silently rendering the old graph while claiming the new one is the exact
// class of lie the magenta error graph exists to prevent, which is why the failure is a `false`, not a log line.
// ⭐⭐ 38-G1: the by-NAME face of the above. The text comes from the asset system (disk-first), never from
// the caller — an app names a frame, it does not carry one.
bool SceneRenderer::set_frame_graph_asset(const char* asset_name)
{
    if (asset_name == nullptr || m_impl == nullptr) { return false; }
    crd::containers::String text(m_impl->alloc);
    if (!m_impl->asset_text(asset_name, text))
    {
        CRD_LOG_ERROR(g_log_scenerender, "set_frame_graph_asset: no such asset '{}'", asset_name);
        return false;
    }
    return set_frame_graph_toml(text.c_str());
}

bool SceneRenderer::set_frame_graph_toml(const char* toml_text)
{
    if (toml_text == nullptr || m_impl == nullptr) { return false; }
    Impl&                          impl = *m_impl;
    crd::framecook::FrameGraphDesc d(impl.alloc);
    crd::containers::String        where(impl.alloc);
    // ⛔ REN-40-A: report the REASON, not just the place. `where` is empty for every whole-file error (a bad
    // resource `kind`, a missing key), so the old message read "parse failed at ''" — a diagnostic that tells the
    // author nothing at the exact moment they need it most. The cook layer already carries an error STRING; use it.
    if (const auto perr = crd::framecook::parse_frame_toml(crd::containers::StringView(toml_text), d, &where);
        perr != crd::framecook::FrameCookError::Ok)
    {
        CRD_LOG_ERROR(g_log_scenerender, "set_frame_graph_toml: parse failed: {} (at '{}')",
                      crd::framecook::frame_cook_error_text(perr), where.c_str());
        return false;
    }
    if (const auto verr = crd::framecook::validate_frame_graph(d, &where);
        verr != crd::framecook::FrameCookError::Ok)
    {
        CRD_LOG_ERROR(g_log_scenerender, "set_frame_graph_toml: validation failed: {} (at '{}')",
                      crd::framecook::frame_cook_error_text(verr), where.c_str());
        return false;
    }
    impl.frame            = static_cast<crd::framecook::FrameGraphDesc&&>(d);
    impl.frame_ok         = true;
    impl.frame_overridden = true;
    return true;
}

void SceneRenderer::set_scene_accel(crd::gpu::IAccelerationStructure* accel) noexcept
{
    if (m_impl != nullptr) { m_impl->scene_accel = accel; }
}

// ── ⭐ REN-38-F15 / REN-41: the ASSET ROOT is the SINGLE SOURCE. ────────────────────────────────────────────
// Every authored asset this renderer cooks is read from a file under `root` — which is what makes editing
// `assets/` change the frame without a rebuild. The default FRAME PAIR is (re-)parsed here so a root swap takes
// effect immediately; ⛔ a file that exists but fails to parse REFUSES the root (false, nothing half-installed).
// There is no in-binary pack to fall back to.
bool SceneRenderer::set_asset_root(const char* dir)
{
    if (m_impl == nullptr || dir == nullptr) { return false; }
    Impl& impl = *m_impl;
    impl.asset_root.clear();
    impl.asset_root.append(dir);
    crd::containers::String t(impl.alloc);
    crd::containers::String where(impl.alloc);
    const auto reparse = [&](const char* name, crd::framecook::FrameGraphDesc& into) {
        if (!impl.asset_text(name, t)) { return false; }
        crd::framecook::FrameGraphDesc d(impl.alloc);
        if (crd::framecook::parse_frame_toml(crd::containers::StringView(t.c_str(), t.size()), d, &where)
            != crd::framecook::FrameCookError::Ok)
        {
            CRD_LOG_ERROR(g_log_scenerender, "set_asset_root: '{}' failed to parse at '{}'", name, where.c_str());
            return false;
        }
        into = static_cast<crd::framecook::FrameGraphDesc&&>(d);
        return true;
    };
    // an EXPLICITLY installed graph (set_frame_graph_toml) is not disturbed by the root swap
    if (!impl.frame_overridden && !reparse("frame/forward_csm.frame.toml", impl.frame))
    {
        impl.asset_root.clear();
        return false;
    }
    if (!reparse("frame/forward_basic.frame.toml", impl.fallback))
    {
        impl.asset_root.clear();
        return false;
    }
    impl.frame_ok = true;
    return true;
}

const crd::gpu::IFrameGraph* SceneRenderer::debug_frame_graph() const noexcept
{
    return m_impl != nullptr ? m_impl->frame_graph.get() : nullptr;
}

crd::u32 SceneRenderer::debug_variant_vertex() const noexcept
{
    return m_impl != nullptr ? m_impl->vertex_variant : 0U;
}

crd::gpu::IStorageBuffer* SceneRenderer::debug_scene_buffer(const char* name) noexcept
{
    if (m_impl == nullptr || name == nullptr) { return nullptr; }
    const crd::containers::StringView n(name);
    if (n == crd::containers::StringView("cull_flags")) { return m_impl->buf_cull_flags.get(); }
    if (n == crd::containers::StringView("cull_marks")) { return m_impl->buf_cull_marks.get(); }
    if (n == crd::containers::StringView("hits")) { return m_impl->buf_hits.get(); }
    return nullptr;
}

namespace
{
// The parsed declaration, owned for the process. ⛔ Parsed ONCE and reused: re-parsing per cook would make the
// technique’s identity depend on when it was cooked rather than on what it says.
// 38-G1 (user-directed): the DISK text, when the renderer resolved one, wins over the embedded copy — the
// same F15 shadowing every other default follows. The injection happens ONCE, before first cook (the
// "parsed once" identity rule holds: whichever text won, it wins for the whole process).
[[nodiscard]] crd::containers::String& scene_lighting_disk_text()
{
    static crd::containers::String text(crd::memory::default_allocator());
    return text;
}
[[nodiscard]] crd::lightcook::LightingDesc& scene_lighting_desc()
{
    static crd::lightcook::LightingDesc desc = [] {
        crd::lightcook::LightingDesc d(crd::memory::default_allocator());
        crd::containers::String       where(crd::memory::default_allocator());
        const crd::containers::String& disk = scene_lighting_disk_text();
        (void)crd::lightcook::parse_lighting_toml(
            crd::containers::StringView(disk.c_str(), disk.size()), d, &where);
        return d;
    }();
    return desc;
}

// The technique body: the ABI’s fixed inputs in, the authored lighting out. ⛔ Returns -1 on any missing input
// or binding rather than a partial graph — a lighting technique that silently dropped its shadow term renders a
// scene with no shadows and no error.
int body_scene_authored(crd::kir::KGraph& g, const crd::kir::technique::TechniqueContext& tc, void* /*user*/)
{
    namespace tq = crd::kir::technique;
    crd::lightcook::LightingInputs in;
    in.base_color = tc.fixed[tq::kTiBaseColor];
    in.metallic   = tc.fixed[tq::kTiMetallic];
    in.roughness  = tc.fixed[tq::kTiRoughness];
    in.normal     = tc.fixed[tq::kTiNormal];
    in.view_dir   = tc.fixed[tq::kTiViewDir];
    in.world_pos  = tc.fixed[tq::kTiWorldPos];
    in.emissive   = tc.fixed[tq::kTiEmissive];
    // ⛔ The fixed ABI carries no fragment COORDINATE, so the PCF dither falls back to a constant. Stated
    // rather than hidden: it costs the per-pixel rotation that hides the tap pattern, not correctness.
    in.frag_xy = -1;

    crd::lightcook::LightingBindings b;
    b.shadow_atlas   = tc.binding(tq::kCsmBindAtlasTex);
    b.shadow_sampler = tc.binding(tq::kCsmBindAtlasSamp);
    for (crd::u32 c = 0; c < crd::lightcook::kMaxCascades; ++c)
    {
        b.csm_light_vp[c] = tc.binding(tq::kCsmBindLightVp0 + static_cast<int>(c));
    }
    b.csm_map_size = tc.binding(tq::kCsmBindMapSize);
    return crd::lightcook::cook_lighting(scene_lighting_desc(), g, in, b);
}

// ⭐ Same DECLARED contract as `forward_csm` — same bindings, same ABI order — so the renderer’s resolver
// feeds it identically. What differs is the provenance of the BODY: a declaration instead of C++.
[[nodiscard]] crd::kir::technique::Technique scene_authored_technique() noexcept
{
    crd::kir::technique::Technique t;
    t.name       = "forward_authored";
    t.body       = &body_scene_authored;
    t.bindings   = crd::kir::technique::kForwardCsmBindings;
    // ⛔⛔ DERIVED, the same fix forward_csm needed twice: hand-counted 3/2 here silently pinned the authored
    // technique to the pre-40-D contract — the fourth binding (the PCSS depth sampler) never resolved and every
    // soft/blend option quietly read its default. The body ignores axes it does not consume, and unused binding
    // nodes are DCE'd, so deriving the full counts costs nothing and stops the lag from ever recurring.
    t.n_bindings = static_cast<int>(sizeof(crd::kir::technique::kForwardCsmBindings)
                                    / sizeof(crd::kir::technique::kForwardCsmBindings[0]));
    t.options    = crd::kir::technique::kForwardCsmOptions;
    t.n_options  = static_cast<int>(sizeof(crd::kir::technique::kForwardCsmOptions)
                                    / sizeof(crd::kir::technique::kForwardCsmOptions[0]));
    return t;
}
} // namespace
bool SceneRenderer::init_programs(crd::gpu::IGpuContext& ctx)
{
    if (m_impl->raster == nullptr) { CRD_LOG_ERROR(g_log_scenerender, "init_programs: raster is null"); return false; }
    m_impl->ctx = &ctx;

    // ⭐⭐ REN-37.2: EVERY fragment program this renderer runs is now COOKED FROM THE MATERIAL and SHADED BY A
    // NAMED TECHNIQUE. There is no hand-written fragment shader left in this file. The technique library is the
    // shader-half twin of the built-in frame-graph pack: engine defaults register first, and an app overrides
    // purely by shadowing a name.
    // 38-G1 (user-directed): resolve the LIGHTING declaration disk-first BEFORE anything cooks — a shipped
    // `assets/lighting/scene_forward.crdl` shadows the embedded copy, like every other default.
    {
        crd::containers::String ltext(m_impl->alloc);
        if (m_impl->asset_text("lighting/scene_forward.crdl", ltext) && ltext.size() > 0U)
        {
            scene_lighting_disk_text().clear();
            scene_lighting_disk_text().append(ltext.c_str());
        }
    }
    crd::kir::technique::register_builtin_techniques(m_impl->techniques);
    // REN-38-E7: the AUTHORED lighting technique, registered after the builtins so an app can shadow
    // it by name exactly as it can shadow a built-in one.
    m_impl->techniques.define(scene_authored_technique());
    const crd::kir::technique::Technique* fwd = m_impl->techniques.find(m_impl->forward_technique);
    const crd::kir::technique::Technique* csm = m_impl->techniques.find(m_impl->shadow_technique);
    // ⛔ A named technique that does not resolve FAILS. Falling back to a default would render a plausible frame
    // for the WRONG technique, which is the exact class of lie the error graph exists to prevent.
    if (fwd == nullptr) { CRD_LOG_ERROR(g_log_scenerender, "init_programs: forward technique '{}' not found", m_impl->forward_technique); return false; }

    // ONE vertex program feeds every fragment variant (normal@0 · tint@1 · worldpos+depth@2 · uv@3). Before
    // REN-37.2 there were three VS variants with three different varying layouts, which is precisely how the
    // textured path and the shadowed path ended up disagreeing about what location 2 meant.
    crd::kir::KGraph vg(m_impl->alloc);
    crd::kir::KEntry ve;
    // ⭐⭐ 38-G1 (user-directed): resolved BY NAME — a shipped `assets/vertex/scene.crdv` shadows the
    // embedded copy, exactly like the frames, materials and post graphs. The builtin pack is the fallback.
    crd::containers::String vs_scene(m_impl->alloc);
    {
        crd::containers::String vs_body(m_impl->alloc);
        if (!m_impl->asset_text("vertex/scene.crdv", vs_body)) { CRD_LOG_ERROR(g_log_scenerender, "init_programs: scene.crdv not found"); return false; }
        // ── ⭐⭐ REN-40-C2: THE DRAW TABLE IS UNIVERSAL. ─────────────────────────────────────────────────────
        // ⛔⛔ IT HAD TO BECOME UNIVERSAL, and the reason is the defect it fixes. The per-draw LOD SLOT reaches a
        // vertex program through the draw-table row, and the table used to exist ONLY in the consolidated scene
        // buffer — which `consolidate` disables whenever shadows are active. So in every shadowed configuration
        // (i.e. the shipping one, and the one both REN-40 boards were measured in) the stage was cooked WITHOUT
        // `rebase_table`, had no row to read, computed slot 0, and read slot 0's visible list — which is EMPTY,
        // because the cull sent those survivors to slots 1..n. **Levels 1 and coarser drew NOTHING**, the frame
        // still rendered, every count still reconciled, and GPU time DROPPED — so it read as an LOD win.
        // ⭐ Now every group's buffer carries the table at the SAME word offset the scene buffer's prefix does
        // (`kSceneDrawTableOff`, right after the header), so ONE constant addresses both layouts and the two
        // draw paths stop being two paths. A private buffer's row simply carries base 0.
        const crd::f32 db = m_impl->lod_enabled ? m_impl->lod_policy.dither_band : 0.0F;
        char rb[256];
        (void)std::snprintf(static_cast<char*>(rb), sizeof(rb),
                            "rebase_table = %u\nrebase_stride = %u\nlod_slots = %u\n"
                            "instance_capacity_word = %u\ndither_band = %.6f\n",
                            kSceneDrawTableOff, kSceneDrawRowWords, m_impl->lod_slots, kHdrInstanceCapacity,
                            static_cast<double>(db));
        vs_scene.append(static_cast<const char*>(rb));
        vs_scene.append(vs_body.c_str());
    }
    crd::vertcook::VertexProgramDesc scene_desc(m_impl->alloc);
    if (!cook_vs(m_impl->alloc, vs_scene.c_str(), nullptr, vg, ve, &scene_desc)) { CRD_LOG_ERROR(g_log_scenerender, "init_programs: scene VS cook failed"); return false; }
    // ⭐ REN-38 (the D5 correction closed): `VariantKey::vertex` is ENGINE-FILLED from the LIVE declaration —
    // the folded `vertex_layout_id` of the very `.crdv` this renderer just cooked. Until now no engine code
    // filled the field at all, so the variant identity's vertex axis was a documented claim, not a value.
    {
        const crd::u64 lid       = crd::vertcook::vertex_layout_id(scene_desc);
        m_impl->vertex_variant   = static_cast<crd::u32>(lid ^ (lid >> 32U));
    }
    // ⭐⭐⭐ REN-41 (VARYING CONTRACT): capture the scene VS's full interpolant layout — location + type + interp,
    // straight from the COOKED entry, so it reflects the LIVE set (the cook already injected the LOD-dither varying
    // into `ve.out` when dither_band>0). `cook_fs` replays this into every scene fragment on the DX12 side.
    m_impl->n_scene_varyings = 0U;
    for (int k = 0; k < ve.n_out && m_impl->n_scene_varyings < crd::vertcook::kMaxVaryings; ++k)
    {
        if (ve.out[k].node < 0) { continue; }
        auto& sv    = m_impl->scene_varyings[m_impl->n_scene_varyings++];
        sv.location = static_cast<crd::u32>(ve.out[k].location);
        sv.type     = vg.node(ve.out[k].node).type;
        sv.interp   = ve.out[k].interp;
    }
    // REN-40-H: the dither fade varying at location 4 is cook-time injected (vertex_asset.cpp line
    // 3337) when dither_band > 0 and lod_slots > 1, but the TOML declaration only carries the
    // authored locations 0..3. verify_varying_contract iterates desc.varyings and cannot find a
    // cook-time injection, so the contract fails. Stamp a synthetic entry so the check succeeds.
    const auto stamp_dither_varying = [&](crd::vertcook::VertexProgramDesc& d) {
        if (!m_impl->lod_enabled || m_impl->lod_policy.dither_band <= 0.0F || m_impl->lod_slots <= 1U)
            return;
        for (crd::usize i = 0; i < d.varyings.size(); ++i)
        {
            if (d.varyings[i].location == 4U) return;
        }
        crd::vertcook::VaryingDesc dv(m_impl->alloc);
        dv.location = 4;
        dv.flat     = true;
        crd::vertcook::VaryingSource ds(m_impl->alloc);
        ds.kind = crd::vertcook::VaryingSourceKind::ClipW;
        dv.source.push_back(static_cast<crd::vertcook::VaryingSource&&>(ds));
        d.varyings.push_back(static_cast<crd::vertcook::VaryingDesc&&>(dv));
    };
    stamp_dither_varying(scene_desc);
    // ⭐⭐ REN-38 audit: the 38-D4 varying contract runs at THE JOIN, on the live path — every cooked fragment
    // read set (location + width + interpolation, derived from the graph itself) is verified against the `.crdv`
    // declaration BEFORE a program pair is created. The skinned-VS scar linked, bound and rendered from
    // undefined interpolants; nothing on either backend can see it, so the cook boundary is where it must die.
    const auto contract_ok = [&](const crd::vertcook::VertexProgramDesc&      vdesc,
                                 const crd::vertcook::VaryingRequirement* rq, crd::u32 nq) {
        crd::containers::String cw(m_impl->alloc);
        const auto              rc = crd::vertcook::verify_varying_contract(vdesc, rq, nq, &cw);
        if (rc != crd::vertcook::VertexCookError::Ok)
        {
            CRD_LOG_ERROR(g_log_scenerender, "varying contract refused: {} at '{}' ({} fragment reads)",
                          crd::vertcook::vertex_cook_error_text(rc), cw.c_str(), nq);
        }
        return rc == crd::vertcook::VertexCookError::Ok;
    };
    // ⭐ REN-37.7: every fragment program below comes from `cook_fs` — cook, lower, CONTENT-HASH, reuse. The
    // renderer no longer decides how many device programs it needs; the deduped matrix does.
    SceneShaderConfig fcfg;
    fcfg.pass = crd::kir::cook::PassType::Forward;
    fcfg.tech = fwd;
    m_impl->vs = ctx.create_program(vg, ve);
    crd::vertcook::VaryingRequirement fwd_reqs[crd::vertcook::kMaxVaryings];
    crd::u32                          n_fwd_reqs = 0U;
    crd::gpu::IGpuProgram* fs_flat = m_impl->cook_fs(fcfg, fwd_reqs, crd::vertcook::kMaxVaryings, &n_fwd_reqs);
    if (m_impl->vs == nullptr || fs_flat == nullptr) { CRD_LOG_ERROR(g_log_scenerender, "init_programs: program creation failed"); return false; }
    if (!contract_ok(scene_desc, static_cast<const crd::vertcook::VaryingRequirement*>(fwd_reqs), n_fwd_reqs))
    {
        CRD_LOG_ERROR(g_log_scenerender, "init_programs: varying contract failed"); return false;
    }
    m_impl->fs      = fs_flat;
    m_impl->program = m_impl->raster->create_raster_program(*m_impl->vs, *fs_flat);

    // ⭐⭐ REN-38 (scene-buffer consolidation): the REBASED variant of the SAME declaration — one extra
    // top-level line (`rebase_table`) ahead of the same prologue+varyings, so there is exactly one source of
    // truth for the vertex vocabulary and the rebased twin can never drift from it.
    {
        crd::kir::KGraph rvg(m_impl->alloc);
        crd::kir::KEntry rve;
        crd::containers::String vs_rb(m_impl->alloc);
        // ⭐⭐ REN-40-C2: NO PREFIX ANY MORE. `vs_scene` itself now carries `rebase_table` / `rebase_stride` /
        // `lod_slots` / `instance_capacity_word`, because the draw table became UNIVERSAL — every buffer holds
        // one, so every scene stage rebases. ⛔⛔ Prefixing them a SECOND time here produced a DUPLICATE TOML
        // KEY: toml++ refused the document, this cook returned null, `program_rebased` stayed null, and
        // `consolidate` silently turned OFF — two mesh groups stopped batching into one multi-draw. The REN-38
        // gate caught it as `2 == 1`, which is exactly what that gate exists for.
        vs_rb.append(vs_scene.c_str()); // the SAME resolved declaration — the twin can never drift from it
        crd::vertcook::VertexProgramDesc rb_desc(m_impl->alloc);
        if (cook_vs(m_impl->alloc, vs_rb.c_str(), nullptr, rvg, rve, &rb_desc))
        {
            stamp_dither_varying(rb_desc);
            if (contract_ok(rb_desc, static_cast<const crd::vertcook::VaryingRequirement*>(fwd_reqs), n_fwd_reqs))
            {
                m_impl->vs_rebased = ctx.create_program(rvg, rve);
                if (m_impl->vs_rebased != nullptr)
                {
                    m_impl->program_rebased = m_impl->raster->create_raster_program(*m_impl->vs_rebased, *fs_flat);
                }
            }
        }
    }

    crd::kir::KGraph svg(m_impl->alloc);
    crd::kir::KEntry sve;
    crd::containers::String vs_skin(m_impl->alloc);
    {
        crd::containers::String vs_skin_body(m_impl->alloc);
        if (!m_impl->asset_text("vertex/scene_skinned.crdv", vs_skin_body)) { CRD_LOG_ERROR(g_log_scenerender, "init_programs: scene_skinned.crdv not found"); return false; }
        const crd::f32 db = m_impl->lod_enabled ? m_impl->lod_policy.dither_band : 0.0F;
        char rb[256];
        (void)std::snprintf(static_cast<char*>(rb), sizeof(rb),
                            "rebase_table = %u\nrebase_stride = %u\nlod_slots = %u\n"
                            "instance_capacity_word = %u\ndither_band = %.6f\n",
                            kSceneDrawTableOff, kSceneDrawRowWords, m_impl->lod_slots, kHdrInstanceCapacity,
                            static_cast<double>(db));
        vs_skin.append(static_cast<const char*>(rb));
        vs_skin.append(vs_skin_body.c_str());
    }
    crd::vertcook::VertexProgramDesc skin_desc(m_impl->alloc);
    if (!cook_vs(m_impl->alloc, vs_skin.c_str(), nullptr, svg, sve, &skin_desc)) { CRD_LOG_ERROR(g_log_scenerender, "init_programs: skinned VS cook failed"); return false; }
    stamp_dither_varying(skin_desc);
    if (!contract_ok(skin_desc, static_cast<const crd::vertcook::VaryingRequirement*>(fwd_reqs), n_fwd_reqs))
    {
        CRD_LOG_ERROR(g_log_scenerender, "init_programs: skinned varying contract failed"); return false;
    }
    m_impl->vs_skinned = ctx.create_program(svg, sve);
    if (m_impl->vs_skinned != nullptr)
    {
        m_impl->program_skinned = m_impl->raster->create_raster_program(*m_impl->vs_skinned, *fs_flat);
    }

    // The TEXTURED variant is the SAME cooked material with `textured = true` — the base-colour map rides the
    // material SURFACE, not a second shader. Same VS, same technique, same BRDF.
    SceneShaderConfig tcfg = fcfg;
    tcfg.textured          = true;
    crd::vertcook::VaryingRequirement tex_reqs[crd::vertcook::kMaxVaryings];
    crd::u32                          n_tex_reqs = 0U;
    m_impl->fs_textured = m_impl->cook_fs(tcfg, tex_reqs, crd::vertcook::kMaxVaryings, &n_tex_reqs);
    if (m_impl->fs_textured != nullptr
        && contract_ok(scene_desc, static_cast<const crd::vertcook::VaryingRequirement*>(tex_reqs), n_tex_reqs))
    {
        m_impl->program_textured = m_impl->raster->create_raster_program(*m_impl->vs, *m_impl->fs_textured);
    }

    // REN-3.2-b: one shadow VARIANT per cascade. The cascade index is baked as a compile-time constant so the
    // VS reads its own light_vp slice out of the shared header — that is what lets all four cascade passes bind
    // the SAME storage buffer instead of needing four copies of the geometry.
    SceneShaderConfig depth_cfg;
    depth_cfg.pass    = crd::kir::cook::PassType::Shadow;
    m_impl->shadow_fs = m_impl->cook_fs(depth_cfg);
    if (csm == nullptr) { m_impl->shadow_fs = nullptr; }
    if (m_impl->shadow_fs != nullptr)
    {
        bool all_ok = true;
        // ⭐⭐ 38-G1 (user-directed): the shadow declaration resolves BY NAME (`assets/vertex/shadow.crdv`
        // shadows the embedded copy). The per-cascade variant is stamped on the PARSED desc, not spliced into
        // the text — the cascade is the renderer's pass semantics, the vocabulary is the asset's.
        crd::containers::String shadow_text(m_impl->alloc);
        if (!m_impl->asset_text("vertex/shadow.crdv", shadow_text)) { all_ok = false; }
        for (crd::u32 c = 0; all_ok && c < kMaxCascades; ++c)
        {
            crd::kir::KGraph shvg(m_impl->alloc);
            crd::kir::KEntry shve;
            crd::vertcook::VertexProgramDesc sdesc(m_impl->alloc);
            crd::containers::String          swhere(m_impl->alloc);
            if (crd::vertcook::parse_vertex_toml(
                    crd::containers::StringView(shadow_text.c_str(), shadow_text.size()), sdesc, &swhere)
                != crd::vertcook::VertexCookError::Ok)
            {
                all_ok = false;
                break;
            }
            // ⛔ The cascade is a COMPILE-TIME constant, so one storage binding serves every cascade pass:
            // the variant selects the header slice rather than a uniform selecting it per draw.
            sdesc.transform              = crd::vertcook::VertexTransform::LightVp;
            sdesc.cascade                = c;
            sdesc.instance_capacity_word = kHdrInstanceCapacity;
            // ⭐⭐ REN-40-C2: the LIST LAYOUT WIDTH. Cascade c's lists are at `(1 + c) * slots + slot`, and
            // the slot arrives per draw item through the table row — so the stage needs both the width
            // (cook-time) and the row stride (stamped with `rebase_table`).
            sdesc.lod_slots              = m_impl->lod_slots;
            sdesc.dither_band            = m_impl->lod_enabled ? m_impl->lod_policy.dither_band : 0.0F;
            // ⛔⛔ THE TABLE STAMPS WERE MISSING HERE, and the failure is the universal-table scar verbatim:
            // without `rebase_table`/`rebase_stride` the stage has no row to read, `row_slot` never emits, and
            // every cascade draw of every LOD slot reads slot 0's list with its own slot's COUNT — entries past
            // slot 0's valid length are stale, so the shadow pass pulls instances the cull never selected.
            sdesc.rebase_table           = kSceneDrawTableOff;
            sdesc.rebase_stride          = kSceneDrawRowWords;
            if (!crd::vertcook::cook_vertex_program(sdesc, shvg, shve)) { all_ok = false; break; }
            m_impl->shadow_vs[c] = ctx.create_program(shvg, shve);
            if (m_impl->shadow_vs[c] == nullptr) { all_ok = false; break; }
            m_impl->shadow_prog[c] =
                m_impl->raster->create_raster_program(*m_impl->shadow_vs[c], *m_impl->shadow_fs);
            if (m_impl->shadow_prog[c] == nullptr) { all_ok = false; break; }
        }
        // ⛔ All-or-nothing: a PARTIAL cascade set would render some cascades and silently drop others, which
        // looks like shadows fading out at a distance rather than like a failure.
        //
        // ⭐⭐ REN-37.4: the forward program that CONSUMES the atlas is the same cooked material shaded by the
        // authored `forward_csm` TECHNIQUE. The cascade count and PCF tap count are that technique's DECLARED
        // options, baked here as compile-time values so each choice cooks to its own fully-unrolled variant.
        SceneShaderConfig scfg;
        scfg.pass     = crd::kir::cook::PassType::Forward;
        scfg.tech     = csm;
        scfg.map_size = m_impl->csm.map_size;
        scfg.cascades = m_impl->csm.cascade_count;
        scfg.pcf_taps = m_impl->pcf_taps;
        scfg.blend_pct = m_impl->blend_pct; // REN-40-D
        scfg.soft_mode = m_impl->soft_mode;
        scfg.light_angle_x100 = m_impl->light_angle_x100;
        scfg.soft_max_texels  = m_impl->soft_max_texels;
        scfg.soft_search_taps = m_impl->soft_search_taps;
        scfg.fade_pct         = m_impl->fade_pct;
        crd::vertcook::VaryingRequirement sh_reqs[crd::vertcook::kMaxVaryings];
        crd::u32                          n_sh_reqs = 0U;
        m_impl->fs_shadowed = m_impl->cook_fs(scfg, sh_reqs, crd::vertcook::kMaxVaryings, &n_sh_reqs);
        m_impl->vs_shadowed = m_impl->vs.get(); // one VS, every variant
        if (m_impl->fs_shadowed != nullptr
            && contract_ok(scene_desc, static_cast<const crd::vertcook::VaryingRequirement*>(sh_reqs), n_sh_reqs))
        {
            m_impl->program_shadowed =
                m_impl->raster->create_raster_program(*m_impl->vs, *m_impl->fs_shadowed);
        }
        // ⭐⭐ REN-38: the COMBINED variant — the SAME cooked material with textured=true UNDER the SAME shadow
        // technique. The base-colour map rides bindings 1/2 and the atlas its own 4/5, so a group keeps its
        // texture AND its shadow; `record_one_group` stops having to null one of them.
        SceneShaderConfig tscfg = scfg;
        tscfg.textured          = true;
        crd::vertcook::VaryingRequirement ts_reqs[crd::vertcook::kMaxVaryings];
        crd::u32                          n_ts_reqs = 0U;
        m_impl->fs_textured_shadowed = m_impl->cook_fs(tscfg, ts_reqs, crd::vertcook::kMaxVaryings, &n_ts_reqs);
        if (m_impl->fs_textured_shadowed != nullptr
            && contract_ok(scene_desc, static_cast<const crd::vertcook::VaryingRequirement*>(ts_reqs), n_ts_reqs))
        {
            m_impl->program_textured_shadowed =
                m_impl->raster->create_raster_program(*m_impl->vs, *m_impl->fs_textured_shadowed);
        }
        // ⛔ Shadows need BOTH halves: the cascade writers AND the reader. Having only the writers would render
        // a shadow atlas nothing samples — pure cost, zero pixels changed.
        m_impl->shadow_programs_ok = all_ok && m_impl->program_shadowed != nullptr;

        // REN-40-F: skinned variants under shadows
        if (m_impl->vs_skinned != nullptr && m_impl->fs_shadowed != nullptr)
        {
            m_impl->program_skinned_shadowed =
                m_impl->raster->create_raster_program(*m_impl->vs_skinned, *m_impl->fs_shadowed);
        }
        if (m_impl->vs_skinned != nullptr && m_impl->fs_textured_shadowed != nullptr)
        {
            m_impl->program_skinned_textured_shadowed =
                m_impl->raster->create_raster_program(*m_impl->vs_skinned, *m_impl->fs_textured_shadowed);
        }
    }

    // REN-40-F: skinned + textured (shadow-independent)
    if (m_impl->vs_skinned != nullptr && m_impl->fs_textured != nullptr)
    {
        m_impl->program_skinned_textured =
            m_impl->raster->create_raster_program(*m_impl->vs_skinned, *m_impl->fs_textured);
    }

    // ── ⭐⭐ REN-39-C1: THE INDEXED PROGRAM SET — the SAME resolved declarations cooked `indexed = true` (one
    // prefix line, so a twin can never drift from its source) over the READ-ONLY fragment twins (the flag rides
    // the KEntry into the emitters and the content hash). ⛔ ALL-OR-NOTHING: a cascade pass draws every item
    // with ONE instance program, so a frame may never mix addressing modes — if any twin whose base cooked
    // fails, the switch turns OFF, logged, and the renderer keeps the proven pull path.
    if (m_impl->use_indexed && m_impl->program != nullptr)
    {
        bool ok = true;
        const auto cook_vs_text =
            [&](const char* prefix, const crd::containers::String& base, std::unique_ptr<crd::gpu::IGpuProgram>& out_vs)
        {
            crd::kir::KGraph ig(m_impl->alloc);
            crd::kir::KEntry ie;
            crd::containers::String t(m_impl->alloc);
            t.append(prefix);
            t.append(base.c_str());
            if (!cook_vs(m_impl->alloc, t.c_str(), nullptr, ig, ie))
            {
                return false;
            }
            out_vs = ctx.create_program(ig, ie);
            return out_vs != nullptr;
        };
        // the READ-ONLY fragment twins. The varying sets are IDENTICAL to their u0 siblings (the flag only
        // re-addresses the storage declaration), so the 38-D4 contract verified above covers them.
        SceneShaderConfig rocfg = fcfg;
        rocfg.storage_read_only = true;
        crd::gpu::IGpuProgram* fs_flat_ro = m_impl->cook_fs(rocfg);
        ok = fs_flat_ro != nullptr;
        crd::gpu::IGpuProgram* fs_tex_ro = nullptr;
        if (ok && m_impl->fs_textured != nullptr)
        {
            SceneShaderConfig rt = rocfg;
            rt.textured = true;
            fs_tex_ro = m_impl->cook_fs(rt);
            ok = fs_tex_ro != nullptr;
        }
        crd::gpu::IGpuProgram* fs_sh_ro = nullptr;
        crd::gpu::IGpuProgram* fs_tsh_ro = nullptr;
        if (ok && m_impl->fs_shadowed != nullptr && csm != nullptr)
        {
            SceneShaderConfig rs;
            rs.pass = crd::kir::cook::PassType::Forward;
            rs.tech = csm;
            rs.map_size = m_impl->csm.map_size;
            rs.cascades = m_impl->csm.cascade_count;
            rs.pcf_taps = m_impl->pcf_taps;
            rs.blend_pct = m_impl->blend_pct; // REN-40-D
            rs.soft_mode = m_impl->soft_mode;
            rs.light_angle_x100 = m_impl->light_angle_x100;
            rs.soft_max_texels  = m_impl->soft_max_texels;
            rs.soft_search_taps = m_impl->soft_search_taps;
            rs.fade_pct         = m_impl->fade_pct;
            rs.storage_read_only = true;
            fs_sh_ro = m_impl->cook_fs(rs);
            ok = fs_sh_ro != nullptr;
            if (ok && m_impl->fs_textured_shadowed != nullptr)
            {
                SceneShaderConfig rts = rs;
                rts.textured = true;
                fs_tsh_ro = m_impl->cook_fs(rts);
                ok = fs_tsh_ro != nullptr;
            }
        }
        // the indexed VERTEX twins + their raster pairs
        if (ok)
        {
            ok = cook_vs_text("indexed = true\n", vs_scene, m_impl->vs_idx);
        }
        if (ok)
        {
            m_impl->program_idx = m_impl->raster->create_raster_program(*m_impl->vs_idx, *fs_flat_ro);
            ok = m_impl->program_idx != nullptr;
        }
        if (ok && m_impl->program_rebased != nullptr)
        {
            ok = cook_vs_text("indexed = true\n", vs_scene, m_impl->vs_rebased_idx);
            if (ok)
            {
                m_impl->program_rebased_idx =
                    m_impl->raster->create_raster_program(*m_impl->vs_rebased_idx, *fs_flat_ro);
                ok = m_impl->program_rebased_idx != nullptr;
            }
        }
        if (ok && m_impl->program_skinned != nullptr)
        {
            ok = cook_vs_text("indexed = true\n", vs_skin, m_impl->vs_skinned_idx);
            if (ok)
            {
                m_impl->program_skinned_idx =
                    m_impl->raster->create_raster_program(*m_impl->vs_skinned_idx, *fs_flat_ro);
                ok = m_impl->program_skinned_idx != nullptr;
            }
            // REN-40-F: skinned indexed variants for textures and shadows. These are OPTIONAL — a
            // failure falls back to the base skinned indexed program, never breaks ALL-OR-NOTHING.
            if (ok && fs_tex_ro != nullptr && m_impl->program_skinned_textured != nullptr)
            {
                m_impl->program_skinned_textured_idx =
                    m_impl->raster->create_raster_program(*m_impl->vs_skinned_idx, *fs_tex_ro);
            }
            if (ok && fs_sh_ro != nullptr && m_impl->program_skinned_shadowed != nullptr)
            {
                m_impl->program_skinned_shadowed_idx =
                    m_impl->raster->create_raster_program(*m_impl->vs_skinned_idx, *fs_sh_ro);
            }
            if (ok && fs_tsh_ro != nullptr && m_impl->program_skinned_textured_shadowed != nullptr)
            {
                m_impl->program_skinned_textured_shadowed_idx =
                    m_impl->raster->create_raster_program(*m_impl->vs_skinned_idx, *fs_tsh_ro);
            }
        }
        if (ok && m_impl->program_textured != nullptr && fs_tex_ro != nullptr)
        {
            m_impl->program_textured_idx = m_impl->raster->create_raster_program(*m_impl->vs_idx, *fs_tex_ro);
            ok = m_impl->program_textured_idx != nullptr;
            if (!ok) { CRD_LOG_ERROR(g_log_scenerender, "REN-39: program_textured_idx create failed"); }
        }
        if (ok && m_impl->program_shadowed != nullptr && fs_sh_ro != nullptr)
        {
            m_impl->program_shadowed_idx = m_impl->raster->create_raster_program(*m_impl->vs_idx, *fs_sh_ro);
            ok = m_impl->program_shadowed_idx != nullptr;
            if (!ok) { CRD_LOG_ERROR(g_log_scenerender, "REN-39: program_shadowed_idx create failed"); }
        }
        if (ok && m_impl->program_textured_shadowed != nullptr && fs_tsh_ro != nullptr)
        {
            m_impl->program_textured_shadowed_idx = m_impl->raster->create_raster_program(*m_impl->vs_idx, *fs_tsh_ro);
            ok = m_impl->program_textured_shadowed_idx != nullptr;
            if (!ok) { CRD_LOG_ERROR(g_log_scenerender, "REN-39: program_textured_shadowed_idx create failed"); }
        }
        // ── the DEPTH PREPASS programs: camera VS + a depth-only FS. ⛔⛔ Without these the prepass fell back to
        // the items' FORWARD programs — a texture-sampling FS in a pass that binds no textures, which the driver
        // dead-coded until the dither discard forced it to execute (see DrawItem::program_depth). When the LOD
        // dither is on, the FS carries the SAME Bayer discard, so prepass depth exists exactly where the forward
        // draw will keep pixels.
        if (ok)
        {
            SceneShaderConfig pp;
            pp.pass              = crd::kir::cook::PassType::Shadow;
            pp.storage_read_only = true;
            pp.dither_depth      = true;
            pp.dither_band = (m_impl->lod_enabled && m_impl->lod_slots > 1U) ? m_impl->lod_policy.dither_band : 0.0F;
            crd::gpu::IGpuProgram* fs_pp = m_impl->cook_fs(pp);
            ok = fs_pp != nullptr;
            if (ok)
            {
                m_impl->program_prepass_idx = m_impl->raster->create_raster_program(*m_impl->vs_idx, *fs_pp);
                ok = m_impl->program_prepass_idx != nullptr;
            }
            if (ok && m_impl->vs_skinned_idx != nullptr)
            {
                m_impl->program_prepass_skinned_idx =
                    m_impl->raster->create_raster_program(*m_impl->vs_skinned_idx, *fs_pp);
                ok = m_impl->program_prepass_skinned_idx != nullptr;
            }
            if (!ok) { CRD_LOG_ERROR(g_log_scenerender, "depth-prepass program create failed"); }
        }
        // ── ⭐⭐ REN-41 (velocity, path A): the MOTION-VECTOR twins — velocity VS assets + the velocity FS,
        // cooked `indexed = true` with the SAME LOD/dither prefix the scene VS carries (so the VS reads the draw
        // table row and emits `fade`@4, and the FS reads the exact same fade for its discard). ⛔ OPTIONAL: this
        // never sets `ok = false` — a velocity cook failure leaves the programs null and the resolve keeps its
        // neighbourhood clamp; it must never break the depth prepass or the ALL-OR-NOTHING indexed switch.
        if (ok)
        {
            crd::gpu::IGpuProgram* fs_vel = m_impl->cook_velocity_fs();
            const auto build_vel_vs = [&](const char* asset, std::unique_ptr<crd::gpu::IGpuProgram>& out_vs) -> bool
            {
                crd::containers::String body(m_impl->alloc);
                if (!m_impl->asset_text(asset, body)) { return false; }
                const crd::f32 db = m_impl->lod_enabled ? m_impl->lod_policy.dither_band : 0.0F;
                char rb[256];
                (void)std::snprintf(static_cast<char*>(rb), sizeof(rb),
                                    "indexed = true\nrebase_table = %u\nrebase_stride = %u\nlod_slots = %u\n"
                                    "instance_capacity_word = %u\ndither_band = %.6f\n",
                                    kSceneDrawTableOff, kSceneDrawRowWords, m_impl->lod_slots, kHdrInstanceCapacity,
                                    static_cast<double>(db));
                crd::containers::String t(m_impl->alloc);
                t.append(static_cast<const char*>(rb));
                t.append(body.c_str());
                crd::kir::KGraph vvg(m_impl->alloc);
                crd::kir::KEntry vve;
                if (!cook_vs(m_impl->alloc, t.c_str(), nullptr, vvg, vve)) { return false; }
                out_vs = ctx.create_program(vvg, vve);
                return out_vs != nullptr;
            };
            if (fs_vel != nullptr && build_vel_vs("vertex/velocity.crdv", m_impl->vs_velocity_idx))
            {
                m_impl->program_velocity_idx =
                    m_impl->raster->create_raster_program(*m_impl->vs_velocity_idx, *fs_vel);
            }
            if (fs_vel != nullptr && m_impl->program_velocity_idx != nullptr
                && build_vel_vs("vertex/velocity_skinned.crdv", m_impl->vs_skinned_velocity_idx))
            {
                m_impl->program_skinned_velocity_idx =
                    m_impl->raster->create_raster_program(*m_impl->vs_skinned_velocity_idx, *fs_vel);
            }
            if (m_impl->program_velocity_idx == nullptr)
            {
                CRD_LOG_ERROR(g_log_scenerender, "REN-41: velocity program cook failed — motion vectors disabled");
            }
        }
        // the shadow CASCADE twins — the same parsed declaration, `indexed` stamped beside cascade (the 38-G1
        // stamping rule: the variant is the renderer's pass semantics, the vocabulary is the asset's)
        if (ok && m_impl->shadow_programs_ok)
        {
            SceneShaderConfig rd;
            rd.pass = crd::kir::cook::PassType::Shadow;
            rd.storage_read_only = true;
            crd::gpu::IGpuProgram* sfs_ro = m_impl->cook_fs(rd);
            crd::containers::String sh_text(m_impl->alloc);
            ok = sfs_ro != nullptr && m_impl->asset_text("vertex/shadow.crdv", sh_text);
            if (!ok) { CRD_LOG_ERROR(g_log_scenerender, "REN-39: shadow FS or vertex text failed"); }
            for (crd::u32 c = 0; ok && c < kMaxCascades; ++c)
            {
                crd::kir::KGraph sg(m_impl->alloc);
                crd::kir::KEntry se;
                crd::vertcook::VertexProgramDesc sdesc(m_impl->alloc);
                crd::containers::String sw(m_impl->alloc);
                ok = crd::vertcook::parse_vertex_toml(crd::containers::StringView(sh_text.c_str(), sh_text.size()),
                                                      sdesc, &sw) == crd::vertcook::VertexCookError::Ok;
                if (!ok)
                {
                    break;
                }
                sdesc.transform = crd::vertcook::VertexTransform::LightVp;
                sdesc.cascade = c;
                sdesc.instance_capacity_word = kHdrInstanceCapacity;
                sdesc.lod_slots              = m_impl->lod_slots; // REN-40-C2, the indexed twin
                sdesc.dither_band            = m_impl->lod_enabled ? m_impl->lod_policy.dither_band : 0.0F;
                // ⛔⛔ the same missing table stamps the non-indexed variant had (see the note there): without
                // them every cascade draw of every LOD slot reads slot 0's list with its own slot's count.
                sdesc.rebase_table           = kSceneDrawTableOff;
                sdesc.rebase_stride          = kSceneDrawRowWords;
                sdesc.indexed = true;
                ok = crd::vertcook::cook_vertex_program(sdesc, sg, se);
                if (!ok)
                {
                    break;
                }
                m_impl->shadow_vs_idx[c] = ctx.create_program(sg, se);
                ok = m_impl->shadow_vs_idx[c] != nullptr;
                if (!ok)
                {
                    break;
                }
                m_impl->shadow_prog_idx[c] = m_impl->raster->create_raster_program(*m_impl->shadow_vs_idx[c], *sfs_ro);
                ok = m_impl->shadow_prog_idx[c] != nullptr;
                if (!ok) { CRD_LOG_ERROR(g_log_scenerender, "REN-39: shadow_prog_idx create failed"); }
            }
        }
        if (!ok)
        {
            CRD_LOG_ERROR(g_log_scenerender,
                          "REN-39: indexed program set incomplete — keeping the pull path (all-or-nothing)");
            m_impl->use_indexed = false;
            // ⛔ drop the PARTIAL set — a later set_indexed_pull(true) over half a set would mix addressing
            // modes inside one cascade pass, the exact hazard all-or-nothing exists to prevent
            m_impl->program_idx.reset();
            m_impl->program_rebased_idx.reset();
            m_impl->program_skinned_idx.reset();
            m_impl->program_skinned_textured_idx.reset();
            m_impl->program_skinned_shadowed_idx.reset();
            m_impl->program_skinned_textured_shadowed_idx.reset();
            m_impl->program_textured_idx.reset();
            m_impl->program_shadowed_idx.reset();
            m_impl->program_textured_shadowed_idx.reset();
            m_impl->program_prepass_idx.reset();
            m_impl->program_prepass_skinned_idx.reset();
            for (crd::u32 c = 0; c < kMaxCascades; ++c)
            {
                m_impl->shadow_prog_idx[c].reset();
            }
        }
    }
    return m_impl->program != nullptr;
}

// REN-39-C1: the indexed-pull switch (see the header). Honoured only while the indexed set exists — flipping ON
// after a failed/skipped cook keeps the pull path (the fill chain null-checks the twin it picks).
void SceneRenderer::set_indexed_pull(bool on) noexcept
{
    if (m_impl->use_indexed != on)
    {
        m_impl->use_indexed = on;
        for (crd::u32 c = 0; c < kMaxCascades; ++c)
        {
            m_impl->prev_light_vp[c] = {};
            m_impl->cascades.light_vp[c] = {};
        }
        m_impl->csm_frame = 0;
    }
}

// ⭐⭐ REN-40-A: the GPU-driven cull switch. ⛔ It requires the INDEXED path — a GPU-written command IS an
// indexed-indirect command, and the classic pull draw has no count field to source from device memory.
void SceneRenderer::set_gpu_cull(bool on) noexcept
{
    m_impl->gpu_cull_on = on;
}

// ⭐⭐ REN-41 Stage 4 (S4-0): render a device cluster buffer through the Nanite mesh path. One mesh workgroup per
// packed cluster; the shader unpacks its ≤128 tris and self-selects the leaves. Binds `cluster_buf` at set 0 /
// binding 0 (what the mesh shader's storage loads read) via `draw_mesh_storage`.
void SceneRenderer::draw_clusters(crd::gpu::IRasterTarget& target, crd::gpu::IStorageBuffer& cluster_buf,
                                  crd::u32 cluster_count, crd::gpu::ClearColor clear)
{
    if (m_impl->raster == nullptr || cluster_count == 0U) { return; }
    crd::gpu::IRasterProgram* prog = m_impl->ensure_cluster_mesh_program();
    if (prog == nullptr) { return; } // no mesh-shader support on this device
    m_impl->raster->draw_mesh_storage(target, *prog, clear, cluster_buf, cluster_count);
}

bool SceneRenderer::supports_clusters()
{
    return m_impl->ensure_cluster_mesh_program() != nullptr;
}

bool SceneRenderer::gpu_cull() const noexcept
{
    return m_impl->gpu_cull_on && m_impl->use_indexed;
}

bool SceneRenderer::set_lod_policy_asset(const char* asset_name)
{
    if (asset_name == nullptr || m_impl == nullptr) { return false; }
    Impl&                   impl = *m_impl;
    crd::containers::String text(impl.alloc);
    if (!impl.asset_text(asset_name, text))
    {
        CRD_LOG_ERROR(g_log_scenerender, "set_lod_policy_asset: no such asset '{}'", asset_name);
        return false;
    }
    crd::lod::LodPolicy     p{};
    crd::containers::String where(impl.alloc);
    const auto              err =
        crd::lod::parse_lod_toml(crd::containers::StringView(text.c_str(), text.size()), p, &where);
    if (err != crd::lod::LodCookError::Ok)
    {
        // ⛔ NAMED, and nothing installed. A policy that half-applied would leave
        // chains built to switch distances nobody declared.
        CRD_LOG_ERROR(g_log_scenerender, "set_lod_policy_asset '{}': {} (at '{}')", asset_name,
                      crd::lod::lod_cook_error_text(err), where.c_str());
        return false;
    }
    impl.lod_policy  = p;
    impl.lod_enabled = true;
    // ⭐⭐ REN-40-C2: the LAYOUT WIDTH comes from the POLICY, which is authored. ⛔ Not `kMaxLodSlots`: every slot
    // costs one visible list of `capacity` per view (5 views x capacity x 4 B), so reserving eight when the
    // policy declares four would double the scene buffer for nothing. And not "the deepest chain actually built"
    // either — the kernels are cooked before any mesh is loaded, so the number has to be knowable up front. The
    // policy is the one place that knows it and is content rather than code.
    impl.lod_slots = p.extra_levels + 1U;
    // ⭐⭐ REN-40-C5: the impostor adds ONE MORE SLOT at the end of the chain. The cull kernel's LOD selection
    // naturally routes instances below the coarsest mesh level's switch height into this slot; the draw path
    // serves it with a billboard program instead of a mesh pull program.
    if (p.impostor_grid > 0U) { impl.lod_slots += 1U; }
    if (impl.lod_slots > kMaxLodSlots) { impl.lod_slots = kMaxLodSlots; }
    CRD_LOG_INFO(g_log_scenerender, "LOD policy '{}' installed: {} extra levels ({} slots{}), identity {:016x}",
                 asset_name, p.extra_levels, impl.lod_slots,
                 p.impostor_grid > 0U ? ", impostor" : "", crd::lod::lod_policy_identity(p));
    return true;
}

bool SceneRenderer::lod_enabled() const noexcept { return m_impl != nullptr && m_impl->lod_enabled; }

SceneRenderer::LodChainInfo SceneRenderer::lod_chain_info() const noexcept
{
    LodChainInfo info{};
    if (m_impl == nullptr) { return info; }
    for (const MeshGroup& g : m_groups)
    {
        ++info.groups;
        if (g.lod_count <= 1U) { continue; }
        ++info.groups_with_lod;
        if (g.lod_count > info.levels_max) { info.levels_max = g.lod_count; }
        info.tris_level0 += g.lod_indices[0] / 3U;
        info.tris_coarsest += g.lod_indices[g.lod_count - 1U] / 3U;
    }
    return info;
}

void SceneRenderer::set_gpu_cull_verify(bool on) noexcept { m_impl->gpu_cull_verify = on; }
bool SceneRenderer::gpu_cull_verify() const noexcept { return m_impl->gpu_cull_verify; }

void SceneRenderer::set_gpu_skinning(bool on) noexcept { m_impl->gpu_skinning_on = on; }
bool SceneRenderer::gpu_skinning() const noexcept { return m_impl->gpu_skinning_on; }

// ⭐⭐ REN-40-A: the device's own verdict, read back. See `GpuCullCounts` for why this is part of the feature.
bool SceneRenderer::read_gpu_cull_counts(GpuCullCounts& out) const
{
    out = GpuCullCounts{};
    const Impl& impl = *m_impl;
    if (impl.raster == nullptr) { return false; }
    const crd::u32 stride_w = impl.raster->indirect_command_stride() / 4U;
    const crd::u32 argw     = kCullArgsHeaderWords + (impl.raster->indirect_command_arg_offset() / 4U);
    out.views               = 1U + kMaxCascades;
    bool any                = false;
    // ⛔⛔ REN-40-C2: `draw_groups` is index-parallel with the DRAW LIST, and the list now carries one item per
    // (group, LOD slot) — so walking it naively counts every group `slots` times and reports a device cull that
    // "found" four times what the CPU did. Groups are contiguous in the list, so remembering the last pointer is
    // enough; the SLOT sum below is the real per-view total.
    const MeshGroup* prev_g = nullptr;
    for (crd::usize gi = 0; gi < impl.draw_groups.size(); ++gi)
    {
        MeshGroup* g = impl.draw_groups[gi];
        if (g == nullptr || g == prev_g || g->cull_args == nullptr) { continue; }
        prev_g = g;
        if (!impl.raster->download_storage(*g->cull_args)) { continue; }
        if (!any)
        {
            out.args_size = g->cull_args->size_bytes();
            for (crd::u32 w = 0; w < 16U && (w + 1U) * 4U <= out.args_size; ++w)
            {
                out.raw_args[w] = g->cull_args->read_u32(w);
            }
        }
        // ⛔ `read_u32` reads the HOST MIRROR — without the download above it answers whatever was last uploaded,
        // which is a confident zero. That mistake made this gate report an empty cull on a cull that worked.
        for (crd::u32 v = 0; v < out.views; ++v)
        {
            // ⭐⭐ A VIEW'S SURVIVORS ARE THE SUM OVER ITS LOD SLOTS — every instance lands in exactly one, so
            // the sum is what must equal the CPU cull's count for that view. That equality is the ONLY check
            // that can see a mis-selected level or a doubly-appended survivor: the pixels cannot, because
            // drawing one instance twice at the same depth is invisible.
            for (crd::u32 s = 0; s < impl.lod_slots; ++s)
            {
                const crd::u32 aw = ((v * impl.lod_slots + s) * stride_w) + argw;
                out.instances[v] += g->cull_args->read_u32(aw + 1U);
                // ⭐⭐ VIEW 0's COMMANDS, SLOT BY SLOT. A per-view TOTAL cannot distinguish "the cull never chose
                // slot 2" from "slot 2's command is empty" — and those have completely different causes (the
                // selector vs the reset's LOD-table read). This is the line that separates them.
                if (v == 0U && s < 8U)
                {
                    out.slot_instances[s] += g->cull_args->read_u32(aw + 1U);
                    if (!any)
                    {
                        out.slot_indices[s] = g->cull_args->read_u32(aw + 0U);
                        out.slot_first[s]   = g->cull_args->read_u32(aw + 2U);
                    }
                }
                if (!any && s == 0U)
                {
                    out.indices[v]     = g->cull_args->read_u32(aw + 0U);
                    out.first_index[v] = g->cull_args->read_u32(aw + 2U);
                }
            }
        }
        // ⭐⭐ and the INPUT, for the first group we can read: the device's copy of the world AABBs against the
        // CPU's. See `GpuCullCounts::bounds_mismatch`.
        if (out.bounds_checked == 0U && g->buffer != nullptr && g->world_bounds.size() > 0U
            && impl.raster->download_storage(*g->buffer))
        {
            const crd::u32 base = (g->region_base != 0U ? g->region_base : 0U) + g->bounds_off;
            const auto     n    = static_cast<crd::u32>(g->world_bounds.size());
            for (crd::u32 i = 0; i < n; ++i)
            {
                const crd::f32 want[6] = {g->world_bounds[i].min.x, g->world_bounds[i].min.y,
                                          g->world_bounds[i].min.z, g->world_bounds[i].max.x,
                                          g->world_bounds[i].max.y, g->world_bounds[i].max.z};
                bool           same    = true;
                for (crd::u32 k = 0; k < 6U; ++k)
                {
                    const crd::u32 w = g->buffer->read_u32(base + (i * 6U) + k);
                    crd::f32       f = 0.0F;
                    std::memcpy(&f, &w, 4U);
                    if (f != want[k]) { same = false; }
                }
                ++out.bounds_checked;
                if (!same) { ++out.bounds_mismatch; }
            }
        }
        if (out.header_w0 == 0U && g->buffer != nullptr && impl.raster->download_storage(*g->buffer))
        {
            const crd::u32 hbase = g->region_base != 0U ? g->region_base : 0U;
            out.header_w0 = g->buffer->read_u32(hbase + 0U);
            out.header_w2 = g->buffer->read_u32(hbase + 2U);
        }
        // the CPU's verdict for the SAME group, this frame — see `GpuCullCounts::cpu_instances`
        out.cpu_instances[0] += g->visible_count_cpu;
        for (crd::u32 c = 0; c < kMaxCascades; ++c) { out.cpu_instances[1U + c] += g->cascade_visible_count[c]; }
        any = true;
        ++out.groups;
    }
    out.fill_cull_gates    = impl.fill_diag_cull_gates;
    out.fill_dispatch_max  = impl.fill_diag_dispatch_max;
    out.fill_total_items   = impl.fill_diag_total_items;
    out.fill_index_count_0 = impl.fill_diag_index_count_0;
    out.fill_record_ok     = impl.fill_diag_record_ok;
    out.fill_build_ok      = impl.fill_diag_build_ok;
    out.fill_pass_count    = impl.fill_diag_pass_count;
    out.occ_step           = impl.fill_diag_occ_step;
    return any;
}

// REN-39 (the gizmo fix): resolve the woven overlay pass's DECLARED image for the app callback (see header).
crd::gpu::IRasterTarget* SceneRenderer::overlay_target(crd::gpu::IFrameContext& ctx) const noexcept
{
    if (m_impl == nullptr || !m_impl->overlay_img.valid()) { return nullptr; }
    return ctx.image(m_impl->overlay_img);
}

namespace
{

// the chunk-visitor context for the two extraction passes
struct ExtractCtx
{
    crd::scene::World*        world = nullptr;
    SceneRenderer::Impl*      impl  = nullptr;
    crd::containers::Array<MeshGroup>* groups = nullptr;
    crd::scene::ComponentId   transform_id{};
    crd::scene::ComponentId   renderer_id{};
    crd::scene::ComponentId   animator_id{}; // GEO-8
    crd::scene::ComponentId   lod_override_id{}; // REN-40-C2 (OPTIONAL — absent chunks surface nullptr)
    SyncStats*                stats = nullptr;

    // pass 1 outputs
    crd::u64 sig       = kFnvOffset;
    bool     any_dirty = false;

    // rebuild-pass scratch: per-(chunk × group) run starts
    crd::containers::Array<crd::u32> run_first; // parallel to groups, valid within one chunk visit

    explicit ExtractCtx(crd::memory::IAllocator* a) : run_first(a) {}
};

// ⭐⭐ REN-40-B: fold ONE u64 into the running signature. The old signature hashed every entity id and every
// MeshRenderer byte in the world every frame; this is the same FNV over 8 bytes per FACT instead.
inline void hash_u64_into(crd::u64& h, crd::u64 v) noexcept
{
    for (crd::u32 i = 0; i < 8U; ++i)
    {
        h ^= (v >> (i * 8U)) & 0xFFULL;
        h *= kFnvPrime;
    }
}

// resolve-or-create the group for a mesh id (loads the mesh resource; null on load failure)
// ⭐⭐ REN-40-A: push a RANGE of per-instance world AABBs into the group buffer's bounds section.
// ⛔ Six floats per instance (min.xyz, max.xyz) — the SAME box `aabb_in_frustum` reads on the CPU, so the GPU
// cull is testing one truth rather than a re-derivation. Called on exactly the grain the instance payload uses,
// because bounds that lag their transforms would cull against last frame's positions — geometry popping in and
// out for a frame, which reads as a culling bug and is really a staleness bug.
void upload_bounds_range(SceneRenderer::Impl& impl, MeshGroup& group, crd::u32 first, crd::u32 n,
                         SyncStats& stats)
{
    if (n == 0U || group.buffer == nullptr || impl.raster == nullptr) { return; }
    if (first + n > group.world_bounds.size()) { return; }
    // ⛔ REN-40-B: a REUSED staging array. This allocated a fresh one per call, so a structural frame at 1M
    // instances did a 24 MB allocate/free inside the upload loop.
    crd::containers::Array<crd::f32>& tmp = impl.bounds_staging;
    tmp.resize(static_cast<crd::usize>(n) * 6U, 0.0F);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const auto& b = group.world_bounds[first + i];
        tmp[static_cast<crd::usize>(i) * 6U + 0U] = b.min.x;
        tmp[static_cast<crd::usize>(i) * 6U + 1U] = b.min.y;
        tmp[static_cast<crd::usize>(i) * 6U + 2U] = b.min.z;
        tmp[static_cast<crd::usize>(i) * 6U + 3U] = b.max.x;
        tmp[static_cast<crd::usize>(i) * 6U + 4U] = b.max.y;
        tmp[static_cast<crd::usize>(i) * 6U + 5U] = b.max.z;
    }
    const crd::u32 bytes = n * 6U * 4U;
    (void)impl.raster->upload_storage(*group.buffer, (group.bounds_off + first * 6U) * 4U, tmp.data(), bytes);
    stats.uploaded_bytes += bytes;
    // ⭐⭐ REN-40-C2: the per-instance LOD OVERRIDE rides the SAME grain, for the same reason the bounds do — an
    // override that lagged its entity would select against the previous frame's policy, which reads as a level
    // popping one frame late rather than as a staleness bug.
    if (group.lod_override_off != 0U && (first + n) * 2U <= group.lod_override.size())
    {
        const crd::u32 ob = n * 2U * 4U;
        (void)impl.raster->upload_storage(*group.buffer, (group.lod_override_off + first * 2U) * 4U,
                                          group.lod_override.data() + static_cast<crd::usize>(first) * 2U, ob);
        stats.uploaded_bytes += ob;
    }
}

[[nodiscard]] crd::i64 group_for_mesh(ExtractCtx& ctx, const crd::resources::ResourceId& mesh_id)
{
    if (const crd::u32* found = ctx.impl->group_of_mesh.find(mesh_id)) { return static_cast<crd::i64>(*found); }

    auto handle = ctx.impl->rm->load_sync<crd::resources::MeshResource>(mesh_id);
    if (handle.state() != crd::resources::LoadState::Ready || handle.get() == nullptr)
    {
        ++ctx.stats->meshes_pending;
        return -1;
    }
    const crd::resources::MeshResource* mesh = handle.get();
    if (mesh->indices.size() < 4U || !mesh->has_bounds()) { return -1; }

    MeshGroup group(ctx.impl->alloc);
    group.mesh_id     = mesh_id;
    group.mesh        = handle; // keeps the payload resident

    // ⭐⭐ REN-40-C2: BUILD THE MESH'S LOD CHAIN, ONCE, HERE. The resource is shared
    // and cached, so the chain is built on first use and every later group that
    // names the same mesh finds it already there (`build_lod_chain` REFUSES a second
    // build rather than appending a second chain onto the same streams).
    // ⛔ Built on the RESOURCE, not on the group: two groups of the same mesh must
    // not decimate it twice, and the index ranges are properties of the geometry.
    if (ctx.impl->lod_enabled && mesh->lods.size() == 0U && !mesh->has_skin())
    {
        // ⛔ SKINNED MESHES ARE EXCLUDED, deliberately and reportedly: the chain
        // carries no skin stream, so a decimated level would have no joints or
        // weights and would collapse to the bind pose. Skinned LOD is 40-F.
        // ⛔ The const_cast is DELIBERATE and bounded, and it is not hidden: `ResourceHandle::get()` is const
        // because a resource is SHARED and essentially every consumer must not write it, so the handle
        // deliberately offers no mutable accessor. This is the one legitimate writer — the chain is appended to
        // the mesh's OWN streams, EXACTLY ONCE (`build_lod_chain` refuses a second build), on the load path
        // before any consumer has drawn the mesh. Adding a public `get_mut()` for it would hand every consumer a
        // write pointer to shared payload to serve one caller; keeping the exception here, named, means a grep
        // for this cast finds every writer.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — see above: the sole one-time chain builder
        auto& mutable_mesh = const_cast<crd::resources::MeshResource&>(*mesh);
        const auto rep     = crd::lod::build_lod_chain(mutable_mesh, ctx.impl->lod_policy, ctx.impl->alloc);
        if (rep.status != crd::lod::LodBuildStatus::Ok)
        {
            // ⛔ REPORTED. A mesh that silently stayed one-level would show up only
            // as "LOD did nothing" on the fps board, with no thread back to here.
            CRD_LOG_WARN(g_log_scenerender, "LOD chain REFUSED: status {} (levels built {})",
                         static_cast<crd::u32>(rep.status), rep.levels_built);
        }
        else
        {
            CRD_LOG_INFO(g_log_scenerender, "LOD chain: {} levels, {} -> {} tris", rep.levels_built,
                         rep.triangles[0], rep.triangles[rep.levels_built - 1U]);
        }
    }

    // ⛔⛔ `index_count` IS LEVEL 0's, not the whole buffer's. With a chain the index
    // stream holds every level end to end, and drawing all of it would render every
    // level on top of itself — a mesh that looks right, costs several times what it
    // should, and z-fights with its own coarse copy.
    group.index_count = mesh->lods.size() > 0U
                            ? mesh->lods[0].index_count
                            : static_cast<crd::u32>(mesh->indices.size() / 4U);
    group.lod_count   = static_cast<crd::u32>(mesh->lods.size() < kMaxLodSlots ? mesh->lods.size() : kMaxLodSlots);
    group.lod_hysteresis = ctx.impl->lod_enabled ? ctx.impl->lod_policy.hysteresis : 0.0F;
    for (crd::u32 l = 0; l < group.lod_count; ++l)
    {
        group.lod_first[l]   = mesh->lods[l].first_index;
        group.lod_indices[l] = mesh->lods[l].index_count;
        group.lod_height[l]  = mesh->lods[l].screen_height;
    }
    // ⭐⭐ REN-40-C5: the IMPOSTOR SLOT — one past the coarsest mesh level. Its switch height is the coarsest
    // mesh level's threshold (the implicit threshold from the .crdlod doc), and its index range is zero (the
    // billboard VS generates its own geometry). The cull kernel's unrolled loop treats it as just another level
    // whose height happens to be the one below which all mesh levels have already been exhausted.
    if (ctx.impl->lod_policy.impostor_grid > 0U && group.lod_count > 1U)
    {
        const crd::u32 imp = group.lod_count;
        if (imp < kMaxLodSlots)
        {
            group.lod_first[imp]   = static_cast<crd::u32>(mesh->indices.size() / 4U);
            group.lod_indices[imp] = 6U;
            group.lod_height[imp]  = group.lod_height[group.lod_count - 1U];
            group.lod_count        = imp + 1U;
            group.has_impostor     = true;
        }
    }
    ctx.groups->push_back(static_cast<MeshGroup&&>(group));
    const crd::u32 gi = static_cast<crd::u32>(ctx.groups->size() - 1U);
    ctx.impl->group_of_mesh.insert(mesh_id, gi);
    return static_cast<crd::i64>(gi);
}

// write one instance record + world AABB into a group slot
void write_slot(MeshGroup& group, crd::u32 slot, const crd::scene::Transform& t, const crd::math::Vec4f& color,
                const crd::scene::MeshLodOverride* ovr)
{
    // ⭐⭐ REN-40-C2: the per-entity LOD override, packed for the device. ⛔ `ovr == nullptr` is the COMMON case
    // (the component is optional and most entities never carry one), and it must cost a constant, not a branch
    // in the kernel — so the default rides the same two words every other slot writes.
    {
        // ⭐⭐ REN-40-C4: HYSTERESIS AS A PER-INSTANCE THRESHOLD JITTER — the transition, without any history.
        // ⛔⛔ WHAT MAKES A LEVEL CHANGE VISIBLE AT A MILLION INSTANCES IS NOT THE CHANGE, IT IS THE SYNCHRONY.
        // Every instance of a mesh shares one switch height, so a camera moving forward flips a whole BAND of
        // them on the same frame: a wave sweeping across the field, which reads as the world rebuilding itself.
        // A per-pixel dissolve does not fix that — it fades a wave instead of cutting one.
        // ⭐ Giving each instance its OWN boundary inside a narrow band breaks the synchrony at its source: the
        // population crosses over a RANGE of distances, so at any instant a handful of instances are changing
        // instead of thousands. And it costs nothing — no per-instance history, no read-modify-write on a
        // stateless kernel over a million instances, because the offset is a pure function of the SLOT INDEX
        // and therefore stable frame to frame, which is what stops it becoming crawling noise.
        // ⛔ It MULTIPLIES the author's bias rather than replacing it: an entity that asked to stay sharp still
        // does, it just does not switch on the same frame as its neighbour.
        crd::f32 jitter = 1.0F;
        if (group.lod_hysteresis > 0.0F)
        {
            // splitmix32 over the slot — deterministic, well-distributed, and no state
            crd::u32 h = slot * 0x9E3779B9U;
            h ^= h >> 16U;
            h *= 0x7FEB352DU;
            h ^= h >> 15U;
            h *= 0x846CA68BU;
            h ^= h >> 16U;
            const crd::f32 u = static_cast<crd::f32>(h >> 8U) * (1.0F / 16777216.0F); // [0,1)
            jitter           = 1.0F + (group.lod_hysteresis * (u - 0.5F));
        }
        const crd::f32 bias = (ovr != nullptr ? ovr->screen_bias : 1.0F) * jitter;
        crd::u32       bits = 0U;
        std::memcpy(&bits, static_cast<const void*>(&bias), 4U);
        const crd::u32 lo = ovr != nullptr ? (ovr->min_level & 0xFFU) : 0U;
        const crd::u32 hi = ovr != nullptr ? (ovr->max_level & 0xFFU) : (kMaxLodSlots - 1U);
        group.lod_override[static_cast<crd::usize>(slot) * 2U + 0U] = bits;
        group.lod_override[static_cast<crd::usize>(slot) * 2U + 1U] = lo | (hi << 8U);
    }
    InstanceGpu& rec = group.instances[slot];
    // ⭐⭐ REN-41 (velocity): snapshot last frame's transform BEFORE the current one overwrites it, so the velocity
    // prepass can form `prev_clip = prev_vp · prev_world · pos`. A FRESH slot (rec.world zero-initialised, so the
    // homogeneous [15] element is 0 rather than the 1 every affine transform carries) has no previous frame — it
    // shadows itself, yielding zero velocity on spawn instead of a spike reprojected from the origin.
    if (static_cast<crd::usize>(slot) * 16U + 16U <= group.prev_world.size())
    {
        const bool      fresh = (rec.world[15] == 0.0F);
        const crd::f32* src   = fresh ? reinterpret_cast<const crd::f32*>(&t.world) : rec.world;
        std::memcpy(group.prev_world.data() + static_cast<crd::usize>(slot) * 16U, src, 16U * 4U);
    }
    std::memcpy(rec.world, &t.world, sizeof(rec.world)); // Mat4f = 4 packed Vec4 columns = column-major floats
    rec.color[0] = color.x;
    rec.color[1] = color.y;
    rec.color[2] = color.z;
    rec.color[3] = color.w;

    const crd::resources::MeshResource* mesh = group.mesh.get();
    crd::geometry::primitives::AABB3<crd::f32> local;
    local.min = {mesh->bounds_min[0], mesh->bounds_min[1], mesh->bounds_min[2]};
    local.max = {mesh->bounds_max[0], mesh->bounds_max[1], mesh->bounds_max[2]};
    group.world_bounds[slot] = crd::geometry::primitives::transform_aabb(t.world, local);
}

// ⭐⭐ REN-40-B, pass 1: THE STRUCTURE SIGNATURE, IN O(1) PER CHUNK — and the incremental update, fused into
// the same walk so the common frame touches every chunk exactly once.
//
// ⛔ What makes the O(1) signature EXACT rather than merely cheap, case by case:
//   · spawn / despawn / archetype move  → the destination chunk's `version_counter` for MeshRenderer is bumped
//     by the insert (archetype_chunk_storage.cpp: both the in-place upsert and the add-component move do it),
//     and the source chunk's `entity_count` changes under the swap-remove.
//   · a chunk created or freed          → the visited key SET changes, and the fold is ordered.
//   · a MeshRenderer rewritten in place (a new mesh id, a new material) → the same version bump. This is
//     STRICTLY more conservative than the old byte hash: it also fires on a write of an identical value, which
//     can only cost an extra rebuild, never miss one.
//   · spawn AND despawn into one chunk in one frame → the count returns to where it was, which a count-only
//     signature would miss; the renderer version bump catches it, and the first/last entity ids catch a chunk
//     address that was freed and handed back.
// ⛔ The Transform version is deliberately NOT in the signature. It moves whenever anything MOVES, and a moving
// scene must re-extract, not rebuild — folding it in would make every frame structural, which is the 337 ms
// frame this slice exists to delete.
void pass_signature(const crd::scene::ChunkView& view, void* ud)
{
    auto& ctx = *static_cast<ExtractCtx*>(ud);
    const auto* renderers = view.array<const crd::scene::MeshRenderer>(ctx.renderer_id);
    if (renderers == nullptr || view.entity_count == 0U) { return; }

    hash_u64_into(ctx.sig, static_cast<crd::u64>(reinterpret_cast<std::uintptr_t>(view.entities)));
    hash_u64_into(ctx.sig, view.entity_count);
    hash_u64_into(ctx.sig, view.entities[0].raw);
    hash_u64_into(ctx.sig, view.entities[view.entity_count - 1U].raw);
    hash_u64_into(ctx.sig, view.version_of(ctx.renderer_id));
    ++ctx.stats->chunks_visited;
    ctx.stats->signature_bytes += 5U * sizeof(crd::u64);
}

// pass 2a: FULL rebuild — append every instance, group runs per (chunk × group)
void pass_rebuild(const crd::scene::ChunkView& view, void* ud)
{
    auto& ctx = *static_cast<ExtractCtx*>(ud);
    const auto* transforms = view.array<const crd::scene::Transform>(ctx.transform_id);
    const auto* renderers  = view.array<const crd::scene::MeshRenderer>(ctx.renderer_id);
    if (transforms == nullptr || renderers == nullptr || view.entity_count == 0U) { return; }

    // per-chunk: remember each group's size BEFORE this chunk appends (the run start)
    // ⭐ REN-40-C2: OPTIONAL — a chunk whose archetype lacks the component surfaces nullptr, which is the
    // common case and costs one null test per CHUNK rather than anything per entity.
    const auto* overrides = view.array<const crd::scene::MeshLodOverride>(ctx.lod_override_id);

    ctx.run_first.clear();
    for (crd::usize gi = 0; gi < ctx.groups->size(); ++gi)
    {
        ctx.run_first.push_back(static_cast<crd::u32>((*ctx.groups)[gi].instances.size()));
    }

    for (crd::u32 i = 0; i < view.entity_count; ++i)
    {
        const crd::i64 gi64 = group_for_mesh(ctx, renderers[i].mesh);
        if (gi64 < 0) { continue; }
        const auto gi = static_cast<crd::usize>(gi64);
        while (ctx.run_first.size() < ctx.groups->size()) // groups created mid-chunk start their run at 0
        {
            ctx.run_first.push_back(0U);
        }
        MeshGroup& group = (*ctx.groups)[gi];
        const auto slot  = static_cast<crd::u32>(group.instances.size());
        group.instances.push_back(InstanceGpu{});
        group.slot_entity.push_back(view.entities[i]);
        group.world_bounds.push_back({});
        for (crd::u32 w = 0; w < 16U; ++w) { group.prev_world.push_back(0.0F); } // REN-41 velocity: 16 words/slot
        group.lod_override.push_back(0U);
        group.lod_override.push_back(0U);
        group.slot_skeleton.push_back({});
        group.slot_clip.push_back({});
        group.slot_time.push_back(0.0F);
        if (group.material.is_null()) { group.material = renderers[i].material; } // REN-2 Half B: representative material
        write_slot(group, slot, transforms[i], ctx.impl->resolve_color(renderers[i].material),
                   overrides != nullptr ? &overrides[i] : nullptr);
        ++ctx.stats->entities_extracted;
        ctx.impl->entity_slot.insert(view.entities[i],
                                     (static_cast<crd::u64>(gi) << 32U) | static_cast<crd::u64>(slot));
    }

    // ⭐⭐ REN-40-B: record this chunk's runs CONTIGUOUSLY and index them by the chunk key. The runs a chunk
    // owns are appended back to back, so one (first_run, run_count) pair reaches all of them — which is what
    // turns the incremental pass's "is this chunk stale, and where does it live?" from a scan over every run
    // of every group into a single hash probe.
    SceneRenderer::Impl& impl = *ctx.impl;
    const crd::u64 tversion   = view.version_of(ctx.transform_id);
    SceneRenderer::Impl::ChunkEntry entry;
    entry.tversion     = tversion;
    entry.entity_count = view.entity_count;
    entry.first_run    = static_cast<crd::u32>(impl.runs.size());
    for (crd::usize gi = 0; gi < ctx.groups->size(); ++gi)
    {
        const crd::u32 first = ctx.run_first[gi];
        const auto     now   = static_cast<crd::u32>((*ctx.groups)[gi].instances.size());
        if (now > first)
        {
            SceneRenderer::Impl::RunEntry run;
            run.group = static_cast<crd::u32>(gi);
            run.first = first;
            run.count = now - first;
            impl.runs.push_back(run);
        }
    }
    entry.run_count = static_cast<crd::u32>(impl.runs.size()) - entry.first_run;
    impl.chunk_index.insert(view.entities, static_cast<crd::u32>(impl.chunks.size()));
    impl.chunks.push_back(entry);
}

// ⭐⭐ REN-40-B, pass 2b: INCREMENTAL — one hash probe decides, and a clean chunk costs NOTHING.
// ⛔ The whole cost model of the renderer lives in the first six lines: a chunk whose Transform chunk-version
// has not moved returns before it reads a single Transform, so a static 1M-instance frame is O(chunks).
void pass_update(const crd::scene::ChunkView& view, void* ud)
{
    auto& ctx = *static_cast<ExtractCtx*>(ud);
    SceneRenderer::Impl& impl = *ctx.impl;
    const crd::u32* slot_of = impl.chunk_index.find(view.entities);
    if (slot_of == nullptr) { return; } // a chunk the index does not know ⇒ the signature already said rebuild
    SceneRenderer::Impl::ChunkEntry& entry = impl.chunks[*slot_of];

    const crd::u64 tversion = view.version_of(ctx.transform_id);
    if (tversion == entry.tversion) { return; } // ← the fast path: nothing in this chunk moved
    // ⛔ A guard, not an optimisation: if the chunk's population changed, the recorded run spans no longer
    // describe it and writing through them would scribble on another chunk's slots. The signature will have
    // ordered a rebuild for this frame; do nothing here rather than write into a stale mapping.
    if (view.entity_count != entry.entity_count) { return; }

    const auto* transforms = view.array<const crd::scene::Transform>(ctx.transform_id);
    const auto* renderers  = view.array<const crd::scene::MeshRenderer>(ctx.renderer_id);
    if (transforms == nullptr || renderers == nullptr || view.entity_count == 0U) { return; }

    const auto* overrides = view.array<const crd::scene::MeshLodOverride>(ctx.lod_override_id); // REN-40-C2

    entry.tversion = tversion;
    ++ctx.stats->chunks_reextracted;
    ctx.stats->runs_visited += entry.run_count;

    // Seed each group's write cursor from THIS chunk's own run, and mark those runs dirty for the upload.
    // ⛔ `run_first` is indexed by group, so it has to cover every group even though this chunk owns runs in
    // only a few — an entity whose group has no run here would otherwise index out of range.
    ctx.run_first.clear();
    ctx.run_first.resize(ctx.groups->size(), 0U);
    for (crd::u32 r = 0; r < entry.run_count; ++r)
    {
        const crd::u32 ri = entry.first_run + r;
        const SceneRenderer::Impl::RunEntry& run = impl.runs[ri];
        ctx.run_first[run.group] = run.first;
        impl.dirty_runs.push_back(ri);
        ++ctx.stats->dirty_runs;
    }
    for (crd::u32 i = 0; i < view.entity_count; ++i)
    {
        const crd::u32* gi_found = impl.group_of_mesh.find(renderers[i].mesh);
        if (gi_found == nullptr) { continue; }
        const crd::usize gi   = *gi_found;
        const crd::u32   slot = ctx.run_first[gi]++;
        write_slot((*ctx.groups)[gi], slot, transforms[i], impl.resolve_color(renderers[i].material),
                   overrides != nullptr ? &overrides[i] : nullptr);
        ++ctx.stats->entities_extracted;
    }
}

// GEO-8: the animator pass — every sync, over the SKINNED chunks only (Transform+MeshRenderer+SkeletonAnimator):
// mirror the animator state into the groups' slot arrays (palettes re-sample from these each frame)
void pass_animators(const crd::scene::ChunkView& view, void* ud)
{
    auto& ctx = *static_cast<ExtractCtx*>(ud);
    const auto* renderers = view.array<const crd::scene::MeshRenderer>(ctx.renderer_id);
    const auto* animators = view.array<const crd::scene::SkeletonAnimator>(ctx.animator_id);
    if (renderers == nullptr || animators == nullptr) { return; }
    for (crd::u32 i = 0; i < view.entity_count; ++i)
    {
        const crd::u64* packed = ctx.impl->entity_slot.find(view.entities[i]);
        if (packed == nullptr) { continue; }
        const auto gi   = static_cast<crd::u32>(*packed >> 32U);
        const auto slot = static_cast<crd::u32>(*packed & 0xFFFFFFFFU);
        MeshGroup& group = (*ctx.groups)[gi];
        if (slot >= group.slot_skeleton.size()) { continue; }
        group.slot_skeleton[slot] = animators[i].skeleton;
        group.slot_clip[slot]     = animators[i].clip;
        group.slot_time[slot]     = animators[i].time * animators[i].speed;
    }
}

} // namespace

SyncStats SceneRenderer::sync(crd::scene::World& world)
{
    m_impl->world = &world; // REN-36.3-b: the draw-list filter needs the archetype it was extracted from
    SyncStats stats;
    Impl&     impl = *m_impl;
    // ⭐⭐ 38-G1 perf: EVERY per-frame upload from here through render() rides ONE batched transfer submission
    // (see IRasterContext::begin_upload_batch). Before this, each upload paid its own submit + queue idle —
    // measured at 8.3 ms of a 16 ms frame, the single largest cost in the loop. The batch is flushed by the
    // frame graph's execute() (or by ANY synchronous verb via begin_cmd), so ordering is exactly what it was.
    if (impl.raster != nullptr) { impl.raster->begin_upload_batch(); }

    const auto ms_now = [] {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    const double t0 = ms_now();
    ExtractCtx ctx(impl.alloc);
    ctx.world        = &world;
    ctx.impl         = &impl;
    ctx.groups       = &m_groups;
    ctx.transform_id = world.component_id<crd::scene::Transform>();
    ctx.renderer_id  = world.component_id<crd::scene::MeshRenderer>();
    ctx.animator_id  = world.component_id<crd::scene::SkeletonAnimator>();
    ctx.lod_override_id = world.component_id<crd::scene::MeshLodOverride>();
    ctx.stats        = &stats;

    // pass 1: the structure signature
    {
        auto q = world.query<crd::scene::Transform, crd::scene::MeshRenderer>();
        q.for_each_chunk(&pass_signature, &ctx);
    }

    impl.dirty_runs.clear();
    const bool structural = !impl.has_structure || ctx.sig != impl.structure_sig;
    if (structural)
    {
        stats.structural_rebuild = true;
        for (MeshGroup& group : m_groups) // keep buffers/meshes; drop the instance tables
        {
            group.instances.clear();
            group.slot_entity.clear();
            group.world_bounds.clear();
            group.prev_world.clear(); // REN-41 velocity: shadows the instance table
            group.lod_override.clear();
            group.slot_skeleton.clear();
            group.slot_clip.clear();
            group.slot_time.clear();
        }
        impl.entity_slot.clear();
        impl.chunk_index.clear();
        impl.chunks.clear();
        impl.runs.clear();
        auto q = world.query<crd::scene::Transform, crd::scene::MeshRenderer>();
        q.for_each_chunk(&pass_rebuild, &ctx);
        impl.structure_sig = ctx.sig;
        impl.has_structure = true;
    }
    else
    {
        auto q = world.query<crd::scene::Transform, crd::scene::MeshRenderer>();
        q.for_each_chunk(&pass_update, &ctx);
    }

    // GEO-8: mirror the animator state (skinned chunks only — the third required component narrows the walk)
    {
        auto q = world.query<crd::scene::Transform, crd::scene::MeshRenderer, crd::scene::SkeletonAnimator>();
        q.for_each_chunk(&pass_animators, &ctx);
    }

    const double t1 = ms_now();
    stats.extract_ms = t1 - t0;
    // ── GPU: (re)create buffers + upload geometry once + instance payloads by dirty grain ──────────────────────
    // `full_uploaded[gi]` records the groups that re-sent their WHOLE payload this frame, so the dirty-run pass
    // below does not send the same bytes a second time.
    crd::containers::Array<crd::u8> full_uploaded(impl.alloc);
    full_uploaded.resize(m_groups.size(), static_cast<crd::u8>(0));
    for (crd::usize gi = 0; gi < m_groups.size(); ++gi)
    {
        MeshGroup& group = m_groups[gi];
        const auto count = static_cast<crd::u32>(group.instances.size());
        stats.total_instances += count;
        if (count == 0U) { continue; }

        const crd::resources::MeshResource* mesh = group.mesh.get();
        const auto vertex_words = static_cast<crd::u32>(mesh->vertices.size() / 4U);
        const auto vcount       = static_cast<crd::u32>(mesh->vertices.size() / 48U);
        const crd::u32 needed_capacity = count;

        // GEO-8: a skinned group's JOINT COUNT comes from the first slot's skeleton (uniform per mesh in practice)
        if (mesh->has_skin() && group.joint_count == 0U)
        {
            for (crd::usize s = 0; s < group.slot_skeleton.size(); ++s)
            {
                if (group.slot_skeleton[s].is_null()) { continue; }
                auto handle = impl.rm->load_sync<crd::anim::SkeletonResource>(group.slot_skeleton[s]);
                if (handle.state() == crd::resources::LoadState::Ready && handle.get() != nullptr)
                {
                    group.joint_count = handle.get()->joint_count();
                    group.skinned     = true;
                    group.buffer.reset(); // relayout with the skin + palette sections
                    break;
                }
            }
        }

        if (group.buffer == nullptr || group.capacity < needed_capacity)
        {
            // ⭐⭐ REN-40-C2: after the header AND after this buffer's own copy of the DRAW TABLE — see
            // `kGroupSectionsOff`. Every buffer carries the table at the same offset, so one cooked
            // `rebase_table` serves the private and the consolidated layouts alike.
            group.indices_off   = kGroupSectionsOff;
            // ⛔⛔ REN-40-C2: THE INDEX SECTION IS EVERY LEVEL, NOT LEVEL 0. `group.index_count` is what a DRAW
            // consumes (level 0's range); `mesh->indices` holds the whole chain end to end. Sizing the section by
            // the draw count put the VERTEX section on top of levels 1..n — the coarse levels' indices were
            // overwritten by vertex floats, and the only symptom would have been a coarse LOD drawing garbage
            // triangles at a distance, which reads as a decimator bug. Two quantities, deliberately named apart.
            const auto index_words = static_cast<crd::u32>(mesh->indices.size() / 4U)
                                     + (group.has_impostor ? 6U : 0U);
            group.vertices_off  = group.indices_off + index_words;
            group.skin_off      = group.vertices_off + vertex_words;
            const crd::u32 skin_words = group.skinned ? vcount * 6U : 0U;
            group.instances_off = group.skin_off + skin_words;
            group.palette_off   = group.instances_off + needed_capacity * kInstanceWords;
            const crd::u32 palette_words = group.skinned ? needed_capacity * group.joint_count * 16U : 0U;
            group.visible_off   = group.palette_off + palette_words;
            // 38-G1: + one visible list per cascade (see MeshGroup::cascade_visible_count)
            // ⭐⭐ REN-40-A: + the per-instance WORLD AABB section (6 floats each) the GPU cull tests. It lives
            // in the SAME buffer as everything else so a cull dispatch binds ONE resource, and it is uploaded on
            // the same dirty grain as the instances beside it. ⛔ The GPU must cull the BOX, not the transform's
            // translation: a point test disagrees with `aabb_in_frustum` for anything larger than a texel.
            // ⭐⭐ REN-40-C2: one visible list per (VIEW, LOD SLOT) — an instance lands in exactly one, so its
            // draw is a contiguous range and the whole selection stays a single atomic per survivor.
            // ⚠ MEMORY, STATED: this multiplies the visible section by `lod_slots`. At 1M instances and 4 slots
            // that is 80 MB of lists against 20 MB before. The frontier form (a Count -> prefix-sum -> Scatter
            // pass trio, as in PLAYERUNKNOWN's GPU-driven instancing) packs every slot of a view into ONE
            // capacity-sized list and would give it back; it is a NAMED follow-up, not a thing left unsaid.
            group.bounds_off    = group.visible_off + needed_capacity * (1U + kMaxCascades) * impl.lod_slots;
            // ⭐⭐ REN-40-C2: the per-instance LOD OVERRIDE section (2 words each) after the bounds — the cull's
            // second per-instance input, on the same buffer and the same dirty grain.
            group.lod_override_off = group.bounds_off + needed_capacity * 6U;
            group.atlas_off = group.lod_override_off + needed_capacity * 2U;
            // ⭐⭐ REN-41: the atlas is a MIP PYRAMID now — size it from the shared formula, never a bare
            // (grid*tile)², or the buffer is short by the mip tail and the FS reads into the next section.
            const crd::u32 atlas_words = group.has_impostor
                ? crd::lod::impostor_atlas_texels(impl.lod_policy.impostor_grid, impl.lod_policy.impostor_tile)
                : 0U;
            // ⭐⭐ REN-40-F: GPU skinning sections — skeleton data, pre-baked clip, and per-instance anim state.
            // All three exist only for skinned groups; the kernel reads skeleton + clip (uploaded once) and
            // anim_state (per frame), and writes the palette section in place.
            group.skel_off       = group.atlas_off + atlas_words;
            const crd::u32 skel_words = group.skinned ? group.joint_count * 27U : 0U;
            group.clip_off       = group.skel_off + skel_words;
            // clip_words computed later during skeleton upload — sized from the actual pre-baked data.
            // For the buffer allocation, reserve a generous default (1 clip × 64 frames × jc × 10).
            const crd::u32 clip_reserve = group.skinned ? (4U + 64U * group.joint_count * 10U) : 0U;
            group.anim_state_off = group.clip_off + clip_reserve;
            const crd::u32 anim_state_words = group.skinned ? needed_capacity * 2U : 0U;
            // ⭐⭐ REN-41 (velocity): the previous-frame sections, after the skinning ones. prev_world is per-instance
            // for EVERY group (a static instance shadows itself → the prepass reads camera-only velocity); prev_palette
            // exists only for skinned groups, where the velocity prepass deforms each vertex by LAST frame's pose so
            // limb motion reprojects exactly. Both ride the instance dirty grain (prev_world) / the skin copy pass.
            group.prev_world_off = group.anim_state_off + anim_state_words;
            const crd::u32 prev_world_words   = needed_capacity * 16U;
            group.prev_palette_off            = group.prev_world_off + prev_world_words;
            const crd::u32 prev_palette_words = group.skinned ? needed_capacity * group.joint_count * 16U : 0U;
            const crd::u32 total_words = group.prev_palette_off + prev_palette_words;
            group.buffer             = impl.raster->create_storage_buffer(total_words * 4U);
            // ⭐⭐ REN-40-A/C2: and its indirect commands — (1 + cascades) x slots at the backend's stride.
                group.cull_args = impl.raster->create_storage_buffer(
                (kCullArgsHeaderWords * 4U)
                + ((1U + kMaxCascades) * impl.lod_slots * impl.raster->indirect_command_stride()));
            group.cull_base_uploaded = 0xFFFFFFFFU; // force the params write on the next frame
            group.capacity           = needed_capacity;
            group.geometry_uploaded  = false;
            group.skel_uploaded     = false;
            group.baked_clip_off.clear();
        }
        if (group.buffer == nullptr) { continue; }

        if (!group.geometry_uploaded || structural)
        {
            if (!group.geometry_uploaded)
            {
                (void)impl.raster->upload_storage(*group.buffer, group.indices_off * 4U, mesh->indices.data(),
                                                  static_cast<crd::u32>(mesh->indices.size()));
                if (group.has_impostor)
                {
                    const crd::u32 identity_ib[6] = {0U, 1U, 2U, 3U, 4U, 5U};
                    const crd::u32 ib_off = group.indices_off + static_cast<crd::u32>(mesh->indices.size() / 4U);
                    (void)impl.raster->upload_storage(*group.buffer, ib_off * 4U, identity_ib, sizeof(identity_ib));
                    crd::lod::ImpostorAtlas atlas(impl.alloc);
                    (void)crd::lod::bake_impostor_atlas(*mesh, impl.lod_policy.impostor_grid,
                                                        impl.lod_policy.impostor_tile, atlas, impl.alloc);
                    if (!atlas.pixels.empty())
                    {
                        const crd::u32 texel_words = static_cast<crd::u32>(atlas.pixels.size() / 4U);
                        (void)impl.raster->upload_storage(*group.buffer, group.atlas_off * 4U,
                                                          atlas.pixels.data(),
                                                          static_cast<crd::u32>(atlas.pixels.size()));
                        (void)texel_words;
                    }
                }
                (void)impl.raster->upload_storage(*group.buffer, group.vertices_off * 4U, mesh->vertices.data(),
                                                  static_cast<crd::u32>(mesh->vertices.size()));
                if (group.skinned) // the packed skin stream: 2 words of u16-pair joints + 4 weight words
                {
                    crd::containers::Array<crd::u32> packed(impl.alloc);
                    packed.resize(static_cast<crd::usize>(vcount) * 6U);
                    for (crd::u32 v = 0; v < vcount; ++v)
                    {
                        const crd::u16* j = mesh->skin_joints.data() + static_cast<crd::usize>(v) * 4U;
                        const crd::f32* w = mesh->skin_weights.data() + static_cast<crd::usize>(v) * 4U;
                        packed[static_cast<crd::usize>(v) * 6U + 0U] =
                            static_cast<crd::u32>(j[0]) | (static_cast<crd::u32>(j[1]) << 16U);
                        packed[static_cast<crd::usize>(v) * 6U + 1U] =
                            static_cast<crd::u32>(j[2]) | (static_cast<crd::u32>(j[3]) << 16U);
                        std::memcpy(packed.data() + static_cast<crd::usize>(v) * 6U + 2U, w, 16U);
                    }
                    (void)impl.raster->upload_storage(*group.buffer, group.skin_off * 4U, packed.data(),
                                                      static_cast<crd::u32>(packed.size() * 4U));
                }
                group.geometry_uploaded = true;
            }
            // a fresh buffer / a rebuilt table needs the FULL instance payload regardless of per-run dirt
            const crd::u32 bytes = count * static_cast<crd::u32>(sizeof(InstanceGpu));
            (void)impl.raster->upload_storage(*group.buffer, group.instances_off * 4U, group.instances.data(), bytes);
            stats.uploaded_bytes += bytes;
            // ⭐⭐ REN-40-A: the world AABBs ride the SAME grain as the instances they describe — a bounds
            // section that could go stale against its transforms would cull against last frame's positions.
            upload_bounds_range(impl, group, 0U, count, stats);
            // REN-41 velocity: the previous-world section rides the same grain — a stale prev_world would reproject
            // last frame's motion vectors against this frame's geometry.
            if (group.prev_world_off != 0U && static_cast<crd::usize>(count) * 16U <= group.prev_world.size())
            {
                (void)impl.raster->upload_storage(*group.buffer, group.prev_world_off * 4U,
                                                  group.prev_world.data(), count * 16U * 4U);
            }
            full_uploaded[gi] = 1U;
            continue;
        }
    }

    // ── ⭐⭐ REN-40-B: the incremental upload walks the DIRTY LIST. ─────────────────────────────────────────
    // This was `for every group: for every run: if (run.dirty)`, i.e. a full scan of every chunk-run in the
    // scene to find the handful that moved. `pass_update` now names them as it marks them, so the walk is
    // proportional to what CHANGED rather than to how much exists.
    // ⛔ A group that did a FULL payload upload above must not also upload its runs: the bytes are already
    // there, and re-sending them is exactly the per-call upload cost the batch contract exists to avoid.
    for (const crd::u32 ri : impl.dirty_runs)
    {
        const SceneRenderer::Impl::RunEntry& run = impl.runs[ri];
        if (run.group >= m_groups.size() || full_uploaded[run.group] != 0U) { continue; }
        MeshGroup& group = m_groups[run.group];
        if (group.buffer == nullptr || run.first + run.count > group.instances.size()) { continue; }
        const crd::u32 bytes = run.count * static_cast<crd::u32>(sizeof(InstanceGpu));
        (void)impl.raster->upload_storage(*group.buffer,
                                          (group.instances_off + run.first * kInstanceWords) * 4U,
                                          group.instances.data() + run.first, bytes);
        stats.uploaded_bytes += bytes;
        upload_bounds_range(impl, group, run.first, run.count, stats); // REN-40-A, same grain
        // REN-41 velocity: the moved instances' previous transforms, on the same dirty run
        if (group.prev_world_off != 0U
            && static_cast<crd::usize>(run.first + run.count) * 16U <= group.prev_world.size())
        {
            (void)impl.raster->upload_storage(*group.buffer, (group.prev_world_off + run.first * 16U) * 4U,
                                              group.prev_world.data() + static_cast<crd::usize>(run.first) * 16U,
                                              run.count * 16U * 4U);
        }
    }

    const double t2 = ms_now();
    stats.upload_ms = t2 - t1;
    // ── GEO-8 / REN-40-F: the skinned palettes.
    // GPU path: upload skeleton + pre-baked clips once, per-frame anim state only (2 words/instance).
    // CPU path: the original sample→FK→IBM→palette pipeline, retained for A/B comparison.
    if (impl.gpu_skinning_on)
    {
        for (MeshGroup& group : m_groups)
        {
            if (!group.skinned || group.buffer == nullptr || group.instances.size() == 0U) { continue; }
            const crd::u32 jc    = group.joint_count;
            const auto     count = static_cast<crd::u32>(group.instances.size());

            const crd::anim::SkeletonResource* skel = nullptr;
            for (crd::u32 slot = 0; slot < count && skel == nullptr; ++slot)
            {
                if (group.slot_skeleton[slot].is_null()) { continue; }
                auto* cached = impl.skeleton_cache.find(group.slot_skeleton[slot]);
                if (cached == nullptr)
                {
                    impl.skeleton_cache.insert(
                        group.slot_skeleton[slot],
                        impl.rm->load_sync<crd::anim::SkeletonResource>(group.slot_skeleton[slot]));
                    cached = impl.skeleton_cache.find(group.slot_skeleton[slot]);
                }
                if (cached != nullptr && cached->get() != nullptr && cached->get()->joint_count() == jc)
                {
                    skel = cached->get();
                }
            }
            if (skel == nullptr) { continue; }

            // ── ONE-TIME: skeleton data + pre-baked clips ──
            if (!group.skel_uploaded)
            {
                // Parents (i32 → u32 bitcast) + inverse_binds + rest_pose = jc × 27 words
                crd::containers::Array<crd::u32> skel_buf(impl.alloc);
                skel_buf.resize(static_cast<crd::usize>(jc) * 27U);
                for (crd::u32 j = 0; j < jc; ++j)
                {
                    crd::u32 pu = 0;
                    std::memcpy(&pu, &skel->parents[j], 4U);
                    skel_buf[j] = pu;
                }
                std::memcpy(skel_buf.data() + jc, skel->inverse_binds.data(),
                            static_cast<crd::usize>(jc) * 16U * 4U);
                std::memcpy(skel_buf.data() + jc + static_cast<crd::usize>(jc) * 16U,
                            skel->rest.data(), static_cast<crd::usize>(jc) * 10U * 4U);
                (void)impl.raster->upload_storage(*group.buffer, group.skel_off * 4U,
                                                  skel_buf.data(),
                                                  static_cast<crd::u32>(skel_buf.size() * 4U));

                // Pre-bake clips at 30 fps: rest-pose clip first (offset 0), then each unique clip
                crd::containers::Array<crd::u32> clip_buf(impl.alloc);
                group.baked_clip_off.clear();
                {
                    // Rest-pose clip: header(4) + 1 frame × jc × 10 words
                    crd::u32 jc_u = jc;
                    crd::u32 fc_u = 1U;
                    clip_buf.push_back(jc_u);
                    clip_buf.push_back(fc_u);
                    crd::f32 zero_f = 0.0F;
                    crd::u32 zero_u = 0;
                    std::memcpy(&zero_u, &zero_f, 4U);
                    clip_buf.push_back(zero_u); // duration
                    clip_buf.push_back(zero_u); // frame_rate
                    for (crd::u32 j = 0; j < jc; ++j)
                    {
                        const crd::f32* r = skel->rest.data() + static_cast<crd::usize>(j) * crd::anim::kRestFloats;
                        for (crd::u32 c = 0; c < 10U; ++c)
                        {
                            crd::u32 bits = 0;
                            std::memcpy(&bits, &r[c], 4U);
                            clip_buf.push_back(bits);
                        }
                    }
                }

                impl.pose_scratch.resize(jc);
                for (crd::u32 slot = 0; slot < count; ++slot)
                {
                    if (group.slot_clip[slot].is_null()) { continue; }
                    if (group.baked_clip_off.find(group.slot_clip[slot]) != nullptr) { continue; }

                    const crd::anim::AnimClipResource* clip = nullptr;
                    auto* ccached = impl.clip_cache.find(group.slot_clip[slot]);
                    if (ccached == nullptr)
                    {
                        impl.clip_cache.insert(group.slot_clip[slot],
                                               impl.rm->load_sync<crd::anim::AnimClipResource>(group.slot_clip[slot]));
                        ccached = impl.clip_cache.find(group.slot_clip[slot]);
                    }
                    if (ccached != nullptr) { clip = ccached->get(); }
                    if (clip == nullptr || clip->duration <= 0.0F) { continue; }

                    const crd::f32 bake_fps = 30.0F;
                    const crd::u32 frame_count = static_cast<crd::u32>(crd::math::ceil(clip->duration * bake_fps)) + 1U;
                    group.baked_clip_off.insert(group.slot_clip[slot],
                                               static_cast<crd::u32>(clip_buf.size()));

                    clip_buf.push_back(jc);
                    clip_buf.push_back(frame_count);
                    crd::u32 dur_u = 0;
                    crd::u32 fps_u = 0;
                    std::memcpy(&dur_u, &clip->duration, 4U);
                    const crd::f32 frame_rate = static_cast<crd::f32>(frame_count - 1U) / clip->duration;
                    std::memcpy(&fps_u, &frame_rate, 4U);
                    clip_buf.push_back(dur_u);
                    clip_buf.push_back(fps_u);

                    for (crd::u32 f = 0; f < frame_count; ++f)
                    {
                        const crd::f32 t = (frame_count > 1U)
                            ? static_cast<crd::f32>(f) / frame_rate : 0.0F;
                        crd::anim::sample_clip(*clip, *skel, t,
                                               {impl.pose_scratch.data(), impl.pose_scratch.size()});
                        for (crd::u32 j = 0; j < jc; ++j)
                        {
                            const auto& p = impl.pose_scratch[j];
                            crd::f32 trs[10] = {p.translation.x, p.translation.y, p.translation.z,
                                                p.rotation.x, p.rotation.y, p.rotation.z, p.rotation.w,
                                                p.scale.x, p.scale.y, p.scale.z};
                            for (crd::u32 c = 0; c < 10U; ++c)
                            {
                                crd::u32 bits = 0;
                                std::memcpy(&bits, &trs[c], 4U);
                                clip_buf.push_back(bits);
                            }
                        }
                    }
                }
                (void)impl.raster->upload_storage(*group.buffer, group.clip_off * 4U,
                                                  clip_buf.data(),
                                                  static_cast<crd::u32>(clip_buf.size() * 4U));
                group.skel_uploaded = true;
            }

            // ── PER-FRAME: upload anim_state (2 words per instance) ──
            impl.anim_state_staging.resize(static_cast<crd::usize>(count) * 2U);
            for (crd::u32 slot = 0; slot < count; ++slot)
            {
                crd::u32 clip_local = 0U; // rest-pose clip at word 0
                if (!group.slot_clip[slot].is_null())
                {
                    const crd::u32* off = group.baked_clip_off.find(group.slot_clip[slot]);
                    if (off != nullptr) { clip_local = *off; }
                }
                impl.anim_state_staging[static_cast<crd::usize>(slot) * 2U] = clip_local;
                crd::u32 t_bits = 0;
                std::memcpy(&t_bits, &group.slot_time[slot], 4U);
                impl.anim_state_staging[static_cast<crd::usize>(slot) * 2U + 1U] = t_bits;
            }
            (void)impl.raster->upload_storage(*group.buffer, group.anim_state_off * 4U,
                                              impl.anim_state_staging.data(),
                                              static_cast<crd::u32>(impl.anim_state_staging.size() * 4U));
        }
    }
    else
    {
        // ── GEO-8 CPU PATH: sample every skinned instance's clip, upload the palette section ──
        for (MeshGroup& group : m_groups)
        {
            if (!group.skinned || group.buffer == nullptr || group.instances.size() == 0U) { continue; }
            const crd::u32 jc    = group.joint_count;
            const auto     count = static_cast<crd::u32>(group.instances.size());
            impl.palette_staging.resize(static_cast<crd::usize>(count) * jc * 16U);
            impl.pose_scratch.resize(jc);
            impl.world_scratch.resize(jc);
            impl.palette_scratch.resize(jc);

            for (crd::u32 slot = 0; slot < count; ++slot)
            {
                crd::f32* dst = impl.palette_staging.data() + static_cast<crd::usize>(slot) * jc * 16U;
                const crd::anim::SkeletonResource* skel = nullptr;
                if (!group.slot_skeleton[slot].is_null())
                {
                    auto* cached = impl.skeleton_cache.find(group.slot_skeleton[slot]);
                    if (cached == nullptr)
                    {
                        impl.skeleton_cache.insert(
                            group.slot_skeleton[slot],
                            impl.rm->load_sync<crd::anim::SkeletonResource>(group.slot_skeleton[slot]));
                        cached = impl.skeleton_cache.find(group.slot_skeleton[slot]);
                    }
                    if (cached != nullptr && cached->get() != nullptr && cached->get()->joint_count() == jc)
                    {
                        skel = cached->get();
                    }
                }
                if (skel == nullptr)
                {
                    for (crd::u32 j = 0; j < jc; ++j)
                    {
                        for (crd::u32 c = 0; c < 16U; ++c)
                        {
                            dst[j * 16U + c] = (c % 5U) == 0U ? 1.0F : 0.0F;
                        }
                    }
                    continue;
                }

                const crd::anim::AnimClipResource* clip = nullptr;
                if (!group.slot_clip[slot].is_null())
                {
                    auto* ccached = impl.clip_cache.find(group.slot_clip[slot]);
                    if (ccached == nullptr)
                    {
                        impl.clip_cache.insert(group.slot_clip[slot],
                                               impl.rm->load_sync<crd::anim::AnimClipResource>(group.slot_clip[slot]));
                        ccached = impl.clip_cache.find(group.slot_clip[slot]);
                    }
                    if (ccached != nullptr) { clip = ccached->get(); }
                }

                if (clip != nullptr && clip->duration > 0.0F)
                {
                    const crd::f32 t = group.slot_time[slot] - clip->duration
                                           * static_cast<crd::f32>(static_cast<crd::i64>(group.slot_time[slot]
                                                                                         / clip->duration));
                    crd::anim::sample_clip(*clip, *skel, t, {impl.pose_scratch.data(), impl.pose_scratch.size()});
                }
                else
                {
                    for (crd::u32 j = 0; j < jc; ++j)
                    {
                        const crd::f32* r = skel->rest.data() + static_cast<crd::usize>(j) * crd::anim::kRestFloats;
                        impl.pose_scratch[j].translation = {r[0], r[1], r[2]};
                        impl.pose_scratch[j].rotation    = {r[3], r[4], r[5], r[6]};
                        impl.pose_scratch[j].scale       = {r[7], r[8], r[9]};
                    }
                }
                crd::anim::compute_pose_matrices(*skel, {impl.pose_scratch.data(), impl.pose_scratch.size()},
                                                 {impl.world_scratch.data(), impl.world_scratch.size()});
                crd::anim::compute_skin_palette(*skel, {impl.world_scratch.data(), impl.world_scratch.size()},
                                                {impl.palette_scratch.data(), impl.palette_scratch.size()});
                std::memcpy(dst, impl.palette_scratch.data(), static_cast<crd::usize>(jc) * 64U);
            }
            // ⭐⭐ REN-41 (velocity, skinned): upload LAST frame's palette to prev_palette_off, THEN remember this
            // frame's — the CPU-skin analog of the GPU palette_snapshot pass. First frame (or an instance-count
            // change) seeds prev = cur so a spawn yields zero velocity instead of a spike from an all-zero pose.
            if (group.prev_palette_off != 0U)
            {
                if (group.prev_palette.size() != impl.palette_staging.size())
                {
                    group.prev_palette.resize(impl.palette_staging.size());
                    std::memcpy(group.prev_palette.data(), impl.palette_staging.data(),
                                impl.palette_staging.size() * 4U);
                }
                (void)impl.raster->upload_storage(*group.buffer, group.prev_palette_off * 4U,
                                                  group.prev_palette.data(),
                                                  static_cast<crd::u32>(group.prev_palette.size() * 4U));
                std::memcpy(group.prev_palette.data(), impl.palette_staging.data(),
                            impl.palette_staging.size() * 4U);
            }
            (void)impl.raster->upload_storage(*group.buffer, group.palette_off * 4U, impl.palette_staging.data(),
                                              static_cast<crd::u32>(impl.palette_staging.size() * 4U));
        }
    }

    stats.palette_ms = ms_now() - t2;
    stats.groups     = static_cast<crd::u32>(m_groups.size());
    return stats;
}

// REN-1: one culled group's draw (the frame graph records the whole list in ONE submission).
namespace
{
// (SceneDraw is defined above `SceneRenderer::Impl` — the contribution arena holds the per-viewport list.)
// REN-2 Half B: record ONE scene group — the first clears colour+depth, later groups LOAD; a group whose material
// carries a base-color map draws through draw_storage_textured_depth (samples albedo), else the flat draw.
void record_one_group(crd::gpu::IRasterContext& r, crd::gpu::IRasterTarget& t, const SceneDraw& d,
                      crd::gpu::ClearColor clear, bool first, crd::gpu::ITexture* shadow_atlas)
{
    const auto cmp = crd::gpu::DepthCompare::GreaterEqual;
    // REN-3.2-b: with an atlas bound the group draws SHADOWED (cascade select + PCF). The shadowed and textured
    // paths are mutually exclusive today because both want the same descriptor slot 1 - combining them needs the
    // bindless material work in REN-3.3, so shadows take precedence and the albedo map rides that slice.
    if (shadow_atlas != nullptr)
    {
        if (first)
        {
            r.draw_storage_shadowed_depth(t, *d.program, clear, 0.0F, cmp, *d.buffer, *shadow_atlas, d.vertex_count);
        }
        else
        {
            r.draw_storage_shadowed_depth_load(t, *d.program, cmp, *d.buffer, *shadow_atlas, d.vertex_count);
        }
    }
    else if (d.base_color != nullptr)
    {
        if (first) { r.draw_storage_textured_depth(t, *d.program, clear, 0.0F, cmp, *d.buffer, *d.base_color, d.vertex_count); }
        else { r.draw_storage_textured_depth_load(t, *d.program, cmp, *d.buffer, *d.base_color, d.vertex_count); }
    }
    else if (first) { r.draw_storage_depth(t, *d.program, clear, 0.0F, cmp, *d.buffer, d.vertex_count); }
    else { r.draw_storage_depth_load(t, *d.program, cmp, *d.buffer, d.vertex_count); }
}
// ⭐ REN-37.10: `record_scene_groups` and the CASCADE pass recorder used to live here. Both are GONE — the
// executor's `record_pass` drives every geometry and depth-only pass from the AUTHORED graph now, including the
// first-clears-rest-loads rule and the per-slice cascade target. `record_one_group` above survives only for the
// synchronous fallback a backend without a frame graph takes.

} // namespace

// ── REN-37.10: THE HOST. What an authored graph CANNOT know, and only the renderer can answer. ──────────────
// The split is the whole point of `IFrameGraphHost`: the ASSET declares topology (which passes, what they read
// and write, which draw list, which technique); the HOST resolves the names to live objects (this target, these
// mesh groups, this many cascades, these programs). Neither knows the other's internals, which is why the same
// asset runs in a test, a game and an editor viewport.
class SceneHost final : public crd::framecook::IFrameGraphHost
{
public:
    SceneHost(SceneRenderer::Impl& impl, crd::gpu::IRasterTarget& target, crd::gpu::ClearColor clear,
              const crd::containers::Array<SceneDraw>& draws, crd::gpu::FgImage out)
        : m_impl(impl), m_target(target), m_clear(clear), m_draws(draws), m_out(out)
    {
        (void)m_clear;
        (void)m_out;
    }

    [[nodiscard]] crd::gpu::IRasterTarget* output() override { return &m_target; }

    // ⭐⭐ REN-38-F6: the ADVANCED-FAMILY program ids, resolved to programs cooked from the authored
    // declarations. An id the host does not know stays null, which the executor reports BY NAME.
    [[nodiscard]] crd::gpu::IRasterProgram* program(crd::containers::StringView id) override
    {
        if (str_is(id, "crd://scene/tess")) { return m_impl.ensure_tess_program(); }
        if (str_is(id, "crd://scene/mesh")) { return m_impl.ensure_mesh_program(); }
        if (str_is(id, "crd://scene/visbuffer")) { return m_impl.ensure_visbuffer_program(); }
        if (str_is(id, "crd://scene/impostor")) { return m_impl.ensure_impostor_program(); }
        if (str_is(id, "crd://scene/hzb_build")) { return m_impl.ensure_hzb_program(); }
        if (str_is(id, "crd://scene/taa_resolve")) { return m_impl.ensure_taa_program(); } // REN-41 TAA
        if (str_is(id, "crd://scene/velocity_debug")) { return m_impl.ensure_velocity_debug_program(); } // REN-41
        // ⭐⭐ 38-G1b: the authored POST programs — the technique library's first device-reachable family
        if (str_is(id, "crd://post/tonemap_agx") || str_is(id, "crd://post/srgb_only"))
        {
            return m_impl.ensure_post_program(id);
        }
        // ⭐⭐ REN-40-D: the moment-atlas family. The BASE program is instance 0; the for_each expansion asks
        // `instance_program` for each cascade's own (the layer is baked per instance).
        if (str_is(id, "crd://shadow/moment_convert")) { return m_impl.ensure_moment_program(0U, 0U); }
        if (str_is(id, "crd://shadow/moment_blur_x")) { return m_impl.ensure_moment_program(1U, 0U); }
        if (str_is(id, "crd://shadow/moment_blur_y")) { return m_impl.ensure_moment_program(2U, 0U); }
        return nullptr;
    }

    // The single-stage kernels: GPU culling and the four ray-tracing stages.
    [[nodiscard]] crd::gpu::IGpuProgram* kernel(crd::containers::StringView id) override
    {
        if (str_is(id, "crd://scene/cull")) { return m_impl.ensure_cull_kernel(); }
        if (str_is(id, "crd://scene/cull_mark")) { return m_impl.ensure_cull_mark_kernel(); }
        // ⭐⭐ REN-40-A: the GPU-driven pair the `scene_gpu_cull` frame graph names
        // ⛔ ONE authored asset, FIVE named views — the suffix IS the view index, so the frame asset declares
        // `crd://scene/cull_view0` .. `cull_view4` and the renderer cooks the matching variant. Explicit beats a
        // new `for_each` expansion here: five passes that a reader can count are worth more than machinery.
        if (str_is(id, "crd://scene/cull_view0")) { return m_impl.ensure_cull_view_kernel(0U); }
        if (str_is(id, "crd://scene/cull_view1")) { return m_impl.ensure_cull_view_kernel(1U); }
        if (str_is(id, "crd://scene/cull_view2")) { return m_impl.ensure_cull_view_kernel(2U); }
        if (str_is(id, "crd://scene/cull_view3")) { return m_impl.ensure_cull_view_kernel(3U); }
        if (str_is(id, "crd://scene/cull_view4")) { return m_impl.ensure_cull_view_kernel(4U); }
        if (str_is(id, "crd://scene/cull_reset")) { return m_impl.ensure_cull_reset_kernel(); }
        if (str_is(id, "crd://scene/occlusion_cull")) { return m_impl.ensure_occlusion_cull_kernel(); }
        if (str_is(id, "crd://scene/gpu_skin")) { return m_impl.ensure_skin_compute_kernel(); }
        if (str_is(id, "crd://scene/palette_snapshot")) { return m_impl.ensure_palette_snapshot_kernel(); }
        if (str_is(id, "crd://scene/rt/raygen")) { return m_impl.ensure_rt_kernel(0U); }
        if (str_is(id, "crd://scene/rt/miss")) { return m_impl.ensure_rt_kernel(1U); }
        if (str_is(id, "crd://scene/rt/chit")) { return m_impl.ensure_rt_kernel(2U); }
        if (str_is(id, "crd://scene/rt/anyhit")) { return m_impl.ensure_rt_kernel(3U); }
        return nullptr;
    }

    // B4: the graph NAMES an acceleration structure; the renderer resolves it to the one the host installed
    // via `set_scene_accel` — the asset format stays free of engine types.
    [[nodiscard]] crd::gpu::IAccelerationStructure* acceleration_structure(crd::containers::StringView) override
    {
        return m_impl.scene_accel;
    }

    // The external buffers the compute/RT scene graphs declare: the culled group's instance buffer, and the
    // renderer-owned result buffers the gates read back.
    [[nodiscard]] crd::gpu::IStorageBuffer* storage_buffer(crd::containers::StringView name) override
    {
        if (str_is(name, "instances"))
        {
            return m_draws.size() > 0U ? m_draws[0].buffer : nullptr;
        }
        // ⭐⭐ REN-40-A: the GPU-driven commands. ⛔ A compute pass walking a DRAW LIST binds each item's OWN
        // args (DrawItem::args) — this resolver only answers the graph's declared-resource question so the pass
        // can express the read/write EDGE that orders reset before cull and cull before the draws. Returning the
        // first group's buffer is enough for that: the edge is per-RESOURCE-NAME, the binding is per-item.
        if (str_is(name, "cull_args"))
        {
            return m_draws.size() > 0U ? m_draws[0].cull_args : nullptr;
        }
        if (str_is(name, "cull_flags")) { return m_impl.ensure_scratch(m_impl.buf_cull_flags); }
        if (str_is(name, "cull_marks")) { return m_impl.ensure_scratch(m_impl.buf_cull_marks); }
        if (str_is(name, "hits")) { return m_impl.ensure_scratch(m_impl.buf_hits); }
        if (str_is(name, "taa_constants")) { return m_impl.ensure_taa_constants(); } // REN-41 TAA
        return nullptr;
    }

    [[nodiscard]] bool draw_list(crd::containers::StringView name, crd::framecook::DrawListBinding& out) override
    {
        // REN-38-F6: the visibility-buffer list is ONE fullscreen expansion (6 procedural vertices) drawn with
        // the visbuffer program — `draw_visbuffer` binds no storage, so the scene pull items cannot serve here.
        if (str_is(name, "visbuffer_geometry"))
        {
            crd::gpu::IRasterProgram* vp = m_impl.ensure_visbuffer_program();
            if (vp == nullptr) { return false; }
            out.items[0] = crd::framecook::DrawItem{nullptr, vp, 6U, nullptr};
            out.resolved = 1U;
            return true;
        }
        if (str_is(name, "impostor_geometry")) { return fill_impostor(out); }
        return fill(out, nullptr);
    }

    // ⭐ REN-36.3-b: the ECS QUERY, actually evaluated. Before this the asset's `all`/`any`/`none` filters were
    // parsed, validated, cooked and round-tripped — then DROPPED, because the host only ever got a NAME and
    // invented its own list. A declared-but-ignored filter is worse than an unsupported one: it reads as working.
    [[nodiscard]] bool draw_list_query(const crd::framecook::FrameDrawListDesc& q,
                                       crd::framecook::DrawListBinding&         out) override
    {
        // REN-38-F6: the visibility-buffer list resolves through the QUERY path too (the executor prefers it
        // whenever the desc exists) — missing this arm handed the visbuffer pass the scene PULL items, whose
        // programs bind storage `draw_visbuffer` never provides.
        if (str_is(crd::containers::StringView(q.name.c_str(), q.name.size()), "visbuffer_geometry"))
        {
            crd::gpu::IRasterProgram* vp = m_impl.ensure_visbuffer_program();
            if (vp == nullptr) { return false; }
            out.items[0] = crd::framecook::DrawItem{nullptr, vp, 6U, nullptr};
            out.resolved = 1U;
            return true;
        }
        if (str_is(crd::containers::StringView(q.name.c_str(), q.name.size()), "impostor_geometry"))
        {
            return fill_impostor(out);
        }
        return fill(out, &q);
    }

    // ⭐⭐ 38-G1 perf: the EXPANSION-INDEX face. A CSM cascade pass asks with its cascade number, and the
    // answer carries that cascade's OWN vertex count — the count that makes the draw read the per-cascade
    // visible list the renderer just uploaded. Everything else falls through to the shared answer.
    [[nodiscard]] bool draw_list_query(const crd::framecook::FrameDrawListDesc& q,
                                       crd::framecook::DrawListBinding& out, crd::u32 instance) override
    {
        if (!draw_list_query(q, out)) { return false; }
        // 0xFFFFFFFF = NOT an expanded pass (the forward draw) — its counts must stay the camera's.
        if (!m_impl.shadows_active() || instance >= kMaxCascades) { return true; }
        // the draw list is index-parallel with the culled groups (the same construction `groups_view` uses)
        crd::u32 gi = 0U;
        for (crd::u32 i = 0; i < out.resolved; ++i)
        {
            const MeshGroup* g = m_impl.group_of_draw(gi);
            if (g == nullptr) { break; }
            out.items[i].vertex_count = g->cascade_visible_count[instance] * g->index_count;
            // REN-39-C1: an indexed item's per-cascade count rides `instance_count` (the vertex_count above is
            // the pull twin's spelling of the SAME number — both stay correct, whichever mode the frame runs)
            if (out.items[i].index_count > 0U)
            {
                out.items[i].instance_count = g->cascade_visible_count[instance];
            }
            // ⭐⭐ REN-40-A: cascade c reads VIEW c+1's command (view 0 is the camera), so the item's args offset
            // moves along by one command stride per cascade. ⛔ The stride is the BACKEND'S — 20 B on Vulkan,
            // 24 B on D3D12 — so it is asked for, never assumed.
            if (out.items[i].args != nullptr && m_impl.raster != nullptr)
            {
                // ⭐⭐ REN-40-C2: view (1 + instance) owns `slots` consecutive commands; this item draws its own
                // LOD slot inside them.
                const crd::u32 stride = m_impl.raster->indirect_command_stride();
                const crd::u32 cmd    = ((1U + instance) * m_impl.lod_slots) + m_item_slot[i];
                out.items[i].args_offset = (kCullArgsHeaderWords * 4U) + cmd * stride;
            }
            ++gi;
        }
        return true;
    }

    // Shadows on/off is a DECLARED CAPABILITY TIER, not an `if` in this renderer: when it is off the executor
    // steps down to the `fallback` graph, which has no atlas and no cascade passes at all.
    [[nodiscard]] bool capability(crd::containers::StringView name) override
    {
        return str_is(name, "shadows") ? m_impl.shadows_active() : false;
    }

    // ⭐⭐ REN-39 (the gizmo fix): the overlay is WOVEN into the recording by the recorder — after the last
    // geometry pass, onto the SCENE image (live depth, pre-tonemap) — instead of appended after the whole
    // graph, where a post chain left it depth-testing a never-written buffer and escaping the tonemap. The
    // declared image is stashed so the app's callback can resolve it via `overlay_target(ctx)` — drawing a
    // captured raw target renders an image the graph never barriered.
    [[nodiscard]] bool overlay_pass(crd::gpu::FgExecuteFn* fn, void** user, crd::gpu::FgImage target) override
    {
        if (m_impl.overlay_fn == nullptr) { return false; }
        *fn                = m_impl.overlay_fn;
        *user              = m_impl.overlay_user;
        m_impl.overlay_img = target;
        return true;
    }

    // REN-39: the NAME is honored — `fallback` is an asset reference, resolved disk-first like every other
    // default. The parsed `forward_basic` stays as the last-resort floor when the name does not resolve (the
    // pre-existing behavior), so a broken fallback still degrades loudly instead of erroring silently.
    [[nodiscard]] const crd::framecook::FrameGraphDesc* fallback_graph(crd::containers::StringView name) override
    {
        const crd::framecook::FrameGraphDesc* fb = m_impl.resolve_frame_asset(name);
        return fb != nullptr ? fb : &m_impl.fallback;
    }

    // How many cascades THIS FRAME. Scene state, so the host owns it — and 0 is a REPORTED failure upstream, not
    // a silently shadow-less frame.
    [[nodiscard]] crd::u32 for_each_count(crd::framecook::FrameForEach kind, crd::u32) override
    {
        return kind == crd::framecook::FrameForEach::LightCascades ? m_impl.cascades.count : 0U;
    }

    // ⛔ Each cascade has its OWN program: the cascade index is baked as a compile-time constant so the VS reads
    // its own light_vp slice from the shared header. Returning the same program for every instance would render
    // all four slices from cascade 0 — identical slices, which is exactly what the REN-3.2 gate rejects.
    // ⭐⭐ REN-39-C1: under the indexed switch the cascade programs are the INDEXED twins — the items carry
    // index fields exactly when `use_indexed` survived init (all-or-nothing), so program and items always agree.
    // ⛔⛔ REN-40-D: dispatch by PASS NAME now — the moment graph has FOUR for_each passes, and serving every
    // one of them the cascade DEPTH program would raster the whole scene into what should be a fullscreen
    // filter. The name is the pass's identity in the asset, so it is the key here too.
    [[nodiscard]] crd::gpu::IRasterProgram* instance_program(crd::containers::StringView name, crd::u32 index) override
    {
        if (index >= kMaxCascades)
        {
            return nullptr;
        }
        if (str_is(name, "moment_convert")) { return m_impl.ensure_moment_program(0U, index); }
        if (str_is(name, "moment_blur_x")) { return m_impl.ensure_moment_program(1U, index); }
        if (str_is(name, "moment_blur_y")) { return m_impl.ensure_moment_program(2U, index); }
        if (m_impl.use_indexed && m_impl.shadow_prog_idx[index] != nullptr)
        {
            return m_impl.shadow_prog_idx[index].get();
        }
        return m_impl.shadow_prog[index].get();
    }

    // REN-40-E1: a cascade whose light_vp has NOT changed since the previous frame is CACHED — the persistent
    // atlas retains its data and the pass records zero draws (the expansion path skips draw-list resolution).
    // On the FIRST frame, prev_light_vp is all-zeros and cascades.light_vp carries real values, so every
    // cascade renders — no explicit first-frame guard needed.
    // REN-40-E2: far cascades (2, 3) ALTERNATE on a round-robin schedule even when dirty — texel-snapped
    // cascades at that range make one-frame staleness invisible. Near cascades (0, 1) always update. The
    // round-robin is disabled on frame 0 (csm_frame == 1 after the first compute) so every cascade populates
    // the persistent atlas at least once.
    [[nodiscard]] bool for_each_load(crd::framecook::FrameForEach kind, crd::u32 index) const override
    {
        if (kind != crd::framecook::FrameForEach::LightCascades) { return false; }
        if (index >= kMaxCascades) { return false; }
        if (m_impl.prev_light_vp[index] == m_impl.cascades.light_vp[index]) { return true; }
        if (index >= 2U && m_impl.csm_frame > 1U && !m_impl.cascade_scheduled(index)) { return true; }
        return false;
    }

private:
    static bool str_is(crd::containers::StringView a, const char* b)
    {
        crd::usize i = 0;
        while (b[i] != '\0' && i < a.size() && a[i] == b[i]) { ++i; }
        return b[i] == '\0' && i == a.size();
    }

    // ⭐⭐ REN-40-C5: resolve the IMPOSTOR draw list — one item per group that carries an impostor slot. The items
    // read the impostor draw table (kImpostorTableOff), NOT the mesh table, because the impostor pass has its own
    // DrawIndex space. Under the GPU cull the impostor slot's camera command is already filled by the compact kernel.
    bool fill_impostor(crd::framecook::DrawListBinding& out)
    {
        out.resolved = 0U;
        for (crd::usize i = 0; i < m_impl.impostor_draws.size()
                                && out.resolved < crd::framecook::kMaxDrawItems; ++i)
        {
            const SceneDraw& d = m_impl.impostor_draws[i];
            if (d.buffer == nullptr || d.program == nullptr) { continue; }
            crd::framecook::DrawItem it;
            it.storage        = d.buffer;
            it.program        = d.program;
            it.indexed        = true;
            it.index_count    = d.index_count;
            it.instance_count = d.instance_count;
            it.first_index    = d.first_index;
            if (m_impl.gpu_cull_on && d.index_count > 0U && d.cull_args != nullptr && m_impl.raster != nullptr)
            {
                it.args        = d.cull_args;
                it.args_offset = (kCullArgsHeaderWords * 4U) + d.cull_args_offset
                                 + d.lod_slot * m_impl.raster->indirect_command_stride();
                it.dispatch_groups = 0U;
            }
            out.items[out.resolved++] = it;
        }
        return true;
    }

    // Resolve the frame's culled groups into draw items, applying the asset's component filter when there is one.
    bool fill(crd::framecook::DrawListBinding& out, const crd::framecook::FrameDrawListDesc* q)
    {
        out.resolved = 0U;
        for (crd::usize i = 0; i < m_draws.size() && out.resolved < crd::framecook::kMaxDrawItems; ++i)
        {
            const SceneDraw& d = m_draws[i];
            if (d.buffer == nullptr || d.program == nullptr) { continue; }
            if (q != nullptr && !group_matches(*q, i)) { continue; }
            crd::framecook::DrawItem it;
            it.storage      = d.buffer;
            it.program      = d.program;
            // ⭐⭐ REN-40-C2: EVERY scene item records through the MULTI verb, because every scene program now
            // rebases by `table[DrawIndex]` — and only the multi verb pushes the row. A classic verb would
            // leave the push stale and the draw would read another item's base and LOD slot.
            it.indexed      = true;
            it.vertex_count = d.vertex_count;
            it.texture      = d.base_color; // beats the pass's sampled read (REN-37.10)
            it.program_depth = d.program_depth; // the prepass twin — see DrawItem::program_depth
            it.program_velocity = d.program_velocity; // REN-41: the velocity MRT prepass twin
            // REN-39-C1: the indexed-pull fields ride through — index_count > 0 routes the indexed verbs
            it.index_count = d.index_count;
            it.instance_count = d.instance_count;
            it.first_index = d.first_index;
            // ⭐⭐ REN-40-A: under the GPU cull the command lives in DEVICE memory. `args` routes this item to
            // the indirect verb (`instance_count` above is then never read — it is stale by construction), and
            // `dispatch_groups` is what a COMPUTE pass walking this same list uses as its grid.
            // ⛔ Only for INDEXED items: a GPU-written command IS an indexed-indirect command.
            if (m_impl.fill_diag_index_count_0 == 0U) { m_impl.fill_diag_index_count_0 = d.index_count; }
            if (m_impl.gpu_cull_on && d.index_count > 0U && d.cull_args != nullptr)
            {
                ++m_impl.fill_diag_cull_gates;
                it.args            = d.cull_args;
                // ⭐⭐ REN-40-C2: view 0 (the camera) owns commands [0 .. slots), so THIS item's is its slot's.
                it.args_offset     = (kCullArgsHeaderWords * 4U) + d.cull_args_offset
                                     + (m_impl.raster != nullptr
                                            ? d.lod_slot * m_impl.raster->indirect_command_stride()
                                            : 0U);
                // ⛔⛔ REN-40-C2: THE CULL DISPATCHES ONCE PER GROUP, NEVER ONCE PER SLOT. A compute pass walking
                // this list dispatches per ITEM, and the list now carries one item per (group, slot) — so a
                // 4-slot group would run the same cull four times, and each run would append the SAME survivors
                // again. The counts would be 4x and **the picture would look correct**, because drawing one
                // instance four times at the same depth is invisible. It showed up only as GPU time going the
                // wrong way (86.8 -> 92.7 ms at 1M) and as the device/CPU count comparison disagreeing.
                // Slot 0's item carries the dispatch; the rest are draw-only.
                it.dispatch_groups = d.lod_slot == 0U ? d.cull_groups : 0U;
                if (it.dispatch_groups > m_impl.fill_diag_dispatch_max)
                {
                    m_impl.fill_diag_dispatch_max = it.dispatch_groups;
                }
            }
            // ⭐⭐ REN-40-F: when GPU skinning is on, the skin compute pass also walks this list. If the cull
            // path didn't set dispatch_groups (gpu_cull off, or non-indexed items), set it here so the skin
            // kernel still dispatches. Non-skinned groups run the kernel's for loops 0 times (jc == 0).
            if (m_impl.gpu_skinning_on && it.dispatch_groups == 0U && d.cull_groups > 0U && d.lod_slot == 0U)
            {
                it.dispatch_groups = d.cull_groups;
            }
            // ⛔ The item -> slot map, kept EXACTLY rather than re-derived: the expansion face below moves each
            // item's args offset to its cascade's command, and a FILTERED draw list makes any counting-based
            // reconstruction wrong the moment one group is skipped.
            m_item_slot[out.resolved] = d.lod_slot;
            out.items[out.resolved++] = it;
            ++m_impl.fill_diag_total_items;
        }
        return true;
    }

    // ⛔ The filter is evaluated per GROUP, against the group's representative entity. Groups batch by MESH, and
    // a mesh group's entities share their archetype in every scene this renderer builds, so one test per group is
    // exact here — and it is O(groups) rather than O(instances), which matters at 10k instances.
    // An UNKNOWN component name matches NOTHING (and is reported once), because "I could not resolve this filter"
    // must never silently mean "this filter passes".
    [[nodiscard]] bool group_matches(const crd::framecook::FrameDrawListDesc& q, crd::usize group) const
    {
        if (m_impl.world == nullptr) { return true; } // no World bound (a stub-raster test): filters cannot apply
        const crd::containers::Array<crd::scene::EntityId>* slots = m_impl.group_entities(group);
        if (slots == nullptr || slots->size() == 0U) { return false; }
        const crd::scene::EntityId e = (*slots)[0];
        const auto has = [&](const crd::containers::String& n) {
            const crd::scene::ComponentId id =
                m_impl.world->component_id_by_name(crd::containers::StringView(n.c_str(), n.size()));
            return !id.is_null() && m_impl.world->has_component_id(e, id);
        };
        for (crd::usize i = 0; i < q.all.size(); ++i)
        {
            if (!has(q.all[i])) { return false; }
        }
        for (crd::usize i = 0; i < q.none.size(); ++i)
        {
            if (has(q.none[i])) { return false; }
        }
        if (q.any.size() > 0U)
        {
            bool any_hit = false;
            for (crd::usize i = 0; i < q.any.size() && !any_hit; ++i) { any_hit = has(q.any[i]); }
            if (!any_hit) { return false; }
        }
        return true;
    }

    SceneRenderer::Impl&                     m_impl;
    crd::gpu::IRasterTarget&                 m_target;
    crd::gpu::ClearColor                     m_clear;
    const crd::containers::Array<SceneDraw>& m_draws;
    // ⭐⭐ REN-40-C2: emitted-item index -> LOD slot. Parallel to `out.items`, filled by `fill()`.
    mutable crd::u32 m_item_slot[crd::framecook::kMaxDrawItems] = {};
    crd::gpu::FgImage                        m_out;
};

RenderStats SceneRenderer::render(crd::gpu::IRasterTarget& target, const crd::math::Mat4f& view_proj,
                                  const crd::math::Vec3f& light_dir, crd::gpu::ClearColor clear,
                                  const crd::scene::SpatialBVHIndex* bvh)
{
    RenderStats stats;
    Impl&       impl = *m_impl;
    // REN-8: wall-clock the WHOLE render call, including the frame graph's fence wait. render() has several
    // early returns, so an RAII stamp is the only way to time it without a leak-prone exit in each branch.
    struct CpuStamp
    {
        RenderStats*                                   s;
        std::chrono::steady_clock::time_point          t0 = std::chrono::steady_clock::now();
        ~CpuStamp()
        {
            s->cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
    } cpu_stamp{&stats};
    if (impl.program == nullptr) { return stats; }

    // REN-3.2-b: fit the frame's stabilized cascades. `render()` receives the combined view_proj, so the split
    // fitting reads the frustum shape out of it directly — the camera's separate view/proj are not available
    // here, and reconstructing them would be guesswork. `compute_csm_cascades` only needs the projection's
    // half-extents and the inverse view, both recoverable from view_proj for a standard camera.
    ++impl.frame_index; // ⭐⭐⭐ REN-41 (Stage 3): advance the temporal LOD-dither seed once per frame
    if (impl.shadows_active())
    {
        for (crd::u32 ci = 0; ci < kMaxCascades; ++ci) { impl.prev_light_vp[ci] = impl.cascades.light_vp[ci]; }
        impl.cascades = compute_csm_cascades_from_vp(view_proj, light_dir, impl.csm);
        ++impl.csm_frame;
    }
    // REN-37.3: the frame-frequency camera position, from the SAME exact reconstruction the cascade fit uses.
    const crd::math::Vec3f eye_ws = camera_position_from_vp(view_proj);

    // ── ⭐⭐ REN-41 (TAA): upload this frame's reproject matrix + constants. R = prev_UNJITTERED_vp · inv(cur
    // JITTERED_vp) maps a current pixel (world reconstructed from the jittered depth) to its position on last
    // frame's STABLE (unjittered) history grid — so the motion vector carries only real motion, never the jitter
    // delta (which otherwise samples history a fraction of a pixel off every frame and reads as a shimmer). R is
    // computed by the caller, which owns both the jittered and unjittered projections; the renderer only uploads.
    if (crd::gpu::IStorageBuffer* tc = impl.ensure_taa_constants(); tc != nullptr && impl.raster != nullptr)
    {
        crd::f32 consts[24] = {};
        std::memcpy(consts, &impl.taa_reproj, 16U * sizeof(crd::f32)); // R, column-major words 0..15
        consts[16] = target.width() > 0U ? 1.0F / static_cast<crd::f32>(target.width()) : 0.0F;
        consts[17] = target.height() > 0U ? 1.0F / static_cast<crd::f32>(target.height()) : 0.0F;
        consts[18] = impl.taa_feedback;                             // history feedback weight
        consts[19] = impl.taa_has_history ? 1.0F : 0.0F;            // no history on the very first frame
        (void)impl.raster->upload_storage(*tc, 0U, consts, sizeof(consts));
    }

    crd::math::Vec4f planes[6];
    frustum_planes(view_proj, planes);
    // ⛔ The frame's culled groups live in the CONTRIBUTION ARENA, not on the stack: the graph holds a pointer to
    // this list until `execute()`, which on the multi-viewport path happens long after this call returns.
    if (impl.contrib_used >= SceneRenderer::Impl::kMaxContributions) { return stats; } // stated cap, CHECKED
    const crd::u32                     contrib = impl.contrib_used++;
    crd::containers::Array<SceneDraw>& draw_list = impl.contrib_draws[contrib];
    draw_list.clear();
    impl.groups_view.clear();
    impl.draw_groups.clear();
    impl.fill_diag_cull_gates = 0U;
    impl.fill_diag_dispatch_max = 0U;
    impl.fill_diag_total_items = 0U;
    impl.fill_diag_index_count_0 = 0U;
    impl.impostor_draws.clear();

    // ── ⭐⭐ REN-38 (scene-buffer consolidation): can THIS frame render its plain groups from the ONE scene
    // buffer as a single multi-draw batch? Requires the rebased program, the single-viewport owner path (a
    // contribution would collide on the shared draw table), and a shadow-free frame (the shadowed forward pass
    // samples the atlas per draw — a per-draw binding a batch cannot share until material indices go bindless).
    // Skinned and textured groups keep their private buffers either way.
    // ⛔ An EXPLICITLY installed frame graph owns its own data flow (the authored CULL graph computes
    // visibility INTO the private buffers) — consolidation would split the frame across two buffers.
    const bool consolidate = impl.program_rebased != nullptr && impl.scene_ok() && contrib == 0U
                             && !impl.shadows_active() && !impl.frame_overridden;
    if (consolidate)
    {
        // assign region bases (an exact image of each group's private layout) and size the buffer
        crd::u32 total = kSceneFirstRegion;
        for (MeshGroup& group : m_groups)
        {
            if (group.skinned || group.buffer == nullptr) { continue; }
            group.region_base = total;
            // ⛔⛔ REN-40-C2: the region must cover the WHOLE group image, and it did not — it stopped at
            // `visible_off + capacity * (1 + cascades)`, which is exactly `bounds_off`, so the per-instance world
            // AABB section fell OUTSIDE the region and into the next group's header. It survived only because the
            // GPU cull reads the bounds through `base + header[bounds_off]` and the LAST group has slack behind
            // it. One derivation, from the group's own layout, so a new section can never be forgotten again.
            const crd::u32 atlas_words = group.has_impostor
                ? crd::lod::impostor_atlas_texels(impl.lod_policy.impostor_grid, impl.lod_policy.impostor_tile)
                : 0U;
            const crd::u32 region_words = group.atlas_off + atlas_words;
            total += (region_words + 3U) & ~3U;
        }
        if (impl.scene_buf == nullptr || impl.scene_buf_words < total)
        {
            impl.scene_buf        = impl.raster->create_storage_buffer(total * 4U);
            impl.scene_buf_words  = total;
            impl.scene_geom_valid = false;
        }
        if (impl.scene_buf != nullptr && !impl.scene_geom_valid)
        {
            // regions carry their own geometry image — uploaded on (re)build, exactly as the private buffers do
            for (MeshGroup& group : m_groups)
            {
                if (group.skinned || group.buffer == nullptr || group.mesh.get() == nullptr) { continue; }
                const auto* mesh = group.mesh.get();
                (void)impl.raster->upload_storage(*impl.scene_buf, (group.region_base + group.indices_off) * 4U,
                                                  mesh->indices.data(), static_cast<crd::u32>(mesh->indices.size()));
                if (group.has_impostor)
                {
                    const crd::u32 identity_ib[6] = {0U, 1U, 2U, 3U, 4U, 5U};
                    const crd::u32 ib_off = group.region_base + group.indices_off
                                            + static_cast<crd::u32>(mesh->indices.size() / 4U);
                    (void)impl.raster->upload_storage(*impl.scene_buf, ib_off * 4U, identity_ib, sizeof(identity_ib));
                    crd::lod::ImpostorAtlas atlas(impl.alloc);
                    (void)crd::lod::bake_impostor_atlas(*mesh, impl.lod_policy.impostor_grid,
                                                        impl.lod_policy.impostor_tile, atlas, impl.alloc);
                    if (!atlas.pixels.empty())
                    {
                        (void)impl.raster->upload_storage(*impl.scene_buf,
                                                          (group.region_base + group.atlas_off) * 4U,
                                                          atlas.pixels.data(),
                                                          static_cast<crd::u32>(atlas.pixels.size()));
                    }
                }
                (void)impl.raster->upload_storage(*impl.scene_buf, (group.region_base + group.vertices_off) * 4U,
                                                  mesh->vertices.data(),
                                                  static_cast<crd::u32>(mesh->vertices.size()));
            }
            impl.scene_geom_valid = true;
        }
    }
    // ⭐⭐ REN-40-C2: one RECORD per draw-list row — [0] region base, [1] LOD slot.
    crd::u32 draw_table[crd::framecook::kMaxDrawItems * kSceneDrawRowWords] = {};
    crd::u32 impostor_table[kImpostorDrawRows * kSceneDrawRowWords]         = {};
    bool     wrote_frame_header = false;

    // broad phase: a configured BVH prunes to the frustum's AABB; otherwise every slot is a candidate
    const bool use_bvh = bvh != nullptr && bvh->is_configured();
    crd::containers::Array<crd::scene::EntityId> candidates(impl.alloc);
    if (use_bvh) { bvh->overlap(frustum_aabb(view_proj), candidates); }

    // ── the MIN-DRAW screen-size cull, CPU side — the same camera-projected metric the device kernels apply
    // (see CullDesc::caster_min_px). Sub-pixel geometry cannot contribute anything but aliasing energy, so
    // instances whose projected height falls below `min_draw_px` do not draw at all. Row norms per SCAR-5.
    const crd::f32 cull_r1_len = crd::math::sqrt(view_proj.c0.y * view_proj.c0.y + view_proj.c1.y * view_proj.c1.y
                                                 + view_proj.c2.y * view_proj.c2.y);
    const crd::f32 cull_h_px   = static_cast<crd::f32>(target.height());
    const auto     draw_px_ok  = [&](const crd::geometry::primitives::AABB3<crd::f32>& b) {
        if (!(impl.min_draw_px > 0.0F)) { return true; }
        const crd::f32 ex = (b.max.x - b.min.x) * 0.5F;
        const crd::f32 ey = (b.max.y - b.min.y) * 0.5F;
        const crd::f32 ez = (b.max.z - b.min.z) * 0.5F;
        const crd::f32 r  = crd::math::sqrt(ex * ex + ey * ey + ez * ez);
        const crd::f32 cx = (b.min.x + b.max.x) * 0.5F;
        const crd::f32 cy = (b.min.y + b.max.y) * 0.5F;
        const crd::f32 cz = (b.min.z + b.max.z) * 0.5F;
        const crd::f32 w  = view_proj.c0.w * cx + view_proj.c1.w * cy + view_proj.c2.w * cz + view_proj.c3.w;
        if (!(w > 1.0e-5F)) { return true; } // behind / at the near plane: the frustum test owns the verdict
        return (r * cull_r1_len * cull_h_px) / w >= impl.min_draw_px;
    };

    for (MeshGroup& group : m_groups)
    {
        group.visible.clear();
        const auto count = static_cast<crd::u32>(group.instances.size());
        if (count == 0U || group.buffer == nullptr) { continue; }

        // ⭐⭐ REN-40-A: UNDER THE DEVICE CULL THE CPU DOES NOT CULL AT ALL. This is the performance the slice
        // exists for: at 1M instances the camera test plus four cascade tests are 5M `aabb_in_frustum` calls and
        // the visible-list uploads that follow them measured ~160 ms of a 337 ms frame. Skipping them is only
        // legal because EVERY consumer of the result is now GPU-driven — the draws take their counts from device
        // memory (`DrawItem::args`) — which is why `set_gpu_cull` requires the indexed path.
        // ⛔ The header still uploads, and so do the bounds: the kernel reads both. What stops is the CPU's own
        // verdict, not the data the device needs to reach its own.
        if (impl.gpu_cull_on && !impl.gpu_cull_verify)
        {
            group.visible.clear();
        }
        else if (use_bvh)
        {
            // REN-40-C4: when dither is active the VS masks the entry with 0x00FFFFFF and reads bits
            // 24..31 as fade alpha — a bare slot has alpha 0 and discards every pixel (black frame).
            // Pack 0xFF (fully opaque) so the CPU path renders without a transition.
            const bool dither_pack = impl.lod_enabled && impl.lod_policy.dither_band > 0.0F
                                     && impl.lod_slots > 1U;
            for (const crd::scene::EntityId e : candidates)
            {
                const crd::u64* packed = impl.entity_slot.find(e);
                if (packed == nullptr) { continue; }
                const auto gi   = static_cast<crd::u32>(*packed >> 32U);
                const auto slot = static_cast<crd::u32>(*packed & 0xFFFFFFFFU);
                if (&m_groups[gi] != &group) { continue; }
                if (aabb_in_frustum(group.world_bounds[slot], planes) && draw_px_ok(group.world_bounds[slot]))
                {
                    group.visible.push_back(dither_pack ? (0xFFU << 24U) | slot : slot);
                }
            }
        }
        else
        {
            const bool dither_pack = impl.lod_enabled && impl.lod_policy.dither_band > 0.0F
                                     && impl.lod_slots > 1U;
            for (crd::u32 slot = 0; slot < count; ++slot)
            {
                if (aabb_in_frustum(group.world_bounds[slot], planes) && draw_px_ok(group.world_bounds[slot]))
                {
                    group.visible.push_back(dither_pack ? (0xFFU << 24U) | slot : slot);
                }
            }
        }

        const auto visible_count = static_cast<crd::u32>(group.visible.size());
        group.visible_count_cpu  = visible_count; // REN-40-A: the reference the device cull is compared against
        stats.culled_instances += count - visible_count;
        // ⛔⛔ UNDER THE DEVICE CULL AN EMPTY CPU LIST IS THE NORMAL CASE, NOT A REASON TO SKIP. This `continue`
        // is what drops the group's HEADER upload — and the cull kernel reads the header for the bounds offset,
        // the instance count and the visible-list stride, so skipping it would hand the kernel last frame's
        // header (or none at all) and the device would cull against stale data.
        if (visible_count == 0U && !impl.gpu_cull_on) { continue; }

        // per-frame uploads: the 32-word header + the visible list
        crd::u32 header[kHeaderWords] = {};
        header[0] = group.index_count;
        header[1] = visible_count;
        header[2] = group.indices_off;
        header[3] = group.vertices_off;
        header[4] = group.instances_off;
        header[5] = group.visible_off;
        std::memcpy(&header[6], &view_proj, 16U * 4U);
        std::memcpy(&header[22], &light_dir, 3U * 4U);
        // ⭐ REN-38-E7: the authored light ARRAY. One directional record, laid out exactly as
        // `assets/lighting/scene_forward.crdl` declares it — the declaration and the upload agree because the
        // record offsets are the SAME numbers in both, which is the point of declaring them.
        // ⛔ Floats go in as BIT PATTERNS: the pull shader reinterprets the word, it does not convert it.
        header[kHdrLightOff] = kHeaderWords - kLightSectionWords;
        // REN-38-F6+: the GPU cull kernel's range guard — the group's TOTAL instance count
        header[kHdrInstanceCount] = static_cast<crd::u32>(group.instances.size());
        header[kHdrInstanceCapacity] = group.capacity; // 38-G1: the per-cascade visible-list stride
        header[kHdrFrameIndex]       = impl.frame_index; // ⭐⭐⭐ REN-41 (Stage 3): the temporal LOD-dither seed
        header[kHdrBoundsOff]        = group.bounds_off; // REN-40-A: the GPU cull's world-AABB section
        header[kHdrLodOverrideOff]   = group.lod_override_off; // REN-40-C2: the per-instance LOD override
        header[kHdrPrevWorldOff]     = group.prev_world_off;   // REN-41 velocity: per-instance previous transform
        header[kHdrPrevPaletteOff]   = group.skinned ? group.prev_palette_off : 0U; // REN-41: prev pose (skinned only)
        header[kHdrGpuSkinActive]    = impl.gpu_skinning_on ? 1U : 0U; // REN-41: gate the device palette_snapshot pass
        if (group.has_impostor)
        {
            header[kHdrAtlasOff]  = group.atlas_off;
            header[kHdrAtlasDims] = (impl.lod_policy.impostor_grid << 16U) | impl.lod_policy.impostor_tile;
        }
        // ⭐⭐ REN-40-C2: THE LOD TABLE. `first_index` is published ABSOLUTE (the
        // group's index section base plus the level's own offset) because that is
        // what an indexed draw command consumes — the kernel writing a command must
        // not have to know how the group was laid out.
        if (group.lod_count > 0U) { header[kHdrLodCount] = group.lod_count; }
        for (crd::u32 l = 0; l < group.lod_count && l < kMaxLodSlots; ++l)
        {
            header[kHdrLodTable + (l * 2U) + 0U] = group.indices_off + group.lod_first[l];
            header[kHdrLodTable + (l * 2U) + 1U] = group.lod_indices[l];
            const crd::f32 h                     = group.lod_height[l];
            std::memcpy(&header[kHdrLodHeight + l], static_cast<const void*>(&h), 4U);
        }
        {
            const crd::u32 lb = header[kHdrLightOff];
            const crd::f32 white[3] = {1.0F, 1.0F, 1.0F};
            std::memcpy(&header[lb + 4U], static_cast<const void*>(white), 3U * 4U);   // color @4
            // ⛔⛔ THE CONVENTION BOUNDARY, and it renders PURE BLACK if you get it wrong. The header stores
            // the direction TOWARD the light at [22..24]; the light record’s `direction` field is the
            // direction light TRAVELS, because that is what `lighting::directional_light` negates internally.
            // Copying the header value straight through flips L away from the light, so N·L ≤ 0 everywhere
            // and the scene is uniformly dark — the same scar `build_scene_fs_cooked` documents one negation
            // over. Negate ONCE, here, at the boundary.
            const crd::f32 travel[3] = {-light_dir.x, -light_dir.y, -light_dir.z};
            std::memcpy(&header[lb + 8U], static_cast<const void*>(travel), 3U * 4U); // direction @8
        }

        header[25] = group.skin_off;    // GEO-8: the skinned VS's extra sections
        header[26] = group.palette_off;
        header[27] = group.joint_count;
        // ⭐⭐ REN-40-F: GPU skinning section offsets — the compute kernel reads these to find the skeleton,
        // pre-baked clip data, and per-instance animation state in the group's buffer.
        header[kHdrSkelOff]      = group.skel_off;
        header[kHdrClipOff]      = group.clip_off;
        header[kHdrAnimStateOff] = group.anim_state_off;
        // REN-3.2-b: the frame's stabilized cascades ride the SAME header every pull shader already reads, so
        // the shadow VS needs no extra binding and the forward FS can select its cascade from the splits.
        std::memcpy(&header[kHdrCsmSplits], impl.cascades.split_far, kMaxCascades * 4U);
        std::memcpy(&header[kHdrCsmLightVp], impl.cascades.light_vp, kMaxCascades * 16U * 4U);
        // ⭐ REN-37.3: the FRAME-frequency camera position, so the forward BRDF has a real view vector. Derived
        // from `view_proj` rather than passed in, so it cannot disagree with the matrix beside it in the header.
        std::memcpy(&header[kHdrCameraPos], &eye_ws, 3U * 4U);
        // ⭐⭐ REN-38: a plain group in a consolidated frame writes its region of THE scene buffer instead of
        // its private one — header at the region base, visible list at its region offset, and the FULL instance
        // payload (the region's copy is not covered by sync's dirty tracking; this is the per-frame cost the
        // consolidation pays for one-batch drawing, in the same class as the header+visible writes beside it).
        const bool use_region = consolidate && !group.skinned && impl.scene_buf != nullptr
                                && group.region_base != 0U
                                && impl.resolve_base_color_texture(group.material) == nullptr;
        if (use_region)
        {
            const crd::u32 rb = group.region_base;
            (void)impl.raster->upload_storage(*impl.scene_buf, rb * 4U, header, sizeof(header));
            // ⛔ 0 words means the DEVICE fills this list (see the gpu_cull note above) — `data()` on an empty
            // array is null, and handing a null pointer to an upload is not a "harmless no-op" contract to lean on.
            if (visible_count > 0U)
            {
                (void)impl.raster->upload_storage(*impl.scene_buf, (rb + group.visible_off) * 4U,
                                                  group.visible.data(), visible_count * 4U);
            }
            const crd::u32 inst_bytes = static_cast<crd::u32>(group.instances.size())
                                        * static_cast<crd::u32>(sizeof(InstanceGpu));
            (void)impl.raster->upload_storage(*impl.scene_buf, (rb + group.instances_off) * 4U,
                                              group.instances.data(), inst_bytes);
            stats.uploaded_bytes += sizeof(header) + static_cast<crd::u64>(visible_count) * 4U + inst_bytes;
            if (!wrote_frame_header)
            {
                // word 0 = THE frame header the (absolute-reading) FS samples — one canonical copy
                (void)impl.raster->upload_storage(*impl.scene_buf, 0U, header, sizeof(header));
                wrote_frame_header = true;
            }
        }
        else
        {
            (void)impl.raster->upload_storage(*group.buffer, 0U, header, sizeof(header));
            if (visible_count > 0U) // see the null-data note in the region branch
            {
                (void)impl.raster->upload_storage(*group.buffer, group.visible_off * 4U, group.visible.data(),
                                                  visible_count * 4U);
            }
            stats.uploaded_bytes += sizeof(header) + static_cast<crd::u64>(visible_count) * 4U;
        }
        // ── ⭐⭐ 38-G1 perf: PER-CASCADE CULLING. Each cascade gets its OWN visible list, tested against THAT
        // cascade's light clip volume. The shadow passes used to draw the CAMERA's list four times, and
        // cascade 0 covers a few metres of a 110-unit field — so nearly all of that geometry was pulled,
        // transformed and then clipped. Measured waste: 8 ms of GPU, 130 fps -> 53 with shadows on.
        // ⛔⛔ REN-39-D1: THE CASCADE LISTS MUST LAND IN THE BUFFER THE CASCADE DRAW ACTUALLY BINDS. A
        // consolidated group draws REBASED over `scene_buf` (`d.buffer = impl.scene_buf` below), and its region
        // already reserves the per-cascade blocks (`region_base` sizing above counts `1 + kMaxCascades` lists).
        // Uploading them to the group's PRIVATE buffer instead left every cascade pass pulling whatever happened
        // to sit at that region of the scene buffer: the atlas held a handful of stale instances rather than the
        // scene, so most casters threw NO shadow at all and the few that did landed nowhere near their caster.
        // It reads as "the shadows are in completely wrong places" and points at neither the fit nor the bias.
        crd::gpu::IStorageBuffer* const cdst      = use_region ? impl.scene_buf.get() : group.buffer.get();
        const crd::u32                  cdst_base = use_region ? group.region_base : 0U;
        // ⭐⭐ REN-40-A: hand the cull kernels this group's BASE. `cdst_base` is already exactly the number the
        // per-cascade list uploads use, so the kernel and the CPU cull agree by construction rather than by two
        // parallel derivations. ⛔ Written only when it CHANGES — a per-frame per-group upload is the shape that
        // measured 8.3 ms/frame in the queue-idle scar.
        // ⭐⭐ REN-40-C2: the params block also carries EACH VIEW'S HEIGHT IN PIXELS — the LOD selector's only
        // runtime input beyond the matrix and the box. The camera's is the render target's height; a cascade's is
        // its atlas slice's. ⛔ Folded into the SAME change-gated write: a resize or a cascade-size change moves
        // it, and nothing else does, so the per-frame cost stays zero.
        if (group.cull_args != nullptr)
        {
            crd::u32 params[kCullArgsHeaderWords] = {};
            params[0]                             = cdst_base;
            // ⭐⭐ REN-40-C3: THE PER-VIEW BIAS FOLDS IN HERE, and that is the whole implementation. The selector
            // already divides by a per-view pixel height; biasing a view is exactly scaling that number, so the
            // kernel needs NO change and there is no second place for the two to disagree.
            // ⛔ Cascade 3 sits at roughly 0.18 world-units per texel — it physically cannot resolve what the
            // forward pass can — yet it was selecting the same level for the same instance, so the most
            // expensive pass in the frame drew the finest geometry into the coarsest buffer.
            const crd::f32 cam_px =
                static_cast<crd::f32>(target.height()) * (impl.lod_enabled ? impl.lod_policy.view_bias[0] : 1.0F);
            std::memcpy(&params[kCullArgsPixelHeight], static_cast<const void*>(&cam_px), 4U);
            for (crd::u32 cv = 0; cv < kMaxCascades; ++cv)
            {
                const crd::f32 casc_px = static_cast<crd::f32>(impl.csm.map_size)
                                         * (impl.lod_enabled ? impl.lod_policy.view_bias[1U + cv] : 1.0F);
                std::memcpy(&params[kCullArgsPixelHeight + 1U + cv], static_cast<const void*>(&casc_px), 4U);
            }
            // ⭐⭐ REN-40-C2 / D3D12: this group's FIRST DRAW-LIST ROW. The rows for this group's (slot) items
            // start exactly here — the loop below pushes them — and D3D12's reset writes `base_row + slot` into
            // each command's DrawIndex root constant. ⛔ It joins the change signature: a row that moved because
            // an earlier group appeared or vanished must re-upload, or every D3D12 draw of this group reads
            // another group's table row.
            params[kCullArgsBaseRow] = static_cast<crd::u32>(draw_list.size());
            // the HZB's texel dims (half the target) — the conservative occlusion test's span check. They ride
            // the same change-gated write: a resize moves them, nothing else does.
            const crd::f32 hzb_w = static_cast<crd::f32>(target.width()) * 0.5F;
            const crd::f32 hzb_h = static_cast<crd::f32>(target.height()) * 0.5F;
            std::memcpy(&params[kCullArgsHzbSize + 0U], static_cast<const void*>(&hzb_w), 4U);
            std::memcpy(&params[kCullArgsHzbSize + 1U], static_cast<const void*>(&hzb_h), 4U);
            crd::u64 sig = cdst_base;
            sig          = (sig * 1099511628211ULL) ^ static_cast<crd::u64>(target.height());
            sig          = (sig * 1099511628211ULL) ^ static_cast<crd::u64>(target.width());
            sig          = (sig * 1099511628211ULL) ^ static_cast<crd::u64>(impl.csm.map_size);
            sig          = (sig * 1099511628211ULL) ^ static_cast<crd::u64>(params[kCullArgsBaseRow]);
            // ⛔ the BIAS joins the change signature: installing a policy with different per-view biases
            // must re-upload, or the selector keeps using the previous policy's pixel heights forever.
            sig          = (sig * 1099511628211ULL) ^ static_cast<crd::u64>(params[kCullArgsPixelHeight]);
            for (crd::u32 cv = 0; cv < kMaxCascades; ++cv)
            {
                sig = (sig * 1099511628211ULL)
                      ^ static_cast<crd::u64>(params[kCullArgsPixelHeight + 1U + cv]);
            }
            if (group.cull_params_sig != sig
                && impl.raster->upload_storage(*group.cull_args, 0U, params, sizeof(params)))
            {
                group.cull_base_uploaded = cdst_base;
                group.cull_params_sig    = sig;
                stats.uploaded_bytes += sizeof(params);
            }
        }
        if (impl.shadows_active() && cdst != nullptr && (!impl.gpu_cull_on || impl.gpu_cull_verify))
        {
            // the shadow-caster screen-size cull, CPU side — the same camera-projected metric the device
            // kernels apply (see CullDesc::caster_min_px), derived from the SAME row norms (the SCAR-5 rule).
            const crd::f32 cam_r1_len = crd::math::sqrt(view_proj.c0.y * view_proj.c0.y
                                                        + view_proj.c1.y * view_proj.c1.y
                                                        + view_proj.c2.y * view_proj.c2.y);
            const crd::f32 cam_h_px   = static_cast<crd::f32>(target.height());
            const auto     caster_px_ok = [&](const crd::geometry::primitives::AABB3<crd::f32>& b) {
                if (!(impl.shadow_caster_min_px > 0.0F)) { return true; }
                const crd::math::Vec3f cen{(b.min.x + b.max.x) * 0.5F, (b.min.y + b.max.y) * 0.5F,
                                           (b.min.z + b.max.z) * 0.5F};
                const crd::math::Vec3f ext{(b.max.x - b.min.x) * 0.5F, (b.max.y - b.min.y) * 0.5F,
                                           (b.max.z - b.min.z) * 0.5F};
                const crd::f32 r = crd::math::sqrt(ext.x * ext.x + ext.y * ext.y + ext.z * ext.z);
                const crd::f32 w = view_proj.c0.w * cen.x + view_proj.c1.w * cen.y + view_proj.c2.w * cen.z
                                   + view_proj.c3.w;
                // a caster BEHIND the camera has w <= 0: its screen size is meaningless and it always passes —
                // dropping it would delete exactly the off-screen shadows the full-set cull exists to keep.
                if (!(w > 1.0e-5F)) { return true; }
                return (r * cam_r1_len * cam_h_px) / w >= impl.shadow_caster_min_px;
            };
            crd::containers::Array<crd::u32> clist(impl.alloc);
            for (crd::u32 c = 0; c < impl.cascades.count && c < kMaxCascades; ++c)
            {
                crd::math::Vec4f cplanes[6];
                frustum_planes(impl.cascades.light_vp[c], cplanes);
                clist.clear();
                // ⛔ Cull from the FULL instance set, not the camera's list: a caster BEHIND the camera still
                // casts into the frame, and starting from the camera's set would delete exactly those shadows
                // (the classic "shadows pop when you turn around" bug).
                for (crd::u32 slot = 0; slot < count; ++slot)
                {
                    if (aabb_in_frustum(group.world_bounds[slot], cplanes)
                        && caster_px_ok(group.world_bounds[slot]))
                    {
                        clist.push_back(slot);
                    }
                }
                const auto ccount = static_cast<crd::u32>(clist.size());
                group.cascade_visible_count[c] = ccount;
                if (ccount > 0U)
                {
                    (void)impl.raster->upload_storage(
                        *cdst, (cdst_base + group.visible_off + (1U + c) * group.capacity) * 4U, clist.data(),
                        ccount * 4U);
                }
            }
        }

        // REN-2 Half B: a group whose material carries a base-color map draws TEXTURED (samples albedo); else flat.
        crd::gpu::ITexture*       base_color = impl.resolve_base_color_texture(group.material);
        crd::gpu::IRasterProgram* program    = impl.program.get();
        if (group.skinned && impl.program_skinned != nullptr) { program = impl.program_skinned.get(); }
        const bool want_shadow = impl.shadows_active();
        if (group.skinned)
        {
            if (want_shadow && base_color != nullptr && impl.program_skinned_textured_shadowed != nullptr)
            {
                program = impl.program_skinned_textured_shadowed.get();
            }
            else if (want_shadow && base_color == nullptr && impl.program_skinned_shadowed != nullptr)
            {
                program = impl.program_skinned_shadowed.get();
            }
            else if (base_color != nullptr && impl.program_skinned_textured != nullptr)
            {
                program = impl.program_skinned_textured.get();
            }
            else { base_color = nullptr; }
        }
        else if (want_shadow && base_color != nullptr && impl.program_textured_shadowed != nullptr)
        {
            program = impl.program_textured_shadowed.get();
        }
        else if (want_shadow && base_color == nullptr && impl.program_shadowed != nullptr)
        {
            program = impl.program_shadowed.get();
        }
        else if (base_color != nullptr && impl.program_textured != nullptr) { program = impl.program_textured.get(); }
        else { base_color = nullptr; }
        SceneDraw d;
        d.program      = program;
        d.buffer       = group.buffer.get();
        // ⭐⭐ REN-40-A: the group's GPU-written commands + the cull grid for its instance count. The VIEW offset
        // is applied per pass by the expansion index; view 0 (the camera) is the default here.
        d.cull_args    = group.cull_args.get();
        d.cull_groups  = (count + 63U) / 64U; // the authored cull kernel's workgroup is 64
        d.base_color   = base_color;
        d.vertex_count = visible_count * group.index_count;
        if (use_region)
        {
            // ⭐⭐ REN-38: the consolidated draw — the rebased program over THE scene buffer; its table row is
            // its position in this draw list (the executor pushes exactly that as the batch's first index).
            d.program = impl.program_rebased.get();
            d.buffer  = impl.scene_buf.get();
            d.rebased = true;
            // ⭐ REN-40-C2: the table ROW is written per SLOT, where the item is actually pushed (below).
        }
        // ── ⭐⭐ REN-39-C1: THE INDEXED-PULL SWITCH. The chosen program is replaced by its indexed twin (the
        // set is all-or-nothing, so every twin the chain can pick exists when `use_indexed` survived init) and
        // the draw carries the indexed fields: index_count per instance, the visible count, and the ABSOLUTE
        // word of this group's index section (private buffer: indices_off; region: region_base + indices_off).
        if (impl.use_indexed)
        {
            crd::gpu::IRasterProgram* iprog = nullptr;
            if (d.rebased)
            {
                iprog = impl.program_rebased_idx.get();
            }
            else if (group.skinned)
            {
                if (want_shadow && base_color != nullptr && impl.program_skinned_textured_shadowed_idx != nullptr)
                {
                    iprog = impl.program_skinned_textured_shadowed_idx.get();
                }
                else if (want_shadow && base_color == nullptr && impl.program_skinned_shadowed_idx != nullptr)
                {
                    iprog = impl.program_skinned_shadowed_idx.get();
                }
                else if (base_color != nullptr && impl.program_skinned_textured_idx != nullptr)
                {
                    iprog = impl.program_skinned_textured_idx.get();
                }
                else
                {
                    iprog = impl.program_skinned_idx != nullptr ? impl.program_skinned_idx.get() : impl.program_idx.get();
                }
            }
            else if (want_shadow && base_color != nullptr)
            {
                iprog = impl.program_textured_shadowed_idx.get();
            }
            else if (want_shadow)
            {
                iprog = impl.program_shadowed_idx.get();
            }
            else if (base_color != nullptr)
            {
                iprog = impl.program_textured_idx.get();
            }
            else
            {
                iprog = impl.program_idx.get();
            }
            if (iprog != nullptr)
            {
                d.program = iprog;
                d.index_count = group.index_count;
                d.instance_count = visible_count;
                d.first_index = (d.rebased ? group.region_base : 0U) + group.indices_off;
                // the depth-only twin a non-instance depth pass (the prepass) must draw with — never the
                // forward program above (see DrawItem::program_depth for the device fault that caused)
                d.program_depth = group.skinned && impl.program_prepass_skinned_idx != nullptr
                                      ? impl.program_prepass_skinned_idx.get()
                                      : impl.program_prepass_idx.get();
                // ⭐⭐ REN-41 (velocity): the motion-vector twin for the MRT velocity prepass — same skinned/rigid
                // selection. Null when velocity did not cook, which the velocity pass tolerates (falls back).
                d.program_velocity = group.skinned && impl.program_skinned_velocity_idx != nullptr
                                         ? impl.program_skinned_velocity_idx.get()
                                         : impl.program_velocity_idx.get();
            }
        }
        // ── ⭐⭐ REN-40-C2: ONE DRAW ITEM PER (GROUP, LOD SLOT) — the "render item" of the frontier GPU-driven
        // designs, an atomic (mesh x material x level) unit with its own indirect command. ⛔ The slot cannot be
        // a cook-time constant the way the CASCADE is: a frame-graph pass binds ONE program for its whole list,
        // so the level has to arrive per DRAW — through the draw table row, the only per-draw channel a
        // GPU-written multi-draw has. ⛔ Emitted only when the group HAS that level: a command for a level the
        // mesh does not carry would be an empty draw every frame, and the row would still cost a table slot.
        // ⛔ Under the CPU cull `lod_slots` is 1 by construction (the policy sizes it and selection is a device
        // decision), so this loop runs exactly once and the list is what it always was.
        // ⭐⭐ THE SLOT IS READABLE ON BOTH PATHS NOW. Every buffer carries the draw table at `kSceneDrawTableOff`
        // (see `kGroupSectionsOff`), so a private-buffer draw reads its row exactly as a consolidated one does —
        // its row simply carries base 0. Before that the table existed ONLY in the consolidated scene buffer,
        // `consolidate` is disabled whenever shadows are active, and so in every shadowed frame the stage had no
        // row, computed slot 0, and read slot 0's visible list — EMPTY, because the cull sent those survivors to
        // slots 1..n. Levels 1 and coarser drew NOTHING while the frame rendered, every count reconciled, and GPU
        // time DROPPED — it read as an LOD win. That is why the table stopped being a property of one path.
        crd::u32 slots_here = 1U;
        if (impl.gpu_cull_on && group.lod_count > 1U) { slots_here = impl.lod_slots; }
        // ⭐⭐ REN-40-C5: the impostor slot is the LAST slot when impostor_grid > 0. It draws through its
        // OWN pass (billboard VS + impostor FS), so the mesh draw list SKIPS it here. The cull kernel still
        // routes instances into its visible list; the impostor pass reads that list with its own program.
        const crd::u32 impostor_slot = (impl.lod_policy.impostor_grid > 0U && impl.lod_slots > 1U)
                                           ? impl.lod_slots - 1U : 0xFFFFFFFFU;
        for (crd::u32 sl = 0; sl < slots_here; ++sl)
        {
            if (sl > 0U && sl >= group.lod_count) { break; }
            if (sl == impostor_slot) { continue; }
            SceneDraw ds = d;
            ds.lod_slot  = sl;
            if (sl > 0U)
            {
                // The CPU-side fields describe the LEVEL. Under the indirect path the device command overrides
                // them, but a frame that falls back to CPU args must still draw the right range.
                ds.index_count  = group.lod_indices[sl];
                ds.first_index  = (ds.rebased ? group.region_base : 0U) + group.indices_off + group.lod_first[sl];
                ds.vertex_count = 0U; // the coarse levels only ever draw indexed-indirect
            }
            if (draw_list.size() < crd::framecook::kMaxDrawItems)
            {
                const crd::usize row = draw_list.size() * kSceneDrawRowWords;
                draw_table[row + 0U] = ds.rebased ? group.region_base : 0U;
                draw_table[row + 1U] = sl;
            }
            draw_list.push_back(ds);
            // REN-36.3-b: mirror this group's entities alongside the draw so the asset's component filter has
            // something to test. Index-parallel with `draw_list` BY CONSTRUCTION — culled groups are skipped in
            // both, and every slot of one group mirrors the SAME entity set.
            crd::containers::Array<crd::scene::EntityId> ents(impl.alloc);
            for (crd::usize k = 0; k < group.slot_entity.size(); ++k) { ents.push_back(group.slot_entity[k]); }
            impl.groups_view.push_back(static_cast<crd::containers::Array<crd::scene::EntityId>&&>(ents));
            impl.draw_groups.push_back(&group); // 38-G1: index-parallel, for the per-cascade counts
            ++stats.draws;
        }
        // ⭐⭐ REN-40-C5: emit the IMPOSTOR DRAW for this group into the separate impostor list. One draw per
        // group with an impostor slot, using the billboard VS + impostor FS and the identity IB. The impostor
        // draw table is separate from the mesh table (kImpostorTableOff) to avoid DrawIndex collision with
        // cascade passes that override per-item programs.
        if (impostor_slot != 0xFFFFFFFFU && group.has_impostor)
        {
            crd::gpu::IRasterProgram* imp_prog = impl.ensure_impostor_program();
            if (imp_prog != nullptr)
            {
                SceneDraw ids = d;
                ids.program    = imp_prog;
                ids.lod_slot   = impostor_slot;
                ids.index_count  = group.lod_indices[impostor_slot];
                ids.first_index  = (ids.rebased ? group.region_base : 0U) + group.indices_off
                                   + group.lod_first[impostor_slot];
                ids.vertex_count = 0U;
                if (impl.impostor_draws.size() < kImpostorDrawRows)
                {
                    const crd::usize irow = impl.impostor_draws.size() * kSceneDrawRowWords;
                    impostor_table[irow + 0U] = ids.rebased ? group.region_base : 0U;
                    impostor_table[irow + 1U] = impostor_slot;
                }
                impl.impostor_draws.push_back(ids);
                ++stats.draws;
            }
        }
        stats.drawn_instances += visible_count;
    }

    // ⭐⭐ REN-40-C2: ONE canonical table per frame — row i belongs to draw-list item i, which is exactly what the
    // multi verb pushes as DrawIndex — REPLICATED into every buffer a draw can bind. ⛔ Replicated rather than
    // shared because a private-buffer draw can only read its OWN buffer, while the row it needs is its position
    // in the GLOBAL draw list; a per-group table indexed by a global row would be wrong for every group but the
    // first. It costs 2 KB per group per frame, against the alternative of LOD not working at all outside the
    // consolidated path — which is what it did.
    if (consolidate && impl.scene_buf != nullptr && wrote_frame_header)
    {
        (void)impl.raster->upload_storage(*impl.scene_buf, kSceneDrawTableOff * 4U, draw_table,
                                          sizeof(draw_table));
        if (impl.impostor_draws.size() > 0U)
        {
            (void)impl.raster->upload_storage(*impl.scene_buf, kImpostorTableOff * 4U, impostor_table,
                                              sizeof(impostor_table));
        }
    }
    for (MeshGroup& group : m_groups)
    {
        if (group.buffer == nullptr || group.instances.size() == 0U) { continue; }
        (void)impl.raster->upload_storage(*group.buffer, kSceneDrawTableOff * 4U, draw_table, sizeof(draw_table));
        if (impl.impostor_draws.size() > 0U)
        {
            (void)impl.raster->upload_storage(*group.buffer, kImpostorTableOff * 4U, impostor_table,
                                              sizeof(impostor_table));
        }
        stats.uploaded_bytes += sizeof(draw_table) + sizeof(impostor_table);
    }
    if (draw_list.size() == 0U) { return stats; }

    // REN-1: compose all N culled groups in ONE submission through the frame graph (the async single-submission
    // surface). The per-frame header/visible uploads already ran (synchronous transfers, complete before this).
    // ⭐ REN-37.8: THE RENDERER CONTRIBUTES TO A GRAPH; IT DOES NOT NECESSARILY OWN ONE.
    // When `external_fg` is set (via `contribute()`), this call only RECORDS — the caller owns reset / build /
    // execute, so N viewports compose into ONE graph and ONE submission. Without that split an editor with a main
    // viewport, an animation preview and 12 dirty thumbnails submits FOURTEEN TIMES, allocates every viewport's
    // transients separately (peak VRAM = SUM instead of MAX), and cannot order one viewport against another.
    // The single-viewport `render()` path is the same code with `owns_graph` true.
    crd::gpu::IFrameGraph* use_fg     = impl.external_fg;
    const bool             owns_graph = use_fg == nullptr;
    if (owns_graph)
    {
        if (impl.frame_graph == nullptr) { impl.frame_graph = impl.raster->create_frame_graph(); }
        use_fg = impl.frame_graph.get();
    }
    if (use_fg != nullptr)
    {
        crd::gpu::IFrameGraph& fg = *use_fg;
        if (owns_graph)
        {
            fg.set_readback_enabled(impl.readback); // re-applied per frame: the graph is created lazily
            fg.reset();
            impl.contrib_used = 0U; // the owner's reset also recycles the contribution arena
            // ⛔⛔ AND THE RECORDER'S ARENA. `FrameRecorder` hands out one PassRec block per `record()` from a
            // fixed ring of 32, and `begin_frame()` is what returns them. Nothing called it: the counter
            // climbed one per frame and the 33rd frame — about half a second in — began failing EVERY record
            // with BuildRejected, forever. The app froze on its authored frame while every offscreen gate
            // (one render per test) stayed green. ⛔ A per-frame arena needs a per-frame reset ON THE LIVE
            // PATH, and only an app that runs for more than 32 frames can prove it.
            impl.recorder.begin_frame();
        }
        const crd::gpu::FgImage img = fg.import_target(target);

        // ⭐⭐ REN-37.10: THE RENDERER HOSTS AN AUTHORED GRAPH. It does not build passes any more.
        //
        // What used to be here was ~50 lines of C++ that created the cascade atlas, added one depth-only pass per
        // cascade, added the scene pass, and wired the reads that order them — carrying its own notice that it
        // violated the top rule. It is GONE. The identical frame is now `assets/frame/forward_csm.frame.toml`,
        // recorded through `FrameRecorder`, with this renderer supplying only what a graph CANNOT know: the
        // target, the resolved draw lists, the cascade count, and the per-cascade programs.
        //
        // The deletion is the proof. Cascade count, atlas size and format, pass order, what casts, what receives,
        // and which TECHNIQUE shades it are all asset text now — none of them need a rebuild.
        SceneHost host(impl, target, clear, draw_list, img);
        crd::framecook::FrameExecError  ferr = crd::framecook::FrameExecError::Ok;
        crd::containers::String         fwhere(impl.alloc);
        // ~~REN-38-F6: an EXPLICITLY installed graph (set_frame_graph_toml) is the frame, full stop~~ —
        // SUPERSEDED (REN-39): the graph's own DECLARED capability tier is part of the asset's contract, so it
        // applies to EVERY installed frame, not just the built-in pair. When a `requires` capability is missing,
        // the recording path steps down to the graph the asset's `fallback` NAMES — resolved through the same
        // disk-first asset system — and REPORTS it. (Before this, installing `forward_csm_agx` with shadows off
        // failed to record every frame — `for_each` answered 0 cascades — and the app presented BLACK.)
        const crd::framecook::FrameGraphDesc* authored = &impl.frame;
        {
            const crd::containers::String* missing = nullptr;
            for (crd::usize ci = 0; ci < authored->requires_caps.size(); ++ci)
            {
                const crd::containers::String& cap = authored->requires_caps[ci];
                if (!host.capability(crd::containers::StringView(cap.c_str(), cap.size())))
                {
                    missing = &cap;
                    break;
                }
            }
            if (missing != nullptr && !authored->fallback.empty())
            {
                const crd::framecook::FrameGraphDesc* fb = impl.resolve_frame_asset(
                    crd::containers::StringView(authored->fallback.c_str(), authored->fallback.size()));
                if (fb != nullptr)
                {
                    // reported ONCE per transition, not once per frame — the step-down is state, not an event
                    crd::u64 key = 1469598103934665603ULL;
                    for (const char* p = fb->name.c_str(); *p != '\0'; ++p)
                    {
                        key = (key ^ static_cast<crd::u64>(*p)) * 1099511628211ULL;
                    }
                    if (impl.stepdown_logged != key)
                    {
                        impl.stepdown_logged = key;
                        CRD_LOG_WARN(g_log_scenerender,
                                     "frame '{}' requires '{}' the host lacks — stepped down to '{}'",
                                     authored->name.c_str(), missing->c_str(), fb->name.c_str());
                    }
                    authored = fb;
                }
            }
            else if (missing == nullptr) { impl.stepdown_logged = 0U; }
        }
        if (!impl.frame_ok || !impl.recorder.record(*authored, fg, *impl.raster, host, &ferr, &fwhere))
        {
            // ⛔ REPORTED, never a silent black frame. A graph that fails to record must say which pass, which
            // resource, and why — that is the whole point of the named rejections.
            CRD_LOG_ERROR(g_log_scenerender, "authored frame graph '{}' failed to record: {} (at '{}')",
                          authored->name.c_str(), crd::framecook::frame_exec_error_text(ferr), fwhere.c_str());
            impl.fill_diag_record_ok = 100U + static_cast<crd::u32>(ferr);
            return stats;
        }
        impl.fill_diag_record_ok = 1U;
        impl.fill_diag_pass_count = fg.pass_count();

        // ⛔ THE OVERLAY IS NOT A RENDERING TECHNIQUE. The grid, gizmos and editor chrome are an APPLICATION
        // callback — still a PASS IN THIS GRAPH (one submission, ordered, barriered). ⭐⭐ REN-39: it is now
        // WOVEN IN by the recorder (SceneHost::overlay_pass) directly after the last geometry pass, onto the
        // SCENE image — appending it here put it AFTER a frame's post chain, where it depth-tested the output's
        // never-written depth buffer and escaped the display transform (the sandbox's "weird gizmo").
        // ⛔ Only the OWNER builds and executes. A contributor that built here would submit a partial frame and
        // reset the graph out from under the viewports that had not recorded yet.
        const bool built_ok = owns_graph ? fg.build() : false;
        impl.fill_diag_build_ok = built_ok ? 1U : 0U;
        // ⛔⛔ A GRAPH THAT FAILS TO BUILD RENDERS NOTHING AND USED TO SAY NOTHING. `record()` has named
        // rejections for every way an ASSET can be wrong, but the DEVICE build — the topological sort, the
        // transient allocations, the barrier schedule — returned a bare `false` that nobody reported. The frame
        // then showed the previous contents of the canvas: a plausible picture, missing exactly the passes that
        // mattered, with a clean log. That cost a long hunt through the cull path for a failure that was one
        // layer below it.
        if (owns_graph && !built_ok)
        {
            CRD_LOG_ERROR(g_log_scenerender, "frame graph '{}' failed to BUILD ({} passes) — nothing was drawn",
                          authored != nullptr ? authored->name.c_str() : "<programmatic>", fg.pass_count());
        }
        if (owns_graph && built_ok)
        {
            fg.execute();
            // REN-8: what the DEVICE spent. Compared against `cpu_ms` below, the gap is the frame's stall.
            stats.gpu_ms       = fg.gpu_ms_total();
            stats.timed_passes = fg.pass_count();
        }
    }
    else // synchronous fallback (a backend without the frame graph): the GEO-8 first-clears-rest-load loop
    {
        for (crd::usize i = 0; i < draw_list.size(); ++i)
        {
            record_one_group(*impl.raster, target, draw_list[i], clear, i == 0U, nullptr);
        }
    }
    return stats;
}

// ── frustum helpers ────────────────────────────────────────────────────────────────────────────────────────────────

void frustum_planes(const crd::math::Mat4f& view_proj, crd::math::Vec4f out_planes[6])
{
    // Gribb–Hartmann on the column-major matrix: row_i[j] = m.c{j}[i]. Clip volume: |x|≤w, |y|≤w, 0≤z≤w.
    const auto row = [&](int i) -> crd::math::Vec4f {
        const crd::f32* m = reinterpret_cast<const crd::f32*>(&view_proj);
        return {m[0 * 4 + i], m[1 * 4 + i], m[2 * 4 + i], m[3 * 4 + i]};
    };
    const crd::math::Vec4f r0 = row(0);
    const crd::math::Vec4f r1 = row(1);
    const crd::math::Vec4f r2 = row(2);
    const crd::math::Vec4f r3 = row(3);
    const auto add4 = [](const crd::math::Vec4f& a, const crd::math::Vec4f& b) {
        return crd::math::Vec4f{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    };
    const auto sub4 = [](const crd::math::Vec4f& a, const crd::math::Vec4f& b) {
        return crd::math::Vec4f{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
    };
    out_planes[0] = add4(r3, r0); // left:   x ≥ −w
    out_planes[1] = sub4(r3, r0); // right:  x ≤ +w
    out_planes[2] = add4(r3, r1); // bottom
    out_planes[3] = sub4(r3, r1); // top
    out_planes[4] = r2;           // near:   z ≥ 0 (the [0,1] clip convention — reverse-Z far plane in practice)
    out_planes[5] = sub4(r3, r2); // far:    z ≤ w
}

bool aabb_in_frustum(const crd::geometry::primitives::AABB3<crd::f32>& box, const crd::math::Vec4f planes[6]) noexcept
{
    for (int p = 0; p < 6; ++p)
    {
        const crd::math::Vec4f& pl = planes[p];
        // the positive vertex: the corner farthest along the plane normal
        const crd::f32 px = pl.x >= 0.0F ? box.max.x : box.min.x;
        const crd::f32 py = pl.y >= 0.0F ? box.max.y : box.min.y;
        const crd::f32 pz = pl.z >= 0.0F ? box.max.z : box.min.z;
        if (pl.x * px + pl.y * py + pl.z * pz + pl.w < 0.0F) { return false; }
    }
    return true;
}

crd::geometry::primitives::AABB3<crd::f32> frustum_aabb(const crd::math::Mat4f& view_proj)
{
    const crd::math::Mat4f inv = crd::math::inverse(view_proj);
    crd::geometry::primitives::AABB3<crd::f32> box;
    bool first = true;
    for (int zi = 0; zi < 2; ++zi)
    {
        for (int yi = 0; yi < 2; ++yi)
        {
            for (int xi = 0; xi < 2; ++xi)
            {
                const crd::math::Vec4f clip{xi != 0 ? 1.0F : -1.0F, yi != 0 ? 1.0F : -1.0F,
                                            zi != 0 ? 1.0F : 0.0F, 1.0F};
                const crd::math::Vec4f h = inv * clip;
                if (h.w == 0.0F || !std::isfinite(h.w)) // degenerate → an everything-box (prunes nothing — safe)
                {
                    constexpr crd::f32 big = 1.0e18F;
                    return {{-big, -big, -big}, {big, big, big}};
                }
                const crd::math::Vec3f w{h.x / h.w, h.y / h.w, h.z / h.w};
                if (first)
                {
                    box.min = w;
                    box.max = w;
                    first   = false;
                }
                else
                {
                    box.min.x = w.x < box.min.x ? w.x : box.min.x;
                    box.min.y = w.y < box.min.y ? w.y : box.min.y;
                    box.min.z = w.z < box.min.z ? w.z : box.min.z;
                    box.max.x = w.x > box.max.x ? w.x : box.max.x;
                    box.max.y = w.y > box.max.y ? w.y : box.max.y;
                    box.max.z = w.z > box.max.z ? w.z : box.max.z;
                }
            }
        }
    }
    return box;
}

// ── ⭐ REN-38 audit: the BUILT-IN AUTHORED PACK, exposed (see the header note). ─────────────────────────────
} // namespace crd::scenerender
