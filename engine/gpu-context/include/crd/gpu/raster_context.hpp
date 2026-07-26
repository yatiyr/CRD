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

    // Clear `target` to `clear`, then draw `vertex_count` vertices with `program` (attributeless — the VS positions from
    // its vertex index), via dynamic rendering + shader objects. Result host-readable (`read_pixel`). Synchronous.
    virtual void draw(IRasterTarget& target, IRasterProgram& program, ClearColor clear, crd::u32 vertex_count) = 0;

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

    // Clear colour to `clear` and depth to `clear_depth`, then draw with the depth test enabled at `compare` (depth write
    // on). A fragment passes iff `fragment_depth compare stored_depth`. Target must come from `create_color_depth_target`.
    virtual void draw_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear, float clear_depth,
                            DepthCompare compare, crd::u32 vertex_count) = 0;

    // --- B1-e: variable-rate shading (VRS) — coarse shading, three combinable rate sources ------------------------------

    // A colour target that ALSO carries a per-tile shading-rate ATTACHMENT image, every tile set to `tile_rate` (the third
    // VRS source). `draw_vrs` into it applies that rate. Falls back to a plain colour target where VRS is unsupported.
    [[nodiscard]] virtual std::unique_ptr<IRasterTarget> create_color_vrs_target(crd::u32 width, crd::u32 height,
                                                                                 ShadingRate tile_rate) = 0;

    // Clear + draw with VRS. `pipeline_rate` is the per-DRAW rate; `primitive_combiner` says how the shader-output
    // (per-PRIMITIVE, `KEntry::shading_rate`) rate combines with it (Keep = pipeline wins, Replace = primitive wins). If
    // `target` came from `create_color_vrs_target`, its per-tile attachment rate then REPLACES the result. On an adapter
    // without VRS this shades at 1x1 (a correct, if not coarse, result). Result host-readable via `read_pixel`.
    virtual void draw_vrs(IRasterTarget& target, IRasterProgram& program, ClearColor clear, ShadingRate pipeline_rate,
                          ShadingRateCombiner primitive_combiner, crd::u32 vertex_count) = 0;

    // True iff all three VRS rate sources (pipeline · primitive · attachment) are usable (Vulkan: the feature is enabled;
    // DX12: shading-rate Tier 2). draw_vrs is still callable when false — it just shades at 1x1.
    [[nodiscard]] virtual bool supports_vrs() const noexcept = 0;

    // --- B1-f: conservative rasterization + the inner-coverage input ----------------------------------------------------

    // True iff conservative rasterization is usable (Vulkan: the extension + EDS3 conservative mode; DX12: Tier ≥ 1).
    [[nodiscard]] virtual bool supports_conservative_raster() const noexcept = 0;

    // True iff the INNER-COVERAGE input (`KBuiltin::InnerCoverage`) under UNDERESTIMATE conservative raster is usable
    // (Vulkan: VK_EXT_conservative_rasterization ⇒ FullyCoveredEXT; DX12: conservative Tier 3, which adds SV_InnerCoverage).
    [[nodiscard]] virtual bool supports_inner_coverage() const noexcept = 0;

    // Clear + draw with conservative rasterization at `mode` (Off = normal). Overestimate covers every touched pixel;
    // Underestimate feeds the inner-coverage input (`KBuiltin::InnerCoverage`). Falls back to a normal draw when unsupported.
    virtual void draw_conservative(IRasterTarget& target, IRasterProgram& program, ClearColor clear, ConservativeMode mode,
                                   crd::u32 vertex_count) = 0;

    // --- B1-f: fragment-shader storage buffer + interlock (ROV) ---------------------------------------------------------

    // Create a device-local storage buffer of `size_bytes` (host-readable via `read_u32`), for a fragment shader to
    // write/read through `draw_storage`. Zero-initialised. nullptr on failure.
    [[nodiscard]] virtual std::unique_ptr<class IStorageBuffer> create_storage_buffer(crd::u32 size_bytes) = 0;

    // Clear + draw with `storage` bound to the fragment shader at set 0 / binding 0 (u0 on DX12). The program's FS may
    // read/write it (and, if it declared interlock, do so under rasterizer-ordered access). Result host-readable.
    virtual void draw_storage(IRasterTarget& target, IRasterProgram& program, ClearColor clear, IStorageBuffer& storage,
                              crd::u32 vertex_count) = 0;

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

    // Clear + draw with `texture` bound to the fragment shader's texture slot (Vulkan set 0 / binding 1 sampled image +
    // binding 2 sampler; DX12 t1 SRV + s2 sampler). The program's FS samples it (`KOp::TexSample`) through a default
    // bilinear/repeat sampler. Result host-readable via `read_pixel`.
    virtual void draw_textured(IRasterTarget& target, IRasterProgram& program, ClearColor clear, ITexture& texture,
                               crd::u32 vertex_count) = 0;

    // B2-b: a single-channel DEPTH texture for shadow-compare sampling (`KOp::SampleCmp`). `depth` is width*height floats in
    // [0,1], row-major. Sampled via `draw_shadow`'s COMPARISON sampler. nullptr on failure.
    [[nodiscard]] virtual std::unique_ptr<ITexture>
    create_depth_texture(crd::u32 width, crd::u32 height, const float* depth) = 0;

    // B2-b: clear + draw with `depth` bound at the texture slot and a COMPARISON sampler (compareOp LESS_OR_EQUAL) at the
    // sampler slot. The FS's shadow sample (`KOp::SampleCmp`) returns 1 where `ref <= stored depth`, else 0. Host-readable.
    virtual void draw_shadow(IRasterTarget& target, IRasterProgram& program, ClearColor clear, ITexture& depth,
                             crd::u32 vertex_count) = 0;

    // B2-c: create a RGBA8 texture of dimension `kind`. `depth_or_layers` = 3D slice count (Tex3D), or array/cube layer
    // count (Cube = 6, Tex2DArray = N, CubeArray = 6*N); ignored (1) for Tex1D/Tex2D. `rgba` is width*height*
    // max(depth_or_layers,1)*4 bytes, slice/layer-major (a cube's 6 faces are +X,-X,+Y,-Y,+Z,-Z). Sampled via `draw_textured`
    // — the FS's sample coordinate must match the kind (1D float · 2D vec2 · 3D/Cube vec3 · Array vec3 · CubeArray vec4).
    [[nodiscard]] virtual std::unique_ptr<ITexture>
    create_texture_dim(TextureKind kind, crd::u32 width, crd::u32 height, crd::u32 depth_or_layers, const void* rgba) = 0;

    // B2-d: clear + draw with `count` textures bound as a BINDLESS descriptor ARRAY at set 0 / binding 3 (t3 on DX12). The
    // FS samples an element chosen by a DYNAMIC per-fragment index (`KOp::SampleIndexed`, authored on a `texture(..., N)`
    // array binding). `count` must be ≤ 8 (the array capacity). Falls back to a normal draw when bindless is unsupported.
    virtual void draw_bindless(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                               ITexture* const* textures, crd::u32 count, crd::u32 vertex_count) = 0;

    // B2-d: true iff dynamic/non-uniform texture-array indexing (bindless) is usable (Vulkan: the descriptor-indexing
    // feature; DX12: resource-binding tier 2+, effectively always). draw_bindless still runs when false (uniform index only).
    [[nodiscard]] virtual bool supports_bindless() const noexcept = 0;

    // B16: like draw_bindless, but into a `create_color_depth_target` with a DEPTH TEST (clear to `clear_depth`, `compare` op) —
    // for DEPTH-OCCLUDED displaced geometry: the vertex-displaced ocean grid, where a near wave must hide the trough behind it.
    // The bound cascade textures are VERTEX+FRAGMENT visible, so the VS samples the FFT DISPLACEMENT to move each grid vertex.
    // Default (backends without the override) falls back to a depthless draw_bindless.
    virtual void draw_bindless_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear, float clear_depth,
                                     DepthCompare compare, ITexture* const* textures, crd::u32 count, crd::u32 vertex_count)
    {
        (void)clear_depth;
        (void)compare;
        draw_bindless(target, program, clear, textures, count, vertex_count);
    }

    // B4: create a MESH+FRAGMENT program (the modern amplification path — the mesh shader generates geometry directly, no vertex
    // input). Default (no mesh-shader support) ⇒ nullptr, so the caller falls back to the vertex-pull path. WebGPU has no mesh
    // shaders, so this stays backend-optional.
    [[nodiscard]] virtual std::unique_ptr<IRasterProgram> create_mesh_program(IGpuProgram& /*mesh*/, IGpuProgram& /*fragment*/)
    {
        return nullptr;
    }

    // B4: clear `target` and dispatch `group_count` mesh workgroups (a mesh program from create_mesh_program). Colour-only —
    // the mesh-shader proof. Default (backends without mesh shaders) ⇒ no-op. Result host-readable via `read_pixel`.
    virtual void draw_mesh(IRasterTarget& /*target*/, IRasterProgram& /*program*/, ClearColor /*clear*/, crd::u32 /*group_count*/) {}

    // B4: like draw_bindless_depth, but the geometry comes from a MESH program — `group_count` meshlet workgroups instead of a
    // vertex count. The bindless cascade textures are bound for the mesh shader (it samples the FFT displacement). The ocean
    // fast path. Default (no mesh shaders) ⇒ no-op; the caller uses draw_bindless_depth (vertex-pull) instead.
    virtual void draw_mesh_bindless_depth(IRasterTarget& /*target*/, IRasterProgram& /*program*/, ClearColor /*clear*/,
                                          float /*clear_depth*/, DepthCompare /*compare*/, ITexture* const* /*textures*/,
                                          crd::u32 /*count*/, crd::u32 /*group_count*/) {}

    // --- B5: deferred G-buffer (MRT) — a material writes its OpenPBR surface to N colour attachments -------------------

    // A G-BUFFER render target: `attachments` RGBA8 colour attachments (2..8), each host-readable per attachment. The
    // substrate for a deferred material (`ckir::material::pack_gbuffer` → N `KEntry` colour outputs). nullptr on failure.
    [[nodiscard]] virtual std::unique_ptr<class IGBufferTarget>
    create_gbuffer_target(crd::u32 width, crd::u32 height, crd::u32 attachments) = 0;

    // Clear all attachments to `clear`, then draw `program` (a surface material with N colour outputs) into the G-buffer via
    // MRT. Each attachment is host-readable via `IGBufferTarget::read_pixel(attachment, x, y)`. Synchronous.
    virtual void draw_gbuffer(IGBufferTarget& target, IRasterProgram& program, ClearColor clear, crd::u32 vertex_count) = 0;

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

    // B4-tess: draw `patch_count` QUAD patches through the tessellator (PATCH_LIST + patch size 4). Default ⇒ no-op.
    virtual void draw_tess(IRasterTarget& /*target*/, IRasterProgram& /*program*/, ClearColor /*clear*/,
                           crd::u32 /*patch_count*/) {}

    // B4-vis-4: a R32_UINT VISIBILITY-BUFFER target — the HW-raster half of the Nanite split (HW raster wins on big triangles).
    // The fragment shader writes a per-pixel primitive id (KBuiltin::PrimitiveId → SV_PrimitiveId / gl_PrimitiveID), which a
    // deferred pass materializes. `read_pixel` returns the raw u32 id. Default (no uint-target support) ⇒ nullptr.
    [[nodiscard]] virtual std::unique_ptr<IRasterTarget> create_visbuffer_target(crd::u32 /*width*/, crd::u32 /*height*/)
    {
        return nullptr;
    }

    // B4-vis-4: HW-raster a VS→FS program into a visibility-buffer target, clearing the id to `clear_id`. Default ⇒ no-op.
    virtual void draw_visbuffer(IRasterTarget& /*target*/, IRasterProgram& /*program*/, crd::u32 /*clear_id*/,
                                crd::u32 /*vertex_count*/) {}

    // B4: GPU-DRIVEN INDIRECT MESHLET DISPATCH — the mesh-workgroup count comes from `native_args` (the backend-native handle
    // of a buffer a compute CULL pass wrote as {groupCountX, 1, 1}; `ComputeBuffer::native_handle()`), consumed by
    // vkCmdDrawMeshTasksIndirectEXT / DX12 ExecuteIndirect(DISPATCH_MESH). The culled meshlets never dispatch and the CPU
    // never learns the count — the Nanite scale loop. Colour-only. Default (no indirect support) ⇒ no-op.
    virtual void draw_mesh_indirect(IRasterTarget& /*target*/, IRasterProgram& /*program*/, ClearColor /*clear*/,
                                    void* /*native_args*/, crd::u64 /*args_offset*/) {}

    // B4: dispatch a MESH program with PER-PRIMITIVE VRS — the mesh's `KEntry::shading_rate` output (a distant/low-detail
    // meshlet shading itself at a coarser fragment rate — `gl_MeshPrimitivesEXT[].gl_PrimitiveShadingRateEXT` / SV_ShadingRate)
    // drives the coarse rate via a REPLACE combiner. Colour-only. Default (no VRS support) ⇒ falls back to a full-rate draw.
    virtual void draw_mesh_vrs(IRasterTarget& /*target*/, IRasterProgram& /*program*/, ClearColor /*clear*/,
                               crd::u32 /*group_count*/) {}

    // --- B17-a: WEIGHTED-BLENDED ORDER-INDEPENDENT TRANSPARENCY (WBOIT, McGuire-Bavoil 2013) -----------------------------
    //
    // The cheap single-pass OIT tier: transparency composited WITHOUT a depth sort. Two internal FLOAT render targets are
    // created per call: an ACCUMULATION buffer (RGBA16F) and a REVEALAGE buffer (R16F). `transparent` is a VS+FS program
    // whose FS emits TWO colour attachments — location 0 = the weighted premultiplied colour `vec4(rgb*a*w, a*w)` (blended
    // ADDITIVELY: `Σ`), location 1 = the coverage `a` (blended MULTIPLICATIVELY into revealage: `Π(1-a)`), where `w` is the
    // depth weight. `vertex_count` transparent-triangle vertices are accumulated in ONE pass, ANY draw order (the OIT
    // property). Then `composite` (a full-screen VS+FS, `vertex_count`=3) samples accum at bindless index 0 and revealage at
    // index 1 and resolves `rgb = accum.rgb/max(accum.a, eps)`, output `(rgb, revealage)`, blended over a `background`-cleared
    // `target` with `(ONE_MINUS_SRC_ALPHA, SRC_ALPHA)` ⇒ `rgb·(1-reveal) + background·reveal`. `target` (RGBA8) is
    // host-readable after. Default (no float-target / blend-equation support) ⇒ no-op. Appended at END (vtable-stable).
    virtual void draw_wboit(IRasterTarget& /*target*/, IRasterProgram& /*transparent*/, IRasterProgram& /*composite*/,
                            ClearColor /*background*/, crd::u32 /*vertex_count*/) {}

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

    // GEO-7 (D-007 row 72): the SCENE-GEOMETRY draw — `draw_storage`'s vertex-pulling seam WITH a real depth pass:
    // clear colour to `clear` and depth to `clear_depth`, then draw `vertex_count` vertices with the depth test at
    // `compare` (depth WRITE on — this is the scene pass overlays later test against). `storage` binds at set 0 /
    // binding 0, VERTEX+FRAGMENT visible (the GEO-1 pulling path: the VS fetches indices/vertices/instances by
    // `VertexIndex`). Target must come from `create_color_depth_target`. Default (backends without the override)
    // falls back to the DEPTHLESS draw_storage — the draw_bindless_depth precedent. Appended at END (vtable-stable).
    virtual void draw_storage_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                                    float /*clear_depth*/, DepthCompare /*compare*/, IStorageBuffer& storage,
                                    crd::u32 vertex_count)
    {
        draw_storage(target, program, clear, storage, vertex_count);
    }

    // GEO-8 showcase fix: the CONTINUING scene-geometry draw — draw_storage_depth WITHOUT the clear: colour and
    // depth both loadOp=LOAD, depth test at `compare` with WRITE ON, so a multi-group scene composes correctly
    // (group N occludes/is occluded by groups 0..N-1 through the REAL depth buffer). The frame's FIRST scene draw
    // uses draw_storage_depth (the clear); every subsequent group uses this. Target must have been drawn at least
    // once this frame. Default (backends without the override) falls back to the CLEARING variant — last-drawn
    // wins, visibly wrong but never silently absent. Appended at END (vtable-stable).
    virtual void draw_storage_depth_load(IRasterTarget& target, IRasterProgram& program, DepthCompare compare,
                                         IStorageBuffer& storage, crd::u32 vertex_count)
    {
        draw_storage_depth(target, program, ClearColor{}, 0.0F, compare, storage, vertex_count);
    }

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
    // ── REN-38-A2: DISPATCH A COMPUTE KERNEL INSIDE THE FRAME GRAPH. ─────────────────────────────────────────
    // ⛔ `FramePassKind::Compute` was DECLARED BUT NOT IMPLEMENTED: the executor's `record_pass` fell through to
    // `break`, so a compute pass in an authored asset VALIDATED, COOKED, RAN and rendered NOTHING. Every check
    // passed and the frame was silently wrong — the worst shape a defect can take in this system.
    //
    // Compute lives in `IComputeContext`, which owns its own command buffer and submits independently. That is
    // exactly what a graph pass must NOT do, so the dispatch is a RASTER-CONTEXT verb: it records into the
    // FRAME's one command buffer, between the raster passes, ordered and barriered by the same declared
    // reads/writes as everything else. One submission still means one submission.
    //
    // `buffers` are the kernel's storage bindings at set 0, bindings 0..n-1 — the graph resolves them from the
    // pass's declared reads and writes, so a kernel never names a slot. Appended at the vtable END (D135); the
    // default is a no-op so a backend without it fails VISIBLY rather than half-running a technique.
    virtual void dispatch_kernel(IGpuProgram& /*kernel*/, crd::u32 /*groups_x*/, crd::u32 /*groups_y*/,
                                 crd::u32 /*groups_z*/, IStorageBuffer* const* /*buffers*/, crd::u32 /*count*/)
    {
    }

    virtual void draw_storage_mrt(IRasterTarget* const* /*targets*/, crd::u32 /*count*/, IRasterProgram& /*program*/,
                                  ClearColor /*clear*/, float /*clear_depth*/, DepthCompare /*compare*/,
                                  IStorageBuffer& /*storage*/, crd::u32 /*vertex_count*/,
                                  const BlendMode* /*blend*/ = nullptr)
    {
    }

    // RET-6 (ADR-0105): the OVERLAY draw — compose instanced primitives ONTO an existing target: color loadOp=LOAD
    // (the previous contents STAY — never cleared), standard alpha blending (srcAlpha · 1−srcAlpha), and a READ-ONLY
    // depth test at `compare` when the target carries a depth buffer (depth writes are never enabled; on a depthless
    // target `compare` is ignored). `storage` binds at set 0 / binding 0, VERTEX+FRAGMENT visible — the VS pulls
    // per-instance records by `VertexIndex` (the GEO-1 vertex-pulling seam), which is what lets ONE program + dynamic
    // state replace the retiring rhi renderer's six PSOs. The target must have been drawn at least once (its contents
    // are what the overlay composites over). Multiple overlay draws CHAIN — each composites over the last, the
    // debug-draw variant order (Test → Always → GreaterDimmed). Returns false when refused (invalid target/program,
    // an MSAA target, or a backend without the capability) — refusal over a silent wrong draw. Appended at END.
    [[nodiscard]] virtual bool draw_overlay(IRasterTarget& /*target*/, IRasterProgram& /*program*/,
                                            IStorageBuffer& /*storage*/, DepthCompare /*compare*/,
                                            crd::u32 /*vertex_count*/) { return false; }

    // REN-2 (D-007 row 99) Half B: the TEXTURED forward scene draw — draw_storage_depth PLUS a sampled material
    // texture. `storage` binds at set 0 / binding 0 (the VS vertex-pulls position+UV by VertexIndex, the GEO-1 seam);
    // `texture` binds at binding 1 + the default sampler at binding 2 (draw_textured's layout), so the FS samples the
    // material's base-color (albedo) map at the pulled UV instead of a flat colour. Cleared colour+depth, depth test
    // at `compare` with WRITE on (a multi-group scene composes through the real depth buffer, like draw_storage_depth).
    // Records into the frame graph in recording mode. Default (backends without the override) DROPS the texture and
    // falls back to draw_storage_depth — flat-coloured but never absent. Appended at END (vtable-stable).
    virtual void draw_storage_textured_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                                             float /*clear_depth*/, DepthCompare compare, IStorageBuffer& storage,
                                             class ITexture& /*texture*/, crd::u32 vertex_count)
    {
        draw_storage_depth(target, program, clear, 0.0F, compare, storage, vertex_count);
    }

    // The depth-LOAD companion (the CONTINUING textured scene draw — no clear; colour+depth persist), for group N>0.
    virtual void draw_storage_textured_depth_load(IRasterTarget& target, IRasterProgram& program, DepthCompare compare,
                                                  IStorageBuffer& storage, class ITexture& /*texture*/,
                                                  crd::u32 vertex_count)
    {
        draw_storage_depth_load(target, program, compare, storage, vertex_count);
    }

    // ── REN-3.1 (D-007 row 100): the DEPTH-ONLY pass — the shadow-map substrate. ────────────────────────────────
    // Renders `storage`-pulled geometry writing ONLY depth: no colour attachment is bound at all (Vulkan
    // `vkCmdBeginRendering` with colorAttachmentCount = 0 + pDepthAttachment; DX12 `OMSetRenderTargets(0, nullptr,
    // FALSE, &dsv)`). `target` supplies the depth attachment — for a shadow pass that is a frame-graph `D32Float`
    // transient declared `sampled`, which a LATER pass then reads through the COMPARISON sampler (`shadow_factor`).
    // This closes the gap `ckir_lighting.hpp` names: every shadow test until now bound a CPU-UPLOADED depth map
    // (`create_depth_texture`), because the device could not RENDER one.
    //
    // Depth is CLEARED to `clear_depth` and written with compare `compare` (a shadow map is a plain depth render:
    // clear to the far value, LessEqual/GreaterEqual per the projection's convention). Records into the frame graph
    // in recording mode, exactly like `draw_storage_depth`.
    //
    // ⛔ APPENDED AT END (vtable-stable, D135): inserting a pure-virtual mid-interface shifts every later slot and
    // silently dispatches to the wrong method under win-release LTCG. The default is a NO-OP rather than a colour
    // fallback — a backend without a depth-only path must produce NO shadow map, not a wrong one that reads as a
    // valid (all-lit) shadow term.
    virtual void draw_storage_depth_only(IRasterTarget& /*target*/, IRasterProgram& /*program*/,
                                         float /*clear_depth*/, DepthCompare /*compare*/,
                                         IStorageBuffer& /*storage*/, crd::u32 /*vertex_count*/)
    {
    }

    // The depth-LOAD companion — the CONTINUING depth-only draw (no clear; the map so far persists), for mesh N>0
    // of a shadow pass. ⛔ WITHOUT THIS A MULTI-MESH SHADOW PASS IS BROKEN: every `draw_storage_depth_only` clears,
    // so drawing a second occluder would WIPE the first one's depth and the shadow map would contain only the last
    // mesh. Found by the REN-3.1 bench (the depth arm did N clears while the colour arm did 1 clear + N-1 loads,
    // which showed up as depth-only being *slower* on DX12 — a measurement artefact that turned out to be pointing
    // at a real missing API). Same clear/load split `draw_storage_depth` / `draw_storage_depth_load` already has.
    // Appended at END (vtable-stable, D135).
    virtual void draw_storage_depth_only_load(IRasterTarget& /*target*/, IRasterProgram& /*program*/,
                                              DepthCompare /*compare*/, IStorageBuffer& /*storage*/,
                                              crd::u32 /*vertex_count*/)
    {
    }

    // ── REN-3.2-b: the SHADOWED scene draw. Appended at the END of the vtable (D135). ──
    // Identical to `draw_storage_textured_depth` in every respect but ONE: the sampler bound at slot 2 is the
    // COMPARISON sampler, not the filtering one. The descriptor layout (storage 0 · sampled image 1 · sampler 2)
    // is unchanged, which is why this needs no new set layout on either backend — a shadow lookup is a normal
    // texture read whose sampler happens to compare instead of filter.
    // `shadow_atlas` is the layered depth atlas from REN-3.2-a; the FS selects its cascade via the layer coord.
    // ⛔ Choosing the comparison sampler from the CALL rather than from the texture keeps it impossible to
    // sample a shadow map with a filtering sampler by accident — the two draws are different entry points.
    // The default drops the atlas and draws unshadowed, so a backend that has not implemented it renders a
    // correct-but-unshadowed image rather than nothing.
    virtual void draw_storage_shadowed_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                                             float /*clear_depth*/, DepthCompare compare, IStorageBuffer& storage,
                                             class ITexture& /*shadow_atlas*/, crd::u32 vertex_count)
    {
        draw_storage_depth(target, program, clear, 0.0F, compare, storage, vertex_count);
    }
    virtual void draw_storage_shadowed_depth_load(IRasterTarget& target, IRasterProgram& program,
                                                  DepthCompare compare, IStorageBuffer& storage,
                                                  class ITexture& /*shadow_atlas*/, crd::u32 vertex_count)
    {
        draw_storage_depth_load(target, program, compare, storage, vertex_count);
    }

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

    // Exact copy. `dst` and `src` must have the SAME extent and format; a mismatch is a NO-OP, never a partial
    // copy — a half-copied target is indistinguishable from a correctly copied one in a readback of the copied
    // region, which is exactly the kind of result this engine refuses to produce.
    virtual void copy_image(IRasterTarget& /*dst*/, IRasterTarget& /*src*/) {}

    // Rescaling copy. `src`'s full extent maps to `dst`'s full extent.
    // ⛔ DX12's copy engine has NO blit: D3D12 offers CopyResource/CopyTextureRegion (1:1 only) and
    // ResolveSubresource (MSAA only). The DX12 implementation therefore rescales through a fullscreen DRAW, and
    // that asymmetry is REAL, not hidden — it is why `blit_image` is its own verb rather than `copy_image` with
    // an extent argument.
    virtual void blit_image(IRasterTarget& /*dst*/, IRasterTarget& /*src*/, BlitFilter /*filter*/) {}

    // MSAA resolve: average `src`'s samples into single-sample `dst`. Same extent and a resolve-compatible
    // format on both sides.
    virtual void resolve_image(IRasterTarget& /*dst*/, IRasterTarget& /*src*/) {}

    // ── ⭐ REN-38-A7 / A8: the CONTINUING tessellation and mesh draws. Appended at the END (D135). ──
    // `draw_tess` and `draw_mesh` both CLEAR. That is right for the single-draw proof they were written for and
    // WRONG for a pass that iterates a draw list: ⛔ every draw after the first would wipe the ones before it, so
    // a scene with three tessellated meshes would render exactly ONE — the last — and look entirely plausible.
    // This is the multi-pass load-not-clear scar in its tessellation/mesh form, and it is why an authored
    // `raster.tess` / `raster.mesh` pass needs a continuing verb before it can iterate anything.
    //
    // Colour and depth LOAD (previous contents kept); otherwise identical to the clearing verb. The FIRST draw of
    // a pass uses the clearing form, every later one uses this.
    virtual void draw_tess_load(IRasterTarget& /*target*/, IRasterProgram& /*program*/, crd::u32 /*patch_count*/) {}
    virtual void draw_mesh_load(IRasterTarget& /*target*/, IRasterProgram& /*program*/, crd::u32 /*group_count*/) {}

    // ── ⭐ REN-38-A9: RAY TRACING INSIDE THE FRAME. Appended at the END of the vtable (D135). ──
    // ⛔ The ray-tracing contexts (`VulkanRayTracingContext` / `Dx12RayTracingContext`) are OFFLINE rigs: every
    // one of their verbs creates its own buffers, its own descriptor pool, its own pipeline, then submits AND
    // WAITS. That is right for an oracle comparison and impossible inside a frame — it is the universal port
    // defect this band keeps meeting, in its most extreme form (the verb owns the whole submission, not merely
    // the descriptor pool). So an authored ray-tracing PASS cannot call them.
    //
    // What it calls instead is this: an INLINE RAY QUERY dispatch, recorded into the frame's command buffer like
    // any other kernel. `VK_KHR_ray_query` / DXR-1.1 inline `RayQuery<>` need exactly two things a compute
    // dispatch does not already have — the TLAS bound as a descriptor, and a shader that declares it — and CKIR
    // already emits both (`KOp::AccelStructDecl` → `accelerationStructureEXT` / `RaytracingAccelerationStructure`).
    // No shader binding table, no ray-tracing pipeline, one submission.
    //
    // ⛔ BINDING CONVENTION, stated here because it is the only place both backends can read it: the TLAS is at
    // set 0 / binding 0 (t0 on DX12) and the pass's storage buffers follow at bindings 1..N (u1..uN). It matches
    // `VulkanRayTracingContext::trace_dispatch` exactly, so a kernel written for the offline rig runs unchanged
    // inside a frame — which is the whole point of having one convention rather than two.
    virtual void dispatch_kernel_rt(IGpuProgram& /*kernel*/, class IAccelerationStructure& /*as*/, crd::u32 /*groups_x*/,
                                    crd::u32 /*groups_y*/, crd::u32 /*groups_z*/, IStorageBuffer* const* /*buffers*/,
                                    crd::u32 /*count*/)
    {
    }

    // ── ⭐ REN-38-A10: GPU-DRIVEN DISPATCH. Appended at the END of the vtable (D135). ──
    // The workgroup count comes from `args` (a buffer some earlier pass WROTE as {x, y, z}) rather than from the
    // CPU. ⛔ That is the whole point and the reason it needs its own verb: with `dispatch_kernel` the count is a
    // parameter the CPU had to know, so a cull pass could never actually decide how much work followed it — the
    // GPU-driven loop was expressible in the device (`dispatch_indirect` exists on the compute context) and NOT
    // inside a frame graph, which is where a cull pass lives.
    virtual void dispatch_kernel_indirect(IGpuProgram& /*kernel*/, IStorageBuffer& /*args*/, crd::u64 /*args_offset*/,
                                          IStorageBuffer* const* /*buffers*/, crd::u32 /*count*/)
    {
    }

    // REN-38-A10: the mesh half of the same loop — `draw_mesh_indirect` against a graph-tracked buffer rather
    // than a raw backend handle, so an authored pass can name the args buffer a compute pass just wrote.
    virtual void draw_mesh_indirect_buffer(IRasterTarget& /*target*/, IRasterProgram& /*program*/,
                                           ClearColor /*clear*/, IStorageBuffer& /*args*/, crd::u64 /*args_offset*/)
    {
    }

    // ── ⭐ REN-38-A11: the CONTINUING visibility-buffer draw. Appended at the END (D135). ──
    // ⛔ Same rule as `draw_tess_load` / `draw_mesh_load`, and it matters MORE here: a visibility buffer's whole
    // purpose is that ONE image holds the ids of EVERY visible primitive. A pass that cleared per draw would keep
    // only the LAST mesh's ids and the deferred materialisation would shade one object over a background of
    // "nothing" — a picture that looks like aggressive culling rather than a bug.
    virtual void draw_visbuffer_load(IRasterTarget& /*target*/, IRasterProgram& /*program*/,
                                     crd::u32 /*vertex_count*/) {}

    // ── ⭐ REN-38-A12: the COMPOSITE draw — fullscreen, N bindless textures, LOAD (not clear), BLENDED. ──
    // ⛔ This is the verb that makes ORDER-INDEPENDENT TRANSPARENCY authorable as two ordinary passes instead of
    // a bespoke `draw_wboit` that allocates its own images (the 38-A1f finding). WBOIT's resolve is by definition
    // `rgb·(1-reveal) + background·reveal`: it must READ what is already in the target and BLEND over it. Every
    // fullscreen verb before this CLEARED, so the background was gone before the composite ran and the "OIT
    // technique" could only ever be a single opaque layer.
    virtual void draw_bindless_blend_load(IRasterTarget& /*target*/, IRasterProgram& /*program*/,
                                          ITexture* const* /*textures*/, crd::u32 /*count*/,
                                          crd::u32 /*vertex_count*/, BlendMode /*blend*/) {}

    // ── ⭐ REN-38-A16: THE RAY-TRACING PIPELINE, inside the frame. Appended at the END (D135). ──
    // Distinct from 38-A9's inline ray query in the way that matters to the hardware: an inline query is an
    // ordinary dispatch that happens to traverse, while THIS builds a PIPELINE out of separate raygen / miss /
    // closest-hit programs and a SHADER BINDING TABLE the traversal hardware indexes into. That is what buys
    // per-geometry hit shaders — different materials answering the same ray differently — which an inline query
    // cannot express at all: it has ONE shader and must branch on everything itself.
    //
    // ⛔ SAME BINDING CONVENTION AS A9, deliberately: TLAS at set 0 / binding 0 (t0), the pass's buffers at 1..N
    // (u1..uN). A kernel and a raygen shader should not need two mental models of where their data is.
    //
    // ⛔ Recorded into the FRAME's command buffer — not `VulkanRayTracingContext::trace_rays_pipeline`, which
    // creates its own buffers, its own pool, its own pipeline and then SUBMITS AND WAITS. That verb is an offline
    // rig; this is a pass.
    virtual void trace_rays(IGpuProgram& /*raygen*/, IGpuProgram& /*miss*/, IGpuProgram& /*closest_hit*/,
                            class IAccelerationStructure& /*as*/, crd::u32 /*width*/, crd::u32 /*height*/,
                            IStorageBuffer* const* /*buffers*/, crd::u32 /*count*/)
    {
    }

    // Does this backend/adapter offer a ray-tracing PIPELINE (not merely inline ray query)? ⛔ Reported, so an
    // authored graph degrades by DECLARED CAPABILITY (REN-35's rule) instead of rendering a black frame.
    [[nodiscard]] virtual bool supports_rt_pipeline() const noexcept { return false; }
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
