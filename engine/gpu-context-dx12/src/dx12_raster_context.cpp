// dx12_raster_context.cpp — the D3D12 implementation of crd::gpu::IRasterContext (ADR-0103 / D-008 C4). The DX12 mirror
// of the Vulkan raster context (C1): a graphics (DIRECT) queue + offscreen RGBA8 render targets + a CLEAR with pixel
// readback. Raw D3D12, no crd-rhi. D3D12 has no dynamic-rendering equivalent to fuss over — a bare RTV + ClearRTV is the
// whole clear path; the texture→readback copy honours the 256-byte row-pitch alignment D3D12 demands (GetCopyableFootprints).
// The shader DRAW path (a graphics PSO from a VS+FS DXIL pair + DrawInstanced) appends in C4-b.

#include <crd/gpu/dx12_raster_context.hpp>

#include <crd/gpu/dx12_context.hpp> // Dx12GpuProgram::dxil() — the VS+FS bytecode a graphics PSO is built from (C4-b)
#include <crd/gpu/frame_graph.hpp>  // REN-1 pt-2 (D-007 row 98): the DX12 frame graph (Dx12FrameGraph + create_frame_graph)

#include <crd/containers/array.hpp> // REN-1 pt-2: the frame graph's node/pass/slot arrays (no std containers)
#include <crd/memory/allocator.hpp> // REN-1 pt-2: crd::memory::default_allocator() for those arrays

#include <crd/core/types.hpp>

#include <d3d12.h>
#include <dxgi1_5.h> // RET-2: IDXGIFactory2/IDXGISwapChain3 (present) + IDXGIFactory5 (the tearing capability probe)
#include <windows.h>
#include <wrl/client.h>

#include <cstring> // std::memcpy — the program owns a copy of the VS/FS DXIL to rebuild PSOs at other sample counts
#include <memory>

namespace crd::gpu
{

using Microsoft::WRL::ComPtr;

namespace
{
constexpr DXGI_FORMAT kColorFormat  = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr UINT        kBindlessMax  = 8; // B2-d: bindless texture-array capacity (heap slots 2.. + the t3 SRV table)

// A host-visible READBACK buffer (the render target is copied here so the CPU can read pixels back).
ComPtr<ID3D12Resource> make_readback_buffer(ID3D12Device* dev, UINT64 size)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = size;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> res;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                 IID_PPV_ARGS(&res));
    return res;
}

// B1-e: a host-visible UPLOAD buffer (staging source to fill the VRS shading-rate image, which has no clear op).
ComPtr<ID3D12Resource> make_upload_buffer(ID3D12Device* dev, UINT64 size)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = size;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> res;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                 IID_PPV_ARGS(&res));
    return res;
}

// Build the attributeless graphics PSO (empty root sig, cull-none, RGBA8 RTV) at a given sample count + depth config. A
// D3D12 PSO BAKES SampleDesc.Count AND the depth-stencil state + DSVFormat (unlike Vulkan shader objects, where these are
// dynamic), so a draw needs a PSO matching the target's samples AND depth compare — hence both are parameters and
// Dx12RasterProgram caches a PSO per (samples, depth) key. `dsv == DXGI_FORMAT_UNKNOWN` ⇒ depth disabled (the B1-d off path).
ComPtr<ID3D12PipelineState> build_graphics_pso(ID3D12Device* dev, ID3D12RootSignature* root, D3D12_SHADER_BYTECODE vs,
                                               D3D12_SHADER_BYTECODE fs, UINT samples, DXGI_FORMAT dsv,
                                               D3D12_COMPARISON_FUNC depth_func, bool conservative, UINT num_rts = 1,
                                               D3D12_SHADER_BYTECODE hs = D3D12_SHADER_BYTECODE{},  // B4-tess: hull (empty = none)
                                               D3D12_SHADER_BYTECODE ds = D3D12_SHADER_BYTECODE{},  // B4-tess: domain
                                               DXGI_FORMAT rt_fmt = kColorFormat)                   // B4-vis-4: R32_UINT vis buffer
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature                                   = root;
    pd.VS                                               = vs;
    pd.PS                                               = fs;
    pd.HS                                               = hs; // B4-tess: hull + domain when present ⇒ a tessellation PSO
    pd.DS                                               = ds;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; // no blend, write RGBA
    pd.SampleMask                                       = 0xFFFFFFFFU;
    pd.RasterizerState.FillMode                         = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode                         = D3D12_CULL_MODE_NONE; // attributeless triangle: winding moot
    pd.RasterizerState.DepthClipEnable                  = TRUE;
    // B1-f: conservative rasterization (a PSO-baked raster state on DX12 — no dynamic-state equivalent). Both Overestimate
    // and Underestimate map to the single D3D12 ON mode; Underestimate is realized in the FS by reading SV_InnerCoverage.
    if (conservative) { pd.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON; }
    // B4-tess: a hull/domain pair rasterizes PATCHES (the IA feeds control-point patches; the tessellator generates triangles).
    pd.PrimitiveTopologyType                            = (hs.pShaderBytecode != nullptr) ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH
                                                                                          : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets                                 = num_rts; // B5: >1 for a deferred G-buffer (MRT)
    for (UINT i = 0; i < num_rts; ++i) { pd.RTVFormats[i] = rt_fmt; } // B4-vis-4: R32_UINT for the visibility buffer, else RGBA8
    pd.SampleDesc.Count                                 = samples;
    pd.DSVFormat                                        = dsv;
    if (dsv != DXGI_FORMAT_UNKNOWN) // B1-d: depth test + write ON with the given compare func
    {
        pd.DepthStencilState.DepthEnable    = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc      = depth_func;
    }
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) { return nullptr; }
    return pso;
}

// B17-a: the WBOIT ACCUMULATION PSO — two render targets with INDEPENDENT blend: RT0 (RGBA16F accum) additive `Σ`
// (ONE,ONE,ADD); RT1 (R16F revealage) multiplicative `Π(1-a)` (ZERO, INV_SRC_COLOR, ADD). No depth. The transparent VS+PS
// (whose FS emits BOTH attachments) draw ANY order — the accumulation is commutative.
inline ComPtr<ID3D12PipelineState> build_wboit_accum_pso(ID3D12Device* dev, ID3D12RootSignature* root,
                                                         D3D12_SHADER_BYTECODE vs, D3D12_SHADER_BYTECODE ps)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature                    = root;
    pd.VS                                = vs;
    pd.PS                                = ps;
    pd.BlendState.IndependentBlendEnable = TRUE; // per-RT blend equations
    auto& b0                             = pd.BlendState.RenderTarget[0];
    b0.BlendEnable                       = TRUE;
    b0.SrcBlend = D3D12_BLEND_ONE;  b0.DestBlend = D3D12_BLEND_ONE;  b0.BlendOp = D3D12_BLEND_OP_ADD;
    b0.SrcBlendAlpha = D3D12_BLEND_ONE; b0.DestBlendAlpha = D3D12_BLEND_ONE; b0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    auto& b1                 = pd.BlendState.RenderTarget[1];
    b1.BlendEnable           = TRUE;
    b1.SrcBlend = D3D12_BLEND_ZERO; b1.DestBlend = D3D12_BLEND_INV_SRC_COLOR; b1.BlendOp = D3D12_BLEND_OP_ADD;
    b1.SrcBlendAlpha = D3D12_BLEND_ZERO; b1.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; b1.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b1.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask                      = 0xFFFFFFFFU;
    pd.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType           = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets                = 2;
    pd.RTVFormats[0]                   = DXGI_FORMAT_R16G16B16A16_FLOAT; // accum
    pd.RTVFormats[1]                   = DXGI_FORMAT_R16_FLOAT;          // revealage
    pd.SampleDesc.Count                = 1;
    pd.DSVFormat                       = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) { return nullptr; }
    return pso;
}

// B17-a: the WBOIT COMPOSITE PSO — one RGBA8 target, `avg·(1-reveal) + background·reveal` via `(INV_SRC_ALPHA, SRC_ALPHA)`
// (the composite FS outputs `vec4(avg, reveal)`, and the target is pre-cleared to `background`).
inline ComPtr<ID3D12PipelineState> build_wboit_composite_pso(ID3D12Device* dev, ID3D12RootSignature* root,
                                                             D3D12_SHADER_BYTECODE vs, D3D12_SHADER_BYTECODE ps)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root;
    pd.VS             = vs;
    pd.PS             = ps;
    auto& b0          = pd.BlendState.RenderTarget[0];
    b0.BlendEnable    = TRUE;
    b0.SrcBlend = D3D12_BLEND_INV_SRC_ALPHA; b0.DestBlend = D3D12_BLEND_SRC_ALPHA; b0.BlendOp = D3D12_BLEND_OP_ADD;
    b0.SrcBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; b0.DestBlendAlpha = D3D12_BLEND_SRC_ALPHA; b0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b0.RenderTargetWriteMask           = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask                      = 0xFFFFFFFFU;
    pd.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType           = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets                = 1;
    pd.RTVFormats[0]                   = kColorFormat;
    pd.SampleDesc.Count                = 1;
    pd.DSVFormat                       = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) { return nullptr; }
    return pso;
}

// B4: a D3D12 pipeline-state STREAM subobject — {type enum, payload}, pointer-aligned so the next subobject starts aligned.
// (The backend has no CD3DX12 helpers, so the mesh-PSO stream is hand-rolled.) C4324 (padded-due-to-alignas) is the very
// layout this needs — the trailing pad is what makes the next subobject pointer-aligned — so it is suppressed by design.
#pragma warning(push)
#pragma warning(disable : 4324)
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Payload>
struct alignas(void*) PsoStreamSub
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    Payload                             value{};
};
#pragma warning(pop)

// B4: build a MESH pipeline state (MS + PS). Unlike a graphics PSO there is no VS/input-layout/IA-topology — a mesh shader
// GENERATES its topology — so it must be created via the SUBOBJECT-STREAM path (ID3D12Device2::CreatePipelineState), not
// CreateGraphicsPipelineState. Like the graphics PSO it BAKES SampleDesc + depth + RTV formats, so a program caches one per
// (samples, depth) key. `dsv == DXGI_FORMAT_UNKNOWN` ⇒ the colour-only path.
ComPtr<ID3D12PipelineState> build_mesh_pso(ID3D12Device2* dev2, ID3D12RootSignature* root, D3D12_SHADER_BYTECODE ms,
                                           D3D12_SHADER_BYTECODE ps, UINT samples, DXGI_FORMAT dsv,
                                           D3D12_COMPARISON_FUNC depth_func, UINT num_rts = 1,
                                           D3D12_SHADER_BYTECODE as = D3D12_SHADER_BYTECODE{}) // B4: AS = the task shader (empty = none)
{
    if (dev2 == nullptr) { return nullptr; }
    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode        = D3D12_FILL_MODE_SOLID;
    rast.CullMode        = D3D12_CULL_MODE_NONE; // match the graphics path (attributeless / mesh-generated winding moot)
    rast.DepthClipEnable = TRUE;
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RT_FORMAT_ARRAY rts{};
    rts.NumRenderTargets = num_rts;
    for (UINT i = 0; i < num_rts; ++i) { rts.RTFormats[i] = kColorFormat; }
    DXGI_SAMPLE_DESC sd{};
    sd.Count = samples;
    D3D12_DEPTH_STENCIL_DESC ds{};
    if (dsv != DXGI_FORMAT_UNKNOWN)
    {
        ds.DepthEnable    = TRUE;
        ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc      = depth_func;
    }

    struct MeshStream
    {
        PsoStreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*>        root_sig;
        PsoStreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE>                   as_bc; // B4: amplification (empty = none)
        PsoStreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE>                   ms_bc;
        PsoStreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE>                   ps_bc;
        PsoStreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC>           rast;
        PsoStreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC>                     blend;
        PsoStreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rts;
        PsoStreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT>           dsv_fmt;
        PsoStreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC>     ds;
        PsoStreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC>               sample;
    } stream;
    stream.root_sig.value = root;
    stream.as_bc.value    = as; // B4: the amplification (task) shader — empty D3D12_SHADER_BYTECODE for a plain mesh
    stream.ms_bc.value    = ms;
    stream.ps_bc.value    = ps;
    stream.rast.value     = rast;
    stream.blend.value    = blend;
    stream.rts.value      = rts;
    stream.dsv_fmt.value  = dsv;
    stream.ds.value       = ds;
    stream.sample.value   = sd;

    D3D12_PIPELINE_STATE_STREAM_DESC sdesc{};
    sdesc.SizeInBytes                   = sizeof(stream);
    sdesc.pPipelineStateSubobjectStream = &stream;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev2->CreatePipelineState(&sdesc, IID_PPV_ARGS(&pso)))) { return nullptr; }
    return pso;
}

// B1-d: the backend-neutral DepthCompare → D3D12_COMPARISON_FUNC (map explicitly, not by cast).
[[nodiscard]] D3D12_COMPARISON_FUNC to_d3d12_compare(DepthCompare c) noexcept
{
    switch (c)
    {
    case DepthCompare::Never:        return D3D12_COMPARISON_FUNC_NEVER;
    case DepthCompare::Less:         return D3D12_COMPARISON_FUNC_LESS;
    case DepthCompare::Equal:        return D3D12_COMPARISON_FUNC_EQUAL;
    case DepthCompare::LessEqual:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case DepthCompare::Greater:      return D3D12_COMPARISON_FUNC_GREATER;
    case DepthCompare::NotEqual:     return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case DepthCompare::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case DepthCompare::Always:       return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT; // B1-d depth buffer format

// B1-e: ShadingRate → D3D12_SHADING_RATE. The D3D12 enum values ARE the packed (Yshift<<2)|Xshift byte (2x2 = 0x5), the
// same encoding as gl_PrimitiveShadingRateEXT + the VRS image texel.
[[nodiscard]] D3D12_SHADING_RATE to_d3d12_rate(ShadingRate r) noexcept
{
    switch (r)
    {
    case ShadingRate::Rate1x2: return D3D12_SHADING_RATE_1X2;
    case ShadingRate::Rate2x1: return D3D12_SHADING_RATE_2X1;
    case ShadingRate::Rate2x2: return D3D12_SHADING_RATE_2X2;
    case ShadingRate::Rate2x4: return D3D12_SHADING_RATE_2X4;
    case ShadingRate::Rate4x2: return D3D12_SHADING_RATE_4X2;
    case ShadingRate::Rate4x4: return D3D12_SHADING_RATE_4X4;
    default:                   return D3D12_SHADING_RATE_1X1;
    }
}

// B1-e: ShadingRateCombiner → D3D12_SHADING_RATE_COMBINER (Keep=PASSTHROUGH, Replace=OVERRIDE, Mul≈SUM).
[[nodiscard]] D3D12_SHADING_RATE_COMBINER to_d3d12_combiner(ShadingRateCombiner c) noexcept
{
    switch (c)
    {
    case ShadingRateCombiner::Replace: return D3D12_SHADING_RATE_COMBINER_OVERRIDE;
    case ShadingRateCombiner::Min:     return D3D12_SHADING_RATE_COMBINER_MIN;
    case ShadingRateCombiner::Max:     return D3D12_SHADING_RATE_COMBINER_MAX;
    case ShadingRateCombiner::Mul:     return D3D12_SHADING_RATE_COMBINER_SUM;
    default:                           return D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
    }
}

// B1-e: ShadingRate → the packed byte written into every texel of the VRS image (== the D3D12_SHADING_RATE value).
[[nodiscard]] crd::u8 vrs_packed(ShadingRate r) noexcept { return static_cast<crd::u8>(to_d3d12_rate(r)); }
} // namespace

// An offscreen RGBA8 render target: a DEFAULT-heap texture + its RTV + a persistently-mapped READBACK buffer. `read_pixel`
// is valid after a context op copied the texture into the readback (accounting for the aligned row pitch).
class Dx12RasterTarget final : public IRasterTarget
{
public:
    Dx12RasterTarget(ComPtr<ID3D12Resource> tex, ComPtr<ID3D12Resource> resolve, ComPtr<ID3D12Resource> depth,
                     ComPtr<ID3D12Resource> readback, ComPtr<ID3D12DescriptorHeap> rtv_heap,
                     ComPtr<ID3D12DescriptorHeap> dsv_heap, void* mapped, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
                     crd::u32 samples, crd::u32 w, crd::u32 h) noexcept
        : m_tex(std::move(tex)), m_resolve(std::move(resolve)), m_depth(std::move(depth)),
          m_readback(std::move(readback)), m_rtv_heap(std::move(rtv_heap)), m_dsv_heap(std::move(dsv_heap)),
          m_mapped(mapped), m_fp(fp), m_samples(samples), m_w(w), m_h(h)
    {
    }
    ~Dx12RasterTarget() override
    {
        if (m_mapped != nullptr && m_readback != nullptr) { m_readback->Unmap(0, nullptr); }
    }
    Dx12RasterTarget(const Dx12RasterTarget&)            = delete;
    Dx12RasterTarget& operator=(const Dx12RasterTarget&) = delete;
    Dx12RasterTarget(Dx12RasterTarget&&)                 = delete;
    Dx12RasterTarget& operator=(Dx12RasterTarget&&)      = delete;

    [[nodiscard]] crd::u32 width() const noexcept override { return m_w; }
    [[nodiscard]] crd::u32 height() const noexcept override { return m_h; }
    [[nodiscard]] crd::u32 read_pixel(crd::u32 x, crd::u32 y) const noexcept override
    {
        if (m_mapped == nullptr || x >= m_w || y >= m_h) { return 0U; }
        const auto*      base   = static_cast<const crd::u8*>(m_mapped) + m_fp.Offset;
        const crd::usize pitch  = m_fp.Footprint.RowPitch; // 256-byte-aligned, so ≥ width*4
        const crd::usize offset = static_cast<crd::usize>(y) * pitch + static_cast<crd::usize>(x) * 4U;
        crd::u32         px     = 0U;
        for (int i = 0; i < 4; ++i) { px |= static_cast<crd::u32>(base[offset + static_cast<crd::usize>(i)]) << (8 * i); }
        return px; // little-endian RGBA8: R low byte
    }

    [[nodiscard]] ID3D12Resource*             tex() const noexcept { return m_tex.Get(); }       // colour RT (MSAA if >1)
    [[nodiscard]] ID3D12Resource*             resolve() const noexcept { return m_resolve.Get(); } // single-sample (MSAA)
    [[nodiscard]] ID3D12Resource*             readback() const noexcept { return m_readback.Get(); }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE rtv() const noexcept
    {
        return m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    }
    [[nodiscard]] const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint() const noexcept { return m_fp; }
    [[nodiscard]] crd::u32 samples() const noexcept { return m_samples; }
    [[nodiscard]] bool     multisampled() const noexcept { return m_samples > 1U; }
    // The subresource the readback is copied from: the resolved single-sample texture for MSAA, else the colour texture.
    [[nodiscard]] ID3D12Resource* copy_src() const noexcept { return m_samples > 1U ? m_resolve.Get() : m_tex.Get(); }
    [[nodiscard]] bool            has_depth() const noexcept { return m_depth != nullptr; } // B1-d
    [[nodiscard]] ID3D12Resource* depth_tex() const noexcept { return m_depth.Get(); }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE dsv() const noexcept
    {
        return m_dsv_heap->GetCPUDescriptorHandleForHeapStart();
    }
    [[nodiscard]] bool            has_vrs() const noexcept { return m_vrs != nullptr; } // B1-e attachment VRS image
    [[nodiscard]] ID3D12Resource* vrs_tex() const noexcept { return m_vrs.Get(); }
    void set_vrs(ComPtr<ID3D12Resource> vrs) noexcept { m_vrs = std::move(vrs); }

private:
    ComPtr<ID3D12Resource>              m_tex;
    ComPtr<ID3D12Resource>              m_resolve; // null for a single-sample target
    ComPtr<ID3D12Resource>              m_depth;   // B1-d: null unless created via create_color_depth_target
    ComPtr<ID3D12Resource>              m_vrs;     // B1-e: null unless created via create_color_vrs_target (R8_UINT rates)
    ComPtr<ID3D12Resource>              m_readback;
    ComPtr<ID3D12DescriptorHeap>        m_rtv_heap;
    ComPtr<ID3D12DescriptorHeap>        m_dsv_heap; // B1-d: null unless depth
    void*                               m_mapped = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT  m_fp{};
    crd::u32                            m_samples = 1;
    crd::u32                            m_w = 0;
    crd::u32                            m_h = 0;
};

// ── RET-2 (ADR-0105): the DX12 present surface — the DXGI mirror of the Vulkan sink design ────────────────────────────
// The app renders into a NORMAL color target (post-draw state: COMMON) and `present(target)` CopyResource-s the canvas
// into the current backbuffer, then Presents. Self-contained (own allocator/list/fence) and fully serialized per frame,
// exactly like the Vulkan surface. DXGI has no headless-surface equivalent — a real HWND is the one path here.
class Dx12PresentSurface final : public IPresentSurface
{
public:
    Dx12PresentSurface(ID3D12Device* device, ID3D12CommandQueue* queue, void* hwnd, crd::u32 w, crd::u32 h,
                       PresentMode mode) noexcept
        : m_queue(queue), m_mode(mode)
    {
        if (hwnd == nullptr) { return; } // DXGI presents to windows only — nullptr is the Vulkan headless path
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { return; }
        {
            ComPtr<IDXGIFactory5> f5; // tearing (true immediate mode) is a CAPABILITY — probe, never assume
            BOOL                  allow = FALSE;
            if (SUCCEEDED(factory.As(&f5))
                && SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            {
                m_tearing = allow == TRUE && mode == PresentMode::Immediate;
            }
        }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width            = w;
        sd.Height           = h;
        sd.Format           = kColorFormat; // matches the canvas ⇒ CopyResource is legal
        sd.SampleDesc.Count = 1;
        sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount      = 2;
        sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags            = m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(factory->CreateSwapChainForHwnd(queue, static_cast<HWND>(hwnd), &sd, nullptr, nullptr, &sc1)))
        {
            return;
        }
        if (FAILED(sc1.As(&m_swapchain))) { return; }

        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc)))) { return; }
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc.Get(), nullptr,
                                             IID_PPV_ARGS(&m_list))))
        {
            return;
        }
        m_list->Close();
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) { return; }
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_event == nullptr) { return; }
        m_w     = w;
        m_h     = h;
        m_valid = true;
    }

    ~Dx12PresentSurface() override
    {
        wait_gpu();
        if (m_event != nullptr) { CloseHandle(m_event); }
    }
    Dx12PresentSurface(const Dx12PresentSurface&)            = delete;
    Dx12PresentSurface& operator=(const Dx12PresentSurface&) = delete;
    Dx12PresentSurface(Dx12PresentSurface&&)                 = delete;
    Dx12PresentSurface& operator=(Dx12PresentSurface&&)      = delete;

    [[nodiscard]] bool     valid() const noexcept override { return m_valid; }
    [[nodiscard]] crd::u32 width() const noexcept override { return m_w; }
    [[nodiscard]] crd::u32 height() const noexcept override { return m_h; }
    [[nodiscard]] crd::u64 frame_count() const noexcept override { return m_frames; }

    [[nodiscard]] bool present(IRasterTarget& target) override
    {
        if (!m_valid) { return false; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        if (t.width() != m_w || t.height() != m_h) { return false; } // never a stretched half-frame

        const UINT             idx = m_swapchain->GetCurrentBackBufferIndex();
        ComPtr<ID3D12Resource> bb;
        if (FAILED(m_swapchain->GetBuffer(idx, IID_PPV_ARGS(&bb)))) { return false; }

        m_alloc->Reset();
        m_list->Reset(m_alloc.Get(), nullptr);
        barrier(bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        barrier(t.copy_src(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_list->CopyResource(bb.Get(), t.copy_src()); // same format + extent — the whole-resource copy
        barrier(t.copy_src(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        barrier(bb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        m_list->Close();
        ID3D12CommandList* lists[] = {m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);

        const UINT sync  = m_mode == PresentMode::Fifo ? 1U : 0U;
        const UINT flags = m_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0U;
        const HRESULT pr = m_swapchain->Present(sync, flags);
        wait_gpu(); // full per-frame serialization (v1 pacing — the frame graph takes over later)
        if (FAILED(pr)) { return false; }
        ++m_frames;
        return true;
    }

    [[nodiscard]] bool resize(crd::u32 width, crd::u32 height) override
    {
        if (!m_valid) { return false; }
        wait_gpu();
        const UINT flags = m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U;
        if (FAILED(m_swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags))) { return false; }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        if (FAILED(m_swapchain->GetDesc1(&sd))) { return false; }
        m_w = sd.Width; // a real window's swapchain follows the client area — report the truth
        m_h = sd.Height;
        return true;
    }

private:
    void barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter  = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_list->ResourceBarrier(1, &b);
    }
    void wait_gpu()
    {
        if (m_fence == nullptr || m_event == nullptr) { return; }
        const crd::u64 v = ++m_fence_value;
        m_queue->Signal(m_fence.Get(), v);
        if (m_fence->GetCompletedValue() < v)
        {
            m_fence->SetEventOnCompletion(v, m_event);
            WaitForSingleObject(m_event, INFINITE);
        }
    }

    ID3D12CommandQueue*               m_queue = nullptr;
    PresentMode                       m_mode  = PresentMode::Fifo;
    ComPtr<IDXGISwapChain3>           m_swapchain;
    ComPtr<ID3D12CommandAllocator>    m_alloc;
    ComPtr<ID3D12GraphicsCommandList> m_list;
    ComPtr<ID3D12Fence>               m_fence;
    HANDLE                            m_event       = nullptr;
    crd::u64                          m_fence_value = 0;
    crd::u32                          m_w           = 0;
    crd::u32                          m_h           = 0;
    crd::u64                          m_frames      = 0;
    bool                              m_tearing     = false;
    bool                              m_valid       = false;
};

// An assembled graphics program: a root signature + a graphics PSO (VS+FS), built once and drawn many. The DX12 analog of
// the Vulkan shader-object pair — a PSO here, not shader objects, but the same "linked VS+FS, drawn many times" role.
class Dx12RasterProgram final : public IRasterProgram
{
public:
    // The trailing `device2`/`is_mesh` default off ⇒ the graphics call sites are unchanged. For a MESH program (B4) `vs`/`fs`
    // carry the MS/PS DXIL and `pso_for` builds a stream PSO via `device2` instead of a graphics PSO.
    Dx12RasterProgram(ID3D12Device* device, ComPtr<ID3D12RootSignature> root, std::unique_ptr<crd::u8[]> vs,
                      crd::usize vs_size, std::unique_ptr<crd::u8[]> fs, crd::usize fs_size,
                      ComPtr<ID3D12PipelineState> pso1, ID3D12Device2* device2 = nullptr, bool is_mesh = false,
                      std::unique_ptr<crd::u8[]> as = nullptr, crd::usize as_size = 0,   // B4: AS (task) DXIL
                      std::unique_ptr<crd::u8[]> hs = nullptr, crd::usize hs_size = 0,   // B4-tess: HS (hull) DXIL
                      std::unique_ptr<crd::u8[]> ds = nullptr, crd::usize ds_size = 0) noexcept // B4-tess: DS (domain) DXIL
        : m_device(device), m_device2(device2), m_is_mesh(is_mesh), m_is_tess(hs != nullptr), m_root(std::move(root)),
          m_vs(std::move(vs)), m_vs_size(vs_size), m_fs(std::move(fs)), m_fs_size(fs_size), m_as(std::move(as)),
          m_as_size(as_size), m_hs(std::move(hs)), m_hs_size(hs_size), m_ds(std::move(ds)), m_ds_size(ds_size),
          m_pso1(std::move(pso1))
    {
    }
    ~Dx12RasterProgram() override                           = default;
    Dx12RasterProgram(const Dx12RasterProgram&)             = delete;
    Dx12RasterProgram& operator=(const Dx12RasterProgram&)  = delete;
    Dx12RasterProgram(Dx12RasterProgram&&)                  = delete;
    Dx12RasterProgram& operator=(Dx12RasterProgram&&)       = delete;

    [[nodiscard]] bool                 valid() const noexcept override { return m_pso1 != nullptr; }
    [[nodiscard]] bool                 is_mesh() const noexcept { return m_is_mesh; } // B4: DispatchMesh vs DrawInstanced
    [[nodiscard]] bool                 is_tess() const noexcept { return m_is_tess; } // B4-tess: PATCH-list DrawInstanced
    [[nodiscard]] ID3D12RootSignature* root() const noexcept { return m_root.Get(); }
    // B17-a: raw VS/PS bytecode + device — draw_wboit builds bespoke blend PSOs (per-RT additive/multiplicative) the cached
    // pso_for cannot express (it always bakes blend-off).
    [[nodiscard]] D3D12_SHADER_BYTECODE vs_bytecode() const noexcept { return {m_vs.get(), m_vs_size}; }
    [[nodiscard]] D3D12_SHADER_BYTECODE ps_bytecode() const noexcept { return {m_fs.get(), m_fs_size}; }
    [[nodiscard]] ID3D12Device*         device() const noexcept { return m_device; }

    // The PSO for a target of `samples` samples and depth/conservative config (a graphics PSO bakes ALL of them). The plain
    // 1×/no-depth/non-conservative PSO is prebuilt (also gates valid()); every other combo is built + cached lazily in a
    // small keyed cache (a handful of configs per program at most). `dsv == DXGI_FORMAT_UNKNOWN` ⇒ the depth-off colour path.
    [[nodiscard]] ID3D12PipelineState* pso_for(crd::u32 samples, DXGI_FORMAT dsv, D3D12_COMPARISON_FUNC depth_func,
                                               bool conservative, crd::u32 num_rts = 1U, DXGI_FORMAT rt_fmt = kColorFormat)
    {
        if (samples <= 1U && dsv == DXGI_FORMAT_UNKNOWN && !conservative && num_rts == 1U && rt_fmt == kColorFormat)
        {
            return m_pso1.Get();
        }
        const crd::u32 key = (samples << 8U)
                             | (dsv != DXGI_FORMAT_UNKNOWN ? (0x80U | static_cast<crd::u32>(depth_func)) : 0U)
                             | (conservative ? 0x10000U : 0U) | (num_rts << 20U) // B5: RT count in the key (MRT G-buffer)
                             | (rt_fmt == kColorFormat ? 0U : 0x40000U); // B4-vis-4: the R32_UINT visibility-buffer format
        for (int i = 0; i < m_cache_n; ++i) { if (m_cache[i].key == key) { return m_cache[i].pso.Get(); } }
        if (m_cache_n >= kPsoCacheCap) { return nullptr; }
        m_cache[m_cache_n].key = key;
        m_cache[m_cache_n].pso =
            m_is_mesh ? build_mesh_pso(m_device2, m_root.Get(), D3D12_SHADER_BYTECODE{m_vs.get(), m_vs_size},
                                       D3D12_SHADER_BYTECODE{m_fs.get(), m_fs_size}, samples, dsv, depth_func, num_rts,
                                       D3D12_SHADER_BYTECODE{m_as.get(), m_as_size}) // B4: AS (task) when present, else empty
                      : build_graphics_pso(m_device, m_root.Get(), D3D12_SHADER_BYTECODE{m_vs.get(), m_vs_size},
                                           D3D12_SHADER_BYTECODE{m_fs.get(), m_fs_size}, samples, dsv, depth_func,
                                           conservative, num_rts,
                                           D3D12_SHADER_BYTECODE{m_hs.get(), m_hs_size},   // B4-tess: hull (empty ⇒ non-tess)
                                           D3D12_SHADER_BYTECODE{m_ds.get(), m_ds_size},   // B4-tess: domain
                                           rt_fmt);                                        // B4-vis-4: RT format
        return m_cache[m_cache_n++].pso.Get();
    }

private:
    static constexpr int kPsoCacheCap = 8;
    struct PsoCacheEntry
    {
        crd::u32                    key = 0;
        ComPtr<ID3D12PipelineState> pso;
    };
    ID3D12Device*               m_device  = nullptr; // the context (which owns the device) outlives its programs
    ID3D12Device2*              m_device2 = nullptr; // B4: mesh stream-PSO device (null for graphics programs)
    bool                        m_is_mesh = false;   // B4: pso_for builds a mesh PSO instead of a graphics PSO
    bool                        m_is_tess = false;   // B4-tess: a VS+HS+DS+PS graphics PSO drawn as a control-point patch list
    ComPtr<ID3D12RootSignature> m_root;
    std::unique_ptr<crd::u8[]>  m_vs; // owned DXIL copies so a PSO can be rebuilt at any (samples, depth) config
    crd::usize                  m_vs_size = 0;
    std::unique_ptr<crd::u8[]>  m_fs;
    crd::usize                  m_fs_size = 0;
    std::unique_ptr<crd::u8[]>  m_as; // B4: owned AS (task) DXIL copy — empty for a plain mesh program
    crd::usize                  m_as_size = 0;
    std::unique_ptr<crd::u8[]>  m_hs; // B4-tess: owned HS (hull) DXIL copy — empty for a non-tess program
    crd::usize                  m_hs_size = 0;
    std::unique_ptr<crd::u8[]>  m_ds; // B4-tess: owned DS (domain) DXIL copy
    crd::usize                  m_ds_size = 0;
    ComPtr<ID3D12PipelineState> m_pso1;                // plain 1×/no-depth PSO (prebuilt; also gates valid())
    PsoCacheEntry               m_cache[kPsoCacheCap]; // lazily-built PSOs for MSAA / depth configs
    int                         m_cache_n = 0;
};

// B1-f: a fragment-shader storage buffer — a DEFAULT-heap UAV buffer (RWStructuredBuffer<uint> / RasterizerOrdered… on
// the shader side) + a persistently-mapped READBACK buffer the draw copies into so read_u32 sees the result on the CPU.
class Dx12StorageBuffer final : public IStorageBuffer
{
public:
    Dx12StorageBuffer(ComPtr<ID3D12Resource> buf, ComPtr<ID3D12Resource> readback, void* mapped, crd::u32 size) noexcept
        : m_buf(std::move(buf)), m_readback(std::move(readback)), m_mapped(mapped), m_size(size)
    {
    }
    ~Dx12StorageBuffer() override
    {
        if (m_mapped != nullptr && m_readback != nullptr) { m_readback->Unmap(0, nullptr); }
    }
    Dx12StorageBuffer(const Dx12StorageBuffer&)            = delete;
    Dx12StorageBuffer& operator=(const Dx12StorageBuffer&) = delete;
    Dx12StorageBuffer(Dx12StorageBuffer&&)                 = delete;
    Dx12StorageBuffer& operator=(Dx12StorageBuffer&&)      = delete;

    [[nodiscard]] crd::u32 size_bytes() const noexcept override { return m_size; }
    [[nodiscard]] crd::u32 read_u32(crd::u32 index) const noexcept override
    {
        if (m_mapped == nullptr || index >= m_size / 4U) { return 0U; }
        crd::u32 v = 0U;
        std::memcpy(&v, static_cast<const crd::u8*>(m_mapped) + static_cast<crd::usize>(index) * 4U, 4U);
        return v;
    }

    [[nodiscard]] ID3D12Resource* buf() const noexcept { return m_buf.Get(); }
    [[nodiscard]] ID3D12Resource* readback() const noexcept { return m_readback.Get(); }
    [[nodiscard]] crd::u32        num_elements() const noexcept { return m_size / 4U; }

private:
    ComPtr<ID3D12Resource> m_buf;
    ComPtr<ID3D12Resource> m_readback;
    void*                  m_mapped = nullptr;
    crd::u32               m_size   = 0;
};

// B2: a sampled texture — a DEFAULT-heap RGBA8 resource parked in PIXEL_SHADER_RESOURCE state. It carries the fully-formed
// SRV desc (its ViewDimension/format vary by kind: 2D colour · R32F depth · 1D/3D/Cube/Array), minted into the heap per draw.
class Dx12Texture final : public ITexture
{
public:
    Dx12Texture(ComPtr<ID3D12Resource> tex, crd::u32 w, crd::u32 h, const D3D12_SHADER_RESOURCE_VIEW_DESC& srv) noexcept
        : m_tex(std::move(tex)), m_srv(srv), m_w(w), m_h(h)
    {
    }
    ~Dx12Texture() override                    = default;
    Dx12Texture(const Dx12Texture&)            = delete;
    Dx12Texture& operator=(const Dx12Texture&) = delete;
    Dx12Texture(Dx12Texture&&)                 = delete;
    Dx12Texture& operator=(Dx12Texture&&)      = delete;

    [[nodiscard]] crd::u32                              width() const noexcept override { return m_w; }
    [[nodiscard]] crd::u32                              height() const noexcept override { return m_h; }
    [[nodiscard]] ID3D12Resource*                       tex() const noexcept { return m_tex.Get(); }
    [[nodiscard]] const D3D12_SHADER_RESOURCE_VIEW_DESC& srv() const noexcept { return m_srv; }

private:
    ComPtr<ID3D12Resource>          m_tex;
    D3D12_SHADER_RESOURCE_VIEW_DESC m_srv{};
    crd::u32                        m_w = 0;
    crd::u32                        m_h = 0;
};

inline constexpr crd::u32 kMaxGBuffer = 8U; // B5: max deferred G-buffer colour attachments

// B5: a deferred G-BUFFER — `n` RGBA8 render targets (one RTV heap) each with its own persistently-mapped readback buffer.
// The material writes all `n` in one MRT draw; `read_pixel(attachment, x, y)` reads a specific attachment (aligned pitch).
class Dx12GBufferTarget final : public IGBufferTarget
{
public:
    Dx12GBufferTarget(ComPtr<ID3D12Resource> (&tex)[kMaxGBuffer], ComPtr<ID3D12Resource> (&rb)[kMaxGBuffer],
                      void* (&mapped)[kMaxGBuffer], const D3D12_PLACED_SUBRESOURCE_FOOTPRINT (&fp)[kMaxGBuffer],
                      ComPtr<ID3D12DescriptorHeap> rtv_heap, UINT rtv_inc, crd::u32 n, crd::u32 w, crd::u32 h) noexcept
        : m_rtv_heap(std::move(rtv_heap)), m_rtv_inc(rtv_inc), m_n(n), m_w(w), m_h(h)
    {
        for (crd::u32 i = 0; i < n; ++i) { m_tex[i] = std::move(tex[i]); m_rb[i] = std::move(rb[i]); m_mapped[i] = mapped[i]; m_fp[i] = fp[i]; }
    }
    ~Dx12GBufferTarget() override
    {
        for (crd::u32 i = 0; i < m_n; ++i) { if (m_mapped[i] != nullptr && m_rb[i] != nullptr) { m_rb[i]->Unmap(0, nullptr); } }
    }
    Dx12GBufferTarget(const Dx12GBufferTarget&)            = delete;
    Dx12GBufferTarget& operator=(const Dx12GBufferTarget&) = delete;
    Dx12GBufferTarget(Dx12GBufferTarget&&)                 = delete;
    Dx12GBufferTarget& operator=(Dx12GBufferTarget&&)      = delete;

    [[nodiscard]] crd::u32 width() const noexcept override { return m_w; }
    [[nodiscard]] crd::u32 height() const noexcept override { return m_h; }
    [[nodiscard]] crd::u32 attachment_count() const noexcept override { return m_n; }
    [[nodiscard]] crd::u32 read_pixel(crd::u32 attachment, crd::u32 x, crd::u32 y) const noexcept override
    {
        if (attachment >= m_n || m_mapped[attachment] == nullptr || x >= m_w || y >= m_h) { return 0U; }
        const auto*      base   = static_cast<const crd::u8*>(m_mapped[attachment]) + m_fp[attachment].Offset;
        const crd::usize pitch  = m_fp[attachment].Footprint.RowPitch;
        const crd::usize offset = static_cast<crd::usize>(y) * pitch + static_cast<crd::usize>(x) * 4U;
        crd::u32         px     = 0U;
        for (int i = 0; i < 4; ++i) { px |= static_cast<crd::u32>(base[offset + static_cast<crd::usize>(i)]) << (8 * i); }
        return px;
    }
    [[nodiscard]] ID3D12Resource*                          tex(crd::u32 i) const noexcept { return m_tex[i].Get(); }
    [[nodiscard]] ID3D12Resource*                          readback(crd::u32 i) const noexcept { return m_rb[i].Get(); }
    [[nodiscard]] const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint(crd::u32 i) const noexcept { return m_fp[i]; }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE              rtv_start() const noexcept { return m_rtv_heap->GetCPUDescriptorHandleForHeapStart(); }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE rtv(crd::u32 i) const noexcept
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * m_rtv_inc;
        return h;
    }

private:
    ComPtr<ID3D12Resource>             m_tex[kMaxGBuffer];
    ComPtr<ID3D12Resource>             m_rb[kMaxGBuffer];
    void*                              m_mapped[kMaxGBuffer]{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_fp[kMaxGBuffer]{};
    ComPtr<ID3D12DescriptorHeap>       m_rtv_heap;
    UINT                               m_rtv_inc = 0;
    crd::u32                           m_n = 0;
    crd::u32                           m_w = 0;
    crd::u32                           m_h = 0;
};

class Dx12RasterContext final : public IRasterContext
{
public:
    Dx12RasterContext()
    {
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)))) { return; }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; // graphics queue (vs the compute context's COMPUTE queue)
        if (FAILED(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)))) { return; }
        // REN-8: the frame graph's allocator+list RING (see frame_rec_begin). Allocation failure is non-fatal —
        // `m_ring_list[0] == nullptr` simply means the graph records on the dedicated pair, exactly as before.
        for (crd::u32 s = 0; s < kFrameRingSize; ++s)
        {
            if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&m_ring_alloc[s])))
                || FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_ring_alloc[s].Get(),
                                                      nullptr, IID_PPV_ARGS(&m_ring_list[s]))))
            {
                for (crd::u32 k = 0; k <= s; ++k) { m_ring_list[k].Reset(); m_ring_alloc[k].Reset(); }
                break;
            }
            m_ring_list[s]->Close(); // created OPEN; frame_rec_begin Resets before recording
        }
        if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmd_alloc))))
        {
            return;
        }
        if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmd_alloc.Get(), nullptr,
                                               IID_PPV_ARGS(&m_list))))
        {
            return;
        }
        m_list->Close();
        if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) { return; }
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_ok    = m_event != nullptr;

        // B1-e: VRS. RSSetShadingRate / RSSetShadingRateImage live on ID3D12GraphicsCommandList5 (null on an old runtime).
        m_list.As(&m_list5);
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 opt6{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &opt6, sizeof(opt6))))
        {
            m_vrs_tier      = opt6.VariableShadingRateTier; // 0 = none · 1 = per-draw · 2 = + per-primitive + image
            m_vrs_tile_size = opt6.ShadingRateImageTileSize;
        }

        // B4: mesh shader — the amplification path. Needs ID3D12Device2::CreatePipelineState (the stream PSO for MS+PS) +
        // ID3D12GraphicsCommandList6::DispatchMesh, gated on D3D12_FEATURE_D3D12_OPTIONS7 MeshShaderTier (both interfaces and
        // the tier are absent on an old runtime, so the mesh path self-skips like every other optional capability).
        m_device.As(&m_device2);
        m_list.As(&m_list6);
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7{};
        if (m_device2 != nullptr && m_list6 != nullptr
            && SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7))))
        {
            m_mesh_shader = opt7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
        }

        // B1-f: conservative-raster tier (1 = overestimate · 3 adds SV_InnerCoverage) + ROV support (rasterizer-ordered
        // fragment-shader storage access, the DX12 analog of Vulkan's fragment-shader interlock).
        D3D12_FEATURE_DATA_D3D12_OPTIONS opt{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof(opt))))
        {
            m_conservative_tier = opt.ConservativeRasterizationTier;
            m_rov               = opt.ROVsSupported != FALSE;
            m_binding_tier      = opt.ResourceBindingTier; // B2-d: Tier 2+ = dynamic/non-uniform descriptor-array indexing
        }
        // A shader-visible CBV/SRV/UAV heap: slot 0 = storage UAV (draw_storage) · slot 1 = a texture SRV (draw_textured) ·
        // slots 2..2+kBindlessMax-1 = the bindless SRV array (draw_bindless, B2-d). Plus a shader-visible SAMPLER heap.
        D3D12_DESCRIPTOR_HEAP_DESC shd{};
        shd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        shd.NumDescriptors = 2 + kBindlessMax;
        shd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        m_device->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&m_uav_heap));
        m_srv_inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_HEAP_DESC smh{};
        smh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        smh.NumDescriptors = 2; // slot 0 = default bilinear/wrap · slot 1 = comparison (shadow)
        smh.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        m_device->CreateDescriptorHeap(&smh, IID_PPV_ARGS(&m_sampler_heap));
        m_sampler_inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        if (m_sampler_heap != nullptr)
        {
            D3D12_SAMPLER_DESC sd{};
            sd.Filter   = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sd.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sd.MaxLOD   = D3D12_FLOAT32_MAX;
            m_device->CreateSampler(&sd, m_sampler_heap->GetCPUDescriptorHandleForHeapStart()); // slot 0
            // B2-b: the COMPARISON (shadow) sampler at slot 1 — ref <= stored ⇒ 1.
            D3D12_SAMPLER_DESC cd{};
            cd.Filter         = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
            cd.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            cd.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            cd.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            cd.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            cd.MaxLOD         = D3D12_FLOAT32_MAX;
            D3D12_CPU_DESCRIPTOR_HANDLE ch = m_sampler_heap->GetCPUDescriptorHandleForHeapStart();
            ch.ptr += static_cast<SIZE_T>(m_sampler_inc);
            m_device->CreateSampler(&cd, ch);
        }
    }
    ~Dx12RasterContext() override
    {
        if (m_event != nullptr) { CloseHandle(m_event); }
    }
    Dx12RasterContext(const Dx12RasterContext&)            = delete;
    Dx12RasterContext& operator=(const Dx12RasterContext&) = delete;
    Dx12RasterContext(Dx12RasterContext&&)                 = delete;
    Dx12RasterContext& operator=(Dx12RasterContext&&)      = delete;

    [[nodiscard]] bool valid() const noexcept override { return m_ok; }
    [[nodiscard]] bool supports_vrs() const noexcept override // B1-e: Tier 2 = per-draw + per-primitive + image
    {
        return m_list5 != nullptr && m_vrs_tier >= D3D12_VARIABLE_SHADING_RATE_TIER_2;
    }
    // B1-f: conservative raster (Tier ≥ 1 = overestimate) · inner coverage (Tier 3 adds SV_InnerCoverage) · ROV.
    [[nodiscard]] bool supports_conservative_raster() const noexcept override
    {
        return m_conservative_tier >= D3D12_CONSERVATIVE_RASTERIZATION_TIER_1;
    }
    [[nodiscard]] bool supports_inner_coverage() const noexcept override
    {
        return m_conservative_tier >= D3D12_CONSERVATIVE_RASTERIZATION_TIER_3;
    }
    [[nodiscard]] bool supports_fragment_interlock() const noexcept override { return m_rov; }

    [[nodiscard]] std::unique_ptr<IRasterTarget> create_color_target(crd::u32 width, crd::u32 height) override
    {
        if (!m_ok || width == 0U || height == 0U) { return nullptr; }

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = width;
        rd.Height           = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = kColorFormat;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> tex;
        // No optimized clear value: the harness clears to arbitrary colours, so a baked value would only earn a
        // clear-value-mismatch warning on every clear. nullptr is legal for a render target.
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, IID_PPV_ARGS(&tex))))
        {
            return nullptr;
        }

        // Footprint of subresource 0 — RowPitch is aligned up to 256 (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT).
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT                               num_rows  = 0;
        UINT64                             row_bytes = 0;
        UINT64                             total     = 0;
        m_device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &num_rows, &row_bytes, &total);

        ComPtr<ID3D12Resource> readback = make_readback_buffer(m_device.Get(), total);
        if (readback == nullptr) { return nullptr; }

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)))) { return nullptr; }
        m_device->CreateRenderTargetView(tex.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

        void* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, &mapped))) { return nullptr; }

        return std::make_unique<Dx12RasterTarget>(std::move(tex), ComPtr<ID3D12Resource>{}, ComPtr<ID3D12Resource>{},
                                                  std::move(readback), std::move(rtv_heap),
                                                  ComPtr<ID3D12DescriptorHeap>{}, mapped, fp, 1U, width, height);
    }

    // B4-vis-4: a R32_UINT VISIBILITY-BUFFER target — identical to create_color_target but a single-channel uint format, so the
    // HW-raster fragment shader writes a per-pixel primitive id (SV_PrimitiveId). read_pixel returns the raw u32 id. Draw with
    // draw_visbuffer. (The RTV uses the texture's own format via a null view desc.)
    [[nodiscard]] std::unique_ptr<IRasterTarget> create_visbuffer_target(crd::u32 width, crd::u32 height) override
    {
        if (!m_ok || width == 0U || height == 0U) { return nullptr; }
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = width;
        rd.Height           = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_R32_UINT; // the visibility-id format
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> tex;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&tex))))
        {
            return nullptr;
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT                               num_rows  = 0;
        UINT64                             row_bytes = 0;
        UINT64                             total     = 0;
        m_device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &num_rows, &row_bytes, &total);
        ComPtr<ID3D12Resource> readback = make_readback_buffer(m_device.Get(), total);
        if (readback == nullptr) { return nullptr; }
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)))) { return nullptr; }
        m_device->CreateRenderTargetView(tex.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        void* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, &mapped))) { return nullptr; }
        return std::make_unique<Dx12RasterTarget>(std::move(tex), ComPtr<ID3D12Resource>{}, ComPtr<ID3D12Resource>{},
                                                  std::move(readback), std::move(rtv_heap),
                                                  ComPtr<ID3D12DescriptorHeap>{}, mapped, fp, 1U, width, height);
    }

    [[nodiscard]] std::unique_ptr<IRasterTarget> create_color_target_ms(crd::u32 width, crd::u32 height,
                                                                        crd::u32 samples) override
    {
        if (!m_ok || width == 0U || height == 0U) { return nullptr; }
        if (samples <= 1U) { return create_color_target(width, height); }             // 1x ⇒ the single-sample path
        if (samples != 2U && samples != 4U && samples != 8U) { return nullptr; }

        // Is this sample count supported for the colour format? (quality level 0 = the standard pattern.)
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ql{};
        ql.Format      = kColorFormat;
        ql.SampleCount = samples;
        ql.Flags       = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &ql, sizeof(ql)))
            || ql.NumQualityLevels == 0U)
        {
            return nullptr;
        }

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        // The MSAA colour texture (the render target).
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width              = width;
        rd.Height             = height;
        rd.DepthOrArraySize   = 1;
        rd.MipLevels          = 1;
        rd.Format             = kColorFormat;
        rd.SampleDesc.Count   = samples;
        rd.SampleDesc.Quality = 0;
        rd.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> tex;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&tex))))
        {
            return nullptr;
        }

        // The single-sample RESOLVE texture (resolve destination + copy source for readback).
        D3D12_RESOURCE_DESC rrd = rd;
        rrd.SampleDesc.Count    = 1;
        rrd.SampleDesc.Quality  = 0;
        rrd.Flags               = D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> resolve;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rrd, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, IID_PPV_ARGS(&resolve))))
        {
            return nullptr;
        }

        // Footprint from the SINGLE-SAMPLE resolve desc (what read_pixel reads back), 256-byte-aligned row pitch.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT                               num_rows  = 0;
        UINT64                             row_bytes = 0;
        UINT64                             total     = 0;
        m_device->GetCopyableFootprints(&rrd, 0, 1, 0, &fp, &num_rows, &row_bytes, &total);

        ComPtr<ID3D12Resource> readback = make_readback_buffer(m_device.Get(), total);
        if (readback == nullptr) { return nullptr; }

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)))) { return nullptr; }
        // A null RTV desc on a multisampled resource infers a TEXTURE2DMS view.
        m_device->CreateRenderTargetView(tex.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

        void* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, &mapped))) { return nullptr; }

        return std::make_unique<Dx12RasterTarget>(std::move(tex), std::move(resolve), ComPtr<ID3D12Resource>{},
                                                  std::move(readback), std::move(rtv_heap),
                                                  ComPtr<ID3D12DescriptorHeap>{}, mapped, fp, samples, width, height);
    }

    [[nodiscard]] std::unique_ptr<IRasterTarget> create_color_depth_target(crd::u32 width, crd::u32 height) override
    {
        if (!m_ok || width == 0U || height == 0U) { return nullptr; }

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        // Colour texture (RGBA8 render target + readback source).
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = width;
        rd.Height           = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = kColorFormat;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> tex;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&tex))))
        {
            return nullptr;
        }

        // D32 depth texture, created in DEPTH_WRITE (it is only ever a depth attachment, never copied). No optimized clear
        // value: the harness clears to an arbitrary depth, so a baked value would only earn a clear-value-mismatch warning.
        D3D12_RESOURCE_DESC dd = rd;
        dd.Format              = kDepthFormat;
        dd.Flags               = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        ComPtr<ID3D12Resource> depth;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                     nullptr, IID_PPV_ARGS(&depth))))
        {
            return nullptr;
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT                               num_rows  = 0;
        UINT64                             row_bytes = 0;
        UINT64                             total     = 0;
        m_device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &num_rows, &row_bytes, &total);

        ComPtr<ID3D12Resource> readback = make_readback_buffer(m_device.Get(), total);
        if (readback == nullptr) { return nullptr; }

        D3D12_DESCRIPTOR_HEAP_DESC rhd{};
        rhd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rhd.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        if (FAILED(m_device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)))) { return nullptr; }
        m_device->CreateRenderTargetView(tex.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

        D3D12_DESCRIPTOR_HEAP_DESC dhd{};
        dhd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dhd.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> dsv_heap;
        if (FAILED(m_device->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&dsv_heap)))) { return nullptr; }
        m_device->CreateDepthStencilView(depth.Get(), nullptr, dsv_heap->GetCPUDescriptorHandleForHeapStart());

        void* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, &mapped))) { return nullptr; }

        return std::make_unique<Dx12RasterTarget>(std::move(tex), ComPtr<ID3D12Resource>{}, std::move(depth),
                                                  std::move(readback), std::move(rtv_heap), std::move(dsv_heap), mapped,
                                                  fp, 1U, width, height);
    }

    [[nodiscard]] std::unique_ptr<IRasterTarget> create_color_vrs_target(crd::u32 width, crd::u32 height,
                                                                         ShadingRate tile_rate) override
    {
        auto target = create_color_target(width, height); // a plain colour target first
        // The per-tile shading-rate IMAGE needs Tier 2. Below that ⇒ a plain colour target (draw_vrs still coarsens by the
        // pipeline/primitive rate, and shades at 1x1 with no VRS at all).
        if (target == nullptr || m_list5 == nullptr || m_vrs_tier < D3D12_VARIABLE_SHADING_RATE_TIER_2 || m_vrs_tile_size == 0U)
        {
            return target;
        }

        const UINT     tile = m_vrs_tile_size;
        const crd::u32 vw   = (width + tile - 1U) / tile;
        const crd::u32 vh   = (height + tile - 1U) / tile;

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC vd{};
        vd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        vd.Width            = vw;
        vd.Height           = vh;
        vd.DepthOrArraySize = 1;
        vd.MipLevels        = 1;
        vd.Format           = DXGI_FORMAT_R8_UINT;
        vd.SampleDesc.Count = 1;
        vd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ComPtr<ID3D12Resource> vrs;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &vd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&vrs))))
        {
            return target;
        }

        // The rate image has no clear op ⇒ fill it from an UPLOAD staging buffer (every texel = the packed rate byte).
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT vfp{};
        UINT                               vrows  = 0;
        UINT64                             vrb    = 0;
        UINT64                             vtotal = 0;
        m_device->GetCopyableFootprints(&vd, 0, 1, 0, &vfp, &vrows, &vrb, &vtotal);
        ComPtr<ID3D12Resource> upload = make_upload_buffer(m_device.Get(), vtotal);
        if (upload == nullptr) { return target; }
        void* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, &mapped))) { return target; }
        const crd::u8 packed = vrs_packed(tile_rate);
        auto*         base   = static_cast<crd::u8*>(mapped) + vfp.Offset;
        for (crd::u32 row = 0; row < vh; ++row) { std::memset(base + row * vfp.Footprint.RowPitch, packed, vw); }
        upload->Unmap(0, nullptr);

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        transition(vrs.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = vrs.Get();
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource       = upload.Get();
        src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = vfp;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(vrs.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
        submit_and_wait();

        static_cast<Dx12RasterTarget&>(*target).set_vrs(std::move(vrs)); // the target owns the rate image
        return target;
    }

    void clear(IRasterTarget& target, ClearColor color) override
    {
        if (!m_ok) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);

        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float rgba[4] = {color.r, color.g, color.b, color.a};
        m_list->ClearRenderTargetView(t.rtv(), rgba, 0, nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.tex();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON); // back to the start state
        submit_and_wait();
    }

    // --- C4-b: the graphics-PSO DRAW path (append-only — vtable-stable) -------------------------------------------------
    [[nodiscard]] std::unique_ptr<IRasterProgram> create_raster_program(IGpuProgram& vertex,
                                                                        IGpuProgram& fragment) override
    {
        if (!m_ok || vertex.stage() != ShaderStage::Vertex || fragment.stage() != ShaderStage::Fragment)
        {
            return nullptr;
        }
        const auto vs = static_cast<Dx12GpuProgram&>(vertex).dxil();
        const auto fs = static_cast<Dx12GpuProgram&>(fragment).dxil();

        // Root signature: a single PIXEL-visible descriptor table with one UAV at u0 — the FS storage buffer (draw_storage).
        // Every program carries it so a storage FS's PSO links (a shader that uses u0 needs the root sig to declare it); a
        // program whose FS doesn't touch u0 just leaves the table unset (unreferenced ⇒ no draw-time binding needed), exactly
        // like the Vulkan side gives every program the storage descriptor-set layout. (Positions come from SV_VertexID.)
        D3D12_DESCRIPTOR_RANGE uav_range{};
        uav_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav_range.NumDescriptors                    = 1;
        uav_range.BaseShaderRegister                = 0; // u0 (storage buffer)
        uav_range.RegisterSpace                     = 0;
        uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE srv_range{}; // B2: t1 (texture SRV)
        srv_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors                    = 1;
        srv_range.BaseShaderRegister                = 1; // t1
        srv_range.RegisterSpace                     = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE samp_range{}; // B2: s2 (sampler)
        samp_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        samp_range.NumDescriptors                    = 1;
        samp_range.BaseShaderRegister                = 2; // s2
        samp_range.RegisterSpace                     = 0;
        samp_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE bindless_range{}; // B2-d: t3[kBindlessMax] (bindless texture array)
        bindless_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        bindless_range.NumDescriptors                    = kBindlessMax;
        bindless_range.BaseShaderRegister                = 3; // t3
        bindless_range.RegisterSpace                     = 0;
        bindless_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        // param 0 = UAV (u0, storage) · 1 = SRV (t1, texture) · 2 = sampler (s2) · 3 = bindless SRV array (t3[N]). Every
        // program carries all four; a draw sets only the tables its FS uses (an unreferenced table needs no binding).
        D3D12_ROOT_PARAMETER param[4]{};
        param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[0].DescriptorTable.NumDescriptorRanges = 1; param[0].DescriptorTable.pDescriptorRanges = &uav_range;      param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // GEO-1: + VERTEX (vertex pulling reads u0 in the VS)
        param[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[1].DescriptorTable.NumDescriptorRanges = 1; param[1].DescriptorTable.pDescriptorRanges = &srv_range;      param[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        param[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[2].DescriptorTable.NumDescriptorRanges = 1; param[2].DescriptorTable.pDescriptorRanges = &samp_range;     param[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        param[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[3].DescriptorTable.NumDescriptorRanges = 1; param[3].DescriptorTable.pDescriptorRanges = &bindless_range; param[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 4;
        rsd.pParameters   = param;
        rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> sig;
        ComPtr<ID3DBlob> err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { return nullptr; }
        ComPtr<ID3D12RootSignature> root;
        if (FAILED(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                 IID_PPV_ARGS(&root))))
        {
            return nullptr;
        }

        // The program OWNS a copy of the DXIL so it can rebuild a PSO at any sample count (an MSAA draw needs a matching one).
        auto vs_copy = std::make_unique<crd::u8[]>(vs.size());
        std::memcpy(vs_copy.get(), vs.data(), vs.size());
        auto fs_copy = std::make_unique<crd::u8[]>(fs.size());
        std::memcpy(fs_copy.get(), fs.data(), fs.size());

        // An FS that reads SV_InnerCoverage links ONLY into a conservative PSO (the D3D12 rasterizer rejects it with
        // conservative OFF — a build failure the debug layer flags). Such a program is drawn via draw_conservative, so
        // prebuild its m_pso1 conservative directly. Every other program prebuilds the plain PSO.
        const bool                  fs_conservative = static_cast<Dx12GpuProgram&>(fragment).wants_conservative_raster();
        ComPtr<ID3D12PipelineState> pso1 = build_graphics_pso(
            m_device.Get(), root.Get(), D3D12_SHADER_BYTECODE{vs.data(), vs.size()},
            D3D12_SHADER_BYTECODE{fs.data(), fs.size()}, 1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS,
            fs_conservative);
        if (pso1 == nullptr) { return nullptr; }

        return std::make_unique<Dx12RasterProgram>(m_device.Get(), std::move(root), std::move(vs_copy), vs.size(),
                                                   std::move(fs_copy), fs.size(), std::move(pso1));
    }

    // --- B4: the MESH-shader device path (append-only — vtable-stable) ----------------------------------------------------
    // Assemble a mesh program (MS + FS DXIL) into a stream PSO + root sig. Returns nullptr if the device has no mesh-shader
    // tier (D3D12_FEATURE_D3D12_OPTIONS7) — the caller guards on that, exactly like every other optional capability.
    [[nodiscard]] std::unique_ptr<IRasterProgram> create_mesh_program(IGpuProgram& mesh, IGpuProgram& fragment) override
    {
        if (!m_ok || !m_mesh_shader || m_device2 == nullptr || mesh.stage() != ShaderStage::Mesh
            || fragment.stage() != ShaderStage::Fragment)
        {
            return nullptr;
        }
        const auto ms = static_cast<Dx12GpuProgram&>(mesh).dxil();
        const auto ps = static_cast<Dx12GpuProgram&>(fragment).dxil();

        // Same 4-table root sig as the graphics path (u0 storage · t1 texture · s2 sampler · t3[N] bindless), but the
        // texture/sampler/bindless tables are ALL-visible so the MESH stage can sample the FFT cascade textures (the ocean
        // meshlet path, draw_mesh_bindless_depth); the UAV storage stays pixel-only (only the FS ROV writes it).
        D3D12_DESCRIPTOR_RANGE uav_range{};
        uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uav_range.NumDescriptors = 1; uav_range.BaseShaderRegister = 0;
        uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srv_range.NumDescriptors = 1; srv_range.BaseShaderRegister = 1;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE samp_range{};
        samp_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; samp_range.NumDescriptors = 1; samp_range.BaseShaderRegister = 2;
        samp_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE bindless_range{};
        bindless_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; bindless_range.NumDescriptors = kBindlessMax; bindless_range.BaseShaderRegister = 3;
        bindless_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER param[4]{};
        param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[0].DescriptorTable.NumDescriptorRanges = 1; param[0].DescriptorTable.pDescriptorRanges = &uav_range;      param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // GEO-1: + VERTEX (vertex pulling reads u0 in the VS)
        param[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[1].DescriptorTable.NumDescriptorRanges = 1; param[1].DescriptorTable.pDescriptorRanges = &srv_range;      param[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        param[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[2].DescriptorTable.NumDescriptorRanges = 1; param[2].DescriptorTable.pDescriptorRanges = &samp_range;     param[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        param[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[3].DescriptorTable.NumDescriptorRanges = 1; param[3].DescriptorTable.pDescriptorRanges = &bindless_range; param[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 4;
        rsd.pParameters   = param;
        rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> sig;
        ComPtr<ID3DBlob> err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { return nullptr; }
        ComPtr<ID3D12RootSignature> root;
        if (FAILED(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root))))
        {
            return nullptr;
        }

        auto ms_copy = std::make_unique<crd::u8[]>(ms.size());
        std::memcpy(ms_copy.get(), ms.data(), ms.size());
        auto ps_copy = std::make_unique<crd::u8[]>(ps.size());
        std::memcpy(ps_copy.get(), ps.data(), ps.size());
        ComPtr<ID3D12PipelineState> pso1 =
            build_mesh_pso(m_device2.Get(), root.Get(), D3D12_SHADER_BYTECODE{ms.data(), ms.size()},
                           D3D12_SHADER_BYTECODE{ps.data(), ps.size()}, 1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS);
        if (pso1 == nullptr) { return nullptr; }
        return std::make_unique<Dx12RasterProgram>(m_device.Get(), std::move(root), std::move(ms_copy), ms.size(),
                                                   std::move(ps_copy), ps.size(), std::move(pso1), m_device2.Get(),
                                                   /*is_mesh=*/true);
    }

    // B4: a TASK→MESH→FRAGMENT program (the amplification path). The AS (task) runs first, DispatchMesh-es the mesh workgroups
    // + a payload; the mesh reads it. Same 4-table root sig as create_mesh_program; the PSO adds the AS subobject. Draw with
    // draw_mesh(group_count = TASK workgroups). Returns nullptr if the device has no mesh-shader tier.
    [[nodiscard]] std::unique_ptr<IRasterProgram>
    create_task_mesh_program(IGpuProgram& task, IGpuProgram& mesh, IGpuProgram& fragment) override
    {
        if (!m_ok || !m_mesh_shader || m_device2 == nullptr || task.stage() != ShaderStage::Task
            || mesh.stage() != ShaderStage::Mesh || fragment.stage() != ShaderStage::Fragment)
        {
            return nullptr;
        }
        const auto as = static_cast<Dx12GpuProgram&>(task).dxil();
        const auto ms = static_cast<Dx12GpuProgram&>(mesh).dxil();
        const auto ps = static_cast<Dx12GpuProgram&>(fragment).dxil();

        // Identical 4-table root sig as create_mesh_program (u0 storage · t1 tex · s2 sampler · t3[N] bindless, tex tables ALL-visible).
        D3D12_DESCRIPTOR_RANGE uav_range{};
        uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uav_range.NumDescriptors = 1; uav_range.BaseShaderRegister = 0;
        uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srv_range.NumDescriptors = 1; srv_range.BaseShaderRegister = 1;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE samp_range{};
        samp_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; samp_range.NumDescriptors = 1; samp_range.BaseShaderRegister = 2;
        samp_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE bindless_range{};
        bindless_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; bindless_range.NumDescriptors = kBindlessMax; bindless_range.BaseShaderRegister = 3;
        bindless_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER param[4]{};
        param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[0].DescriptorTable.NumDescriptorRanges = 1; param[0].DescriptorTable.pDescriptorRanges = &uav_range;      param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // GEO-1: + VERTEX (vertex pulling reads u0 in the VS)
        param[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[1].DescriptorTable.NumDescriptorRanges = 1; param[1].DescriptorTable.pDescriptorRanges = &srv_range;      param[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        param[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[2].DescriptorTable.NumDescriptorRanges = 1; param[2].DescriptorTable.pDescriptorRanges = &samp_range;     param[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        param[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[3].DescriptorTable.NumDescriptorRanges = 1; param[3].DescriptorTable.pDescriptorRanges = &bindless_range; param[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 4;
        rsd.pParameters   = param;
        rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> sig;
        ComPtr<ID3DBlob> err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { return nullptr; }
        ComPtr<ID3D12RootSignature> root;
        if (FAILED(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root))))
        {
            return nullptr;
        }

        auto as_copy = std::make_unique<crd::u8[]>(as.size());
        std::memcpy(as_copy.get(), as.data(), as.size());
        auto ms_copy = std::make_unique<crd::u8[]>(ms.size());
        std::memcpy(ms_copy.get(), ms.data(), ms.size());
        auto ps_copy = std::make_unique<crd::u8[]>(ps.size());
        std::memcpy(ps_copy.get(), ps.data(), ps.size());
        ComPtr<ID3D12PipelineState> pso1 =
            build_mesh_pso(m_device2.Get(), root.Get(), D3D12_SHADER_BYTECODE{ms.data(), ms.size()},
                           D3D12_SHADER_BYTECODE{ps.data(), ps.size()}, 1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS,
                           1U, D3D12_SHADER_BYTECODE{as.data(), as.size()});
        if (pso1 == nullptr) { return nullptr; }
        return std::make_unique<Dx12RasterProgram>(m_device.Get(), std::move(root), std::move(ms_copy), ms.size(),
                                                   std::move(ps_copy), ps.size(), std::move(pso1), m_device2.Get(),
                                                   /*is_mesh=*/true, std::move(as_copy), as.size());
    }

    // B4-tess: a VS→HS→DS→PS program (the PORTABLE displacement path for HW without mesh shaders). The VS emits patch control
    // points; the hull sets the tess factors; the domain evaluates the subdivided surface + displaces it. A CLASSIC graphics
    // PSO (PrimitiveTopologyType = PATCH) drawn as a control-point patch list (draw_tess). Reuses the 4-table root sig with the
    // texture/sampler tables ALL-visible so the domain shader can sample a displacement map. nullptr on a stage mismatch.
    [[nodiscard]] std::unique_ptr<IRasterProgram> create_tess_program(IGpuProgram& vertex, IGpuProgram& tess_control,
                                                                      IGpuProgram& tess_eval, IGpuProgram& fragment) override
    {
        if (!m_ok || vertex.stage() != ShaderStage::Vertex || tess_control.stage() != ShaderStage::TessControl
            || tess_eval.stage() != ShaderStage::TessEval || fragment.stage() != ShaderStage::Fragment)
        {
            return nullptr;
        }
        const auto vs = static_cast<Dx12GpuProgram&>(vertex).dxil();
        const auto hs = static_cast<Dx12GpuProgram&>(tess_control).dxil();
        const auto ds = static_cast<Dx12GpuProgram&>(tess_eval).dxil();
        const auto ps = static_cast<Dx12GpuProgram&>(fragment).dxil();

        // 4-table root sig (u0 storage pixel-only · t1 tex · s2 sampler · t3[N] bindless, texture tables ALL-visible so the DS
        // can sample a displacement map) — identical shape to create_mesh_program.
        D3D12_DESCRIPTOR_RANGE uav_range{};
        uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uav_range.NumDescriptors = 1; uav_range.BaseShaderRegister = 0;
        uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srv_range.NumDescriptors = 1; srv_range.BaseShaderRegister = 1;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE samp_range{};
        samp_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; samp_range.NumDescriptors = 1; samp_range.BaseShaderRegister = 2;
        samp_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE bindless_range{};
        bindless_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; bindless_range.NumDescriptors = kBindlessMax; bindless_range.BaseShaderRegister = 3;
        bindless_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER param[4]{};
        param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[0].DescriptorTable.NumDescriptorRanges = 1; param[0].DescriptorTable.pDescriptorRanges = &uav_range;      param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // GEO-1: + VERTEX (vertex pulling reads u0 in the VS)
        param[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[1].DescriptorTable.NumDescriptorRanges = 1; param[1].DescriptorTable.pDescriptorRanges = &srv_range;      param[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        param[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[2].DescriptorTable.NumDescriptorRanges = 1; param[2].DescriptorTable.pDescriptorRanges = &samp_range;     param[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        param[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[3].DescriptorTable.NumDescriptorRanges = 1; param[3].DescriptorTable.pDescriptorRanges = &bindless_range; param[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 4;
        rsd.pParameters   = param;
        rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> sig;
        ComPtr<ID3DBlob> err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { return nullptr; }
        ComPtr<ID3D12RootSignature> root;
        if (FAILED(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root))))
        {
            return nullptr;
        }

        auto vs_copy = std::make_unique<crd::u8[]>(vs.size());
        std::memcpy(vs_copy.get(), vs.data(), vs.size());
        auto ps_copy = std::make_unique<crd::u8[]>(ps.size());
        std::memcpy(ps_copy.get(), ps.data(), ps.size());
        auto hs_copy = std::make_unique<crd::u8[]>(hs.size());
        std::memcpy(hs_copy.get(), hs.data(), hs.size());
        auto ds_copy = std::make_unique<crd::u8[]>(ds.size());
        std::memcpy(ds_copy.get(), ds.data(), ds.size());
        ComPtr<ID3D12PipelineState> pso1 = build_graphics_pso(
            m_device.Get(), root.Get(), D3D12_SHADER_BYTECODE{vs.data(), vs.size()},
            D3D12_SHADER_BYTECODE{ps.data(), ps.size()}, 1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U,
            D3D12_SHADER_BYTECODE{hs.data(), hs.size()}, D3D12_SHADER_BYTECODE{ds.data(), ds.size()}); // B4-tess: HS + DS ⇒ PATCH PSO
        if (pso1 == nullptr) { return nullptr; }
        return std::make_unique<Dx12RasterProgram>(m_device.Get(), std::move(root), std::move(vs_copy), vs.size(),
                                                   std::move(ps_copy), ps.size(), std::move(pso1), nullptr, /*is_mesh=*/false,
                                                   nullptr, 0, std::move(hs_copy), hs.size(), std::move(ds_copy), ds.size());
    }

    // B4-tess: draw `patch_count` QUAD patches through the tessellator (VS→HS→DS→PS). Identical to draw() except the IA feeds
    // 4-control-point PATCHES (a control-point patch list) instead of a triangle list. Colour-only; result host-readable.
    void draw_tess(IRasterTarget& target, IRasterProgram& program, ClearColor clear, crd::u32 patch_count) override
    {
        if (!m_ok) { return; }
        auto&      t  = static_cast<Dx12RasterTarget&>(target);
        auto&      p  = static_cast<Dx12RasterProgram&>(program);
        if (!p.is_tess()) { return; } // only a VS+HS+DS+PS program can be patch-drawn
        const bool ms = t.multisampled();
        ID3D12PipelineState* pso = p.pso_for(t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false);
        if (!p.valid() || pso == nullptr) { return; }

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);

        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);

        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST); // the tessellator consumes quad patches
        m_list->DrawInstanced(patch_count * 4U, 1, 0, 0);                                 // patch_count patches × 4 control points

        if (ms) // AVERAGE-resolve the MSAA colour texture into the single-sample resolve texture (the readback source)
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            transition(t.resolve(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            m_list->ResolveSubresource(t.resolve(), 0, t.tex(), 0, kColorFormat);
            transition(t.resolve(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        else
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.copy_src();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        if (ms)
        {
            transition(t.resolve(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            transition(t.tex(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        else
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        submit_and_wait();
    }

    // Clear `target` and DISPATCH `group_count` mesh workgroups (a mesh program). Colour-only (the mesh-triangle proof); the
    // readback tail is identical to draw() — only the topology-less DispatchMesh replaces IASetPrimitiveTopology+DrawInstanced.
    void draw_mesh(IRasterTarget& target, IRasterProgram& program, ClearColor clear, crd::u32 group_count) override
    {
        if (!m_ok || !m_mesh_shader || m_list6 == nullptr) { return; }
        auto&      t   = static_cast<Dx12RasterTarget&>(target);
        auto&      p   = static_cast<Dx12RasterProgram&>(program);
        const bool ms  = t.multisampled();
        ID3D12PipelineState* pso = p.pso_for(t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false);
        if (!p.valid() || !p.is_mesh() || pso == nullptr) { return; }

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);

        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);

        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetPipelineState(pso);
        m_list6->DispatchMesh(group_count, 1, 1); // mesh shaders emit their own topology — no IASetPrimitiveTopology

        if (ms)
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            transition(t.resolve(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            m_list->ResolveSubresource(t.resolve(), 0, t.tex(), 0, kColorFormat);
            transition(t.resolve(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        else
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.copy_src();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        if (ms)
        {
            transition(t.resolve(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            transition(t.tex(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        else
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        submit_and_wait();
    }

    // B4: DISPATCH a MESH program with PER-PRIMITIVE VRS. Identical to draw_mesh, but sets the shading-rate combiner to OVERRIDE
    // so the mesh's per-primitive SV_ShadingRate output drives the coarse fragment rate (the base rate stays 1×1).
    void draw_mesh_vrs(IRasterTarget& target, IRasterProgram& program, ClearColor clear, crd::u32 group_count) override
    {
        if (!m_ok || !m_mesh_shader || m_list6 == nullptr) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        if (m_list5 == nullptr || m_vrs_tier == D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED)
        {
            draw_mesh(target, program, clear, group_count); // no VRS ⇒ a full-rate mesh draw
            return;
        }
        ID3D12PipelineState* pso = p.pso_for(1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false);
        if (!p.valid() || !p.is_mesh() || pso == nullptr) { return; }

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetPipelineState(pso);
        // combiner[0] = base∘primitive (OVERRIDE ⇒ the SV_ShadingRate output wins); combiner[1] = ∘attachment (none).
        const D3D12_SHADING_RATE_COMBINER comb[2] = {D3D12_SHADING_RATE_COMBINER_OVERRIDE,
                                                     D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
        m_list5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, comb);
        m_list6->DispatchMesh(group_count, 1, 1);
        m_list5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr); // reset to the default 1×1 (no combiners)

        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.copy_src();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit_and_wait();
    }

    // B4: GPU-DRIVEN INDIRECT MESHLET DISPATCH. The mesh-workgroup count comes from `native_args` (an ID3D12Resource* a compute
    // CULL pass wrote as {ThreadGroupCountX, 1, 1}), consumed by ExecuteIndirect with a DISPATCH_MESH command signature — the
    // count decided entirely on the GPU. The args buffer decayed to COMMON after the compute submit, so this transitions it to
    // INDIRECT_ARGUMENT freely. Colour-only.
    void draw_mesh_indirect(IRasterTarget& target, IRasterProgram& program, ClearColor clear, void* native_args,
                            crd::u64 args_offset) override
    {
        if (!m_ok || !m_mesh_shader || m_list6 == nullptr || native_args == nullptr) { return; }
        auto&                t   = static_cast<Dx12RasterTarget&>(target);
        auto&                p   = static_cast<Dx12RasterProgram&>(program);
        ID3D12PipelineState* pso = p.pso_for(1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false);
        if (!p.valid() || !p.is_mesh() || pso == nullptr) { return; }
        auto* args = static_cast<ID3D12Resource*>(native_args);

        if (m_mesh_indirect_sig == nullptr) // lazily create the DISPATCH_MESH command signature (a pure dispatch ⇒ null root sig)
        {
            D3D12_INDIRECT_ARGUMENT_DESC arg{};
            arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
            D3D12_COMMAND_SIGNATURE_DESC csd{};
            csd.ByteStride       = 3U * static_cast<UINT>(sizeof(UINT)); // {X, Y, Z}
            csd.NumArgumentDescs = 1U;
            csd.pArgumentDescs   = &arg;
            if (FAILED(m_device->CreateCommandSignature(&csd, nullptr, IID_PPV_ARGS(&m_mesh_indirect_sig)))) { return; }
        }

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(args, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetPipelineState(pso);
        m_list->ExecuteIndirect(m_mesh_indirect_sig.Get(), 1U, args, args_offset, nullptr, 0U); // ONE DISPATCH_MESH command

        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(args, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON); // restore for reuse
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.copy_src();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit_and_wait();
    }

    // B4 ocean fast path: DISPATCH `group_count` meshlet workgroups into a colour+DEPTH target with the cascade textures bound
    // BINDLESS (t3[]) + the sampler (s2) — the mesh shader samples the FFT displacement (its root sig makes them ALL-visible).
    // Combines the bindless-SRV-heap machinery of draw_bindless with the depth target of draw_depth, then DispatchMesh.
    void draw_mesh_bindless_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear, float clear_depth,
                                  DepthCompare compare, ITexture* const* textures, crd::u32 count,
                                  crd::u32 group_count) override
    {
        if (!m_ok || !m_mesh_shader || m_list6 == nullptr || m_uav_heap == nullptr || m_sampler_heap == nullptr
            || count == 0U || textures == nullptr)
        {
            return;
        }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        if (!t.has_depth()) { return; } // needs a create_color_depth_target
        ID3D12PipelineState* pso = p.pso_for(1U, kDepthFormat, to_d3d12_compare(compare), false);
        if (!p.valid() || !p.is_mesh() || pso == nullptr) { return; }
        const crd::u32 n = count < static_cast<crd::u32>(kBindlessMax) ? count : static_cast<crd::u32>(kBindlessMax);

        // Mint the bindless SRV array into heap slots 2..2+kBindlessMax-1 (0..n-1 = cascades, rest replicate #0).
        for (UINT i = 0; i < kBindlessMax; ++i)
        {
            auto&                           tex = static_cast<Dx12Texture&>(*textures[i < n ? i : 0U]);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = tex.srv();
            D3D12_CPU_DESCRIPTOR_HANDLE     h   = m_uav_heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(2U + i) * m_srv_inc; // bindless array starts at slot 2
            m_device->CreateShaderResourceView(tex.tex(), &srv, h);
        }
        D3D12_GPU_DESCRIPTOR_HANDLE bindless_gpu = m_uav_heap->GetGPUDescriptorHandleForHeapStart();
        bindless_gpu.ptr += static_cast<UINT64>(2U) * m_srv_inc; // slot 2

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = t.dsv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        m_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, clear_depth, 0, 0, nullptr);

        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        ID3D12DescriptorHeap* heaps[] = {m_uav_heap.Get(), m_sampler_heap.Get()};
        m_list->SetDescriptorHeaps(2, heaps);
        m_list->SetGraphicsRootDescriptorTable(2, m_sampler_heap->GetGPUDescriptorHandleForHeapStart()); // sampler s2
        m_list->SetGraphicsRootDescriptorTable(3, bindless_gpu);                                          // bindless t3[]
        m_list->SetPipelineState(pso);
        m_list6->DispatchMesh(group_count, 1, 1); // mesh emits its own topology — no IASetPrimitiveTopology

        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.tex();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit_and_wait();
    }

    void draw(IRasterTarget& target, IRasterProgram& program, ClearColor clear, crd::u32 vertex_count) override
    {
        if (!m_ok) { return; }
        auto&      t  = static_cast<Dx12RasterTarget&>(target);
        auto&      p  = static_cast<Dx12RasterProgram&>(program);
        const bool ms = t.multisampled();
        ID3D12PipelineState* pso = p.pso_for(t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false); // colour, no depth
        if (!p.valid() || pso == nullptr) { return; }

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);

        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);

        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);

        if (ms) // AVERAGE-resolve the MSAA colour texture into the single-sample resolve texture (the readback source)
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            transition(t.resolve(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            m_list->ResolveSubresource(t.resolve(), 0, t.tex(), 0, kColorFormat);
            transition(t.resolve(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        else
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.copy_src(); // the resolved single-sample texture for MSAA, else the colour texture
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        if (ms)
        {
            transition(t.resolve(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            transition(t.tex(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        else
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        submit_and_wait();
    }

    // B4-vis-4: HW-raster into a R32_UINT VISIBILITY BUFFER (create_visbuffer_target) via a R32_UINT PSO. The FS writes
    // SV_PrimitiveId per covered pixel. Single-sample, colour-only. (ClearRenderTargetView can't express an arbitrary uint id
    // on an integer RTV, so it clears to 0; the fullscreen visibility draw overwrites every pixel, so `clear_id` is advisory.)
    void draw_visbuffer(IRasterTarget& target, IRasterProgram& program, crd::u32 /*clear_id*/, crd::u32 vertex_count) override
    {
        if (!m_ok) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        ID3D12PipelineState* pso = p.pso_for(1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, DXGI_FORMAT_R32_UINT);
        if (!p.valid() || pso == nullptr) { return; }

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float zero[4] = {0.0F, 0.0F, 0.0F, 0.0F};
        m_list->ClearRenderTargetView(rtv, zero, 0, nullptr);
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);

        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.copy_src();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit_and_wait();
    }

    void draw_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear, float clear_depth,
                    DepthCompare compare, crd::u32 vertex_count) override
    {
        if (!m_ok) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        if (!t.has_depth()) { return; } // needs a create_color_depth_target target
        ID3D12PipelineState* pso = p.pso_for(1U, kDepthFormat, to_d3d12_compare(compare), false); // depth-enabled PSO (baked)
        if (!p.valid() || pso == nullptr) { return; }

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);

        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = t.dsv(); // depth stays in DEPTH_WRITE (created that way; never copied)
        m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        m_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, clear_depth, 0, 0, nullptr);

        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);

        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.tex(); // single-sample colour
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit_and_wait();
    }

    void draw_vrs(IRasterTarget& target, IRasterProgram& program, ClearColor clear, ShadingRate pipeline_rate,
                  ShadingRateCombiner primitive_combiner, crd::u32 vertex_count) override
    {
        if (!m_ok) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        if (m_list5 == nullptr || m_vrs_tier == D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED)
        {
            draw(target, program, clear, vertex_count); // no VRS ⇒ a plain 1x1 draw
            return;
        }
        ID3D12PipelineState* pso = p.pso_for(t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false);
        if (!p.valid() || pso == nullptr) { return; }

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);

        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);

        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // VRS: combiner[0] = pipeline∘primitive (Override ⇒ the SV_ShadingRate output wins); combiner[1] = ∘attachment
        // (Override when the target carries a per-tile rate image, else Passthrough).
        const D3D12_SHADING_RATE_COMBINER comb[2] = {
            to_d3d12_combiner(primitive_combiner),
            t.has_vrs() ? D3D12_SHADING_RATE_COMBINER_OVERRIDE : D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
        m_list5->RSSetShadingRate(to_d3d12_rate(pipeline_rate), comb);
        if (t.has_vrs()) { m_list5->RSSetShadingRateImage(t.vrs_tex()); }

        m_list->DrawInstanced(vertex_count, 1, 0, 0);

        if (t.has_vrs()) { m_list5->RSSetShadingRateImage(nullptr); } // unbind before the image is freed

        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.tex();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit_and_wait();
    }

    void draw_conservative(IRasterTarget& target, IRasterProgram& program, ClearColor clear, ConservativeMode mode,
                           crd::u32 vertex_count) override
    {
        if (!m_ok) { return; }
        auto&      t            = static_cast<Dx12RasterTarget&>(target);
        auto&      p            = static_cast<Dx12RasterProgram&>(program);
        const bool conservative = mode != ConservativeMode::Off && supports_conservative_raster();
        if (!conservative) { draw(target, program, clear, vertex_count); return; } // no conservative raster ⇒ a normal draw
        const bool           ms  = t.multisampled();
        ID3D12PipelineState* pso = p.pso_for(t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, true);
        if (!p.valid() || pso == nullptr) { return; }

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);

        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);

        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);

        if (ms) // resolve then copy (a conservative overestimate + MSAA test exercises both)
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            transition(t.resolve(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            m_list->ResolveSubresource(t.resolve(), 0, t.tex(), 0, kColorFormat);
            transition(t.resolve(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        else
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.copy_src();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        if (ms)
        {
            transition(t.resolve(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            transition(t.tex(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        else
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        submit_and_wait();
    }

    [[nodiscard]] std::unique_ptr<IStorageBuffer> create_storage_buffer(crd::u32 size_bytes) override
    {
        if (!m_ok || size_bytes == 0U) { return nullptr; }
        const UINT64 size = (static_cast<UINT64>(size_bytes) + 3U) & ~static_cast<UINT64>(3U); // round up to 4-byte words

        // The DEFAULT-heap UAV buffer the FS writes (created COPY_DEST so it can be zero-filled, then parked in UAV state).
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = size;
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> buf;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&buf))))
        {
            return nullptr;
        }

        // Zero-init from an UPLOAD staging buffer (committed resources are not guaranteed zeroed).
        ComPtr<ID3D12Resource> upload = make_upload_buffer(m_device.Get(), size);
        if (upload == nullptr) { return nullptr; }
        void* umap = nullptr;
        if (FAILED(upload->Map(0, nullptr, &umap))) { return nullptr; }
        std::memset(umap, 0, static_cast<size_t>(size));
        upload->Unmap(0, nullptr);

        ComPtr<ID3D12Resource> readback = make_readback_buffer(m_device.Get(), size);
        if (readback == nullptr) { return nullptr; }

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        m_list->CopyBufferRegion(buf.Get(), 0, upload.Get(), 0, size);
        transition(buf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit_and_wait();

        void* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, &mapped))) { return nullptr; }
        return std::make_unique<Dx12StorageBuffer>(std::move(buf), std::move(readback), mapped, size_bytes);
    }

    // GEO-1: staged CPU→UAV upload (vertex pulling — the cooked vertex stream the VS fetches by SV_VertexID). Mirrors the
    // zero-init pattern: UPLOAD buffer + CopyBufferRegion, UAV→COPY_DEST→UAV transitions, blocking submit.
    [[nodiscard]] bool upload_storage(IStorageBuffer& storage, crd::u32 byte_offset, const void* data,
                                      crd::u32 size_bytes) override
    {
        if (!m_ok || data == nullptr || size_bytes == 0U) { return false; }
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        if (static_cast<crd::u64>(byte_offset) + size_bytes > s.size_bytes()) { return false; }

        ComPtr<ID3D12Resource> upload = make_upload_buffer(m_device.Get(), size_bytes);
        if (upload == nullptr) { return false; }
        void* umap = nullptr;
        if (FAILED(upload->Map(0, nullptr, &umap))) { return false; }
        std::memcpy(umap, data, size_bytes);
        upload->Unmap(0, nullptr);

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        transition(s.buf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        m_list->CopyBufferRegion(s.buf(), byte_offset, upload.Get(), 0, size_bytes);
        transition(s.buf(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit_and_wait();
        return true;
    }

    void draw_storage(IRasterTarget& target, IRasterProgram& program, ClearColor clear, IStorageBuffer& storage,
                      crd::u32 vertex_count) override
    {
        if (!m_ok || m_uav_heap == nullptr) { return; }
        auto&                t   = static_cast<Dx12RasterTarget&>(target);
        auto&                p   = static_cast<Dx12RasterProgram&>(program);
        auto&                s   = static_cast<Dx12StorageBuffer&>(storage);
        ID3D12PipelineState* pso = p.pso_for(1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false); // single-sample colour
        if (!p.valid() || pso == nullptr) { return; }

        // REN-2: in frame-graph recording mode, draw_storage into an RTT transient records color-only (no readback).
        if (frame_recording()) { record_offscreen(t, p, s, pso, clear, vertex_count); return; }

        // Point the heap's slot-0 UAV at the storage buffer (a structured buffer of uint words — the shader's RWStructured…).
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format                     = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement        = 0;
        uav.Buffer.NumElements         = s.num_elements();
        uav.Buffer.StructureByteStride = 4;
        m_device->CreateUnorderedAccessView(s.buf(), nullptr, &uav, m_uav_heap->GetCPUDescriptorHandleForHeapStart());

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);

        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);

        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        ID3D12DescriptorHeap* heaps[] = {m_uav_heap.Get()};
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetGraphicsRootDescriptorTable(0, m_uav_heap->GetGPUDescriptorHandleForHeapStart());
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);

        // Colour → readback (the target), and the storage buffer → its readback.
        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.tex();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

        transition(s.buf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_list->CopyBufferRegion(s.readback(), 0, s.buf(), 0, s.size_bytes());
        transition(s.buf(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit_and_wait();
    }

    // GEO-7: the scene-geometry draw — draw_storage's UAV seam + draw_depth's cleared colour+depth pass (depth
    // write ON). No per-draw SSBO readback (scene buffers are GPU-consumed; download_storage on demand).
    // REN-3.1: the DEPTH-ONLY (shadow) pass — render storage-pulled geometry writing ONLY depth. The PSO is built
    // with num_rts = 0 (a legal depth-only D3D12 graphics PSO: NumRenderTargets = 0, DSVFormat = D32_FLOAT), and no
    // RTV is bound. For a frame-graph D32Float+sampled transient this PRODUCES a shadow map the next pass samples
    // through its R32_FLOAT SRV (see the R32_TYPELESS rule at create_transient_image).
    void draw_storage_depth_only(IRasterTarget& target, IRasterProgram& program, float clear_depth,
                                 DepthCompare compare, IStorageBuffer& storage, crd::u32 vertex_count) override
    {
        if (!m_ok || m_uav_heap == nullptr) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        if (!t.has_depth()) { return; }
        ID3D12PipelineState* pso = p.pso_for(1U, kDepthFormat, to_d3d12_compare(compare), false, /*num_rts*/ 0U);
        if (!p.valid() || pso == nullptr) { return; }

        if (frame_recording()) { record_depth_only(t, p, s, pso, true, clear_depth, vertex_count); return; }

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format                     = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement        = 0;
        uav.Buffer.NumElements         = s.num_elements();
        uav.Buffer.StructureByteStride = 4;
        m_device->CreateUnorderedAccessView(s.buf(), nullptr, &uav, m_uav_heap->GetCPUDescriptorHandleForHeapStart());

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = t.dsv();
        m_list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        m_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, clear_depth, 0, 0, nullptr);
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        ID3D12DescriptorHeap* heaps[] = {m_uav_heap.Get()};
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetGraphicsRootDescriptorTable(0, m_uav_heap->GetGPUDescriptorHandleForHeapStart());
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);
        submit_and_wait(); // (submit_and_wait closes the list itself, like every other standalone draw)
    }

    // REN-3.1: the CONTINUING depth-only draw — mesh N>0 of a shadow pass joins the SAME depth map (no clear).
    void draw_storage_depth_only_load(IRasterTarget& target, IRasterProgram& program, DepthCompare compare,
                                      IStorageBuffer& storage, crd::u32 vertex_count) override
    {
        if (!m_ok || m_uav_heap == nullptr) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        if (!t.has_depth()) { return; }
        ID3D12PipelineState* pso = p.pso_for(1U, kDepthFormat, to_d3d12_compare(compare), false, /*num_rts*/ 0U);
        if (!p.valid() || pso == nullptr) { return; }
        if (frame_recording()) { record_depth_only(t, p, s, pso, false, 0.0F, vertex_count); }
    }

    void draw_storage_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear, float clear_depth,
                            DepthCompare compare, IStorageBuffer& storage, crd::u32 vertex_count) override
    {
        if (!m_ok || m_uav_heap == nullptr) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        if (!t.has_depth()) { return; } // needs a create_color_depth_target target
        ID3D12PipelineState* pso = p.pso_for(1U, kDepthFormat, to_d3d12_compare(compare), false);
        if (!p.valid() || pso == nullptr) { return; }

        // REN-1 pt-2: in frame-graph recording mode, record into the shared open list (no per-draw reset/submit).
        if (frame_recording()) { record_scene(t, p, s, pso, true, clear, clear_depth, vertex_count); return; }

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format                     = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement        = 0;
        uav.Buffer.NumElements         = s.num_elements();
        uav.Buffer.StructureByteStride = 4;
        m_device->CreateUnorderedAccessView(s.buf(), nullptr, &uav, m_uav_heap->GetCPUDescriptorHandleForHeapStart());

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);

        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = t.dsv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        m_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, clear_depth, 0, 0, nullptr);

        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        ID3D12DescriptorHeap* heaps[] = {m_uav_heap.Get()};
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetGraphicsRootDescriptorTable(0, m_uav_heap->GetGPUDescriptorHandleForHeapStart());
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);

        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.tex();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit_and_wait();
    }

    // REN-2 Half B: the TEXTURED scene draw — draw_storage_depth + a sampled material (base-color) texture. Records
    // into the frame graph (record_scene_textured); standalone binds storage UAV (slot 0) + the SRV (slot 1) + sampler.
    void draw_storage_textured_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear, float clear_depth,
                                     DepthCompare compare, IStorageBuffer& storage, ITexture& texture,
                                     crd::u32 vertex_count) override
    {
        draw_storage_sampled_depth(target, program, clear, clear_depth, compare, storage, texture, vertex_count, 0U);
    }
    // REN-3.2-b: the shared body. sampler_slot picks FILTERING (0) or COMPARISON (1) from the sampler heap;
    // everything else is identical, which is why the shadowed draw needs no new root signature or set layout.
    void draw_storage_sampled_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                                    float clear_depth, DepthCompare compare, IStorageBuffer& storage,
                                    ITexture& texture, crd::u32 vertex_count, crd::u32 sampler_slot)
    {
        if (!m_ok || m_uav_heap == nullptr || m_sampler_heap == nullptr) { return; }
        auto& t   = static_cast<Dx12RasterTarget&>(target);
        auto& p   = static_cast<Dx12RasterProgram&>(program);
        auto& s   = static_cast<Dx12StorageBuffer&>(storage);
        auto& tex = static_cast<Dx12Texture&>(texture);
        if (!t.has_depth()) { return; }
        ID3D12PipelineState* pso = p.pso_for(1U, kDepthFormat, to_d3d12_compare(compare), false);
        if (!p.valid() || pso == nullptr) { return; }
        if (frame_recording())
        {
            record_scene_textured(t, p, s, tex, pso, true, clear, clear_depth, vertex_count);
            return;
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format                     = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements         = s.num_elements();
        uav.Buffer.StructureByteStride = 4;
        m_device->CreateUnorderedAccessView(s.buf(), nullptr, &uav, m_uav_heap->GetCPUDescriptorHandleForHeapStart());
        const D3D12_SHADER_RESOURCE_VIEW_DESC srv     = tex.srv(); // SRV at heap slot 1
        D3D12_CPU_DESCRIPTOR_HANDLE           srv_cpu = m_uav_heap->GetCPUDescriptorHandleForHeapStart();
        srv_cpu.ptr += static_cast<SIZE_T>(m_srv_inc);
        m_device->CreateShaderResourceView(tex.tex(), &srv, srv_cpu);
        D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = m_uav_heap->GetGPUDescriptorHandleForHeapStart();
        srv_gpu.ptr += static_cast<UINT64>(m_srv_inc);

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = t.dsv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        m_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, clear_depth, 0, 0, nullptr);
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        ID3D12DescriptorHeap* heaps[] = {m_uav_heap.Get(), m_sampler_heap.Get()};
        m_list->SetDescriptorHeaps(2, heaps);
        m_list->SetGraphicsRootDescriptorTable(0, m_uav_heap->GetGPUDescriptorHandleForHeapStart());  // storage (u0)
        m_list->SetGraphicsRootDescriptorTable(1, srv_gpu);                                            // base-color (t1)
        D3D12_GPU_DESCRIPTOR_HANDLE samp_tbl = m_sampler_heap->GetGPUDescriptorHandleForHeapStart();
        samp_tbl.ptr += static_cast<UINT64>(sampler_slot) * m_sampler_inc;
        m_list->SetGraphicsRootDescriptorTable(2, samp_tbl); // sampler (s2): 0 = filtering, 1 = comparison
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);

        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.tex();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit_and_wait();
    }
    void draw_storage_textured_depth_load(IRasterTarget& target, IRasterProgram& program, DepthCompare compare,
                                          IStorageBuffer& storage, ITexture& texture, crd::u32 vertex_count) override
    {
        auto& t   = static_cast<Dx12RasterTarget&>(target);
        auto& p   = static_cast<Dx12RasterProgram&>(program);
        auto& s   = static_cast<Dx12StorageBuffer&>(storage);
        auto& tex = static_cast<Dx12Texture&>(texture);
        if (!m_ok || !t.has_depth()) { return; }
        ID3D12PipelineState* pso = p.pso_for(1U, kDepthFormat, to_d3d12_compare(compare), false);
        if (!p.valid() || pso == nullptr) { return; }
        if (frame_recording())
        {
            record_scene_textured(t, p, s, tex, pso, false, ClearColor{}, 0.0F, vertex_count, 0U);
            return;
        }
        draw_storage_textured_depth(target, program, ClearColor{}, 0.0F, compare, storage, texture, vertex_count);
    }

    // ── REN-3.2-b: the SHADOWED scene draw — the textured body with the COMPARISON sampler at slot 1. ──
    void draw_storage_shadowed_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                                     float clear_depth, DepthCompare compare, IStorageBuffer& storage,
                                     ITexture& shadow_atlas, crd::u32 vertex_count) override
    {
        draw_storage_sampled_depth(target, program, clear, clear_depth, compare, storage, shadow_atlas,
                                   vertex_count, 1U);
    }
    void draw_storage_shadowed_depth_load(IRasterTarget& target, IRasterProgram& program, DepthCompare compare,
                                          IStorageBuffer& storage, ITexture& shadow_atlas,
                                          crd::u32 vertex_count) override
    {
        auto& t   = static_cast<Dx12RasterTarget&>(target);
        auto& p   = static_cast<Dx12RasterProgram&>(program);
        auto& s   = static_cast<Dx12StorageBuffer&>(storage);
        auto& tex = static_cast<Dx12Texture&>(shadow_atlas);
        if (!m_ok || !t.has_depth()) { return; }
        ID3D12PipelineState* pso = p.pso_for(1U, kDepthFormat, to_d3d12_compare(compare), false);
        if (!p.valid() || pso == nullptr) { return; }
        if (frame_recording())
        {
            record_scene_textured(t, p, s, tex, pso, false, ClearColor{}, 0.0F, vertex_count, 1U);
            return;
        }
        draw_storage_shadowed_depth(target, program, ClearColor{}, 0.0F, compare, storage, shadow_atlas,
                                    vertex_count);
    }

    // GEO-8: the CONTINUING scene draw — draw_storage_depth minus the Clear calls (colour + depth both persist;
    // depth keeps testing AND writing so mesh groups compose through the real depth buffer).
    void draw_storage_depth_load(IRasterTarget& target, IRasterProgram& program, DepthCompare compare,
                                 IStorageBuffer& storage, crd::u32 vertex_count) override
    {
        if (!m_ok || m_uav_heap == nullptr) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        if (!t.has_depth()) { return; }
        ID3D12PipelineState* pso = p.pso_for(1U, kDepthFormat, to_d3d12_compare(compare), false);
        if (!p.valid() || pso == nullptr) { return; }

        // REN-1 pt-2: record into the shared open list (LOAD variant — no clears) when a frame graph is executing.
        if (frame_recording()) { record_scene(t, p, s, pso, false, ClearColor{}, 0.0F, vertex_count); return; }

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format                     = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement        = 0;
        uav.Buffer.NumElements         = s.num_elements();
        uav.Buffer.StructureByteStride = 4;
        m_device->CreateUnorderedAccessView(s.buf(), nullptr, &uav, m_uav_heap->GetCPUDescriptorHandleForHeapStart());

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);

        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = t.dsv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv); // NO clears — the frame's contents persist

        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        ID3D12DescriptorHeap* heaps[] = {m_uav_heap.Get()};
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetGraphicsRootDescriptorTable(0, m_uav_heap->GetGPUDescriptorHandleForHeapStart());
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);

        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.tex();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit_and_wait();
    }

    [[nodiscard]] std::unique_ptr<ITexture> create_texture(crd::u32 width, crd::u32 height, const void* rgba) override
    {
        if (!m_ok || width == 0U || height == 0U || rgba == nullptr) { return nullptr; }

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = width;
        rd.Height           = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = kColorFormat;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ComPtr<ID3D12Resource> tex;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&tex))))
        {
            return nullptr;
        }

        // Upload: copy the tightly-packed pixels into an UPLOAD buffer honouring the 256-byte-aligned dest row pitch.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT                               num_rows  = 0;
        UINT64                             row_bytes = 0;
        UINT64                             total     = 0;
        m_device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &num_rows, &row_bytes, &total);
        ComPtr<ID3D12Resource> upload = make_upload_buffer(m_device.Get(), total);
        if (upload == nullptr) { return nullptr; }
        void* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, &mapped))) { return nullptr; }
        auto*            dst_base = static_cast<crd::u8*>(mapped) + fp.Offset;
        const auto*      src_base = static_cast<const crd::u8*>(rgba);
        const crd::usize src_row  = static_cast<crd::usize>(width) * 4U;
        for (crd::u32 y = 0; y < height; ++y)
        {
            std::memcpy(dst_base + static_cast<crd::usize>(y) * fp.Footprint.RowPitch, src_base + static_cast<crd::usize>(y) * src_row, src_row);
        }
        upload->Unmap(0, nullptr);

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = tex.Get();
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource       = upload.Get();
        src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        submit_and_wait();

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format                  = kColorFormat;
        srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels     = 1;
        return std::make_unique<Dx12Texture>(std::move(tex), width, height, srv);
    }

    // RET-2 (ADR-0105): the DXGI present seam. `native_window` must be a real HWND (DXGI has no headless surface).
    [[nodiscard]] std::unique_ptr<IPresentSurface> create_present_surface(void* native_window, crd::u32 width,
                                                                          crd::u32 height, PresentMode mode) override
    {
        if (!m_ok || native_window == nullptr) { return nullptr; }
        auto surface =
            std::make_unique<Dx12PresentSurface>(m_device.Get(), m_queue.Get(), native_window, width, height, mode);
        if (!surface->valid()) { return nullptr; }
        return surface;
    }

    // GEO-3 stage 4 / RET-3: the cooked chain uploads VERBATIM (one footprint copy per level, no device-side
    // re-derivation); `srgb` picks the _SRGB format so sampling hardware-decodes.
    [[nodiscard]] std::unique_ptr<ITexture> create_texture_from_mips(crd::u32 width, crd::u32 height, crd::u32 mip_count,
                                                                    const void* const* mips, bool srgb) override
    {
        if (!m_ok || width == 0U || height == 0U || mip_count == 0U || mip_count > 16U || mips == nullptr)
        {
            return nullptr;
        }
        for (crd::u32 i = 0; i < mip_count; ++i)
        {
            if (mips[i] == nullptr) { return nullptr; }
        }
        const DXGI_FORMAT fmt = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : kColorFormat;

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = width;
        rd.Height           = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = static_cast<UINT16>(mip_count);
        rd.Format           = fmt;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ComPtr<ID3D12Resource> tex;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&tex))))
        {
            return nullptr;
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fps[16]{};
        UINT                               rows[16]{};
        UINT64                             rowbytes[16]{};
        UINT64                             total = 0;
        m_device->GetCopyableFootprints(&rd, 0, mip_count, 0, fps, rows, rowbytes, &total);
        ComPtr<ID3D12Resource> upload = make_upload_buffer(m_device.Get(), total);
        if (upload == nullptr) { return nullptr; }
        void* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, &mapped))) { return nullptr; }
        {
            crd::u32 mw = width;
            crd::u32 mh = height;
            for (crd::u32 i = 0; i < mip_count; ++i)
            {
                auto*            dst_base = static_cast<crd::u8*>(mapped) + fps[i].Offset;
                const auto*      src_base = static_cast<const crd::u8*>(mips[i]);
                const crd::usize src_row  = static_cast<crd::usize>(mw) * 4U;
                for (crd::u32 y = 0; y < mh; ++y)
                {
                    std::memcpy(dst_base + static_cast<crd::usize>(y) * fps[i].Footprint.RowPitch,
                                src_base + static_cast<crd::usize>(y) * src_row, src_row);
                }
                mw = mw > 1U ? mw / 2U : 1U;
                mh = mh > 1U ? mh / 2U : 1U;
            }
        }
        upload->Unmap(0, nullptr);

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        for (crd::u32 i = 0; i < mip_count; ++i)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource        = tex.Get();
            dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = i;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource       = upload.Get();
            src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = fps[i];
            m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        transition(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        submit_and_wait();

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format                  = fmt;
        srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels     = mip_count;
        return std::make_unique<Dx12Texture>(std::move(tex), width, height, srv);
    }

    [[nodiscard]] std::unique_ptr<ITexture> create_texture_dim(TextureKind kind, crd::u32 width, crd::u32 height,
                                                               crd::u32 depth_or_layers, const void* rgba) override
    {
        if (!m_ok || width == 0U || height == 0U || rgba == nullptr) { return nullptr; }
        UINT                     depth   = 1;
        UINT                     layers  = 1;
        D3D12_RESOURCE_DIMENSION rdim    = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        D3D12_SRV_DIMENSION      srv_dim = D3D12_SRV_DIMENSION_TEXTURE2D;
        switch (kind)
        {
        case TextureKind::Tex1D:      rdim = D3D12_RESOURCE_DIMENSION_TEXTURE1D; srv_dim = D3D12_SRV_DIMENSION_TEXTURE1D; height = 1; break;
        case TextureKind::Tex2D:      break;
        case TextureKind::Tex3D:      rdim = D3D12_RESOURCE_DIMENSION_TEXTURE3D; srv_dim = D3D12_SRV_DIMENSION_TEXTURE3D; depth = depth_or_layers > 0U ? depth_or_layers : 1U; break;
        case TextureKind::Cube:       srv_dim = D3D12_SRV_DIMENSION_TEXTURECUBE;      layers = 6; break;
        case TextureKind::Tex2DArray: srv_dim = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;   layers = depth_or_layers > 0U ? depth_or_layers : 1U; break;
        case TextureKind::CubeArray:  srv_dim = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY; layers = depth_or_layers > 0U ? depth_or_layers : 6U; break;
        }

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = rdim;
        rd.Width            = width;
        rd.Height           = height;
        rd.DepthOrArraySize = static_cast<UINT16>(rdim == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? depth : layers);
        rd.MipLevels        = 1;
        rd.Format           = kColorFormat;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ComPtr<ID3D12Resource> tex;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&tex))))
        {
            return nullptr;
        }

        // Upload per subresource (3D = 1 subresource with `depth` planes; cube/array = one per layer).
        const UINT nsub = (rdim == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? 1U : layers;
        if (nsub > 32U) { return nullptr; }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fps[32]{};
        UINT                               rows[32]{};
        UINT64                             rowbytes[32]{};
        UINT64                             total = 0;
        m_device->GetCopyableFootprints(&rd, 0, nsub, 0, fps, rows, rowbytes, &total);
        ComPtr<ID3D12Resource> upload = make_upload_buffer(m_device.Get(), total);
        if (upload == nullptr) { return nullptr; }
        void* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, &mapped))) { return nullptr; }
        const auto* src     = static_cast<const crd::u8*>(rgba);
        crd::usize  src_off = 0;
        for (UINT s = 0; s < nsub; ++s)
        {
            auto*        dst    = static_cast<crd::u8*>(mapped) + fps[s].Offset;
            const UINT   pitch  = fps[s].Footprint.RowPitch;
            const UINT   nrows  = rows[s];
            const UINT   planes = fps[s].Footprint.Depth; // depth for 3D, 1 for a 2D layer
            const auto   rb     = static_cast<crd::usize>(rowbytes[s]);
            const UINT   splane = pitch * nrows;
            for (UINT z = 0; z < planes; ++z)
            {
                for (UINT r = 0; r < nrows; ++r)
                {
                    std::memcpy(dst + static_cast<crd::usize>(z) * splane + static_cast<crd::usize>(r) * pitch, src + src_off, rb);
                    src_off += rb;
                }
            }
        }
        upload->Unmap(0, nullptr);

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        for (UINT s = 0; s < nsub; ++s)
        {
            D3D12_TEXTURE_COPY_LOCATION d{};
            d.pResource        = tex.Get();
            d.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            d.SubresourceIndex = s;
            D3D12_TEXTURE_COPY_LOCATION sr{};
            sr.pResource       = upload.Get();
            sr.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            sr.PlacedFootprint = fps[s];
            m_list->CopyTextureRegion(&d, 0, 0, 0, &sr, nullptr);
        }
        transition(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        submit_and_wait();

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format                  = kColorFormat;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension           = srv_dim;
        switch (srv_dim)
        {
        case D3D12_SRV_DIMENSION_TEXTURE1D:        srv.Texture1D.MipLevels = 1; break;
        case D3D12_SRV_DIMENSION_TEXTURE3D:        srv.Texture3D.MipLevels = 1; break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE:      srv.TextureCube.MipLevels = 1; break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:   srv.Texture2DArray.MipLevels = 1; srv.Texture2DArray.ArraySize = layers; break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: srv.TextureCubeArray.MipLevels = 1; srv.TextureCubeArray.NumCubes = layers / 6U; break;
        default:                                   srv.Texture2D.MipLevels = 1; break;
        }
        return std::make_unique<Dx12Texture>(std::move(tex), width, height, srv);
    }

    void draw_textured(IRasterTarget& target, IRasterProgram& program, ClearColor clear, ITexture& texture,
                       crd::u32 vertex_count) override
    {
        draw_sampled(target, program, clear, static_cast<Dx12Texture&>(texture), 0U, vertex_count);
    }

    [[nodiscard]] std::unique_ptr<ITexture> create_depth_texture(crd::u32 width, crd::u32 height,
                                                                 const float* depth) override
    {
        if (!m_ok || width == 0U || height == 0U || depth == nullptr) { return nullptr; }
        // R32_FLOAT + a comparison sampler realises shadow-compare sampling on DX12 (no true depth format needed for SRVs).
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = width;
        rd.Height           = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_R32_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ComPtr<ID3D12Resource> tex;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&tex))))
        {
            return nullptr;
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT                               num_rows  = 0;
        UINT64                             row_bytes = 0;
        UINT64                             total     = 0;
        m_device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &num_rows, &row_bytes, &total);
        ComPtr<ID3D12Resource> upload = make_upload_buffer(m_device.Get(), total);
        if (upload == nullptr) { return nullptr; }
        void* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, &mapped))) { return nullptr; }
        auto*            dst_base = static_cast<crd::u8*>(mapped) + fp.Offset;
        const auto*      src_base = reinterpret_cast<const crd::u8*>(depth);
        const crd::usize src_row  = static_cast<crd::usize>(width) * 4U; // one float per texel
        for (crd::u32 y = 0; y < height; ++y)
        {
            std::memcpy(dst_base + static_cast<crd::usize>(y) * fp.Footprint.RowPitch, src_base + static_cast<crd::usize>(y) * src_row, src_row);
        }
        upload->Unmap(0, nullptr);

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = tex.Get();
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource       = upload.Get();
        src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        submit_and_wait();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format                  = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels     = 1;
        return std::make_unique<Dx12Texture>(std::move(tex), width, height, srv);
    }

    void draw_shadow(IRasterTarget& target, IRasterProgram& program, ClearColor clear, ITexture& depth,
                     crd::u32 vertex_count) override
    {
        draw_sampled(target, program, clear, static_cast<Dx12Texture&>(depth), 1U, vertex_count);
    }

    [[nodiscard]] bool supports_bindless() const noexcept override
    {
        return m_binding_tier >= D3D12_RESOURCE_BINDING_TIER_2;
    }

    void draw_bindless(IRasterTarget& target, IRasterProgram& program, ClearColor clear, ITexture* const* textures,
                       crd::u32 count, crd::u32 vertex_count) override
    {
        if (!m_ok || m_uav_heap == nullptr || m_sampler_heap == nullptr || count == 0U || textures == nullptr) { return; }
        auto&                t   = static_cast<Dx12RasterTarget&>(target);
        auto&                p   = static_cast<Dx12RasterProgram&>(program);
        ID3D12PipelineState* pso = p.pso_for(1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false);
        if (!p.valid() || pso == nullptr) { return; }
        const crd::u32 n = count < static_cast<crd::u32>(kBindlessMax) ? count : static_cast<crd::u32>(kBindlessMax);

        // Mint the bindless SRV array into heap slots 2..2+kBindlessMax-1 (elements 0..n-1 = textures, rest replicate #0).
        for (UINT i = 0; i < kBindlessMax; ++i)
        {
            auto& tex = static_cast<Dx12Texture&>(*textures[i < n ? i : 0U]);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = tex.srv();
            D3D12_CPU_DESCRIPTOR_HANDLE     h   = m_uav_heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(2U + i) * m_srv_inc; // bindless array starts at slot 2
            m_device->CreateShaderResourceView(tex.tex(), &srv, h);
        }
        D3D12_GPU_DESCRIPTOR_HANDLE bindless_gpu = m_uav_heap->GetGPUDescriptorHandleForHeapStart();
        bindless_gpu.ptr += static_cast<UINT64>(2U) * m_srv_inc; // slot 2

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        ID3D12DescriptorHeap* heaps[] = {m_uav_heap.Get(), m_sampler_heap.Get()};
        m_list->SetDescriptorHeaps(2, heaps);
        m_list->SetGraphicsRootDescriptorTable(2, m_sampler_heap->GetGPUDescriptorHandleForHeapStart()); // sampler s2
        m_list->SetGraphicsRootDescriptorTable(3, bindless_gpu);                                          // bindless t3[]
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);

        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.tex();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit_and_wait();
    }

    [[nodiscard]] std::unique_ptr<IGBufferTarget> create_gbuffer_target(crd::u32 width, crd::u32 height,
                                                                        crd::u32 attachments) override
    {
        if (!m_ok || width == 0U || height == 0U || attachments < 2U || attachments > kMaxGBuffer) { return nullptr; }
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = attachments;
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)))) { return nullptr; }
        const UINT rtv_inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        ComPtr<ID3D12Resource>             tex[kMaxGBuffer]{};
        ComPtr<ID3D12Resource>             rb[kMaxGBuffer]{};
        void*                              mapped[kMaxGBuffer]{};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[kMaxGBuffer]{};
        D3D12_HEAP_PROPERTIES              hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = width;
        rd.Height           = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = kColorFormat;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (crd::u32 i = 0; i < attachments; ++i)
        {
            if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                         nullptr, IID_PPV_ARGS(&tex[i]))))
            {
                return nullptr;
            }
            UINT   num_rows  = 0;
            UINT64 row_bytes = 0;
            UINT64 total     = 0;
            m_device->GetCopyableFootprints(&rd, 0, 1, 0, &fp[i], &num_rows, &row_bytes, &total);
            rb[i] = make_readback_buffer(m_device.Get(), total);
            if (rb[i] == nullptr || FAILED(rb[i]->Map(0, nullptr, &mapped[i]))) { return nullptr; }
            D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(i) * rtv_inc;
            m_device->CreateRenderTargetView(tex[i].Get(), nullptr, h);
        }
        return std::make_unique<Dx12GBufferTarget>(tex, rb, mapped, fp, std::move(rtv_heap), rtv_inc, attachments, width,
                                                   height);
    }

    void draw_gbuffer(IGBufferTarget& target, IRasterProgram& program, ClearColor clear, crd::u32 vertex_count) override
    {
        if (!m_ok) { return; }
        auto&                t   = static_cast<Dx12GBufferTarget&>(target);
        auto&                p   = static_cast<Dx12RasterProgram&>(program);
        const crd::u32       n   = t.attachment_count();
        ID3D12PipelineState* pso = p.pso_for(1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, n);
        if (!p.valid() || pso == nullptr) { return; }

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        for (crd::u32 i = 0; i < n; ++i) { transition(t.tex(i), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET); }
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = t.rtv_start();
        m_list->OMSetRenderTargets(n, &rtv0, TRUE, nullptr); // TRUE: the N RTVs are contiguous in one heap
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        for (crd::u32 i = 0; i < n; ++i) { m_list->ClearRenderTargetView(t.rtv(i), rgba, 0, nullptr); }

        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);

        for (crd::u32 i = 0; i < n; ++i) // copy each attachment to its readback
        {
            transition(t.tex(i), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource       = t.readback(i);
            dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = t.footprint(i);
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource        = t.tex(i);
            src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            transition(t.tex(i), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        submit_and_wait();
    }

    // B17-a: WEIGHTED-BLENDED OIT (McGuire-Bavoil 2013) — see raster_context.hpp. Two internal float targets (accum RGBA16F
    // additive · revealage R16F multiplicative) accumulate ALL transparent fragments in one order-independent pass; a
    // full-screen composite resolves `avg = accum.rgb/max(accum.a, eps)` over a `background`-cleared RGBA8 target.
    void draw_wboit(IRasterTarget& target, IRasterProgram& transparent, IRasterProgram& composite, ClearColor background,
                    crd::u32 vertex_count) override
    {
        if (!m_ok || m_uav_heap == nullptr || m_sampler_heap == nullptr) { return; }
        auto& t  = static_cast<Dx12RasterTarget&>(target);
        auto& tp = static_cast<Dx12RasterProgram&>(transparent);
        auto& cp = static_cast<Dx12RasterProgram&>(composite);
        if (!tp.valid() || !cp.valid()) { return; }
        const crd::u32 w = t.width();
        const crd::u32 h = t.height();

        ComPtr<ID3D12PipelineState> accum_pso =
            build_wboit_accum_pso(tp.device(), tp.root(), tp.vs_bytecode(), tp.ps_bytecode());
        ComPtr<ID3D12PipelineState> comp_pso =
            build_wboit_composite_pso(cp.device(), cp.root(), cp.vs_bytecode(), cp.ps_bytecode());
        if (accum_pso == nullptr || comp_pso == nullptr) { return; }

        // The two internal float targets (render target + shader resource).
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type       = D3D12_HEAP_TYPE_DEFAULT;
        const auto rt = [&](DXGI_FORMAT fmt) -> ComPtr<ID3D12Resource> {
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width            = w;
            rd.Height           = h;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.Format           = fmt;
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            ComPtr<ID3D12Resource> r;
            if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                         nullptr, IID_PPV_ARGS(&r))))
            {
                return nullptr;
            }
            return r;
        };
        ComPtr<ID3D12Resource> accum  = rt(DXGI_FORMAT_R16G16B16A16_FLOAT);
        ComPtr<ID3D12Resource> reveal = rt(DXGI_FORMAT_R16_FLOAT);
        if (accum == nullptr || reveal == nullptr) { return; }

        D3D12_DESCRIPTOR_HEAP_DESC rhd{};
        rhd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rhd.NumDescriptors = 2;
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        if (FAILED(m_device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)))) { return; }
        const UINT                  rtv_inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv0    = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE rtv1    = rtv0;
        rtv1.ptr += rtv_inc;
        m_device->CreateRenderTargetView(accum.Get(), nullptr, rtv0);
        m_device->CreateRenderTargetView(reveal.Get(), nullptr, rtv1);

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);

        // ---- Pass 1: accumulate (accum additively, revealage multiplicatively) ---------------------------------------
        transition(accum.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(reveal.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_list->OMSetRenderTargets(2, &rtv0, TRUE, nullptr); // the 2 RTVs are contiguous in one heap
        const float accum_clear[4]  = {0.0F, 0.0F, 0.0F, 0.0F};
        const float reveal_clear[4] = {1.0F, 1.0F, 1.0F, 1.0F}; // revealage starts at full 1
        m_list->ClearRenderTargetView(rtv0, accum_clear, 0, nullptr);
        m_list->ClearRenderTargetView(rtv1, reveal_clear, 0, nullptr);
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(w), static_cast<float>(h), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(tp.root());
        m_list->SetPipelineState(accum_pso.Get());
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);

        // ---- Pass 2: composite over the `background`-cleared target ---------------------------------------------------
        transition(accum.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(reveal.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_a{};
        srv_a.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_a.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_a.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv_a.Texture2D.MipLevels     = 1;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_r = srv_a;
        srv_r.Format                          = DXGI_FORMAT_R16_FLOAT;
        for (UINT i = 0; i < kBindlessMax; ++i) // bindless[0]=accum (slot 2), [1]=revealage (slot 3); rest replicate accum
        {
            D3D12_CPU_DESCRIPTOR_HANDLE hh = m_uav_heap->GetCPUDescriptorHandleForHeapStart();
            hh.ptr += static_cast<SIZE_T>(2U + i) * m_srv_inc;
            if (i == 1U) { m_device->CreateShaderResourceView(reveal.Get(), &srv_r, hh); }
            else { m_device->CreateShaderResourceView(accum.Get(), &srv_a, hh); }
        }
        D3D12_GPU_DESCRIPTOR_HANDLE bindless_gpu = m_uav_heap->GetGPUDescriptorHandleForHeapStart();
        bindless_gpu.ptr += static_cast<UINT64>(2U) * m_srv_inc;

        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE trtv = t.rtv();
        m_list->OMSetRenderTargets(1, &trtv, FALSE, nullptr);
        const float bg[4] = {background.r, background.g, background.b, background.a};
        m_list->ClearRenderTargetView(trtv, bg, 0, nullptr);
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(cp.root());
        ID3D12DescriptorHeap* heaps[] = {m_uav_heap.Get(), m_sampler_heap.Get()};
        m_list->SetDescriptorHeaps(2, heaps);
        m_list->SetGraphicsRootDescriptorTable(2, m_sampler_heap->GetGPUDescriptorHandleForHeapStart()); // sampler s2
        m_list->SetGraphicsRootDescriptorTable(3, bindless_gpu);                                          // bindless t3[]
        m_list->SetPipelineState(comp_pso.Get());
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(3, 1, 0, 0); // full-screen triangle

        // ---- Readback ------------------------------------------------------------------------------------------------
        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.tex();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        transition(accum.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        transition(reveal.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit_and_wait();
    }

private:
    // B2: clear `target`, bind the texture's SRV (heap slot 1) + the sampler at `sampler_slot`, draw, copy back. Shared by
    // draw_textured (default sampler slot 0) and draw_shadow (comparison sampler slot 1); the SRV dim/format ride the texture.
    void draw_sampled(IRasterTarget& target, IRasterProgram& program, ClearColor clear, Dx12Texture& tex,
                      UINT sampler_slot, crd::u32 vertex_count)
    {
        if (!m_ok || m_uav_heap == nullptr || m_sampler_heap == nullptr) { return; }
        auto&                t   = static_cast<Dx12RasterTarget&>(target);
        auto&                p   = static_cast<Dx12RasterProgram&>(program);
        ID3D12PipelineState* pso = p.pso_for(1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false);
        if (!p.valid() || pso == nullptr) { return; }

        // REN-2: in frame-graph recording mode, sampling records into the shared list (RTT compose / material forward).
        if (frame_recording()) { record_textured(t, p, pso, tex, sampler_slot, clear, vertex_count); return; }

        const D3D12_SHADER_RESOURCE_VIEW_DESC srv     = tex.srv();
        D3D12_CPU_DESCRIPTOR_HANDLE           srv_cpu = m_uav_heap->GetCPUDescriptorHandleForHeapStart();
        srv_cpu.ptr += static_cast<SIZE_T>(m_srv_inc); // slot 1
        m_device->CreateShaderResourceView(tex.tex(), &srv, srv_cpu);
        D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = m_uav_heap->GetGPUDescriptorHandleForHeapStart();
        srv_gpu.ptr += static_cast<UINT64>(m_srv_inc);
        D3D12_GPU_DESCRIPTOR_HANDLE samp_gpu = m_sampler_heap->GetGPUDescriptorHandleForHeapStart();
        samp_gpu.ptr += static_cast<UINT64>(sampler_slot) * m_sampler_inc;

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        ID3D12DescriptorHeap* heaps[] = {m_uav_heap.Get(), m_sampler_heap.Get()};
        m_list->SetDescriptorHeaps(2, heaps);
        m_list->SetGraphicsRootDescriptorTable(1, srv_gpu);  // SRV table (t1)
        m_list->SetGraphicsRootDescriptorTable(2, samp_gpu); // sampler table (s2)
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);

        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.tex();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit_and_wait();
    }

    void transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        // ⛔ REN-3.2: ALL_SUBRESOURCES, not 0. A layered transient is ONE graph node, so its transitions must
        // cover EVERY slice — `Subresource = 0` moved only cascade 0 out of DEPTH_WRITE and left slices 1..N-1
        // in the wrong state when the lighting pass sampled the array.
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter  = after;
        m_list->ResourceBarrier(1, &b);
    }

    // REN-8: submit WITHOUT blocking. The caller decides when to wait — a presenting frame defers it to just
    // before the next frame resets this allocator/list, so the CPU builds frame N+1 while the GPU runs N.
    void submit_no_wait()
    {
        m_list->Close();
        ID3D12CommandList* lists[] = {m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        ++m_fence_val;
        m_queue->Signal(m_fence.Get(), m_fence_val);
    }
    // Block until everything submitted so far has completed.
    void wait_submitted()
    {
        if (m_fence->GetCompletedValue() < m_fence_val)
        {
            m_fence->SetEventOnCompletion(m_fence_val, m_event);
            WaitForSingleObject(m_event, INFINITE);
        }
    }
    void submit_and_wait()
    {
        submit_no_wait();
        wait_submitted();
    }

    // ── REN-1 pt-2 (D-007 row 98): the frame-graph RECORDING MODE ─────────────────────────────────────────────────
    // When a frame graph is executing, the public draw_storage_depth / _load record into ONE shared open command
    // list (m_list, already Reset) with a per-draw slot from the graph's frame descriptor heap — NO per-draw
    // Reset/transition/copy/submit. The DX12 analog of the Vulkan frame descriptor pool: the single storage-UAV
    // heap slot is consumed at EXECUTE time, so each recorded draw needs its OWN heap slot (else all draws read the
    // last-written descriptor). The graph owns the frame heap + passes it in; the context bumps a per-frame cursor.
    struct FrameRec
    {
        ID3D12DescriptorHeap* heap   = nullptr; // the graph's shader-visible CBV/SRV/UAV heap (kFrameSlots UAVs)
        UINT                  inc    = 0;        // descriptor increment size
        UINT                  cursor = 0;        // next free slot (reset once per execute)
        bool                  active = false;    // a frame graph is recording
    };
    FrameRec m_frame_rec{};

public:
    // The graph brackets a frame's recording with these (defined out-of-line in Dx12FrameGraph). While recording,
    // draw_storage_depth / _load record into the shared list; the graph inserts barriers + submits ONCE.
    void frame_rec_begin(ID3D12DescriptorHeap* heap, UINT inc)
    {
        // ── REN-8: FRAMES IN FLIGHT (the DX12 half). ────────────────────────────────────────────────────────
        // The frame graph records into its OWN ring of allocator+list pairs, swapped in here and restored in
        // frame_rec_end(). The standalone synchronous draw paths keep the context's dedicated pair untouched —
        // they Reset-then-submit-and-wait, so sharing the ring with them would let a Reset land on an allocator
        // the deferred graph submission is still consuming (a use-after-free that renders fine almost always).
        //
        // ⛔ Each slot remembers the FENCE VALUE of its own submission. Reusing a slot waits for THAT value, not
        // the latest one — waiting for the latest would mean waiting for the frame we just submitted, i.e. no
        // pipelining at all, which is the bug this ring exists to avoid.
        if (m_ring_list[0] != nullptr)
        {
            m_ring_slot = (m_ring_slot + 1U) % kFrameRingSize;
            if (m_fence->GetCompletedValue() < m_ring_val[m_ring_slot])
            {
                m_fence->SetEventOnCompletion(m_ring_val[m_ring_slot], m_event);
                WaitForSingleObject(m_event, INFINITE);
            }
            m_saved_alloc = m_cmd_alloc;
            m_saved_list  = m_list;
            m_cmd_alloc   = m_ring_alloc[m_ring_slot];
            m_list        = m_ring_list[m_ring_slot];
        }
        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        m_frame_rec.heap   = heap;
        m_frame_rec.inc    = inc;
        m_frame_rec.cursor = 0U;
        m_frame_rec.active = true;
        // REN-2: bind BOTH the frame heap (SRV/UAV per-draw ring) and the context sampler heap (record_textured's s2).
        ID3D12DescriptorHeap* heaps[] = {heap, m_sampler_heap.Get()};
        m_list->SetDescriptorHeaps(m_sampler_heap != nullptr ? 2U : 1U, heaps);
    }
    void frame_rec_end() noexcept
    {
        m_frame_rec = FrameRec{};
        // REN-8: record THIS slot's fence value, then hand the context's dedicated pair back to the standalone
        // paths. `m_fence_val` was just bumped by the submit, so it is exactly this frame's completion value.
        if (m_saved_list != nullptr)
        {
            m_ring_val[m_ring_slot] = m_fence_val;
            m_cmd_alloc             = m_saved_alloc;
            m_list                  = m_saved_list;
            m_saved_alloc.Reset();
            m_saved_list.Reset();
        }
    }
    [[nodiscard]] bool frame_recording() const noexcept { return m_frame_rec.active; }

    // The graph's cross-pass barrier hook + end-of-frame readback (so read_pixel is bit-identical to the sync path).
    void frame_transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        transition(res, before, after);
    }
    void frame_readback(Dx12RasterTarget& t)
    {
        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = t.readback();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.tex();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    void                  frame_submit() { submit_and_wait(); } // ONE ExecuteCommandLists for the whole frame
    // REN-8: the frame graph writes timestamps into the SHARED recording list and needs the queue's tick rate.
    [[nodiscard]] ID3D12GraphicsCommandList* frame_cmd_list() const noexcept { return m_list.Get(); }
    [[nodiscard]] ID3D12CommandQueue*        frame_cmd_queue() const noexcept { return m_queue.Get(); }
    // REN-8: the deferred-wait pair — ONE ExecuteCommandLists, the block moved to the next frame's start.
    void                  frame_submit_no_wait() { submit_no_wait(); }
    void                  frame_wait_submitted() { wait_submitted(); }
    [[nodiscard]] ID3D12Device* dx_device() const noexcept { return m_device.Get(); }

    // REN-1 pt-2: create a DX12 frame graph bound to this context (defined out-of-line after Dx12FrameGraph).
    [[nodiscard]] std::unique_ptr<IFrameGraph> create_frame_graph() override;

private:
    // Allocate the next frame-heap slot, point it at `s`'s UAV, return its GPU handle (for the root table).
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE frame_alloc_storage_slot(Dx12StorageBuffer& s)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format                     = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement        = 0;
        uav.Buffer.NumElements         = s.num_elements();
        uav.Buffer.StructureByteStride = 4;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_frame_rec.heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(m_frame_rec.cursor) * m_frame_rec.inc;
        m_device->CreateUnorderedAccessView(s.buf(), nullptr, &uav, cpu);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_frame_rec.heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(m_frame_rec.cursor) * m_frame_rec.inc;
        ++m_frame_rec.cursor;
        return gpu;
    }

    // The frame-mode body of draw_storage_depth (clear=true) / _load (clear=false): record into the shared list,
    // no per-draw reset/transition/copy/submit. Consecutive draws to the same RTV are rasterization-ordered by DX12
    // (no self-barrier needed — the storage buffer is read-only vertex-pull data); the target was transitioned to
    // RENDER_TARGET by the graph before the pass, and depth stays in DEPTH_WRITE (created that way).
    void record_scene(Dx12RasterTarget& t, Dx12RasterProgram& p, Dx12StorageBuffer& s, ID3D12PipelineState* pso,
                      bool clear, ClearColor clear_color, float clear_depth, crd::u32 vertex_count)
    {
        const D3D12_GPU_DESCRIPTOR_HANDLE table = frame_alloc_storage_slot(s);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv   = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv   = t.dsv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        if (clear)
        {
            const float rgba[4] = {clear_color.r, clear_color.g, clear_color.b, clear_color.a};
            m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
            m_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, clear_depth, 0, 0, nullptr);
        }
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, table);
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);
    }

    // REN-3.1: the frame-mode body of draw_storage_depth_only — the SHADOW PASS. Binds NO render target
    // (`OMSetRenderTargets(0, nullptr, FALSE, &dsv)`) and clears + writes only depth. The PSO carries
    // NumRenderTargets = 0 (see the pso_for(..., num_rts = 0) call in draw_storage_depth_only).
    void record_depth_only(Dx12RasterTarget& t, Dx12RasterProgram& p, Dx12StorageBuffer& s,
                           ID3D12PipelineState* pso, bool clear, float clear_depth, crd::u32 vertex_count)
    {
        const D3D12_GPU_DESCRIPTOR_HANDLE table = frame_alloc_storage_slot(s);
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv   = t.dsv();
        m_list->OMSetRenderTargets(0, nullptr, FALSE, &dsv); // ⛔ zero RTVs: depth-only
        // clear ⇒ the FIRST mesh of the shadow pass; otherwise every later mesh joins the SAME map (clearing per
        // draw would wipe the previous occluder — see draw_storage_depth_only_load).
        if (clear) { m_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, clear_depth, 0, 0, nullptr); }
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, table);
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);
    }

    // REN-2: allocate the next frame-heap slot, write `tex`'s SRV into it, return its GPU handle (for root table 1).
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE frame_alloc_srv_slot(Dx12Texture& tex)
    {
        const D3D12_SHADER_RESOURCE_VIEW_DESC srv = tex.srv();
        D3D12_CPU_DESCRIPTOR_HANDLE           cpu = m_frame_rec.heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(m_frame_rec.cursor) * m_frame_rec.inc;
        m_device->CreateShaderResourceView(tex.tex(), &srv, cpu);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_frame_rec.heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(m_frame_rec.cursor) * m_frame_rec.inc;
        ++m_frame_rec.cursor;
        return gpu;
    }

    // REN-2: the frame-mode body of draw_storage — a COLOR-ONLY render into an RTT transient (no depth, no readback),
    // into the shared list. Pass 1 of render-to-texture; a later pass samples it via record_textured.
    void record_offscreen(Dx12RasterTarget& t, Dx12RasterProgram& p, Dx12StorageBuffer& s, ID3D12PipelineState* pso,
                          ClearColor clear_color, crd::u32 vertex_count)
    {
        const D3D12_GPU_DESCRIPTOR_HANDLE table = frame_alloc_storage_slot(s);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv   = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float rgba[4] = {clear_color.r, clear_color.g, clear_color.b, clear_color.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, table);
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);
    }

    // REN-2: the frame-mode body of draw_sampled — bind a SAMPLED image (an RTT transient or a material map, SRV at
    // frame-heap slot / root table 1) + sampler (root table 2), COLOR-render into the target, into the shared list.
    void record_textured(Dx12RasterTarget& t, Dx12RasterProgram& p, ID3D12PipelineState* pso, Dx12Texture& tex,
                         UINT sampler_slot, ClearColor clear_color, crd::u32 vertex_count)
    {
        const D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu  = frame_alloc_srv_slot(tex);
        D3D12_GPU_DESCRIPTOR_HANDLE       samp_gpu = m_sampler_heap->GetGPUDescriptorHandleForHeapStart();
        samp_gpu.ptr += static_cast<UINT64>(sampler_slot) * m_sampler_inc;
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float rgba[4] = {clear_color.r, clear_color.g, clear_color.b, clear_color.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(1, srv_gpu);  // SRV table (t1)
        m_list->SetGraphicsRootDescriptorTable(2, samp_gpu); // sampler table (s2)
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);
    }

    // REN-2 Half B: the frame-mode body of draw_storage_textured_depth (clear) / _load (no clear) — a depth-tested
    // scene draw that ALSO samples a material texture: storage UAV (table 0) from the ring + SRV (table 1) from the
    // ring + sampler (table 2). Into the shared list; the graph transitioned the target to RENDER_TARGET.
    // REN-3.2-b: sampler_slot 0 = the default FILTERING sampler (textured scene draw), 1 = the COMPARISON
    // sampler (shadowed scene draw). Same tables either way, so one recording path serves both.
    void record_scene_textured(Dx12RasterTarget& t, Dx12RasterProgram& p, Dx12StorageBuffer& s, Dx12Texture& tex,
                               ID3D12PipelineState* pso, bool clear, ClearColor clear_color, float clear_depth,
                               crd::u32 vertex_count, crd::u32 sampler_slot = 0U)
    {
        const D3D12_GPU_DESCRIPTOR_HANDLE uav_table = frame_alloc_storage_slot(s);
        const D3D12_GPU_DESCRIPTOR_HANDLE srv_table = frame_alloc_srv_slot(tex);
        D3D12_GPU_DESCRIPTOR_HANDLE samp_gpu = m_sampler_heap->GetGPUDescriptorHandleForHeapStart();
        samp_gpu.ptr += static_cast<UINT64>(sampler_slot) * m_sampler_inc;
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv       = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv       = t.dsv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        if (clear)
        {
            const float rgba[4] = {clear_color.r, clear_color.g, clear_color.b, clear_color.a};
            m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
            m_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, clear_depth, 0, 0, nullptr);
        }
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, uav_table); // storage (u0)
        m_list->SetGraphicsRootDescriptorTable(1, srv_table); // base-color SRV (t1)
        m_list->SetGraphicsRootDescriptorTable(2, samp_gpu);  // sampler (s2)
        m_list->SetPipelineState(pso);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);
    }

    ComPtr<ID3D12Device>               m_device;
    ComPtr<ID3D12Device2>              m_device2; // B4: CreatePipelineState (stream PSO) for mesh — null on an old runtime
    ComPtr<ID3D12CommandQueue>         m_queue;
    ComPtr<ID3D12CommandAllocator>     m_cmd_alloc;
    // REN-8: the frame graph's per-frame allocator+list RING, plus the dedicated pair saved while it is swapped
    // in (see frame_rec_begin). The standalone synchronous draw paths always use the dedicated pair.
    static constexpr crd::u32          kFrameRingSize = 2U;
    ComPtr<ID3D12CommandAllocator>     m_ring_alloc[kFrameRingSize];
    ComPtr<ID3D12GraphicsCommandList>  m_ring_list[kFrameRingSize];
    crd::u64                           m_ring_val[kFrameRingSize]{};
    crd::u32                           m_ring_slot = 0U;
    ComPtr<ID3D12CommandAllocator>     m_saved_alloc;
    ComPtr<ID3D12GraphicsCommandList>  m_saved_list;
    ComPtr<ID3D12GraphicsCommandList>  m_list;
    ComPtr<ID3D12GraphicsCommandList5> m_list5; // B1-e: RSSetShadingRate(Image) — null on an old runtime
    ComPtr<ID3D12GraphicsCommandList6> m_list6; // B4: DispatchMesh — null on an old runtime
    ComPtr<ID3D12CommandSignature>     m_mesh_indirect_sig; // B4: DISPATCH_MESH command signature for ExecuteIndirect (lazy)
    bool                               m_mesh_shader = false; // B4: D3D12_FEATURE_D3D12_OPTIONS7 MeshShaderTier supported
    ComPtr<ID3D12Fence>                m_fence;
    HANDLE                             m_event     = nullptr;
    crd::u64                           m_fence_val = 0;
    D3D12_VARIABLE_SHADING_RATE_TIER            m_vrs_tier      = D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED; // B1-e
    UINT                               m_vrs_tile_size = 0;                                     // B1-e: square tile edge
    D3D12_CONSERVATIVE_RASTERIZATION_TIER m_conservative_tier = D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED; // B1-f
    bool                               m_rov           = false; // B1-f: rasterizer-ordered views (fragment interlock analog)
    D3D12_RESOURCE_BINDING_TIER        m_binding_tier  = D3D12_RESOURCE_BINDING_TIER_1; // B2-d: Tier 2+ ⇒ bindless
    ComPtr<ID3D12DescriptorHeap>       m_uav_heap;              // B1-f/B2: shader-visible heap — slot0 UAV (storage) · slot1 SRV (texture)
    ComPtr<ID3D12DescriptorHeap>       m_sampler_heap;          // B2: shader-visible sampler heap — slot0 default · slot1 comparison
    UINT                               m_srv_inc = 0;           // B2: CBV/SRV/UAV descriptor increment size
    UINT                               m_sampler_inc = 0;       // B2-b: sampler descriptor increment size
    bool                               m_ok            = false;
};

// ── REN-1 pt-2 (D-007 row 98): the DX12 FRAME GRAPH ───────────────────────────────────────────────────────────────
// The DX12 mirror of VulkanFrameGraph: records a frame's passes into ONE command list (replacing the synchronous
// submit+wait-per-draw substrate) with automatic cross-pass resource barriers, plus graph-owned TRANSIENT resources
// whose backing D3D12 heap is ALIASED (placed resources) when their lifetimes are disjoint. Passes record via the
// raster context in frame-recording mode. Executes passes in DECLARATION ORDER (a valid topological order by the API
// contract — a pass is declared after its producers).

// REN-2: a color transient used as a render target is a BORROWED Dx12RasterTarget over the placed resource (built in
// Dx12FrameGraph::build; ComPtr refcounting keeps it alive); `sampled` transients also get a borrowed Dx12Texture SRV.

// A minimal IStorageBuffer adapter over a transient buffer (aliased + tracked; the drawn/compute-written path is later).
class Dx12TransientBuffer final : public IStorageBuffer
{
public:
    explicit Dx12TransientBuffer(crd::u32 size) noexcept : m_size(size) {}
    [[nodiscard]] crd::u32 size_bytes() const noexcept override { return m_size; }
    [[nodiscard]] crd::u32 read_u32(crd::u32) const noexcept override { return 0U; }
private:
    crd::u32 m_size = 0;
};

class Dx12FrameGraph final : public IFrameGraph, public IFrameContext
{
public:
    explicit Dx12FrameGraph(Dx12RasterContext& rc) : m_rc(&rc), m_device(rc.dx_device())
    {
        // the graph's shader-visible frame descriptor heap: one storage UAV per recorded draw (reset each execute).
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kFrameSlots;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_frame_heap));
        m_srv_inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // ── REN-8: per-pass GPU timestamps, the DX12 half of the attribution machinery. ──
        // A TIMESTAMP query heap + a READBACK buffer that `ResolveQueryData` writes ticks into. A queue whose
        // `GetTimestampFrequency` fails cannot timestamp at all — then `gpu_timing_available()` stays false and
        // the frame renders exactly as before. Timing must never be able to change what is drawn.
        D3D12_QUERY_HEAP_DESC qhd{};
        qhd.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qhd.Count = kMaxTimedPasses * 2U;
        UINT64 freq = 0;
        if (SUCCEEDED(m_device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&m_ts_heap)))
            && rc.frame_cmd_queue() != nullptr && SUCCEEDED(rc.frame_cmd_queue()->GetTimestampFrequency(&freq))
            && freq != 0U)
        {
            m_ts_freq = static_cast<double>(freq);
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width            = sizeof(UINT64) * kMaxTimedPasses * 2U;
            rd.Height           = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.Format           = DXGI_FORMAT_UNKNOWN;
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&m_ts_readback))))
            {
                m_ts_heap.Reset();
            }
        }
        else { m_ts_heap.Reset(); }
    }
    // ⛔ never release a placed resource / heap with work still in flight
    ~Dx12FrameGraph() override
    {
        wait_pending_submit();
        free_transients();
        // REN-37.5: the persistent registry is the one thing `reset()` never touches, so the DESTRUCTOR is the
        // only place it is released.
        for (Persistent& p : m_persist) { destroy_persistent_impl(p); }
        m_persist.clear();
    }
    Dx12FrameGraph(const Dx12FrameGraph&)            = delete;
    Dx12FrameGraph& operator=(const Dx12FrameGraph&) = delete;
    Dx12FrameGraph(Dx12FrameGraph&&)                 = delete;
    Dx12FrameGraph& operator=(Dx12FrameGraph&&)      = delete;

    // ── IFrameContext ──
    [[nodiscard]] IRasterContext& raster() noexcept override { return *m_rc; }
    [[nodiscard]] IRasterTarget*  image(FgImage h) noexcept override
    {
        if (!h.valid() || h.id > m_images.size()) { return nullptr; }
        return m_images[h.id - 1U].target;
    }
    [[nodiscard]] ITexture*       texture(FgImage h) noexcept override // REN-2: a `sampled` transient resolves to its SRV
    {
        if (!h.valid() || h.id > m_images.size()) { return nullptr; }
        return m_images[h.id - 1U].texture;
    }
    [[nodiscard]] IStorageBuffer* buffer(FgBuffer h) noexcept override
    {
        if (!h.valid() || h.id > m_buffers.size()) { return nullptr; }
        return m_buffers[h.id - 1U].buffer;
    }
    // REN-3.2: one SLICE of a layered transient as a render target (the per-cascade shadow write). A non-layered
    // image has no layer_targets, so layer 0 falls through to the single target — `image_layer(h,0)` is exactly
    // `image(h)` there, which is what lets a for_each-expanded pass use ONE code path for both shapes.
    [[nodiscard]] IRasterTarget* image_layer(FgImage h, crd::u32 layer) noexcept override
    {
        if (!h.valid() || h.id > m_images.size()) { return nullptr; }
        ImageNode& n = m_images[h.id - 1U];
        if (layer < n.layer_targets.size()) { return n.layer_targets[layer]; }
        return layer == 0U ? n.target : nullptr;
    }

    // ── IFrameGraph ──
    [[nodiscard]] FgImage import_target(IRasterTarget& target) override
    {
        for (crd::usize i = 0; i < m_images.size(); ++i)
        {
            if (m_images[i].target == &target) { return FgImage{static_cast<crd::u32>(i + 1U)}; }
        }
        ImageNode n{};
        n.target = &target;
        n.own    = Own::Imported;
        m_images.push_back(n);
        return FgImage{static_cast<crd::u32>(m_images.size())};
    }
    [[nodiscard]] FgBuffer import_storage(IStorageBuffer& buffer) override
    {
        for (crd::usize i = 0; i < m_buffers.size(); ++i)
        {
            if (m_buffers[i].buffer == &buffer) { return FgBuffer{static_cast<crd::u32>(i + 1U)}; }
        }
        BufferNode n{};
        n.buffer    = &buffer;
        n.transient = false;
        m_buffers.push_back(n);
        return FgBuffer{static_cast<crd::u32>(m_buffers.size())};
    }

    [[nodiscard]] FgImage create_transient_image(const FgImageDesc& desc) override
    {
        if (desc.width == 0U || desc.height == 0U) { return FgImage{0U}; }
        // REN-3.2: reject a bad layer count by RETURN VALUE — an invalid handle build() then refuses — rather
        // than clamping. A silently truncated cascade atlas renders a plausible-looking image with missing
        // cascades, which is the worst class of graphics bug: it looks like art direction.
        if (desc.layers == 0U || desc.layers > kFgMaxImageLayers) { return FgImage{0U}; }
        ImageNode n{};
        n.own  = Own::Transient;
        n.desc = desc;
        bool is_depth = false;
        n.fmt       = to_dxgi_format(desc.format, is_depth);
        n.is_depth  = is_depth;
        n.rdesc                  = {};
        n.rdesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        n.rdesc.Width            = desc.width;
        n.rdesc.Height           = desc.height;
        n.rdesc.DepthOrArraySize = static_cast<UINT16>(desc.layers); // REN-3.2: the 2D-ARRAY cascade atlas
        n.rdesc.MipLevels        = 1;
        // ⛔ REN-3.1 — THE DX12 DEPTH-SRV RULE. A D3D12 resource created with a fully-TYPED depth format
        // (DXGI_FORMAT_D32_FLOAT) can never have a Shader-Resource View created over it, so a `sampled` depth
        // transient would fail SRV creation (or hand back garbage). Sampling a depth target requires THREE formats
        // over ONE resource:
        //     resource = R32_TYPELESS   ·   DSV = D32_FLOAT   ·   SRV = R32_FLOAT
        // Vulkan needs no equivalent — one VK_FORMAT_D32_SFLOAT image serves both uses via aspect + layout — so this
        // asymmetry is DX12-only and is the single largest piece of REN-3.1.
        // A depth transient that is NOT sampled keeps the typed format (simpler, and it is what the depth buffer
        // path already does).
        const bool depth_sampled = is_depth && desc.sampled;
        n.depth_sampled          = depth_sampled;
        n.rdesc.Format           = depth_sampled ? DXGI_FORMAT_R32_TYPELESS : n.fmt;
        n.rdesc.SampleDesc.Count = 1;
        n.rdesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        n.rdesc.Flags            = is_depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const D3D12_RESOURCE_ALLOCATION_INFO ai = m_device->GetResourceAllocationInfo(0, 1, &n.rdesc);
        n.mem_size  = ai.SizeInBytes;
        n.mem_align = ai.Alignment;
        m_images.push_back(n);
        return FgImage{static_cast<crd::u32>(m_images.size())};
    }
    // ── REN-37.5: PERSISTENT images - created ONCE, kept across `reset()`, never aliased, never retired. ──
    // The DX12 mirror of the Vulkan contract: same keys, same semantics, same `was_live` reporting. The only
    // backend difference is COMMITTED vs PLACED - a persistent resource cannot live in the aliasing heap, which
    // is precisely what makes it persistent.
    [[nodiscard]] FgImage create_persistent_image(crd::u32 key, const FgImageDesc& desc) override
    {
        if (desc.width == 0U || desc.height == 0U) { return FgImage{0U}; }
        if (desc.layers == 0U || desc.layers > kFgMaxImageLayers) { return FgImage{0U}; }

        crd::i32 found = -1;
        for (crd::u32 i = 0; i < m_persist.size(); ++i)
        {
            if (m_persist[i].key != key) { continue; }
            const FgImageDesc& d = m_persist[i].node.desc;
            // A desc change (a resize, a format switch) genuinely INVALIDATES the history; reusing a
            // differently-shaped image would read plausible-looking garbage rather than obviously-missing data.
            if (d.width == desc.width && d.height == desc.height && d.format == desc.format
                && d.layers == desc.layers && d.sampled == desc.sampled && d.storage == desc.storage)
            {
                found = static_cast<crd::i32>(i);
            }
            else { destroy_persistent_impl(m_persist[i]); m_persist[i].key = 0U; }
            break;
        }

        if (found < 0)
        {
            Persistent p{};
            p.key            = key;
            p.node.own = Own::Persistent;
            p.node.desc      = desc;
            bool is_depth    = false;
            p.node.fmt       = to_dxgi_format(desc.format, is_depth);
            p.node.is_depth  = is_depth;
            const bool depth_sampled  = is_depth && desc.sampled;
            p.node.depth_sampled      = depth_sampled;
            p.node.rdesc              = {};
            p.node.rdesc.Dimension    = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            p.node.rdesc.Width        = desc.width;
            p.node.rdesc.Height       = desc.height;
            p.node.rdesc.DepthOrArraySize = static_cast<UINT16>(desc.layers);
            p.node.rdesc.MipLevels    = 1;
            p.node.rdesc.Format       = depth_sampled ? DXGI_FORMAT_R32_TYPELESS : p.node.fmt;
            p.node.rdesc.SampleDesc.Count = 1;
            p.node.rdesc.Layout       = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            p.node.rdesc.Flags = is_depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            const D3D12_RESOURCE_STATES init = is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_COMMON;
            if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &p.node.rdesc, init, nullptr,
                                                         IID_PPV_ARGS(&p.node.resource))))
            {
                return FgImage{0U};
            }
            p.node.state = init;
            crd::i32 dead = -1;
            for (crd::u32 i = 0; i < m_persist.size(); ++i)
            {
                if (m_persist[i].key == 0U) { dead = static_cast<crd::i32>(i); break; }
            }
            if (dead >= 0) { m_persist[static_cast<crd::u32>(dead)] = static_cast<Persistent&&>(p); found = dead; }
            else
            {
                m_persist.push_back(static_cast<Persistent&&>(p));
                found = static_cast<crd::i32>(m_persist.size() - 1U);
            }
            if (!materialize_image(m_persist[static_cast<crd::u32>(found)].node)) { return FgImage{0U}; }
            m_persist[static_cast<crd::u32>(found)].was_live = false;
        }
        else { m_persist[static_cast<crd::u32>(found)].was_live = true; }

        Persistent& pe = m_persist[static_cast<crd::u32>(found)];
        ImageNode   n{};
        n.target        = pe.node.target;
        n.texture       = pe.node.texture;
        n.rtv_heap      = pe.node.rtv_heap;
        n.dsv_heap      = pe.node.dsv_heap;
        for (IRasterTarget* t : pe.node.layer_targets) { n.layer_targets.push_back(t); }
        n.own           = Own::Persistent;
        n.persist_index = found;
        n.desc          = pe.node.desc;
        n.fmt           = pe.node.fmt;
        n.is_depth      = pe.node.is_depth;
        n.depth_sampled = pe.node.depth_sampled;
        n.rdesc         = pe.node.rdesc;
        n.mem_size      = pe.node.mem_size;
        n.mem_align     = pe.node.mem_align;
        n.resource      = pe.node.resource;
        n.state         = pe.node.state; // carried ACROSS the frame boundary - see the write-back in execute()
        m_images.push_back(static_cast<ImageNode&&>(n));
        return FgImage{static_cast<crd::u32>(m_images.size())};
    }

    [[nodiscard]] bool persistent_image_was_live(crd::u32 key) const noexcept override
    {
        for (crd::u32 i = 0; i < m_persist.size(); ++i)
        {
            if (m_persist[i].key == key) { return m_persist[i].was_live; }
        }
        return false;
    }

    [[nodiscard]] FgBuffer create_transient_buffer(crd::u32 size_bytes) override
    {
        if (size_bytes == 0U) { return FgBuffer{0U}; }
        BufferNode n{};
        n.transient              = true;
        n.size                   = size_bytes;
        n.rdesc                  = {};
        n.rdesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        n.rdesc.Width            = size_bytes;
        n.rdesc.Height           = 1;
        n.rdesc.DepthOrArraySize = 1;
        n.rdesc.MipLevels        = 1;
        n.rdesc.Format           = DXGI_FORMAT_UNKNOWN;
        n.rdesc.SampleDesc.Count = 1;
        n.rdesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        n.rdesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const D3D12_RESOURCE_ALLOCATION_INFO ai = m_device->GetResourceAllocationInfo(0, 1, &n.rdesc);
        n.mem_size  = ai.SizeInBytes;
        n.mem_align = ai.Alignment;
        m_buffers.push_back(n);
        return FgBuffer{static_cast<crd::u32>(m_buffers.size())};
    }

    [[nodiscard]] IFramePassBuilder& add_pass(const char* name, FgPassKind kind) override
    {
        Pass p{};
        p.name = name;
        p.kind = kind;
        m_passes.push_back(p);
        m_builder.bind(this, m_passes.size() - 1U);
        return m_builder;
    }

    [[nodiscard]] bool build() override;
    void               execute() override;
    void               reset() override
    {
        wait_pending_submit(); // ⛔ free_transients() releases resources the in-flight frame may still read
        free_transients();
        m_images.clear();
        m_buffers.clear();
        m_passes.clear();
        m_barrier_count = 0U;
    }

    [[nodiscard]] crd::u32 last_barrier_count() const noexcept override { return m_barrier_count; }
    [[nodiscard]] crd::u32 last_submit_count() const noexcept override { return m_submit_count; }
    [[nodiscard]] crd::u32 transient_memory_bytes() const noexcept override { return m_physical_bytes; }
    [[nodiscard]] crd::u32 transient_logical_bytes() const noexcept override { return m_logical_bytes; }
    // REN-8: same opt-out as Vulkan — the per-frame full-target host copy is a TEST affordance, so a presenting
    // app turns it off. Default true keeps every readback-asserting gate's semantics unchanged.
    void                   set_readback_enabled(bool on) noexcept override { m_readback = on; }

    // ── REN-8: GPU timing (mirrors the Vulkan contract exactly) ──
    [[nodiscard]] crd::u32 pass_count() const noexcept override { return m_timed_passes; }
    [[nodiscard]] const char* pass_name(crd::u32 i) const noexcept override
    {
        return i < m_timed_passes ? m_pass_names[i] : nullptr;
    }
    [[nodiscard]] double pass_gpu_ms(crd::u32 i) const noexcept override
    {
        return i < m_timed_passes ? m_pass_ms[i] : 0.0;
    }
    [[nodiscard]] double gpu_ms_total() const noexcept override { return m_gpu_ms_total; }
    [[nodiscard]] bool   gpu_timing_available() const noexcept override { return m_ts_heap != nullptr; }

    void wait_pending_submit() noexcept; // defined next to execute()
    void resolve_timestamps() noexcept;

private:
    // one physical memory slot (one ID3D12Heap) reusable across disjoint-lifetime transients
    struct Slot
    {
        ComPtr<ID3D12Heap> heap;
        UINT64             size       = 0;
        crd::i32           free_after  = -1; // the last pass index whose transient occupied this slot (-1 = free)
    };
    // ⛔ WHO OWNS THIS, and FOR HOW LONG. This used to be a single `bool transient`, and that bool silently
    // conflated THREE INDEPENDENT QUESTIONS:
    //     · is the resource GRAPH-OWNED?   -> RTT barrier semantics, and NO end-of-frame readback (a borrowed
    //                                         wrapper has no readback buffer to copy into)
    //     · is it ALIASABLE?               -> the placed-resource heap, the free path
    //     · is its state FRAME-LOCAL?      -> whether frame-start resets apply to it
    // The third question has NO predicate on purpose: it is answered by WHERE THE STATE LIVES. A persistent
    // image's live state is stored in the registry entry (`live_state()`), which the frame-start reset
    // cannot reach, so "skip persistent here" is not a rule anyone has to remember.
    // For a transient all three answers are "yes" and for an import all three are "no", so one bool served — right
    // up until a PERSISTENT resource, which is graph-owned, NOT aliasable, and NOT frame-local. The enum + the
    // three NAMED predicates make each site say which question it is asking. (Kept identical to the Vulkan side:
    // the two backends must not diverge on what ownership MEANS.)
    enum class Own : crd::u8
    {
        Imported = 0, // the application owns it; the graph only tracks its access
        Transient,    // the graph owns it for ONE frame: aliased, freed, reset
        Persistent,   // the graph owns it ACROSS frames: never aliased, never freed, never reset
    };
    struct ImageNode
    {
        IRasterTarget*         target    = nullptr; // imported: the real target; transient: the borrowed target (set in build)
        ITexture*              texture   = nullptr; // REN-2: transient + sampled ⇒ the borrowed SRV texture (set in build)
        ComPtr<ID3D12DescriptorHeap> rtv_heap;      // REN-2: transient RTT ⇒ the single-RTV heap the borrowed target renders to
        ComPtr<ID3D12DescriptorHeap> dsv_heap;      // REN-3.1: transient DEPTH ⇒ the single-DSV heap the depth-only pass renders to
        // REN-3.2: layers>1 ⇒ one borrowed target PER SLICE, each owning its OWN single-descriptor heap —
        // Dx12RasterTarget renders to its heap's START handle, so N descriptors in one shared heap would all
        // resolve to slice 0 and every cascade would land on top of cascade 0. The heaps need no separate array:
        // Dx12RasterTarget takes the ComPtr BY VALUE, so each slice target keeps its own heap alive.
        // The SRV, by contrast, is a single TEXTURE2DARRAY view spanning every slice.
        crd::containers::Array<IRasterTarget*> layer_targets{crd::memory::default_allocator()};
        Own                    own       = Own::Imported;
        FgImageDesc            desc{};
        DXGI_FORMAT            fmt       = DXGI_FORMAT_UNKNOWN;
        bool                   is_depth  = false;
        // REN-3.1: is_depth AND desc.sampled ⇒ the resource is R32_TYPELESS so an SRV is legal over it
        // (DSV = D32_FLOAT, SRV = R32_FLOAT). See the rule at create_transient_image.
        bool                   depth_sampled = false;
        D3D12_RESOURCE_DESC    rdesc{};
        UINT64                 mem_size  = 0;
        UINT64                 mem_align = 0;
        ComPtr<ID3D12Resource> resource;             // transient only (placed)
        crd::i32               first_pass = -1;
        crd::i32               last_pass  = -1;
        crd::i32               slot       = -1;
        bool                   has_write  = false;
        D3D12_RESOURCE_STATES  state      = D3D12_RESOURCE_STATE_COMMON; // imported target's live state during execute
        // REN-37.5: >= 0 => this node BORROWS a persistent entry's device objects AND its LIVE STATE. The
        // resource state of a persistent image lives in the REGISTRY, never here — see `live_state()`. That is
        // what makes the frame-start reset harmless for it BY CONSTRUCTION rather than by remembering a skip.
        crd::i32               persist_index = -1;
    };

    // The two questions that DO need a predicate, each answerable on its own.
    [[nodiscard]] static bool graph_owned(const ImageNode& n) noexcept { return n.own != Own::Imported; }
    [[nodiscard]] static bool aliasable(const ImageNode& n) noexcept { return n.own == Own::Transient; }

    // REN-37.5: one persistent image, keyed by a caller-chosen stable identity. Survives `reset()`, owns a
    // COMMITTED resource (not a placed one - it is never part of the aliasing heap), and is never retired.
    struct Persistent
    {
        crd::u32  key      = 0U;   // 0 = a dead slot, reusable after a resize freed one
        bool      was_live = false; // did it already carry history when this frame asked for it?
        ImageNode node{};
    };
    struct BufferNode
    {
        IStorageBuffer*        buffer    = nullptr;
        bool                   transient = false;
        crd::u32               size      = 0;
        D3D12_RESOURCE_DESC    rdesc{};
        UINT64                 mem_size  = 0;
        UINT64                 mem_align = 0;
        ComPtr<ID3D12Resource> resource;             // transient only (placed)
        crd::i32               first_pass = -1;
        crd::i32               last_pass  = -1;
        crd::i32               slot       = -1;
        bool                   has_write  = false;
    };
    struct Access
    {
        crd::u32 handle = 0; // 1-based image/buffer id
        FgAccess access = FgAccess::Read;
    };
    struct Pass
    {
        const char*      name = nullptr;
        FgPassKind       kind = FgPassKind::Raster;
        crd::containers::Array<Access> img_access{crd::memory::default_allocator()};
        crd::containers::Array<Access> buf_access{crd::memory::default_allocator()};
        FgExecuteFn      fn      = nullptr;
        void*            user    = nullptr;
        IPresentSurface* present = nullptr;
    };

    // a fluent builder that rebinds to the current pass (one instance reused — add_pass returns it)
    class Builder final : public IFramePassBuilder
    {
    public:
        void bind(Dx12FrameGraph* g, crd::usize pass) noexcept { m_g = g; m_pass = pass; }
        IFramePassBuilder& reads(FgImage h) override { add_img(h, FgAccess::Read); return *this; }
        IFramePassBuilder& reads(FgBuffer h) override { add_buf(h, FgAccess::Read); return *this; }
        IFramePassBuilder& writes(FgImage h) override { add_img(h, FgAccess::Write); return *this; }
        IFramePassBuilder& writes(FgBuffer h) override { add_buf(h, FgAccess::Write); return *this; }
        IFramePassBuilder& read_writes(FgImage h) override { add_img(h, FgAccess::ReadWrite); return *this; }
        IFramePassBuilder& read_writes(FgBuffer h) override { add_buf(h, FgAccess::ReadWrite); return *this; }
        IFramePassBuilder& execute(FgExecuteFn fn, void* user) override
        {
            m_g->m_passes[m_pass].fn   = fn;
            m_g->m_passes[m_pass].user = user;
            return *this;
        }
        IFramePassBuilder& present(IPresentSurface& surface) override
        {
            m_g->m_passes[m_pass].present = &surface;
            return *this;
        }
    private:
        void add_img(FgImage h, FgAccess a) { m_g->m_passes[m_pass].img_access.push_back({h.id, a}); }
        void add_buf(FgBuffer h, FgAccess a) { m_g->m_passes[m_pass].buf_access.push_back({h.id, a}); }
        Dx12FrameGraph* m_g    = nullptr;
        crd::usize      m_pass = 0;
    };

    // Greedy interval-color + place one memory class (images or buffers). Returns slots created, 0xFFFFFFFF on failure.
    [[nodiscard]] crd::u32 alias_class(bool images);

    static DXGI_FORMAT to_dxgi_format(FgImageFormat f, bool& is_depth) noexcept
    {
        is_depth = false;
        switch (f)
        {
        case FgImageFormat::RGBA8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case FgImageFormat::RGBA8Srgb:  return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case FgImageFormat::RGBA16F:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case FgImageFormat::R16F:       return DXGI_FORMAT_R16_FLOAT;
        case FgImageFormat::R32F:       return DXGI_FORMAT_R32_FLOAT;
        case FgImageFormat::R32Uint:    return DXGI_FORMAT_R32_UINT;
        case FgImageFormat::D32Float:
        default:                        is_depth = true; return DXGI_FORMAT_D32_FLOAT;
        }
    }

    void free_transients() noexcept
    {
        for (ImageNode& n : m_images)
        {
            if (aliasable(n))
            {
                // REN-2: delete the borrowed wrappers (they hold ComPtr refs), then release the placed resource + RTV heap.
                delete n.texture;
                n.texture = nullptr;
                // REN-3.2: on a layered transient `target` ALIASES layer_targets[0] — free the per-slice targets
                // and null `target` FIRST, or the shared slice-0 wrapper is deleted twice.
                for (IRasterTarget* lt : n.layer_targets) { delete lt; }
                if (n.layer_targets.size() > 0) { n.target = nullptr; }
                n.layer_targets.clear();
                delete n.target; // Dx12RasterTarget — virtual dtor
                n.target = nullptr;
                n.rtv_heap.Reset();
                n.dsv_heap.Reset();
                n.resource.Reset();
            }
        }
        for (BufferNode& n : m_buffers)
        {
            if (n.transient) { n.resource.Reset(); }
        }
        m_slots.clear();
        m_physical_bytes = 0U;
        m_logical_bytes  = 0U;
    }

    static constexpr crd::u32 kFrameSlots = 256U;

    Dx12RasterContext*           m_rc     = nullptr;
    ID3D12Device*                m_device = nullptr;
    ComPtr<ID3D12DescriptorHeap> m_frame_heap;
    UINT                         m_srv_inc = 0;

    crd::containers::Array<ImageNode>  m_images{crd::memory::default_allocator()};
    crd::containers::Array<BufferNode> m_buffers{crd::memory::default_allocator()};
    // REN-37.5: the persistent registry - the ONE thing in this graph that outlives `reset()`.
    crd::containers::Array<Persistent> m_persist{crd::memory::default_allocator()};

    // ⭐ THE LIVE RESOURCE STATE HAS EXACTLY ONE HOME. For a frame-local node that is the node's own field; for a
    // PERSISTENT one it is the registry entry, which `reset()` and the frame-start reset never touch. Routing
    // every read and write through here means there is nothing to copy back at end of frame and nothing to
    // remember to skip at the start of one.
    [[nodiscard]] D3D12_RESOURCE_STATES& live_state(ImageNode& n) noexcept
    {
        return n.persist_index >= 0 ? m_persist[static_cast<crd::u32>(n.persist_index)].node.state : n.state;
    }

    [[nodiscard]] bool materialize_image(ImageNode& n);
    // Release ONE persistent entry. Same teardown order as `free_transients` (the layered `target` aliases
    // layer_targets[0], so null it before the plain delete or slice 0 is destroyed twice).
    void destroy_persistent_impl(Persistent& p) noexcept
    {
        ImageNode& n = p.node;
        delete n.texture;
        n.texture = nullptr;
        for (IRasterTarget* lt : n.layer_targets) { delete lt; }
        if (n.layer_targets.size() > 0) { n.target = nullptr; }
        n.layer_targets.clear();
        delete n.target;
        n.target = nullptr;
        n.rtv_heap.Reset();
        n.dsv_heap.Reset();
        n.resource.Reset();
        n.state = D3D12_RESOURCE_STATE_COMMON;
    }
    crd::containers::Array<Pass>       m_passes{crd::memory::default_allocator()};
    crd::containers::Array<Slot>       m_slots{crd::memory::default_allocator()};
    Builder m_builder{};

    crd::u32 m_barrier_count  = 0U;
    crd::u32 m_submit_count   = 0U;
    bool     m_readback       = true;  // REN-8: opt-out; gates keep read_pixel
    bool     m_pending_submit = false; // REN-8: a submit not yet waited on
    // REN-1: the DEPENDENCY-SORTED execution order (declaration indices) — execute() walks THIS.
    crd::containers::Array<crd::u32> m_order{crd::memory::default_allocator()};
    // REN-8: device timestamp queries around each pass
    static constexpr crd::u32 kMaxTimedPasses = 64U;
    ComPtr<ID3D12QueryHeap>             m_ts_heap;
    ComPtr<ID3D12Resource>              m_ts_readback;
    double                              m_ts_freq = 0.0; // ticks per second
    crd::containers::Array<const char*> m_pass_names{crd::memory::default_allocator()};
    double                              m_pass_ms[kMaxTimedPasses]{};
    crd::u32                            m_timed_passes = 0U;
    double                              m_gpu_ms_total = 0.0;
    crd::u32 m_physical_bytes = 0U;
    crd::u32 m_logical_bytes  = 0U;
};

bool Dx12FrameGraph::build()
{
    m_barrier_count = 0U;
    m_submit_count  = 0U;

    // 1) every pass access must reference an existing resource
    for (const Pass& p : m_passes)
    {
        for (const Access& a : p.img_access) { if (a.handle == 0U || a.handle > m_images.size()) { return false; } }
        for (const Access& a : p.buf_access) { if (a.handle == 0U || a.handle > m_buffers.size()) { return false; } }
    }

    // ── 1b) TOPOLOGICAL SORT — identical semantics to the Vulkan graph. ────────────────────────────────────
    // Passes execute in DEPENDENCY order: for every resource, all WRITERS run before all READERS. A pass can be
    // added anywhere — appended, or inserted between two existing passes — and still lands correctly, because
    // the order comes from the DATA, not from declaration position. Ties break on declaration index, so the
    // schedule is deterministic and a dependency-free graph keeps the author's order.
    // ⛔ This MUST match Vulkan exactly. A graph that schedules differently per backend would break REN-36's
    // "one asset, both backends, bit-identical" claim in the most confusing way possible: same asset, same
    // pixels on one API, garbage on the other.
    {
        const crd::u32 np = static_cast<crd::u32>(m_passes.size());
        m_order.clear();
        crd::containers::Array<crd::u32> indeg(crd::memory::default_allocator());
        crd::containers::Array<crd::u32> edges(crd::memory::default_allocator());
        indeg.resize(np, 0U);
        edges.resize(static_cast<crd::usize>(np) * np, 0U);
        const auto add_edge = [&](crd::u32 from, crd::u32 to) {
            if (from == to) { return; }
            crd::u32& e = edges[static_cast<crd::usize>(from) * np + to];
            if (e == 0U) { e = 1U; ++indeg[to]; }
        };
        const auto link = [&](auto accessor) {
            for (crd::u32 w = 0; w < np; ++w)
            {
                for (const Access& aw : accessor(m_passes[w]))
                {
                    if (aw.access == FgAccess::Read) { continue; }
                    for (crd::u32 r = 0; r < np; ++r)
                    {
                        for (const Access& ar : accessor(m_passes[r]))
                        {
                            if (ar.handle != aw.handle) { continue; }
                            // A pure reader must follow the writer. Two WRITERS of the same resource keep
                            // declaration order (w < r) — that is the author's stated intent and the only
                            // defensible tie-break; reordering two writes would silently change the result.
                            if (ar.access == FgAccess::Read || w < r) { add_edge(w, r); }
                        }
                    }
                }
            }
        };
        link([](Pass& p) -> crd::containers::Array<Access>& { return p.img_access; });
        link([](Pass& p) -> crd::containers::Array<Access>& { return p.buf_access; });

        for (crd::u32 done = 0; done < np; ++done)
        {
            crd::u32 pick = np;
            for (crd::u32 i = 0; i < np; ++i)
            {
                if (indeg[i] == 0U) { pick = i; break; }
            }
            if (pick == np) { return false; } // ⛔ a dependency CYCLE — never a partial schedule
            indeg[pick] = 0xFFFFFFFFU;
            m_order.push_back(pick);
            for (crd::u32 t = 0; t < np; ++t)
            {
                if (edges[static_cast<crd::usize>(pick) * np + t] != 0U && indeg[t] != 0xFFFFFFFFU) { --indeg[t]; }
            }
        }
    }

    // 2) transient LIFETIME analysis — [first pass touching .. last pass touching] + whether any pass writes it
    // ⛔ Positions index the SORTED order: aliasing reuses memory across disjoint lifetimes, and "disjoint" is
    // only meaningful in EXECUTION order.
    for (ImageNode& n : m_images) { n.first_pass = -1; n.last_pass = -1; n.has_write = false; n.slot = -1; }
    for (BufferNode& n : m_buffers) { n.first_pass = -1; n.last_pass = -1; n.has_write = false; n.slot = -1; }
    for (crd::usize oi = 0; oi < m_order.size(); ++oi)
    {
        const crd::usize pi = m_order[oi];
        for (const Access& a : m_passes[pi].img_access)
        {
            ImageNode& n = m_images[a.handle - 1U];
            if (n.first_pass < 0) { n.first_pass = static_cast<crd::i32>(oi); }
            n.last_pass = static_cast<crd::i32>(oi);
            if (a.access != FgAccess::Read) { n.has_write = true; }
        }
        for (const Access& a : m_passes[pi].buf_access)
        {
            BufferNode& n = m_buffers[a.handle - 1U];
            if (n.first_pass < 0) { n.first_pass = static_cast<crd::i32>(oi); }
            n.last_pass = static_cast<crd::i32>(oi);
            if (a.access != FgAccess::Read) { n.has_write = true; }
        }
    }
    for (const ImageNode& n : m_images) { if (aliasable(n) && !n.has_write) { return false; } } // a transient no pass writes
    for (const BufferNode& n : m_buffers) { if (n.transient && !n.has_write) { return false; } }

    // 3) ALIASING — greedy interval assignment: process transients in first_pass order; reuse a slot whose last
    //    occupant's lifetime ended before this one begins (disjoint ⇒ shared heap). Images then buffers (each a
    //    heap class — RT/DS vs buffers); physical = Σ heap sizes (post-aliasing), logical = Σ transient sizes.
    m_physical_bytes = 0U;
    m_logical_bytes  = 0U;

    const crd::u32 img_slots = alias_class(true);
    if (img_slots == 0xFFFFFFFFU) { return false; }
    if (alias_class(false) == 0xFFFFFFFFU) { return false; }
    return true;
}

// Greedy interval-color one memory class (images if `images` else buffers): assign disjoint-lifetime transients to
// shared slots, allocate each slot's heap, and place each resource at offset 0. Returns the number of slots created
// for this class, or 0xFFFFFFFF on failure.
crd::u32 Dx12FrameGraph::alias_class(bool images)
{
    const crd::u32 slot_base = static_cast<crd::u32>(m_slots.size());
    crd::containers::Array<crd::u32> order{crd::memory::default_allocator()};
    const crd::usize count = images ? m_images.size() : m_buffers.size();
    for (crd::u32 i = 0; i < count; ++i)
    {
        const bool tr = images ? aliasable(m_images[i]) : m_buffers[i].transient;
        if (tr) { order.push_back(i); }
    }
    const auto first_pass_of = [&](crd::u32 i) { return images ? m_images[i].first_pass : m_buffers[i].first_pass; };
    for (crd::usize a = 1; a < order.size(); ++a) // insertion sort by first_pass (small N)
    {
        const crd::u32 v = order[a];
        crd::usize     b = a;
        while (b > 0 && first_pass_of(order[b - 1]) > first_pass_of(v)) { order[b] = order[b - 1]; --b; }
        order[b] = v;
    }

    const D3D12_HEAP_FLAGS class_flags = images ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES : D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    for (crd::u32 idx : order)
    {
        const UINT64 sz     = images ? m_images[idx].mem_size : m_buffers[idx].mem_size;
        const crd::i32 fp   = images ? m_images[idx].first_pass : m_buffers[idx].first_pass;
        const crd::i32 lp   = images ? m_images[idx].last_pass : m_buffers[idx].last_pass;
        m_logical_bytes += static_cast<crd::u32>(sz);

        crd::i32 chosen = -1;
        for (crd::u32 si = slot_base; si < m_slots.size(); ++si)
        {
            if (m_slots[si].free_after < fp) { chosen = static_cast<crd::i32>(si); break; }
        }
        if (chosen < 0)
        {
            Slot s{};
            s.size = sz;
            D3D12_HEAP_DESC hd{};
            hd.SizeInBytes           = sz;
            hd.Properties.Type       = D3D12_HEAP_TYPE_DEFAULT;
            hd.Alignment             = images ? m_images[idx].mem_align : m_buffers[idx].mem_align;
            hd.Flags                 = class_flags;
            if (FAILED(m_device->CreateHeap(&hd, IID_PPV_ARGS(&s.heap)))) { return 0xFFFFFFFFU; }
            m_slots.push_back(std::move(s));
            chosen = static_cast<crd::i32>(m_slots.size() - 1U);
            m_physical_bytes += static_cast<crd::u32>(sz);
        }
        else if (sz > m_slots[static_cast<crd::u32>(chosen)].size)
        {
            m_slots[static_cast<crd::u32>(chosen)].size = sz; // (won't happen for equal-size transients; guarded anyway)
        }
        Slot& s      = m_slots[static_cast<crd::u32>(chosen)];
        s.free_after = lp;

        // place the resource into the chosen slot's heap at offset 0
        if (images)
        {
            ImageNode& n = m_images[idx];
            n.slot = chosen;
            const D3D12_RESOURCE_STATES init = n.is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_COMMON;
            if (FAILED(m_device->CreatePlacedResource(s.heap.Get(), 0, &n.rdesc, init, nullptr, IID_PPV_ARGS(&n.resource))))
            {
                return 0xFFFFFFFFU;
            }
            if (!materialize_image(n)) { return 0xFFFFFFFFU; }
        }
        else
        {
            BufferNode& n = m_buffers[idx];
            n.slot = chosen;
            if (FAILED(m_device->CreatePlacedResource(s.heap.Get(), 0, &n.rdesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&n.resource))))
            {
                return 0xFFFFFFFFU;
            }
            n.buffer = new Dx12TransientBuffer(n.size);
        }
    }
    return static_cast<crd::u32>(m_slots.size()) - slot_base;
}

// REN-37.5: MATERIALIZE an image node - its attachment view(s), its borrowed render target(s) and (if `sampled`)
// its borrowed SRV texture. Factored out of the placement loop because a PERSISTENT image needs exactly the same
// materialization but over a COMMITTED resource instead of a placed one. Two copies of this would be two places
// for the layered/depth/typeless rules to drift apart - and those rules are the largest, sharpest part of the
// DX12 port (the R32_TYPELESS / D32_FLOAT / R32_FLOAT three-format dance, the per-slice DSV-in-its-own-heap rule).
bool Dx12FrameGraph::materialize_image(ImageNode& n)
{
    {
        {
            const bool   is_array = n.desc.layers > 1U;
            const crd::u32 slices = n.desc.layers;
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT tfp{}; // no readback footprint — a transient is device-local

            // REN-3.2: one attachment view PER SLICE. `slice_heap` builds the l-th single-slice RTV/DSV in its
            // own heap; the non-layered path is the same code with slices == 1, so there is no second code path
            // to drift. A layered view MUST be the *ARRAY* dimension with FirstArraySlice — a TEXTURE2D view over
            // an array resource silently addresses slice 0 only, so every cascade would land on top of cascade 0.
            const auto slice_heap = [&](crd::u32 l, ComPtr<ID3D12DescriptorHeap>& out) -> bool {
                D3D12_DESCRIPTOR_HEAP_DESC hd{};
                hd.Type           = n.is_depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                hd.NumDescriptors = 1;
                if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out)))) { return false; }
                const D3D12_CPU_DESCRIPTOR_HANDLE h = out->GetCPUDescriptorHandleForHeapStart();
                if (n.is_depth)
                {
                    // The DSV is ALWAYS the typed depth format — even when the resource is R32_TYPELESS.
                    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
                    dsv.Format = DXGI_FORMAT_D32_FLOAT;
                    if (is_array)
                    {
                        dsv.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                        dsv.Texture2DArray.FirstArraySlice = l;
                        dsv.Texture2DArray.ArraySize       = 1;
                    }
                    else { dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D; }
                    m_device->CreateDepthStencilView(n.resource.Get(), &dsv, h);
                }
                else if (is_array)
                {
                    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
                    rtv.Format                         = n.fmt;
                    rtv.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    rtv.Texture2DArray.FirstArraySlice = l;
                    rtv.Texture2DArray.ArraySize       = 1;
                    m_device->CreateRenderTargetView(n.resource.Get(), &rtv, h);
                }
                else { m_device->CreateRenderTargetView(n.resource.Get(), nullptr, h); }
                return true;
            };
            const auto slice_target = [&](const ComPtr<ID3D12DescriptorHeap>& heap) -> IRasterTarget* {
                // ⛔ REN-3.1: a DEPTH transient is a BORROWED DEPTH target — the placed resource goes in the DEPTH
                // slot so `draw_storage_depth_only` can render into it with no colour attachment. Before REN-3.1
                // depth transients got NEITHER a target NOR a texture, which is exactly why the device could not
                // render a shadow map.
                return n.is_depth ? new Dx12RasterTarget(nullptr, nullptr, n.resource, nullptr, nullptr, heap, nullptr,
                                                         tfp, 1U, n.desc.width, n.desc.height)
                                  : new Dx12RasterTarget(n.resource, nullptr, nullptr, nullptr, heap, nullptr, nullptr,
                                                         tfp, 1U, n.desc.width, n.desc.height);
            };

            if (is_array)
            {
                for (crd::u32 l = 0; l < slices; ++l)
                {
                    ComPtr<ID3D12DescriptorHeap> heap;
                    if (!slice_heap(l, heap)) { return false; }
                    n.layer_targets.push_back(slice_target(heap));
                }
                n.target = n.layer_targets[0]; // image() on a layered transient = slice 0 (never the whole array)
            }
            else
            {
                ComPtr<ID3D12DescriptorHeap>& heap = n.is_depth ? n.dsv_heap : n.rtv_heap;
                if (!slice_heap(0U, heap)) { return false; }
                n.target = slice_target(heap);
            }

            // REN-2/3.2: a `sampled` transient is ALSO a borrowed SRV texture a later pass reads via texture()
            // (the RTT round-trip) — a TEXTURE2DARRAY SRV spanning every slice when layered, so the shader
            // selects its cascade. For depth the SRV is R32_FLOAT over the SAME R32_TYPELESS resource: a typed-D32
            // resource would have failed SRV creation, which is the rule the three-format dance exists to honour.
            if (n.desc.sampled && (!n.is_depth || n.depth_sampled))
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format                  = n.is_depth ? DXGI_FORMAT_R32_FLOAT : n.fmt;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if (is_array)
                {
                    srv.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    srv.Texture2DArray.MipLevels   = 1;
                    srv.Texture2DArray.ArraySize   = slices;
                }
                else
                {
                    srv.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srv.Texture2D.MipLevels = 1;
                }
                n.texture = new Dx12Texture(n.resource, n.desc.width, n.desc.height, srv);
            }
        }
    }
    return true;
}

void Dx12FrameGraph::execute()
{
    // ⛔ REN-8: the deferred wait lands HERE, before `frame_rec_begin` resets the command ALLOCATOR and LIST —
    // both are still being consumed by the previous submission until its fence value is reached. This is where
    // the CPU/GPU overlap is cashed in; by now the GPU has usually finished, so the wait costs ~nothing.
    wait_pending_submit();
    m_barrier_count = 0U;
    m_submit_count  = 0U;
    m_rc->frame_rec_begin(m_frame_heap.Get(), m_srv_inc);
    // Every FRAME-LOCAL node starts the frame at COMMON: a transient is a brand-new placed resource and an
    // imported target's state is tracked by the raster context. This loop needs no exception for persistent
    // images — their live state is not stored here at all (`live_state`), so clearing these fields cannot reach it.
    for (ImageNode& n : m_images) { n.state = D3D12_RESOURCE_STATE_COMMON; }

    // REN-8: only the NAMES are cleared. m_timed_passes / m_gpu_ms_total belong to resolve_timestamps(), which
    // on the deferred path just published the PREVIOUS frame's numbers — zeroing them here would wipe exactly
    // those (the bug the Vulkan side hit, which reported "0 passes" while timing worked).
    m_pass_names.clear();
    crd::u32 pass_index = 0U;

    // walk the DEPENDENCY-SORTED order from build(), never m_passes directly
    for (const crd::u32 pass_idx : m_order)
    {
        Pass& p = m_passes[pass_idx];
        for (const Access& a : p.img_access)
        {
            ImageNode& n = m_images[a.handle - 1U];
            if (n.target == nullptr) { continue; }
            const bool writes = (a.access != FgAccess::Read);
            if (graph_owned(n) && n.is_depth)
            {
                // REN-3.1: a DEPTH RTT transient — the depth-only pass renders into it (DEPTH_WRITE), a later pass
                // SAMPLES it (PIXEL_SHADER_RESOURCE). Note the resource was CREATED in DEPTH_WRITE (see the placed-
                // resource init state), so the first write needs no barrier — only the write→read flip does.
                auto& dt = static_cast<Dx12RasterTarget&>(*n.target);
                if (writes && live_state(n) != D3D12_RESOURCE_STATE_DEPTH_WRITE)
                {
                    m_rc->frame_transition(dt.depth_tex(), live_state(n), D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    live_state(n) = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                    ++m_barrier_count;
                }
                else if (!writes && live_state(n) == D3D12_RESOURCE_STATE_DEPTH_WRITE)
                {
                    // the DEPTH RTT barrier: the shadow pass's depth writes complete → this pass samples it
                    m_rc->frame_transition(dt.depth_tex(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    live_state(n) = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    ++m_barrier_count;
                }
                continue;
            }
            // ⛔ REN-37.5: the RTT barrier path keys on GRAPH-OWNED, not on `transient`. A PERSISTENT image is
            // graph-owned too - rendered into, then sampled - it simply is not aliased or freed. Keying on
            // `transient` sent it down the IMPORTED path, whose end-of-frame readback belongs to a target the
            // application owns (and whose borrowed wrapper has no readback buffer at all).
            if (graph_owned(n)) // REN-2: an RTT image — render into it (RENDER_TARGET), then a later pass SAMPLES it
            {
                auto& tt = static_cast<Dx12RasterTarget&>(*n.target);
                if (writes && live_state(n) != D3D12_RESOURCE_STATE_RENDER_TARGET)
                {
                    m_rc->frame_transition(tt.tex(), live_state(n), D3D12_RESOURCE_STATE_RENDER_TARGET);
                    live_state(n) = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    ++m_barrier_count;
                }
                else if (!writes && live_state(n) == D3D12_RESOURCE_STATE_RENDER_TARGET)
                {
                    // the RTT barrier: the render pass's writes complete → this pass samples it
                    m_rc->frame_transition(tt.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    live_state(n) = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    ++m_barrier_count;
                }
                continue;
            }
            if (p.present != nullptr) { continue; } // present pass reads → readback loop transitions
            if (writes && live_state(n) != D3D12_RESOURCE_STATE_RENDER_TARGET)
            {
                auto& t = static_cast<Dx12RasterTarget&>(*n.target);
                m_rc->frame_transition(t.tex(), live_state(n), D3D12_RESOURCE_STATE_RENDER_TARGET);
                live_state(n) = D3D12_RESOURCE_STATE_RENDER_TARGET;
                ++m_barrier_count;
            }
        }
        // REN-8: bracket THIS pass with device timestamps. D3D12 has no begin/end pair for TIMESTAMP — each is a
        // single `EndQuery`, so a pass is two of them and the delta is its GPU cost.
        ID3D12GraphicsCommandList* ts_list = m_rc->frame_cmd_list();
        const bool stamp = m_ts_heap != nullptr && ts_list != nullptr && pass_index < kMaxTimedPasses;
        if (stamp) { ts_list->EndQuery(m_ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, pass_index * 2U); }
        if (p.fn != nullptr) { p.fn(*this, p.user); }
        if (stamp)
        {
            ts_list->EndQuery(m_ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, pass_index * 2U + 1U);
            m_pass_names.push_back(p.name);
        }
        ++pass_index;
    }

    // REN-8: resolve the queries into the readback buffer as part of THIS submission — ResolveQueryData is a
    // command, not a CPU call, so it must be recorded before the list closes.
    if (m_ts_heap != nullptr && !m_pass_names.empty())
    {
        ID3D12GraphicsCommandList* ts_list = m_rc->frame_cmd_list();
        if (ts_list != nullptr)
        {
            ts_list->ResolveQueryData(m_ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0U,
                                      static_cast<UINT>(m_pass_names.size()) * 2U, m_ts_readback.Get(), 0U);
        }
    }

    // final readback: every imported target left in RENDER_TARGET → COPY_SOURCE + copy to its readback (so read_pixel
    // is bit-identical to the sync path), then back to COMMON. The direct-to-backbuffer present is REN-8.
    for (ImageNode& n : m_images)
    {
        // Only APPLICATION-owned targets have a readback buffer, and `read_pixel` is only ever about those.
        if (m_readback && !graph_owned(n) && n.target != nullptr
            && n.state == D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            m_rc->frame_readback(static_cast<Dx12RasterTarget&>(*n.target));
            n.state = D3D12_RESOURCE_STATE_COMMON;
            m_barrier_count += 2U; // frame_readback inserts RT→COPY_SOURCE and COPY_SOURCE→COMMON
        }
    }

    // REN-8: same contract as Vulkan — readback on ⇒ wait now (gates read pixels the instant execute() returns);
    // readback off ⇒ ONE ExecuteCommandLists and the block deferred to the top of the next execute()/reset().
    m_rc->frame_submit_no_wait(); // ONE ExecuteCommandLists
    m_pending_submit = true;
    m_rc->frame_rec_end();
    m_submit_count = 1U;
    if (m_readback) { wait_pending_submit(); }
}

// REN-8: block until the in-flight submission (if any) completes. Called at the top of execute()/reset() and
// from the dtor — every point about to touch something the GPU could still be reading.
void Dx12FrameGraph::wait_pending_submit() noexcept
{
    if (!m_pending_submit) { return; }
    m_rc->frame_wait_submitted();
    m_pending_submit = false;
    resolve_timestamps();
}

// REN-8: the fence has been reached, so the resolved ticks are readable. Ticks → ms via the queue frequency.
void Dx12FrameGraph::resolve_timestamps() noexcept
{
    m_timed_passes = 0U;
    if (m_ts_heap == nullptr || m_ts_readback == nullptr || m_pass_names.empty() || m_ts_freq <= 0.0) { return; }
    const crd::u32 n = static_cast<crd::u32>(m_pass_names.size());
    // read ONLY the range that was resolved; a wider range would map uninitialised bytes
    D3D12_RANGE  rr{0U, sizeof(UINT64) * n * 2U};
    UINT64* ticks = nullptr; // Map wants a writable void**; the mapping itself is only ever READ below
    if (FAILED(m_ts_readback->Map(0U, &rr, reinterpret_cast<void**>(&ticks))) || ticks == nullptr) { return; }
    for (crd::u32 i = 0; i < n; ++i)
    {
        const UINT64 a = ticks[i * 2U];
        const UINT64 b = ticks[i * 2U + 1U];
        m_pass_ms[i]   = b > a ? (static_cast<double>(b - a) / m_ts_freq) * 1000.0 : 0.0;
    }
    m_gpu_ms_total = ticks[n * 2U - 1U] > ticks[0]
                         ? (static_cast<double>(ticks[n * 2U - 1U] - ticks[0]) / m_ts_freq) * 1000.0
                         : 0.0;
    const D3D12_RANGE wr{0U, 0U}; // read-only map: nothing written back
    m_ts_readback->Unmap(0U, &wr);
    m_timed_passes = n;
}

std::unique_ptr<IFrameGraph> Dx12RasterContext::create_frame_graph()
{
    return std::make_unique<Dx12FrameGraph>(*this);
}

std::unique_ptr<IRasterContext> create_dx12_raster_context(crd::memory::IAllocator* /*alloc*/)
{
    auto ctx = std::make_unique<Dx12RasterContext>();
    if (!ctx->valid()) { return nullptr; }
    return ctx;
}

} // namespace crd::gpu
