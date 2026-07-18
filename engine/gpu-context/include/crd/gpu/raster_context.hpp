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
