#pragma once

// crd-gpu-context — the backend-agnostic RASTER dispatch surface (ADR-0103 / D-008 C1). The graphics analog of
// `IComputeContext`: it draws IR-authored programs into offscreen targets through DYNAMIC RENDERING (no VkRenderPass),
// on the SHADER-OBJECT model — programs + dynamic state, never a monolithic PSO (`VK_EXT_shader_object`), so the
// variant/permutation explosion ADR-0101 warns about never arises. Backends implement it; consumers depend on THIS,
// never on Vulkan. Created by a per-backend factory (`create_vulkan_raster_context`) from a graphics-capable context.
//
// C1-a (this slice) lands the interface + offscreen render targets + a dynamic-rendering CLEAR with pixel readback (the
// graphics-queue/dynamic-rendering/readback plumbing, green). C1-b appends the shader-object DRAW (bind VS+FS programs +
// dynamic state + `vkCmdDraw`); the interface is shaped for it (append-only, vtable-stable).

#include <crd/core/types.hpp>
#include <crd/gpu/frame_graph.hpp> // REN-1: IFrameGraph (create_frame_graph's return — the default body needs it complete)

#include <memory>

namespace crd::gpu
{

class IGpuProgram; // fwd — a raster program is assembled from VS + FS programs (the ADR-0103 currency)
class ICommandEncoder; // fwd — RAF-2: the canonical command-model recorder (command_model.hpp); factory below

// A clear / attachment colour, linear 0..1.
struct ClearColor
{
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
};

// B1-d: the depth-test comparison (fragment depth `op` stored depth ⇒ pass). Order matches VkCompareOp / D3D12.
// ── REN-38-A15: PER-ATTACHMENT BLEND. ────────────────────────────────────────────────────────────────────────
// A pass can declare N colour attachments and N reads, but until this it could not say how they BLEND — so every
// pass rendered opaque. WBOIT alone needs TWO DIFFERENT EQUATIONS ON TWO ATTACHMENTS OF ONE PASS (accumulation
// additive, revealage multiplicative), and additive particles, decals, premultiplied UI and every soft-blend
// technique need it too.
//
// A small CLOSED SET rather than raw src/dst/op factors, for the same reason `BindType` is closed: the cooker has
// to be able to VERIFY what an asset asked for, and an open factor triple cannot be checked — only obeyed.
enum class BlendMode : crd::u8
{
    Opaque = 0,          // no blend; write RGBA (the default, and what every existing pass gets)
    Alpha,               // src.a * src + (1 - src.a) * dst — the ordinary transparency blend
    PremultipliedAlpha,  // src + (1 - src.a) * dst — UI and anything already premultiplied
    Additive,            // src + dst — particles, emissive accumulation, WBOIT ACCUMULATION
    Multiply,            // src * dst — decals, WBOIT REVEALAGE uses the ONE_MINUS_SRC_COLOR form below
    RevealageMultiply,   // dst * (1 - src.rgb) — the WBOIT revealage equation, which no generic mode expresses
    RevealComposite,     // (1 - src.a) * src + src.a * dst — the WBOIT resolve composited OVER the background:
                         // the FS emits (avg, reveal), so this yields `avg·(1-reveal) + background·reveal`. It is the
                         // INVERSE of Alpha's factors, which no other mode expresses — a symmetric quad hides it, an
                         // asymmetric multi-layer scene (reveal far from 0.5) exposes it.
};

enum class DepthCompare : crd::u8
{
    Never = 0,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

// B1-e: a VARIABLE-RATE-SHADING rate — one fragment-shader invocation covers a WxH block of pixels (coarser = cheaper).
// ── ⭐ REN-38-B8: HOW a binding is SAMPLED. ──
// ⛔ This is a CORRECTNESS vocabulary, not a tuning one. A bloom chain sampled with REPEAT wraps bright pixels
// onto the opposite edge — a bloom that glows from the wrong side of the screen. A tiling detail map sampled with
// CLAMP smears its border across the whole surface. One binding needs each, and until this row an asset could
// say NEITHER: the sampler was INFERRED from the resource FORMAT (depth ⇒ comparison, else linear-repeat).
enum class SamplerFilter : crd::u8 { Nearest = 0, Linear };
enum class SamplerAddress : crd::u8
{
    Repeat = 0,
    ClampToEdge,   // the post-process default — an edge texel repeats instead of wrapping
    ClampToBorder, // … or a declared border colour, which is what a shadow map outside its frustum needs
    Mirror,
};
// A whole sampler, as an asset says it. `compare` turns it into a COMPARISON sampler — which is what a shadow
// lookup is, and which the format-inference rule used to decide for the author.
struct SamplerDesc
{
    SamplerFilter  min_filter = SamplerFilter::Linear;
    SamplerFilter  mag_filter = SamplerFilter::Linear;
    SamplerFilter  mip_filter = SamplerFilter::Linear;
    SamplerAddress address    = SamplerAddress::Repeat;
    bool           compare    = false;
    crd::u32       anisotropy = 1U; // 1 = off; the device clamps to its maximum
    float          mip_bias   = 0.0F;
};

// ── ⭐ REN-38 audit: the PASS-STATE vocabulary — the per-pass raster state a technique DECLARES. ─────────────
// ⛔ Until this landed, depth bias was hardwired OFF and face culling hardwired NONE in both backends, the
// depth WRITE could not be turned off, and stencil had FORMATS (B7) but no OPS — so a transparent pass could
// not keep the opaque depth read-only, a shadow pass could not bias or front-face-cull, and the decal/portal/
// outline techniques B7's stencil formats exist for were still inexpressible. Every default below is the
// exact behaviour the backends hardwired, so every existing asset and every existing gate is byte-unchanged.
enum class FaceCull : crd::u8
{
    None = 0, // the historical hardwired value — vertex-pulled geometry has no authored winding guarantee
    Back,
    Front,    // the shadow-caster trick: cull FRONT faces so acne moves to the unlit side
};
enum class FrontFace : crd::u8
{
    CounterClockwise = 0,
    Clockwise,
};
// The closed stencil-op set (VK and D3D12 agree on all eight).
enum class StencilOp : crd::u8
{
    Keep = 0,
    Zero,
    Replace,   // write the reference — the "mark this region" half of every portal/outline/mask technique
    IncrClamp,
    DecrClamp,
    Invert,
    IncrWrap,
    DecrWrap,
};

// The whole per-pass raster state, as an asset says it. CONTEXT STATE like `SamplerDesc` (the B8 discipline):
// installed before a pass records, RESET to these defaults at every pass boundary, never an argument on twenty
// draw verbs. ⛔ On Vulkan (shader objects) every field is dynamic state; on D3D12 everything except the
// stencil REFERENCE is PSO state, so the state participates in the PSO cache key exactly as BlendMode does —
// two passes with different state must never share a pipeline.
struct PassRasterState
{
    bool         depth_write      = true;
    // Depth bias: constant (in device units) + slope-scaled + clamp. All zero = disabled, the historical value.
    float        depth_bias       = 0.0F; // crd-lint-allow-untagged-physical: device depth-bias units (the API scalar VK/D3D define), not a physical quantity
    float        depth_bias_slope = 0.0F; // crd-lint-allow-untagged-physical: dimensionless slope factor
    float        depth_bias_clamp = 0.0F; // crd-lint-allow-untagged-physical: device depth units, same space as depth_bias
    FaceCull     face_cull        = FaceCull::None;
    FrontFace    front_face       = FrontFace::CounterClockwise;
    // Stencil (front and back faces share one description — the closed form every declared technique needs).
    bool         stencil_enable     = false;
    DepthCompare stencil_compare    = DepthCompare::Always;
    crd::u32     stencil_ref        = 0U;
    crd::u32     stencil_read_mask  = 0xFFU;
    crd::u32     stencil_write_mask = 0xFFU;
    StencilOp    stencil_fail       = StencilOp::Keep; // stencil test failed
    StencilOp    stencil_depth_fail = StencilOp::Keep; // stencil passed, depth failed
    StencilOp    stencil_pass       = StencilOp::Keep; // both passed

    // ⛔ EXACT comparison, member by member — the D3D12 PSO cache keys on the WHOLE state (hashing it and
    // colliding would hand one pass another's pipeline: a wrong image with both declarations looking correct).
    [[nodiscard]] constexpr bool operator==(const PassRasterState&) const noexcept = default;
};

enum class ShadingRate : crd::u8
{
    Rate1x1 = 0, // one invocation per pixel (the default)
    Rate1x2,
    Rate2x1,
    Rate2x2, // one invocation per 2x2 block
    Rate2x4,
    Rate4x2,
    Rate4x4,
};

// B1-e: how one VRS rate source combines with the next (pipeline ∘ primitive ∘ attachment). Keep = the first, Replace =
// the second, Min/Max/Mul as named. Matches VkFragmentShadingRateCombinerOpKHR / D3D12_SHADING_RATE_COMBINER order.
enum class ShadingRateCombiner : crd::u8
{
    Keep = 0,
    Replace,
    Min,
    Max,
    Mul,
};

// B1-f: conservative rasterization mode. `Overestimate` = a primitive covers EVERY pixel it touches (even 1% — for
// voxelization / collision). `Underestimate` = only pixels FULLY inside are covered (feeds the inner-coverage input). On
// D3D12 there is one conservative mode (ON); both map to it (inner coverage is read via SV_InnerCoverage under it).
enum class ConservativeMode : crd::u8
{
    Off = 0,
    Overestimate,
    Underestimate,
};

// B2-c: a texture DIMENSION (the gpu-side mirror of kir::TexDim + arrayed/cube). Selects the image/view type on creation.
enum class TextureKind : crd::u8
{
    Tex1D = 0,
    Tex2D,
    Tex3D,
    Cube,       // 6 faces (+X,-X,+Y,-Y,+Z,-Z), sampled by a vec3 direction
    Tex2DArray, // N layers, sampled by vec3(uv, layer)
    CubeArray,  // 6*N faces, sampled by vec4(dir, cube)
};

// An opaque offscreen colour render target (RGBA8). `read_pixel` is valid after an `IRasterContext` op that targeted it.
class IRasterTarget
{
public:
    IRasterTarget()                                = default;
    virtual ~IRasterTarget()                       = default;
    IRasterTarget(const IRasterTarget&)            = delete;
    IRasterTarget& operator=(const IRasterTarget&) = delete;
    IRasterTarget(IRasterTarget&&)                 = delete;
    IRasterTarget& operator=(IRasterTarget&&)      = delete;

    [[nodiscard]] virtual crd::u32 width() const noexcept  = 0;
    [[nodiscard]] virtual crd::u32 height() const noexcept = 0;
    // One texel of the last rendered result, packed little-endian RGBA8 (0xAABBGGRR — R in the low byte).
    [[nodiscard]] virtual crd::u32 read_pixel(crd::u32 x, crd::u32 y) const noexcept = 0;
    // True if this target carries a BUNDLED depth attachment (a `create_color_depth_target`), so a depth-testing pass
    // uses it as its own depth-stencil with no separate resource — the way the live scene verbs read depth off the
    // colour target. Plain colour targets return false (a scene pass into one renders without a depth test).
    [[nodiscard]] virtual bool has_depth() const noexcept { return false; }
};

// An opaque assembled raster program: the SHADER-OBJECT form (a linked VS + FS as `VkShaderEXT` + a pipeline layout),
// built once and drawn many times. This is what makes the seam free of monolithic PSOs (ADR-0101's permutation fear).
class IRasterProgram
{
public:
    IRasterProgram()                                 = default;
    virtual ~IRasterProgram()                        = default;
    IRasterProgram(const IRasterProgram&)            = delete;
    IRasterProgram& operator=(const IRasterProgram&) = delete;
    IRasterProgram(IRasterProgram&&)                 = delete;
    IRasterProgram& operator=(IRasterProgram&&)      = delete;

    [[nodiscard]] virtual bool valid() const noexcept = 0;
};

// The raster dispatch surface. Kernel-source-agnostic like `IComputeContext` (ADR-0100): it renders compiled programs.
// RET-2 (ADR-0105): the present-mode request. Fifo (vsync) is universally available; Mailbox/Immediate fall back to
// Fifo when the surface doesn't offer them (never a creation failure over a pacing preference).
enum class PresentMode : crd::u8
{
    Fifo = 0,
    Mailbox,
    Immediate,
};

// RET-2 (ADR-0105): a swapchain the ONE graphics layer presents through — the capability that retires crd-rhi's
// present path. The design is a pure SINK: the app renders into a NORMAL color target through the unchanged draw
// paths, and `present(target)` blits that canvas into the acquired backbuffer (direct-to-backbuffer rendering arrives
// with the frame graph, post-RET). Two surface kinds behind one interface: a real window (native handle) and a
// HEADLESS surface (VK_EXT_headless_surface) — the full acquire/present/resize machinery, gate-testable with no
// window system at all.
class IPresentSurface
{
public:
    IPresentSurface()                                  = default;
    virtual ~IPresentSurface()                         = default;
    IPresentSurface(const IPresentSurface&)            = delete;
    IPresentSurface& operator=(const IPresentSurface&) = delete;
    IPresentSurface(IPresentSurface&&)                 = delete;
    IPresentSurface& operator=(IPresentSurface&&)      = delete;

    [[nodiscard]] virtual bool     valid() const noexcept = 0;
    [[nodiscard]] virtual crd::u32 width() const noexcept = 0;
    [[nodiscard]] virtual crd::u32 height() const noexcept = 0;

    // Blit `target`'s contents (a color target of MATCHING size that has been drawn at least once) into the next
    // backbuffer and present it. Returns false on a size mismatch or an out-of-date swapchain — call resize().
    [[nodiscard]] virtual bool present(class IRasterTarget& target) = 0;

    // RET-5: present with an OVERLAY composited onto the backbuffer AFTER the canvas blit (ImGui/debug HUDs — the
    // scene canvas stays clean; overlays live at the present seam). `overlay` receives the backend's frame command
    // buffer as an opaque pointer (Vulkan: VkCommandBuffer) inside an active render pass targeting the backbuffer —
    // backend-aware layers (crd-imgui's gpu backend) record into it. Returns false when overlays are unsupported on
    // this surface (never silently dropped).
    using OverlayFn = void (*)(void* backend_cmd, void* user);
    [[nodiscard]] virtual bool present(class IRasterTarget& target, OverlayFn overlay, void* user)
    {
        if (overlay == nullptr) { return present(target); }
        (void)user;
        return false; // a surface that cannot composite overlays says so — honesty over a silent plain present
    }

    // Recreate the swapchain at the new size (window resized / out-of-date). Returns false when the device refuses.
    [[nodiscard]] virtual bool resize(crd::u32 width, crd::u32 height) = 0;

    [[nodiscard]] virtual crd::u64 frame_count() const noexcept = 0; // frames successfully presented

    // ⭐⭐ 38-G1 perf: THE PIPELINING CONTRACT, explicit. `present()` no longer drains the frame it submitted —
    // a ring keeps up to two presents in flight (that deferral removed a ~7 ms per-frame stall in the app), so
    // a resource a present referenced (the blit source, an overlay texture) must stay alive until the surface
    // drains. The surface's DESTRUCTOR and `resize()` always drain; a client tearing down its own targets
    // while the surface lives calls THIS first. Default is a no-op: a backend whose present is synchronous
    // already satisfies the contract. Appended at END (vtable-stable).
    virtual void wait_idle() {}
};

class IRasterContext
{
public:
    IRasterContext()                                 = default;
    virtual ~IRasterContext()                        = default;
    IRasterContext(const IRasterContext&)            = delete;
    IRasterContext& operator=(const IRasterContext&) = delete;
    IRasterContext(IRasterContext&&)                 = delete;
    IRasterContext& operator=(IRasterContext&&)      = delete;

    [[nodiscard]] virtual bool valid() const noexcept = 0; // false ⇒ context not graphics-capable (skip)

    [[nodiscard]] virtual std::unique_ptr<IRasterTarget> create_color_target(crd::u32 width, crd::u32 height) = 0;

    // Clear `target` to `color` via dynamic rendering and make the result host-readable (`read_pixel`). Synchronous.
    virtual void clear(IRasterTarget& target, ClearColor color) = 0;

    // --- C1-b: the shader-object DRAW path (appended — vtable-stable) ---------------------------------------------------

    // Assemble a raster program from a VERTEX and a FRAGMENT `IGpuProgram` (both created via `create_program`). Returns
    // nullptr if the backend can't (e.g. `VK_EXT_shader_object` absent, or a stage mismatch).
    [[nodiscard]] virtual std::unique_ptr<IRasterProgram>
    create_raster_program(IGpuProgram& vertex, IGpuProgram& fragment) = 0;


    // --- B1-c: MSAA render targets (the substrate for `centroid` / `sample` interpolation) --------------------------------

    // A multisample colour target: a draw is `samples`-way multisampled and AVERAGE-resolved to a single-sample image, and
    // `read_pixel` returns the RESOLVED texel — the same surface as a single-sample target, so `draw` is unchanged. The
    // fragment stage runs once per PIXEL unless a `sample`-qualified interpolant forces per-SAMPLE shading (B1-c), and a
    // `centroid`-qualified interpolant is sampled inside the covered area instead of at the pixel centre. `samples` must be
    // a count the backend supports for an RGBA8 colour target (typically 2/4/8); an unsupported count returns nullptr.
    [[nodiscard]] virtual std::unique_ptr<IRasterTarget> create_color_target_ms(crd::u32 width, crd::u32 height,
                                                                                crd::u32 samples) = 0;

    // --- B1-d: depth buffer + depth test (makes a fragment `frag_depth` write observable, and exercises early-Z) ---------

    // A colour target that also carries a D32_SFLOAT depth buffer. Draw into it with `draw_depth`; `read_pixel` returns the
    // COLOUR texel (depth is the test target, not read back). Single-sample.
    [[nodiscard]] virtual std::unique_ptr<IRasterTarget> create_color_depth_target(crd::u32 width, crd::u32 height) = 0;


    // --- B1-e: variable-rate shading (VRS) — coarse shading, three combinable rate sources ------------------------------

    // A colour target that ALSO carries a per-tile shading-rate ATTACHMENT image, every tile set to `tile_rate` (the third
    // VRS source). `draw_vrs` into it applies that rate. Falls back to a plain colour target where VRS is unsupported.
    [[nodiscard]] virtual std::unique_ptr<IRasterTarget> create_color_vrs_target(crd::u32 width, crd::u32 height,
                                                                                 ShadingRate tile_rate) = 0;


    // True iff all three VRS rate sources (pipeline · primitive · attachment) are usable (Vulkan: the feature is enabled;
    // DX12: shading-rate Tier 2). draw_vrs is still callable when false — it just shades at 1x1.
    [[nodiscard]] virtual bool supports_vrs() const noexcept = 0;

    // --- B1-f: conservative rasterization + the inner-coverage input ----------------------------------------------------

    // True iff conservative rasterization is usable (Vulkan: the extension + EDS3 conservative mode; DX12: Tier ≥ 1).
    [[nodiscard]] virtual bool supports_conservative_raster() const noexcept = 0;

    // True iff the INNER-COVERAGE input (`KBuiltin::InnerCoverage`) under UNDERESTIMATE conservative raster is usable
    // (Vulkan: VK_EXT_conservative_rasterization ⇒ FullyCoveredEXT; DX12: conservative Tier 3, which adds SV_InnerCoverage).
    [[nodiscard]] virtual bool supports_inner_coverage() const noexcept = 0;


    // --- B1-f: fragment-shader storage buffer + interlock (ROV) ---------------------------------------------------------

    // Create a device-local storage buffer of `size_bytes` (host-readable via `read_u32`), for a fragment shader to
    // write/read through `draw_storage`. Zero-initialised. nullptr on failure.
    [[nodiscard]] virtual std::unique_ptr<class IStorageBuffer> create_storage_buffer(crd::u32 size_bytes) = 0;


    // True iff RASTERIZER-ORDERED fragment-shader storage access (`KEntry::interlock`) is usable (Vulkan:
    // VK_EXT_fragment_shader_interlock pixel-ordered; DX12: ROVs supported). draw_storage still runs a non-ordered write
    // when false — only the ordering guarantee (deterministic overlapping-pixel RMW) is absent.
    [[nodiscard]] virtual bool supports_fragment_interlock() const noexcept = 0;

    // --- B2: texture & sampler system -----------------------------------------------------------------------------------

    // Create a 2D RGBA8 (`R8G8B8A8_UNORM`) sampled texture of `width`x`height`, initialised from `rgba` (width*height*4
    // bytes, row-major, one byte per channel). Sampled in a fragment shader via `draw_textured`. nullptr on failure.
    [[nodiscard]] virtual std::unique_ptr<class ITexture>
    create_texture(crd::u32 width, crd::u32 height, const void* rgba) = 0;

    // Like `create_texture` but with a FULL MIP CHAIN (box-filtered down the pyramid), so minified sampling filters instead of
    // aliasing — essential for a tiled surface viewed to the horizon (the FFT ocean). The default bilinear/repeat sampler is
    // LINEAR-mipmap, so implicit-LOD `KOp::TexSample` picks the right level per fragment. Default falls back to the single-level
    // `create_texture` (backends without mip generation still work — just unfiltered at minification).
    [[nodiscard]] virtual std::unique_ptr<class ITexture>
    create_texture_mipped(crd::u32 width, crd::u32 height, const void* rgba) { return create_texture(width, height, rgba); }


    // B2-b: a single-channel DEPTH texture for shadow-compare sampling (`KOp::SampleCmp`). `depth` is width*height floats in
    // [0,1], row-major. Sampled via `draw_shadow`'s COMPARISON sampler. nullptr on failure.
    [[nodiscard]] virtual std::unique_ptr<ITexture>
    create_depth_texture(crd::u32 width, crd::u32 height, const float* depth) = 0;


    // B2-c: create a RGBA8 texture of dimension `kind`. `depth_or_layers` = 3D slice count (Tex3D), or array/cube layer
    // count (Cube = 6, Tex2DArray = N, CubeArray = 6*N); ignored (1) for Tex1D/Tex2D. `rgba` is width*height*
    // max(depth_or_layers,1)*4 bytes, slice/layer-major (a cube's 6 faces are +X,-X,+Y,-Y,+Z,-Z). Sampled via `draw_textured`
    // — the FS's sample coordinate must match the kind (1D float · 2D vec2 · 3D/Cube vec3 · Array vec3 · CubeArray vec4).
    [[nodiscard]] virtual std::unique_ptr<ITexture>
    create_texture_dim(TextureKind kind, crd::u32 width, crd::u32 height, crd::u32 depth_or_layers, const void* rgba) = 0;


    // B2-d: true iff dynamic/non-uniform texture-array indexing (bindless) is usable (Vulkan: the descriptor-indexing
    // feature; DX12: resource-binding tier 2+, effectively always). draw_bindless still runs when false (uniform index only).
    [[nodiscard]] virtual bool supports_bindless() const noexcept = 0;


    // B4: create a MESH+FRAGMENT program (the modern amplification path — the mesh shader generates geometry directly, no vertex
    // input). Default (no mesh-shader support) ⇒ nullptr, so the caller falls back to the vertex-pull path. WebGPU has no mesh
    // shaders, so this stays backend-optional.
    [[nodiscard]] virtual std::unique_ptr<IRasterProgram> create_mesh_program(IGpuProgram& /*mesh*/, IGpuProgram& /*fragment*/)
    {
        return nullptr;
    }



    // --- B5: deferred G-buffer (MRT) — a material writes its OpenPBR surface to N colour attachments -------------------

    // A G-BUFFER render target: `attachments` RGBA8 colour attachments (2..8), each host-readable per attachment. The
    // substrate for a deferred material (`ckir::material::pack_gbuffer` → N `KEntry` colour outputs). nullptr on failure.
    [[nodiscard]] virtual std::unique_ptr<class IGBufferTarget>
    create_gbuffer_target(crd::u32 width, crd::u32 height, crd::u32 attachments) = 0;

    // draw_gbuffer (clear all attachments, then draw the surface material's N colour outputs via MRT) de-virtualized off
    // IRasterContext at RAF-12.4 — the command encoder lowers the G-buffer SHAPE (GeometryKind::None + RenderingDesc::
    // gbuffer set, the clear on the clear-carrier colour entry) to each backend's private draw_gbuffer body. See
    // detail/command_lowering.hpp (None case) and engine/*/src/*_raster_context.cpp.

    // B4: create a TASK→MESH→FRAGMENT program (the amplification path). The task shader runs first, computes how many mesh
    // workgroups to launch (`EmitMeshTasksEXT` / AS `DispatchMesh`) + a single-uint payload the mesh reads (`KBuiltin::TaskPayload`).
    // Draw with `draw_mesh` / `draw_mesh_bindless_depth`, passing the TASK-workgroup count as `group_count`. Default (no mesh/task
    // support) ⇒ nullptr, so the caller falls back to a plain mesh or the vertex-pull path. Appended at END (vtable-stable, D135).
    [[nodiscard]] virtual std::unique_ptr<IRasterProgram>
    create_task_mesh_program(IGpuProgram& /*task*/, IGpuProgram& /*mesh*/, IGpuProgram& /*fragment*/)
    {
        return nullptr;
    }

    // B4-tess: create a VS→TessControl→TessEval→FRAGMENT program (the PORTABLE displacement path for HW without mesh shaders —
    // mobile / WebGPU / older). Default (no tessellation support) ⇒ nullptr, so the caller falls back to mesh / vertex-pull.
    [[nodiscard]] virtual std::unique_ptr<IRasterProgram>
    create_tess_program(IGpuProgram& /*vertex*/, IGpuProgram& /*tess_control*/, IGpuProgram& /*tess_eval*/,
                        IGpuProgram& /*fragment*/)
    {
        return nullptr;
    }


    // B4-vis-4: a R32_UINT VISIBILITY-BUFFER target — the HW-raster half of the Nanite split (HW raster wins on big triangles).
    // The fragment shader writes a per-pixel primitive id (KBuiltin::PrimitiveId → SV_PrimitiveId / gl_PrimitiveID), which a
    // deferred pass materializes. `read_pixel` returns the raw u32 id. Default (no uint-target support) ⇒ nullptr.
    [[nodiscard]] virtual std::unique_ptr<IRasterTarget> create_visbuffer_target(crd::u32 /*width*/, crd::u32 /*height*/)
    {
        return nullptr;
    }




    // B17-a WEIGHTED-BLENDED OIT (McGuire-Bavoil 2013): the fused draw_wboit verb was DELETED at RAF-12.4 — WBOIT is now
    // TWO authored frame-graph passes (a `raster.mrt` accumulate with per-attachment additive/revealage_multiply blend +
    // a `raster.composite` resolve blended `RevealComposite` = `{1-srcα, srcα}` over the background), the mission's ONE
    // rendering path. The fused verb allocated its own accum/revealage images inside the call — a second untracked
    // allocator the frame graph forbids. Gated per-texel vs the McGuire-Bavoil oracle on an asymmetric 4-layer scene
    // (REN-38-A12 ORACLE, both backends); the encoder's draw_storage_mrt lowers the accumulate pass.

    // --- GEO-1: CPU upload into a storage buffer (VERTEX PULLING — the bindless vertex-feeding path) ---------------------
    //
    // Copy `size_bytes` from `data` into `storage` at `byte_offset` (a staged transfer; blocks until visible to shaders).
    // The GEO-1 draw gate uploads a COOKED MeshResource vertex stream and the VS fetches it by `VertexIndex` through
    // `storage_load` (set 0 / binding 0 — the same buffer `draw_storage` binds, now visible to the VERTEX stage too).
    // Returns false when out of range / unsupported. Appended at END (vtable-stable).
    [[nodiscard]] virtual bool upload_storage(IStorageBuffer& /*storage*/, crd::u32 /*byte_offset*/,
                                              const void* /*data*/, crd::u32 /*size_bytes*/) { return false; }

    // GEO-3 stage 4 / RET-3 (ADR-0105): create a sampled 2D texture from a COOKED mip chain. `mips[i]` points at
    // level-i RGBA8 pixels (dimensions halving from width×height down to 1×1, `mip_count` levels — the TXTR artifact
    // layout, ADR-0042). The chain uploads VERBATIM — never re-derived on device (the cook filters sRGB content in
    // LINEAR space; a device-side box blit would re-introduce the mip-darkening bug this pipeline eliminates).
    // `srgb` selects an sRGB image format ⇒ hardware decode-on-sample (TexSample AND TexelFetch both decode).
    // Returns nullptr on invalid input / unsupported. Appended at END (vtable-stable).
    [[nodiscard]] virtual std::unique_ptr<class ITexture>
    create_texture_from_mips(crd::u32 /*width*/, crd::u32 /*height*/, crd::u32 /*mip_count*/,
                             const void* const* /*mips*/, bool /*srgb*/) { return nullptr; }

    // RET-2 (ADR-0105): create a present surface. `native_window` = the platform window handle (HWND on Windows);
    // nullptr requests a HEADLESS surface (VK_EXT_headless_surface — the full swapchain machinery with no window,
    // the gate-testable path). Returns nullptr when the context lacks the present capability (see
    // VulkanGpuContext::present_capable / headless_surface). Appended at END (vtable-stable).
    [[nodiscard]] virtual std::unique_ptr<IPresentSurface>
    create_present_surface(void* /*native_window*/, crd::u32 /*width*/, crd::u32 /*height*/, PresentMode /*mode*/)
    {
        return nullptr;
    }

    // RET-4 pt 5: copy `storage`'s CURRENT device contents into its host-visible readback so `read_u32` reflects
    // them WITHOUT a draw — `upload_storage`'s twin (compute results, defrag verification, tooling reads).
    // Returns false when unsupported. Appended at END (vtable-stable).
    [[nodiscard]] virtual bool download_storage(IStorageBuffer& /*storage*/) { return false; }



    // REN-1 (D-007 row 98): create a FRAME GRAPH bound to this context — the async single-submission surface
    // that replaces the synchronous submit+wait+readback-per-draw substrate (see frame_graph.hpp). Passes
    // record via THIS context in frame-recording mode. Returns nullptr on a backend that lacks it (DX12 until
    // its port). Appended at END (vtable-stable).
    [[nodiscard]] virtual std::unique_ptr<IFrameGraph> create_frame_graph() { return nullptr; }

    // ── REN-38-A1b: MULTIPLE RENDER TARGETS from the FRAME GRAPH's own transients. ────────────────────────────
    // ⛔ WHY THIS EXISTS ALONGSIDE `draw_gbuffer`. That verb takes an `IGBufferTarget` — a self-contained resource
    // with its own images, its own readbacks and its own lifetime. The frame graph owns none of that: it owns N
    // independent transient images, aliases them against each other, and barriers them individually. Handing it a
    // G-buffer object would put a second, untracked allocator inside the graph and defeat the aliasing it exists
    // for. So the graph's MRT verb takes N of ITS OWN targets, which is also exactly what the asset writes:
    //     writes = ["albedo", "normal", "material"]
    // `draw_gbuffer` remains the standalone path; neither is a special case of the other.
    //
    // Every target must share width/height; the FIRST one's dimensions define the render area. Appended at the
    // END of the vtable (D135). Default is a no-op so a backend without it fails VISIBLY (nothing renders) rather
    // than by silently drawing into attachment 0 only.


    // RET-6 / REN-39: the OVERLAY draw (draw_overlay / draw_overlay_range) de-virtualized off IRasterContext at
    // RAF-12.4 — the command encoder lowers the overlay SHAPE (a StoragePull draw with a single colour attachment
    // that LOADs and Alpha-blends + a read-only depth test carried by `compare`) to each backend's private overlay
    // body. See engine/*/src/*_raster_context.cpp and detail/command_lowering.hpp (StoragePull case).





    // ── ⭐ REN-38-A6: THE TRANSFER VERBS. Appended at the END of the vtable (D135). ──
    // Moving pixels from one target to another without a shader. Every real frame graph needs these — a
    // half-resolution downsample before a blur, an MSAA resolve before post, a snapshot for the next frame's
    // reprojection — and until this row NONE of them could be expressed: the only way to copy an image was to
    // author a fullscreen pass with a pass-through shader, which pays for a rasterizer, a descriptor set and a
    // pipeline to do what the copy engine does for free.
    //
    // ⛔ THREE VERBS, NOT ONE, because the hardware genuinely distinguishes them and collapsing them would make
    // one of the three silently wrong:
    //   · copy    — identical extent AND format; a straight `vkCmdCopyImage` / `CopyResource`. Exact bytes.
    //   · blit    — rescales and filters. NOT available on DX12's copy engine at all (see below).
    //   · resolve — collapses an MSAA image to one sample. A copy of an MSAA image is illegal, not merely slow.
    // A single `copy` that quietly rescaled would turn a mismatched-size authoring mistake into a soft image; a
    // single `blit` that quietly did exact copies would turn a downsample into a crop.

    // Filtering for `blit_image`. Nearest preserves exact texel values (the choice for id/visibility buffers,
    // where interpolating two ids yields a THIRD id that names nothing); Linear is the box filter a downsample
    // wants.
    enum class BlitFilter : crd::u8 { Nearest = 0, Linear };

    // ⭐ RAF-12.4 (F1): copy_image / blit_image / resolve_image are no longer public verbs — the exact-copy /
    // rescaling-blit (DX12 rescales through a fullscreen DRAW; its copy engine has no blit) / MSAA-resolve lowering
    // moved into each backend's PRIVATE method, reached only by CommandEncoder<Ctx> through a TransferDesc.









    // Does this backend/adapter offer a ray-tracing PIPELINE (not merely inline ray query)? ⛔ Reported, so an
    // authored graph degrades by DECLARED CAPABILITY (REN-35's rule) instead of rendering a black frame.
    [[nodiscard]] virtual bool supports_rt_pipeline() const noexcept { return false; }

    // ── ⭐ REN-38-B3: FILL a buffer range with a 32-bit value, recorded into the frame. Appended at END (D135). ──
    // ⛔ This exists for ONE reason and it is not convenience: an APPEND/COUNTER buffer must be RESET TO ZERO at
    // the start of every frame. A counter that is not reset accumulates across frames — so the cull pass appends
    // past the end of its list on frame 2, and the GPU-driven draw reads garbage indices. Nothing about that is
    // visible in the asset, and it gets worse the longer the app runs, which is the hardest failure shape to
    // attribute to a missing memset.
    //
    // Recorded, never a separate submission: the reset must be ORDERED against the pass that appends, and the only
    // thing that can order it is the graph.
    virtual void fill_buffer(IStorageBuffer& /*buffer*/, crd::u64 /*offset*/, crd::u64 /*size*/, crd::u32 /*value*/) {}

    // ── ⭐ REN-38-B8: install the sampler the NEXT recorded draw binds. Appended at the END (D135). ──
    // ⛔ Stated as CONTEXT STATE rather than an extra argument on twenty draw verbs, for the same reason the frame
    // graph owns barriers: adding a parameter to every `draw_*` would mean every future verb has to remember it,
    // and the one that forgets renders with whatever the last pass installed — a bug that only appears when two
    // passes with different sampling run in the same frame. Reset to the engine default at each pass boundary,
    // so a pass that says nothing gets the default and never inherits its neighbour's.
    virtual void set_sampler(const SamplerDesc& /*desc*/) {}

    // ── ⭐ REN-38 audit: install the pass-state the NEXT recorded draws use. Appended at the END (D135). ──
    // Same discipline as `set_sampler` directly above, for the same reason: context state, reset to the
    // defaults at each pass boundary, so a pass that declares nothing gets the historical behaviour and never
    // inherits its neighbour's bias, cull mode or stencil configuration.
    virtual void set_pass_state(const PassRasterState& /*state*/) {}




    // ── ⭐ REN-38-F11: a colour + DEPTH-STENCIL target (D24S8). Appended at the END (D135). ──
    // The F10 pass-state vocabulary declared, cooked and installed every stencil op — and no target carried the
    // stencil ASPECT, so the whole axis could configure but never draw. Draw verbs bind the stencil attachment
    // whenever the target has one; the pass-state ref/masks/ops then work exactly as declared. Default null: a
    // backend without it makes the stencil gates SKIP rather than pass on a depth-only image.
    [[nodiscard]] virtual std::unique_ptr<IRasterTarget> create_color_depth_stencil_target(crd::u32 /*width*/,
                                                                                           crd::u32 /*height*/)
    {
        return nullptr;
    }



    // ── ⭐⭐ REN-38: MULTI-DRAW — N depth-tested storage draws in ONE device command. Appended at END. ──────
    // The batching measurement (6.1x on Vulkan, 38.8x on DX12 at 64 draws) named the per-draw descriptor
    // reset as the whole cost; this verb records ONE descriptor set, ONE state block, and ONE
    // vkCmdDrawIndirect / ExecuteIndirect over `count` commands. `vertex_counts[i]` sizes command i.
    //
    // THE DRAW-INDEX CONTRACT: command i executes with `DrawIndex == first_draw_index + i` — on Vulkan the
    // batch's first index rides a push constant and `gl_DrawID` counts commands; on DX12 the command signature
    // varies a root constant per command. A vertex program cooked with `KBuiltin::DrawIndex` uses it to find
    // its record (its buffer base, its material index) in the shared storage buffer — which is what makes many
    // DIFFERENT objects one draw call. A program that never reads DrawIndex batches identical-record draws.
    //
    // `load_target` false ⇒ the first command clears (colour + depth), the rest continue — the same first/load
    // split every scene pass already uses. ⛔ The default falls back to a LOOP of classic draws WITHOUT the
    // index channel: correct only for programs that do not read DrawIndex. Both real backends override; the
    // fallback exists for stubs, and a stub feeding an index-reading program would draw record 0 repeatedly —
    // visibly wrong rather than silently absent, which is the honest failure shape.
    // REN-38: how many MULTI-DRAW batches this context has recorded (monotonic). The batching gates assert a
    // DELTA of exactly one batch per bucket — pixels alone cannot distinguish "batched" from "looped".
    [[nodiscard]] virtual crd::u64 multi_batch_count() const noexcept { return 0U; }
    // ⭐⭐ REN-39-C1: the subset of `multi_batch_count` that used an INDEX BUFFER (the indexed-pull verbs). Both pull
    // and indexed cascades batch now (both push the DrawIndex row), so total batches no longer tells the modes apart —
    // this counter does: indexed pull records these, non-indexed pull records ZERO of them.
    [[nodiscard]] virtual crd::u64 multi_indexed_batch_count() const noexcept { return 0U; }
    [[nodiscard]] virtual crd::u64 compute_dispatch_count() const noexcept { return 0U; }
    virtual void compute_diag(crd::u32 phase) noexcept { (void)phase; }
    [[nodiscard]] virtual crd::u64 compute_diag_count(crd::u32 phase) const noexcept { (void)phase; return 0U; }

    // ── ⭐⭐ 38-G1 perf: BATCHED UPLOADS. Appended at END. ─────────────────────────────────────────────────
    // `upload_storage` is contractually synchronous — staged copy, submit, WAIT — which is correct for a test
    // that reads the buffer back on the next line and catastrophic for a frame: the live scene issued ~50
    // uploads per frame at one queue-idle each (measured: 8.3 ms of a 16 ms frame was upload waits).
    // Between `begin_upload_batch()` and `end_upload_batch()`, uploads on a supporting backend instead land in
    // a persistent staging ring and record into ONE transfer command buffer; end submits it ONCE, with no host
    // wait — same-queue submission order sequences the copies before the frame's draws.
    // THE CONTRACT HOLDS EVERYWHERE ELSE: any synchronous verb (a draw, a dispatch, a readback) FLUSHES an
    // open batch first, so upload-then-read code observes exactly what it always did, just without paying a
    // wait per upload. The default implementations are no-ops: a backend without the fast path simply keeps
    // its synchronous uploads — semantics identical, speed unchanged.
    virtual void begin_upload_batch() {}
    virtual void end_upload_batch() {}


    // ── ⭐⭐ REN-39-A2: INDEXED MULTI-DRAW — N indexed draws in ONE device command. Appended at END. ──────────
    // The indexed twin of `draw_storage_multi_depth`: ONE descriptor set, ONE state block, ONE index-buffer bind
    // (`index_offset_bytes` locates the u32 index section; each draw's `first_index` addresses WITHIN it), and
    // ONE `vkCmdDrawIndexedIndirect` / `ExecuteIndirect` over `count` commands. The DrawIndex contract is
    // UNCHANGED from the non-indexed verb: command i executes with `DrawIndex == first_draw_index + i`, which is
    // how a program finds its record (buffer base, material index, instance-list base) in the shared storage.
    //
    // ⛔ `IndexedDraw` carries THREE fields ON PURPOSE — the hardware args' other two (base-vertex,
    // first-instance) are DELIBERATELY UNREPRESENTABLE: VK's `gl_InstanceIndex` includes firstInstance and
    // DX12's `SV_InstanceID` does not, so a verb that let a caller set them would let the two backends read
    // DIFFERENT instances. Both are 0 in every backend, always; per-draw bases ride the DrawIndex'd draw table.
    //
    // ⛔ The default falls back to a LOOP of classic indexed draws WITHOUT the index channel — correct only for
    // programs that do not read DrawIndex (the same documented shape as `draw_storage_multi_depth`'s fallback).
    // Both real backends override; a stub feeding an index-reading program draws record 0 repeatedly — visibly
    // wrong rather than silently absent. Batches count via the SAME `multi_batch_count()` counter.
    struct IndexedDraw
    {
        crd::u32 index_count;    // indices this command consumes
        crd::u32 instance_count; // the caller's per-draw visible count
        crd::u32 first_index;    // start within the bound index section (in INDICES, not bytes)
    };



    // ── ⭐⭐ REN-39-D1: WHICH WAY IS +Y IN CLIP SPACE. Appended at END. ───────────────────────────────────────
    // Vulkan's NDC has +Y pointing DOWN the framebuffer; D3D12's points UP. For ordinary rendering this is
    // invisible — a render target and the fullscreen pass that consumes it flip together, so the screen comes out
    // identical on both. It becomes VISIBLE the moment a shader turns a CLIP position back into a TEXTURE
    // coordinate, because `uv = ndc*0.5 + 0.5` bakes one convention in: the same formula reads the MIRRORED row
    // on the other backend.
    //
    // ⛔ THE SCAR THIS ENCODES. Shadow mapping is exactly that operation, and it read as "DX12's shadows are
    // wrong" for a whole session: the cascade fit, the culling, the per-slice DSVs, the barriers, the indexed
    // draw path, the emitters and the atlas contents were all measured CORRECT, because none of them was wrong.
    // Proof was one line: flipping V made DX12 match Vulkan to 0.83% (from 12.94%). Every future clip-derived UV
    // — SSR, TAA reprojection, planar reflections, DDGI — has the same hazard, so the answer is a DECLARED
    // backend fact rather than a fix at each call site.
    //
    // ⛔ A TECHNIQUE MUST NEVER READ THIS. Authors write one portable formula; the ENGINE folds the convention
    // into the matrix it supplies (see the `csm_light_vp` binding in scene_renderer.cpp). That is the same
    // "the technique declares WHAT, the engine decides WHERE" seam the binding resolver already is.
    [[nodiscard]] virtual bool ndc_y_points_down() const noexcept { return true; } // Vulkan's convention

    // ── ⭐⭐ REN-40-A: THE GPU-WRITTEN DRAW — args AND count sourced from device memory. Appended at END. ─────
    // The verb declares the INTENT: *execute up to `max_draws` indexed indirect commands from `args`, taking the
    // ACTUAL count from a u32 in `count_buf`*. Nothing about the caller's data touches the CPU: a compute pass
    // writes both buffers, and the device decides how many commands run — which is the whole point at a million
    // instances, because an empty batch then costs NOTHING rather than a zero-instance command each.
    //
    // ⛔⛔⛔ THE STANDING RULE THIS VERB EXISTS UNDER: use every ability of every API, incorporated into the
    // general context; where a backend lacks one, fall back to THAT BACKEND'S equivalent — but always do the
    // best it can. Never pick the lowest common denominator "for portability". Both backends have the real
    // mechanism (`vkCmdDrawIndexedIndirectCount`, core since Vulkan 1.2 and we target 1.3; `ExecuteIndirect`'s
    // `pCountBuffer`, D3D12 since 1.0), so BOTH use it. `indirect_count_supported()` is the declared capability;
    // the DEFAULT below is the NAMED fallback — clamp to `max_draws` and rely on zero-instance commands — for a
    // device that reports it missing. A step-down nobody can observe is how "portable" becomes "slow everywhere".
    //
    // ⛔ `args` is an array of the API's indexed-indirect command struct, which is BINARY-IDENTICAL across the
    // two (`VkDrawIndexedIndirectCommand` == `D3D12_DRAW_INDEXED_ARGUMENTS`: index_count, instance_count,
    // first_index, base_vertex, first_instance), so ONE GPU-written buffer feeds both backends unchanged.
    // ⛔ base_vertex/first_instance must be written 0 by the producer — `IndexedDraw` above documents why
    // (VK folds firstInstance into gl_InstanceIndex, DX12's SV_InstanceID does not). A GPU-driven producer
    // addresses its per-batch region through DrawIndex + the draw table instead.
    // ⛔⛔ THE COMMAND LAYOUT IS A BACKEND FACT — DECLARE IT, do not assume it matches.
    // The ARGS STRUCTS are binary-identical (`VkDrawIndexedIndirectCommand` == `D3D12_DRAW_INDEXED_ARGUMENTS`:
    // index_count, instance_count, first_index, base_vertex, first_instance), and it is tempting to conclude one
    // GPU-written buffer feeds both unchanged. IT DOES NOT. This engine's D3D12 command signature PREPENDS a
    // root constant carrying DrawIndex — and D3D12 requires the draw argument to be LAST in a signature — so the
    // D3D12 command is [u32 draw_index][5×u32 args] at a 24-byte stride, while Vulkan's is [5×u32 args] at 20.
    // Same fields, different ORDER and STRIDE.
    // ⛔ Levelling one backend down to the other's layout would cost DX12 its DrawIndex channel (which is how a
    // rebased program finds its region) — so the layout is EXPOSED and the GPU producer writes each backend's
    // own form: args at `base + i*stride + arg_offset`, and where `arg_offset != 0` the leading u32 is the
    // command's DrawIndex. This is the standing rule applied to data layout, not just to verbs.
    [[nodiscard]] virtual crd::u32 indirect_command_stride() const noexcept { return 20U; }
    [[nodiscard]] virtual crd::u32 indirect_command_arg_offset() const noexcept { return 0U; }

    [[nodiscard]] virtual bool indirect_count_supported() const noexcept { return false; }


    // REN-40-G1: depth prepass support. When set, the NEXT draw verb that begins a render pass will LOAD the depth
    // attachment instead of clearing it, while still CLEARING the colour attachment normally. The flag is consumed
    // by the next draw and auto-resets. Context state, same discipline as set_sampler / set_pass_state.
    virtual void set_next_draw_load_depth(bool /*load*/) {}




    // ── RAF-2/12 (D-007 "RAF band"): the CANONICAL command-model ENCODER factory — THE one recording path. ──────────
    // Returns an ICommandEncoder that records the backend-neutral command model (command_model.hpp), replacing the ~53
    // combinatorial draw_*/dispatch_*/trace_* verbs. ⭐ RAF-12.4: PURE VIRTUAL — each backend returns its own
    // `detail::CommandEncoder<Concrete>` (detail/command_lowering.hpp) so the lowering resolves STATICALLY to that
    // backend's (Phase B: private) methods. The verbs are being de-virtualized off this interface family-by-family; a
    // backend's encoder is the ONLY thing that reaches them. ⛔ Appended at the END of the vtable (D135).
    [[nodiscard]] virtual std::unique_ptr<ICommandEncoder> create_command_encoder() = 0;
};

// ⭐ REN-38-A9: a BUILT acceleration structure, behind one portable handle.
// ⛔ It is declared HERE, in the backend-neutral header, and the backends' scene types DERIVE from it — rather
// than the frame layer knowing about `RtScene` or `Dx12RtScene`. The asset names an AS; the host resolves that
// name to one of these; the raster context unwraps it to its native handle. No layer above the backend ever sees
// a VkAccelerationStructureKHR or a D3D12 GPU virtual address.
class IAccelerationStructure
{
public:
    IAccelerationStructure()                                         = default;
    virtual ~IAccelerationStructure()                                = default;
    IAccelerationStructure(const IAccelerationStructure&)            = delete;
    IAccelerationStructure& operator=(const IAccelerationStructure&) = delete;
    IAccelerationStructure(IAccelerationStructure&&)                 = delete;
    IAccelerationStructure& operator=(IAccelerationStructure&&)      = delete;
};

// B5: an opaque deferred G-buffer (see `create_gbuffer_target`). `read_pixel(attachment, x, y)` is valid after a draw.
class IGBufferTarget
{
public:
    IGBufferTarget()                                 = default;
    virtual ~IGBufferTarget()                        = default;
    IGBufferTarget(const IGBufferTarget&)            = delete;
    IGBufferTarget& operator=(const IGBufferTarget&) = delete;
    IGBufferTarget(IGBufferTarget&&)                 = delete;
    IGBufferTarget& operator=(IGBufferTarget&&)      = delete;

    [[nodiscard]] virtual crd::u32 width() const noexcept            = 0;
    [[nodiscard]] virtual crd::u32 height() const noexcept           = 0;
    [[nodiscard]] virtual crd::u32 attachment_count() const noexcept = 0;
    // One texel of attachment `attachment`, packed little-endian RGBA8 (0xAABBGGRR). 0 if out of range.
    [[nodiscard]] virtual crd::u32 read_pixel(crd::u32 attachment, crd::u32 x, crd::u32 y) const noexcept = 0;
};

// B2: an opaque sampled texture (see `IRasterContext::create_texture`). Bound to a draw via `draw_textured`.
class ITexture
{
public:
    ITexture()                           = default;
    virtual ~ITexture()                  = default;
    ITexture(const ITexture&)            = delete;
    ITexture& operator=(const ITexture&) = delete;
    ITexture(ITexture&&)                 = delete;
    ITexture& operator=(ITexture&&)      = delete;

    [[nodiscard]] virtual crd::u32 width() const noexcept  = 0;
    [[nodiscard]] virtual crd::u32 height() const noexcept = 0;
    // ⭐⭐ REN-40-D: whether this texture holds DEPTH — the property the ATLAS SAMPLER is chosen by. A depth
    // atlas is sampled through the comparison sampler (that is what a shadow lookup is); a MOMENT atlas is a
    // colour image whose whole point is ordinary filterable sampling, so it takes a linear/clamp sampler at the
    // same slot. Keyed off the texture rather than off a flag the caller must remember, exactly like the
    // frame graph's own "the sampler is chosen from the resource format" rule. Defaulted so external
    // implementations keep compiling; both engine backends override it from their stored format.
    [[nodiscard]] virtual bool is_depth() const noexcept { return false; }
};

// B1-f: an opaque fragment-shader storage buffer. `read_u32(i)` returns element i (4-byte words) after a `draw_storage`.
class IStorageBuffer
{
public:
    IStorageBuffer()                                 = default;
    virtual ~IStorageBuffer()                        = default;
    IStorageBuffer(const IStorageBuffer&)            = delete;
    IStorageBuffer& operator=(const IStorageBuffer&) = delete;
    IStorageBuffer(IStorageBuffer&&)                 = delete;
    IStorageBuffer& operator=(IStorageBuffer&&)      = delete;

    [[nodiscard]] virtual crd::u32 size_bytes() const noexcept       = 0;
    [[nodiscard]] virtual crd::u32 read_u32(crd::u32 index) const noexcept = 0; // element `index` (0 out of range)
};

} // namespace crd::gpu
