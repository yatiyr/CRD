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
// per-pixel is CKIR; the MECHANIC of a pass ("bind these attachments and iterate a draw list") is a C++
// `FramePassKind`. That three-way split is what makes "anyone can invent a rendering technique" true without
// inventing a language: a new technique = a CKIR shader + a graph node, and both are assets.

#include <crd/gpu/frame_graph.hpp>   // FgImageFormat — the API-neutral format enum the asset reuses
#include <crd/gpu/raster_context.hpp> // DepthCompare — likewise

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::framecook
{

inline constexpr crd::u32 kFrameSchemaVersion = 1U;

// The MECHANIC of a pass — the C++/data boundary. Adding a kind is a deliberate engine change with its own gate,
// never an escape hatch for expressing arbitrary logic in data.
enum class FramePassKind : crd::u8
{
    RasterGeometry = 0,  // iterate a draw list into colour(+depth) attachments
    RasterDepthOnly,     // iterate a draw list into DEPTH only (REN-3.1 — built)
    RasterFullscreen,    // one triangle over the target, sampling declared inputs
    RasterMrt,           // geometry into N colour attachments (deferred G-buffer)
    Compute,             // dispatch a CKIR kernel over a declared grid
    Present,             // hand the target to the swapchain
    // ── ⭐ REN-38-A6: the UTILITY passes every real graph needs and none could express. Appended at the END
    // of the enum (a renumbered kind silently reclassifies every already-cooked graph). ──
    // ⛔ Before this row the ONLY way to move pixels was a fullscreen pass with a pass-through shader, paying
    // for a rasterizer, a descriptor set and a pipeline to do what the copy engine does for free — and an MSAA
    // resolve was not expressible AT ALL, because copying a multisampled image is illegal, not merely slow.
    Clear,               // set a target to a constant colour, no draw
    Copy,                // exact 1:1 image copy (same extent + format)
    Blit,                // RESCALING copy, filtered — the half-res downsample every post chain starts with
    Resolve,             // collapse an MSAA image to one sample
    // ── ⭐ REN-38-A7 / A8: the two GEOMETRY-AMPLIFICATION pass kinds. Appended at the END of the enum. ──
    // Both existed as DEVICE verbs (38-A1c, 38-A1d) and neither was reachable from an asset, so the entire
    // amplification half of the hardware — tessellation AND mesh/task shaders — could only be driven by C++.
    // That is precisely what the top rule forbids: a technique must be a `.frame.toml`, not a call site.
    RasterTess,          // VS→TCS→TES→FS over PATCHES — the portable displacement path (no mesh-shader HW needed)
    RasterMesh,          // TASK→MESH→FS — the amplification path; the mesh shader generates geometry, no vertex input
    // ── ⭐ REN-38-A9 / A10. Appended at the END of the enum. ──
    // ⛔ `RayTrace` is an INLINE RAY QUERY dispatch, not a ray-tracing PIPELINE. The distinction is real and is
    // stated here rather than discovered: inline ray query (VK_KHR_ray_query / DXR-1.1 `RayQuery<>`) is an
    // ordinary compute dispatch with a TLAS descriptor, so it records into the frame's ONE submission like every
    // other pass. A ray-tracing PIPELINE needs a shader binding table and `vkCmdTraceRays` / `DispatchRays` —
    // that is `RayTracePipeline` below (38-A16, both backends; the DX12 state-object half was written for it).
    // ── ⭐ REN-38-A11 / A12. Appended at the END of the enum. ──
    // ⛔ `RasterVisbuffer` is not `RasterGeometry` with a different format: an R32_UINT target clears with a UINT
    // clear value, and writing the id through a FLOAT clear reinterprets its BIT PATTERN (id 1 becomes 1.4e-45),
    // so every pixel reads "background" while the draw looks perfectly healthy. The kind carries that.
    RasterVisbuffer,     // HW-raster primitive ids into an R32_UINT target — the HW half of the Nanite split
    // ⛔ `RasterComposite` is `RasterFullscreen` that LOADS and BLENDS instead of clearing. WBOIT's resolve is by
    // definition `rgb·(1-reveal) + background·reveal` — it must read what is already in the target. Every
    // fullscreen kind before this cleared, so the background was gone before the composite ran.
    RasterComposite,     // fullscreen, N bindless reads, LOAD + BLEND over what the target already holds
    RayTrace,            // an inline-ray-query kernel against a declared acceleration structure
    // ⭐ REN-38-A16: the ray-tracing PIPELINE. ⛔ A separate kind from `RayTrace`, not a flag on it, because the
    // two take DIFFERENT INPUTS: an inline query names ONE kernel, while a pipeline names THREE programs
    // (raygen · miss · closest-hit) that the traversal hardware selects between through a shader binding table.
    // That is what buys per-geometry hit shaders — different materials answering the same ray differently —
    // which an inline query cannot express: it has one shader and must branch on everything itself.
    RayTracePipeline,
    ComputeIndirect,     // a kernel whose WORKGROUP COUNT a buffer holds — the GPU decides how much work follows
    RasterMeshIndirect,  // a meshlet dispatch whose count a buffer holds — the Nanite-style cull→draw loop
};

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
    crd::u32                mips    = 1U;  // the chain length; 0 ⇒ "full chain" is REJECTED, never guessed
    // ── ⭐ REN-38-B3: a STRUCTURED buffer's shape. `size_bytes` is derived (stride × count) when both are given,
    // so an author states elements — which is what they actually have — rather than doing the multiplication.
    // ⭐ REN-38-B6: PIN this transient out of the aliaser. ⛔ The graph aliases by DECLARED lifetime; when a pass
    // touches a resource it did not declare (a debug overlay, a tool capture) the derivation is right and the
    // reality is not, and the symptom is another transient's pixels inside this one. An author who knows that
    // needs a way to say so — the alternative is turning aliasing off globally, trading the whole memory win.
    bool                    no_alias = false;
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

// A typed pass parameter. Scalars/vectors/booleans ONLY — never an expression. If a graph needs arithmetic, that
// arithmetic belongs in CKIR (this is the line that keeps a DSL out; see the spec's risk #1).
enum class FrameParamType : crd::u8 { Float = 0, Int, Bool, Vec4 };

struct FrameParam
{
    crd::containers::String name;
    FrameParamType          type = FrameParamType::Float;
    double                  v[4] = {0.0, 0.0, 0.0, 0.0};

    explicit FrameParam(crd::memory::IAllocator* a) : name(a) {}
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

struct FramePassDesc
{
    crd::containers::String                       name;
    FramePassKind                                 kind = FramePassKind::RasterGeometry;
    crd::containers::Array<FrameResourceRef>      reads;
    crd::containers::Array<FrameResourceRef>      writes;
    crd::containers::String                       draw_list; // geometry/depth-only kinds
    crd::containers::String                       view;      // "camera.main", "light.0.cascade[$index]"
    crd::containers::String                       shader;    // fullscreen kinds — a cooked CKIR program id
    crd::containers::String                       kernel;    // compute kind — a cooked CKIR kernel id
    // ⭐ REN-38-A16: the three programs a ray-tracing PIPELINE is built from. Named separately rather than as a
    // list because their ROLES are fixed and positional: swapping miss and closest-hit in a list would build a
    // pipeline that traces correctly and shades every hit with the miss shader.
    crd::containers::String                       raygen;
    crd::containers::String                       miss;
    crd::containers::String                       closest_hit;
    // ⭐ REN-38 audit (the full hit group): the OPTIONAL any-hit — what alpha-tested geometry needs in RT. The
    // traversal calls it per candidate and it may IGNORE the hit, so a chain-link fence shadows as a fence
    // rather than as a solid plate. Empty = no any-hit stage (the historical pipeline, byte-unchanged).
    crd::containers::String                       any_hit;
    // ⭐ REN-38-F13: the LAST two SBT roles. `intersection` makes the hit group PROCEDURAL (the BLAS AABBs only
    // bound the shape — the authored math IS the geometry); `callable` joins the SBT's fourth table. Both
    // optional; a NAMED one that does not resolve FAILS (the F12 any-hit rule).
    crd::containers::String                       intersection;
    crd::containers::String                       callable;
    // REN-37.2: the LIGHTING TECHNIQUE this pass shades with (a `.crdt` name — "standard_forward",
    // "forward_csm", "toon"). Empty ⇒ the engine's default. This is the field that makes the top rule reach the
    // FRAGMENT SHADER: swapping a technique is an asset edit, and the technique's declared PASS-frequency
    // bindings are verified against this pass's `reads` at cook time (`verify_technique_bindings`).
    crd::containers::String                       technique;
    FrameMaterialPass                             material_pass = FrameMaterialPass::None;
    FrameForEach                                  for_each      = FrameForEach::None;
    crd::u32                                      for_each_arg  = 0U; // e.g. the light index in light.N.cascades
    bool                                          has_clear_color = false;
    float                                         clear_color[4]  = {0.0F, 0.0F, 0.0F, 1.0F};
    bool                                          has_clear_depth = false;
    float                                         clear_depth     = 1.0F; // crd-lint-allow-untagged-physical: NDC depth in [0,1] (a normalized-device coordinate, not a length)
    crd::gpu::DepthCompare                        depth = crd::gpu::DepthCompare::LessEqual;
    // REN-38-A15: PER-ATTACHMENT BLEND, one entry per declared `writes` (missing entries default to Opaque).
    // ⛔ A pass could declare N attachments (38-A1b) and N reads (38-A3) but not how they BLEND, so every pass
    // rendered OPAQUE. WBOIT needs TWO DIFFERENT EQUATIONS ON TWO ATTACHMENTS OF ONE PASS — accumulation additive,
    // revealage multiplicative — which is exactly what made it un-authorable and forced `draw_wboit` to allocate
    // its own images. Additive particles, decals and premultiplied UI need it too.
    crd::containers::Array<crd::gpu::BlendMode>   blend;
    // ── ⭐ REN-38-A13: PER-PASS RENDER STATE. ──
    // ⛔ These are ATTRIBUTES, not pass KINDS, and that is a deliberate design call rather than a shortcut. A
    // shading rate is orthogonal to WHAT a pass draws — a geometry pass, a fullscreen pass and a mesh pass can
    // each want one — so making "VRS" its own kind would have forced a combinatorial `raster.geometry.vrs`,
    // `raster.fullscreen.vrs`, `raster.mesh.vrs` … and every future kind would have to be doubled again.
    // Absent ⇒ the hardware default (1×1, no conservative raster), which is what every existing asset means.
    crd::gpu::ShadingRate                         shading_rate = crd::gpu::ShadingRate::Rate1x1;
    crd::gpu::ShadingRateCombiner                 rate_combiner = crd::gpu::ShadingRateCombiner::Keep;
    crd::gpu::ConservativeMode                    conservative = crd::gpu::ConservativeMode::Off;
    FrameQueue                                    queue        = FrameQueue::Graphics; // REN-38-A14
    // ── ⭐ REN-38-B8: HOW this pass SAMPLES its reads. ──
    // ⛔ Per PASS, not per binding, and that is a deliberate limit rather than an oversight: a pass samples its
    // inputs one way (a post-process clamps, a material tiles), and per-binding samplers would mean the asset
    // carries a sampler for every read whether it differs or not. When a technique genuinely needs two, it is two
    // passes — which the graph already composes. Absent ⇒ the engine default, so every existing asset is
    // byte-unchanged.
    bool                                          has_sampler  = false;
    crd::gpu::SamplerDesc                         sampler{};
    // REN-38-A6: how a `kind = "blit"` pass filters while it rescales. Ignored by every other kind.
    FrameBlitFilter                               filter = FrameBlitFilter::Linear;
    // ── ⭐ REN-38 audit: the PASS-STATE vocabulary (depth-write · depth bias · face cull · stencil). ──
    // Appended at the END; every default is the backends' historical hardwired behaviour, so every existing
    // asset is byte-unchanged. Same attribute-not-kind reasoning as the A13 block above. The stencil REFERENCE
    // riding the asset is deliberate: a portal/outline pass pair must agree on the marked value, and two
    // passes agreeing through data they both declare is the whole point of authoring.
    crd::gpu::PassRasterState                     state{};
    // ── ⭐ REN-38-F11: `load = true` — this pass LOADS its target instead of clearing it. ──
    // Appended at the END (blob v5). Without it, two raster passes stacked on ONE target re-cleared: the second
    // pass's first draw wiped the first pass's colour, depth AND stencil — which made the whole stencil
    // vocabulary un-authorable as a mask-then-test pass pair (the very thing stencil exists for).
    bool                                          load_target = false;
    crd::containers::Array<FrameParam>            params;

    explicit FramePassDesc(crd::memory::IAllocator* a)
        : name(a), reads(a), writes(a), draw_list(a), view(a), shader(a), kernel(a), raygen(a), miss(a),
          closest_hit(a), any_hit(a), intersection(a), callable(a), technique(a), blend(a), params(a)
    {
    }
};

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

    // Passes.
    crd::u32 add_pass(crd::containers::StringView name, FramePassKind kind);
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
