// scene_renderer.cpp — GEO-7 (D-007 row 72): chunk-grain extract → cull → partial upload → vertex-pulling
// instanced submission. See scene_renderer.hpp for the pipeline + data contract.

#include <crd/scenerender/scene_renderer.hpp>

// REN-38-D5: every vertex program this renderer runs is COOKED FROM A `.crdv`.
#include <crd/vertexcook/vertex_asset.hpp>
// REN-38-C4: the scene SURFACE is cooked from an authored `.crdm`, not a C++ builder.
#include <crd/matcook/material_asset.hpp>
// REN-38-E7: the LIGHTING is an authored declaration cooked into the technique body.
#include <crd/lightcook/lighting_asset.hpp>

#include <crd/scenerender/csm.hpp> // REN-3.2-b: stabilized cascade fitting

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

// ⛔ GENERATED FROM `assets/frame/*.frame.toml` — the BUILT-IN PACK, embedded so the engine has a working
// default with no filesystem dependency. `set_frame_graph()` overrides it with any text an app loads, which
// is how "change the cascade count in a file, no rebuild" works. Keep these in sync with the assets.
constexpr const char* kBuiltinForwardCsm = R"CRDFG(
# forward_csm.frame.toml — the ENGINE'S DEFAULT forward renderer with cascaded shadow maps.
#
# ⛔⛔⛔ TOP RULE: WE WILL ONLY USE OUR AUTHORED FRAME GRAPHS. This file IS cascaded shadow mapping — the
# technique lives here, as data, not in C++. Everything a user might want to change about it is in this file:
#
#   * cascade COUNT      -> `layers` on the atlas (and how many the light reports; the pass expands to match)
#   * shadow RESOLUTION  -> `width`/`height` on the atlas
#   * atlas FORMAT       -> `format`
#   * what CASTS shadows -> the `shadow_casters` draw list's component query
#   * what RECEIVES them -> the `visible_geometry` query
#   * pass ORDER         -> derived from the declared reads/writes, so inserting a pass just works
#   * the LIGHTING itself -> `technique` on the forward pass (REN-37.2). Point it at a different `.crdt` and
#                            every material re-shades: `standard_forward` (no shadows), `unlit`, a toon
#                            technique someone authored. THAT is the frame graph reaching the fragment shader.
#
# None of that requires recompiling the engine. That is the whole point of REN-36, and the reason this file
# exists instead of the equivalent C++ in `scene_renderer.cpp`.
#
# ── how it composes ─────────────────────────────────────────────────────────────────────────────────────────
# ONE `[[pass]]` declaration renders ALL cascades: `for_each = "light.0.cascades"` expands it into N ordinary
# passes at build time, each writing `shadow_atlas[$index]` — one array SLICE per cascade. The forward pass then
# READS the whole atlas, and because it reads what the cascade passes write, the graph's dependency sort places
# every cascade FIRST automatically. Declaration order here is for human readability only.
#
# The forward pass samples the atlas through the COMPARISON sampler purely because the resource's format is
# `D32Float` — the asset never names a sampler, so a shadow map cannot be sampled with a filtering sampler by
# mistake.

schema   = 1
name     = "crd://frame/forward_csm"
# ⭐ SHADOWS ON/OFF is a DECLARED CAPABILITY TIER with a named fallback (REN-35’s rule), not an `if` in the
# renderer. A host that cannot (or will not) do shadows steps down to `forward_basic` — which has no atlas, no
# cascade passes and a different technique — and the step-down is REPORTED, never silent.
requires = ["shadows"]
fallback = "crd://frame/forward_basic"

# ── the cascade atlas: one 2D-ARRAY depth image, one slice per cascade ──────────────────────────────────────
[[resource]]
name    = "shadow_atlas"
kind    = "transient_image"
format  = "D32Float"
width   = 2048
height  = 2048
layers  = 4       # cascade count. 1..16; the pass expands to whatever the light actually reports.
sampled = true    # rendered into, then READ by the forward pass

# ── what casts, and what receives ──────────────────────────────────────────────────────────────────────────
[[draw_list]]
name = "shadow_casters"
all  = ["MeshRenderer", "Transform"]
cull = "frustum"          # per-cascade frustum culling is the host's job; the asset states the intent
sort = "front_to_back"    # depth-only passes want early-Z

[[draw_list]]
name = "visible_geometry"
all  = ["MeshRenderer", "Transform"]
cull = "frustum"
sort = "material"         # batch by material to minimise state changes in the forward pass

# ── ONE declaration -> N cascade passes ────────────────────────────────────────────────────────────────────
[[pass]]
name          = "csm_cascade"
kind          = "raster.depth_only"
draw_list     = "shadow_casters"
for_each      = "light.0.cascades"
writes        = ["shadow_atlas[$index]"]
material_pass = "Shadow"   # depth-only: n_out = 0, the surface is never consumed, lowering DCEs all of it
clear_depth   = 1.0
depth         = "LessEqual"

# ── the shadowed forward pass ──────────────────────────────────────────────────────────────────────────────
# `reads` on a geometry pass is what makes it SHADOWED: the executor binds the atlas and, seeing a depth format,
# selects the comparison sampler. No bespoke pass kind, no sampler named in the asset.
[[pass]]
name          = "forward"
kind          = "raster.geometry"
draw_list     = "visible_geometry"
reads         = ["shadow_atlas"]
writes        = ["@output"]
technique     = "forward_csm"   # <- REN-37.2: THE LIGHTING, AS A NAME. Swap it and the whole scene re-shades.
material_pass = "Forward"
clear_color   = [0.09, 0.10, 0.13, 1.0]
clear_depth   = 0.0
depth         = "GreaterEqual"   # reverse-Z
)CRDFG";

constexpr const char* kBuiltinForwardBasic = R"CRDFG(
# forward_basic.frame.toml — the SHADOWS-OFF tier of the engine's default forward renderer.
#
# ⭐ This file is how "shadows off" is expressed: NOT an `if` in the renderer, but a DECLARED CAPABILITY TIER with
# a named fallback (REN-35's rule). `forward_csm.frame.toml` says `requires = ["shadows"]` and
# `fallback = "crd://frame/forward_basic"`; when the host reports it cannot (or will not) do shadows, the executor
# steps down to THIS graph and REPORTS that it did.
#
# The step-down is reported rather than silent for the same reason every fallback in this system is: a graph that
# quietly rendered something plausible would be indistinguishable from the one you asked for.
#
# Note what it does NOT contain: no atlas, no cascade passes, and a `standard_forward` technique instead of
# `forward_csm`. Turning shadows off therefore costs nothing at all — not a pass, not a transient, not a binding —
# rather than running the machinery and multiplying by one.

schema = 1
name   = "crd://frame/forward_basic"

[[draw_list]]
name = "visible_geometry"
all  = ["MeshRenderer", "Transform"]
cull = "frustum"
sort = "material"

[[pass]]
name          = "forward"
kind          = "raster.geometry"
draw_list     = "visible_geometry"
writes        = ["@output"]
technique     = "standard_forward"
material_pass = "Forward"
clear_color   = [0.09, 0.10, 0.13, 1.0]
clear_depth   = 0.0
depth         = "GreaterEqual"   # reverse-Z
)CRDFG";

// ── ⭐⭐ REN-39 fix (the tonemap toggle): the POST-CHAIN frames JOIN THE BUILTIN PACK. ────────────────────────
// They had been DISK-ONLY: without `CRD_ASSETS_DIR` the sandbox's AgX/sRGB switch hit "no such asset", logged
// an error nobody watches, and the frame silently stayed the default — "changing tonemapping does nothing".
// They were also OUTSIDE the drift gate's list, which is exactly how a disk-only default stays invisible. Both
// fixed: embedded here (a disk copy SHADOWS these, like every default) and drift-gated. `scene_hdr` is
// RGBA16F — the second half of the same fix: an RGBA8 intermediate clamped + quantized the scene to LDR
// BEFORE the display transform, so even an installed tonemap barely changed the image.
constexpr const char* kBuiltinForwardCsmAgx = R"CRDFG(
schema = 1
name   = "crd://frame/forward_csm_agx"
requires = ["shadows"]
fallback = "crd://frame/forward_agx"

[[resource]]
name    = "shadow_atlas"
kind    = "transient_image"
format  = "D32Float"
width   = 2048
height  = 2048
layers  = 4
sampled = true

[[resource]]
name    = "scene_hdr"
kind    = "transient_image"
format  = "RGBA16F"
scale   = 1.0
sampled = true
depth_buffer = true

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
name          = "csm_cascade"
kind          = "raster.depth_only"
draw_list     = "shadow_casters"
for_each      = "light.0.cascades"
writes        = ["shadow_atlas[$index]"]
material_pass = "Shadow"
clear_depth   = 1.0
depth         = "LessEqual"

[[pass]]
name          = "forward"
kind          = "raster.geometry"
draw_list     = "visible_geometry"
reads         = ["shadow_atlas"]
writes        = ["scene_hdr"]
technique     = "forward_csm"
material_pass = "Forward"
clear_color   = [0.09, 0.10, 0.13, 1.0]
clear_depth   = 0.0
depth         = "GreaterEqual"

[[pass]]
name   = "post"
kind   = "raster.fullscreen"
reads  = ["scene_hdr"]
writes = ["@output"]
shader = "crd://post/tonemap_agx"
)CRDFG";

constexpr const char* kBuiltinForwardCsmSrgb = R"CRDFG(
schema = 1
name   = "crd://frame/forward_csm_srgb"
requires = ["shadows"]
fallback = "crd://frame/forward_srgb"

[[resource]]
name    = "shadow_atlas"
kind    = "transient_image"
format  = "D32Float"
width   = 2048
height  = 2048
layers  = 4
sampled = true

[[resource]]
name    = "scene_hdr"
kind    = "transient_image"
format  = "RGBA16F"
scale   = 1.0
sampled = true
depth_buffer = true

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
name          = "csm_cascade"
kind          = "raster.depth_only"
draw_list     = "shadow_casters"
for_each      = "light.0.cascades"
writes        = ["shadow_atlas[$index]"]
material_pass = "Shadow"
clear_depth   = 1.0
depth         = "LessEqual"

[[pass]]
name          = "forward"
kind          = "raster.geometry"
draw_list     = "visible_geometry"
reads         = ["shadow_atlas"]
writes        = ["scene_hdr"]
technique     = "forward_csm"
material_pass = "Forward"
clear_color   = [0.09, 0.10, 0.13, 1.0]
clear_depth   = 0.0
depth         = "GreaterEqual"

[[pass]]
name   = "post"
kind   = "raster.fullscreen"
reads  = ["scene_hdr"]
writes = ["@output"]
shader = "crd://post/srgb_only"
)CRDFG";

// The SHADOWS-OFF tiers of the tonemapped pair (REN-35's rule applied to 38-G1's frames): same HDR
// intermediate, same display transform, no atlas and no cascade passes. `forward_csm_agx/srgb` name these as
// their `fallback`, so a shadow-less host steps down instead of failing to record and presenting BLACK.
constexpr const char* kBuiltinForwardAgx = R"CRDFG(
schema = 1
name   = "crd://frame/forward_agx"

[[resource]]
name    = "scene_hdr"
kind    = "transient_image"
format  = "RGBA16F"
scale   = 1.0
sampled = true
depth_buffer = true

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
technique     = "standard_forward"
material_pass = "Forward"
clear_color   = [0.09, 0.10, 0.13, 1.0]
clear_depth   = 0.0
depth         = "GreaterEqual"

[[pass]]
name   = "post"
kind   = "raster.fullscreen"
reads  = ["scene_hdr"]
writes = ["@output"]
shader = "crd://post/tonemap_agx"
)CRDFG";

constexpr const char* kBuiltinForwardSrgb = R"CRDFG(
schema = 1
name   = "crd://frame/forward_srgb"

[[resource]]
name    = "scene_hdr"
kind    = "transient_image"
format  = "RGBA16F"
scale   = 1.0
sampled = true
depth_buffer = true

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
technique     = "standard_forward"
material_pass = "Forward"
clear_color   = [0.09, 0.10, 0.13, 1.0]
clear_depth   = 0.0
depth         = "GreaterEqual"

[[pass]]
name   = "post"
kind   = "raster.fullscreen"
reads  = ["scene_hdr"]
writes = ["@output"]
shader = "crd://post/srgb_only"
)CRDFG";

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
    // ⭐⭐ 38-G1 (user-directed): the DISK-resolVED material text — F15, exactly as the flat material already
    // does. ⛔ Null falls back to the embedded copy, but the resolution MUST be attempted: a shipped
    // `assets/material/scene.crdm` that the renderer silently ignored made "override any default" a half-truth
    // — the files existed, were drift-gated, and did nothing.
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
namespace
{
constexpr const char* kSceneMaterial = R"(
schema = 1
name   = "crd://material/scene"

[[node]]
name   = "tint"
op     = "geomcolor"
inputs = [1]

[[node]]
name   = "tr"
op     = "extract"
inputs = ["tint", 0]

[[node]]
name   = "tg"
op     = "extract"
inputs = ["tint", 1]

[[node]]
name   = "tb"
op     = "extract"
inputs = ["tint", 2]

[[node]]
name   = "alpha"
op     = "extract"
inputs = ["tint", 3]

[[node]]
name   = "base"
op     = "combine3"
inputs = ["tr", "tg", "tb"]

[[node]]
name   = "nrm"
op     = "normal"
inputs = [0]

[[node]]
name   = "metal"
op     = "multiply"
inputs = [0.0, 1.0]

[[node]]
name   = "rough"
op     = "multiply"
inputs = [0.8, 1.0]

[surface]
base_color = "base"
normal     = "nrm"
metallic   = "metal"
roughness  = "rough"
opacity    = "alpha"
)";

// The TEXTURED variant: the same surface with the base-colour map multiplied in. ⭐ It is a different MATERIAL,
// not a different shader — which is the whole REN-37.2 point, now expressible without touching C++. The
// descriptor coordinates (set 0, texture binding 1, sampler binding 2) are ATTRIBUTES of the `sample2d` node:
// a binding index is topology, not a value.
constexpr const char* kSceneMaterialTextured = R"(
schema = 1
name   = "crd://material/scene_textured"

[[node]]
name   = "tint"
op     = "geomcolor"
inputs = [1]

[[node]]
name   = "uv"
op     = "texcoord"
inputs = [3]

[[node]]
name   = "map"
op     = "sample2d"
inputs = ["uv", 0, 1, 2]

[[node]]
name   = "tr"
op     = "extract"
inputs = ["tint", 0]

[[node]]
name   = "tg"
op     = "extract"
inputs = ["tint", 1]

[[node]]
name   = "tb"
op     = "extract"
inputs = ["tint", 2]

[[node]]
name   = "alpha"
op     = "extract"
inputs = ["tint", 3]

[[node]]
name   = "mr"
op     = "extract"
inputs = ["map", 0]

[[node]]
name   = "mg"
op     = "extract"
inputs = ["map", 1]

[[node]]
name   = "mb"
op     = "extract"
inputs = ["map", 2]

[[node]]
name   = "br"
op     = "multiply"
inputs = ["tr", "mr"]

[[node]]
name   = "bg"
op     = "multiply"
inputs = ["tg", "mg"]

[[node]]
name   = "bb"
op     = "multiply"
inputs = ["tb", "mb"]

[[node]]
name   = "base"
op     = "combine3"
inputs = ["br", "bg", "bb"]

[[node]]
name   = "nrm"
op     = "normal"
inputs = [0]

[[node]]
name   = "metal"
op     = "multiply"
inputs = [0.0, 1.0]

[[node]]
name   = "rough"
op     = "multiply"
inputs = [0.8, 1.0]

[surface]
base_color = "base"
normal     = "nrm"
metallic   = "metal"
roughness  = "rough"
opacity    = "alpha"
)";

// ⭐ REN-38-F6: the FLAT material — a CONSTANT surface that reads NO varyings at all. It is the fragment side of
// the advanced-geometry pipelines (tessellation / mesh shading), whose procedural stages emit no attribute
// varyings; an FS with an EMPTY read set satisfies the 38-D4 contract against ANY vertex program trivially.
constexpr const char* kFlatMaterial = R"(
schema = 1
name   = "crd://material/flat"

[[node]]
name   = "base"
op     = "combine3"
inputs = [1.0, 0.35, 0.1]

[[node]]
name   = "one"
op     = "multiply"
inputs = [1.0, 1.0]

[surface]
base_color = "base"
opacity    = "one"
)";
} // namespace

// The `MaterialTemplate` adapter: cook the authored `.crdm` for this variant. ⛔ A cook failure returns a
// NEGATIVE node, never a substitute surface — a material silently replaced by another renders a plausible
// object that is not the one anybody authored.
int scene_build_surface(crd::kir::KGraph& g, int struct_id, const crd::kir::cook::SurfaceInputs& /*in*/, void* user)
{
    auto* c = static_cast<SceneSurfaceCtx*>(user);
    crd::matcook::MaterialDesc desc(crd::memory::default_allocator());
    crd::containers::String    where(crd::memory::default_allocator());
    const char* text = (c != nullptr && c->textured) ? kSceneMaterialTextured : kSceneMaterial;
    if (c != nullptr && c->disk_text != nullptr) { text = c->disk_text->c_str(); }
    if (crd::matcook::parse_material_toml(crd::containers::StringView(text), desc, &where)
        != crd::matcook::MaterialCookError::Ok)
    {
        return -1;
    }
    return crd::matcook::cook_material(desc, g, struct_id);
}

// REN-38-F6: the same adapter for the FLAT material (nothing varies). `user` (F15) optionally carries the
// DISK-resolved text; null falls back to the embedded copy.
int flat_build_surface(crd::kir::KGraph& g, int struct_id, const crd::kir::cook::SurfaceInputs& /*in*/,
                       void* user)
{
    crd::matcook::MaterialDesc desc(crd::memory::default_allocator());
    crd::containers::String    where(crd::memory::default_allocator());
    const char* text = user != nullptr ? static_cast<const crd::containers::String*>(user)->c_str() : kFlatMaterial;
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
            out.push_back(g.texture(0, 4, kir::DType::F32, kir::TexDim::Tex2D, /*arrayed=*/true, /*ms=*/false,
                                    /*shadow=*/true));
            out.push_back(g.sampler(0, 5, /*shadow=*/true));
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
    if (cfg.pass != ck::PassType::Forward || cfg.tech == nullptr)
    {
        // Depth-only / G-buffer: no technique is invoked at all, which is exactly why every opaque material
        // collapses to the SAME shadow program under lowering.
        const tq::Technique none = tq::standard_forward();
        return tq::build_fs_for_pass(tmpl, none, cfg.pass, opts, in, g, fe, ldir, lcol, nullptr, 0, nullptr, 0);
    }

    crd::containers::Array<crd::i32> binds(g.serial_nodes().allocator());
    if (!resolve_scene_bindings(g, *cfg.tech, cfg, binds)) { return false; }
    const crd::f64 option_values[2] = {static_cast<crd::f64>(cfg.cascades), static_cast<crd::f64>(cfg.pcf_taps)};
    return tq::build_fs_for_pass(tmpl, *cfg.tech, cfg.pass, opts, in, g, fe, ldir, lcol, binds.data(),
                                 static_cast<int>(binds.size()), option_values, cfg.tech->n_options);
}

// ── ⭐⭐ REN-38-D5: THE VERTEX PROGRAMS ARE AUTHORED. ─────────────────────────────
// ⛔⛔ `build_scene_vs_shadowed` / `_skinned` / `build_shadow_vs` USED TO LIVE HERE — ~200 lines of C++ with the
// vertex-pull layout compiled in: the header word map as bare integers, the 12-word vertex record, the instance
// record, four-influence linear-blend skinning, and a hardcoded varying set. They are GONE. Each is now a `.crdv`
// cooked by `crd-vertex-cook`, and THE DELETION IS THE PROOF the D band landed: if the asset could not express
// these programs, this code would have had to stay.
//
// ⭐ The declarations are BUILT-IN TEXT for the same reason the technique library and the frame-graph pack are:
// an engine default an app overrides by name, with no file IO on the init path. The shipped copies under
// `assets/vertex/` are the same declarations in editable form.
namespace
{
// The shared prologue — the header word map, the cooked 48-byte vertex record and the 20-word instance record.
// ⭐ The TANGENT at words 8..11 has been in this buffer since the mesh cooker was written and no shader could
// read it; it is declared here so a material that wants a tangent frame needs no engine change.
constexpr const char* kVsPrologue = R"(
schema = 1
name   = "crd://vertex/scene"

[header]
index_count  = 0
index_off    = 2
vertex_off   = 3
instance_off = 4
visible_off  = 5
view_proj    = 6
light_vp     = 32
skin_off     = 25
palette_off  = 26
joint_count  = 27
instance_count = 100

[vertex]
stride = 12

[[attribute]]
name   = "position"
offset = 0
comps  = 3
kind   = "position"

[[attribute]]
name   = "normal"
offset = 3
comps  = 3
kind   = "direction"

[[attribute]]
name   = "uv"
offset = 6
comps  = 2
kind   = "value"

[[attribute]]
name   = "tangent"
offset = 8
comps  = 4
kind   = "direction"

[instance]
stride    = 20
transform = 0

[[instance_attribute]]
name   = "color"
offset = 16
comps  = 4
kind   = "value"
)";

// The varying set EVERY cooked fragment variant reads: normal@0 · tint@1 · worldpos+depth@2 · uv@3.
// ⛔ The tint is SMOOTH, not flat, and that is the CONTRACT, not a preference: the material reads it through
// `geomcolor`, whose StageIn declares Smooth (a MaterialX vertex colour), and mismatched interpolation between
// the two stages is a spec violation that HAPPENS to render here — a per-instance constant interpolates to
// itself — while shading faceted the moment a real per-vertex colour arrives. The live 38-D4 contract check
// (REN-38 audit) is what caught the disagreement; it refuses the pair at init now.
// ⛔⛔ THE SKINNED PATH USED TO EMIT ONLY TWO OF THESE. `build_scene_vs_skinned` wrote locations 0 and 1 and
// stopped, while every fragment program reads 0..3 — so a skinned draw shaded from UNDEFINED interpolants at
// locations 2 and 3 (the world position the specular and shadow terms need, and the uv). It linked, it bound, it
// rendered. 38-D4’s varying contract is exactly the check that finds this, and sharing one declared varying set
// is what makes it impossible to reintroduce.
constexpr const char* kVsVaryings = R"(
[[varying]]
name     = "world_normal"
location = 0
interp   = "smooth"
source   = ["world:normal"]

[[varying]]
name     = "tint"
location = 1
interp   = "smooth"
source   = ["instance:color"]

[[varying]]
name     = "world_pos_depth"
location = 2
interp   = "smooth"
source   = ["world:position", "clip.w"]

[[varying]]
name     = "uv"
location = 3
interp   = "smooth"
source   = ["uv"]
)";

// GEO-8 skinning, DECLARED: four influences of linear blending over a 6-word skin record and a 16-word palette.
// Changing the influence count or moving to dual-quaternion blending is now an edit here, not an engine change.
constexpr const char* kVsSkin = R"(
[skin]
scheme         = "linear_blend"
influences     = 4
stride         = 6
joint_words    = 2
weight_off     = 2
palette_stride = 16
)";

// ── ⭐⭐ REN-38-F6: THE ADVANCED-STAGE DECLARATIONS. ─────────────────────────────────────────────────────────
// Every stage of every advanced pipeline — tessellation, mesh shading, the visibility buffer, GPU culling and
// the ray-tracing hit group — is an AUTHORED `.crdv`, cooked here exactly the way the scene VS is. The F band
// proved the vocabulary; this is the join the F6 audit found missing: nothing had ever CREATED a program from
// these declarations, and the first attempt exposed that the F1/F2 cooks produced device-impossible entries.

// The tessellation control-point VS: a PROCEDURAL corner table (the F7 vocabulary) — four quad corners at
// ±0.6 clip, selected by `@corner`. `draw_tess` binds NO storage, so a pulling VS could never run here.
constexpr const char* kTessCornersVs = R"(
schema   = 1
name     = "crd://vertex/tess_corners"
position = "node:clip"

[expand]
verts_per_instance = 4

[[node]]
name   = "x_hi"
op     = "ifequal"
inputs = ["@corner", 2.0, 0.6, -0.6]

[[node]]
name   = "x"
op     = "ifequal"
inputs = ["@corner", 1.0, 0.6, "x_hi"]

[[node]]
name   = "y"
op     = "ifgreater"
inputs = ["@corner", 1.5, 0.6, -0.6]

[[node]]
name   = "clip"
op     = "combine4"
inputs = ["x", "y", 0.0, 1.0]
)";

// The hull: levels only — its whole job in the B4 model.
constexpr const char* kTessHullVs = R"(
schema = 1
name   = "crd://vertex/tess_hull"
stage  = "tess_control"

[tess]
patch_size = 4
inner      = 8.0
outer      = 8.0
)";

// The domain: the emitter's bilerped patch point, DISPLACED by the authored graph (×1.3 in xy — the proven
// A1d expansion, so a pixel between the base edge and the expanded one proves the domain ran per vertex).
constexpr const char* kTessDomainVs = R"(
schema   = 1
name     = "crd://vertex/tess_domain"
stage    = "tess_eval"
displace = "expand"

[vertex]
stride = 3

[[attribute]]
name   = "position"
offset = 0
comps  = 3
kind   = "position"

[tess]
patch_size = 4
inner      = 8.0
outer      = 8.0

[[node]]
name   = "expand"
op     = "multiply"
inputs = ["@position", [1.3, 1.3, 1.0]]
)";

// The meshlet grid (the F6-corrected procedural mesh contract: vertices == 3 · primitives).
constexpr const char* kMeshletVs = R"(
schema = 1
name   = "crd://vertex/scene_meshlet"
stage  = "mesh"

[mesh]
max_vertices   = 18
max_primitives = 6
workgroup      = 18
)";

// The amplification stage: each task workgroup launches `emit` mesh workgroups.
constexpr const char* kTaskVs = R"(
schema = 1
name   = "crd://vertex/scene_task"
stage  = "task"

[mesh]
workgroup = 32

[task]
emit = 2
)";

// The visibility-buffer VS: a PROCEDURAL fullscreen pair (6 vertices, 2 triangles) — `draw_visbuffer` binds no
// storage either, and each half of the screen carries a distinct primitive id for the FS to write.
constexpr const char* kVisbufferVs = R"(
schema   = 1
name     = "crd://vertex/visbuffer_fullscreen"
position = "node:clip"

[expand]
verts_per_instance = 6

[[node]]
name   = "x_b"
op     = "ifequal"
inputs = ["@corner", 4.0, 1.0, -1.0]

[[node]]
name   = "x_a"
op     = "ifequal"
inputs = ["@corner", 2.0, 1.0, "x_b"]

[[node]]
name   = "x"
op     = "ifequal"
inputs = ["@corner", 1.0, 1.0, "x_a"]

[[node]]
name   = "y_b"
op     = "ifequal"
inputs = ["@corner", 5.0, 1.0, -1.0]

[[node]]
name   = "y_a"
op     = "ifequal"
inputs = ["@corner", 4.0, 1.0, "y_b"]

[[node]]
name   = "y"
op     = "ifequal"
inputs = ["@corner", 2.0, 1.0, "y_a"]

[[node]]
name   = "clip"
op     = "combine4"
inputs = ["x", "y", 0.5, 1.0]
)";

// ── ⭐⭐ 38-G1b: the POST fullscreen VS — the visbuffer pair PLUS a UV varying (locations map the corner to
// [0,1]²), so an authored post graph's `texcoord` reads the screen coordinate. Same 6-vertex expansion.
constexpr const char* kPostFullscreenVs = R"(
schema   = 1
name     = "crd://vertex/post_fullscreen"
position = "node:clip"

[expand]
verts_per_instance = 3

[[node]]
name   = "x"
op     = "ifequal"
inputs = ["@corner", 1.0, 3.0, -1.0]

[[node]]
name   = "y"
op     = "ifequal"
inputs = ["@corner", 2.0, 3.0, -1.0]

[[node]]
name   = "clip"
op     = "combine4"
inputs = ["x", "y", 0.0, 1.0]

[[node]]
name   = "u_half"
op     = "multiply"
inputs = ["x", 0.5]

[[node]]
name   = "u01"
op     = "add"
inputs = ["u_half", 0.5]

[[node]]
name   = "v_half"
op     = "multiply"
inputs = ["y", 0.5]

[[node]]
name   = "v01"
op     = "add"
inputs = ["v_half", 0.5]

[[varying]]
name       = "uv"
location   = 0
interp     = "smooth"
source     = ["node:u01", "node:v01"]
node_comps = [1, 1]
)";

// 38-G1b: the AUTHORED tonemap — sample the pass input, AgX, sRGB-encode. The whole display transform as data.
constexpr const char* kPostTonemapAgx = R"(
schema = 1
name   = "crd://post/tonemap_agx"

[[node]]
name   = "uv"
op     = "texcoord"
inputs = [0]

[[node]]
name   = "scene"
op     = "sample2d"
inputs = ["uv", 0, 1, 2]

[[node]]
name   = "mapped"
op     = "agx"
inputs = ["scene"]

[[node]]
name   = "output"
op     = "srgb_encode"
inputs = ["mapped"]
)";

// 38-G1b: the sRGB-ONLY variant — the gate's EXACTNESS half (the OETF is a spec formula the test recomputes
// independently, which a copied AgX polynomial could not honestly provide).
constexpr const char* kPostSrgbOnly = R"(
schema = 1
name   = "crd://post/srgb_only"

[[node]]
name   = "uv"
op     = "texcoord"
inputs = [0]

[[node]]
name   = "scene"
op     = "sample2d"
inputs = ["uv", 0, 1, 2]

[[node]]
name   = "output"
op     = "srgb_encode"
inputs = ["scene"]
)";

// The ray-tracing declaration — ONE [rt] contract, cooked once per stage (the stage line is prepended).
constexpr const char* kRtBody = R"(
schema = 1
name   = "crd://vertex/scene_rt"

[rt]
payload_words = 2
as_binding    = 0
out_binding   = 1
alpha_cutoff  = 0.35
)";

// ── The SCENE FRAME GRAPHS for the advanced families. Each is the smallest graph that RENDERS (or computes)
// through its family — the F6 join: an authored pass naming an authored program, resolved by the live host.
constexpr const char* kSceneTessGraph = R"(
schema = 1
name   = "crd://frame/scene_tess"

[[pass]]
name        = "displace"
kind        = "raster.tess"
shader      = "crd://scene/tess"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
params      = { patches = 1 }
)";

constexpr const char* kSceneMeshGraph = R"(
schema = 1
name   = "crd://frame/scene_mesh"

[[pass]]
name        = "amplify"
kind        = "raster.mesh"
shader      = "crd://scene/mesh"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
params      = { groups = 2 }
)";

constexpr const char* kSceneVisbufferGraph = R"(
schema = 1
name   = "crd://frame/scene_visbuffer"

[[draw_list]]
name = "visbuffer_geometry"

[[pass]]
name      = "ids"
kind      = "raster.visbuffer"
shader    = "crd://scene/visbuffer"
draw_list = "visbuffer_geometry"
writes    = ["@output"]
params    = { clear_id = 7 }
)";

constexpr const char* kSceneCullGraph = R"(
schema = 1
name   = "crd://frame/scene_cull"

[[resource]]
name = "instances"
kind = "external_buffer"

[[resource]]
name = "cull_flags"
kind = "external_buffer"

[[resource]]
name = "cull_args"
kind = "indirect_args"
size_bytes = 16

[[resource]]
name = "cull_marks"
kind = "external_buffer"

[[pass]]
name   = "cull"
kind   = "compute"
kernel = "crd://scene/cull"
reads  = ["instances"]
writes = ["cull_flags", "cull_args"]
params = { groups_x = 1 }

[[pass]]
name   = "mark"
kind   = "compute.indirect"
kernel = "crd://scene/cull_mark"
reads  = ["cull_args", "instances"]
writes = ["cull_marks"]

[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";

constexpr const char* kSceneRtGraph = R"(
schema = 1
name   = "crd://frame/scene_rt"

[[resource]]
name = "scene_tlas"
kind = "acceleration_structure"

[[resource]]
name = "hits"
kind = "external_buffer"

[[pass]]
name        = "trace"
kind        = "raytrace.pipeline"
raygen      = "crd://scene/rt/raygen"
miss        = "crd://scene/rt/miss"
closest_hit = "crd://scene/rt/chit"
any_hit     = "crd://scene/rt/anyhit"
reads       = ["scene_tlas"]
writes      = ["hits"]
params      = { groups_x = 4, groups_y = 1 }

[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";
} // namespace

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

void hash_bytes(crd::u64& h, const void* data, crd::usize n) noexcept
{
    const auto* p = static_cast<const crd::u8*>(data);
    for (crd::usize i = 0; i < n; ++i)
    {
        h ^= p[i];
        h *= kFnvPrime;
    }
}

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

    crd::u64 structure_sig = 0;
    bool     has_structure = false;

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
    std::unique_ptr<crd::gpu::IStorageBuffer> cull_args;
    crd::u32                                  cull_args_groups = 0U;
    std::unique_ptr<crd::gpu::IGpuProgram> vs_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_idx;
    std::unique_ptr<crd::gpu::IGpuProgram> vs_rebased_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_rebased_idx;
    std::unique_ptr<crd::gpu::IGpuProgram> vs_skinned_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_skinned_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_textured_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_shadowed_idx;
    std::unique_ptr<crd::gpu::IRasterProgram> program_textured_shadowed_idx;
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
        crd::containers::String mat_text(alloc);
        if (cfg.pass == crd::kir::cook::PassType::Forward
            && asset_text(cfg.textured ? "material/scene_textured.crdm" : "material/scene.crdm", mat_text))
        {
            rcfg.material_text = &mat_text;
        }
        crd::kir::KGraph g(alloc);
        crd::kir::KEntry e;
        if (!build_scene_fs_cooked(g, e, rcfg)) { return nullptr; }
        // REN-39-C1: the read-only promise is entry state — it feeds the emitters AND the content hash below,
        // so the indexed pair's FS twin is a distinct deduped program, never a collision with its u0 sibling.
        e.storage_read_only = rcfg.storage_read_only;
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

    crd::kir::technique::TechniqueLibrary  techniques{nullptr};
    const char*                            forward_technique = "standard_forward";
    const char*                            shadow_technique  = "forward_csm";
    crd::u32                               pcf_taps          = 4U; // the `pcf_taps` option value (1|2|4|8|16)

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
    crd::gpu::IGpuProgram*                    kern_cull_reset   = nullptr; // borrowed from adv_stages
    crd::gpu::IGpuProgram*                    kern_rt[4] = {nullptr, nullptr, nullptr, nullptr}; // rg/ms/ch/ah
    crd::gpu::IGpuProgram*                    flat_fs   = nullptr; // borrowed from adv_stages
    // The scene TLAS, provided by whoever owns the geometry's device form (B4: the graph names it, the HOST
    // resolves it — the asset format stays free of engine types).
    crd::gpu::IAccelerationStructure*         scene_accel = nullptr;
    // REN-38: the vertex axis of the VariantKey, folded from the LIVE .crdv (the D5-correction close)
    crd::u32                                  vertex_variant = 0U;
    // Host-resolved external buffers for the compute/RT graphs ("cull_flags", "hits").
    std::unique_ptr<crd::gpu::IStorageBuffer> buf_cull_flags;
    std::unique_ptr<crd::gpu::IStorageBuffer> buf_cull_marks;
    std::unique_ptr<crd::gpu::IStorageBuffer> buf_hits;

    // ── ⭐ REN-38-F15: DISK-FIRST asset resolution. ──
    // When an asset root is installed (`set_asset_root`), a shipped file under it SHADOWS the embedded copy —
    // that is what makes the `assets/` directory live rather than documentation. ⛔ A file that EXISTS is used
    // unconditionally: a corrupt disk copy fails the cook loudly downstream, it never silently falls back to
    // the embedded text (a fallback that renders is indistinguishable from the edit having worked).
    crd::containers::String asset_root; // empty = embedded pack only
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
        return builtin_asset_text(name, out);
    }

    // Cook one authored stage BY ASSET NAME (disk-first) and create its device program.
    [[nodiscard]] crd::gpu::IGpuProgram* cook_stage_named(const char* asset_name)
    {
        if (ctx == nullptr) { return nullptr; }
        crd::containers::String t(alloc);
        if (!asset_text(asset_name, t)) { return nullptr; }
        crd::kir::KGraph g(alloc);
        crd::kir::KEntry e;
        if (!cook_vs(alloc, t.c_str(), nullptr, g, e)) { return nullptr; }
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

    // ⭐⭐ REN-40-A: cook a CULL asset with THIS BACKEND'S indirect-command layout STAMPED onto the parsed desc.
    // ⛔ The asset carries the Vulkan form (20-byte command, args at 0) as its written default; D3D12 needs 24
    // with the args at 4 behind a DrawIndex root constant. Stamping the DESC — never editing the text — is the
    // same discipline the per-cascade shadow variant uses: the variant is the renderer's pass semantics, the
    // vocabulary is the asset's. A kernel that assumed one layout would write garbage commands on the other
    // backend, which is the clip-space-Y failure shape again.
    [[nodiscard]] crd::gpu::IGpuProgram* cook_cull_stage_named(const char* asset_name, crd::u32 view)
    {
        if (ctx == nullptr || raster == nullptr) { return nullptr; }
        crd::containers::String t(alloc);
        if (!asset_text(asset_name, t)) { return nullptr; }
        crd::vertcook::VertexProgramDesc desc(alloc);
        crd::containers::String          where(alloc);
        if (crd::vertcook::parse_vertex_toml(crd::containers::StringView(t.c_str(), t.size()), desc, &where)
            != crd::vertcook::VertexCookError::Ok)
        {
            return nullptr;
        }
        desc.cull.draw_stride  = raster->indirect_command_stride();
        // ⛔ view v's command sits `v * stride` along, so its 5 args start at `arg_off + v * stride`. Baking the
        // VIEW into the offset is what lets one authored asset produce every view's kernel.
        // ⛔ The commands start AFTER the params block (`kCullArgsHeaderWords`), whose word 0 carries the group's
        // buffer base. Every consumer of an args offset adds the same constant: the cook stamp here, the draw
        // items in `fill()`, the cascade expansion, and the counts readback.
        desc.cull.draw_arg_off = (kCullArgsHeaderWords * 4U) + raster->indirect_command_arg_offset()
                                 + view * raster->indirect_command_stride();
        desc.cull.base_word    = 0U; // params word 0
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
        crd::kir::KGraph g(alloc);
        crd::kir::KEntry e;
        if (!crd::vertcook::cook_vertex_program(desc, g, e)) { return nullptr; }
        std::unique_ptr<crd::gpu::IGpuProgram> p = ctx->create_program(g, e);
        if (p == nullptr) { return nullptr; }
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
          palette_staging(a), group_of_mesh(a), material_color(a), material_texture(a), entity_slot(a),
          contrib_draws(a), frame(a), fallback(a), recorder(a), groups_view(a), fs_hashes(a), fs_programs(a),
          fb_frame_names(a), fb_frame_descs(a), techniques(a), adv_stages(a)
    {
        // The built-in pack, parsed once. ⛔ If the EMBEDDED default fails to parse the renderer must not fall
        // back to hand-built passes — there would then be two rendering paths and only one of them authored.
        frame_ok = crd::framecook::parse_frame_toml(crd::containers::StringView(kBuiltinForwardCsm), frame)
                       == crd::framecook::FrameCookError::Ok
                   && crd::framecook::parse_frame_toml(crd::containers::StringView(kBuiltinForwardBasic), fallback)
                          == crd::framecook::FrameCookError::Ok;
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

bool SceneRenderer::init(crd::gpu::IRasterContext& raster, crd::resources::ResourceManager& rm)
{
    m_impl->raster = &raster;
    m_impl->rm     = &rm;
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

// ── ⭐ REN-38-F15: DISK-FIRST asset loading. ────────────────────────────────────────────────────────────────
// A shipped file under `root` SHADOWS the embedded copy for every authored asset this renderer cooks — which is
// what makes editing `assets/` change the frame without a rebuild. The built-in FRAME PAIR was parsed at
// construction, so it re-resolves here; ⛔ a disk copy that exists but fails to parse REFUSES the root (false,
// nothing half-installed) — it never silently falls back to the embedded text.
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
// ── ⭐⭐ REN-38-E7: THE LIGHTING TECHNIQUE IS AN AUTHORED DECLARATION. ─────────────────
// ⛔⛔ Every technique body was a C++ `TechniqueBody` (`body = "builtin:forward_csm"`), so the scene was lit by
// ONE HARDCODED DIRECTIONAL LIGHT no matter what any asset said — while `ckir_lighting.hpp` held 1100 lines of
// punctual/area/IBL/shadow math nothing could reach. This body is `cook_lighting` driven by the declaration
// below: change the light counts, the shadow scheme or the filter HERE and the cooked program changes.
//
// ⛔ The light RECORD is declared against the same group storage buffer everything else pulls from, so lighting
// needs no second binding mechanism — the light section is a header word like every other section.
constexpr const char* kSceneLighting = R"(
schema = 1
name   = "crd://lighting/scene_forward"

[header]
view_proj   = 6
csm_splits  = 28
light_off   = 99

[record]
stride      = 16
position    = 0
color       = 4
direction   = 8
falloff     = 3
spot_scale  = 7
spot_offset = 11

[counts]
directional = 1

[shadow]
directional = "csm"
filter      = "pcf"
taps        = 4
cascades    = 4
)";

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
            disk.size() > 0U ? crd::containers::StringView(disk.c_str(), disk.size())
                             : crd::containers::StringView(kSceneLighting),
            d, &where);
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
    t.n_bindings = 3;
    t.options    = crd::kir::technique::kForwardCsmOptions;
    t.n_options  = 2;
    return t;
}
} // namespace
bool SceneRenderer::init_programs(crd::gpu::IGpuContext& ctx)
{
    if (m_impl->raster == nullptr) { return false; }
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
    if (fwd == nullptr) { return false; }

    // ONE vertex program feeds every fragment variant (normal@0 · tint@1 · worldpos+depth@2 · uv@3). Before
    // REN-37.2 there were three VS variants with three different varying layouts, which is precisely how the
    // textured path and the shadowed path ended up disagreeing about what location 2 meant.
    crd::kir::KGraph vg(m_impl->alloc);
    crd::kir::KEntry ve;
    // ⭐⭐ 38-G1 (user-directed): resolved BY NAME — a shipped `assets/vertex/scene.crdv` shadows the
    // embedded copy, exactly like the frames, materials and post graphs. The builtin pack is the fallback.
    crd::containers::String vs_scene(m_impl->alloc);
    if (!m_impl->asset_text("vertex/scene.crdv", vs_scene)) { return false; }
    crd::vertcook::VertexProgramDesc scene_desc(m_impl->alloc);
    if (!cook_vs(m_impl->alloc, vs_scene.c_str(), nullptr, vg, ve, &scene_desc)) { return false; }
    // ⭐ REN-38 (the D5 correction closed): `VariantKey::vertex` is ENGINE-FILLED from the LIVE declaration —
    // the folded `vertex_layout_id` of the very `.crdv` this renderer just cooked. Until now no engine code
    // filled the field at all, so the variant identity's vertex axis was a documented claim, not a value.
    {
        const crd::u64 lid       = crd::vertcook::vertex_layout_id(scene_desc);
        m_impl->vertex_variant   = static_cast<crd::u32>(lid ^ (lid >> 32U));
    }
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
    if (m_impl->vs == nullptr || fs_flat == nullptr) { return false; }
    if (!contract_ok(scene_desc, static_cast<const crd::vertcook::VaryingRequirement*>(fwd_reqs), n_fwd_reqs))
    {
        { return false; }
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
        char rb_line[64];
        (void)std::snprintf(static_cast<char*>(rb_line), sizeof(rb_line), "rebase_table = %u\n",
                            kSceneDrawTableOff);
        vs_rb.append(static_cast<const char*>(rb_line));
        vs_rb.append(vs_scene.c_str()); // the SAME resolved declaration — the twin can never drift from it
        crd::vertcook::VertexProgramDesc rb_desc(m_impl->alloc);
        if (cook_vs(m_impl->alloc, vs_rb.c_str(), nullptr, rvg, rve, &rb_desc)
            && contract_ok(rb_desc, static_cast<const crd::vertcook::VaryingRequirement*>(fwd_reqs), n_fwd_reqs))
        {
            m_impl->vs_rebased = ctx.create_program(rvg, rve);
            if (m_impl->vs_rebased != nullptr)
            {
                m_impl->program_rebased = m_impl->raster->create_raster_program(*m_impl->vs_rebased, *fs_flat);
            }
        }
    }

    crd::kir::KGraph svg(m_impl->alloc);
    crd::kir::KEntry sve;
    crd::containers::String vs_skin(m_impl->alloc);
    if (!m_impl->asset_text("vertex/scene_skinned.crdv", vs_skin)) { return false; }
    crd::vertcook::VertexProgramDesc skin_desc(m_impl->alloc);
    if (!cook_vs(m_impl->alloc, vs_skin.c_str(), nullptr, svg, sve, &skin_desc)) { return false; }
    // The skinned declaration serves the SAME fragment set — the exact pair the 38-D5 scar broke.
    if (!contract_ok(skin_desc, static_cast<const crd::vertcook::VaryingRequirement*>(fwd_reqs), n_fwd_reqs))
    {
        { return false; }
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
            char rbl[80];
            (void)std::snprintf(static_cast<char*>(rbl), sizeof(rbl), "indexed = true\nrebase_table = %u\n",
                                kSceneDrawTableOff);
            ok = cook_vs_text(static_cast<const char*>(rbl), vs_scene, m_impl->vs_rebased_idx);
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
        }
        if (ok && m_impl->program_textured != nullptr && fs_tex_ro != nullptr)
        {
            m_impl->program_textured_idx = m_impl->raster->create_raster_program(*m_impl->vs_idx, *fs_tex_ro);
            ok = m_impl->program_textured_idx != nullptr;
        }
        if (ok && m_impl->program_shadowed != nullptr && fs_sh_ro != nullptr)
        {
            m_impl->program_shadowed_idx = m_impl->raster->create_raster_program(*m_impl->vs_idx, *fs_sh_ro);
            ok = m_impl->program_shadowed_idx != nullptr;
        }
        if (ok && m_impl->program_textured_shadowed != nullptr && fs_tsh_ro != nullptr)
        {
            m_impl->program_textured_shadowed_idx = m_impl->raster->create_raster_program(*m_impl->vs_idx, *fs_tsh_ro);
            ok = m_impl->program_textured_shadowed_idx != nullptr;
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
            m_impl->program_textured_idx.reset();
            m_impl->program_shadowed_idx.reset();
            m_impl->program_textured_shadowed_idx.reset();
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
    m_impl->use_indexed = on;
}

// ⭐⭐ REN-40-A: the GPU-driven cull switch. ⛔ It requires the INDEXED path — a GPU-written command IS an
// indexed-indirect command, and the classic pull draw has no count field to source from device memory.
void SceneRenderer::set_gpu_cull(bool on) noexcept
{
    m_impl->gpu_cull_on = on;
}

bool SceneRenderer::gpu_cull() const noexcept
{
    return m_impl->gpu_cull_on && m_impl->use_indexed;
}

void SceneRenderer::set_gpu_cull_verify(bool on) noexcept { m_impl->gpu_cull_verify = on; }
bool SceneRenderer::gpu_cull_verify() const noexcept { return m_impl->gpu_cull_verify; }

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
    for (crd::usize gi = 0; gi < impl.draw_groups.size(); ++gi)
    {
        MeshGroup* g = impl.draw_groups[gi];
        if (g == nullptr || g->cull_args == nullptr) { continue; }
        if (!impl.raster->download_storage(*g->cull_args)) { continue; }
        // ⛔ `read_u32` reads the HOST MIRROR — without the download above it answers whatever was last uploaded,
        // which is a confident zero. That mistake made this gate report an empty cull on a cull that worked.
        for (crd::u32 v = 0; v < out.views; ++v)
        {
            const crd::u32 aw = (v * stride_w) + argw;
            out.instances[v] += g->cull_args->read_u32(aw + 1U);
            if (!any)
            {
                out.indices[v]     = g->cull_args->read_u32(aw + 0U);
                out.first_index[v] = g->cull_args->read_u32(aw + 2U);
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
        // the CPU's verdict for the SAME group, this frame — see `GpuCullCounts::cpu_instances`
        out.cpu_instances[0] += g->visible_count_cpu;
        for (crd::u32 c = 0; c < kMaxCascades; ++c) { out.cpu_instances[1U + c] += g->cascade_visible_count[c]; }
        any = true;
        ++out.groups;
    }
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
    SyncStats*                stats = nullptr;

    // pass 1 outputs
    crd::u64 sig       = kFnvOffset;
    bool     any_dirty = false;

    // rebuild-pass scratch: per-(chunk × group) run starts
    crd::containers::Array<crd::u32> run_first; // parallel to groups, valid within one chunk visit

    explicit ExtractCtx(crd::memory::IAllocator* a) : run_first(a) {}
};

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
    crd::containers::Array<crd::f32> tmp(impl.alloc);
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
    group.index_count = static_cast<crd::u32>(mesh->indices.size() / 4U);
    ctx.groups->push_back(static_cast<MeshGroup&&>(group));
    const crd::u32 gi = static_cast<crd::u32>(ctx.groups->size() - 1U);
    ctx.impl->group_of_mesh.insert(mesh_id, gi);
    return static_cast<crd::i64>(gi);
}

// write one instance record + world AABB into a group slot
void write_slot(MeshGroup& group, crd::u32 slot, const crd::scene::Transform& t, const crd::math::Vec4f& color)
{
    InstanceGpu& rec = group.instances[slot];
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

// pass 1: the structure signature + any-dirty detection (no mutation)
void pass_signature(const crd::scene::ChunkView& view, void* ud)
{
    auto& ctx = *static_cast<ExtractCtx*>(ud);
    const auto* renderers = view.array<const crd::scene::MeshRenderer>(ctx.renderer_id);
    if (renderers == nullptr || view.entity_count == 0U) { return; }

    hash_bytes(ctx.sig, &view.entities, sizeof(view.entities)); // the chunk key (entity-array pointer)
    hash_bytes(ctx.sig, view.entities, sizeof(crd::scene::EntityId) * view.entity_count);
    hash_bytes(ctx.sig, renderers, sizeof(crd::scene::MeshRenderer) * view.entity_count);
}

// pass 2a: FULL rebuild — append every instance, group runs per (chunk × group)
void pass_rebuild(const crd::scene::ChunkView& view, void* ud)
{
    auto& ctx = *static_cast<ExtractCtx*>(ud);
    const auto* transforms = view.array<const crd::scene::Transform>(ctx.transform_id);
    const auto* renderers  = view.array<const crd::scene::MeshRenderer>(ctx.renderer_id);
    if (transforms == nullptr || renderers == nullptr || view.entity_count == 0U) { return; }

    // per-chunk: remember each group's size BEFORE this chunk appends (the run start)
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
        group.slot_skeleton.push_back({});
        group.slot_clip.push_back({});
        group.slot_time.push_back(0.0F);
        if (group.material.is_null()) { group.material = renderers[i].material; } // REN-2 Half B: representative material
        write_slot(group, slot, transforms[i], ctx.impl->resolve_color(renderers[i].material));
        ctx.impl->entity_slot.insert(view.entities[i],
                                     (static_cast<crd::u64>(gi) << 32U) | static_cast<crd::u64>(slot));
    }

    // record this chunk's runs
    const crd::u64 tversion = view.version_of(ctx.transform_id);
    for (crd::usize gi = 0; gi < ctx.groups->size(); ++gi)
    {
        const crd::u32 first = ctx.run_first[gi];
        const auto     now   = static_cast<crd::u32>((*ctx.groups)[gi].instances.size());
        if (now > first)
        {
            ChunkRun run;
            run.chunk_key = view.entities;
            run.version   = tversion;
            run.first     = first;
            run.count     = now - first;
            run.dirty     = true;
            (*ctx.groups)[gi].runs.push_back(run);
        }
    }
}

// pass 2b: INCREMENTAL — re-extract only chunks whose Transform version moved (slots unchanged by construction:
// the structure signature matched)
void pass_update(const crd::scene::ChunkView& view, void* ud)
{
    auto& ctx = *static_cast<ExtractCtx*>(ud);
    const auto* transforms = view.array<const crd::scene::Transform>(ctx.transform_id);
    const auto* renderers  = view.array<const crd::scene::MeshRenderer>(ctx.renderer_id);
    if (transforms == nullptr || renderers == nullptr || view.entity_count == 0U) { return; }

    const crd::u64 tversion = view.version_of(ctx.transform_id);

    // is any run of this chunk stale?
    bool stale = false;
    for (crd::usize gi = 0; gi < ctx.groups->size() && !stale; ++gi)
    {
        for (const ChunkRun& run : (*ctx.groups)[gi].runs)
        {
            if (run.chunk_key == view.entities && run.version != tversion)
            {
                stale = true;
                break;
            }
        }
    }
    if (!stale) { return; }

    // rewrite this chunk's slots group by group, walking entities in visit order (the rebuild's order)
    ctx.run_first.clear();
    for (crd::usize gi = 0; gi < ctx.groups->size(); ++gi)
    {
        crd::u32 first = 0U;
        for (ChunkRun& run : (*ctx.groups)[gi].runs)
        {
            if (run.chunk_key == view.entities)
            {
                first       = run.first;
                run.version = tversion;
                run.dirty   = true;
                ++ctx.stats->dirty_runs;
            }
        }
        ctx.run_first.push_back(first);
    }
    for (crd::u32 i = 0; i < view.entity_count; ++i)
    {
        const crd::u32* gi_found = ctx.impl->group_of_mesh.find(renderers[i].mesh);
        if (gi_found == nullptr) { continue; }
        const crd::usize gi   = *gi_found;
        const crd::u32   slot = ctx.run_first[gi]++;
        write_slot((*ctx.groups)[gi], slot, transforms[i], ctx.impl->resolve_color(renderers[i].material));
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
    ctx.stats        = &stats;

    // pass 1: the structure signature
    {
        auto q = world.query<crd::scene::Transform, crd::scene::MeshRenderer>();
        q.for_each_chunk(&pass_signature, &ctx);
    }

    const bool structural = !impl.has_structure || ctx.sig != impl.structure_sig;
    if (structural)
    {
        stats.structural_rebuild = true;
        for (MeshGroup& group : m_groups) // keep buffers/meshes; drop the instance tables
        {
            group.instances.clear();
            group.slot_entity.clear();
            group.world_bounds.clear();
            group.runs.clear();
            group.slot_skeleton.clear();
            group.slot_clip.clear();
            group.slot_time.clear();
        }
        impl.entity_slot.clear();
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
    for (MeshGroup& group : m_groups)
    {
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
            group.indices_off   = kHeaderWords;
            group.vertices_off  = group.indices_off + group.index_count;
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
            group.bounds_off    = group.visible_off + needed_capacity * (1U + kMaxCascades);
            const crd::u32 total_words = group.bounds_off + needed_capacity * 6U;
            group.buffer             = impl.raster->create_storage_buffer(total_words * 4U);
            // ⭐⭐ REN-40-A: and its indirect commands — (1 + cascades) of them at the backend's stride.
                group.cull_args = impl.raster->create_storage_buffer(
                (kCullArgsHeaderWords * 4U)
                + ((1U + kMaxCascades) * impl.raster->indirect_command_stride()));
            group.cull_base_uploaded = 0xFFFFFFFFU; // force the params write on the next frame
            group.capacity           = needed_capacity;
            group.geometry_uploaded  = false;
        }
        if (group.buffer == nullptr) { continue; }

        if (!group.geometry_uploaded || structural)
        {
            if (!group.geometry_uploaded)
            {
                (void)impl.raster->upload_storage(*group.buffer, group.indices_off * 4U, mesh->indices.data(),
                                                  static_cast<crd::u32>(mesh->indices.size()));
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
            for (ChunkRun& run : group.runs) { run.dirty = false; }
            continue;
        }

        // incremental: upload ONLY the dirty runs' byte ranges (the chunk-grain partial re-upload — the gate)
        for (ChunkRun& run : group.runs)
        {
            if (!run.dirty) { continue; }
            run.dirty            = false;
            const crd::u32 bytes = run.count * static_cast<crd::u32>(sizeof(InstanceGpu));
            (void)impl.raster->upload_storage(*group.buffer,
                                              (group.instances_off + run.first * kInstanceWords) * 4U,
                                              group.instances.data() + run.first, bytes);
            stats.uploaded_bytes += bytes;
            upload_bounds_range(impl, group, run.first, run.count, stats); // REN-40-A, same grain
        }
    }

    const double t2 = ms_now();
    stats.upload_ms = t2 - t1;
    // ── GEO-8: the skinned palettes — sample every skinned instance's clip, upload the palette section.
    // Animation is ALWAYS dirty by definition: this is per-frame data, deliberately outside the partial-upload
    // accounting the static gate measures.
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
            if (skel == nullptr) // no skeleton → identity palette (bind pose renders)
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
            else // no clip: the rest pose
            {
                for (crd::u32 j = 0; j < jc; ++j)
                {
                    const crd::f32* r                 = skel->rest.data() + static_cast<crd::usize>(j) * crd::anim::kRestFloats;
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
        (void)impl.raster->upload_storage(*group.buffer, group.palette_off * 4U, impl.palette_staging.data(),
                                          static_cast<crd::u32>(impl.palette_staging.size() * 4U));
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
        // ⭐⭐ 38-G1b: the authored POST programs — the technique library's first device-reachable family
        if (str_is(id, "crd://post/tonemap_agx") || str_is(id, "crd://post/srgb_only"))
        {
            return m_impl.ensure_post_program(id);
        }
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
                out.items[i].args_offset = (kCullArgsHeaderWords * 4U)
                                           + (1U + instance) * m_impl.raster->indirect_command_stride();
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
    [[nodiscard]] crd::gpu::IRasterProgram* instance_program(crd::containers::StringView, crd::u32 index) override
    {
        if (index >= kMaxCascades)
        {
            return nullptr;
        }
        if (m_impl.use_indexed && m_impl.shadow_prog_idx[index] != nullptr)
        {
            return m_impl.shadow_prog_idx[index].get();
        }
        return m_impl.shadow_prog[index].get();
    }

private:
    static bool str_is(crd::containers::StringView a, const char* b)
    {
        crd::usize i = 0;
        while (b[i] != '\0' && i < a.size() && a[i] == b[i]) { ++i; }
        return b[i] == '\0' && i == a.size();
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
            it.indexed      = d.rebased; // REN-38: must record through the multi verb (it pushes the row)
            it.vertex_count = d.vertex_count;
            it.texture      = d.base_color; // beats the pass's sampled read (REN-37.10)
            // REN-39-C1: the indexed-pull fields ride through — index_count > 0 routes the indexed verbs
            it.index_count = d.index_count;
            it.instance_count = d.instance_count;
            it.first_index = d.first_index;
            // ⭐⭐ REN-40-A: under the GPU cull the command lives in DEVICE memory. `args` routes this item to
            // the indirect verb (`instance_count` above is then never read — it is stale by construction), and
            // `dispatch_groups` is what a COMPUTE pass walking this same list uses as its grid.
            // ⛔ Only for INDEXED items: a GPU-written command IS an indexed-indirect command.
            if (m_impl.gpu_cull_on && d.index_count > 0U && d.cull_args != nullptr)
            {
                it.args            = d.cull_args;
                it.args_offset     = (kCullArgsHeaderWords * 4U) + d.cull_args_offset;
                it.dispatch_groups = d.cull_groups;
            }
            out.items[out.resolved++] = it;
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
    if (impl.shadows_active()) { impl.cascades = compute_csm_cascades_from_vp(view_proj, light_dir, impl.csm); }
    // REN-37.3: the frame-frequency camera position, from the SAME exact reconstruction the cascade fit uses.
    const crd::math::Vec3f eye_ws = camera_position_from_vp(view_proj);

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
            total += (group.visible_off + group.capacity * (1U + kMaxCascades) + 3U) & ~3U;
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
                (void)impl.raster->upload_storage(*impl.scene_buf, (group.region_base + group.vertices_off) * 4U,
                                                  mesh->vertices.data(),
                                                  static_cast<crd::u32>(mesh->vertices.size()));
            }
            impl.scene_geom_valid = true;
        }
    }
    crd::u32 draw_table[crd::framecook::kMaxDrawItems] = {};
    bool     wrote_frame_header = false;

    // broad phase: a configured BVH prunes to the frustum's AABB; otherwise every slot is a candidate
    const bool use_bvh = bvh != nullptr && bvh->is_configured();
    crd::containers::Array<crd::scene::EntityId> candidates(impl.alloc);
    if (use_bvh) { bvh->overlap(frustum_aabb(view_proj), candidates); }

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
            for (const crd::scene::EntityId e : candidates)
            {
                const crd::u64* packed = impl.entity_slot.find(e);
                if (packed == nullptr) { continue; }
                const auto gi   = static_cast<crd::u32>(*packed >> 32U);
                const auto slot = static_cast<crd::u32>(*packed & 0xFFFFFFFFU);
                if (&m_groups[gi] != &group) { continue; }
                if (aabb_in_frustum(group.world_bounds[slot], planes)) { group.visible.push_back(slot); }
            }
        }
        else
        {
            for (crd::u32 slot = 0; slot < count; ++slot)
            {
                if (aabb_in_frustum(group.world_bounds[slot], planes)) { group.visible.push_back(slot); }
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
        header[kHdrBoundsOff]        = group.bounds_off; // REN-40-A: the GPU cull's world-AABB section
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
        if (group.cull_args != nullptr && group.cull_base_uploaded != cdst_base)
        {
            const crd::u32 params[kCullArgsHeaderWords] = {cdst_base, 0U, 0U, 0U};
            if (impl.raster->upload_storage(*group.cull_args, 0U, params, sizeof(params)))
            {
                group.cull_base_uploaded = cdst_base;
                stats.uploaded_bytes += sizeof(params);
            }
        }
        if (impl.shadows_active() && cdst != nullptr && (!impl.gpu_cull_on || impl.gpu_cull_verify))
        {
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
                    if (aabb_in_frustum(group.world_bounds[slot], cplanes)) { clist.push_back(slot); }
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
        // (Groups batch by MESH; the group's representative material drives the map — correct for one-material meshes.
        // Per-instance material textures are a bindless follow-up. Skinned takes precedence — no textured-skinned yet.)
        crd::gpu::ITexture*       base_color = group.skinned ? nullptr : impl.resolve_base_color_texture(group.material);
        crd::gpu::IRasterProgram* program    = impl.program.get();
        if (group.skinned && impl.program_skinned != nullptr) { program = impl.program_skinned.get(); }
        // ⭐⭐ REN-38: shadows and albedo COMPOSE now. The atlas moved to its own bindings (4/5), so a textured
        // group under active shadows takes the COMBINED program and keeps BOTH — the interim either/or that made
        // textured monuments lose their maps the instant shadows turned on is gone. Skinned still keeps its own
        // program (a shadowed-skinned variant rides the REN-3.3 material work).
        const bool want_shadow = impl.shadows_active() && !group.skinned;
        if (want_shadow && base_color != nullptr && impl.program_textured_shadowed != nullptr)
        {
            program = impl.program_textured_shadowed.get(); // textured AND shadowed — the whole point
        }
        else if (want_shadow && base_color == nullptr && impl.program_shadowed != nullptr)
        {
            program = impl.program_shadowed.get();
        }
        else if (base_color != nullptr && impl.program_textured != nullptr) { program = impl.program_textured.get(); }
        else { base_color = nullptr; } // no textured program available ⇒ the flat path (drop the map)
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
            if (draw_list.size() < crd::framecook::kMaxDrawItems)
            {
                draw_table[draw_list.size()] = group.region_base;
            }
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
                // mirror the pull path's fallback (a skinned group without its program draws flat) so EVERY
                // item stays indexed — a cascade pass may never receive a mixed-mode list
                iprog = impl.program_skinned_idx != nullptr ? impl.program_skinned_idx.get() : impl.program_idx.get();
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
            }
        }
        draw_list.push_back(d);
        // REN-36.3-b: mirror this group's entities alongside the draw so the asset's component filter has
        // something to test. Index-parallel with `draw_list` BY CONSTRUCTION — culled groups are skipped in both.
        {
            crd::containers::Array<crd::scene::EntityId> ents(impl.alloc);
            for (crd::usize k = 0; k < group.slot_entity.size(); ++k) { ents.push_back(group.slot_entity[k]); }
            impl.groups_view.push_back(static_cast<crd::containers::Array<crd::scene::EntityId>&&>(ents));
            impl.draw_groups.push_back(&group); // 38-G1: index-parallel, for the per-cascade counts
        }
        ++stats.draws;
        stats.drawn_instances += visible_count;
    }

    if (consolidate && impl.scene_buf != nullptr && wrote_frame_header)
    {
        // one canonical table write per frame — row i belongs to draw-list item i, the multi verb's contract
        (void)impl.raster->upload_storage(*impl.scene_buf, kSceneDrawTableOff * 4U, draw_table,
                                          sizeof(draw_table));
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
            return stats;
        }

        // ⛔ THE OVERLAY IS NOT A RENDERING TECHNIQUE. The grid, gizmos and editor chrome are an APPLICATION
        // callback — still a PASS IN THIS GRAPH (one submission, ordered, barriered). ⭐⭐ REN-39: it is now
        // WOVEN IN by the recorder (SceneHost::overlay_pass) directly after the last geometry pass, onto the
        // SCENE image — appending it here put it AFTER a frame's post chain, where it depth-tested the output's
        // never-written depth buffer and escaped the display transform (the sandbox's "weird gizmo").
        // ⛔ Only the OWNER builds and executes. A contributor that built here would submit a partial frame and
        // reset the graph out from under the viewports that had not recorded yet.
        const bool built_ok = owns_graph ? fg.build() : false;
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
bool builtin_asset_text(const char* name, crd::containers::String& out)
{
    const crd::containers::StringView n(name);
    out.clear();
    const auto is = [&](const char* k) { return n == crd::containers::StringView(k); };
    if (is("frame/forward_csm.frame.toml")) { out.append(kBuiltinForwardCsm); return true; }
    if (is("frame/forward_basic.frame.toml")) { out.append(kBuiltinForwardBasic); return true; }
    // REN-39 fix: the post-chain frames are DEFAULTS, so they live in the pack (disk copies shadow them)
    if (is("frame/forward_csm_agx.frame.toml")) { out.append(kBuiltinForwardCsmAgx); return true; }
    if (is("frame/forward_csm_srgb.frame.toml")) { out.append(kBuiltinForwardCsmSrgb); return true; }
    if (is("frame/forward_agx.frame.toml")) { out.append(kBuiltinForwardAgx); return true; }
    if (is("frame/forward_srgb.frame.toml")) { out.append(kBuiltinForwardSrgb); return true; }
    if (is("material/scene.crdm")) { out.append(kSceneMaterial); return true; }
    if (is("material/scene_textured.crdm")) { out.append(kSceneMaterialTextured); return true; }
    if (is("vertex/scene.crdv"))
    {
        out.append(kVsPrologue);
        out.append(kVsVaryings);
        return true;
    }
    if (is("vertex/scene_skinned.crdv"))
    {
        out.append(kVsPrologue);
        out.append(kVsSkin);
        out.append(kVsVaryings);
        return true;
    }
    if (is("vertex/shadow.crdv"))
    {
        // The live renderer bakes one variant PER CASCADE; cascade 0 is the canonical shipped copy.
        out.append("transform = \"light_vp\"\ncascade = 0\n");
        out.append(kVsPrologue);
        return true;
    }
    if (is("lighting/scene_forward.crdl")) { out.append(kSceneLighting); return true; }
    // ── REN-38-F6: the advanced-stage declarations + the scene graphs for their families. ──
    if (is("material/flat.crdm")) { out.append(kFlatMaterial); return true; }
    // 38-G1: the POST family — the fullscreen VS + the two shipped display transforms
    if (is("vertex/post_fullscreen.crdv")) { out.append(kPostFullscreenVs); return true; }
    if (is("post/tonemap_agx.crdp")) { out.append(kPostTonemapAgx); return true; }
    if (is("post/srgb_only.crdp")) { out.append(kPostSrgbOnly); return true; }
    if (is("vertex/tess_corners.crdv")) { out.append(kTessCornersVs); return true; }
    if (is("vertex/tess_hull.crdv")) { out.append(kTessHullVs); return true; }
    if (is("vertex/tess_domain.crdv")) { out.append(kTessDomainVs); return true; }
    if (is("vertex/scene_meshlet.crdv")) { out.append(kMeshletVs); return true; }
    if (is("vertex/scene_task.crdv")) { out.append(kTaskVs); return true; }
    if (is("vertex/visbuffer_fullscreen.crdv")) { out.append(kVisbufferVs); return true; }
    if (is("vertex/scene_cull.crdv"))
    {
        out.append("stage = \"cull\"\n");
        out.append(kVsPrologue);
        out.append("\n[cull]\nfrustum   = true\nworkgroup = 64\n");
        return true;
    }
    // the GPU-driven chain's MARK kernel: the passthrough cull variant at workgroup 1, dispatched INDIRECTLY
    if (is("vertex/scene_cull_mark.crdv"))
    {
        out.append("stage = \"cull\"\n");
        out.append(kVsPrologue);
        out.append("\n[cull]\nfrustum   = false\nworkgroup = 1\n");
        return true;
    }
    if (is("vertex/scene_rt_raygen.crdv")) { out.append("stage = \"raygen\"\n"); out.append(kRtBody); return true; }
    if (is("vertex/scene_rt_miss.crdv")) { out.append("stage = \"miss\"\n"); out.append(kRtBody); return true; }
    if (is("vertex/scene_rt_closest_hit.crdv"))
    {
        out.append("stage = \"closest_hit\"\n");
        out.append(kRtBody);
        return true;
    }
    if (is("vertex/scene_rt_any_hit.crdv")) { out.append("stage = \"any_hit\"\n"); out.append(kRtBody); return true; }
    if (is("frame/scene_tess.frame.toml")) { out.append(kSceneTessGraph); return true; }
    if (is("frame/scene_mesh.frame.toml")) { out.append(kSceneMeshGraph); return true; }
    if (is("frame/scene_visbuffer.frame.toml")) { out.append(kSceneVisbufferGraph); return true; }
    if (is("frame/scene_cull.frame.toml")) { out.append(kSceneCullGraph); return true; }
    if (is("frame/scene_rt.frame.toml")) { out.append(kSceneRtGraph); return true; }
    return false;
}

} // namespace crd::scenerender
