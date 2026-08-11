#pragma once

// frame_asset.hpp — REN-36.1 (D-007 row 139): THE AUTHORABLE FRAME GRAPH, as an asset.
//
// A `.frame.toml` describes a whole RENDERING ARCHITECTURE — which passes exist, what buffers they read and
// write, what formats those buffers have, which draw lists feed them — and cooks to a `.crdr` the runtime turns
// back into `IFrameGraph` calls. Authoring text, runtime binary (PRINCIPLES). Spec:
// `docs/design/ren-36-authorable-frame-graph.md`.
//
// ⛔ THE API-AGNOSTICISM CONTRACT. Everything in this header is an ENGINE concept. There is deliberately no way
// to spell a backend concept: formats are `crd::gpu::FgImageFormat`, comparisons are `crd::gpu::DepthCompare` —
// never VkFormat, never DXGI_FORMAT. One cooked asset must load unmodified on Vulkan AND DX12 and produce
// BIT-IDENTICAL output; a backend-specific escape hatch here would be a DESIGN FAILURE, not a feature. Content
// that must differ per platform differs by declared CAPABILITY TIER (`requires`/`fallback`), never by API.
//
// ⛔ WHAT IS **NOT** HERE, ON PURPOSE. This describes TOPOLOGY and PARAMETERS. It cannot describe LOGIC: there
// are no expressions, no control flow, no scripting (ADR-0081 — C++ only, no DSL). What a pass computes
// per-pixel is CKIR; the MECHANIC of a pass ("bind these attachments and iterate a draw list") is a REGISTERED
// pass EXECUTOR named by a stable `crd::renderpass::ExecutorTypeId` (RAF-12.3 — the old central `FramePassKind`
// enum is retired). That three-way split is what makes "anyone can invent a rendering technique" true without
// inventing a language: a new technique = a CKIR shader + a graph node, and a new MECHANIC = a registered
// executor — none of them an engine-enum edit, all of them assets.

#include <crd/gpu/frame_graph.hpp>   // FgImageFormat — the API-neutral format enum the asset reuses
#include <crd/gpu/raster_context.hpp> // DepthCompare — likewise
#include <crd/renderpass/executor_registry.hpp> // RAF-12.3: ExecutorTypeId — a pass' cooked MECHANIC

#include <crd/containers/array.hpp>
#include <crd/containers/hash.hpp>   // RAF-12.3: fnv1a_64 — the constexpr executor-id constants below
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::framecook
{

inline constexpr crd::u32 kFrameSchemaVersion = 1U;

// ── RAF-12.3: THE PASS MECHANIC IS A REGISTERED EXECUTOR ID, not a central enum. ───────────────────────────────
// The old `enum class FramePassKind` is retired (mission §7 deletion list; §22 condition 10 — "pass kinds no
// longer grow through a central engine enum for app extension"). A pass' cooked mechanic is a stable
// `crd::renderpass::ExecutorTypeId` (a hash of the executor name): one of the engine built-ins below, or an app's
// own id for a `kind = "custom"` pass. A NEW mechanic is a REGISTERED executor (`SceneRenderer::register_pass_
// executor`), never an engine-enum edit. Adding one is a deliberate engine change WHEN it is a built-in — with the
// same gate a new enum value used to carry — but the extension point for applications is the registry, not this file.
//
// The handful of variants ONE executor id cannot spell ride as explicit role BITS on the pass (below), because
// they select the same executor and differ only in how it binds: a DEPTH-ONLY vs. an MRT scene-raster pass, a
// COMPOSITING (load+blend) vs. a plain fullscreen pass, an INDIRECT vs. a direct compute dispatch. Everything else
// the id distinguishes outright (a copy is `transfer.copy`, a mesh pass is `mesh.raster`, …).
namespace detail
{
// constexpr FNV-1a of an executor name — the SAME algorithm + constants as `crd::containers::fnv1a_64` (which
// `crd::renderpass::executor_type_id` runs), so a cook-time id equals a record-time id (gated in test_frame_asset).
// Spelled inline over `char` rather than calling `fnv1a_64` because the latter's `void*`→`u8*` cast is not a
// constant expression — a real MSVC constexpr rejection, not a style choice.
template <crd::usize N>
[[nodiscard]] constexpr crd::renderpass::ExecutorTypeId exec_id(const char (&s)[N]) noexcept
{
    crd::u64 h = 0xcbf29ce484222325ULL; // FNV-1a 64-bit offset basis
    for (crd::usize i = 0; i < N - 1U; ++i)
    {
        h ^= static_cast<crd::u64>(static_cast<crd::u8>(s[i]));
        h *= 0x00000100000001B3ULL; // FNV-1a 64-bit prime
    }
    return crd::renderpass::ExecutorTypeId{h};
}
} // namespace detail

inline constexpr crd::renderpass::ExecutorTypeId kExecSceneRaster      = detail::exec_id("scene.raster");
inline constexpr crd::renderpass::ExecutorTypeId kExecFullscreenRaster = detail::exec_id("fullscreen.raster");
inline constexpr crd::renderpass::ExecutorTypeId kExecComputeDispatch  = detail::exec_id("compute.dispatch");
inline constexpr crd::renderpass::ExecutorTypeId kExecTransferClear    = detail::exec_id("transfer.clear");
inline constexpr crd::renderpass::ExecutorTypeId kExecTransferCopy     = detail::exec_id("transfer.copy");
inline constexpr crd::renderpass::ExecutorTypeId kExecTransferBlit     = detail::exec_id("transfer.blit");
inline constexpr crd::renderpass::ExecutorTypeId kExecTransferResolve  = detail::exec_id("transfer.resolve");
inline constexpr crd::renderpass::ExecutorTypeId kExecRaytraceDispatch = detail::exec_id("raytrace.dispatch");
inline constexpr crd::renderpass::ExecutorTypeId kExecRaytracePipeline = detail::exec_id("raytrace.pipeline");
inline constexpr crd::renderpass::ExecutorTypeId kExecTessRaster       = detail::exec_id("tess.raster");
inline constexpr crd::renderpass::ExecutorTypeId kExecMeshRaster       = detail::exec_id("mesh.raster");
inline constexpr crd::renderpass::ExecutorTypeId kExecMeshIndirect     = detail::exec_id("mesh.indirect");
inline constexpr crd::renderpass::ExecutorTypeId kExecVisbufferRaster  = detail::exec_id("visbuffer.raster");
inline constexpr crd::renderpass::ExecutorTypeId kExecPresent          = detail::exec_id("present");

// REN-38-A6: how `kind = "blit"` filters while it rescales.
// ⛔ Nearest is not a performance option, it is a CORRECTNESS one: interpolating two ids in a visibility or
// object-id buffer yields a THIRD id that names nothing.
enum class FrameBlitFilter : crd::u8 { Nearest = 0, Linear };

// Which cooked material variant a geometry pass draws with. Mirrors `crd::kir::cook::PassType`, which ALREADY
// exists and already routes one authored surface to per-pass fragment programs — the asset names it by string.
enum class FrameMaterialPass : crd::u8
{
    None = 0,
    Shadow,
    DepthPrepass,
    GBuffer,
    Forward,
};

// ⭐ REN-38-A14: which QUEUE a pass asks for. ⛔ `Async` is a REQUEST, not a guarantee: an adapter with one
// queue family runs it on the graphics queue and the graph SAYS SO through its counters, because silently
// honouring it would make a perf claim the hardware never delivered.
enum class FrameQueue : crd::u8 { Graphics = 0, Async };

enum class FrameCullMode : crd::u8 { None = 0, Frustum, FrustumOcclusion };
enum class FrameSortMode : crd::u8 { None = 0, FrontToBack, BackToFront, Material };

// A graph-owned resource. Transients are lifetime-analysed + memory-aliased by the graph exactly as today; the
// asset never expresses a lifetime or a barrier, because both are DERIVED from the pass reads/writes.
// ⭐ REN-38-B3 / B4: what a declared resource IS.
// ⛔ `IndirectArgs` is a distinct kind from `TransientBuffer` even though both are buffers, because the BACKING
// DIFFERS: an indirect-args buffer must carry the INDIRECT usage flag (Vulkan) and be movable to
// INDIRECT_ARGUMENT state (D3D12). Neither can be added after the fact — a buffer created without them simply
// cannot be a draw or dispatch source, so "which kind" is a creation-time decision the ASSET must state.
// ⛔ `AccelerationStructure` is EXTERNAL, like `@output`: the graph never creates one. Building a BLAS/TLAS
// needs the scene's geometry, which lives in the World, which `crd-frame-cook` must never depend on. The asset
// names it; the HOST resolves the name to a built structure.
enum class FrameResourceKind : crd::u8
{
    TransientImage = 0,
    TransientBuffer,
    IndirectArgs,          // REN-38-B3: a buffer a pass writes and a LATER pass consumes as draw/dispatch args
    // ⭐ REN-38-B3: a storage buffer the HOST owns and the graph merely IMPORTS. The scene's vertex, instance and
    // material buffers are host-owned by nature, and so is anything whose value must survive the frame — a
    // transient's memory is aliased away the moment its last reader finishes. ⛔ Without this the asset could
    // only ever name buffers that die inside the frame, so no authored compute or ray-tracing pass could produce
    // a result anything outside the graph could read.
    ExternalBuffer,
    AccelerationStructure, // REN-38-B4: a BLAS/TLAS the host owns — named here, never created here
    // ── ⭐ REN-38-B1: RESOURCES WHOSE VALUE IS THEIR HISTORY. ──
    // A transient's memory is aliased and retired the instant its last reader finishes — correct for a G-buffer,
    // fatal for anything TEMPORAL. TAA history, SSR/DDGI/ReSTIR reuse, auto-exposure and the REN-37.9 cached
    // thumbnail all need the frame N-1 contents to still be there in frame N.
    PersistentImage,
    // ⛔ PING-PONG needs TWO images, and the read/write direction is what distinguishes them — so the asset needs
    // NO new syntax for it: **a READ of a ping-pong resource resolves to the PREVIOUS frame's image and a WRITE
    // resolves to THIS frame's**, and the pair swaps every frame. That IS what "history buffer" means, and it
    // removes the classic one-frame-stale bug at the source: an author cannot hold the parity bit wrong because
    // they never hold it. A `$prev`/`$curr` syntax would have given them the chance.
    PingPongImage,
    // ── ⭐ REN-38-B3 (the half I closed this row without): STRUCTURED and COUNTER buffers. ──
    // ⛔ A STRUCTURED buffer is a stride and a count, not a byte size. It matters on DX12, where a UAV carries
    // `StructureByteStride` and a mismatched stride reads every element at the wrong offset — an off-by-N that
    // grows with the index, so element 0 looks right and element 1000 is nonsense. `size_bytes` alone cannot say
    // it, and hard-coding 4 (as the UAV path did) makes every buffer an array of u32 whatever the shader thinks.
    StructuredBuffer,
    // ⛔ A COUNTER buffer is a structured buffer whose first 4 bytes are an ATOMIC COUNTER the graph ZEROES every
    // frame. The zeroing is the whole point — see `IRasterContext::fill_buffer`.
    CounterBuffer,
    // ⭐ REN-38-B5: an APP-OWNED texture the graph only ever READS — a UI atlas, a video frame, a captured HDR, a
    // baked LUT. ⛔ External like `@output` and the acceleration structure, and for the same reason: its contents
    // and its update schedule belong to the application, not to the frame.
    ExternalTexture,
};

struct FrameResourceDesc
{
    crd::containers::String name;
    FrameResourceKind       kind   = FrameResourceKind::TransientImage;
    crd::gpu::FgImageFormat format = crd::gpu::FgImageFormat::RGBA8Unorm;
    crd::u32                width  = 0U; // absolute; 0 with scale > 0 ⇒ relative to the output target
    crd::u32                height = 0U;
    float                   scale  = 0.0F;
    crd::u32                layers = 1U; // > 1 ⇒ an array (CSM atlas, cube faces)
    crd::u32                samples = 1U;
    bool                    sampled = false;
    bool                    storage = false;
    // ── ⭐ REN-38-B2: SHAPE. Appended at the END. ──
    crd::gpu::FgImageKind   kind_2d = crd::gpu::FgImageKind::Tex2D; // `dimension = "2d"|"3d"|"cube"|"cube_array"`
    crd::u32                depth   = 1U;  // 3d only
    // ⭐⭐ 38-G1: does this COLOUR transient carry a DEPTH ATTACHMENT? A geometry pass drawing into an
    // intermediate target (the scene buffer a post chain reads) needs one exactly as much as the swapchain
    // canvas does — without it the pass is not depth-tested and the frame renders with broken occlusion,
    // which is a WRONG image rather than a missing one. ⛔ Not the same as `depth` above (the 3-D slice
    // count); a render target's depth buffer is a different question and gets a different word.
    bool                    depth_buffer = false;
    crd::u32                mips    = 1U;  // the chain length; 0 ⇒ "full chain" is REJECTED, never guessed
    // ── ⭐ REN-38-B3: a STRUCTURED buffer's shape. `size_bytes` is derived (stride × count) when both are given,
    // so an author states elements — which is what they actually have — rather than doing the multiplication.
    // ⭐ REN-38-B6: PIN this transient out of the aliaser. ⛔ The graph aliases by DECLARED lifetime; when a pass
    // touches a resource it did not declare (a debug overlay, a tool capture) the derivation is right and the
    // reality is not, and the symptom is another transient's pixels inside this one. An author who knows that
    // needs a way to say so — the alternative is turning aliasing off globally, trading the whole memory win.
    bool                    no_alias = false;
    // ── ⭐⭐⭐ REN-41: a PERSISTENT/PING-PONG image that FOLLOWS THE OUTPUT (sized by `scale`, recreated on
    // resize). Normally a persistent image demands an absolute size, because a scale-relative extent would
    // silently discard the history when the window resizes. `resizable = true` is the author OPTING IN to exactly
    // that — the right trade for a TAA HISTORY buffer, whose one lost frame on a resize reconverges invisibly.
    // With it, `scale` alone is legal and the runtime sizes the image from the output every build; the device's
    // `create_persistent_image` already destroys+recreates on a desc-size change, so resize just works.
    bool                    resizable = false;
    crd::u32                stride  = 0U;
    crd::u32                count   = 0U;
    crd::u32                size_bytes = 0U; // buffers only

    explicit FrameResourceDesc(crd::memory::IAllocator* a) : name(a) {}
};

// A DRAW LIST is an ECS QUERY the graph declares itself (user-locked 2026-07-25: the flexible option, not a fixed
// set of engine-provided view names). Reuses crd-scene's archetype matching — no second query engine.
struct FrameDrawListDesc
{
    crd::containers::String                          name;
    crd::containers::Array<crd::containers::String>  all;   // must have ALL of these components
    crd::containers::Array<crd::containers::String>  any;   // must have AT LEAST ONE (empty ⇒ ignored)
    crd::containers::Array<crd::containers::String>  none;  // must have NONE of these
    FrameCullMode                                    cull = FrameCullMode::Frustum;
    FrameSortMode                                    sort = FrameSortMode::None;
    crd::u32                                         limit = 0U; // 0 = unlimited

    explicit FrameDrawListDesc(crd::memory::IAllocator* a) : name(a), all(a), any(a), none(a) {}
};

// A reference to a resource, optionally subscripted by the `for_each` index (`shadow_atlas[$index]`).
struct FrameResourceRef
{
    crd::containers::String name;
    bool                    indexed = false; // true ⇒ the `[$index]` slice of a layered resource

    explicit FrameResourceRef(crd::memory::IAllocator* a) : name(a) {}
};

// A typed pass parameter — the ONE representation of a pass' executor-specific config (RAF-12.3 §7 fold: every
// single-purpose `FramePassDesc` field became a named typed param, so the runtime reads a payload, not a giant
// struct). Never an expression — if a graph needs arithmetic, that belongs in CKIR (the line that keeps a DSL out).
// ⛔ Append new types at the END (the blob stores `type` as a byte). `String` is the program / draw-list / view /
// technique reference the host resolves; `Enum`/`U32` carry the former enum fields (blend, depth-compare, filter…).
enum class FrameParamType : crd::u8 { Float = 0, Int, Bool, Vec4, Enum, U32, String };

struct FrameParam
{
    crd::containers::String name;
    FrameParamType          type = FrameParamType::Float;
    double                  v[4] = {0.0, 0.0, 0.0, 0.0};  // Float/Int/Bool/Enum/U32 use v[0]; Vec4 uses v[0..3]
    crd::containers::String str;                          // String type only (a program / query / resource name)

    explicit FrameParam(crd::memory::IAllocator* a) : name(a), str(a) {}
};

// `for_each` — one pass declaration, N instantiations, each with its own view and resource slice (user-locked:
// the flexible option). EXPANDED AT BUILD TIME into ordinary passes, so lifetime analysis, barriers and aliasing
// need no special cases.
enum class FrameForEach : crd::u8
{
    None = 0,
    LightCascades,      // "light.N.cascades"  — CSM (REN-3.2)
    StereoViews,        // "views.stereo"      — VR
    CubeFaces,          // "cube.faces"        — env prefilter (REN-3.5), point-light shadows
    ShadowCastingLights // "lights.shadow_casting"
};

// ── ⭐⭐⭐ RAF-12.3 §7 FOLD: a pass is COMMON GRAPH METADATA + a TYPED NAMED-PARAMETER PAYLOAD (mission §8). ──────────
// The old ~40 single-purpose fields are GONE. A pass carries only: its name; its MECHANIC (`executor_id`, plus the
// `executor` string that holds a custom pass' app id); its resource `reads`/`writes`; `for_each` graph-expansion
// metadata; a queue preference; and the `params` bag — every executor-specific value as a NAMED TYPED param,
// validated against the executor's schema at cook. Read config with `pass_str`/`pass_f32`/`pass_flag`/`pass_u32`/
// `pass_vec4`; write it with `set_pass_*`. The canonical param names (one home) are the `kPassParam*` constants below.
struct FramePassDesc
{
    crd::containers::String                       name;
    crd::renderpass::ExecutorTypeId               executor_id = kExecSceneRaster; // the MECHANIC (a kExec* id / app id)
    crd::containers::String                       executor;   // a CUSTOM pass' app id (round-trip); empty for builtins
    crd::containers::Array<FrameResourceRef>      reads;
    crd::containers::Array<FrameResourceRef>      writes;
    // graph-expansion metadata (NOT executor-specific): `for_each` expands one declaration into N passes at build.
    FrameForEach                                  for_each     = FrameForEach::None;
    crd::u32                                      for_each_arg = 0U;
    FrameQueue                                    queue        = FrameQueue::Graphics; // QueuePreference (mission §8)
    // ⭐ EVERY executor-specific value (shader/kernel/draw_list/view/technique, the six RT programs, clear/blend/
    // depth-compare/material-pass, VRS/conservative/filter, the decomposed sampler + render-state, load/load_depth/
    // depth_as_float/untracked_storage/shared_depth, and the within-executor role bits) is a named typed param here.
    crd::containers::Array<FrameParam>            params;

    explicit FramePassDesc(crd::memory::IAllocator* a) : name(a), executor(a), reads(a), writes(a), params(a) {}
};

// ── The canonical folded-param NAMES — ONE home (parser, cooker, emitter, validator, runtime all use these). ──────
namespace pp
{
inline constexpr const char* kShader        = "shader";
inline constexpr const char* kKernel        = "kernel";
inline constexpr const char* kDrawList      = "draw_list";
inline constexpr const char* kView          = "view";
inline constexpr const char* kTechnique     = "technique";
inline constexpr const char* kRaygen        = "raygen";
inline constexpr const char* kMiss          = "miss";
inline constexpr const char* kClosestHit    = "closest_hit";
inline constexpr const char* kAnyHit        = "any_hit";
inline constexpr const char* kIntersection  = "intersection";
inline constexpr const char* kCallable      = "callable";
inline constexpr const char* kSharedDepth   = "shared_depth";
inline constexpr const char* kDepthOnly     = "depth_only";
inline constexpr const char* kMrt           = "mrt";
inline constexpr const char* kComposite     = "composite";
inline constexpr const char* kIndirect      = "indirect";
inline constexpr const char* kClearColor    = "clear_color";
inline constexpr const char* kClearDepth    = "clear_depth";
inline constexpr const char* kDepthCompare  = "depth_compare";
inline constexpr const char* kMaterialPass  = "material_pass";
inline constexpr const char* kBlendCount    = "blend_count";
inline constexpr const char* kShadingRate   = "shading_rate";
inline constexpr const char* kRateCombiner  = "rate_combiner";
inline constexpr const char* kConservative  = "conservative";
inline constexpr const char* kFilter        = "filter";
inline constexpr const char* kLoad          = "load";
inline constexpr const char* kLoadDepth     = "load_depth";
inline constexpr const char* kDepthAsFloat  = "depth_as_float";
inline constexpr const char* kUntracked     = "untracked_storage";
inline constexpr const char* kHasSampler    = "has_sampler";
inline constexpr const char* kSamplerMin    = "sampler_min_filter";
inline constexpr const char* kSamplerMag    = "sampler_mag_filter";
inline constexpr const char* kSamplerMip    = "sampler_mip_filter";
inline constexpr const char* kSamplerAddr   = "sampler_address";
inline constexpr const char* kSamplerCompare = "sampler_compare";
inline constexpr const char* kSamplerAniso  = "sampler_anisotropy";
inline constexpr const char* kSamplerBias   = "sampler_mip_bias";
inline constexpr const char* kDepthWriteOff = "depth_write_off";
inline constexpr const char* kDepthBias     = "depth_bias";
inline constexpr const char* kDepthBiasSlope = "depth_bias_slope";
inline constexpr const char* kDepthBiasClamp = "depth_bias_clamp";
inline constexpr const char* kFaceCull      = "face_cull";
inline constexpr const char* kFrontFace     = "front_face";
inline constexpr const char* kStencil       = "stencil";
inline constexpr const char* kStencilCompare = "stencil_compare";
inline constexpr const char* kStencilRef    = "stencil_ref";
inline constexpr const char* kStencilReadMask = "stencil_read_mask";
inline constexpr const char* kStencilWriteMask = "stencil_write_mask";
inline constexpr const char* kStencilFail   = "stencil_fail";
inline constexpr const char* kStencilDepthFail = "stencil_depth_fail";
inline constexpr const char* kStencilPass   = "stencil_pass";
inline constexpr const char* kBlendSlot[4]  = {"blend0", "blend1", "blend2", "blend3"};
} // namespace pp

// ── RAF-12.3 §7 FOLD: a pass' executor-specific config is a NAMED TYPED PARAM BAG — no single-purpose struct
// fields. Typed READ accessors (return the default when the param is absent) + WRITE setters (a setter that would
// write an empty string / a default is a no-op, so round-trip only emits what was authored). `find_pass_param` is
// the raw lookup. These are the ONE way the parser, cooker, emitter, validator and runtime touch pass config. ──────
[[nodiscard]] const FrameParam* find_pass_param(const FramePassDesc& p, crd::containers::StringView name) noexcept;
[[nodiscard]] bool pass_has(const FramePassDesc& p, crd::containers::StringView name) noexcept;
[[nodiscard]] crd::containers::StringView pass_str(const FramePassDesc& p, crd::containers::StringView name) noexcept;
[[nodiscard]] float pass_f32(const FramePassDesc& p, crd::containers::StringView name, float def) noexcept;
[[nodiscard]] crd::u32 pass_u32(const FramePassDesc& p, crd::containers::StringView name, crd::u32 def) noexcept;
[[nodiscard]] bool pass_flag(const FramePassDesc& p, crd::containers::StringView name) noexcept; // bool param, absent ⇒ false
bool pass_vec4(const FramePassDesc& p, crd::containers::StringView name, float out[4]) noexcept; // fills out[4]; returns present (often discarded — the fill is the point)

void set_pass_str(FramePassDesc& p, crd::containers::StringView name, crd::containers::StringView value);  // no-op if empty
void set_pass_f32(FramePassDesc& p, crd::containers::StringView name, float value);
void set_pass_u32(FramePassDesc& p, crd::containers::StringView name, crd::u32 value);
void set_pass_enum(FramePassDesc& p, crd::containers::StringView name, crd::u32 value);
void set_pass_flag(FramePassDesc& p, crd::containers::StringView name, bool value);                        // adds only if true
void set_pass_vec4(FramePassDesc& p, crd::containers::StringView name, const float v[4]);

// Reconstruct the cohesive sub-payloads from their decomposed params (defaults where absent). Consumers use these
// instead of the old `p.sampler` / `p.state` fields. `pass_flag(p, pp::kHasSampler)` says whether the sampler is set.
[[nodiscard]] crd::gpu::SamplerDesc pass_sampler(const FramePassDesc& p) noexcept;
[[nodiscard]] crd::gpu::PassRasterState pass_state(const FramePassDesc& p) noexcept;

// Is `name` one of the FOLDED-config param names (a `pp::k*`)? Distinguishes engine config from GENUINE authored
// params (`groups_x`, `exposure`, `clear_id`, …). The emitter uses it to avoid double-emitting; the custom-pass
// runtime uses it to forward only authored params into the executor payload. ONE home for the folded-name set.
[[nodiscard]] bool is_folded_pass_param(crd::containers::StringView name) noexcept;

// ── RAF-12.3: pass-mechanic predicates. The ONE place the old `switch (kind)` becomes an executor-id test. ──────
[[nodiscard]] inline bool pass_is_scene_raster(const FramePassDesc& p) noexcept { return p.executor_id == kExecSceneRaster; }
// The plain forward-geometry scene-raster pass (not the depth-only or MRT variant) — the old `RasterGeometry`.
// Reads the role bits from the param bag (RAF-12.3 §7 fold — no struct fields).
[[nodiscard]] inline bool pass_is_raster_geometry(const FramePassDesc& p) noexcept
{
    return pass_is_scene_raster(p) && !pass_flag(p, crd::containers::StringView(pp::kDepthOnly))
           && !pass_flag(p, crd::containers::StringView(pp::kMrt));
}
[[nodiscard]] inline bool pass_is_fullscreen(const FramePassDesc& p) noexcept { return p.executor_id == kExecFullscreenRaster; }
[[nodiscard]] inline bool pass_is_compute(const FramePassDesc& p) noexcept { return p.executor_id == kExecComputeDispatch; }
[[nodiscard]] inline bool pass_is_raytrace_dispatch(const FramePassDesc& p) noexcept { return p.executor_id == kExecRaytraceDispatch; }
[[nodiscard]] inline bool pass_is_raytrace_pipeline(const FramePassDesc& p) noexcept { return p.executor_id == kExecRaytracePipeline; }
[[nodiscard]] inline bool pass_is_tess(const FramePassDesc& p) noexcept { return p.executor_id == kExecTessRaster; }
[[nodiscard]] inline bool pass_is_mesh(const FramePassDesc& p) noexcept { return p.executor_id == kExecMeshRaster; }
[[nodiscard]] inline bool pass_is_mesh_indirect(const FramePassDesc& p) noexcept { return p.executor_id == kExecMeshIndirect; }
[[nodiscard]] inline bool pass_is_visbuffer(const FramePassDesc& p) noexcept { return p.executor_id == kExecVisbufferRaster; }
[[nodiscard]] inline bool pass_is_present(const FramePassDesc& p) noexcept { return p.executor_id == kExecPresent; }
[[nodiscard]] inline bool pass_is_transfer_clear(const FramePassDesc& p) noexcept { return p.executor_id == kExecTransferClear; }
[[nodiscard]] inline bool pass_is_transfer_copy(const FramePassDesc& p) noexcept { return p.executor_id == kExecTransferCopy; }
[[nodiscard]] inline bool pass_is_blit(const FramePassDesc& p) noexcept { return p.executor_id == kExecTransferBlit; }
[[nodiscard]] inline bool pass_is_transfer_resolve(const FramePassDesc& p) noexcept { return p.executor_id == kExecTransferResolve; }
[[nodiscard]] inline bool pass_is_transfer(const FramePassDesc& p) noexcept
{
    return pass_is_transfer_clear(p) || pass_is_transfer_copy(p) || pass_is_blit(p) || pass_is_transfer_resolve(p);
}
// scene/tess/mesh raster — the mechanics that iterate a draw list of geometry (any role).
[[nodiscard]] inline bool pass_draws_geometry(const FramePassDesc& p) noexcept
{
    return pass_is_scene_raster(p) || pass_is_tess(p) || pass_is_mesh(p);
}
// compute.dispatch OR inline raytrace.dispatch — the mechanics that bind a CKIR kernel + a storage set + a grid.
[[nodiscard]] inline bool pass_dispatches_kernel(const FramePassDesc& p) noexcept
{
    return pass_is_compute(p) || pass_is_raytrace_dispatch(p);
}
// Is `id` one of the 14 engine built-in mechanics? (A custom pass' id is not.) Defined in frame_asset.cpp.
[[nodiscard]] bool is_builtin_executor(crd::renderpass::ExecutorTypeId id) noexcept;
// A CUSTOM (app-defined) pass — its executor is not an engine built-in.
[[nodiscard]] inline bool pass_is_custom(const FramePassDesc& p) noexcept { return !is_builtin_executor(p.executor_id); }

// ── RAF-12.3: the ONE authoring-vocabulary table (mission "one home per fact"). Forward: map a `kind = "..."`
// string to a pass' `executor_id` + role bits (false ⇒ an unknown kind, a NAMED cook error; a `custom` kind sets
// an INVALID id — the caller resolves it from the pass' `executor` field). Inverse: the kind string the editor
// round-trip re-emits from `executor_id` + roles. The two are defined together so they cannot drift. ─────────────
[[nodiscard]] bool pass_mechanic_from_kind(crd::containers::StringView kind, FramePassDesc& out) noexcept;
[[nodiscard]] const char* pass_kind_string(const FramePassDesc& p) noexcept;

// ── REN-37.6: SUBGRAPHS — a graph may INCLUDE another by name, with PARAMETER BINDING. ─────────────────────
// Taken from AMD RPS subprograms. Techniques then COMPOSE instead of being copy-pasted, and — the reason this is
// the keystone for REN-37.8 — a VIEWPORT is just an include of a rendering graph with its own output, camera and
// draw scope bound.
//
// ⛔ NAMESPACING IS NOT COSMETIC. `as = "vp.main"` prefixes every resource, draw list and pass the included graph
// declares, so TWO INSTANCES OF THE SAME GRAPH CANNOT COLLIDE. Without it, including `forward_csm` twice would
// have both instances writing the same `shadow_atlas`, which is not an error anywhere — it just renders one
// viewport's shadows into the other.
//
// `bind` rewrites a name INSIDE the included graph to a name in the includer's scope: `{ output = "@output" }`
// makes the subgraph's `@output` resolve to the parent's. Anything not bound stays namespaced and private.
struct FrameBinding
{
    crd::containers::String from; // the name as the INCLUDED graph spells it
    crd::containers::String to;   // the name it resolves to in the INCLUDER's scope

    explicit FrameBinding(crd::memory::IAllocator* a) : from(a), to(a) {}
};

struct FrameIncludeDesc
{
    crd::containers::String                 graph; // the graph to include, by name
    crd::containers::String                 as;    // the instance namespace (required — see above)
    crd::containers::Array<FrameBinding>    bind;
    // RPS's warning, adopted: composition needs SCHEDULING ATTRIBUTES, or an injected pass can be scheduled into
    // the middle of a scope that must stay contiguous. An `atomic` include refuses to have anything spliced into
    // it (a same-named anchor inside it is not addressable from outside).
    bool                                    atomic = false;

    explicit FrameIncludeDesc(crd::memory::IAllocator* a) : graph(a), as(a), bind(a) {}
};

// ── REN-37.6: INJECTION POINTS — insert a pass BETWEEN existing nodes without forking the base graph. ──────
// Unity URP's idea, and the one that makes the system genuinely EXTENSIBLE rather than merely editable: a user
// should not have to fork `forward_csm` to add an outline pass.
//
// ⛔ ANCHORS ARE DECLARED. The base-graph author states WHERE extension is safe; that is the whole difference
// between an extension point and a monkey-patch. An `[[inject]]` naming an anchor nobody declared is rejected by
// name at cook time.
//
// After splicing, the NORMAL dependency sort still runs — so an injected pass that reads something produced later
// still lands correctly, and a cycle it introduces is still rejected by name. The anchor decides where it is
// INSERTED; the declared reads/writes still decide where it EXECUTES.
struct FrameAnchorDesc
{
    crd::containers::String                         name;
    crd::containers::Array<crd::containers::String> after;  // passes this anchor sits after
    crd::containers::Array<crd::containers::String> before; // ...and before

    explicit FrameAnchorDesc(crd::memory::IAllocator* a) : name(a), after(a), before(a) {}
};

struct FrameInjectDesc
{
    crd::containers::String anchor; // the anchor name this pass splices at
    crd::containers::String pass;   // a pass declared in THIS asset

    explicit FrameInjectDesc(crd::memory::IAllocator* a) : anchor(a), pass(a) {}
};

// The whole authored graph.
struct FrameGraphDesc
{
    crd::containers::String                              name;
    crd::u32                                             schema = kFrameSchemaVersion;
    crd::containers::Array<FrameResourceDesc>            resources;
    crd::containers::Array<FrameDrawListDesc>            draw_lists;
    crd::containers::Array<FramePassDesc>                passes;
    crd::containers::Array<crd::containers::String>      requires_caps; // capability tier (REN-35's rule)
    crd::containers::String                              fallback;      // the graph to use when unsupported
    // REN-37.6: composition. All three are EXPANDED AWAY by `flatten_frame_graph` before `build()`, exactly as
    // `for_each` is — so lifetime analysis, barriers and aliasing only ever see ordinary passes and need no
    // special cases at all. That is the same design decision, for the same reason, one level up.
    crd::containers::Array<FrameIncludeDesc>             includes;
    crd::containers::Array<FrameAnchorDesc>              anchors;
    crd::containers::Array<FrameInjectDesc>              injects;

    // ⭐ REN-38-B6: a HARD ceiling on graph-owned transient memory, in BYTES. 0 = unbounded, so every existing
    // asset is unchanged. ⛔ Checked AFTER aliasing (the budget is about what the frame COSTS, not what it would
    // cost without the aliaser) and it FAILS the build rather than warning — the failure it prevents is an
    // allocation that succeeds on the dev machine and OOMs on the target months later.
    crd::u64                                 memory_budget_bytes = 0U;

    explicit FrameGraphDesc(crd::memory::IAllocator* a)
        : name(a), resources(a), draw_lists(a), passes(a), requires_caps(a), fallback(a), includes(a), anchors(a),
          injects(a)
    {
    }
};

// Every way a `.frame.toml` can be REJECTED — by name, at COOK time, never at runtime on a user's machine.
enum class FrameCookError : crd::u8
{
    Ok = 0,
    ParseFailed,          // not valid TOML
    BadSchema,            // missing/unsupported `schema`
    MissingName,
    DuplicateName,        // two resources / draw lists / passes share a name
    UnknownPassKind,
    UnknownFormat,
    UnknownCompare,
    UnknownSort,
    UnknownBlend, // REN-38-A15: a `blend` entry that is not in the closed set
    UnknownCull,
    UnknownMaterialPass,
    UnknownForEach,
    UnknownResource,      // a pass reads/writes something never declared
    ResourceNeverWritten, // a declared resource no pass produces
    DependencyCycle,      // pass A reads what B writes and vice-versa
    MissingShader,        // a fullscreen pass with no shader (or compute with no kernel)
    MissingDrawList,      // a geometry pass with no draw list
    SubscriptOnNonLayered, // `res[$index]` where `res` has layers == 1
    IndexWithoutForEach,  // `[$index]` used by a pass that declares no for_each
    NoOutputPass,         // nothing writes "@output"
    BadResourceSize,      // neither absolute size nor scale given
    // REN-3.2: `layers` outside [1, kFgMaxImageLayers]. Rejected HERE, at cook time, so a cascade atlas that
    // the device could not create can never ship inside a pack — appended at the END of the enum.
    LayersOutOfRange,
    // ── REN-38-B2. Appended at the END of the enum. ──
    UnknownDimension,     // a `dimension` outside the closed set
    CubeNeedsSquare,      // a cube face must be square — the hardware has no other shape for one
    BadMipCount,          // `mips` is 0, or more levels than the extent can halve down to
    VolumeNeedsDepth,     // `dimension = "3d"` with no `depth`
    // ── REN-38-B1. Appended at the END of the enum. ──
    PersistentNeedsSize,  // a persistent/ping-pong image sized only by `scale` — its key must be stable across frames
    PingPongNeedsBothWays, // a ping-pong resource that is only read, or only written — the pair would never rotate
    StructuredNeedsStride, // a structured/counter buffer with no `stride` — its elements have no size
    StrideNotAligned,      // … or a stride that is not a multiple of 4 (both APIs require it)
    ExternalTextureIsReadOnly, // a pass WRITES an external texture — the application owns its contents
    UnknownSamplerFilter,      // a `filter`/`mip_filter` outside the closed set
    UnknownSamplerAddress,     // an `address` outside the closed set
    // ── REN-37.6: COMPOSITION. Appended at the END of the enum (a renumbered error is a silently different
    // rejection in every log and every test). ──
    IncludeMissingName,   // an `[[include]]` with no `graph`, or no `as` namespace (two instances would collide)
    DuplicateInclude,     // two includes share an `as` namespace — same collision, stated up front
    UnknownAnchor,        // an `[[inject]]` names an anchor no graph declares
    InjectUnknownPass,    // an `[[inject]]` names a pass this asset does not declare
    AnchorUnknownPass,    // an `[[anchor]]`'s `after`/`before` names a pass that does not exist
    UnresolvedInclude,    // the included graph could not be resolved by name (flatten only)
    IncludeCycle,         // graph A includes B includes A
    // ── REN-38-A5: the PRESENT pass. Appended at the END of the enum. ──
    PresentNeedsOneRead,  // a `kind = "present"` pass must read EXACTLY ONE resource — the canvas it presents
    PresentWritesNothing, // a present pass declares a write: presenting produces no graph resource, it CONSUMES one
    PresentSourceInternal, // a present pass reads a transient — aliased memory cannot survive to reach a swapchain
    // ── REN-38-A6: the utility passes. Appended at the END of the enum. ──
    TransferNeedsOneRead,  // copy/blit/resolve must read EXACTLY ONE source
    TransferNeedsOneWrite, // … and write EXACTLY ONE destination
    ClearReadsNothing,     // a clear pass declares a read; a clear consumes nothing
    UnknownFilter,         // a `filter` that is not in the closed set
    // ── REN-38-A7 / A8. Appended at the END of the enum. ──
    // ⛔ A tess/mesh pass with neither a draw list nor a count has NOTHING TO DISPATCH. Left to the runtime it
    // would draw zero patches or zero workgroups — a black image with no error, which is the shape this band
    // exists to eliminate. Rejected at cook time, where the author is still holding the file.
    AmplifyNeedsCount,
    // ── REN-38-A9 / A10 / B3 / B4. Appended at the END of the enum. ──
    RayTraceNeedsAccel,    // a `raytrace` pass names no acceleration structure — it would traverse nothing
    IndirectNeedsArgs,     // an indirect pass names no args buffer — the count would come from nowhere
    IndirectArgsNotArgs,   // … or names a resource that is not an `indirect_args` buffer
    AccelIsExternal,       // an acceleration structure was given a size/format — the graph never creates one
    // ── REN-38-A11 / A12. Appended at the END of the enum. ──
    VisbufferNeedsUintTarget, // a visbuffer pass writes a target that is not R32Uint — ids would be float-clobbered
    CompositeNeedsBlend,      // a composite pass declares no blend — it would render opaque and erase the background
    // ── REN-38-A13 / A14. Appended at the END of the enum. ──
    UnknownShadingRate,       // a `shading_rate` outside the closed set
    UnknownRateCombiner,      // a `rate_combiner` outside the closed set
    UnknownConservative,      // a `conservative` outside the closed set
    UnknownQueue,             // a `queue` that is not `graphics` or `async`
    AsyncQueueNeedsCompute,   // a raster pass asked for the async-compute queue — it cannot run there
    // ── REN-38-A16. Appended at the END of the enum. ──
    RtPipelineNeedsThree,     // a `raytrace.pipeline` pass must name raygen + miss + closest_hit
    // ── REN-38 audit (pass-state vocabulary). Appended at the END of the enum. ──
    UnknownFaceCull,          // a `face_cull` outside none/back/front
    UnknownFrontFace,         // a `front_face` outside ccw/cw
    UnknownStencilOp,         // a `stencil_*` op outside the closed eight
    BadStencilValue,          // a stencil ref/mask outside 0..255 — truncation would mark one value, test another
    // REN-38-F11 (appended)
    LoadNeedsGeometry         // `load = true` on a kind without load draw verbs — it would silently clear
};

// A human-readable message for `err`, plus the offending name when there is one.
[[nodiscard]] const char* frame_cook_error_text(FrameCookError err) noexcept;

// Parse + VALIDATE a `.frame.toml` into `out`. `where` receives the offending name on failure (may be null).
[[nodiscard]] FrameCookError parse_frame_toml(crd::containers::StringView toml_text, FrameGraphDesc& out,
                                              crd::containers::String* where = nullptr);

// VALIDATE any description, whatever its provenance — parsed from TOML, read from a cooked blob, or BUILT IN
// MEMORY by a node editor, a test, a C++ script or an agent. Same 19 rejections either way; a graph assembled
// programmatically must not get a weaker contract than one someone typed, or the ergonomic path becomes the
// unsafe path. `parse_frame_toml` simply calls this after parsing.
[[nodiscard]] FrameCookError validate_frame_graph(const FrameGraphDesc& desc,
                                                  crd::containers::String* where = nullptr);

// CEIR-15c-1d: the per-executor CONTRACT verdict for ONE pass (MissingShader/MissingDrawList/LoadNeedsGeometry/Visbuffer/
// Composite/Rt*/RayTrace/Indirect*/Amplify/Transfer*/Clear/Present*/AsyncQueue/UnknownResource/Subscript*/Index*). Extracted
// from validate_frame_graph's pass loop so BOTH the desc validator AND the CEIR validator (validate_ceir_frame) share ONE
// contract implementation — no desync, and 15f can then delete validate_frame_graph. PURE per-pass verdict; the caller owns
// the wrote_output accumulation. `resources` is the graph's resource table (several checks consult it).
[[nodiscard]] FrameCookError pass_contract_diag(const FramePassDesc& p,
                                                crd::containers::ConstSpan<FrameResourceDesc> resources,
                                                crd::containers::String* where = nullptr);

// CEIR-15d-2: the pass-DAG CYCLE check (a Kahn topo-sort failure over the REN-41 authored-order-aware dependency graph),
// EXTRACTED from validate_frame_graph so validate_ceir_frame shares the ONE source (the 15c-1d extract-and-share discipline).
// `desc`-only (the temp arrays use its allocator). Returns DependencyCycle or Ok.
[[nodiscard]] FrameCookError dependency_cycle_diag(const FrameGraphDesc& desc);

// Emit a description back to `.frame.toml` — the EDITOR ROUND-TRIP. A node editor loads a graph, the user drags
// wires, and the result must be writable back to the same authoring format a human reads and diffs. Lossless by
// gate: `parse → emit → parse → cook` produces bytes identical to `parse → cook`, so a save cannot quietly drop
// a field. (That gate is what makes it safe for an editor or an agent to rewrite someone's hand-authored file.)
[[nodiscard]] crd::containers::String emit_frame_toml(const FrameGraphDesc& desc, crd::memory::IAllocator* a);

// ── PROGRAMMATIC construction ────────────────────────────────────────────────────────────────────────────────
// A frame graph does not have to come from a file. `FrameGraphDesc` is a plain description and
// `execute_frame_graph` takes it directly, so a graph can equally be BUILT AT RUNTIME — by the node editor as
// the user drags wires, by a test, by the C++ scripting layer, or by an agent composing a renderer. This builder
// exists so that path is as pleasant as the TOML one and lands in the SAME validator.
//
// ⛔ INDEX-BASED, not reference-based, on purpose: handing out `FramePassDesc&` into a growing Array would
// dangle the moment the next `add_pass` reallocates (the `push_back(container[k])` UAF class this repo has
// already paid for once).
class FrameGraphBuilder
{
public:
    FrameGraphBuilder(crd::memory::IAllocator* alloc, crd::containers::StringView name);

    // Resources. `add_image` takes an absolute size; `add_scaled_image` is relative to the output target.
    crd::u32 add_image(crd::containers::StringView name, crd::gpu::FgImageFormat format, crd::u32 width,
                       crd::u32 height, bool sampled = false, crd::u32 layers = 1U);
    crd::u32 add_scaled_image(crd::containers::StringView name, crd::gpu::FgImageFormat format, float scale,
                              bool sampled = false);

    // Draw lists — the ECS query the graph declares (`all`/`any`/`none` + cull + sort).
    crd::u32 add_draw_list(crd::containers::StringView name);
    void     draw_list_all(crd::u32 list, crd::containers::StringView component);
    void     draw_list_none(crd::u32 list, crd::containers::StringView component);
    void     draw_list_policy(crd::u32 list, FrameCullMode cull, FrameSortMode sort);

    // Passes. `kind` is the same authoring vocabulary as a `.frame.toml` `kind = "..."` ("raster.geometry",
    // "compute", "raster.mesh.indirect", "custom", …) — RAF-12.3 replaced the `FramePassKind` enum argument. A
    // "custom" pass then names its app executor with `pass_executor`.
    crd::u32 add_pass(crd::containers::StringView name, crd::containers::StringView kind);
    void     pass_executor(crd::u32 pass, crd::containers::StringView executor_id); // RAF-10/12.3: a custom pass' app id
    void     pass_reads(crd::u32 pass, crd::containers::StringView resource, bool indexed = false);
    void     pass_writes(crd::u32 pass, crd::containers::StringView resource, bool indexed = false);
    void     pass_shader(crd::u32 pass, crd::containers::StringView id);
    void     pass_kernel(crd::u32 pass, crd::containers::StringView id);
    void     pass_technique(crd::u32 pass, crd::containers::StringView id); // REN-37.2: the lighting technique
    void     pass_draw_list(crd::u32 pass, crd::containers::StringView name);
    void     pass_view(crd::u32 pass, crd::containers::StringView name);
    void     pass_material(crd::u32 pass, FrameMaterialPass mp);
    void     pass_for_each(crd::u32 pass, FrameForEach gen, crd::u32 arg = 0U);
    void     pass_clear_color(crd::u32 pass, float r, float g, float b, float a);
    void     pass_clear_depth(crd::u32 pass, float d);
    void     pass_depth(crd::u32 pass, crd::gpu::DepthCompare cmp);
    void     pass_param(crd::u32 pass, crd::containers::StringView name, double value);

    void requires_capability(crd::containers::StringView cap);
    void fallback_to(crd::containers::StringView graph_name);

    // The built description. Run `validate()` before executing it — the builder does not stop you assembling a
    // graph with a cycle any more than a text editor stops you typing one.
    [[nodiscard]] const FrameGraphDesc& desc() const noexcept { return m_desc; }
    [[nodiscard]] FrameGraphDesc&       desc() noexcept { return m_desc; }
    [[nodiscard]] FrameCookError validate(crd::containers::String* where = nullptr) const
    {
        return validate_frame_graph(m_desc, where);
    }

private:
    FrameGraphDesc           m_desc;
    crd::memory::IAllocator* m_alloc = nullptr;
};

// Serialize a validated description to the cooked CRDR blob. CANONICAL, PACKED, PADDING-FREE and endian-defined —
// a graph cooked by MSVC loads byte-identically under gcc/clang. (The `ckir_serialize` scar: never memcpy a POD
// into an artifact; indeterminate padding makes the bytes a function of stack history.)
[[nodiscard]] crd::containers::Array<crd::u8> cook_frame_graph(const FrameGraphDesc& desc,
                                                               crd::memory::IAllocator* a);

// Read a cooked blob back. Returns false on bad magic / version / truncation — never a partial description.
[[nodiscard]] bool read_frame_graph(crd::containers::ConstSpan<crd::u8> bytes, FrameGraphDesc& out);

// ── REN-37.6: FLATTEN — expand every `[[include]]` and `[[inject]]` into ONE ordinary graph. ────────────────
// The composition analogue of REN-36.3's `for_each` expansion, and the same decision for the same reason: it
// happens BEFORE `build()`, so lifetime analysis, barriers, aliasing and the dependency sort only ever see plain
// passes and need no special cases whatsoever. Every composition feature this adds is therefore free at runtime.
//
// `resolve` maps an included graph's NAME to its description (the built-in pack, an app's mounted assets, a test's
// table). Returning null is `UnresolvedInclude` — a named failure, never a quietly missing subgraph, because a
// viewport that silently contributes no passes looks exactly like a viewport that rendered nothing.
//
// Included names are prefixed with `<as>.` unless `bind` rewrites them; nesting is followed recursively and a
// cycle is rejected as `IncludeCycle`.
using FrameGraphResolveFn = const FrameGraphDesc* (*)(crd::containers::StringView name, void* user);

[[nodiscard]] FrameCookError flatten_frame_graph(const FrameGraphDesc& desc, FrameGraphResolveFn resolve, void* user,
                                                 FrameGraphDesc& out, crd::containers::String* where = nullptr);

} // namespace crd::framecook
