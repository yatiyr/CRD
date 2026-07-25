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
};

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

enum class FrameCullMode : crd::u8 { None = 0, Frustum, FrustumOcclusion };
enum class FrameSortMode : crd::u8 { None = 0, FrontToBack, BackToFront, Material };

// A graph-owned resource. Transients are lifetime-analysed + memory-aliased by the graph exactly as today; the
// asset never expresses a lifetime or a barrier, because both are DERIVED from the pass reads/writes.
enum class FrameResourceKind : crd::u8 { TransientImage = 0, TransientBuffer };

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
    FrameMaterialPass                             material_pass = FrameMaterialPass::None;
    FrameForEach                                  for_each      = FrameForEach::None;
    crd::u32                                      for_each_arg  = 0U; // e.g. the light index in light.N.cascades
    bool                                          has_clear_color = false;
    float                                         clear_color[4]  = {0.0F, 0.0F, 0.0F, 1.0F};
    bool                                          has_clear_depth = false;
    float                                         clear_depth     = 1.0F;
    crd::gpu::DepthCompare                        depth = crd::gpu::DepthCompare::LessEqual;
    crd::containers::Array<FrameParam>            params;

    explicit FramePassDesc(crd::memory::IAllocator* a)
        : name(a), reads(a), writes(a), draw_list(a), view(a), shader(a), kernel(a), params(a)
    {
    }
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

    explicit FrameGraphDesc(crd::memory::IAllocator* a)
        : name(a), resources(a), draw_lists(a), passes(a), requires_caps(a), fallback(a)
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

} // namespace crd::framecook
