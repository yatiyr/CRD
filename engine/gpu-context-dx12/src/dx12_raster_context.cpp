// dx12_raster_context.cpp — the D3D12 implementation of crd::gpu::IRasterContext (ADR-0103 / D-008 C4). The DX12 mirror
// of the Vulkan raster context (C1): a graphics (DIRECT) queue + offscreen RGBA8 render targets + a CLEAR with pixel
// readback. Raw D3D12, no crd-rhi. D3D12 has no dynamic-rendering equivalent to fuss over — a bare RTV + ClearRTV is the
// whole clear path; the texture→readback copy honours the 256-byte row-pitch alignment D3D12 demands (GetCopyableFootprints).
// The shader DRAW path (a graphics PSO from a VS+FS DXIL pair + DrawInstanced) appends in C4-b.

#include <crd/gpu/detail/command_lowering.hpp> // RAF-12.4: CommandEncoder<Ctx> — this backend records through its own instantiation
#include <crd/log/log_macros.hpp>
#include <crd/gpu/dx12_raster_context.hpp>

#include <crd/gpu/dx12_ray_tracing_context.hpp> // REN-38-A9: dx12_scene_tlas — the TLAS behind a portable AS handle
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

// REN-38: the ENGINE LOGGER, not fprintf. This channel carries the root-signature refusals whose SILENCE
// cost the entire DX12 scene family (t3[1024] swallowing the atlas t4 — a bare nullptr said nothing).
CRD_DEFINE_LOG_CHANNEL(g_log_dx12raster, "Dx12Raster", crd::log::LogLevel::Info)

namespace crd::gpu
{

using Microsoft::WRL::ComPtr;

namespace
{
constexpr DXGI_FORMAT kColorFormat  = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr UINT        kBindlessMax  = 1024; // REN-38: the material-heap capacity (heap slots 2.. + the t3 SRV table) — mirrors Vulkan

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

// REN-38 audit: the pass-state converters live beside `to_d3d12_compare` below; declared here because
// `build_graphics_pso` bakes the declared state into the PSO.
[[nodiscard]] D3D12_COMPARISON_FUNC to_d3d12_compare(DepthCompare c) noexcept;
[[nodiscard]] D3D12_STENCIL_OP      to_d3d12_stencil_op(StencilOp op) noexcept;

// Build the attributeless graphics PSO (empty root sig, cull-none, RGBA8 RTV) at a given sample count + depth config. A
// D3D12 PSO BAKES SampleDesc.Count AND the depth-stencil state + DSVFormat (unlike Vulkan shader objects, where these are
// dynamic), so a draw needs a PSO matching the target's samples AND depth compare — hence both are parameters and
// Dx12RasterProgram caches a PSO per (samples, depth) key. `dsv == DXGI_FORMAT_UNKNOWN` ⇒ depth disabled (the B1-d off path).
ComPtr<ID3D12PipelineState> build_graphics_pso(ID3D12Device* dev, ID3D12RootSignature* root, D3D12_SHADER_BYTECODE vs,
                                               D3D12_SHADER_BYTECODE fs, UINT samples, DXGI_FORMAT dsv,
                                               D3D12_COMPARISON_FUNC depth_func, bool conservative, UINT num_rts = 1,
                                               D3D12_SHADER_BYTECODE hs = D3D12_SHADER_BYTECODE{},  // B4-tess: hull (empty = none)
                                               D3D12_SHADER_BYTECODE ds = D3D12_SHADER_BYTECODE{},  // B4-tess: domain
                                               DXGI_FORMAT rt_fmt = kColorFormat,                   // B4-vis-4: R32_UINT vis buffer
                                               const BlendMode* blend = nullptr,                    // REN-38-A15: per-RT blend
                                               const PassRasterState* state = nullptr)              // REN-38 audit: pass state
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature                                   = root;
    pd.VS                                               = vs;
    pd.PS                                               = fs;
    pd.HS                                               = hs; // B4-tess: hull + domain when present ⇒ a tessellation PSO
    pd.DS                                               = ds;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; // no blend, write RGBA
    // ── REN-38-A15: PER-ATTACHMENT BLEND. ⛔ On DX12 blend is PSO STATE, not dynamic as it is on Vulkan — so the
    // mode has to be part of the PSO CACHE KEY (see `pso_for`), or two passes asking for different blends would
    // share one pipeline and the second would silently render with the first one's equations.
    if (blend != nullptr)
    {
        pd.BlendState.IndependentBlendEnable = TRUE; // per-RT equations
        for (UINT i = 0; i < num_rts && i < 8U; ++i)
        {
            auto& b                = pd.BlendState.RenderTarget[i];
            b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            b.BlendOp               = D3D12_BLEND_OP_ADD;
            b.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
            switch (blend[i])
            {
            case BlendMode::Alpha:
                b.BlendEnable = TRUE; b.SrcBlend = D3D12_BLEND_SRC_ALPHA; b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; break;
            case BlendMode::PremultipliedAlpha:
                b.BlendEnable = TRUE; b.SrcBlend = D3D12_BLEND_ONE; b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; break;
            case BlendMode::Additive:
                b.BlendEnable = TRUE; b.SrcBlend = D3D12_BLEND_ONE; b.DestBlend = D3D12_BLEND_ONE;
                b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_ONE; break;
            case BlendMode::Multiply:
                b.BlendEnable = TRUE; b.SrcBlend = D3D12_BLEND_DEST_COLOR; b.DestBlend = D3D12_BLEND_ZERO;
                b.SrcBlendAlpha = D3D12_BLEND_DEST_ALPHA; b.DestBlendAlpha = D3D12_BLEND_ZERO; break;
            case BlendMode::RevealageMultiply: // dst * (1 - src.rgb) — the WBOIT revealage equation
                b.BlendEnable = TRUE; b.SrcBlend = D3D12_BLEND_ZERO; b.DestBlend = D3D12_BLEND_INV_SRC_COLOR;
                b.SrcBlendAlpha = D3D12_BLEND_ZERO; b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; break;
            case BlendMode::RevealComposite: // (1-src.a)·src + src.a·dst — the WBOIT resolve OVER the background (the
                                             // inverse of Alpha; matches the fused draw_wboit composite exactly)
                b.BlendEnable = TRUE; b.SrcBlend = D3D12_BLEND_INV_SRC_ALPHA; b.DestBlend = D3D12_BLEND_SRC_ALPHA;
                b.SrcBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; b.DestBlendAlpha = D3D12_BLEND_SRC_ALPHA; break;
            case BlendMode::Opaque:
            default: b.BlendEnable = FALSE; break;
            }
        }
    }
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
    // ── ⭐ REN-38 audit: the DECLARED pass state — PSO-baked on D3D12, so it is part of the PSO cache key
    // (see `pso_for`), exactly as blend is. Null = the historical hardwired behaviour above, bit for bit. ──
    if (state != nullptr)
    {
        auto& rs = pd.RasterizerState;
        if (state->face_cull == FaceCull::Back)  { rs.CullMode = D3D12_CULL_MODE_BACK; }
        if (state->face_cull == FaceCull::Front) { rs.CullMode = D3D12_CULL_MODE_FRONT; }
        rs.FrontCounterClockwise = state->front_face == FrontFace::CounterClockwise ? TRUE : FALSE;
        // ⛔ D3D12's constant bias is an INT in device units where Vulkan's is a float — the asset declares the
        // same number for both, which is the portable meaning for the fixed-point depth paths shadows use.
        rs.DepthBias             = static_cast<INT>(state->depth_bias);
        rs.SlopeScaledDepthBias  = state->depth_bias_slope;
        rs.DepthBiasClamp        = state->depth_bias_clamp;
        if (dsv != DXGI_FORMAT_UNKNOWN)
        {
            pd.DepthStencilState.DepthWriteMask =
                state->depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            if (state->stencil_enable)
            {
                pd.DepthStencilState.StencilEnable    = TRUE;
                pd.DepthStencilState.StencilReadMask  = static_cast<UINT8>(state->stencil_read_mask);
                pd.DepthStencilState.StencilWriteMask = static_cast<UINT8>(state->stencil_write_mask);
                const D3D12_DEPTH_STENCILOP_DESC ops{
                    to_d3d12_stencil_op(state->stencil_fail), to_d3d12_stencil_op(state->stencil_depth_fail),
                    to_d3d12_stencil_op(state->stencil_pass), to_d3d12_compare(state->stencil_compare)};
                pd.DepthStencilState.FrontFace = ops;
                pd.DepthStencilState.BackFace  = ops;
            }
        }
    }
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
                                           D3D12_SHADER_BYTECODE as = D3D12_SHADER_BYTECODE{}, // B4: AS = the task shader (empty = none)
                                           const PassRasterState* state = nullptr) // REN-38 audit: pass state
{
    if (dev2 == nullptr) { return nullptr; }
    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode        = D3D12_FILL_MODE_SOLID;
    rast.CullMode        = D3D12_CULL_MODE_NONE; // match the graphics path (attributeless / mesh-generated winding moot)
    rast.DepthClipEnable = TRUE;
    // REN-38 audit: a mesh pass declares the same state a geometry pass does — same fields, same defaults.
    if (state != nullptr)
    {
        if (state->face_cull == FaceCull::Back)  { rast.CullMode = D3D12_CULL_MODE_BACK; }
        if (state->face_cull == FaceCull::Front) { rast.CullMode = D3D12_CULL_MODE_FRONT; }
        rast.FrontCounterClockwise = state->front_face == FrontFace::CounterClockwise ? TRUE : FALSE;
        rast.DepthBias             = static_cast<INT>(state->depth_bias);
        rast.SlopeScaledDepthBias  = state->depth_bias_slope;
        rast.DepthBiasClamp        = state->depth_bias_clamp;
    }
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
        ds.DepthWriteMask = state != nullptr && !state->depth_write ? D3D12_DEPTH_WRITE_MASK_ZERO
                                                                    : D3D12_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc      = depth_func;
        if (state != nullptr && state->stencil_enable)
        {
            ds.StencilEnable    = TRUE;
            ds.StencilReadMask  = static_cast<UINT8>(state->stencil_read_mask);
            ds.StencilWriteMask = static_cast<UINT8>(state->stencil_write_mask);
            const D3D12_DEPTH_STENCILOP_DESC ops{
                to_d3d12_stencil_op(state->stencil_fail), to_d3d12_stencil_op(state->stencil_depth_fail),
                to_d3d12_stencil_op(state->stencil_pass), to_d3d12_compare(state->stencil_compare)};
            ds.FrontFace = ops;
            ds.BackFace  = ops;
        }
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
// REN-38 audit (pass-state vocabulary): the backend-neutral StencilOp → D3D12_STENCIL_OP, mapped explicitly.
[[nodiscard]] D3D12_STENCIL_OP to_d3d12_stencil_op(StencilOp op) noexcept
{
    switch (op)
    {
    case StencilOp::Keep:      return D3D12_STENCIL_OP_KEEP;
    case StencilOp::Zero:      return D3D12_STENCIL_OP_ZERO;
    case StencilOp::Replace:   return D3D12_STENCIL_OP_REPLACE;
    case StencilOp::IncrClamp: return D3D12_STENCIL_OP_INCR_SAT;
    case StencilOp::DecrClamp: return D3D12_STENCIL_OP_DECR_SAT;
    case StencilOp::Invert:    return D3D12_STENCIL_OP_INVERT;
    case StencilOp::IncrWrap:  return D3D12_STENCIL_OP_INCR;
    case StencilOp::DecrWrap:  return D3D12_STENCIL_OP_DECR;
    }
    return D3D12_STENCIL_OP_KEEP;
}
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT; // B1-d depth buffer format

// ⭐⭐ REN-39-A1: the state a storage buffer holds DURING an indexed draw — the IA's index fetch plus the
// shader-read states an indexed-mode program reads the same buffer through (39-B2's read-only seam). All three
// are READ states, so the combination is legal; UNORDERED_ACCESS is not, which is why storage is read-only
// during an indexed draw (the declared contract on IRasterContext::draw_storage_indexed_depth).
constexpr D3D12_RESOURCE_STATES kIndexedDrawStates = D3D12_RESOURCE_STATE_INDEX_BUFFER |
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

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
                     crd::u32 samples, crd::u32 w, crd::u32 h, bool has_stencil = false) noexcept
        : m_tex(std::move(tex)), m_resolve(std::move(resolve)), m_depth(std::move(depth)),
          m_readback(std::move(readback)), m_rtv_heap(std::move(rtv_heap)), m_dsv_heap(std::move(dsv_heap)),
          m_mapped(mapped), m_fp(fp), m_samples(samples), m_w(w), m_h(h), m_has_stencil(has_stencil)
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
    [[nodiscard]] bool            has_depth() const noexcept override { return m_depth != nullptr; } // B1-d
    [[nodiscard]] ID3D12Resource* depth_tex() const noexcept { return m_depth.Get(); }
    // REN-38-F11: a D24S8 target — the PSO's DSVFormat and every depth clear must then carry the stencil half.
    [[nodiscard]] bool              has_stencil() const noexcept { return m_has_stencil; }
    [[nodiscard]] DXGI_FORMAT       dsv_format() const noexcept
    {
        return m_has_stencil ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_D32_FLOAT;
    }
    [[nodiscard]] DXGI_FORMAT       color_format() const noexcept
    {
        if (m_tex == nullptr) { return kColorFormat; }
        return m_tex->GetDesc().Format;
    }
    [[nodiscard]] D3D12_CLEAR_FLAGS clear_flags() const noexcept
    {
        return m_has_stencil ? (D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL) : D3D12_CLEAR_FLAG_DEPTH;
    }
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
    bool                                m_has_stencil = false; // REN-38-F11: the depth resource is D24S8 (appended at END)
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
        : m_queue(queue), m_mode(mode), m_device(device)
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

    [[nodiscard]] bool present(IRasterTarget& target) override { return present(target, nullptr, nullptr); }

    // ⭐⭐ REN-39-D2: PRESENT WITH AN OVERLAY on DX12 — the RET-5 seam that existed only on Vulkan, and the
    // reason ImGui could not run on this backend at all (the base class returned false, so the sandbox refused
    // to present). After the canvas blit the backbuffer moves COPY_DEST → RENDER_TARGET, the callback records
    // into the SAME list with the backbuffer bound (ImGui's DX12 backend takes an ID3D12GraphicsCommandList*),
    // then it goes to PRESENT. The scene canvas stays clean — overlays live at the present seam, exactly as the
    // Vulkan twin documents.
    [[nodiscard]] bool present(IRasterTarget& target, OverlayFn overlay, void* user) override
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
        if (overlay != nullptr && ensure_backbuffer_rtv(bb.Get(), idx))
        {
            barrier(bb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_bb_rtv_heap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += static_cast<SIZE_T>(idx) * m_bb_rtv_inc;
            m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr); // LOAD — the blitted scene stays
            const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(m_w), static_cast<float>(m_h), 0.0F, 1.0F};
            const D3D12_RECT     sc{0, 0, static_cast<LONG>(m_w), static_cast<LONG>(m_h)};
            m_list->RSSetViewports(1, &vp);
            m_list->RSSetScissorRects(1, &sc);
            overlay(static_cast<void*>(m_list.Get()), user); // ImGui records here
            barrier(bb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        }
        else
        {
            barrier(bb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        }
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

    // REN-39-D2: what the ImGui DX12 backend has to be told about the swapchain it composites into.
    [[nodiscard]] crd::u32    image_count() const noexcept { return kBackBufferCount; }
    [[nodiscard]] DXGI_FORMAT color_format() const noexcept { return kColorFormat; }

    [[nodiscard]] bool resize(crd::u32 width, crd::u32 height) override
    {
        if (!m_valid) { return false; }
        wait_gpu();
        const UINT flags = m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U;
        // REN-39-D2: ResizeBuffers hands back NEW backbuffer resources at the same indices — forget the RTVs or
        // the overlay would render into freed views (the class of bug the `m_bb_rtv_for` identity check exists for).
        for (ID3D12Resource*& r : m_bb_rtv_for) { r = nullptr; }
        if (FAILED(m_swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags))) { return false; }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        if (FAILED(m_swapchain->GetDesc1(&sd))) { return false; }
        m_w = sd.Width; // a real window's swapchain follows the client area — report the truth
        m_h = sd.Height;
        return true;
    }

private:
    // REN-39-D2: one RTV per backbuffer, created lazily and rebuilt after a resize (the heap is sized for the
    // swapchain's buffer count; `m_bb_rtv_for` remembers which resource each slot views so a ResizeBuffers —
    // which hands back NEW resources at the same indices — cannot leave a stale view behind).
    [[nodiscard]] bool ensure_backbuffer_rtv(ID3D12Resource* bb, UINT idx)
    {
        if (m_device == nullptr || idx >= kBackBufferCount) { return false; }
        if (m_bb_rtv_heap == nullptr)
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = kBackBufferCount;
            if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_bb_rtv_heap)))) { return false; }
            m_bb_rtv_inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
        if (m_bb_rtv_for[idx] != bb)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = m_bb_rtv_heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(idx) * m_bb_rtv_inc;
            m_device->CreateRenderTargetView(bb, nullptr, h);
            m_bb_rtv_for[idx] = bb;
        }
        return true;
    }
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
    // REN-39-D2: the present-seam OVERLAY needs the device (to make backbuffer RTVs) and one RTV per buffer.
    static constexpr UINT             kBackBufferCount = 2U; // matches DXGI_SWAP_CHAIN_DESC1::BufferCount above
    ID3D12Device*                     m_device = nullptr;
    ComPtr<ID3D12DescriptorHeap>      m_bb_rtv_heap;
    UINT                              m_bb_rtv_inc = 0U;
    ID3D12Resource*                   m_bb_rtv_for[kBackBufferCount]{};
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
    // (RAF-12.4: the raw vs_bytecode/ps_bytecode/device accessors were deleted with the fused draw_wboit verb — the
    // only caller. WBOIT's per-RT additive/multiplicative blend is now the frame graph's authored raster.mrt pass,
    // whose PSO the standard pass_pso path bakes from the per-attachment BlendMode array.)

    // The PSO for a target of `samples` samples and depth/conservative config (a graphics PSO bakes ALL of them). The plain
    // 1×/no-depth/non-conservative PSO is prebuilt (also gates valid()); every other combo is built + cached lazily in a
    // small keyed cache (a handful of configs per program at most). `dsv == DXGI_FORMAT_UNKNOWN` ⇒ the depth-off colour path.
    [[nodiscard]] ID3D12PipelineState* pso_for(crd::u32 samples, DXGI_FORMAT dsv, D3D12_COMPARISON_FUNC depth_func,
                                               bool conservative, crd::u32 num_rts = 1U, DXGI_FORMAT rt_fmt = kColorFormat,
                                               const BlendMode* blend = nullptr,
                                               const PassRasterState* state = nullptr)
    {
        // REN-38-A15: fold the blend modes into the cache key. ⛔ Without this, two passes asking for DIFFERENT
        // blends would share one PSO and the second would render with the first's equations — a wrong image with
        // nothing to point at, because both passes' declarations look correct.
        crd::u32 blend_key = 0U;
        if (blend != nullptr)
        {
            for (crd::u32 i = 0; i < num_rts && i < 4U; ++i)
            {
                blend_key |= (static_cast<crd::u32>(blend[i]) & 0x7U) << (i * 3U);
            }
        }
        // ⭐ REN-38 audit: the declared pass state joins the cache identity — EXACTLY, member by member, never
        // hashed (a hash collision hands one pass another's pipeline). Default state keeps the historical key.
        const PassRasterState def_state{};
        const PassRasterState& st = state != nullptr ? *state : def_state;
        if (samples <= 1U && dsv == DXGI_FORMAT_UNKNOWN && !conservative && num_rts == 1U && rt_fmt == kColorFormat
            && blend_key == 0U && st == def_state)
        {
            return m_pso1.Get();
        }
        const crd::u32 key = (samples << 8U)
                             | (dsv != DXGI_FORMAT_UNKNOWN ? (0x80U | static_cast<crd::u32>(depth_func)) : 0U)
                             | (conservative ? 0x10000U : 0U) | (num_rts << 20U) // B5: RT count in the key (MRT G-buffer)
                             | (rt_fmt == kColorFormat ? 0U : 0x40000U) // B4-vis-4: the R32_UINT visibility-buffer format
                             | (blend_key << 24U);                       // REN-38-A15: per-RT blend modes
        for (int i = 0; i < m_cache_n; ++i)
        {
            if (m_cache[i].key == key && m_cache[i].rt_fmt == rt_fmt && m_cache[i].state == st)
            {
                return m_cache[i].pso.Get();
            }
        }
        if (m_cache_n >= kPsoCacheCap) { return nullptr; }
        m_cache[m_cache_n].key    = key;
        m_cache[m_cache_n].rt_fmt = rt_fmt;
        m_cache[m_cache_n].state  = st;
        m_cache[m_cache_n].pso =
            m_is_mesh ? build_mesh_pso(m_device2, m_root.Get(), D3D12_SHADER_BYTECODE{m_vs.get(), m_vs_size},
                                       D3D12_SHADER_BYTECODE{m_fs.get(), m_fs_size}, samples, dsv, depth_func, num_rts,
                                       D3D12_SHADER_BYTECODE{m_as.get(), m_as_size}, // B4: AS (task) when present, else empty
                                       &st)                                          // REN-38 audit: pass state
                      : build_graphics_pso(m_device, m_root.Get(), D3D12_SHADER_BYTECODE{m_vs.get(), m_vs_size},
                                           D3D12_SHADER_BYTECODE{m_fs.get(), m_fs_size}, samples, dsv, depth_func,
                                           conservative, num_rts,
                                           D3D12_SHADER_BYTECODE{m_hs.get(), m_hs_size},   // B4-tess: hull (empty ⇒ non-tess)
                                           D3D12_SHADER_BYTECODE{m_ds.get(), m_ds_size},   // B4-tess: domain
                                           rt_fmt, blend, &st);                            // A15 blend · REN-38 state
        return m_cache[m_cache_n++].pso.Get();
    }

private:
    static constexpr int kPsoCacheCap = 8;
    struct PsoCacheEntry
    {
        crd::u32                    key    = 0;
        DXGI_FORMAT                 rt_fmt = kColorFormat;
        PassRasterState             state{}; // REN-38 audit: exact per-pass state, part of the cache identity
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
    // ⛔ `depth` is an EXPLICIT flag, not inferred from the SRV format: a DX12 depth SRV is R32_FLOAT over an
    // R32_TYPELESS resource (typed-D32 SRVs fail creation), so the format alone cannot tell a depth atlas from
    // an ordinary single-channel colour texture — and the atlas SAMPLER is chosen by this answer (REN-40-D).
    Dx12Texture(ComPtr<ID3D12Resource> tex, crd::u32 w, crd::u32 h, const D3D12_SHADER_RESOURCE_VIEW_DESC& srv,
                bool depth = false) noexcept
        : m_tex(std::move(tex)), m_srv(srv), m_w(w), m_h(h), m_depth(depth)
    {
    }
    ~Dx12Texture() override                    = default;
    Dx12Texture(const Dx12Texture&)            = delete;
    Dx12Texture& operator=(const Dx12Texture&) = delete;
    Dx12Texture(Dx12Texture&&)                 = delete;
    Dx12Texture& operator=(Dx12Texture&&)      = delete;

    [[nodiscard]] crd::u32                              width() const noexcept override { return m_w; }
    [[nodiscard]] crd::u32                              height() const noexcept override { return m_h; }
    [[nodiscard]] bool                                  is_depth() const noexcept override { return m_depth; }
    [[nodiscard]] ID3D12Resource*                       tex() const noexcept { return m_tex.Get(); }
    [[nodiscard]] const D3D12_SHADER_RESOURCE_VIEW_DESC& srv() const noexcept { return m_srv; }

private:
    ComPtr<ID3D12Resource>          m_tex;
    D3D12_SHADER_RESOURCE_VIEW_DESC m_srv{};
    crd::u32                        m_w = 0;
    crd::u32                        m_h = 0;
    bool                            m_depth = false;
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

// ── ⛔⛔ REN-38-A9/A10: THE ONE PLACE A BUFFER HANDLE IS RESOLVED — the DX12 mirror of `vk_buffer_of`. ──
// A frame graph hands a pass its buffers as `IStorageBuffer*`, and TWO concrete kinds hide behind that: an
// APPLICATION buffer (`Dx12StorageBuffer`) and a GRAPH TRANSIENT (`Dx12TransientBuffer`, minted by `build()` over
// aliased heap memory). ⛔ Every dispatch verb used to `static_cast<Dx12StorageBuffer&>`, which is undefined the
// moment the buffer is a transient — and crashed the instant an authored pass declared `kind =
// "transient_buffer"` instead of importing an app-owned buffer. The identical defect Vulkan carried.
[[nodiscard]] ID3D12Resource* dx_buffer_of(IStorageBuffer& b) noexcept;
[[nodiscard]] crd::u32        dx_buffer_elems(IStorageBuffer& b) noexcept;

class Dx12RasterContext final : public IRasterContext
{
public:
    // ⭐ RAF-12.4 Phase A: record the canonical command model through this backend's OWN encoder instantiation, so the
    // verb calls resolve STATICALLY to this context (Phase B de-virtualizes them off IRasterContext into private methods
    // this template friend reaches). Byte-identical to the base CommandEncoder<IRasterContext>.
    template <class> friend class detail::CommandEncoder;
    [[nodiscard]] std::unique_ptr<ICommandEncoder> create_command_encoder() override
    {
        return std::make_unique<detail::CommandEncoder<Dx12RasterContext>>(*this);
    }
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
        // slots 2..2+kBindlessMax-1 = the bindless SRV array (draw_bindless, B2-d) · slot 2+kBindlessMax = the
        // SHADOW-ATLAS SRV (REN-38 — appended at the END so the bindless slot math is untouched). Plus a
        // shader-visible SAMPLER heap.
        D3D12_DESCRIPTOR_HEAP_DESC shd{};
        shd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        shd.NumDescriptors = 4 + kBindlessMax; // +1: the REN-39-C1 read-only storage SRV (slot 3 + kBindlessMax)
        shd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        m_device->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&m_uav_heap));
        m_srv_inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_HEAP_DESC smh{};
        smh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        // REN-38-B8: slots 0/1 keep their historical meaning (default · comparison); 2..N are the AUTHORED
        // sampler cache. ⛔ Growing the heap rather than reusing slot 0 matters: a shader-visible sampler heap
        // cannot be resized after creation, and overwriting slot 0 would change every OTHER pass's sampling.
        // ⛔⛔ REN-40-D: +1 for the PLAIN depth sampler (s6) at the fixed END slot `2 + kSamplerCacheCap` —
        // AFTER the authored cache, so none of the existing `2U + i` slot math moves. It exists because a PCSS
        // blocker search needs the STORED depth, which a comparison sampler cannot return; on Vulkan the same
        // sampler lives at binding 6. ⛔ Leaving s6 out of this backend while the technique emits it is the
        // cook-only scar: the program cooks, PSO creation rejects the unbound register, and shadows silently
        // degrade to off — a defect only a DX12 RUN can see.
        // ...and +1 again (REN-40-D moments): slot `3 + kSamplerCacheCap` = the LINEAR/CLAMP ATLAS sampler a
        // COLOUR atlas (the moment atlas) is sampled through at s5 — comparison sampling is meaningless on
        // moments, and the default s0 sampler WRAPs, which would blend the atlas's opposite edges together at
        // every cascade border.
        smh.NumDescriptors = 4 + kSamplerCacheCap;
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
            // REN-40-D: the PLAIN depth sampler (see the heap-size note) — NEAREST + CLAMP, because a blocker
            // search wants the depth actually stored in a texel: a filtered read across a shadow edge returns a
            // depth no blocker has, and WRAP would find "blockers" from the far side of the cascade slice.
            D3D12_SAMPLER_DESC pd{};
            pd.Filter   = D3D12_FILTER_MIN_MAG_MIP_POINT;
            pd.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            pd.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            pd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            pd.MaxLOD   = D3D12_FLOAT32_MAX;
            D3D12_CPU_DESCRIPTOR_HANDLE ph = m_sampler_heap->GetCPUDescriptorHandleForHeapStart();
            ph.ptr += static_cast<SIZE_T>(2U + kSamplerCacheCap) * m_sampler_inc;
            m_device->CreateSampler(&pd, ph);
            // REN-40-D: the LINEAR/CLAMP atlas sampler (see the heap-size note) — filterable-atlas reads (s5).
            D3D12_SAMPLER_DESC ad{};
            ad.Filter   = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ad.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            ad.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            ad.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            ad.MaxLOD   = D3D12_FLOAT32_MAX;
            D3D12_CPU_DESCRIPTOR_HANDLE ah = m_sampler_heap->GetCPUDescriptorHandleForHeapStart();
            ah.ptr += static_cast<SIZE_T>(3U + kSamplerCacheCap) * m_sampler_inc;
            m_device->CreateSampler(&ad, ah);
        }
    }
    ~Dx12RasterContext() override
    {
        drain_upload_batches(); // wait out any in-flight upload batch before its ring/list ComPtrs release
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
        return make_color_depth_target(width, height, kDepthFormat);
    }

    // REN-38-F11: colour + D24S8 - the target the authored stencil vocabulary draws against.
    [[nodiscard]] std::unique_ptr<IRasterTarget> create_color_depth_stencil_target(crd::u32 width,
                                                                                   crd::u32 height) override
    {
        return make_color_depth_target(width, height, DXGI_FORMAT_D24_UNORM_S8_UINT);
    }

    [[nodiscard]] std::unique_ptr<IRasterTarget> make_color_depth_target(crd::u32 width, crd::u32 height,
                                                                         DXGI_FORMAT depth_fmt)
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
        dd.Format              = depth_fmt;
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
                                                  fp, 1U, width, height,
                                                  depth_fmt == DXGI_FORMAT_D24_UNORM_S8_UINT);
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

        // ⭐ REN-38-A6: CLEAR IS RECORDABLE — the last verb in the surface with no recording path. Without it a
        // `kind = "clear"` pass inside a frame graph would Reset the allocator and submit its OWN command list
        // mid-frame: a second submission, out of order with the graph's, clearing a target the graph owned.
        // A clear stays a RASTER pass — the graph has already put the target in RENDER_TARGET, which is where
        // ClearRenderTargetView needs it.
        if (frame_recording())
        {
            const float rgba_rec[4] = {color.r, color.g, color.b, color.a};
            m_list->ClearRenderTargetView(t.rtv(), rgba_rec, 0, nullptr);
            return;
        }

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
        D3D12_DESCRIPTOR_RANGE bindless_range{}; // B2-d: t16[kBindlessMax] (bindless texture array — above the fixed slots)
        bindless_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        bindless_range.NumDescriptors                    = kBindlessMax;
        bindless_range.BaseShaderRegister                = 16; // t16 — above the fixed slots (t1 single, t2 free, t4 atlas); t3-base swallowed t4 and DX12 refuses overlapping ranges
        bindless_range.RegisterSpace                     = 0;
        bindless_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        // REN-38: t4 = the SHADOW ATLAS's own register + s5 = its comparison sampler — the pair that ends the
        // atlas/base-colour fight over t1/s2, mirroring the Vulkan bindings 4/5.
        D3D12_DESCRIPTOR_RANGE atlas_range{};
        atlas_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        atlas_range.NumDescriptors                    = 1;
        atlas_range.BaseShaderRegister                = 4; // t4
        atlas_range.RegisterSpace                     = 0;
        atlas_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE cmp_range{};
        cmp_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        cmp_range.NumDescriptors                    = 1;
        cmp_range.BaseShaderRegister                = 5; // s5
        cmp_range.RegisterSpace                     = 0;
        cmp_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        // ⭐⭐ REN-39-C1: t0 = the storage buffer READ-ONLY — the register an INDEXED program pair reads through
        // (during an indexed draw the resource sits in INDEX_BUFFER | shader-read states, where a u0 UAV access
        // is illegal). ALL-visible (the VS pulls, the FS reads the frame header). t0 was free in this signature.
        D3D12_DESCRIPTOR_RANGE ro_range{};
        ro_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ro_range.NumDescriptors = 1;
        ro_range.BaseShaderRegister = 0; // t0 (read-only storage — the indexed pair)
        ro_range.RegisterSpace = 0;
        ro_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        // REN-40-D: s6 = the atlas's PLAIN sampler (the PCSS blocker search reads STORED depth through it).
        D3D12_DESCRIPTOR_RANGE plain_range{};
        plain_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        plain_range.NumDescriptors                    = 1;
        plain_range.BaseShaderRegister                = 6; // s6
        plain_range.RegisterSpace                     = 0;
        plain_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        // param 0 = UAV (u0, storage) · 1 = SRV (t1, texture) · 2 = sampler (s2) · 3 = bindless SRV array (t3[N]) ·
        // 4 = atlas SRV (t4) · 5 = comparison sampler (s5) · 6 = DrawIndex constant (b7) · 7 = read-only storage
        // SRV (t0, REN-39-C1) · 8 = the atlas's PLAIN sampler (s6, REN-40-D). Every program carries all of them;
        // a draw sets only the tables its stages use.
        D3D12_ROOT_PARAMETER param[9]{};
        param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[0].DescriptorTable.NumDescriptorRanges = 1; param[0].DescriptorTable.pDescriptorRanges = &uav_range;      param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // GEO-1: + VERTEX (vertex pulling reads u0 in the VS)
        param[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[1].DescriptorTable.NumDescriptorRanges = 1; param[1].DescriptorTable.pDescriptorRanges = &srv_range;      param[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        param[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[2].DescriptorTable.NumDescriptorRanges = 1; param[2].DescriptorTable.pDescriptorRanges = &samp_range;     param[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        param[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[3].DescriptorTable.NumDescriptorRanges = 1; param[3].DescriptorTable.pDescriptorRanges = &bindless_range; param[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        param[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[4].DescriptorTable.NumDescriptorRanges = 1; param[4].DescriptorTable.pDescriptorRanges = &atlas_range;    param[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        param[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[5].DescriptorTable.NumDescriptorRanges = 1; param[5].DescriptorTable.pDescriptorRanges = &cmp_range;      param[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        // REN-38: param 6 = the DrawIndex ROOT CONSTANT (b7, one uint, VERTEX) — ExecuteIndirect's command
        // signature varies it PER COMMAND, D3D12's native draw-id channel. A shader without b7 ignores it.
        param[6].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param[6].Constants.ShaderRegister = 7; // b7
        param[6].Constants.RegisterSpace  = 0;
        param[6].Constants.Num32BitValues = 1;
        param[6].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;
        param[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param[7].DescriptorTable.NumDescriptorRanges = 1;
        param[7].DescriptorTable.pDescriptorRanges = &ro_range;
        param[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // REN-39-C1
        param[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param[8].DescriptorTable.NumDescriptorRanges = 1;
        param[8].DescriptorTable.pDescriptorRanges = &plain_range;
        param[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; // REN-40-D: s6
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 9;
        rsd.pParameters   = param;
        rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> sig;
        ComPtr<ID3DBlob> err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        {
            CRD_LOG_ERROR(g_log_dx12raster, "root signature serialize FAILED: {}",
                          err != nullptr ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return nullptr;
        }
        ComPtr<ID3D12RootSignature> root;
        if (FAILED(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                 IID_PPV_ARGS(&root))))
        {
            std::fprintf(stderr, "RSIG create FAILED\n");
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
    // ── ⭐ REN-38-A7/A8: the CONTINUING draws — the DX12 mirror. ──
    // ⛔ RECORDING-ONLY by contract: "keep the previous contents" is only meaningful while a frame is open, and
    // every synchronous verb here ends by copying to the readback buffer and returning the target to COMMON.
    // REN-38-F6+: storage-bound tessellation — frame-graph recording only, like every RT verb.
    void draw_tess_storage(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                           IStorageBuffer& storage, crd::u32 patch_count)
    {
        if (!m_ok || !frame_recording()) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s2 = static_cast<Dx12StorageBuffer&>(storage);
        if (!p.is_tess()) { return; }
        ID3D12PipelineState* pso = pass_pso(p, t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format());
        if (!p.valid() || pso == nullptr) { return; }
        record_plain_topology(t, p, pso, clear, D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST,
                              patch_count * 4U, /*clear=*/true, &s2);
    }
    void draw_tess_storage_load(IRasterTarget& target, IRasterProgram& program, IStorageBuffer& storage,
                                crd::u32 patch_count)
    {
        if (!m_ok || !frame_recording()) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s2 = static_cast<Dx12StorageBuffer&>(storage);
        if (!p.is_tess()) { return; }
        ID3D12PipelineState* pso = pass_pso(p, t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format());
        if (!p.valid() || pso == nullptr) { return; }
        record_plain_topology(t, p, pso, ClearColor{}, D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST,
                              patch_count * 4U, /*clear=*/false, &s2);
    }

    void draw_tess_load(IRasterTarget& target, IRasterProgram& program, crd::u32 patch_count)
    {
        if (!m_ok || !frame_recording()) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        if (!p.is_tess()) { return; }
        ID3D12PipelineState* pso = pass_pso(p, t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format());
        if (!p.valid() || pso == nullptr) { return; }
        record_plain_topology(t, p, pso, ClearColor{}, D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST,
                              patch_count * 4U, /*clear=*/false);
    }

    // REN-38-F6+: storage-bound mesh dispatch — frame-graph recording only, like the tess twin.
    void draw_mesh_storage(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                           IStorageBuffer& storage, crd::u32 group_count)
    {
        if (!m_ok || !m_mesh_shader || m_list6 == nullptr) { return; }
        auto& t  = static_cast<Dx12RasterTarget&>(target);
        auto& p  = static_cast<Dx12RasterProgram&>(program);
        auto& s2 = static_cast<Dx12StorageBuffer&>(storage);
        if (!p.is_mesh() || !p.valid()) { return; }
        ID3D12PipelineState* pso = pass_pso(p, t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format());
        if (pso == nullptr) { return; }
        if (frame_recording())
        {
            record_mesh(t, p, pso, clear, group_count, false, 0.0F, nullptr, false, D3D12_SHADING_RATE_1X1,
                        nullptr, nullptr, 0U, /*clear=*/true, &s2);
            return;
        }
        // ── ⭐⭐ REN-41 Stage 4: the SYNCHRONOUS mesh+storage draw. `draw_mesh_storage` had only the
        // frame-recording path, so a direct (gate) call no-op'd — every other draw verb has a synchronous form.
        // Mirrors the synchronous draw_mesh + binds the storage buffer as a UAV at root param 0 exactly as the
        // synchronous draw_storage does (m_uav_heap slot 0), then DispatchMesh + copy-to-readback + wait.
        if (m_uav_heap == nullptr) { return; }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format                     = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements         = s2.num_elements();
        uav.Buffer.StructureByteStride = 4;
        m_device->CreateUnorderedAccessView(s2.buf(), nullptr, &uav, m_uav_heap->GetCPUDescriptorHandleForHeapStart());

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
        apply_stencil_ref();
        m_list6->DispatchMesh(group_count, 1, 1);
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
    void draw_mesh_storage_load(IRasterTarget& target, IRasterProgram& program, IStorageBuffer& storage,
                                crd::u32 group_count)
    {
        if (!m_ok || !frame_recording() || m_list6 == nullptr) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s2 = static_cast<Dx12StorageBuffer&>(storage);
        if (!p.is_mesh()) { return; }
        ID3D12PipelineState* pso = pass_pso(p, t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format());
        if (!p.valid() || pso == nullptr) { return; }
        record_mesh(t, p, pso, ClearColor{}, group_count, false, 0.0F, nullptr, false, D3D12_SHADING_RATE_1X1,
                    nullptr, nullptr, 0U, /*clear=*/false, &s2);
    }

    void draw_mesh_load(IRasterTarget& target, IRasterProgram& program, crd::u32 group_count)
    {
        if (!m_ok || !frame_recording() || m_list6 == nullptr) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        if (!p.is_mesh()) { return; }
        ID3D12PipelineState* pso = pass_pso(p, t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format());
        if (!p.valid() || pso == nullptr) { return; }
        record_mesh(t, p, pso, ClearColor{}, group_count, false, 0.0F, nullptr, false, D3D12_SHADING_RATE_1X1,
                    nullptr, nullptr, 0U, /*clear=*/false);
    }

    void draw_tess(IRasterTarget& target, IRasterProgram& program, ClearColor clear, crd::u32 patch_count)
    {
        if (!m_ok) { return; }
        auto&      t  = static_cast<Dx12RasterTarget&>(target);
        auto&      p  = static_cast<Dx12RasterProgram&>(program);
        if (!p.is_tess()) { return; } // only a VS+HS+DS+PS program can be patch-drawn
        const bool ms = t.multisampled();
        ID3D12PipelineState* pso = pass_pso(p, t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format());
        if (!p.valid() || pso == nullptr) { return; }
        // REN-38-A1d: inside a frame, RECORD — same attachment-and-draw shape as `record_plain` plus the patch
        // topology the tessellator consumes. No allocator/list reset, no transitions, no resolve, no readback.
        if (frame_recording())
        {
            record_plain_topology(t, p, pso, clear, D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST,
                                  patch_count * 4U);
            return;
        }

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
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST); // the tessellator consumes quad patches
        m_list->DrawInstanced(patch_count * 4U, 1, 0, 0);                                 // patch_count patches × 4 control points

        if (ms) // AVERAGE-resolve the MSAA colour texture into the single-sample resolve texture (the readback source)
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            transition(t.resolve(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            m_list->ResolveSubresource(t.resolve(), 0, t.tex(), 0, t.color_format());
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
    void draw_mesh(IRasterTarget& target, IRasterProgram& program, ClearColor clear, crd::u32 group_count)
    {
        if (!m_ok || !m_mesh_shader || m_list6 == nullptr) { return; }
        auto&      t   = static_cast<Dx12RasterTarget&>(target);
        auto&      p   = static_cast<Dx12RasterProgram&>(program);
        const bool ms  = t.multisampled();
        ID3D12PipelineState* pso = pass_pso(p, t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format());
        if (!p.valid() || !p.is_mesh() || pso == nullptr) { return; }
        if (frame_recording())
        {
            record_mesh(t, p, pso, clear, group_count, false, 0.0F, nullptr, false, D3D12_SHADING_RATE_1X1, nullptr,
                        nullptr, 0U);
            return;
        }

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
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list6->DispatchMesh(group_count, 1, 1); // mesh shaders emit their own topology — no IASetPrimitiveTopology

        if (ms)
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            transition(t.resolve(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            m_list->ResolveSubresource(t.resolve(), 0, t.tex(), 0, t.color_format());
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
    void draw_mesh_vrs(IRasterTarget& target, IRasterProgram& program, ClearColor clear, crd::u32 group_count)
    {
        if (!m_ok || !m_mesh_shader || m_list6 == nullptr) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        if (m_list5 == nullptr || m_vrs_tier == D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED)
        {
            draw_mesh(target, program, clear, group_count); // no VRS ⇒ a full-rate mesh draw
            return;
        }
        ID3D12PipelineState* pso = pass_pso(p, 1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format());
        if (!p.valid() || !p.is_mesh() || pso == nullptr) { return; }
        if (frame_recording())
        {
            const D3D12_SHADING_RATE_COMBINER fcomb[2] = {D3D12_SHADING_RATE_COMBINER_OVERRIDE,
                                                          D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
            record_mesh(t, p, pso, clear, group_count, false, 0.0F, nullptr, true, D3D12_SHADING_RATE_1X1,
                        static_cast<const D3D12_SHADING_RATE_COMBINER*>(fcomb), nullptr, 0U);
            return;
        }

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
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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
                            crd::u64 args_offset)
    {
        if (!m_ok || !m_mesh_shader || m_list6 == nullptr || native_args == nullptr) { return; }
        auto&                t   = static_cast<Dx12RasterTarget&>(target);
        auto&                p   = static_cast<Dx12RasterProgram&>(program);
        ID3D12PipelineState* pso = pass_pso(p, 1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format());
        if (!p.valid() || !p.is_mesh() || pso == nullptr) { return; }
        auto* args = static_cast<ID3D12Resource*>(native_args);
        // REN-38-A1c: the GPU-DRIVEN mesh path. ⛔ The args buffer must already be in INDIRECT_ARGUMENT state —
        // inside a frame that is the GRAPH's job (it is a declared read), not this verb's, which is why the
        // synchronous path's transition pair is absent here.
        if (frame_recording())
        {
            record_mesh(t, p, pso, clear, 0U, false, 0.0F, nullptr, false, D3D12_SHADING_RATE_1X1, nullptr, args,
                        args_offset);
            return;
        }

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
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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
                                  crd::u32 group_count)
    {
        if (!m_ok || !m_mesh_shader || m_list6 == nullptr || m_uav_heap == nullptr || m_sampler_heap == nullptr
            || count == 0U || textures == nullptr)
        {
            return;
        }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        if (!t.has_depth()) { return; } // needs a create_color_depth_target
        ID3D12PipelineState* pso = pass_pso(p, 1U, t.dsv_format(), to_d3d12_compare(compare), false, 1U, t.color_format());
        if (!p.valid() || !p.is_mesh() || pso == nullptr) { return; }
        const crd::u32 n = count < static_cast<crd::u32>(kBindlessMax) ? count : static_cast<crd::u32>(kBindlessMax);
        // REN-38-A1c: the mesh path with the BINDLESS run from 38-A1a's frame ring + a depth attachment. The
        // synchronous body below mints into the GLOBAL heap at fixed slots, which is exactly what cannot happen
        // inside a frame.
        if (frame_recording())
        {
            const D3D12_GPU_DESCRIPTOR_HANDLE run = frame_alloc_bindless_run(textures, n);
            record_mesh(t, p, pso, clear, group_count, true, clear_depth, &run, false, D3D12_SHADING_RATE_1X1,
                        nullptr, nullptr, 0U);
            return;
        }

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
        m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr);

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
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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

    // ── REN-38-A1h: the frame-mode body shared by `draw` and `draw_depth` (the DX12 half). ──────────────────
    // Binds NO descriptor table — these shaders are self-contained. No allocator/list reset, no transitions (the
    // graph owns them for a declared write), no readback, no submit.
    // REN-38-A1d: `record_plain` with an explicit primitive topology — tessellation consumes PATCH lists, not
    // triangles, and that is the ONLY thing that differs from a plain draw once the graph owns the transitions.
    // ── REN-38-A1c: the frame-mode body of the MESH/TASK family (the DX12 half). ────────────────────────────
    // One body for all four verbs, as on Vulkan. ⛔ Mesh shaders emit their OWN topology, so there is NO
    // `IASetPrimitiveTopology` here — issuing one is not merely redundant, it is invalid for a mesh PSO. The
    // amplification (AS/task) stage is part of the PSO on DX12, so unlike Vulkan there is nothing to bind
    // separately: `is_mesh()` already covers task->mesh->pixel.
    //   · `bindless` non-null  -> the 38-A1a frame descriptor run, bound at tables 2 (sampler) + 3 (t3[])
    //   · `indirect` non-null  -> ExecuteIndirect with the DISPATCH_MESH command signature (GPU-driven)
    //   · `vrs` true           -> the rate is set AFTER the PSO and BEFORE the dispatch
    void record_mesh(Dx12RasterTarget& t, Dx12RasterProgram& p, ID3D12PipelineState* pso, ClearColor clear_color,
                     crd::u32 group_count, bool depth_on, float clear_depth,
                     const D3D12_GPU_DESCRIPTOR_HANDLE* bindless, bool vrs, D3D12_SHADING_RATE rate,
                     const D3D12_SHADING_RATE_COMBINER* comb, ID3D12Resource* indirect, crd::u64 indirect_offset,
                     bool clear = true, Dx12StorageBuffer* storage = nullptr)
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv       = t.rtv();
        const bool                        use_depth = depth_on && t.has_depth();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv       = use_depth ? t.dsv() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        m_list->OMSetRenderTargets(1, &rtv, FALSE, use_depth ? &dsv : nullptr);
        // ⛔ REN-38-A8: a CONTINUING mesh draw KEEPS the previous contents — on D3D12 "load" is simply not
        // issuing the clear, since OMSetRenderTargets never discards. Depth too: clearing it would let the second
        // meshlet group pass the test against nothing and overwrite the first.
        if (clear)
        {
            const float rgba[4] = {clear_color.r, clear_color.g, clear_color.b, clear_color.a};
            m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
            if (use_depth) { m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr); }
        }
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        // REN-38-F6+: the storage-bound form — the mesh stage PULLS its geometry (the GEO-1 seam)
        if (storage != nullptr)
        {
            m_list->SetGraphicsRootDescriptorTable(0, frame_alloc_storage_slot(*storage));
        }
        if (bindless != nullptr)
        {
            m_list->SetGraphicsRootDescriptorTable(2, m_sampler_heap->GetGPUDescriptorHandleForHeapStart());
            m_list->SetGraphicsRootDescriptorTable(3, *bindless);
        }
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        if (vrs && m_list5 != nullptr)
        {
            m_list5->RSSetShadingRate(rate, comb);
            if (t.has_vrs()) { m_list5->RSSetShadingRateImage(t.vrs_tex()); }
        }
        if (indirect != nullptr)
        {
            m_list->ExecuteIndirect(m_mesh_indirect_sig.Get(), 1U, indirect, indirect_offset, nullptr, 0U);
        }
        else { m_list6->DispatchMesh(group_count, 1, 1); }
    }

    void record_plain_topology(Dx12RasterTarget& t, Dx12RasterProgram& p, ID3D12PipelineState* pso,
                               ClearColor clear_color, D3D12_PRIMITIVE_TOPOLOGY topo, crd::u32 vertex_count,
                               bool clear = true, Dx12StorageBuffer* storage = nullptr)
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        if (clear) // REN-38-A7: a CONTINUING patch draw keeps what the previous patches wrote
        {
            const float rgba[4] = {clear_color.r, clear_color.g, clear_color.b, clear_color.a};
            m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        }
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        // REN-38-F6+: the storage-bound form — the tess VS PULLS its control points (the GEO-1 seam)
        if (storage != nullptr)
        {
            m_list->SetGraphicsRootDescriptorTable(0, frame_alloc_storage_slot(*storage));
        }
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list->IASetPrimitiveTopology(topo);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);
    }

    void record_plain(Dx12RasterTarget& t, Dx12RasterProgram& p, ID3D12PipelineState* pso, ClearColor clear_color,
                      bool depth_on, float clear_depth, crd::u32 vertex_count)
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        const bool                        use_depth = depth_on && t.has_depth();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = use_depth ? t.dsv() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        m_list->OMSetRenderTargets(1, &rtv, FALSE, use_depth ? &dsv : nullptr);
        const float rgba[4] = {clear_color.r, clear_color.g, clear_color.b, clear_color.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        if (use_depth) { m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr); }
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);
    }

    void draw(IRasterTarget& target, IRasterProgram& program, ClearColor clear, crd::u32 vertex_count)
    {
        if (!m_ok) { return; }
        auto&      t  = static_cast<Dx12RasterTarget&>(target);
        auto&      p  = static_cast<Dx12RasterProgram&>(program);
        const bool ms = t.multisampled();
        ID3D12PipelineState* pso = pass_pso(p, t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format()); // colour, no depth
        if (!p.valid() || pso == nullptr) { return; }
        if (frame_recording()) { record_plain(t, p, pso, clear, false, 0.0F, vertex_count); return; }

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
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);

        if (ms) // AVERAGE-resolve the MSAA colour texture into the single-sample resolve texture (the readback source)
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            transition(t.resolve(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            m_list->ResolveSubresource(t.resolve(), 0, t.tex(), 0, t.color_format());
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
    // ⭐ REN-38-A11: the CONTINUING visibility-buffer draw — one image must hold EVERY visible primitive's id.
    // ── ⭐ REN-38-A16: THE DXR RAY-TRACING PIPELINE — built from scratch on this backend. ──
    // ⛔ `Dx12RayTracingContext` has `trace_dispatch` (inline ray query) and NOTHING ELSE: there was no DXR state
    // object, no shader binding table and no `DispatchRays` anywhere in this engine's DX12 half. This is not a
    // port of the Vulkan path — it is the missing half, written to the same contract.
    //
    // ⛔⛔ THE EXPORT-NAME COLLISION IS THE FIRST THING TO GET RIGHT. Every CKIR ray-tracing stage lowers to a
    // function called `main`, so three DXIL libraries in one state object all export `main` and `CreateStateObject`
    // fails — or, worse on a lenient runtime, one export silently wins and every ray runs the wrong shader.
    // `D3D12_EXPORT_DESC::ExportToRename` is exactly the mechanism for this, and it is why each library below
    // renames its single export.
    struct DxrPipe
    {
        const void*                    key[6]{}; // rg / ms / ch / ah / isect / callable — every stage is identity
        ComPtr<ID3D12StateObject>      state;
        ComPtr<ID3D12Resource>         sbt;
        D3D12_GPU_VIRTUAL_ADDRESS      sbt_va       = 0;
        UINT64                         rec_stride   = 0;
        bool                           has_callable = false; // REN-38-F13: record 3 is the callable identifier
    };

    // ⛔ REN-38 audit: the A16 Vulkan scar in its DX12 form, found by the FIRST DX12 RT-pipeline device gate.
    // The DXR device is created LAZILY by the first trace, so `m_dxr_device != nullptr` answered "no
    // ray-tracing pipeline" on a DXR adapter until the feature had already been used — a capability query that
    // depends on the capability having been exercised is not a query. Answer from the feature check itself.
    [[nodiscard]] bool supports_rt_pipeline() const noexcept override
    {
        if (m_dxr_device != nullptr) { return true; }
        if (m_device == nullptr) { return false; }
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
        return SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5)))
               && o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0;
    }

    void trace_rays(IGpuProgram& raygen, IGpuProgram& miss, IGpuProgram& closest_hit, IAccelerationStructure& as,
                    crd::u32 width, crd::u32 height, IStorageBuffer* const* buffers, crd::u32 count)
    {
        trace_rays_impl(raygen, miss, closest_hit, nullptr, as, width, height, buffers, count);
    }

    // ⭐ REN-38 audit (the full RT hit group): the same trace with an ANY-HIT stage in the hit group — the
    // traversal calls it per candidate and `IgnoreHit()` rejects the transparent ones.
    void trace_rays_anyhit(IGpuProgram& raygen, IGpuProgram& miss, IGpuProgram& closest_hit, IGpuProgram& any_hit,
                           IAccelerationStructure& as, crd::u32 width, crd::u32 height,
                           IStorageBuffer* const* buffers, crd::u32 count)
    {
        trace_rays_impl(raygen, miss, closest_hit, &any_hit, as, width, height, buffers, count);
    }

    // REN-38-F13: the FULL vocabulary — procedural hit groups + the callable table.
    void trace_rays_full(IGpuProgram& raygen, IGpuProgram& miss, IGpuProgram& closest_hit, IGpuProgram* any_hit,
                         IGpuProgram* intersection, IGpuProgram* callable, IAccelerationStructure& as,
                         crd::u32 width, crd::u32 height, IStorageBuffer* const* buffers, crd::u32 count)
    {
        trace_rays_impl(raygen, miss, closest_hit, any_hit, as, width, height, buffers, count, intersection,
                        callable);
    }

    void trace_rays_impl(IGpuProgram& raygen, IGpuProgram& miss, IGpuProgram& closest_hit, IGpuProgram* any_hit,
                         IAccelerationStructure& as, crd::u32 width, crd::u32 height,
                         IStorageBuffer* const* buffers, crd::u32 count, IGpuProgram* intersection = nullptr,
                         IGpuProgram* callable = nullptr)
    {
        if (!m_ok || !frame_recording() || buffers == nullptr || count == 0U) { return; }
        if (!ensure_dxr()) { return; }
        const crd::u64 tlas_va = dx12_scene_tlas(as);
        if (tlas_va == 0U) { return; } // ⛔ never trace against address 0 — undefined, not a frame of misses
        auto* rg = dynamic_cast<Dx12GpuProgram*>(&raygen);
        auto* ms = dynamic_cast<Dx12GpuProgram*>(&miss);
        auto* ch = dynamic_cast<Dx12GpuProgram*>(&closest_hit);
        auto* ah = any_hit != nullptr ? dynamic_cast<Dx12GpuProgram*>(any_hit) : nullptr;
        auto* is = intersection != nullptr ? dynamic_cast<Dx12GpuProgram*>(intersection) : nullptr;
        auto* cl = callable != nullptr ? dynamic_cast<Dx12GpuProgram*>(callable) : nullptr;
        if (rg == nullptr || ms == nullptr || ch == nullptr) { return; }
        if (any_hit != nullptr && ah == nullptr) { return; } // a named any-hit that is not ours is never dropped silently
        if (intersection != nullptr && is == nullptr) { return; } // F13: same rule for the last two stages
        if (callable != nullptr && cl == nullptr) { return; }
        DxrPipe* pipe = dxr_pipeline(rg->dxil(), ms->dxil(), ch->dxil(),
                                     ah != nullptr ? ah->dxil() : crd::containers::ConstSpan<crd::u8>{},
                                     is != nullptr ? is->dxil() : crd::containers::ConstSpan<crd::u8>{},
                                     cl != nullptr ? cl->dxil() : crd::containers::ConstSpan<crd::u8>{});
        if (pipe == nullptr) { return; }

        ComPtr<ID3D12GraphicsCommandList4> list4;
        if (FAILED(m_list.As(&list4))) { return; }
        const crd::u32 n = count < kMaxKernelBuffers ? count : kMaxKernelBuffers;
        list4->SetComputeRootSignature(rt_kernel_root());
        list4->SetComputeRootShaderResourceView(0, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(tlas_va));
        list4->SetComputeRootDescriptorTable(1, alloc_kernel_uav_run(buffers, n));
        list4->SetPipelineState1(pipe->state.Get());

        D3D12_DISPATCH_RAYS_DESC drd{};
        drd.RayGenerationShaderRecord.StartAddress = pipe->sbt_va;
        drd.RayGenerationShaderRecord.SizeInBytes  = pipe->rec_stride;
        drd.MissShaderTable.StartAddress           = pipe->sbt_va + pipe->rec_stride;
        drd.MissShaderTable.SizeInBytes            = pipe->rec_stride;
        drd.MissShaderTable.StrideInBytes          = pipe->rec_stride;
        drd.HitGroupTable.StartAddress             = pipe->sbt_va + pipe->rec_stride * 2U;
        drd.HitGroupTable.SizeInBytes              = pipe->rec_stride;
        drd.HitGroupTable.StrideInBytes            = pipe->rec_stride;
        // REN-38-F13: the CALLABLE table is the fourth record when the pipeline carries one
        if (pipe->has_callable)
        {
            drd.CallableShaderTable.StartAddress  = pipe->sbt_va + pipe->rec_stride * 3U;
            drd.CallableShaderTable.SizeInBytes   = pipe->rec_stride;
            drd.CallableShaderTable.StrideInBytes = pipe->rec_stride;
        }
        drd.Width  = width > 0U ? width : 1U;
        drd.Height = height > 0U ? height : 1U;
        drd.Depth  = 1U;
        list4->DispatchRays(&drd);
        uav_write_barrier(); // the RT stages write UAVs — same COMPUTE→anything hazard as a dispatch
    }

    [[nodiscard]] bool ensure_dxr()
    {
        if (m_dxr_tried) { return m_dxr_device != nullptr; }
        m_dxr_tried = true;
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
        if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5)))
            || o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
        {
            return false; // no DXR ⇒ the verb is a NO-OP, and `supports_rt_pipeline()` says so
        }
        if (FAILED(m_device.As(&m_dxr_device))) { m_dxr_device.Reset(); return false; }
        return true;
    }

    [[nodiscard]] DxrPipe* dxr_pipeline(crd::containers::ConstSpan<crd::u8> rg,
                                        crd::containers::ConstSpan<crd::u8> ms,
                                        crd::containers::ConstSpan<crd::u8> ch,
                                        crd::containers::ConstSpan<crd::u8> ah = {},
                                        crd::containers::ConstSpan<crd::u8> isect = {},
                                        crd::containers::ConstSpan<crd::u8> call = {})
    {
        const bool has_ah = ah.data() != nullptr && ah.size() > 0U;
        const bool has_is = isect.data() != nullptr && isect.size() > 0U; // REN-38-F13
        const bool has_cl = call.data() != nullptr && call.size() > 0U;
        for (crd::u32 i = 0; i < m_dxr_n; ++i)
        {
            if (m_dxr[i].key[0] == rg.data() && m_dxr[i].key[1] == ms.data() && m_dxr[i].key[2] == ch.data()
                && m_dxr[i].key[3] == ah.data() && m_dxr[i].key[4] == isect.data() && m_dxr[i].key[5] == call.data())
            {
                return &m_dxr[i];
            }
        }
        if (m_dxr_n >= kKernelPsoCap) { return nullptr; }
        ID3D12RootSignature* root = rt_kernel_root();
        if (root == nullptr) { return nullptr; }

        // ── the state object: 3–4 renamed DXIL libraries · 1 hit group · shader config · pipeline config ·
        // root sig. REN-38 audit: the ANY-HIT is a fourth renamed library joining the same hit group. ──
        static const wchar_t* rg_name = L"crd_rgen";
        static const wchar_t* ms_name = L"crd_miss";
        static const wchar_t* ch_name = L"crd_chit";
        static const wchar_t* ah_name = L"crd_ahit";
        static const wchar_t* is_name = L"crd_isect"; // REN-38-F13
        static const wchar_t* cl_name = L"crd_call";
        static const wchar_t* hg_name = L"crd_hitgroup";

        D3D12_EXPORT_DESC ex[6]{};
        ex[0].Name = rg_name; ex[0].ExportToRename = L"main";
        ex[1].Name = ms_name; ex[1].ExportToRename = L"main";
        ex[2].Name = ch_name; ex[2].ExportToRename = L"main";
        ex[3].Name = ah_name; ex[3].ExportToRename = L"main";
        ex[4].Name = is_name; ex[4].ExportToRename = L"main";
        ex[5].Name = cl_name; ex[5].ExportToRename = L"main";
        D3D12_DXIL_LIBRARY_DESC libs[6]{};
        const crd::containers::ConstSpan<crd::u8> code[6] = {rg, ms, ch, ah, isect, call};
        crd::u32                                  n_libs  = 0U;
        for (crd::u32 i = 0; i < 6U; ++i)
        {
            if (code[i].data() == nullptr || code[i].size() == 0U) { continue; }
            libs[n_libs].DXILLibrary = {code[i].data(), code[i].size()};
            libs[n_libs].NumExports  = 1U;
            libs[n_libs].pExports    = &ex[i];
            ++n_libs;
        }
        D3D12_HIT_GROUP_DESC hg{};
        hg.HitGroupExport           = hg_name;
        // REN-38-F13: an intersection shader makes the hit group PROCEDURAL — its AABBs only bound the shape
        hg.Type                     = has_is ? D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE : D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hg.ClosestHitShaderImport   = ch_name;
        if (has_ah) { hg.AnyHitShaderImport = ah_name; }
        if (has_is) { hg.IntersectionShaderImport = is_name; }
        // ⛔ The payload is `struct RtPayload { float m0; ... }` from the CKIR emitter and the attributes are
        // `BuiltInTriangleIntersectionAttributes` (2 floats). Declaring these too SMALL is undefined behaviour the
        // runtime does not always catch, so both are sized generously from what the emitter can produce.
        D3D12_RAYTRACING_SHADER_CONFIG shader_cfg{};
        shader_cfg.MaxPayloadSizeInBytes   = 64U;
        shader_cfg.MaxAttributeSizeInBytes = 8U;
        D3D12_RAYTRACING_PIPELINE_CONFIG pipe_cfg{};
        pipe_cfg.MaxTraceRecursionDepth = 1U;
        D3D12_GLOBAL_ROOT_SIGNATURE grs{};
        grs.pGlobalRootSignature = root;

        D3D12_STATE_SUBOBJECT so[10]{};
        crd::u32              n_so = 0U;
        for (crd::u32 i = 0; i < n_libs; ++i) { so[n_so++] = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libs[i]}; }
        so[n_so++] = {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg};
        so[n_so++] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_cfg};
        so[n_so++] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipe_cfg};
        so[n_so++] = {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &grs};
        D3D12_STATE_OBJECT_DESC sod{};
        sod.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        sod.NumSubobjects = n_so;
        sod.pSubobjects   = so;

        DxrPipe out{};
        out.key[0] = rg.data();
        out.key[1] = ms.data();
        out.key[2] = ch.data();
        out.key[3] = ah.data(); // REN-38 audit: the any-hit joins the pipeline identity
        out.key[4] = isect.data(); // REN-38-F13: and so do the intersection + callable stages
        out.key[5] = call.data();
        out.has_callable = has_cl;
        if (FAILED(m_dxr_device->CreateStateObject(&sod, IID_PPV_ARGS(&out.state)))) { return nullptr; }
        ComPtr<ID3D12StateObjectProperties> props;
        if (FAILED(out.state.As(&props))) { return nullptr; }

        // ── the SBT: three records. ⛔ The RECORD stride must be a multiple of
        // D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT (32) and each TABLE base a multiple of
        // D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT (64). Using one number for both is the classic DXR bug
        // where the miss table lands mid-record and every miss runs a garbage shader identifier.
        constexpr UINT64 rec_bytes = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT; // 64 ⇒ satisfies both rules
        out.rec_stride        = rec_bytes;
        const crd::u32   n_rec = has_cl ? 4U : 3U; // REN-38-F13: the callable identifier is record 3
        const UINT64 total    = rec_bytes * n_rec;
        out.sbt               = make_upload_buffer(m_device.Get(), total);
        if (out.sbt == nullptr) { return nullptr; }
        void* mapped = nullptr;
        if (FAILED(out.sbt->Map(0, nullptr, &mapped))) { return nullptr; }
        std::memset(mapped, 0, static_cast<crd::usize>(total));
        auto*                 dst   = static_cast<crd::u8*>(mapped);
        const wchar_t* const  ids[4] = {rg_name, ms_name, hg_name, cl_name};
        for (crd::u32 i = 0; i < n_rec; ++i)
        {
            const void* id = props->GetShaderIdentifier(ids[i]);
            if (id == nullptr) { out.sbt->Unmap(0, nullptr); return nullptr; }
            std::memcpy(dst + rec_bytes * i, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        }
        out.sbt->Unmap(0, nullptr);
        out.sbt_va = out.sbt->GetGPUVirtualAddress();

        m_dxr[m_dxr_n] = out;
        ++m_dxr_n;
        return &m_dxr[m_dxr_n - 1U];
    }

    // ⭐ REN-38-B3: the DX12 mirror — zero a buffer range inside the frame.
    // ⛔ D3D12 has NO `FillBuffer`. The portable way to zero a UAV is `ClearUnorderedAccessViewUint`, which needs
    // BOTH a shader-visible descriptor AND a non-shader-visible one for the same view — an API quirk with no
    // Vulkan counterpart, and getting it wrong is a silent no-op rather than an error. The non-shader-visible
    // heap is created once, lazily, purely for this.
    // ── ⭐ REN-38-B8: the ACTIVE sampler, cached into the shader-visible sampler heap. ──
    // ⛔ D3D12 samplers live in a HEAP, not as free-standing objects, so "cache" here means "which heap slot" —
    // and the heap is fixed-size and shader-visible, which is why the capacity is declared up front and a full
    // cache falls back to slot 0 rather than resizing something the GPU may be reading.
    void set_sampler(const SamplerDesc& desc) override { m_active_sampler_slot = sampler_slot_for(desc); }

    // ── ⭐ REN-38 audit: the DECLARED per-pass raster state. On D3D12 everything except the stencil REFERENCE
    // is PSO state, so `pass_pso` threads the state into every PSO lookup (default state ⇒ the identical cache
    // key, so synchronous paths are untouched) and `apply_stencil_ref` sets the one command-list piece after
    // each pipeline bind. Reset at every pass boundary in `frame_rec_new_pass` — the B8 sampler discipline. ──
    void set_pass_state(const PassRasterState& state) override { m_pass_state = state; }

    [[nodiscard]] ID3D12PipelineState* pass_pso(Dx12RasterProgram& p, crd::u32 samples, DXGI_FORMAT dsv,
                                                D3D12_COMPARISON_FUNC depth_func, bool conservative,
                                                crd::u32 num_rts = 1U, DXGI_FORMAT rt_fmt = kColorFormat,
                                                const BlendMode* blend = nullptr)
    {
        return p.pso_for(samples, dsv, depth_func, conservative, num_rts, rt_fmt, blend, &m_pass_state);
    }

    void apply_stencil_ref()
    {
        if (m_pass_state.stencil_enable && m_list != nullptr) { m_list->OMSetStencilRef(m_pass_state.stencil_ref); }
    }

    [[nodiscard]] crd::u32 sampler_slot_for(const SamplerDesc& d)
    {
        if (m_sampler_heap == nullptr) { return 0U; }
        for (crd::u32 i = 0; i < m_sampler_n; ++i)
        {
            const SamplerDesc& k = m_sampler_key[i];
            if (k.min_filter == d.min_filter && k.mag_filter == d.mag_filter && k.mip_filter == d.mip_filter
                && k.address == d.address && k.compare == d.compare && k.anisotropy == d.anisotropy
                && k.mip_bias == d.mip_bias)
            {
                return 2U + i;
            }
        }
        if (m_sampler_n >= kSamplerCacheCap) { return d.compare ? 1U : 0U; }
        D3D12_SAMPLER_DESC sd{};
        // ⛔ D3D12 packs min/mag/mip filtering + comparison into ONE enum rather than three fields, so the mapping
        // is a lookup, not a field copy. Anisotropy is a FILTER MODE here (not a separate toggle as in Vulkan) and
        // it OVERRIDES the min/mag/mip choice — asking for both is not additive, it is a different filter.
        const bool aniso = d.anisotropy > 1U;
        if (aniso) { sd.Filter = d.compare ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC; }
        else if (d.min_filter == SamplerFilter::Nearest && d.mag_filter == SamplerFilter::Nearest)
        {
            sd.Filter = d.compare ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
        }
        else { sd.Filter = d.compare ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_LINEAR; }
        D3D12_TEXTURE_ADDRESS_MODE am = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        switch (d.address)
        {
        case SamplerAddress::ClampToEdge:   am = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; break;
        case SamplerAddress::ClampToBorder: am = D3D12_TEXTURE_ADDRESS_MODE_BORDER; break;
        case SamplerAddress::Mirror:        am = D3D12_TEXTURE_ADDRESS_MODE_MIRROR; break;
        case SamplerAddress::Repeat:
        default:                            am = D3D12_TEXTURE_ADDRESS_MODE_WRAP; break;
        }
        sd.AddressU       = am;
        sd.AddressV       = am;
        sd.AddressW       = am;
        sd.MipLODBias     = d.mip_bias;
        sd.MaxAnisotropy  = aniso ? d.anisotropy : 1U;
        sd.ComparisonFunc = d.compare ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_NEVER;
        sd.MaxLOD         = D3D12_FLOAT32_MAX;
        for (int c = 0; c < 4; ++c) { sd.BorderColor[c] = 1.0F; } // a shadow lookup outside its frustum must be LIT
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_sampler_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(2U + m_sampler_n) * m_sampler_inc;
        m_device->CreateSampler(&sd, h);
        m_sampler_key[m_sampler_n] = d;
        ++m_sampler_n;
        return 2U + (m_sampler_n - 1U);
    }

    // REN-40-D: the GPU handle of the PLAIN depth sampler's fixed heap slot (see the heap-size note) — the s6
    // table every atlas-carrying draw binds alongside s5.
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE plain_depth_tbl() const noexcept
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_sampler_heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(2U + kSamplerCacheCap) * m_sampler_inc;
        return h;
    }

    // ⭐⭐ REN-40-D: the s5 table for an atlas draw, chosen by WHAT THE ATLAS IS — the comparison sampler for a
    // depth atlas (a shadow lookup), the LINEAR/CLAMP sampler for a colour one (the moment atlas, whose whole
    // point is ordinary filterable sampling). Keyed off the texture, so no call site can pick wrong.
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE atlas_samp_tbl(const Dx12Texture& atl) const noexcept
    {
        const UINT64                slot = atl.is_depth() ? 1U : (3U + kSamplerCacheCap);
        D3D12_GPU_DESCRIPTOR_HANDLE h    = m_sampler_heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += slot * m_sampler_inc;
        return h;
    }

    // The sampler slot a recorded draw binds: the pass's when it declared one, else the historical default for
    // the draw's own kind (0 = filtering, 1 = comparison), which is what every existing gate expects.
    [[nodiscard]] crd::u32 active_sampler_slot(crd::u32 fallback) const noexcept
    {
        return m_active_sampler_slot != 0xFFFFFFFFU ? m_active_sampler_slot : fallback;
    }

    void fill_buffer(IStorageBuffer& buffer, crd::u64 offset, crd::u64 size, crd::u32 value) override
    {
        if (!m_ok || !frame_recording()) { return; }
        ID3D12Resource* res = dx_buffer_of(buffer);
        if (res == nullptr || !ensure_clear_heap()) { return; }
        const crd::u32 first = static_cast<crd::u32>(offset / 4U);
        const crd::u32 n     = static_cast<crd::u32>((size == 0U ? buffer.size_bytes() - offset : size) / 4U);
        if (n == 0U) { return; }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format                     = DXGI_FORMAT_R32_UINT; // a RAW u32 view — the fill value is a u32
        uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement        = first;
        uav.Buffer.NumElements         = n;
        m_device->CreateUnorderedAccessView(res, nullptr, &uav, m_clear_heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_frame_rec.heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(m_frame_rec.cursor) * m_frame_rec.inc;
        m_device->CreateUnorderedAccessView(res, nullptr, &uav, cpu);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_frame_rec.heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(m_frame_rec.cursor) * m_frame_rec.inc;
        ++m_frame_rec.cursor;
        const UINT vals[4] = {value, value, value, value};
        m_list->ClearUnorderedAccessViewUint(gpu, m_clear_heap->GetCPUDescriptorHandleForHeapStart(), res, vals, 0,
                                             nullptr);
        uav_write_barrier();
    }

    [[nodiscard]] bool ensure_clear_heap()
    {
        if (m_clear_heap != nullptr) { return true; }
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 1U;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // ⛔ NON-shader-visible, which is the whole point
        return SUCCEEDED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_clear_heap)));
    }

    void draw_visbuffer_load(IRasterTarget& target, IRasterProgram& program, crd::u32 vertex_count)
    {
        if (!m_ok || !frame_recording()) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        ID3D12PipelineState* pso =
            pass_pso(p, 1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, DXGI_FORMAT_R32_UINT);
        if (!p.valid() || pso == nullptr) { return; }
        record_plain_topology(t, p, pso, ClearColor{}, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, vertex_count,
                              /*clear=*/false);
    }

    // ⭐ REN-38-A12: the WBOIT composite — fullscreen, bindless, LOAD, BLEND. The blend is folded into the PSO
    // (D3D12 bakes blend state, unlike Vulkan's dynamic equations) and therefore into the PSO CACHE KEY, which
    // 38-A15 already established.
    void draw_bindless_blend_load(IRasterTarget& target, IRasterProgram& program, ITexture* const* textures,
                                  crd::u32 count, crd::u32 vertex_count, BlendMode blend)
    {
        if (!m_ok || !frame_recording() || textures == nullptr || count == 0U) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        const BlendMode      modes[1] = {blend};
        ID3D12PipelineState* pso      = pass_pso(p, 1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U,
                                                  t.color_format(), static_cast<const BlendMode*>(modes));
        if (!p.valid() || pso == nullptr) { return; }
        record_bindless(t, p, pso, textures, count, ClearColor{}, vertex_count, /*clear=*/false);
    }

    void draw_visbuffer(IRasterTarget& target, IRasterProgram& program, crd::u32 /*clear_id*/, crd::u32 vertex_count)
    {
        if (!m_ok) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        ID3D12PipelineState* pso = pass_pso(p, 1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, DXGI_FORMAT_R32_UINT);
        if (!p.valid() || pso == nullptr) { return; }
        // REN-38-A1e: inside a frame, RECORD. `ClearRenderTargetView` takes a FLOAT[4] even for an R32_UINT
        // target, and D3D12 reinterprets those bits as the typed clear — so the id goes in as its bit pattern,
        // which is what the synchronous path above does too (it clears to 0). No allocator/list reset, no
        // transitions, no readback: all graph-owned.
        if (frame_recording())
        {
            record_plain_topology(t, p, pso, ClearColor{0.0F, 0.0F, 0.0F, 0.0F},
                                  D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, vertex_count);
            return;
        }

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
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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
                    DepthCompare compare, crd::u32 vertex_count)
    {
        if (!m_ok) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        if (!t.has_depth()) { return; } // needs a create_color_depth_target target
        ID3D12PipelineState* pso = pass_pso(p, 1U, t.dsv_format(), to_d3d12_compare(compare), false, 1U, t.color_format()); // depth-enabled PSO (baked)
        if (!p.valid() || pso == nullptr) { return; }
        if (frame_recording()) { record_plain(t, p, pso, clear, true, clear_depth, vertex_count); return; }

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);

        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = t.dsv(); // depth stays in DEPTH_WRITE (created that way; never copied)
        m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr);

        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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
                  ShadingRateCombiner primitive_combiner, crd::u32 vertex_count)
    {
        if (!m_ok) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        if (m_list5 == nullptr || m_vrs_tier == D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED)
        {
            draw(target, program, clear, vertex_count); // no VRS ⇒ a plain 1x1 draw
            return;
        }
        ID3D12PipelineState* pso = pass_pso(p, t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format());
        if (!p.valid() || pso == nullptr) { return; }
        // REN-38-A1g: inside a frame, RECORD. VRS is DYNAMIC state on DX12, so it is set after the PSO is
        // bound and before the draw; `record_plain_topology` with vertex_count 0 sets up the pass without
        // drawing, then the rate + optional rate IMAGE go on and the draw follows.
        if (frame_recording())
        {
            const D3D12_SHADING_RATE_COMBINER fcomb[2] = {
                to_d3d12_combiner(primitive_combiner),
                t.has_vrs() ? D3D12_SHADING_RATE_COMBINER_OVERRIDE : D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
            record_plain_topology(t, p, pso, clear, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 0U);
            m_list5->RSSetShadingRate(to_d3d12_rate(pipeline_rate), fcomb);
            if (t.has_vrs()) { m_list5->RSSetShadingRateImage(t.vrs_tex()); }
            m_list->DrawInstanced(vertex_count, 1, 0, 0);
            return;
        }

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
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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
                           crd::u32 vertex_count)
    {
        if (!m_ok) { return; }
        auto&      t            = static_cast<Dx12RasterTarget&>(target);
        auto&      p            = static_cast<Dx12RasterProgram&>(program);
        const bool conservative = mode != ConservativeMode::Off && supports_conservative_raster();
        if (!conservative) { draw(target, program, clear, vertex_count); return; } // no conservative raster ⇒ a normal draw
        const bool           ms  = t.multisampled();
        ID3D12PipelineState* pso = pass_pso(p, t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, true,
                                             1U, t.color_format());
        // REN-38-A1g: conservative raster is BAKED INTO THE PSO on DX12 (unlike Vulkan's dynamic state), so the
        // frame path is exactly `record_plain_topology` with that PSO — nothing extra to set.
        if (frame_recording() && pso != nullptr)
        {
            record_plain_topology(t, p, pso, clear, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, vertex_count);
            return;
        }
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
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);

        if (ms) // resolve then copy (a conservative overestimate + MSAA test exercises both)
        {
            transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            transition(t.resolve(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            m_list->ResolveSubresource(t.resolve(), 0, t.tex(), 0, t.color_format());
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
        // ⭐⭐ inside a batch, the upload is a ring memcpy + one recorded copy — no committed resource, no submit,
        // no wait. False (ring alloc failed) falls through to the synchronous body below.
        if (m_batch_open && upload_batched(s, byte_offset, data, size_bytes)) { return true; }

        ComPtr<ID3D12Resource> upload = make_upload_buffer(m_device.Get(), size_bytes);
        if (upload == nullptr) { return false; }
        void* umap = nullptr;
        if (FAILED(upload->Map(0, nullptr, &umap))) { return false; }
        std::memcpy(umap, data, size_bytes);
        upload->Unmap(0, nullptr);

        // ⛔⛔ REN-39-D1: AN UPLOAD DURING FRAME RECORDING MUST NOT TOUCH THE DEDICATED PAIR. While a frame graph
        // executes, `m_cmd_alloc`/`m_list` ARE the graph's ring pair (frame_rec_begin swaps them in) — so the
        // synchronous body below would `Reset()` the allocator and list the graph is still recording into,
        // DISCARDING every command already written, then submit a list holding nothing but this copy. The frame
        // that followed was garbage and the device was REMOVED, after which every `CreateCommittedResource` in
        // the process failed — which is how a 480-byte upload buffer ends up "unavailable" on a healthy adapter.
        // The overlay is the first caller that ever uploads from INSIDE a pass, so nothing had exercised it.
        // Vulkan is immune by construction: its synchronous path opens its OWN command buffer (`begin_cmd`).
        // Here the copy joins the FRAME's list, bracketed by the same UAV↔COPY_DEST pair, and the staging buffer
        // is retained until that frame's fence retires it.
        if (frame_recording())
        {
            frame_transition(s.buf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            m_list->CopyBufferRegion(s.buf(), byte_offset, upload.Get(), 0, size_bytes);
            frame_transition(s.buf(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_frame_uploads[m_ring_slot].push_back(upload);
            return true;
        }

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        transition(s.buf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        m_list->CopyBufferRegion(s.buf(), byte_offset, upload.Get(), 0, size_bytes);
        transition(s.buf(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit_and_wait();
        return true;
    }

    void draw_storage(IRasterTarget& target, IRasterProgram& program, ClearColor clear, IStorageBuffer& storage,
                      crd::u32 vertex_count)
    {
        if (!m_ok || m_uav_heap == nullptr) { return; }
        auto&                t   = static_cast<Dx12RasterTarget&>(target);
        auto&                p   = static_cast<Dx12RasterProgram&>(program);
        auto&                s   = static_cast<Dx12StorageBuffer&>(storage);
        ID3D12PipelineState* pso = pass_pso(p, 1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format()); // single-sample colour
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
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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
                                 DepthCompare compare, IStorageBuffer& storage, crd::u32 vertex_count)
    {
        if (!m_ok || m_uav_heap == nullptr) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        if (!t.has_depth()) { return; }
        ID3D12PipelineState* pso = pass_pso(p, 1U, t.dsv_format(), to_d3d12_compare(compare), false, /*num_rts*/ 0U);
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
        m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr);
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        ID3D12DescriptorHeap* heaps[] = {m_uav_heap.Get()};
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetGraphicsRootDescriptorTable(0, m_uav_heap->GetGPUDescriptorHandleForHeapStart());
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);
        submit_and_wait(); // (submit_and_wait closes the list itself, like every other standalone draw)
    }

    // REN-3.1: the CONTINUING depth-only draw — mesh N>0 of a shadow pass joins the SAME depth map (no clear).
    void draw_storage_depth_only_load(IRasterTarget& target, IRasterProgram& program, DepthCompare compare,
                                      IStorageBuffer& storage, crd::u32 vertex_count)
    {
        if (!m_ok || m_uav_heap == nullptr) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        if (!t.has_depth()) { return; }
        ID3D12PipelineState* pso = pass_pso(p, 1U, t.dsv_format(), to_d3d12_compare(compare), false, /*num_rts*/ 0U);
        if (!p.valid() || pso == nullptr) { return; }
        if (frame_recording()) { record_depth_only(t, p, s, pso, false, 0.0F, vertex_count); }
    }

    void draw_storage_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear, float clear_depth,
                            DepthCompare compare, IStorageBuffer& storage, crd::u32 vertex_count)
    {
        if (!m_ok || m_uav_heap == nullptr) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        // ⛔ A DEPTH-LESS target is drawn COLOUR-ONLY, never silently skipped. The PSO must match: no DSV bound
        // means DXGI_FORMAT_UNKNOWN for the depth-stencil format, or the pipeline disagrees with the OM state.
        const bool           depth_on = t.has_depth();
        ID3D12PipelineState* pso =
            pass_pso(p, 1U, depth_on ? kDepthFormat : DXGI_FORMAT_UNKNOWN, to_d3d12_compare(compare), false,
                     1U, t.color_format());
        if (!p.valid() || pso == nullptr) { return; }
        if (!depth_on && !frame_recording())
        {
            // The SYNCHRONOUS path below binds the DSV unconditionally, so it delegates rather than growing a
            // second shape.
            draw_storage(target, program, clear, storage, vertex_count);
            return;
        }

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
        m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr);

        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        ID3D12DescriptorHeap* heaps[] = {m_uav_heap.Get()};
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetGraphicsRootDescriptorTable(0, m_uav_heap->GetGPUDescriptorHandleForHeapStart());
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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
                                     crd::u32 vertex_count)
    {
        draw_storage_sampled_depth(target, program, clear, clear_depth, compare, storage, texture, vertex_count, 0U);
    }
    // REN-3.2-b: the shared body. sampler_slot picks FILTERING (0) or COMPARISON (1) from the sampler heap;
    // everything else is identical, which is why the shadowed draw needs no new root signature or set layout.
    // REN-38: `atlas` non-null ⇒ ALSO bind it at t4 with the comparison sampler at s5 — the combined
    // textured+shadowed draw (and, after the binding move, the shadowed-only FS reads t4/s5 too).
    void draw_storage_sampled_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                                    float clear_depth, DepthCompare compare, IStorageBuffer& storage,
                                    ITexture& texture, crd::u32 vertex_count, crd::u32 sampler_slot,
                                    Dx12Texture* atlas = nullptr)
    {
        if (!m_ok || m_uav_heap == nullptr || m_sampler_heap == nullptr) { return; }
        auto& t   = static_cast<Dx12RasterTarget&>(target);
        auto& p   = static_cast<Dx12RasterProgram&>(program);
        auto& s   = static_cast<Dx12StorageBuffer&>(storage);
        auto& tex = static_cast<Dx12Texture&>(texture);
        if (!t.has_depth()) { return; }
        ID3D12PipelineState* pso = pass_pso(p, 1U, t.dsv_format(), to_d3d12_compare(compare), false, 1U, t.color_format());
        if (!p.valid() || pso == nullptr) { return; }
        if (frame_recording())
        {
            record_scene_textured(t, p, s, tex, pso, true, clear, clear_depth, vertex_count, sampler_slot, atlas);
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
        D3D12_GPU_DESCRIPTOR_HANDLE atlas_gpu{};
        if (atlas != nullptr) // REN-38: the atlas rides its OWN heap slot (2+kBindlessMax) and root table (t4)
        {
            const D3D12_SHADER_RESOURCE_VIEW_DESC asrv    = atlas->srv();
            D3D12_CPU_DESCRIPTOR_HANDLE           acpu    = m_uav_heap->GetCPUDescriptorHandleForHeapStart();
            acpu.ptr += static_cast<SIZE_T>(2U + kBindlessMax) * m_srv_inc;
            m_device->CreateShaderResourceView(atlas->tex(), &asrv, acpu);
            atlas_gpu = m_uav_heap->GetGPUDescriptorHandleForHeapStart();
            atlas_gpu.ptr += static_cast<UINT64>(2U + kBindlessMax) * m_srv_inc;
        }

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = t.dsv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr);
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
        if (atlas != nullptr)
        {
            m_list->SetGraphicsRootDescriptorTable(4, atlas_gpu); // shadow atlas (t4)
            // REN-40-D: s5 keyed by WHAT the atlas is — comparison for depth, LINEAR/CLAMP for moments.
            m_list->SetGraphicsRootDescriptorTable(5, atlas_samp_tbl(static_cast<Dx12Texture&>(*atlas)));
            m_list->SetGraphicsRootDescriptorTable(8, plain_depth_tbl()); // plain depth sampler (s6, REN-40-D)
        }
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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
                                          IStorageBuffer& storage, ITexture& texture, crd::u32 vertex_count)
    {
        auto& t   = static_cast<Dx12RasterTarget&>(target);
        auto& p   = static_cast<Dx12RasterProgram&>(program);
        auto& s   = static_cast<Dx12StorageBuffer&>(storage);
        auto& tex = static_cast<Dx12Texture&>(texture);
        if (!m_ok || !t.has_depth()) { return; }
        ID3D12PipelineState* pso = pass_pso(p, 1U, t.dsv_format(), to_d3d12_compare(compare), false, 1U, t.color_format());
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
                                     ITexture& shadow_atlas, crd::u32 vertex_count)
    {
        // REN-38: the shadowed FS reads the atlas at t4/s5 now — route it through the atlas channel as well.
        auto& atlas = static_cast<Dx12Texture&>(shadow_atlas);
        draw_storage_sampled_depth(target, program, clear, clear_depth, compare, storage, shadow_atlas,
                                   vertex_count, 1U, &atlas);
    }
    void draw_storage_shadowed_depth_load(IRasterTarget& target, IRasterProgram& program, DepthCompare compare,
                                          IStorageBuffer& storage, ITexture& shadow_atlas,
                                          crd::u32 vertex_count)
    {
        auto& t   = static_cast<Dx12RasterTarget&>(target);
        auto& p   = static_cast<Dx12RasterProgram&>(program);
        auto& s   = static_cast<Dx12StorageBuffer&>(storage);
        auto& tex = static_cast<Dx12Texture&>(shadow_atlas);
        if (!m_ok || !t.has_depth()) { return; }
        ID3D12PipelineState* pso = pass_pso(p, 1U, t.dsv_format(), to_d3d12_compare(compare), false, 1U, t.color_format());
        if (!p.valid() || pso == nullptr) { return; }
        if (frame_recording())
        {
            record_scene_textured(t, p, s, tex, pso, false, ClearColor{}, 0.0F, vertex_count, 1U, &tex);
            return;
        }
        draw_storage_shadowed_depth(target, program, ClearColor{}, 0.0F, compare, storage, shadow_atlas,
                                    vertex_count);
    }

    // ── ⭐⭐ REN-38: the COMBINED textured+shadowed scene draw (the DX12 face). Base colour at t1/s2, the
    // shadow atlas at ITS OWN t4/s5 — one draw samples both, ending the REN-3.2-b either/or.
    void draw_storage_textured_shadowed_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                                              float clear_depth, DepthCompare compare, IStorageBuffer& storage,
                                              ITexture& texture, ITexture& shadow_atlas,
                                              crd::u32 vertex_count)
    {
        auto& atlas = static_cast<Dx12Texture&>(shadow_atlas);
        draw_storage_sampled_depth(target, program, clear, clear_depth, compare, storage, texture, vertex_count, 0U,
                                   &atlas);
    }
    void draw_storage_textured_shadowed_depth_load(IRasterTarget& target, IRasterProgram& program,
                                                   DepthCompare compare, IStorageBuffer& storage, ITexture& texture,
                                                   ITexture& shadow_atlas, crd::u32 vertex_count)
    {
        auto& t     = static_cast<Dx12RasterTarget&>(target);
        auto& p     = static_cast<Dx12RasterProgram&>(program);
        auto& s     = static_cast<Dx12StorageBuffer&>(storage);
        auto& tex   = static_cast<Dx12Texture&>(texture);
        auto& atlas = static_cast<Dx12Texture&>(shadow_atlas);
        if (!m_ok || !t.has_depth()) { return; }
        ID3D12PipelineState* pso = pass_pso(p, 1U, t.dsv_format(), to_d3d12_compare(compare), false, 1U, t.color_format());
        if (!p.valid() || pso == nullptr) { return; }
        if (frame_recording())
        {
            record_scene_textured(t, p, s, tex, pso, false, ClearColor{}, 0.0F, vertex_count, 0U, &atlas);
            return;
        }
        draw_storage_textured_shadowed_depth(target, program, ClearColor{}, 0.0F, compare, storage, texture,
                                             shadow_atlas, vertex_count);
    }

    // ── ⭐⭐ REN-38 MULTI-DRAW: N storage scene draws, ONE ExecuteIndirect. The 38.8x batching headroom at 64
    // draws was per-draw root-table setting + PSO re-binding; this pays them once per BATCH. The command
    // signature {ROOT_CONSTANT(b7), DRAW} varies the DrawIndex per command: command i writes
    // `first_draw_index + i` into b7 before its draw — D3D12's native spelling of gl_DrawID.
    // RAF-12.4-F6: DX12 had no draw_storage_multi_depth_only override — it used the interface default (a loop over
    // draw_storage_depth_only / _load). That default is now no-op'd (the interface can no longer call the private F6
    // verbs), so DX12 needs this concrete loop to keep the depth-only pull shadow-cascade path — byte-identical to the
    // old default behaviour (its DrawIndex-rebasing gap, documented on the interface verb, is unchanged).
    void draw_storage_multi_depth_only(IRasterTarget& target, IRasterProgram& program, float clear_depth,
                                       DepthCompare compare, IStorageBuffer& storage, const crd::u32* vertex_counts,
                                       crd::u32 count, crd::u32 first_draw_index, bool load_target)
    {
        (void)first_draw_index;
        if (vertex_counts == nullptr) { return; }
        for (crd::u32 i = 0; i < count; ++i)
        {
            if (i == 0U && !load_target)
            {
                draw_storage_depth_only(target, program, clear_depth, compare, storage, vertex_counts[i]);
            }
            else { draw_storage_depth_only_load(target, program, compare, storage, vertex_counts[i]); }
        }
    }

    void draw_storage_multi_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                                  float clear_depth, DepthCompare compare, IStorageBuffer& storage,
                                  const crd::u32* vertex_counts, crd::u32 count, crd::u32 first_draw_index,
                                  bool load_target)
    {
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        if (!m_ok || count == 0U || vertex_counts == nullptr || !t.has_depth()) { return; }
        ID3D12PipelineState* pso = pass_pso(p, 1U, t.dsv_format(), to_d3d12_compare(compare), false, 1U, t.color_format());
        if (!p.valid() || pso == nullptr) { return; }
        const crd::u32 n = count < kMultiMax ? count : kMultiMax;
        if (!frame_recording() || !ensure_multi(p.root()))
        {
            for (crd::u32 i = 0; i < n; ++i) // stub-shaped fallback — the interface documents the index gap
            {
                if (i == 0U && !load_target)
                {
                    draw_storage_depth(target, program, clear, clear_depth, compare, storage, vertex_counts[i]);
                }
                else { draw_storage_depth_load(target, program, compare, storage, vertex_counts[i]); }
            }
            return;
        }

        // write this batch's chunk of the args ring: {u32 draw_index, D3D12_DRAW_ARGUMENTS} per command
        const crd::u32 chunk  = m_multi_cursor % kMultiChunks;
        m_multi_cursor        = (m_multi_cursor + 1U) % kMultiChunks;
        const crd::u64 offset = static_cast<crd::u64>(chunk) * kMultiMax * kMultiStride;
        crd::u8*       w      = m_multi_map + offset;
        for (crd::u32 i = 0; i < n; ++i)
        {
            const crd::u32 di      = first_draw_index + i;
            const crd::u32 args[5] = {di, vertex_counts[i], 1U, 0U, 0U};
            std::memcpy(w + static_cast<crd::u64>(i) * kMultiStride, static_cast<const void*>(args), kMultiStride);
        }

        const D3D12_GPU_DESCRIPTOR_HANDLE table = frame_alloc_storage_slot(s);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv   = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv   = t.dsv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        if (!load_target)
        {
            const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
            m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
            if (!m_next_load_depth) { m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr); }
        }
        m_next_load_depth = false;
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, table);
        m_list->SetPipelineState(pso);
        apply_stencil_ref();
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->ExecuteIndirect(m_multi_sig.Get(), n, m_multi_args.Get(), offset, nullptr, 0);
        ++m_multi_batches;
    }

    [[nodiscard]] crd::u64 multi_batch_count() const noexcept override { return m_multi_batches; }
    [[nodiscard]] crd::u64 multi_indexed_batch_count() const noexcept override { return m_multi_indexed_batches; }

    // lazily build the args ring + the {ROOT_CONSTANT(b7), DRAW} command signature. ⛔ A signature containing a
    // root-constant argument must be created AGAINST the root signature it patches, so the cache is keyed on it
    // (every raster program shares one root-signature SHAPE but owns its object; rebuilding on change is cheap
    // and correct where caching on the first would silently patch the wrong object).
    [[nodiscard]] bool ensure_multi(ID3D12RootSignature* root)
    {
        if (m_multi_args == nullptr)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width            = static_cast<crd::u64>(kMultiChunks) * kMultiMax * kMultiStride;
            rd.Height           = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.SampleDesc       = {1, 0};
            rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&m_multi_args))))
            {
                return false;
            }
            const D3D12_RANGE none{0, 0};
            void*             mapped = nullptr;
            if (FAILED(m_multi_args->Map(0, &none, &mapped))) { m_multi_args.Reset(); return false; }
            m_multi_map = static_cast<crd::u8*>(mapped);
        }
        if (m_multi_sig == nullptr || m_multi_sig_root != root)
        {
            D3D12_INDIRECT_ARGUMENT_DESC args[2]{};
            args[0].Type                              = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
            args[0].Constant.RootParameterIndex       = 6; // the DrawIndex root constant (b7)
            args[0].Constant.DestOffsetIn32BitValues  = 0;
            args[0].Constant.Num32BitValuesToSet      = 1;
            args[1].Type                              = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
            D3D12_COMMAND_SIGNATURE_DESC csd{};
            csd.ByteStride       = kMultiStride;
            csd.NumArgumentDescs = 2;
            csd.pArgumentDescs   = args;
            m_multi_sig.Reset();
            if (FAILED(m_device->CreateCommandSignature(&csd, root, IID_PPV_ARGS(&m_multi_sig)))) { return false; }
            m_multi_sig_root = root;
        }
        return true;
    }

    // ⭐⭐ REN-39-A2: the INDEXED twin — {ROOT_CONSTANT(b7), DRAW_INDEXED} against `root` (⛔ a root-constant
    // signature must be created AGAINST the root signature it patches — the 38-4 scar) + the 24-byte args ring.
    [[nodiscard]] bool ensure_multi_indexed(ID3D12RootSignature* root)
    {
        if (m_multi_idx_args == nullptr)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = static_cast<crd::u64>(kMultiChunks) * kMultiMax * kMultiIdxStride;
            rd.Height = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.SampleDesc = {1, 0};
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&m_multi_idx_args))))
            {
                return false;
            }
            const D3D12_RANGE none{0, 0};
            void* mapped = nullptr;
            if (FAILED(m_multi_idx_args->Map(0, &none, &mapped)))
            {
                m_multi_idx_args.Reset();
                return false;
            }
            m_multi_idx_map = static_cast<crd::u8*>(mapped);
        }
        if (m_multi_idx_sig == nullptr || m_multi_idx_sig_root != root)
        {
            D3D12_INDIRECT_ARGUMENT_DESC args[2]{};
            args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
            args[0].Constant.RootParameterIndex = 6; // the DrawIndex root constant (b7)
            args[0].Constant.DestOffsetIn32BitValues = 0;
            args[0].Constant.Num32BitValuesToSet = 1;
            args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
            D3D12_COMMAND_SIGNATURE_DESC csd{};
            csd.ByteStride = kMultiIdxStride;
            csd.NumArgumentDescs = 2;
            csd.pArgumentDescs = args;
            m_multi_idx_sig.Reset();
            if (FAILED(m_device->CreateCommandSignature(&csd, root, IID_PPV_ARGS(&m_multi_idx_sig))))
            {
                return false;
            }
            m_multi_idx_sig_root = root;
        }
        return true;
    }

    // ── ⭐⭐ REN-39-A2: INDEXED MULTI-DRAW (see IRasterContext) — ONE ExecuteIndirect over N DRAW_INDEXED
    // commands, the scene buffer bound ONCE through an IBV covering the index section, bracketed by the
    // UAV ↔ kIndexedDrawStates pair. DrawIndex rides the command signature's root constant, exactly as the
    // non-indexed multi; StartIndex comes from each command's first_index; BaseVertex/StartInstance are ALWAYS
    // 0 (the SV_InstanceID normalization — unrepresentable in IndexedDraw by design).
    void draw_storage_multi_indexed_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                                          float clear_depth, DepthCompare compare, IStorageBuffer& storage,
                                          crd::u32 index_offset_bytes, const IndexedDraw* draws, crd::u32 count,
                                          crd::u32 first_draw_index, bool load_target)
    {
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        if (!m_ok || count == 0U || draws == nullptr)
        {
            return;
        }
        if ((index_offset_bytes & 3U) != 0U || index_offset_bytes >= s.size_bytes())
        {
            return;
        }
        // ⭐ REN-38-A6: depth is OPTIONAL — a colour transient has none (the scene→post shape), and under the
        // REN-39 switch EVERY scene item routes here, so a depth refusal would black the whole frame. Found by
        // the 38-G1 DX12 post gate the moment the switch defaulted ON.
        const bool depth_on = t.has_depth();
        ID3D12PipelineState* pso =
            pass_pso(p, 1U, depth_on ? t.dsv_format() : DXGI_FORMAT_UNKNOWN, to_d3d12_compare(compare), false,
                     1U, t.color_format());
        if (!p.valid() || pso == nullptr)
        {
            return;
        }
        const crd::u32 n = count < kMultiMax ? count : kMultiMax;
        // every command's index range must sit inside the buffer's index section — REFUSED whole, never partial.
        // The IBV is sized to the FURTHEST index any command reaches, so an overrun is never addressable.
        const crd::u64 section_words = (static_cast<crd::u64>(s.size_bytes()) - index_offset_bytes) / 4U;
        crd::u64 need_words = 0U;
        for (crd::u32 i = 0; i < n; ++i)
        {
            const crd::u64 end = static_cast<crd::u64>(draws[i].first_index) + draws[i].index_count;
            if (draws[i].index_count == 0U || end > section_words)
            {
                return;
            }
            if (end > need_words)
            {
                need_words = end;
            }
        }
        if (!frame_recording() || !ensure_multi_indexed(p.root()))
        {
            for (crd::u32 i = 0; i < n; ++i) // stub-shaped fallback — the interface documents the index gap
            {
                draw_storage_indexed_depth(target, program, clear, clear_depth, compare, storage,
                                           index_offset_bytes + draws[i].first_index * 4U, draws[i].index_count,
                                           draws[i].instance_count, load_target || i > 0U);
            }
            return;
        }

        // write this batch's chunk of the INDEXED args ring: {u32 draw_index, D3D12_DRAW_INDEXED_ARGUMENTS}
        const crd::u32 chunk = m_multi_idx_cursor % kMultiChunks;
        m_multi_idx_cursor = (m_multi_idx_cursor + 1U) % kMultiChunks;
        const crd::u64 offset = static_cast<crd::u64>(chunk) * kMultiMax * kMultiIdxStride;
        crd::u8* w = m_multi_idx_map + offset;
        for (crd::u32 i = 0; i < n; ++i)
        {
            const crd::u32 di = first_draw_index + i;
            const crd::u32 args[6] = {di, draws[i].index_count, draws[i].instance_count, draws[i].first_index, 0U,
                                      0U}; // BaseVertex (as u32 bits of 0) + StartInstance — always 0
            std::memcpy(w + static_cast<crd::u64>(i) * kMultiIdxStride, static_cast<const void*>(args),
                        kMultiIdxStride);
        }

        frame_transition(s.buf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kIndexedDrawStates);
        const D3D12_GPU_DESCRIPTOR_HANDLE table = frame_alloc_storage_slot(s);
        const D3D12_GPU_DESCRIPTOR_HANDLE ro = frame_alloc_storage_srv_slot(s); // t0 (REN-39-C1)
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth_on ? t.dsv() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        m_list->OMSetRenderTargets(1, &rtv, FALSE, depth_on ? &dsv : nullptr);
        if (!load_target)
        {
            const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
            m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
            if (depth_on && !m_next_load_depth)
            {
                m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr);
            }
        }
        m_next_load_depth = false;
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, table);
        m_list->SetGraphicsRootDescriptorTable(7, ro); // t0: the read-only storage view (REN-39-C1)
        m_list->SetPipelineState(pso);
        apply_stencil_ref();
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = s.buf()->GetGPUVirtualAddress() + index_offset_bytes;
        ibv.SizeInBytes = static_cast<UINT>(need_words * 4U); // to the furthest index any command reaches
        ibv.Format = DXGI_FORMAT_R32_UINT;
        m_list->IASetIndexBuffer(&ibv);
        m_list->ExecuteIndirect(m_multi_idx_sig.Get(), n, m_multi_idx_args.Get(), offset, nullptr, 0);
        frame_transition(s.buf(), kIndexedDrawStates, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ++m_multi_batches;
        ++m_multi_indexed_batches; // REN-39-C1: indexed
    }

    // ── ⭐⭐ REN-39-C1: INDEXED DEPTH-ONLY MULTI-DRAW (see IRasterContext) — the cascade pass's indexed form.
    // record_depth_only's shape (ZERO RTVs, NumRenderTargets = 0 PSO) + the A2 ring/signature + the
    // kIndexedDrawStates bracket. Frame-recording only — the executor is the consumer.
    void draw_storage_multi_indexed_depth_only(IRasterTarget& target, IRasterProgram& program, float clear_depth,
                                               DepthCompare compare, IStorageBuffer& storage,
                                               crd::u32 index_offset_bytes, const IndexedDraw* draws, crd::u32 count,
                                               bool load_target)
    {
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        if (!m_ok || count == 0U || draws == nullptr || !frame_recording() || !t.has_depth())
        {
            return;
        }
        if ((index_offset_bytes & 3U) != 0U || index_offset_bytes >= s.size_bytes())
        {
            return;
        }
        ID3D12PipelineState* pso = pass_pso(p, 1U, t.dsv_format(), to_d3d12_compare(compare), false, /*num_rts*/ 0U);
        if (!p.valid() || pso == nullptr || !ensure_multi_indexed(p.root()))
        {
            return;
        }
        const crd::u32 n = count < kMultiMax ? count : kMultiMax;
        const crd::u64 section_words = (static_cast<crd::u64>(s.size_bytes()) - index_offset_bytes) / 4U;
        crd::u64 need_words = 0U;
        for (crd::u32 i = 0; i < n; ++i)
        {
            const crd::u64 end = static_cast<crd::u64>(draws[i].first_index) + draws[i].index_count;
            if (draws[i].index_count == 0U || end > section_words)
            {
                return;
            }
            if (end > need_words)
            {
                need_words = end;
            }
        }
        const crd::u32 chunk = m_multi_idx_cursor % kMultiChunks;
        m_multi_idx_cursor = (m_multi_idx_cursor + 1U) % kMultiChunks;
        const crd::u64 offset = static_cast<crd::u64>(chunk) * kMultiMax * kMultiIdxStride;
        crd::u8* w = m_multi_idx_map + offset;
        for (crd::u32 i = 0; i < n; ++i)
        {
            const crd::u32 args[6] = {i, draws[i].index_count, draws[i].instance_count, draws[i].first_index, 0U, 0U};
            std::memcpy(w + static_cast<crd::u64>(i) * kMultiIdxStride, static_cast<const void*>(args),
                        kMultiIdxStride);
        }

        frame_transition(s.buf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kIndexedDrawStates);
        const D3D12_GPU_DESCRIPTOR_HANDLE table = frame_alloc_storage_slot(s);
        const D3D12_GPU_DESCRIPTOR_HANDLE ro = frame_alloc_storage_srv_slot(s); // t0 (REN-39-C1)
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = t.dsv();
        m_list->OMSetRenderTargets(0, nullptr, FALSE, &dsv); // ⛔ zero RTVs: depth-only
        if (!load_target && !m_next_load_depth)
        {
            m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr);
        }
        m_next_load_depth = false;
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, table);
        m_list->SetGraphicsRootDescriptorTable(7, ro); // t0: the read-only storage view (REN-39-C1)
        m_list->SetPipelineState(pso);
        apply_stencil_ref();
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = s.buf()->GetGPUVirtualAddress() + index_offset_bytes;
        ibv.SizeInBytes = static_cast<UINT>(need_words * 4U);
        ibv.Format = DXGI_FORMAT_R32_UINT;
        m_list->IASetIndexBuffer(&ibv);
        m_list->ExecuteIndirect(m_multi_idx_sig.Get(), n, m_multi_idx_args.Get(), offset, nullptr, 0);
        frame_transition(s.buf(), kIndexedDrawStates, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ++m_multi_batches;
        ++m_multi_indexed_batches; // REN-39-C1: indexed
    }

    // ── ⭐⭐ REN-39-C1: the INDEXED SAMPLED scene draw (see IRasterContext) — record_scene_textured's binding
    // shape with tex/atlas by NULLABILITY, the index bind, and DrawIndexedInstanced. Frame-recording only.
    void draw_storage_indexed_sampled_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                                            float clear_depth, DepthCompare compare, IStorageBuffer& storage,
                                            crd::u32 index_offset_bytes, crd::u32 index_count, crd::u32 instance_count,
                                            crd::u32 first_index, ITexture* texture, ITexture* atlas,
                                            bool load_target, crd::u32 first_draw_index = 0U)
    {
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        if (!m_ok || !frame_recording())
        {
            return;
        }
        if ((index_offset_bytes & 3U) != 0U || index_count == 0U || instance_count == 0U ||
            static_cast<crd::u64>(index_offset_bytes) + (static_cast<crd::u64>(first_index) + index_count) * 4U >
                s.size_bytes())
        {
            return;
        }
        // ⭐ REN-38-A6: depth is OPTIONAL (the colour-transient shape) — see the multi-indexed body above.
        const bool depth_on = t.has_depth();
        ID3D12PipelineState* pso =
            pass_pso(p, 1U, depth_on ? t.dsv_format() : DXGI_FORMAT_UNKNOWN, to_d3d12_compare(compare), false,
                     1U, t.color_format());
        if (!p.valid() || pso == nullptr)
        {
            return;
        }

        frame_transition(s.buf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kIndexedDrawStates);
        const D3D12_GPU_DESCRIPTOR_HANDLE table = frame_alloc_storage_slot(s);
        const D3D12_GPU_DESCRIPTOR_HANDLE ro = frame_alloc_storage_srv_slot(s); // t0 (REN-39-C1)
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth_on ? t.dsv() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        m_list->OMSetRenderTargets(1, &rtv, FALSE, depth_on ? &dsv : nullptr);
        if (!load_target)
        {
            const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
            m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
            if (depth_on && !m_next_load_depth)
            {
                m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr);
            }
        }
        m_next_load_depth = false;
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, table);
        m_list->SetGraphicsRootDescriptorTable(7, ro); // t0: the read-only storage view (REN-39-C1)
        if (texture != nullptr)
        {
            auto& tex = static_cast<Dx12Texture&>(*texture);
            const D3D12_GPU_DESCRIPTOR_HANDLE srv_table = frame_alloc_srv_slot(tex);
            D3D12_GPU_DESCRIPTOR_HANDLE samp_gpu = m_sampler_heap->GetGPUDescriptorHandleForHeapStart();
            samp_gpu.ptr += static_cast<UINT64>(active_sampler_slot(0U)) * m_sampler_inc; // REN-38-B8
            m_list->SetGraphicsRootDescriptorTable(1, srv_table);                         // base-color SRV (t1)
            m_list->SetGraphicsRootDescriptorTable(2, samp_gpu);                          // sampler (s2)
        }
        if (atlas != nullptr)
        {
            auto& atl = static_cast<Dx12Texture&>(*atlas);
            const D3D12_GPU_DESCRIPTOR_HANDLE atlas_table = frame_alloc_srv_slot(atl);
            m_list->SetGraphicsRootDescriptorTable(4, atlas_table); // shadow atlas (t4)
            // REN-40-D: s5 keyed by WHAT the atlas is — comparison for depth, LINEAR/CLAMP for moments.
            m_list->SetGraphicsRootDescriptorTable(5, atlas_samp_tbl(atl));
            m_list->SetGraphicsRootDescriptorTable(8, plain_depth_tbl()); // plain depth sampler (s6, REN-40-D)
        }
        m_list->SetPipelineState(pso);
        m_list->SetGraphicsRoot32BitConstant(6, first_draw_index, 0);
        apply_stencil_ref();
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = s.buf()->GetGPUVirtualAddress() + index_offset_bytes;
        ibv.SizeInBytes = (first_index + index_count) * 4U; // to the furthest index this draw reaches
        ibv.Format = DXGI_FORMAT_R32_UINT;
        m_list->IASetIndexBuffer(&ibv);
        m_list->DrawIndexedInstanced(index_count, instance_count, first_index, 0, 0);
        frame_transition(s.buf(), kIndexedDrawStates, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // GEO-8: the CONTINUING scene draw — draw_storage_depth minus the Clear calls (colour + depth both persist;
    // depth keeps testing AND writing so mesh groups compose through the real depth buffer).
    void draw_storage_depth_load(IRasterTarget& target, IRasterProgram& program, DepthCompare compare,
                                 IStorageBuffer& storage, crd::u32 vertex_count)
    {
        if (!m_ok || m_uav_heap == nullptr) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        if (!t.has_depth()) { return; }
        ID3D12PipelineState* pso = pass_pso(p, 1U, t.dsv_format(), to_d3d12_compare(compare), false, 1U, t.color_format());
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
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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

    // ── ⭐⭐ REN-39-D1: THE OVERLAY DRAW, ON DX12. ────────────────────────────────────────────────────────────
    // Parity with the Vulkan verb (RET-6 / ADR-0105): composite instanced debug primitives ONTO a target's
    // EXISTING contents — no clears, standard alpha blending, and a READ-ONLY depth test when the target carries
    // depth and `compare` is not Always. ⛔ This backend had NO overlay implementation at all: the base class
    // returned false, so `crd::draw::submit_overlay` logged "refused" and the grid + gizmo + every debug shape
    // silently did not exist on DX12. An engine capability the second backend cannot reach is not a capability.
    //
    // Two things are deliberately NOT the pass state the caller installed:
    //   * depth WRITE is forced off — the overlay reads the scene's depth, never modifies it, so chained overlay
    //     draws all test against the same scene (the Vulkan verb's `vkCmdSetDepthWriteEnable(FALSE)`).
    //   * the blend is Alpha, unconditionally — the overlay's whole contract is compositing.
    // Both ride the PSO cache key (PassRasterState + BlendMode are part of it), so this cannot collide with a
    // pass that asked for opaque/depth-writing state.
    [[nodiscard]] bool draw_overlay(IRasterTarget& target, IRasterProgram& program, IStorageBuffer& storage,
                                    DepthCompare compare, crd::u32 vertex_count) // RAF-12.4: reached via friend encoder
    {
        return draw_overlay_range(target, program, storage, compare, 0U, vertex_count);
    }

    // The ranged twin (REN-39): `first_vertex` reaches `DrawInstanced`'s StartVertexLocation, which D3D12 folds
    // into SV_VertexID for a non-indexed draw exactly as Vulkan folds firstVertex into gl_VertexIndex — so the
    // expand-VS's `instance = VertexIndex / verts_per_instance` addressing selects the same bucket on both.
    // ⭐⭐ REN-39-D1: D3D12's NDC has +Y pointing UP the render target, the opposite of Vulkan. See the base
    // declaration for why this is a DECLARED backend fact and not a fix at each clip-to-UV call site.
    [[nodiscard]] bool ndc_y_points_down() const noexcept override { return false; }

    // ── ⭐⭐ REN-40-A: THE GPU-WRITTEN DRAW (see IRasterContext for the contract + the standing rule). ────────
    // D3D12 does this natively too: `ExecuteIndirect` has taken a `pCountBuffer` since 12.0 — every call site in
    // this file passed `nullptr, 0` for it until now, so the capability was present and simply unused. The
    // command signature is the SAME `m_multi_idx_sig` the CPU-args path already builds
    // ({ROOT_CONSTANT(b7) = DrawIndex, DRAW_INDEXED}), so a GPU producer writes the identical 6-u32 stride.
    // ⛔ D3D12 requires the args buffer in INDIRECT_ARGUMENT state and the count buffer likewise; the producer
    // pass owns those transitions (the frame graph emits them from the declared read), as on Vulkan.
    // ⛔ D3D12 signature = [ROOT_CONSTANT(b7) = DrawIndex][DRAW_INDEXED], the draw argument LAST as the API
    // requires — so the command is 24 bytes with the 5 draw args starting at byte 4.
    [[nodiscard]] crd::u32 indirect_command_stride() const noexcept override { return kMultiIdxStride; }
    [[nodiscard]] crd::u32 indirect_command_arg_offset() const noexcept override { return 4U; }
    [[nodiscard]] bool indirect_count_supported() const noexcept override { return true; }

    // ── ⭐⭐ REN-40-A: the GEOMETRY half of the GPU-written draw. See IRasterContext for the contract. ─────────
    // A term-for-term mirror of `draw_storage_indexed_sampled_depth` with the draw parameters and the COUNT taken
    // from device memory via `ExecuteIndirect`'s `pCountBuffer` — D3D12's own mechanism, not a levelled-down
    // emulation. ⛔ Both texture slots are bound exactly as that verb binds them (base colour t1/s2, atlas t4/s5).
    void draw_storage_multi_indexed_indirect(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                                             float clear_depth, DepthCompare compare, IStorageBuffer& storage,
                                             crd::u32 index_offset_bytes, ITexture* map, ITexture* atlas,
                                             IStorageBuffer& args, crd::u32 args_offset_bytes,
                                             IStorageBuffer* count_buf, crd::u32 count_offset_bytes,
                                             crd::u32 max_draws, bool load_target,
                                             crd::u32 first_draw_index)
    {
        // ⛔⛔ REN-40-C2 / DX12: the row is NOT pushed here — D3D12's command signature prepends a DrawIndex
        // root constant, so each command carries its OWN row and `ExecuteIndirect` supplies it per draw.
        // ⚠ THAT MAKES THE ROW THE PRODUCER'S JOB: `scene_cull_reset` writes `draw_index + slot`, and
        // `CullDesc::draw_index` is currently 0 — correct for the FIRST group only. A multi-group frame under
        // the device cull therefore needs the group's base row handed to the kernel (the params block, beside
        // `base_word`). Named here rather than left to be discovered: on Vulkan the same row arrives as a push
        // constant and is correct for every group.
        (void)first_draw_index;
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        auto& a = static_cast<Dx12StorageBuffer&>(args);
        if (!m_ok || max_draws == 0U || !frame_recording()) { return; }
        if ((index_offset_bytes & 3U) != 0U || index_offset_bytes >= s.size_bytes()) { return; }
        const crd::u64 need = static_cast<crd::u64>(args_offset_bytes)
                              + static_cast<crd::u64>(max_draws) * kMultiIdxStride;
        if ((args_offset_bytes & 3U) != 0U || need > a.size_bytes()) { return; }
        auto* cb = static_cast<Dx12StorageBuffer*>(count_buf);
        if (cb != nullptr && (static_cast<crd::u64>(count_offset_bytes) + 4ULL > cb->size_bytes()
                              || (count_offset_bytes & 3U) != 0U))
        {
            return;
        }
        const bool           depth_on = t.has_depth(); // REN-38-A6: depth is OPTIONAL
        ID3D12PipelineState* pso =
            pass_pso(p, 1U, depth_on ? t.dsv_format() : DXGI_FORMAT_UNKNOWN, to_d3d12_compare(compare), false,
                     1U, t.color_format());
        if (!p.valid() || pso == nullptr || !ensure_multi_indexed(p.root())) { return; }

        frame_transition(s.buf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kIndexedDrawStates);
        frame_transition(a.buf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        if (cb != nullptr && cb != &a)
        {
            frame_transition(cb->buf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        }
        const D3D12_GPU_DESCRIPTOR_HANDLE table = frame_alloc_storage_slot(s);
        const D3D12_GPU_DESCRIPTOR_HANDLE ro    = frame_alloc_storage_srv_slot(s); // t0 (REN-39-C1)
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv   = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv   = depth_on ? t.dsv() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        m_list->OMSetRenderTargets(1, &rtv, FALSE, depth_on ? &dsv : nullptr);
        if (!load_target)
        {
            const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
            m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
            if (depth_on && !m_next_load_depth) { m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr); }
        }
        m_next_load_depth = false;
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F,
                                1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, table);
        m_list->SetGraphicsRootDescriptorTable(7, ro);
        if (map != nullptr)
        {
            auto&                             tex       = static_cast<Dx12Texture&>(*map);
            const D3D12_GPU_DESCRIPTOR_HANDLE srv_table = frame_alloc_srv_slot(tex);
            D3D12_GPU_DESCRIPTOR_HANDLE       samp_gpu  = m_sampler_heap->GetGPUDescriptorHandleForHeapStart();
            samp_gpu.ptr += static_cast<UINT64>(active_sampler_slot(0U)) * m_sampler_inc; // REN-38-B8
            m_list->SetGraphicsRootDescriptorTable(1, srv_table);                         // base-colour SRV (t1)
            m_list->SetGraphicsRootDescriptorTable(2, samp_gpu);                          // sampler (s2)
        }
        if (atlas != nullptr)
        {
            auto&                             atl         = static_cast<Dx12Texture&>(*atlas);
            const D3D12_GPU_DESCRIPTOR_HANDLE atlas_table = frame_alloc_srv_slot(atl);
            m_list->SetGraphicsRootDescriptorTable(4, atlas_table); // shadow atlas (t4)
            // REN-40-D: s5 keyed by WHAT the atlas is — comparison for depth, LINEAR/CLAMP for moments.
            m_list->SetGraphicsRootDescriptorTable(5, atlas_samp_tbl(atl));
            m_list->SetGraphicsRootDescriptorTable(8, plain_depth_tbl()); // plain depth sampler (s6, REN-40-D)
        }
        m_list->SetPipelineState(pso);
        apply_stencil_ref();
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = s.buf()->GetGPUVirtualAddress() + index_offset_bytes;
        ibv.SizeInBytes    = static_cast<UINT>(s.size_bytes() - index_offset_bytes);
        ibv.Format         = DXGI_FORMAT_R32_UINT;
        m_list->IASetIndexBuffer(&ibv);
        m_list->ExecuteIndirect(m_multi_idx_sig.Get(), max_draws, a.buf(), args_offset_bytes,
                                cb != nullptr ? cb->buf() : nullptr, cb != nullptr ? count_offset_bytes : 0U);
        ++m_multi_batches;
        ++m_multi_indexed_batches; // REN-39-C1: indexed
    }

    void draw_storage_multi_indexed_depth_only_indirect(IRasterTarget& target, IRasterProgram& program,
                                                        float clear_depth, DepthCompare compare,
                                                        IStorageBuffer& storage, crd::u32 index_offset_bytes,
                                                        IStorageBuffer& args, crd::u32 args_offset_bytes,
                                                        IStorageBuffer* count_buf, crd::u32 count_offset_bytes,
                                                        crd::u32 max_draws, bool load_target,
                                             crd::u32 first_draw_index)
    {
        // ⛔⛔ REN-40-C2 / DX12: the row is NOT pushed here — D3D12's command signature prepends a DrawIndex
        // root constant, so each command carries its OWN row and `ExecuteIndirect` supplies it per draw.
        // ⚠ THAT MAKES THE ROW THE PRODUCER'S JOB: `scene_cull_reset` writes `draw_index + slot`, and
        // `CullDesc::draw_index` is currently 0 — correct for the FIRST group only. A multi-group frame under
        // the device cull therefore needs the group's base row handed to the kernel (the params block, beside
        // `base_word`). Named here rather than left to be discovered: on Vulkan the same row arrives as a push
        // constant and is correct for every group.
        (void)first_draw_index;
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        auto& a = static_cast<Dx12StorageBuffer&>(args);
        if (!m_ok || max_draws == 0U || !frame_recording() || !t.has_depth()) { return; }
        if ((index_offset_bytes & 3U) != 0U || index_offset_bytes >= s.size_bytes()) { return; }
        const crd::u64 need = static_cast<crd::u64>(args_offset_bytes)
                              + static_cast<crd::u64>(max_draws) * kMultiIdxStride;
        if ((args_offset_bytes & 3U) != 0U || need > a.size_bytes()) { return; }
        auto* cb = static_cast<Dx12StorageBuffer*>(count_buf);
        if (cb != nullptr && (static_cast<crd::u64>(count_offset_bytes) + 4ULL > cb->size_bytes()
                              || (count_offset_bytes & 3U) != 0U))
        {
            return;
        }
        ID3D12PipelineState* pso = pass_pso(p, 1U, t.dsv_format(), to_d3d12_compare(compare), false, /*num_rts*/ 0U);
        if (!p.valid() || pso == nullptr || !ensure_multi_indexed(p.root())) { return; }

        frame_transition(s.buf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kIndexedDrawStates);
        frame_transition(a.buf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        if (cb != nullptr && cb != &a)
        {
            frame_transition(cb->buf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        }
        const D3D12_GPU_DESCRIPTOR_HANDLE table = frame_alloc_storage_slot(s);
        const D3D12_GPU_DESCRIPTOR_HANDLE ro    = frame_alloc_storage_srv_slot(s); // t0 (REN-39-C1)
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv   = t.dsv();
        m_list->OMSetRenderTargets(0, nullptr, FALSE, &dsv); // ⛔ zero RTVs: depth-only
        if (!load_target && !m_next_load_depth) { m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr); }
        m_next_load_depth = false;
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F,
                                1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, table);
        m_list->SetGraphicsRootDescriptorTable(7, ro);
        m_list->SetPipelineState(pso);
        apply_stencil_ref();
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = s.buf()->GetGPUVirtualAddress() + index_offset_bytes;
        ibv.SizeInBytes    = static_cast<UINT>(s.size_bytes() - index_offset_bytes);
        ibv.Format         = DXGI_FORMAT_R32_UINT;
        m_list->IASetIndexBuffer(&ibv);
        m_list->ExecuteIndirect(m_multi_idx_sig.Get(), max_draws, a.buf(), args_offset_bytes,
                                cb != nullptr ? cb->buf() : nullptr, cb != nullptr ? count_offset_bytes : 0U);
        if (cb != nullptr && cb != &a)
        {
            frame_transition(cb->buf(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        frame_transition(a.buf(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        frame_transition(s.buf(), kIndexedDrawStates, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ++m_multi_batches;
        ++m_multi_indexed_batches; // REN-39-C1: indexed
    }



    [[nodiscard]] bool draw_overlay_range(IRasterTarget& target, IRasterProgram& program, IStorageBuffer& storage,
                                          DepthCompare compare, crd::u32 first_vertex,
                                          crd::u32 vertex_count) // RAF-12.4: reached via friend encoder
    {
        if (!m_ok || m_uav_heap == nullptr || vertex_count == 0U) { return false; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        if (t.samples() != 1U) { return false; } // the overlay canvas contract, same as Vulkan

        const bool      depth_on = t.has_depth() && compare != DepthCompare::Always;
        PassRasterState st       = m_pass_state;
        st.depth_write           = false;
        const BlendMode blend    = BlendMode::Alpha;
        ID3D12PipelineState* pso =
            p.pso_for(1U, depth_on ? t.dsv_format() : DXGI_FORMAT_UNKNOWN,
                      depth_on ? to_d3d12_compare(compare) : D3D12_COMPARISON_FUNC_ALWAYS, /*conservative=*/false,
                      1U, kColorFormat, &blend, &st);
        if (!p.valid() || pso == nullptr) { return false; }

        if (frame_recording())
        {
            record_overlay(t, p, s, pso, depth_on, first_vertex, vertex_count);
            return true;
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format                     = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement        = 0;
        uav.Buffer.NumElements         = s.num_elements();
        uav.Buffer.StructureByteStride = 4;
        m_device->CreateUnorderedAccessView(s.buf(), nullptr, &uav,
                                            m_uav_heap->GetCPUDescriptorHandleForHeapStart());

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);

        // ⛔ COMMON → RENDER_TARGET PRESERVES contents on D3D12 (unlike a Vulkan UNDEFINED acquire, which
        // discards) — so "load" here is simply the absence of a clear call. Chained overlay draws each park the
        // target back in COMMON, which is what makes them compose.
        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth_on ? t.dsv() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        m_list->OMSetRenderTargets(1, &rtv, FALSE, depth_on ? &dsv : nullptr);

        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        ID3D12DescriptorHeap* heaps[] = {m_uav_heap.Get()};
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetGraphicsRootDescriptorTable(0, m_uav_heap->GetGPUDescriptorHandleForHeapStart());
        m_list->SetPipelineState(pso);
        apply_stencil_ref();
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, first_vertex, 0);

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
        return true;
    }

    // ── ⭐⭐ REN-39-A1: the INDEXED storage draw (see IRasterContext) — the scene buffer serves as its OWN
    // index buffer. The IBV addresses the u32 index section at `index_offset_bytes` on the UAV-default resource;
    // ⛔ the buffer moves UNORDERED_ACCESS → INDEX_BUFFER | NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE
    // for the draw and BACK (the dispatch_kernel_indirect precedent: a new ROLE joins the state walk with a
    // locally-balanced pair, so every other walk's parked-in-UAV assumption stays true). The shader-read states
    // are in the combo because an indexed-mode program reads the SAME buffer read-only (the 39-B2 t-register
    // seam); UNORDERED_ACCESS cannot legally combine with INDEX_BUFFER, which is WHY storage is read-only during
    // an indexed draw — a program that stores uses the non-indexed verbs. StartIndex/BaseVertex/StartInstance
    // are ALWAYS 0: SV_InstanceID excludes StartInstanceLocation while gl_InstanceIndex includes firstInstance,
    // so the only portable offset channel is the draw table, never the draw call.
    void draw_storage_indexed_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear, float clear_depth,
                                    DepthCompare compare, IStorageBuffer& storage, crd::u32 index_offset_bytes,
                                    crd::u32 index_count, crd::u32 instance_count, bool load_target)
    {
        if (!m_ok || m_uav_heap == nullptr)
        {
            return;
        }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        auto& s = static_cast<Dx12StorageBuffer&>(storage);
        // a 4-aligned, in-range u32 index section or the draw is REFUSED — never a partial IA fetch
        if ((index_offset_bytes & 3U) != 0U || index_count == 0U || instance_count == 0U ||
            static_cast<crd::u64>(index_offset_bytes) + static_cast<crd::u64>(index_count) * 4U > s.size_bytes())
        {
            return;
        }
        ID3D12PipelineState* pso =
            pass_pso(p, 1U, t.has_depth() ? t.dsv_format() : DXGI_FORMAT_UNKNOWN, to_d3d12_compare(compare), false,
                     1U, t.color_format());
        if (!p.valid() || pso == nullptr)
        {
            return;
        }

        if (frame_recording())
        {
            record_scene_indexed(t, p, s, pso, !load_target, clear, clear_depth, index_offset_bytes, index_count,
                                 instance_count);
            return;
        }
        if (!t.has_depth())
        {
            return;
        } // the sync rig draws depth targets; frame mode handles depthless

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement = 0;
        uav.Buffer.NumElements = s.num_elements();
        uav.Buffer.StructureByteStride = 4;
        m_device->CreateUnorderedAccessView(s.buf(), nullptr, &uav, m_uav_heap->GetCPUDescriptorHandleForHeapStart());
        // REN-39-C1: the READ-ONLY view at its persistent slot — an indexed program reads t0 (root table 7)
        // while the resource sits in kIndexedDrawStates, where the u0 UAV access would be illegal.
        D3D12_SHADER_RESOURCE_VIEW_DESC ro{};
        ro.Format = DXGI_FORMAT_UNKNOWN;
        ro.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        ro.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ro.Buffer.FirstElement = 0;
        ro.Buffer.NumElements = s.num_elements();
        ro.Buffer.StructureByteStride = 4;
        D3D12_CPU_DESCRIPTOR_HANDLE ro_cpu = m_uav_heap->GetCPUDescriptorHandleForHeapStart();
        ro_cpu.ptr += static_cast<SIZE_T>(3U + kBindlessMax) * m_srv_inc;
        m_device->CreateShaderResourceView(s.buf(), &ro, ro_cpu);

        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);

        transition(t.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(s.buf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kIndexedDrawStates);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = t.dsv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        if (!load_target)
        {
            const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
            m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
            if (!m_next_load_depth) { m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr); }
        }
        m_next_load_depth = false;

        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        ID3D12DescriptorHeap* heaps[] = {m_uav_heap.Get()};
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetGraphicsRootDescriptorTable(0, m_uav_heap->GetGPUDescriptorHandleForHeapStart());
        {
            D3D12_GPU_DESCRIPTOR_HANDLE ro_gpu = m_uav_heap->GetGPUDescriptorHandleForHeapStart();
            ro_gpu.ptr += static_cast<UINT64>(3U + kBindlessMax) * m_srv_inc;
            m_list->SetGraphicsRootDescriptorTable(7, ro_gpu); // t0: the read-only storage view (REN-39-C1)
        }
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = s.buf()->GetGPUVirtualAddress() + index_offset_bytes;
        ibv.SizeInBytes = index_count * 4U; // exactly the section the draw reads — an overrun is never legal
        ibv.Format = DXGI_FORMAT_R32_UINT;
        m_list->IASetIndexBuffer(&ibv);
        m_list->DrawIndexedInstanced(index_count, instance_count, 0, 0, 0);
        transition(s.buf(), kIndexedDrawStates, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        transition(t.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = t.readback();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = t.tex();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
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
                       crd::u32 vertex_count)
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
        return std::make_unique<Dx12Texture>(std::move(tex), width, height, srv, /*depth=*/true);
    }

    void draw_shadow(IRasterTarget& target, IRasterProgram& program, ClearColor clear, ITexture& depth,
                     crd::u32 vertex_count)
    {
        draw_sampled(target, program, clear, static_cast<Dx12Texture&>(depth), 1U, vertex_count);
    }

    [[nodiscard]] bool supports_bindless() const noexcept override
    {
        return m_binding_tier >= D3D12_RESOURCE_BINDING_TIER_2;
    }

    // RAF-12.4 F4-depth: DX12 has no native bindless+DEPTH path (Vulkan's draw_bindless_depth renders the ocean's
    // depth-occluded grid). The pre-RAF-12.4 base default fell back to a DEPTHLESS bindless draw, so DX12 kept that
    // behaviour; preserve it as a concrete method now the verb is de-virtualized (reached only via the encoder). No DX12
    // caller exercises it today — the depth-occluded ocean grid gates on Vulkan only.
    void draw_bindless_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear, float clear_depth,
                             DepthCompare compare, ITexture* const* textures, crd::u32 count, crd::u32 vertex_count)
    {
        (void)clear_depth;
        (void)compare;
        draw_bindless(target, program, clear, textures, count, vertex_count);
    }

    void draw_bindless(IRasterTarget& target, IRasterProgram& program, ClearColor clear, ITexture* const* textures,
                       crd::u32 count, crd::u32 vertex_count)
    {
        if (!m_ok || m_uav_heap == nullptr || m_sampler_heap == nullptr || count == 0U || textures == nullptr) { return; }
        auto&                t   = static_cast<Dx12RasterTarget&>(target);
        auto&                p   = static_cast<Dx12RasterProgram&>(program);
        ID3D12PipelineState* pso = pass_pso(p, 1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format());
        if (!p.valid() || pso == nullptr) { return; }
        const crd::u32 n = count < static_cast<crd::u32>(kBindlessMax) ? count : static_cast<crd::u32>(kBindlessMax);
        // REN-38-A1a: inside a frame, RECORD — never stomp the global heap or reset the list mid-frame.
        if (frame_recording()) { record_bindless(t, p, pso, textures, n, clear, vertex_count); return; }

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
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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

    // ── REN-38-A1b: MRT into N of the FRAME GRAPH's own transients (the DX12 half). ─────────────────────────
    // ⛔ Each graph transient owns its OWN single-RTV heap (the REN-3.2 rule: a target renders to its heap's START
    // handle, so a shared heap would stack every attachment on slot 0). `OMSetRenderTargets` accepts an ARRAY of
    // CPU handles from DIFFERENT heaps as long as `RTsSingleHandleToDescriptorRange` is FALSE — which is exactly
    // the shape the graph produces, and why no descriptor copying is needed here.
    // ⛔ The graph has already transitioned every target to RENDER_TARGET; this must not re-transition them.
    // ── REN-38-A2: dispatch a CKIR compute kernel INTO THE FRAME'S command list (the DX12 half). ────────────
    // Same contract as Vulkan: records into the frame's ONE list, no allocator/list reset, no submit — the frame
    // is still one ExecuteCommandLists. ⛔ The kernel needs its OWN root signature (a UAV table u0..u{n-1}) and
    // its own PSO; the raster root signature is a graphics layout and cannot describe a compute dispatch.
    [[nodiscard]] ID3D12RootSignature* kernel_root()
    {
        if (m_kernel_root != nullptr) { return m_kernel_root.Get(); }
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = kMaxKernelBuffers;
        range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp.DescriptorTable.NumDescriptorRanges = 1;
        rp.DescriptorTable.pDescriptorRanges   = &range;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1;
        rsd.pParameters   = &rp;
        ComPtr<ID3DBlob> sig;
        ComPtr<ID3DBlob> err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { return nullptr; }
        if (FAILED(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                 IID_PPV_ARGS(&m_kernel_root))))
        {
            return nullptr;
        }
        return m_kernel_root.Get();
    }

    [[nodiscard]] ID3D12PipelineState* kernel_pipeline(const void* dxil, crd::usize size)
    {
        for (crd::u32 i = 0; i < m_kernel_n; ++i)
        {
            if (m_kernel_key[i] == dxil) { return m_kernel_pso[i].Get(); }
        }
        if (m_kernel_n >= kKernelPsoCap) { return nullptr; }
        ID3D12RootSignature* root = kernel_root();
        if (root == nullptr) { return nullptr; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root;
        pd.CS             = D3D12_SHADER_BYTECODE{dxil, size};
        ComPtr<ID3D12PipelineState> pso;
        if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) { return nullptr; }
        m_kernel_key[m_kernel_n] = dxil;
        m_kernel_pso[m_kernel_n] = pso;
        ++m_kernel_n;
        return m_kernel_pso[m_kernel_n - 1U].Get();
    }

    // ── ⭐ REN-38-A9: the RAY-QUERY root signature. ──
    // Root param 0 = a root SRV for the TLAS (t0) — DXR takes an acceleration structure BY GPU ADDRESS, so a root
    // SRV is not a shortcut here, it is the natural binding: there is no resource descriptor to place in a heap.
    // Root param 1 = the UAV table for the pass's buffers, based at u1 so the register numbering MATCHES Vulkan's
    // (TLAS at 0, buffers from 1). ⛔ One convention across both backends or a CKIR kernel is not portable, which
    // is the entire mission.
    [[nodiscard]] ID3D12RootSignature* rt_kernel_root()
    {
        if (m_rt_root != nullptr) { return m_rt_root.Get(); }
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors     = kMaxKernelBuffers;
        range.BaseShaderRegister = 1; // u1..uN — u0 is unused so the numbering matches Vulkan's bindings
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[0].Descriptor.ShaderRegister = 0; // t0 — the TLAS
        rp[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges   = &range;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2;
        rsd.pParameters   = rp;
        ComPtr<ID3DBlob> sig;
        ComPtr<ID3DBlob> err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { return nullptr; }
        if (FAILED(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                 IID_PPV_ARGS(&m_rt_root))))
        {
            return nullptr;
        }
        return m_rt_root.Get();
    }

    // ⛔ A SEPARATE cache from `kernel_pipeline`, keyed the same way: the two differ only by ROOT SIGNATURE, and a
    // shared cache would hand an RT dispatch the plain root the first time a module was seen as a plain kernel.
    [[nodiscard]] ID3D12PipelineState* rt_kernel_pipeline(const void* dxil, crd::usize size)
    {
        for (crd::u32 i = 0; i < m_rt_pso_n; ++i)
        {
            if (m_rt_pso_key[i] == dxil) { return m_rt_pso[i].Get(); }
        }
        if (m_rt_pso_n >= kKernelPsoCap) { return nullptr; }
        ID3D12RootSignature* root = rt_kernel_root();
        if (root == nullptr) { return nullptr; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root;
        pd.CS             = {dxil, size};
        ComPtr<ID3D12PipelineState> pso;
        if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) { return nullptr; }
        m_rt_pso_key[m_rt_pso_n] = dxil;
        m_rt_pso[m_rt_pso_n]     = pso;
        ++m_rt_pso_n;
        return m_rt_pso[m_rt_pso_n - 1U].Get();
    }

    // REN-40-G3: sampled-compute root — UAV table (u0..u7) + SRV table (t8) + static NEAREST/CLAMP sampler (s9).
    [[nodiscard]] ID3D12RootSignature* sampled_kernel_root()
    {
        if (m_sampled_kernel_root != nullptr) { return m_sampled_kernel_root.Get(); }
        D3D12_DESCRIPTOR_RANGE uav_range{};
        uav_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav_range.NumDescriptors     = kMaxKernelBuffers;
        uav_range.BaseShaderRegister = 0;
        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors     = 1;
        srv_range.BaseShaderRegister = kMaxKernelBuffers; // t8
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = 1;
        rp[0].DescriptorTable.pDescriptorRanges   = &uav_range;
        rp[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges   = &srv_range;
        D3D12_STATIC_SAMPLER_DESC ss{};
        ss.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
        ss.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.ShaderRegister   = kMaxKernelBuffers + 1U; // s9
        ss.RegisterSpace    = 0;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters     = 2;
        rsd.pParameters       = rp;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers   = &ss;
        ComPtr<ID3DBlob> sig;
        ComPtr<ID3DBlob> err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { return nullptr; }
        if (FAILED(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                 IID_PPV_ARGS(&m_sampled_kernel_root))))
        {
            return nullptr;
        }
        return m_sampled_kernel_root.Get();
    }

    [[nodiscard]] ID3D12PipelineState* sampled_kernel_pipeline(const void* dxil, crd::usize size)
    {
        for (crd::u32 i = 0; i < m_sampled_pso_n; ++i)
        {
            if (m_sampled_pso_key[i] == dxil) { return m_sampled_pso[i].Get(); }
        }
        if (m_sampled_pso_n >= kKernelPsoCap) { return nullptr; }
        ID3D12RootSignature* root = sampled_kernel_root();
        if (root == nullptr) { return nullptr; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root;
        pd.CS             = {dxil, size};
        ComPtr<ID3D12PipelineState> pso;
        if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) { return nullptr; }
        m_sampled_pso_key[m_sampled_pso_n] = dxil;
        m_sampled_pso[m_sampled_pso_n]     = pso;
        ++m_sampled_pso_n;
        return m_sampled_pso[m_sampled_pso_n - 1U].Get();
    }

    // A contiguous UAV run from the frame ring for the pass's buffers. Slots past `n` replicate slot 0 so every
    // descriptor the table covers is valid — the same rule every other table in this file follows.
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE alloc_kernel_uav_run(IStorageBuffer* const* buffers, crd::u32 n)
    {
        const UINT base = m_frame_rec.cursor;
        for (UINT i = 0; i < kMaxKernelBuffers; ++i)
        {
            IStorageBuffer&                  sb = *buffers[i < n ? i : 0U];
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format                     = DXGI_FORMAT_UNKNOWN;
            uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements         = dx_buffer_elems(sb);
            uav.Buffer.StructureByteStride = 4;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_frame_rec.heap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(base + i) * m_frame_rec.inc;
            m_device->CreateUnorderedAccessView(dx_buffer_of(sb), nullptr, &uav, cpu);
        }
        m_frame_rec.cursor += static_cast<UINT>(kMaxKernelBuffers);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_frame_rec.heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(base) * m_frame_rec.inc;
        return gpu;
    }

    void uav_write_barrier()
    {
        D3D12_RESOURCE_BARRIER uavb{};
        uavb.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavb.UAV.pResource = nullptr; // all UAVs
        m_list->ResourceBarrier(1, &uavb);
    }

    void dispatch_kernel_rt(IGpuProgram& kernel, IAccelerationStructure& as, crd::u32 gx, crd::u32 gy, crd::u32 gz,
                            IStorageBuffer* const* buffers, crd::u32 count)
    {
        if (!m_ok || !frame_recording() || buffers == nullptr || count == 0U) { return; }
        const crd::u64 tlas_va = dx12_scene_tlas(as);
        // ⛔ A scene that does not resolve is a NO-OP, never a dispatch against address 0 — DXR traversal from a
        // null AS is undefined, not a miss.
        if (tlas_va == 0U) { return; }
        auto* dx_prog = dynamic_cast<Dx12GpuProgram*>(&kernel);
        if (dx_prog == nullptr) { return; }
        const auto           code = dx_prog->dxil();
        ID3D12PipelineState* pso  = rt_kernel_pipeline(code.data(), code.size());
        if (pso == nullptr) { return; }
        const crd::u32 n = count < kMaxKernelBuffers ? count : kMaxKernelBuffers;

        m_list->SetComputeRootSignature(rt_kernel_root());
        m_list->SetComputeRootShaderResourceView(0, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(tlas_va));
        m_list->SetComputeRootDescriptorTable(1, alloc_kernel_uav_run(buffers, n));
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list->Dispatch(gx > 0U ? gx : 1U, gy > 0U ? gy : 1U, gz > 0U ? gz : 1U);
        uav_write_barrier();
    }

    // ── ⭐ REN-40-G3: SAMPLED-COMPUTE DISPATCH — the HZB verb (DX12). ──
    void dispatch_kernel_sampled(IGpuProgram& kernel, crd::u32 gx, crd::u32 gy, crd::u32 gz,
                                 IStorageBuffer* const* buffers, crd::u32 count, ITexture& tex)
    {
        if (!m_ok || !frame_recording() || buffers == nullptr || count == 0U) { return; }
        auto* dx_prog = dynamic_cast<Dx12GpuProgram*>(&kernel);
        if (dx_prog == nullptr) { return; }
        const auto           code = dx_prog->dxil();
        ID3D12PipelineState* pso  = sampled_kernel_pipeline(code.data(), code.size());
        if (pso == nullptr) { return; }
        const crd::u32 n = count < kMaxKernelBuffers ? count : kMaxKernelBuffers;

        D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu = alloc_kernel_uav_run(buffers, n);
        auto& dx_tex = static_cast<Dx12Texture&>(tex);
        const UINT srv_base = m_frame_rec.cursor;
        D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = m_frame_rec.heap->GetCPUDescriptorHandleForHeapStart();
        srv_cpu.ptr += static_cast<SIZE_T>(srv_base) * m_frame_rec.inc;
        m_device->CreateShaderResourceView(dx_tex.tex(), &dx_tex.srv(), srv_cpu);
        m_frame_rec.cursor += 1U;
        D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = m_frame_rec.heap->GetGPUDescriptorHandleForHeapStart();
        srv_gpu.ptr += static_cast<UINT64>(srv_base) * m_frame_rec.inc;

        m_list->SetComputeRootSignature(sampled_kernel_root());
        m_list->SetComputeRootDescriptorTable(0, uav_gpu);
        m_list->SetComputeRootDescriptorTable(1, srv_gpu);
        m_list->SetPipelineState(pso);
        apply_stencil_ref();
        m_list->Dispatch(gx > 0U ? gx : 1U, gy > 0U ? gy : 1U, gz > 0U ? gz : 1U);
        uav_write_barrier();
    }

    // ── ⭐ REN-38-A10: GPU-DRIVEN DISPATCH. ──
    void dispatch_kernel_indirect(IGpuProgram& kernel, IStorageBuffer& args, crd::u64 args_offset,
                                  IStorageBuffer* const* buffers, crd::u32 count)
    {
        if (!m_ok || !frame_recording() || buffers == nullptr || count == 0U) { return; }
        auto* dx_prog = dynamic_cast<Dx12GpuProgram*>(&kernel);
        if (dx_prog == nullptr) { return; }
        const auto           code = dx_prog->dxil();
        ID3D12PipelineState* pso  = kernel_pipeline(code.data(), code.size());
        if (pso == nullptr) { return; }
        ID3D12CommandSignature* sig = dispatch_indirect_sig();
        if (sig == nullptr) { return; }
        const crd::u32 n = count < kMaxKernelBuffers ? count : kMaxKernelBuffers;

        ID3D12Resource* ab = dx_buffer_of(args);
        if (ab == nullptr) { return; }
        // ⛔ D3D12 requires INDIRECT_ARGUMENT state on the args buffer; a storage buffer lives in UNORDERED_ACCESS
        // between passes, so the move is made here and made BACK — the graph's own state tracking stays true.
        frame_transition(ab, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        m_list->SetComputeRootSignature(kernel_root());
        m_list->SetComputeRootDescriptorTable(0, alloc_kernel_uav_run(buffers, n));
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list->ExecuteIndirect(sig, 1U, ab, args_offset, nullptr, 0U);
        frame_transition(ab, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        uav_write_barrier();
    }

    void draw_mesh_indirect_buffer(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                                   IStorageBuffer& args, crd::u64 args_offset)
    {
        if (!m_ok || !frame_recording() || !m_mesh_shader || m_list6 == nullptr) { return; }
        auto& t = static_cast<Dx12RasterTarget&>(target);
        auto& p = static_cast<Dx12RasterProgram&>(program);
        if (!p.is_mesh()) { return; }
        ID3D12PipelineState* pso = pass_pso(p, t.samples(), DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format());
        if (!p.valid() || pso == nullptr) { return; }
        ID3D12Resource* ab = dx_buffer_of(args);
        if (ab == nullptr) { return; }
        frame_transition(ab, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        record_mesh(t, p, pso, clear, 0U, false, 0.0F, nullptr, false, D3D12_SHADING_RATE_1X1, nullptr, ab,
                    args_offset);
        frame_transition(ab, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // The DISPATCH command signature (one D3D12_DISPATCH_ARGUMENTS: {x, y, z}) — built once, lazily.
    [[nodiscard]] ID3D12CommandSignature* dispatch_indirect_sig()
    {
        if (m_dispatch_sig != nullptr) { return m_dispatch_sig.Get(); }
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC csd{};
        csd.ByteStride       = sizeof(D3D12_DISPATCH_ARGUMENTS);
        csd.NumArgumentDescs = 1;
        csd.pArgumentDescs   = &arg;
        if (FAILED(m_device->CreateCommandSignature(&csd, nullptr, IID_PPV_ARGS(&m_dispatch_sig)))) { return nullptr; }
        return m_dispatch_sig.Get();
    }

    void dispatch_kernel(IGpuProgram& kernel, crd::u32 gx, crd::u32 gy, crd::u32 gz, IStorageBuffer* const* buffers,
                         crd::u32 count)
    {
        if (!m_ok || !frame_recording() || buffers == nullptr || count == 0U) { return; }
        auto* dx_prog = dynamic_cast<Dx12GpuProgram*>(&kernel);
        if (dx_prog == nullptr) { return; }
        const auto           code = dx_prog->dxil();
        ID3D12PipelineState* pso  = kernel_pipeline(code.data(), code.size());
        if (pso == nullptr) { return; }
        const crd::u32 n = count < kMaxKernelBuffers ? count : kMaxKernelBuffers;

        // A CONTIGUOUS run from the frame ring — a root descriptor table addresses N consecutive slots from one
        // GPU handle, the same rule `frame_alloc_bindless_run` follows. Slots past `n` replicate slot 0 so every
        // descriptor the table covers is valid.
        const UINT base = m_frame_rec.cursor;
        for (UINT i = 0; i < kMaxKernelBuffers; ++i)
        {
            IStorageBuffer&                  sb = *buffers[i < n ? i : 0U];
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format                     = DXGI_FORMAT_UNKNOWN;
            uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements         = dx_buffer_elems(sb);
            uav.Buffer.StructureByteStride = 4;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_frame_rec.heap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(base + i) * m_frame_rec.inc;
            m_device->CreateUnorderedAccessView(dx_buffer_of(sb), nullptr, &uav, cpu);
        }
        m_frame_rec.cursor += static_cast<UINT>(kMaxKernelBuffers);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_frame_rec.heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(base) * m_frame_rec.inc;

        m_list->SetComputeRootSignature(kernel_root());
        m_list->SetComputeRootDescriptorTable(0, gpu);
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list->Dispatch(gx > 0U ? gx : 1U, gy > 0U ? gy : 1U, gz > 0U ? gz : 1U);

        // ⛔ The COMPUTE->anything hazard: the graph knows the pass wrote a buffer, but the WRITE happens inside
        // the shader, so the UAV barrier is issued HERE. Without it a later pass can read the previous contents.
        D3D12_RESOURCE_BARRIER uavb{};
        uavb.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavb.UAV.pResource = nullptr; // all UAVs
        m_list->ResourceBarrier(1, &uavb);
    }

    void draw_storage_mrt(IRasterTarget* const* targets, crd::u32 count, IRasterProgram& program, ClearColor clear,
                          float clear_depth, DepthCompare compare, IStorageBuffer& storage, crd::u32 vertex_count,
                          const BlendMode* blend)
    {
        if (targets == nullptr || count == 0U || !frame_recording()) { return; }
        auto&          p  = static_cast<Dx12RasterProgram&>(program);
        auto&          sb = static_cast<Dx12StorageBuffer&>(storage);
        auto&          t0 = static_cast<Dx12RasterTarget&>(*targets[0]);
        const crd::u32 n  = count < 8U ? count : 8U;
        const bool     has_depth = t0.has_depth();
        // REN-38-A15: blend is PSO state on DX12, so it goes into `pso_for` (and its cache key) rather than being
        // set dynamically the way Vulkan does it. ⛔ RAF-7 fix: samples=1 (the attachments are single-sample) and
        // conservative=false. This verb historically passed samples=n / conservative=has_depth, a miswiring that only
        // never bit because MRT ran depthless; the RAF-7 one-submission MRT gate exercises it, so it uses the proven
        // shape now. (RAF-12: the indexed/indirect MRT verbs that shared this note are deleted — dead after the
        // record_pass inline-verb-path removal; a multi-colour G-buffer is executor-gated when one is needed.)
        ID3D12PipelineState* pso = pass_pso(p, 1U, has_depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN,
                                             to_d3d12_compare(compare), false, n, t0.color_format(), blend);
        if (!p.valid() || pso == nullptr) { return; }

        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8]{};
        for (crd::u32 i = 0; i < n; ++i) { rtvs[i] = static_cast<Dx12RasterTarget&>(*targets[i]).rtv(); }
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = has_depth ? t0.dsv() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        m_list->OMSetRenderTargets(n, static_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(rtvs), FALSE,
                                   has_depth ? &dsv : nullptr);
        const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
        const float mult_identity[4] = {1.0F, 1.0F, 1.0F, 1.0F};
        for (crd::u32 i = 0; i < n; ++i)
        {
            // ⛔ A MULTIPLICATIVE attachment (Multiply / RevealageMultiply — e.g. the WBOIT revealage) MUST clear to
            // the multiplicative IDENTITY 1, never the pass's clear_color: `dst·(1-src)` from 0 stays 0 forever, so
            // the background is never revealed. Mirrors the fused draw_wboit reveal-clear and the Vulkan MRT path.
            const bool mult = blend != nullptr &&
                              (blend[i] == BlendMode::Multiply || blend[i] == BlendMode::RevealageMultiply);
            m_list->ClearRenderTargetView(rtvs[i], mult ? mult_identity : rgba, 0, nullptr);
        }
        if (has_depth) { m_list->ClearDepthStencilView(dsv, t0.clear_flags(), clear_depth, 0, 0, nullptr); }
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t0.width()), static_cast<float>(t0.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t0.width()), static_cast<LONG>(t0.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, frame_alloc_storage_slot(sb));
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);
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

    void draw_gbuffer(IGBufferTarget& target, IRasterProgram& program, ClearColor clear,
                      crd::u32 vertex_count) // RAF-12.4: reached via friend encoder
    {
        if (!m_ok) { return; }
        auto&                t   = static_cast<Dx12GBufferTarget&>(target);
        auto&                p   = static_cast<Dx12RasterProgram&>(program);
        const crd::u32       n   = t.attachment_count();
        ID3D12PipelineState* pso = pass_pso(p, 1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, n);
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
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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

private:
    // B2: clear `target`, bind the texture's SRV (heap slot 1) + the sampler at `sampler_slot`, draw, copy back. Shared by
    // draw_textured (default sampler slot 0) and draw_shadow (comparison sampler slot 1); the SRV dim/format ride the texture.
    void draw_sampled(IRasterTarget& target, IRasterProgram& program, ClearColor clear, Dx12Texture& tex,
                      UINT sampler_slot, crd::u32 vertex_count)
    {
        if (!m_ok || m_uav_heap == nullptr || m_sampler_heap == nullptr) { return; }
        auto&                t   = static_cast<Dx12RasterTarget&>(target);
        auto&                p   = static_cast<Dx12RasterProgram&>(program);
        ID3D12PipelineState* pso = pass_pso(p, 1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U, t.color_format());
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
        samp_gpu.ptr += static_cast<UINT64>(active_sampler_slot(sampler_slot)) * m_sampler_inc; // REN-38-B8

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
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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

    // ── ⭐ REN-38-A6: THE DX12 BLIT — a fullscreen textured draw, because D3D12 has no copy-engine blit. ──
    // Built ONCE, lazily, and owned by the context: a root signature (one SRV table + one root constant, and two
    // STATIC samplers so no sampler heap is involved), plus a VS/PS pair compiled through the same dxc path every
    // other program in this module uses. Nothing about it is special-cased in the PSO cache — it is an ordinary
    // graphics pipeline that happens to belong to the context rather than to a user program.
    //
    // ⛔ The filter is a ROOT CONSTANT selecting between two static samplers rather than two pipelines: a blit's
    // filter is chosen per CALL (a visibility buffer must be Nearest in the same frame an HDR chain is Linear),
    // and keying the PSO on it would silently build a second pipeline the first time someone mixed them.
    struct BlitKit
    {
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pso;
        bool                        tried = false; // compiled-or-failed once; never retried per call
    };

    [[nodiscard]] bool ensure_blit()
    {
        if (m_blit.tried) { return m_blit.pso != nullptr; }
        m_blit.tried = true;

        static const char* blit_vs_src =
            "struct VSOut { float2 uv : TEXCOORD0; float4 pos : SV_Position; };\n"
            "VSOut main(uint vid : SV_VertexID)\n"
            "{\n"
            "    VSOut o;\n"
            "    float2 p = float2((vid << 1u) & 2u, vid & 2u);\n"
            "    o.uv  = p;\n"
            "    o.pos = float4(p * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
            "    return o;\n"
            "}\n";
        // ⛔ SV_Position LAST in VSOut (D-007's HLSL register-packing scar): a SV_Position declared first shifts
        // every user varying's register and the PS reads the wrong one.
        static const char* blit_ps_src =
            "struct VSOut { float2 uv : TEXCOORD0; float4 pos : SV_Position; };\n"
            "Texture2D<float4> g_src : register(t0);\n"
            "SamplerState g_point  : register(s0);\n"
            "SamplerState g_linear : register(s1);\n"
            "cbuffer BlitCb : register(b0) { uint g_filter; }\n"
            "float4 main(VSOut i) : SV_Target\n"
            "{\n"
            "    return g_filter != 0u ? g_src.Sample(g_linear, i.uv) : g_src.Sample(g_point, i.uv);\n"
            "}\n";

        const auto vs = compile_hlsl_to_dxil(ShaderStage::Vertex, crd::containers::StringView(blit_vs_src),
                                             crd::containers::StringView("crd_blit_vs"));
        const auto ps = compile_hlsl_to_dxil(ShaderStage::Fragment, crd::containers::StringView(blit_ps_src),
                                             crd::containers::StringView("crd_blit_ps"));
        if (!vs.ok || !ps.ok) { return false; } // dxc unavailable ⇒ blit_image is a NO-OP, never a wrong image

        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors                    = 1;
        srv_range.BaseShaderRegister                = 0; // t0
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges   = &srv_range;
        params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0; // b0
        params[1].Constants.Num32BitValues = 1;
        params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        for (int i = 0; i < 2; ++i)
        {
            samplers[i].AddressU        = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].AddressV        = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].AddressW        = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].MaxLOD          = D3D12_FLOAT32_MAX;
            samplers[i].ShaderRegister  = static_cast<UINT>(i);
            samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters     = 2;
        rsd.pParameters       = params;
        rsd.NumStaticSamplers = 2;
        rsd.pStaticSamplers   = samplers;
        rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> sig;
        ComPtr<ID3DBlob> err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { return false; }
        if (FAILED(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                 IID_PPV_ARGS(&m_blit.root))))
        {
            return false;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature                = m_blit.root.Get();
        pd.VS                            = {vs.dxil.data(), vs.dxil.size()};
        pd.PS                            = {ps.dxil.data(), ps.dxil.size()};
        pd.RasterizerState.FillMode      = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode      = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.DepthStencilState.DepthEnable   = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = kColorFormat;
        pd.SampleDesc.Count      = 1;
        return SUCCEEDED(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_blit.pso)));
    }

    // Record the blit draw. `srv_gpu` is a heap slot the CALLER minted (frame ring when recording, the global
    // heap when synchronous) so this body owns no descriptor allocation and is safe inside someone else's frame.
    void emit_blit_draw(Dx12RasterTarget& dst, D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu, BlitFilter filter)
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = dst.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(dst.width()), static_cast<float>(dst.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(dst.width()), static_cast<LONG>(dst.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(m_blit.root.Get());
        m_list->SetGraphicsRootDescriptorTable(0, srv_gpu);
        const UINT filter_u = filter == BlitFilter::Linear ? 1U : 0U;
        m_list->SetGraphicsRoot32BitConstant(1, filter_u, 0);
        m_list->SetPipelineState(m_blit.pso.Get());
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(3, 1, 0, 0);
    }

    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE blit_srv(ID3D12Resource* src, bool frame_ring)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format                  = kColorFormat;
        srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels     = 1;
        if (frame_ring)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_frame_rec.heap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(m_frame_rec.cursor) * m_frame_rec.inc;
            m_device->CreateShaderResourceView(src, &srv, cpu);
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_frame_rec.heap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += static_cast<UINT64>(m_frame_rec.cursor) * m_frame_rec.inc;
            ++m_frame_rec.cursor;
            return gpu;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_uav_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(m_srv_inc); // slot 1, the same slot draw_sampled uses synchronously
        m_device->CreateShaderResourceView(src, &srv, cpu);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_uav_heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(m_srv_inc);
        return gpu;
    }

    void copy_image(IRasterTarget& dst_t, IRasterTarget& src_t)
    {
        if (!m_ok) { return; }
        // ⛔ A MISMATCH IS A NO-OP, never a partial copy — CopyResource requires identical descs, and copying a
        // sub-region instead would read back as a plausible image.
        if (dst_t.width() != src_t.width() || dst_t.height() != src_t.height()) { return; }
        auto& dst = static_cast<Dx12RasterTarget&>(dst_t);
        auto& src = static_cast<Dx12RasterTarget&>(src_t);
        if (frame_recording()) { xfer_copy(m_list.Get(), dst, src); return; }
        sync_xfer(dst, src, false, BlitFilter::Nearest);
    }

    void resolve_image(IRasterTarget& dst_t, IRasterTarget& src_t)
    {
        if (!m_ok) { return; }
        if (dst_t.width() != src_t.width() || dst_t.height() != src_t.height()) { return; }
        auto& dst = static_cast<Dx12RasterTarget&>(dst_t);
        auto& src = static_cast<Dx12RasterTarget&>(src_t);
        // ⛔ A SINGLE-SAMPLE source is REJECTED rather than degraded to a copy: the author asked for a resolve, so
        // a non-multisampled source means the graph declared the wrong sample count.
        if (!src.multisampled()) { return; }
        if (frame_recording()) { xfer_resolve(m_list.Get(), dst, src); return; }
        sync_xfer(dst, src, true, BlitFilter::Nearest);
    }

    void blit_image(IRasterTarget& dst_t, IRasterTarget& src_t, BlitFilter filter)
    {
        if (!m_ok || !ensure_blit()) { return; }
        auto& dst = static_cast<Dx12RasterTarget&>(dst_t);
        auto& src = static_cast<Dx12RasterTarget&>(src_t);
        if (frame_recording())
        {
            // ⛔ The graph put `src` in COPY_SOURCE and `dst` in COPY_DEST because this is a TRANSFER pass — but
            // the DX12 blit is a DRAW, so it needs PIXEL_SHADER_RESOURCE and RENDER_TARGET instead. The verb
            // moves them and moves them BACK, so the graph's own state tracking stays true either way. This is
            // the price of the asymmetry, paid explicitly and locally rather than by widening the graph's model.
            frame_transition(src.copy_src(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            frame_transition(dst.tex(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
            emit_blit_draw(dst, blit_srv(src.copy_src(), true), filter);
            frame_transition(dst.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
            frame_transition(src.copy_src(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            return;
        }
        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        transition(src.copy_src(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(dst.tex(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12DescriptorHeap* heaps[] = {m_uav_heap.Get()};
        m_list->SetDescriptorHeaps(1, heaps);
        emit_blit_draw(dst, blit_srv(src.copy_src(), false), filter);
        transition(src.copy_src(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        transition(dst.tex(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        copy_to_readback(dst);
        transition(dst.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit_and_wait();
    }

    // The SYNCHRONOUS copy/resolve: own the list end to end and leave `dst` read-back so `read_pixel` is valid
    // the instant the call returns — the affordance every other synchronous verb provides.
    void sync_xfer(Dx12RasterTarget& dst, Dx12RasterTarget& src, bool resolve, BlitFilter /*filter*/)
    {
        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        ID3D12Resource* src_res = resolve ? src.tex() : src.copy_src();
        transition(src_res, D3D12_RESOURCE_STATE_COMMON,
                   resolve ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(dst.tex(), D3D12_RESOURCE_STATE_COMMON,
                   resolve ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_COPY_DEST);
        if (resolve) { xfer_resolve(m_list.Get(), dst, src); }
        else         { xfer_copy(m_list.Get(), dst, src); }
        transition(src_res, resolve ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
        transition(dst.tex(), resolve ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        copy_to_readback(dst);
        transition(dst.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit_and_wait();
    }

    // ── ⛔ REN-38-A10: `download_storage` was MISSING on DX12. ──
    // The base returns false, so `read_u32` reflected whatever the last DRAW happened to leave behind — and a
    // buffer written by a COMPUTE pass could not be read back at all. Vulkan has had it since RET-4; the
    // asymmetry survived because no DX12 test ever read a compute result back through the raster context.
    [[nodiscard]] bool download_storage(IStorageBuffer& storage) override
    {
        if (!m_ok || frame_recording()) { return false; } // inside a frame the graph owns the list
        ID3D12Resource* src = dx_buffer_of(storage);
        auto*           sb  = dynamic_cast<Dx12StorageBuffer*>(&storage);
        // Only an APPLICATION buffer has a readback resource — a graph transient lives in device-local aliased
        // memory with no host mapping, which is exactly why an authored pass writes its results to an
        // `external_buffer` when anything outside the frame needs to see them.
        if (src == nullptr || sb == nullptr || sb->readback() == nullptr) { return false; }
        m_cmd_alloc->Reset();
        m_list->Reset(m_cmd_alloc.Get(), nullptr);
        transition(src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_list->CopyResource(sb->readback(), src);
        transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit_and_wait();
        return true;
    }

    void copy_to_readback(Dx12RasterTarget& t)
    {
        D3D12_TEXTURE_COPY_LOCATION d{};
        d.pResource       = t.readback();
        d.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        d.PlacedFootprint = t.footprint();
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource        = t.tex();
        s.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
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

    // ── ⭐⭐ DX12 UPLOAD BATCHING — parity with Vulkan's 38-G1 fast path (the 40-B DX12 upload gap). ──────────────
    // Before this, every sync-phase upload took the SYNCHRONOUS `upload_storage` body: a fresh
    // CreateCommittedResource + m_cmd_alloc->Reset() + submit_and_wait PER CALL. At 1M the renderer issues dozens of
    // uploads per frame, each a full CPU<->GPU flush — measured ~36 ms/frame of upload waits (DX12 ~3x behind
    // Vulkan). Between begin/end_upload_batch, an upload is instead a ring memcpy + ONE recorded copy; end submits
    // them ALL ONCE with NO WAIT. The batch uses its OWN allocator+list (never the dedicated pair), so a synchronous
    // verb's Reset can never discard recorded copies; SAME-QUEUE submission order sequences the copies before the
    // frame's draws (end runs at the end of sync, before render() submits the frame).
    void begin_upload_batch() override
    {
        if (m_batch_open || !m_ok || m_queue == nullptr || m_fence == nullptr || frame_recording()) { return; }
        UploadBatch& b = m_upload[m_upload_slot];
        if (b.submitted) // reclaim the slot from 2 batches ago (its submission has normally long completed)
        {
            if (m_fence->GetCompletedValue() < b.fence_val)
            {
                m_fence->SetEventOnCompletion(b.fence_val, m_event);
                WaitForSingleObject(m_event, INFINITE);
            }
            b.submitted = false;
        }
        if (b.ring == nullptr)
        {
            b.ring = make_upload_buffer(m_device.Get(), kUploadRingBytes);
            if (b.ring == nullptr || FAILED(b.ring->Map(0U, nullptr, reinterpret_cast<void**>(&b.map))))
            {
                b.ring.Reset();
                b.map = nullptr;
                return; // no ring -> the batch never opens; uploads stay on the synchronous path (correct, slower)
            }
            b.cap = kUploadRingBytes;
        }
        if (b.list == nullptr)
        {
            if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&b.alloc)))
                || FAILED(m_device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, b.alloc.Get(), nullptr,
                                                      IID_PPV_ARGS(&b.list))))
            {
                b.list.Reset();
                b.alloc.Reset();
                return;
            }
        }
        else
        {
            b.alloc->Reset();
            b.list->Reset(b.alloc.Get(), nullptr);
        }
        b.used       = 0U;
        m_batch_open = true;
    }

    void end_upload_batch() override
    {
        if (!m_batch_open) { return; }
        m_batch_open   = false;
        UploadBatch& b = m_upload[m_upload_slot];
        b.list->Close();
        ID3D12CommandList* lists[] = {b.list.Get()};
        m_queue->ExecuteCommandLists(1U, lists);
        ++m_fence_val;
        m_queue->Signal(m_fence.Get(), m_fence_val); // ⛔ NO WAIT — that is the entire point
        b.fence_val   = m_fence_val;
        b.submitted   = true;
        m_upload_slot = (m_upload_slot + 1U) % kUploadBatches;
    }

    // the in-batch append: ring memcpy + one recorded copy (bracketed by the UAV<->COPY_DEST pair the synchronous
    // path already uses per upload). False falls back to the synchronous path. Grows the ring by doubling on demand.
    [[nodiscard]] bool upload_batched(Dx12StorageBuffer& s, crd::u32 byte_offset, const void* data,
                                      crd::u32 size_bytes)
    {
        UploadBatch&   b       = m_upload[m_upload_slot];
        const crd::u32 aligned = (size_bytes + 15U) & ~15U;
        // ⛔ A single upload bigger than the base ring (the one-time bulk uploads on the FIRST frame — the whole
        // 1M instance + prev_world arrays) goes to the synchronous path instead of GROWING the ring to hold it:
        // one big committed-buffer copy is fine as a one-time cost, and it keeps the persistent ring small
        // (8 MB × 2 slots) instead of ballooning to hundreds of MB for the rest of the session. Steady-state
        // uploads (the per-run dirty ranges) are all far below this, so they still batch.
        if (aligned > kUploadRingBytes) { return false; }
        if (b.used + aligned > b.cap)
        {
            // grow: submit this batch, drain THIS slot (its copies reference the current ring), rebuild it bigger,
            // reopen on the next slot, retry. `b` still names the just-submitted slot after end advances the cursor.
            const crd::u32 want = (b.cap * 2U > b.used + aligned) ? b.cap * 2U : (b.used + aligned) * 2U;
            end_upload_batch();
            if (m_fence->GetCompletedValue() < b.fence_val)
            {
                m_fence->SetEventOnCompletion(b.fence_val, m_event);
                WaitForSingleObject(m_event, INFINITE);
            }
            b.submitted = false;
            b.map       = nullptr;
            b.ring.Reset();
            b.ring = make_upload_buffer(m_device.Get(), want);
            if (b.ring == nullptr || FAILED(b.ring->Map(0U, nullptr, reinterpret_cast<void**>(&b.map))))
            {
                b.ring.Reset();
                b.map = nullptr;
                b.cap = 0U;
                return false; // caller falls back to the synchronous path
            }
            b.cap = want;
            begin_upload_batch();
            if (!m_batch_open) { return false; }
            return upload_batched(s, byte_offset, data, size_bytes);
        }
        std::memcpy(b.map + b.used, data, size_bytes);
        const auto bar = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
            D3D12_RESOURCE_BARRIER rb{};
            rb.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            rb.Transition.pResource   = s.buf();
            rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            rb.Transition.StateBefore = from;
            rb.Transition.StateAfter  = to;
            b.list->ResourceBarrier(1U, &rb);
        };
        bar(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        b.list->CopyBufferRegion(s.buf(), byte_offset, b.ring.Get(), b.used, size_bytes);
        bar(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        b.used += aligned;
        return true;
    }

    // Flush + drain every batch (waits their submissions). Called at teardown and before a frame starts recording.
    void drain_upload_batches()
    {
        if (m_batch_open) { end_upload_batch(); }
        if (m_fence == nullptr) { return; }
        for (UploadBatch& b : m_upload)
        {
            if (!b.submitted) { continue; }
            if (m_fence->GetCompletedValue() < b.fence_val)
            {
                m_fence->SetEventOnCompletion(b.fence_val, m_event);
                WaitForSingleObject(m_event, INFINITE);
            }
            b.submitted = false;
        }
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
    // ── ⛔⛔ REN-38-A8: THE ACTIVE LIST AND ITS INTERFACE VIEWS MOVE TOGETHER. ──
    // `m_list5` (VRS) and `m_list6` (mesh) are QueryInterface VIEWS of `m_list`. `frame_rec_begin` SWAPS `m_list`
    // to a ring list for the duration of a frame — and before this, the two views were derived ONCE at
    // construction and never moved. So inside a frame graph:
    //   · the clear, viewport, root signature and PSO went to the RING list (they use `m_list`), and
    //   · `m_list6->DispatchMesh(...)` and `m_list5->RSSetShadingRate(...)` went to the context's DEDICATED list
    //     — which is CLOSED and never submitted.
    // The result: every recorded mesh-shader draw cleared its target and dispatched NOTHING, and every recorded
    // VRS rate was silently dropped. No D3D12 error, no validation message, a plausible black frame.
    //
    // ⛔ It survived because the mesh/VRS RECORDING paths (38-A1c) had never been RUN by a gate on DX12 — they
    // were written, they compiled, and the row was closed on the strength of the synchronous path still passing.
    // The 38-A8 gate is the first test to dispatch a mesh shader inside a frame graph on this backend.
    //
    // Assigning through this one function is the fix: the views cannot go stale because there is no other way to
    // change the active list.
    void set_active_list(const ComPtr<ID3D12GraphicsCommandList>& list) noexcept
    {
        m_list = list;
        m_list5.Reset();
        m_list6.Reset();
        m_list.As(&m_list5);
        m_list.As(&m_list6);
    }

    void frame_rec_begin(ID3D12DescriptorHeap* heap, UINT inc)
    {
        if (m_batch_open) { end_upload_batch(); } // never carry an open upload batch across a frame boundary
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
            set_active_list(m_ring_list[m_ring_slot]);
        }
        // REN-39-D1: this slot's fence has retired above, so last cycle's in-frame staging buffers are dead.
        m_frame_uploads[m_ring_slot].clear();
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
            set_active_list(m_saved_list);
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
    // ── ⭐ REN-38-A6: THE TRANSFER VERBS — the DX12 half. ──
    // ⛔ THE ASYMMETRY IS REAL AND IS NOT HIDDEN. D3D12's copy engine offers CopyResource / CopyTextureRegion
    // (1:1 only) and ResolveSubresource (MSAA only). There is NO blit: nothing in D3D12 rescales an image on the
    // copy engine. So `copy_image` and `resolve_image` are native here, and `blit_image` goes through the 3D
    // pipeline — a fullscreen textured draw with a sampler, which is what the API actually gives you.
    // That is exactly why `blit_image` is its OWN verb rather than `copy_image` with an extent: collapsing them
    // would make one backend silently pay for a rasterizer on every copy, or the other silently crop.
    void xfer_copy(ID3D12GraphicsCommandList* list, Dx12RasterTarget& dst, Dx12RasterTarget& src)
    {
        list->CopyResource(dst.tex(), src.copy_src());
    }
    void xfer_resolve(ID3D12GraphicsCommandList* list, Dx12RasterTarget& dst, Dx12RasterTarget& src)
    {
        // ⛔ The MULTISAMPLE texture, not `copy_src()`: `copy_src()` already returns the RESOLVE texture for a
        // multisampled target, so resolving that would be a no-op that reads back as if it had worked.
        list->ResolveSubresource(dst.tex(), 0, src.tex(), 0, kColorFormat);
    }

    // REN-38-A6: the readback for a target the frame left in some OTHER state — a transfer pass's destination
    // ends in COPY_DEST, not RENDER_TARGET, and a transition that named the wrong `before` state is a debug-layer
    // break (or silent corruption with the layer off).
    void frame_readback_from(Dx12RasterTarget& t, D3D12_RESOURCE_STATES from)
    {
        if (from != D3D12_RESOURCE_STATE_COPY_SOURCE)
        {
            transition(t.tex(), from, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        copy_to_readback(t);
        transition(t.tex(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    // REN-38-B8: a pass boundary RESETS the active sampler, so no pass inherits its neighbour's. DX12
    // had NO per-pass hook at all (Vulkan's `frame_rec_new_pass` clears the self-barrier key); it needed one.
    void frame_rec_new_pass() noexcept
    {
        m_active_sampler_slot = 0xFFFFFFFFU;
        m_pass_state          = PassRasterState{}; // REN-38 audit: no pass inherits its neighbour's raster state
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

    // ⭐⭐ REN-39-C1: the READ-ONLY twin — the next frame-heap slot pointed at `s`'s raw SRV (StructuredBuffer
    // of uint words), for root table 7 (t0). An indexed program pair reads storage through THIS while the
    // resource sits in kIndexedDrawStates, where the u0 UAV access would be illegal.
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE frame_alloc_storage_srv_slot(Dx12StorageBuffer& s)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements = s.num_elements();
        srv.Buffer.StructureByteStride = 4;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_frame_rec.heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(m_frame_rec.cursor) * m_frame_rec.inc;
        m_device->CreateShaderResourceView(s.buf(), &srv, cpu);
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
        // ⭐ REN-38-A6: DEPTH IS OPTIONAL — the DX12 mirror of the Vulkan rule. A frame-graph COLOUR transient
        // has no depth texture (depth is a separate `D32Float` resource here), so a `raster.geometry` pass
        // drawing into one has `has_depth() == false`. ⛔ Before this the storage-depth verbs RETURNED in that
        // case: an authored geometry pass writing a colour transient ran, reported no error, and drew NOTHING.
        // Found by the A6 blit gate on BOTH backends, independently — the same defect, written twice.
        const bool                        depth_on = t.has_depth();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv      = depth_on ? t.dsv() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        m_list->OMSetRenderTargets(1, &rtv, FALSE, depth_on ? &dsv : nullptr);
        if (clear)
        {
            const float rgba[4] = {clear_color.r, clear_color.g, clear_color.b, clear_color.a};
            m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
            if (depth_on) { m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr); }
        }
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, table);
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);
    }

    // ⭐⭐ REN-39-D1: the frame-mode body of draw_overlay — LOAD (no clears) + alpha blend + read-only depth,
    // recorded into the graph's shared list so the overlay is a PASS of the frame like every other draw.
    void record_overlay(Dx12RasterTarget& t, Dx12RasterProgram& p, Dx12StorageBuffer& s, ID3D12PipelineState* pso,
                        bool depth_on, crd::u32 first_vertex, crd::u32 vertex_count)
    {
        const D3D12_GPU_DESCRIPTOR_HANDLE table = frame_alloc_storage_slot(s);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv   = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv   = depth_on ? t.dsv() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        m_list->OMSetRenderTargets(1, &rtv, FALSE, depth_on ? &dsv : nullptr);
        // ⛔ NO clears, ever — the overlay composites over what the graph already drew into this target.
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, table);
        m_list->SetPipelineState(pso);
        apply_stencil_ref();
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, first_vertex, 0);
    }

    // ⭐⭐ REN-39-A1: the frame-mode INDEXED scene draw — record_scene with the scene buffer ALSO bound through
    // an IBV at the section offset, bracketed by the UAV ↔ kIndexedDrawStates state pair (the
    // dispatch_kernel_indirect precedent: locally balanced, so the graph's parked-in-UAV assumption stays true).
    void record_scene_indexed(Dx12RasterTarget& t, Dx12RasterProgram& p, Dx12StorageBuffer& s, ID3D12PipelineState* pso,
                              bool clear, ClearColor clear_color, float clear_depth, crd::u32 index_offset_bytes,
                              crd::u32 index_count, crd::u32 instance_count)
    {
        frame_transition(s.buf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kIndexedDrawStates);
        const D3D12_GPU_DESCRIPTOR_HANDLE table = frame_alloc_storage_slot(s);
        const D3D12_GPU_DESCRIPTOR_HANDLE ro = frame_alloc_storage_srv_slot(s); // t0 (REN-39-C1)
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv();
        const bool depth_on = t.has_depth(); // ⭐ REN-38-A6: depth is OPTIONAL
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth_on ? t.dsv() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        m_list->OMSetRenderTargets(1, &rtv, FALSE, depth_on ? &dsv : nullptr);
        if (clear)
        {
            const float rgba[4] = {clear_color.r, clear_color.g, clear_color.b, clear_color.a};
            m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
            if (depth_on)
            {
                m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr);
            }
        }
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, table);
        m_list->SetGraphicsRootDescriptorTable(7, ro); // t0: the read-only storage view (REN-39-C1)
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = s.buf()->GetGPUVirtualAddress() + index_offset_bytes;
        ibv.SizeInBytes = index_count * 4U; // exactly the section the draw reads — an overrun is never legal
        ibv.Format = DXGI_FORMAT_R32_UINT;
        m_list->IASetIndexBuffer(&ibv);
        m_list->DrawIndexedInstanced(index_count, instance_count, 0, 0, 0);
        frame_transition(s.buf(), kIndexedDrawStates, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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
        if (clear) { m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr); }
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, table);
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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

    // ── REN-38-A1a: the FRAME-MODE bindless array + draw (the DX12 half; the Vulkan half is `record_bindless`). ──
    // ⛔⛔ WHY `draw_bindless` COULD NOT BE RECORDED. Its synchronous body mints the SRV array into the GLOBAL
    // `m_uav_heap` at fixed slots 2..2+kBindlessMax-1 and then RESETS the command allocator + list. Both are
    // catastrophic inside a frame: the fixed slots stomp descriptors earlier passes are still using, and the
    // reset throws away everything already recorded. Same shape as Vulkan's `vkResetDescriptorPool` — the verb
    // OWNS the heap, so it cannot be a guest in someone else's frame.
    //
    // The frame-mode body reserves a CONTIGUOUS RUN from the frame's descriptor ring instead. ⛔ Contiguous is
    // required, not convenient: a root descriptor TABLE addresses `kBindlessMax` consecutive slots from one GPU
    // handle, so allocating them one at a time through `frame_alloc_srv_slot` would interleave with other passes
    // and the array would read whatever landed between them.
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE frame_alloc_bindless_run(ITexture* const* textures, crd::u32 n)
    {
        const UINT base = m_frame_rec.cursor;
        for (UINT i = 0; i < static_cast<UINT>(kBindlessMax); ++i)
        {
            auto&                                 tex = static_cast<Dx12Texture&>(*textures[i < n ? i : 0U]);
            const D3D12_SHADER_RESOURCE_VIEW_DESC srv = tex.srv();
            D3D12_CPU_DESCRIPTOR_HANDLE           cpu = m_frame_rec.heap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(base + i) * m_frame_rec.inc;
            m_device->CreateShaderResourceView(tex.tex(), &srv, cpu);
        }
        m_frame_rec.cursor += static_cast<UINT>(kBindlessMax);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_frame_rec.heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(base) * m_frame_rec.inc;
        return gpu;
    }

    void record_bindless(Dx12RasterTarget& t, Dx12RasterProgram& p, ID3D12PipelineState* pso,
                         ITexture* const* textures, crd::u32 n, ClearColor clear_color, crd::u32 vertex_count,
                         bool clear = true)
    {
        const D3D12_GPU_DESCRIPTOR_HANDLE bindless_gpu = frame_alloc_bindless_run(textures, n);
        D3D12_GPU_DESCRIPTOR_HANDLE       samp_gpu     = m_sampler_heap->GetGPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv          = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        // ⛔ REN-38-A12: the WBOIT composite LOADS — it resolves `rgb·(1-reveal) + background·reveal`, so the
        // background must still be there when it runs. On D3D12 "load" is simply not issuing the clear.
        if (clear)
        {
            const float rgba[4] = {clear_color.r, clear_color.g, clear_color.b, clear_color.a};
            m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        }
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(2, samp_gpu);     // sampler s2
        m_list->SetGraphicsRootDescriptorTable(3, bindless_gpu); // bindless t3[]
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);
    }

    // ⭐⭐ REN-41 (TAA): the DX12 twin of Vulkan's record_bindless_storage — record_bindless PLUS the CONSTANTS
    // buffer at u0 (root param 0). The two allocators share the frame heap cursor, so storage takes one slot and
    // the bindless run the next kBindlessMax — non-colliding by construction; each returns its own root handle.
    void record_bindless_storage(Dx12RasterTarget& t, Dx12RasterProgram& p, ID3D12PipelineState* pso,
                                 ITexture* const* textures, crd::u32 n, Dx12StorageBuffer& s,
                                 ClearColor clear_color, crd::u32 vertex_count)
    {
        const D3D12_GPU_DESCRIPTOR_HANDLE storage_gpu  = frame_alloc_storage_slot(s);          // u0, param 0
        const D3D12_GPU_DESCRIPTOR_HANDLE bindless_gpu = frame_alloc_bindless_run(textures, n); // t3[], param 3
        const D3D12_GPU_DESCRIPTOR_HANDLE samp_gpu     = m_sampler_heap->GetGPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv          = t.rtv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float rgba[4] = {clear_color.r, clear_color.g, clear_color.b, clear_color.a};
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, storage_gpu);  // u0 constants (the reproject matrix)
        m_list->SetGraphicsRootDescriptorTable(2, samp_gpu);     // sampler s2
        m_list->SetGraphicsRootDescriptorTable(3, bindless_gpu); // bindless t3[]
        m_list->SetPipelineState(pso);
        apply_stencil_ref();
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);
    }
    void draw_bindless_storage(IRasterTarget& target, IRasterProgram& program, ClearColor clear,
                               ITexture* const* textures, crd::u32 count, IStorageBuffer& constants,
                               crd::u32 vertex_count)
    {
        if (!m_ok || m_uav_heap == nullptr || m_sampler_heap == nullptr || count == 0U || textures == nullptr
            || !frame_recording())
        {
            return;
        }
        auto&                t   = static_cast<Dx12RasterTarget&>(target);
        auto&                p   = static_cast<Dx12RasterProgram&>(program);
        auto&                s   = static_cast<Dx12StorageBuffer&>(constants);
        ID3D12PipelineState* pso = pass_pso(p, 1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false, 1U,
                                            t.color_format());
        if (!p.valid() || pso == nullptr) { return; }
        const crd::u32 n = count < static_cast<crd::u32>(kBindlessMax) ? count : static_cast<crd::u32>(kBindlessMax);
        record_bindless_storage(t, p, pso, textures, n, s, clear, vertex_count);
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
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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
        samp_gpu.ptr += static_cast<UINT64>(active_sampler_slot(sampler_slot)) * m_sampler_inc; // REN-38-B8
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
        // REN-40-D: the plain depth sampler rides along UNCONDITIONALLY (s6) — a fullscreen pass reading a
        // depth resource gets the comparison sampler at s2, but a moment-CONVERT pass needs the STORED depth,
        // which only a plain sampler can return. The slot is always valid, so binding it costs nothing.
        m_list->SetGraphicsRootDescriptorTable(8, plain_depth_tbl());
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(vertex_count, 1, 0, 0);
    }

    // REN-2 Half B: the frame-mode body of draw_storage_textured_depth (clear) / _load (no clear) — a depth-tested
    // scene draw that ALSO samples a material texture: storage UAV (table 0) from the ring + SRV (table 1) from the
    // ring + sampler (table 2). Into the shared list; the graph transitioned the target to RENDER_TARGET.
    // REN-3.2-b: sampler_slot 0 = the default FILTERING sampler (textured scene draw), 1 = the COMPARISON
    // sampler (shadowed scene draw). Same tables either way, so one recording path serves both.
    // REN-38: `atlas` non-null ⇒ the combined textured+shadowed draw — atlas SRV at t4, comparison at s5.
    void record_scene_textured(Dx12RasterTarget& t, Dx12RasterProgram& p, Dx12StorageBuffer& s, Dx12Texture& tex,
                               ID3D12PipelineState* pso, bool clear, ClearColor clear_color, float clear_depth,
                               crd::u32 vertex_count, crd::u32 sampler_slot = 0U, Dx12Texture* atlas = nullptr)
    {
        const D3D12_GPU_DESCRIPTOR_HANDLE uav_table = frame_alloc_storage_slot(s);
        const D3D12_GPU_DESCRIPTOR_HANDLE srv_table = frame_alloc_srv_slot(tex);
        D3D12_GPU_DESCRIPTOR_HANDLE samp_gpu = m_sampler_heap->GetGPUDescriptorHandleForHeapStart();
        samp_gpu.ptr += static_cast<UINT64>(active_sampler_slot(sampler_slot)) * m_sampler_inc; // REN-38-B8
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv       = t.rtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv       = t.dsv();
        m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        if (clear)
        {
            const float rgba[4] = {clear_color.r, clear_color.g, clear_color.b, clear_color.a};
            m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
            m_list->ClearDepthStencilView(dsv, t.clear_flags(), clear_depth, 0, 0, nullptr);
        }
        const D3D12_VIEWPORT vp{0.0F, 0.0F, static_cast<float>(t.width()), static_cast<float>(t.height()), 0.0F, 1.0F};
        const D3D12_RECT     sc{0, 0, static_cast<LONG>(t.width()), static_cast<LONG>(t.height())};
        m_list->RSSetViewports(1, &vp);
        m_list->RSSetScissorRects(1, &sc);
        m_list->SetGraphicsRootSignature(p.root());
        m_list->SetGraphicsRootDescriptorTable(0, uav_table); // storage (u0)
        m_list->SetGraphicsRootDescriptorTable(1, srv_table); // base-color SRV (t1)
        m_list->SetGraphicsRootDescriptorTable(2, samp_gpu);  // sampler (s2)
        if (atlas != nullptr)
        {
            const D3D12_GPU_DESCRIPTOR_HANDLE atlas_table = frame_alloc_srv_slot(*atlas);
            m_list->SetGraphicsRootDescriptorTable(4, atlas_table); // shadow atlas (t4)
            // REN-40-D: s5 keyed by WHAT the atlas is — comparison for depth, LINEAR/CLAMP for moments.
            m_list->SetGraphicsRootDescriptorTable(5, atlas_samp_tbl(*atlas));
            m_list->SetGraphicsRootDescriptorTable(8, plain_depth_tbl()); // plain depth sampler (s6, REN-40-D)
        }
        m_list->SetPipelineState(pso);
        apply_stencil_ref(); // REN-38 audit: the stencil REFERENCE is command-list state, not PSO state
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
    // REN-39-D1: staging buffers for uploads recorded INSIDE a frame (see upload_storage) — retained per ring
    // slot and released when that slot's fence retires, because the GPU reads them after this call returns.
    // ⛔ PARENTHESES, not braces: `Array<T>{alloc_ptr}` selects the initializer_list overload and tries to build
    // a `ComPtr<ID3D12Resource>` FROM the allocator pointer (a raw-pointer ComPtr ctor) — a wall of WRL errors
    // pointing at client.h rather than at this line.
    crd::containers::Array<ComPtr<ID3D12Resource>> m_frame_uploads[kFrameRingSize]{
        crd::containers::Array<ComPtr<ID3D12Resource>>(crd::memory::default_allocator()),
        crd::containers::Array<ComPtr<ID3D12Resource>>(crd::memory::default_allocator())};
    // ── ⭐⭐ DX12 UPLOAD BATCHING (parity with Vulkan's 38-G1 fast path — the 40-B DX12 upload gap). ──────────────
    // Each slot owns a PERSISTENT mapped upload ring + its OWN allocator+list, so between begin/end_upload_batch a
    // storage upload is a ring memcpy + one recorded CopyBufferRegion — no per-call CreateCommittedResource, no
    // submit-and-wait. Double-buffered so `begin` never stalls on the batch submitted last frame. See the methods.
    struct UploadBatch
    {
        ComPtr<ID3D12Resource>            ring;
        crd::u8*                          map       = nullptr;
        ComPtr<ID3D12CommandAllocator>    alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        crd::u64                          fence_val = 0U;
        crd::u32                          cap       = 0U;
        crd::u32                          used      = 0U;
        bool                              submitted = false;
    };
    static constexpr crd::u32          kUploadBatches   = 2U;
    static constexpr crd::u32          kUploadRingBytes = 8U << 20U; // initial; grows by doubling on demand
    UploadBatch                        m_upload[kUploadBatches];
    crd::u32                           m_upload_slot = 0U;
    bool                               m_batch_open  = false;
    ComPtr<ID3D12GraphicsCommandList>  m_list;
    ComPtr<ID3D12GraphicsCommandList5> m_list5; // B1-e: RSSetShadingRate(Image) — null on an old runtime
    ComPtr<ID3D12GraphicsCommandList6> m_list6; // B4: DispatchMesh — null on an old runtime
    ComPtr<ID3D12CommandSignature>     m_mesh_indirect_sig; // B4: DISPATCH_MESH command signature for ExecuteIndirect (lazy)
    // ── REN-38 MULTI-DRAW: the {ROOT_CONSTANT(b7), DRAW} command signature (lazy, per root signature — a
    // signature containing a root-constant argument must be created AGAINST that root signature) + the args
    // upload ring. Stride = 4 (the DrawIndex constant) + 16 (D3D12_DRAW_ARGUMENTS) = 20 bytes per command.
    ComPtr<ID3D12CommandSignature>     m_multi_sig;
    ID3D12RootSignature*               m_multi_sig_root = nullptr; // which root the cached signature was built for
    static constexpr crd::u32          kMultiMax    = 256U;
    static constexpr crd::u32          kMultiChunks = 32U;
    static constexpr crd::u32          kMultiStride = 20U;
    ComPtr<ID3D12Resource>             m_multi_args;
    crd::u8*                           m_multi_map    = nullptr;
    crd::u32                           m_multi_cursor = 0U;
    crd::u64                           m_multi_batches = 0U;
    crd::u64                           m_multi_indexed_batches = 0U; // REN-39-C1: the index-buffer subset of m_multi_batches
    // ── ⭐⭐ REN-39-A2 INDEXED MULTI-DRAW: the {ROOT_CONSTANT(b7), DRAW_INDEXED} signature + its own args
    // ring. Stride = 4 (DrawIndex constant) + 20 (D3D12_DRAW_INDEXED_ARGUMENTS) = 24 bytes per command. A
    // second ring rather than a shared one so neither command type's chunk arithmetic depends on the other's.
    ComPtr<ID3D12CommandSignature> m_multi_idx_sig;
    ID3D12RootSignature* m_multi_idx_sig_root = nullptr;
    static constexpr crd::u32 kMultiIdxStride = 24U;
    ComPtr<ID3D12Resource> m_multi_idx_args;
    crd::u8* m_multi_idx_map = nullptr;
    crd::u32 m_multi_idx_cursor = 0U;
    bool                               m_mesh_shader = false; // B4: D3D12_FEATURE_D3D12_OPTIONS7 MeshShaderTier supported
    ComPtr<ID3D12Fence>                m_fence;
    HANDLE                             m_event     = nullptr;
    crd::u64                           m_fence_val = 0;
    D3D12_VARIABLE_SHADING_RATE_TIER            m_vrs_tier      = D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED; // B1-e
    UINT                               m_vrs_tile_size = 0;                                     // B1-e: square tile edge
    D3D12_CONSERVATIVE_RASTERIZATION_TIER m_conservative_tier = D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED; // B1-f
    bool                               m_rov           = false; // B1-f: rasterizer-ordered views (fragment interlock analog)
    D3D12_RESOURCE_BINDING_TIER        m_binding_tier  = D3D12_RESOURCE_BINDING_TIER_1; // B2-d: Tier 2+ ⇒ bindless
    // REN-38-A2: the kernel root signature + a DXIL-keyed compute PSO cache (a kernel dispatches every frame,
    // so building its PSO per dispatch is not acceptable).
    static constexpr crd::u32          kMaxKernelBuffers = 8U;
    static constexpr crd::u32          kKernelPsoCap     = 16U;
    ComPtr<ID3D12RootSignature>        m_kernel_root;
    const void*                        m_kernel_key[kKernelPsoCap]{};
    ComPtr<ID3D12PipelineState>        m_kernel_pso[kKernelPsoCap];
    crd::u32                           m_kernel_n = 0U;
    BlitKit                            m_blit;                  // REN-38-A6: the internal fullscreen blit (D3D12 has no copy-engine blit)
    ComPtr<ID3D12RootSignature>        m_rt_root;               // REN-38-A9: TLAS root SRV (t0) + UAV table (u1..uN)
    const void*                        m_rt_pso_key[kKernelPsoCap]{};
    ComPtr<ID3D12PipelineState>        m_rt_pso[kKernelPsoCap];
    crd::u32                           m_rt_pso_n = 0U;
    // REN-40-G3: sampled-compute root signature (UAV table u0..u7 + SRV t8 + static sampler s9) and PSO cache.
    ComPtr<ID3D12RootSignature>        m_sampled_kernel_root;
    const void*                        m_sampled_pso_key[kKernelPsoCap]{};
    ComPtr<ID3D12PipelineState>        m_sampled_pso[kKernelPsoCap];
    crd::u32                           m_sampled_pso_n = 0U;
    ComPtr<ID3D12CommandSignature>     m_dispatch_sig;          // REN-38-A10: ExecuteIndirect(DISPATCH)
    ComPtr<ID3D12DescriptorHeap>       m_clear_heap;            // REN-38-B3: the NON-shader-visible UAV ClearUint needs
    // REN-38-B8: the authored-sampler cache — heap slots 2..N.
    static constexpr crd::u32          kSamplerCacheCap = 16U;
    SamplerDesc                        m_sampler_key[kSamplerCacheCap]{};
    crd::u32                           m_sampler_n           = 0U;
    crd::u32                           m_active_sampler_slot = 0xFFFFFFFFU;
    PassRasterState                    m_pass_state{}; // REN-38 audit: the declared pass state (defaults = historical)
    ComPtr<ID3D12Device5>              m_dxr_device;            // REN-38-A16: CreateStateObject lives on Device5
    bool                               m_dxr_tried = false;
    DxrPipe                            m_dxr[kKernelPsoCap];
    crd::u32                           m_dxr_n = 0U;
    ComPtr<ID3D12DescriptorHeap>       m_uav_heap;              // B1-f/B2: shader-visible heap — slot0 UAV (storage) · slot1 SRV (texture)
    ComPtr<ID3D12DescriptorHeap>       m_sampler_heap;          // B2: shader-visible sampler heap — slot0 default · slot1 comparison
    UINT                               m_srv_inc = 0;           // B2: CBV/SRV/UAV descriptor increment size
    UINT                               m_sampler_inc = 0;       // B2-b: sampler descriptor increment size
    bool                               m_ok            = false;
    bool                               m_next_load_depth = false; // REN-40-G1: consumed by the next geometry verb

    void set_next_draw_load_depth(bool load) override { m_next_load_depth = load; }
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
    // ⛔ REN-38-A10: it carries the REAL placed resource. Before this it was a size-only stub, so a pass handed a
    // transient could not bind it, dispatch from it, or copy it — only ask how big it was.
    Dx12TransientBuffer(crd::u32 size, ID3D12Resource* res) noexcept : m_size(size), m_res(res) {}
    [[nodiscard]] crd::u32 size_bytes() const noexcept override { return m_size; }
    [[nodiscard]] crd::u32 read_u32(crd::u32) const noexcept override { return 0U; } // graph memory is not host-visible
    [[nodiscard]] ID3D12Resource* buf() const noexcept { return m_res; }
    [[nodiscard]] crd::u32        num_elements() const noexcept { return m_size / 4U; }
private:
    crd::u32        m_size = 0;
    ID3D12Resource* m_res  = nullptr;
};

ID3D12Resource* dx_buffer_of(IStorageBuffer& b) noexcept
{
    if (auto* sb = dynamic_cast<Dx12StorageBuffer*>(&b)) { return sb->buf(); }
    if (auto* tb = dynamic_cast<Dx12TransientBuffer*>(&b)) { return tb->buf(); }
    return nullptr;
}
crd::u32 dx_buffer_elems(IStorageBuffer& b) noexcept
{
    if (auto* sb = dynamic_cast<Dx12StorageBuffer*>(&b)) { return sb->num_elements(); }
    if (auto* tb = dynamic_cast<Dx12TransientBuffer*>(&b)) { return tb->num_elements(); }
    return 0U;
}

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

    // ── ⭐ REN-40-G3: a render target whose COLOUR comes from one image and DEPTH from another. ──
    [[nodiscard]] IRasterTarget* image_with_depth(FgImage colour, FgImage depth) noexcept override
    {
        if (!colour.valid() || colour.id > m_images.size()) { return nullptr; }
        if (!depth.valid()  || depth.id  > m_images.size()) { return nullptr; }
        ImageNode& cn = m_images[colour.id - 1U];
        ImageNode& dn = m_images[depth.id  - 1U];
        if (cn.shared_depth_target != nullptr) { return cn.shared_depth_target; }
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT tfp{};
        auto* t = new (std::nothrow) Dx12RasterTarget(cn.resource, nullptr, dn.resource, nullptr, cn.rtv_heap,
                                                      dn.dsv_heap ? dn.dsv_heap : dn.depth_dsv_heap, nullptr, tfp, 1U,
                                                      cn.desc.width, cn.desc.height);
        if (t == nullptr) { return nullptr; } // OOM: the caller's needs_target guard skips the pass
        cn.shared_depth_target = t;
        return t;
    }

    // ── IFrameGraph ──
    // ⭐ REN-38-B5: import an APP-OWNED texture — the DX12 mirror. Read-only: no target, so the state tracker
    // never touches it and the app keeps it in PIXEL_SHADER_RESOURCE on its own schedule.
    [[nodiscard]] FgImage import_texture(ITexture& texture) override
    {
        for (crd::usize i = 0; i < m_images.size(); ++i)
        {
            if (m_images[i].texture == &texture && m_images[i].target == nullptr)
            {
                return FgImage{static_cast<crd::u32>(i + 1U)};
            }
        }
        ImageNode n{};
        n.texture = &texture;
        n.own     = Own::Imported;
        m_images.push_back(n);
        return FgImage{static_cast<crd::u32>(m_images.size())};
    }

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
        // ── ⭐ REN-38-B2: the SHAPE. ──
        // ⛔ D3D12 has NO cube RESOURCE and no cube flag: a cube IS a 6-layer Texture2D array, and "cubeness" lives
        // ENTIRELY in the SRV (`D3D12_SRV_DIMENSION_TEXTURECUBE`). That is the opposite of Vulkan, where the image
        // needs CUBE_COMPATIBLE at creation — so the same authored `dimension = "cube"` lands in different places
        // on the two backends, and getting it wrong on either gives an array a cube shader cannot bind.
        // ⛔ `DepthOrArraySize` is exactly one field for two meanings: SLICES for a 3-D texture, LAYERS for a 2-D
        // array. Writing `layers` into it for a volume would create a 1-slice volume with N array elements, which
        // D3D12 rejects — and writing `depth` into it for an array is silently a different resource.
        const bool is_cube = desc.kind == FgImageKind::Cube || desc.kind == FgImageKind::CubeArray;
        const bool is_3d   = desc.kind == FgImageKind::Tex3D;
        n.rdesc.Dimension        = is_3d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        n.rdesc.Width            = desc.width;
        n.rdesc.Height           = desc.height;
        const crd::u32 volume_depth = desc.depth > 0U ? desc.depth : 1U;
        const crd::u32 cube_count   = desc.layers > 0U ? desc.layers : 1U;
        crd::u32       depth_or_layers = desc.layers;
        if (is_3d)        { depth_or_layers = volume_depth; }
        else if (is_cube) { depth_or_layers = 6U * cube_count; }
        n.rdesc.DepthOrArraySize = static_cast<UINT16>(depth_or_layers);
        n.rdesc.MipLevels        = static_cast<UINT16>(desc.mips > 0U ? desc.mips : 1U);
        n.no_alias               = desc.no_alias; // REN-38-B6
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
    [[nodiscard]] crd::u32 last_present_count() const noexcept override { return m_present_count; }
    void set_memory_budget(crd::u64 bytes) noexcept override { m_budget = bytes; }
    [[nodiscard]] bool last_build_exceeded_budget() const noexcept override { return m_over_budget; }
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
        // ⭐⭐ REN-39-D2: the COMPANION DEPTH of a colour transient that declared `depth_buffer = true` — the
        // DX12 half of 38-G1, which existed only on Vulkan. ⛔ Without it `has_depth()` was false for `scene_hdr`,
        // so the forward pass bound NO DSV, depth testing never happened, and whichever triangle rasterized last
        // won: the interior of every mesh visible, shading black wherever a back face survived. A COMMITTED
        // resource (its own memory, like the Vulkan twin) — it must NOT join the aliasing pool, whose lifetimes
        // are computed for the colour nodes only.
        ComPtr<ID3D12Resource>       depth_resource;
        ComPtr<ID3D12DescriptorHeap> depth_dsv_heap;
        D3D12_RESOURCE_STATES        depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        crd::i32               first_pass = -1;
        crd::i32               last_pass  = -1;
        crd::i32               slot       = -1;
        bool                   has_write  = false;
        D3D12_RESOURCE_STATES  state      = D3D12_RESOURCE_STATE_COMMON; // imported target's live state during execute
        // REN-37.5: >= 0 => this node BORROWS a persistent entry's device objects AND its LIVE STATE. The
        // resource state of a persistent image lives in the REGISTRY, never here — see `live_state()`. That is
        // what makes the frame-start reset harmless for it BY CONSTRUCTION rather than by remembering a skip.
        crd::i32               persist_index = -1;
        bool     no_alias = false; // REN-38-B6: the author PINNED this transient out of the aliaser
        IRasterTarget* shared_depth_target = nullptr; // REN-40-G3: combined colour+external-depth target
    };

    // The two questions that DO need a predicate, each answerable on its own.
    [[nodiscard]] static bool graph_owned(const ImageNode& n) noexcept { return n.own != Own::Imported; }
    // ⛔⛔ REN-38-B6: `no_alias` forbids SLOT REUSE, NOT allocation. My first attempt excluded pinned transients
    // from `aliasable()` — and the aliasing pass is what BINDS MEMORY, so a pinned image was created, never
    // backed, and reported zero bytes. A pin must make a resource MORE isolated, never less real.
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
        IFramePassBuilder& reads_depth(FgImage h) override { add_img(h, FgAccess::DepthRead); return *this; }
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
        // ── REN-38-B7: the rest of the vocabulary. ──
        case FgImageFormat::RG16F:       return DXGI_FORMAT_R16G16_FLOAT;
        case FgImageFormat::RG32F:       return DXGI_FORMAT_R32G32_FLOAT;
        case FgImageFormat::RGBA32F:     return DXGI_FORMAT_R32G32B32A32_FLOAT;
        // ⛔ DXGI orders this format's channels the OTHER WAY from Vulkan's name: `R11G11B10_FLOAT` and
        // `B10G11R11_UFLOAT_PACK32` are the SAME bit layout — Vulkan names packed formats least-significant-first.
        // Reading the two names as different formats is how a "portable" HDR buffer ends up channel-swapped.
        case FgImageFormat::R11G11B10F:  return DXGI_FORMAT_R11G11B10_FLOAT;
        case FgImageFormat::RGB10A2:     return DXGI_FORMAT_R10G10B10A2_UNORM;
        case FgImageFormat::R8:          return DXGI_FORMAT_R8_UNORM;
        case FgImageFormat::RG8:         return DXGI_FORMAT_R8G8_UNORM;
        case FgImageFormat::RGBA16Unorm: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case FgImageFormat::D24S8:      is_depth = true; return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case FgImageFormat::D32FloatS8: is_depth = true; return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
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
                delete n.shared_depth_target;
                n.shared_depth_target = nullptr;
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
        // REN-39-D2: the companion depth dies with the node it belongs to (the Vulkan twin's rule).
        n.depth_dsv_heap.Reset();
        n.depth_resource.Reset();
        n.depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        n.state = D3D12_RESOURCE_STATE_COMMON;
    }
    crd::containers::Array<Pass>       m_passes{crd::memory::default_allocator()};
    crd::containers::Array<Slot>       m_slots{crd::memory::default_allocator()};
    Builder m_builder{};

    crd::u32 m_barrier_count  = 0U;
    crd::u32 m_submit_count   = 0U;
    crd::u32 m_present_count  = 0U; // REN-38-A5: present passes that ACTUALLY presented last execute()
    crd::u64 m_budget         = 0U;  // REN-38-B6: 0 = unbounded
    bool     m_over_budget    = false;
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
    m_over_budget   = false; // REN-38-B6: a fresh verdict every build

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
        // ── ⭐⭐ WAR vs forward-reference RAW — the lifetime-aware rule (mirrors frame-cook's DependencyCycle fix;
        // the plain declaration-order heuristic that shipped here MASKED genuine cycles). A read matching a writer
        // declared AFTER the reader is a legitimate WAR (reader-before-writer, edge reader→writer) ONLY when the
        // resource already HAS A VALUE at that point: a PERSISTENT image (TAA history — read old, written new by a
        // later store), an EXTERNAL buffer (host-provided), OR some pass WRITES it BEFORE the reader (the two-phase
        // occlusion re-cull rewrites `instances`/`cull_args`). Otherwise the "later writer" is the ONLY producer,
        // so it must PRECEDE the reader (a forward RAW) — and when two passes each read what the other writes, that
        // pair is the real cycle build() must reject (the REN-1 cycle gate) and the producer-declared-last case
        // still runs producer-first (the REN-1 dependency-order gate).
        const auto link = [&](auto accessor, auto has_value) {
            const auto written_before = [&](crd::u32 handle, crd::u32 before) {
                for (crd::u32 q = 0; q < before; ++q)
                {
                    for (const Access& aq : accessor(m_passes[q]))
                    {
                        if (aq.handle == handle && aq.access != FgAccess::Read && aq.access != FgAccess::DepthRead)
                        {
                            return true;
                        }
                    }
                }
                return false;
            };
            for (crd::u32 w = 0; w < np; ++w)
            {
                for (const Access& aw : accessor(m_passes[w]))
                {
                    if (aw.access == FgAccess::Read || aw.access == FgAccess::DepthRead) { continue; }
                    for (crd::u32 r = 0; r < np; ++r)
                    {
                        for (const Access& ar : accessor(m_passes[r]))
                        {
                            if (ar.handle != aw.handle) { continue; }
                            const bool r_reads = (ar.access == FgAccess::Read || ar.access == FgAccess::DepthRead);
                            if (r_reads)
                            {
                                if (w < r) { add_edge(w, r); } // RAW: reader after writer
                                else if (w > r)                // reader declared BEFORE this writer
                                {
                                    if (has_value(ar.handle) || written_before(ar.handle, r)) { add_edge(r, w); } // WAR
                                    else { add_edge(w, r); } // forward-reference RAW → surfaces a cycle
                                }
                            }
                            else if (w < r) { add_edge(w, r); } // WAW: declaration order
                        }
                    }
                }
            }
        };
        link([](Pass& p) -> crd::containers::Array<Access>& { return p.img_access; },
             [&](crd::u32 h) { return m_images[h - 1U].own == Own::Persistent; });
        link([](Pass& p) -> crd::containers::Array<Access>& { return p.buf_access; },
             [&](crd::u32 h) { return !m_buffers[h - 1U].transient; });

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
            if (a.access != FgAccess::Read && a.access != FgAccess::DepthRead) { n.has_write = true; }
        }
        for (const Access& a : m_passes[pi].buf_access)
        {
            BufferNode& n = m_buffers[a.handle - 1U];
            if (n.first_pass < 0) { n.first_pass = static_cast<crd::i32>(oi); }
            n.last_pass = static_cast<crd::i32>(oi);
            if (a.access != FgAccess::Read && a.access != FgAccess::DepthRead) { n.has_write = true; }
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
    // ── ⭐ REN-38-B6: THE HARD BUDGET. Checked AFTER aliasing, because the budget is about what the frame
    // actually costs, not what it would cost without the aliaser — bounding the pre-alias total would reject
    // graphs that fit comfortably. ⛔ FAIL, never warn: the failure this prevents is an allocation that succeeds
    // on the dev machine and OOMs on the target months later, in a build nobody can bisect.
    m_over_budget = m_budget != 0U && static_cast<crd::u64>(m_physical_bytes) > m_budget;
    if (m_over_budget) { return false; }
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

        // REN-38-B6: a PINNED transient never reuses a heap slot, so it always allocates a fresh one — dedicated
        // memory, still created/barriered/retired like any other transient. Only IMAGES carry the pin today;
        // buffers have no author-visible shape to pin against yet, so they behave exactly as before.
        const bool pinned = images && m_images[idx].no_alias;
        crd::i32   chosen = -1;
        for (crd::u32 si = slot_base; !pinned && si < m_slots.size(); ++si)
        {
            // ⛔⛔ THE SLOT MUST FIT AS WELL AS BE FREE. A heap cannot grow after creation, so choosing a freed
            // slot by LIFETIME alone hands a 64 MB+ε resource a 64 MB heap and `CreatePlacedResource` fails at
            // offset 0 — the whole graph then refuses to build. Latent for as long as every same-class transient
            // happened to be the same size; the moment chain (three RGBA16F atlases beside the R32_TYPELESS
            // depth atlas, whose allocation infos differ by padding) is what finally hit it.
            if (m_slots[si].free_after < fp && m_slots[si].size >= sz) { chosen = static_cast<crd::i32>(si); break; }
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
            n.buffer = new Dx12TransientBuffer(n.size, n.resource.Get());
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
            // ⭐⭐ REN-39-D2: build the COMPANION DEPTH before any target is made (`slice_target` reads it).
            // Committed, D32_FLOAT, one subresource — matched to the colour transient's dimensions.
            if (!n.is_depth && n.desc.depth_buffer && n.depth_resource == nullptr)
            {
                D3D12_HEAP_PROPERTIES dhp{};
                dhp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC drd{};
                drd.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                drd.Width              = n.desc.width;
                drd.Height             = n.desc.height;
                drd.DepthOrArraySize   = 1;
                drd.MipLevels          = 1;
                drd.Format             = DXGI_FORMAT_D32_FLOAT;
                drd.SampleDesc.Count   = 1;
                drd.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                drd.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                // ⛔ An optimized clear value is REQUIRED for a depth resource that is cleared, or D3D12 warns and
                // some drivers take a slow path. It must MATCH the clear the passes issue — reverse-Z clears to 0.
                D3D12_CLEAR_VALUE dcv{};
                dcv.Format               = DXGI_FORMAT_D32_FLOAT;
                dcv.DepthStencil.Depth   = 0.0F;
                dcv.DepthStencil.Stencil = 0;
                if (FAILED(m_device->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_NONE, &drd,
                                                             D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv,
                                                             IID_PPV_ARGS(&n.depth_resource))))
                {
                    return false;
                }
                n.depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                D3D12_DESCRIPTOR_HEAP_DESC dhd{};
                dhd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
                dhd.NumDescriptors = 1;
                if (FAILED(m_device->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&n.depth_dsv_heap)))) { return false; }
                D3D12_DEPTH_STENCIL_VIEW_DESC ddsv{};
                ddsv.Format        = DXGI_FORMAT_D32_FLOAT;
                ddsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                m_device->CreateDepthStencilView(n.depth_resource.Get(), &ddsv,
                                                 n.depth_dsv_heap->GetCPUDescriptorHandleForHeapStart());
            }
            const auto slice_target = [&](const ComPtr<ID3D12DescriptorHeap>& heap) -> IRasterTarget* {
                // ⛔ REN-3.1: a DEPTH transient is a BORROWED DEPTH target — the placed resource goes in the DEPTH
                // slot so `draw_storage_depth_only` can render into it with no colour attachment. Before REN-3.1
                // depth transients got NEITHER a target NOR a texture, which is exactly why the device could not
                // render a shadow map.
                // ⭐ REN-39-D2: a COLOUR transient hands its companion depth + DSV heap to the depth slots, so
                // `has_depth()` is true and every scene verb depth-tests into it (the Vulkan `make_target` twin).
                return n.is_depth ? new Dx12RasterTarget(nullptr, nullptr, n.resource, nullptr, nullptr, heap, nullptr,
                                                         tfp, 1U, n.desc.width, n.desc.height)
                                  : new Dx12RasterTarget(n.resource, nullptr, n.depth_resource, nullptr, heap,
                                                         n.depth_dsv_heap, nullptr, tfp, 1U, n.desc.width,
                                                         n.desc.height);
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
                n.texture = new Dx12Texture(n.resource, n.desc.width, n.desc.height, srv, n.is_depth);
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
    m_present_count = 0U;
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
            const bool writes         = (a.access != FgAccess::Read && a.access != FgAccess::DepthRead);
            const bool depth_reattach = (a.access == FgAccess::DepthRead);
            if (graph_owned(n) && n.is_depth)
            {
                auto& dt = static_cast<Dx12RasterTarget&>(*n.target);
                // ⭐ REN-41 (tidy): writes and depth-reattach both transition to DEPTH_WRITE with an identical
                // barrier — one branch (previously two clones under different conditions).
                if ((writes || depth_reattach) && live_state(n) != D3D12_RESOURCE_STATE_DEPTH_WRITE)
                {
                    m_rc->frame_transition(dt.depth_tex(), live_state(n), D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    live_state(n) = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                    ++m_barrier_count;
                }
                else if (!writes && !depth_reattach && live_state(n) == D3D12_RESOURCE_STATE_DEPTH_WRITE)
                {
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
            // ── ⭐ REN-38-A6: THE STATE A PASS WANTS COMES FROM ITS KIND, not from read-vs-write alone. ──
            // A TRANSFER pass (copy / blit / resolve) needs COPY_DEST for what it writes and COPY_SOURCE for what
            // it reads. ⛔ Deriving from access alone would have handed CopyResource a destination still in
            // RENDER_TARGET — a D3D12 debug-layer break at best, silent corruption where the layer is off.
            const bool xfer = (p.kind == FgPassKind::Transfer);
            const D3D12_RESOURCE_STATES want_w = xfer ? D3D12_RESOURCE_STATE_COPY_DEST
                                                      : D3D12_RESOURCE_STATE_RENDER_TARGET;
            const D3D12_RESOURCE_STATES want_r = xfer ? D3D12_RESOURCE_STATE_COPY_SOURCE
                                                      : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            if (graph_owned(n)) // REN-2: an RTT image — render into it (RENDER_TARGET), then a later pass SAMPLES it
            {
                auto& tt = static_cast<Dx12RasterTarget&>(*n.target);
                // ⭐ REN-39-D2: the COMPANION depth is created in DEPTH_WRITE and stays there for the whole frame
                // — every pass that writes this colour transient also depth-tests/writes into it, and nothing
                // samples it. Parking it in one state keeps the walk honest without a second lifetime analysis.
                if (writes && n.depth_resource != nullptr && n.depth_state != D3D12_RESOURCE_STATE_DEPTH_WRITE)
                {
                    m_rc->frame_transition(n.depth_resource.Get(), n.depth_state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    n.depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                    ++m_barrier_count;
                }
                if (writes && live_state(n) != want_w)
                {
                    m_rc->frame_transition(tt.tex(), live_state(n), want_w);
                    live_state(n) = want_w;
                    ++m_barrier_count;
                }
                else if (!writes && live_state(n) != want_r)
                {
                    // the RTT barrier: the render pass's writes complete → this pass samples (or copies) it
                    m_rc->frame_transition(tt.tex(), live_state(n), want_r);
                    live_state(n) = want_r;
                    ++m_barrier_count;
                }
                continue;
            }
            if (p.present != nullptr) { continue; } // present pass reads → readback loop transitions
            if (xfer) // an IMPORTED target inside a transfer pass — same rule, no attachment special-casing
            {
                const D3D12_RESOURCE_STATES want = writes ? want_w : want_r;
                if (live_state(n) != want)
                {
                    m_rc->frame_transition(static_cast<Dx12RasterTarget&>(*n.target).tex(), live_state(n), want);
                    live_state(n) = want;
                    ++m_barrier_count;
                }
                continue;
            }
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
        m_rc->frame_rec_new_pass(); // REN-38-B8: no pass inherits its neighbour's sampler
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
        // ⛔ REN-38-A6: any live state that is not already COMMON, not just RENDER_TARGET. A transfer pass
        // leaves its destination in COPY_DEST, and the old `== RENDER_TARGET` test skipped it — so a frame whose
        // LAST write was a copy read back the target's PREVIOUS contents. Every check green, image stale.
        if (graph_owned(n) || n.target == nullptr || n.state == D3D12_RESOURCE_STATE_COMMON) { continue; }
        if (m_readback)
        {
            m_rc->frame_readback_from(static_cast<Dx12RasterTarget&>(*n.target), n.state);
            m_barrier_count += 2U; // the readback inserts <live state>→COPY_SOURCE and COPY_SOURCE→COMMON
        }
        else
        {
            // ⛔ SKIPPING THE READBACK MUST SKIP THE COPY, NEVER THE TRANSITION — the identical scar Vulkan
            // carries a few hundred lines away, in its DX12 form. `frame_readback` did two things: a host copy AND
            // a RENDER_TARGET → COMMON move. `IPresentSurface::present` consumes its source in COMMON (the RET-2
            // contract), so dropping both left every presented target in RENDER_TARGET.
            // It stayed invisible for the same reason it did on Vulkan: every GATE runs with readback ON. Only the
            // sandbox turns it off — and only the sandbox presents.
            m_rc->frame_transition(static_cast<Dx12RasterTarget&>(*n.target).tex(), n.state,
                                   D3D12_RESOURCE_STATE_COMMON);
            ++m_barrier_count;
        }
        n.state = D3D12_RESOURCE_STATE_COMMON;
    }

    // REN-8: same contract as Vulkan — readback on ⇒ wait now (gates read pixels the instant execute() returns);
    // readback off ⇒ ONE ExecuteCommandLists and the block deferred to the top of the next execute()/reset().
    m_rc->frame_submit_no_wait(); // ONE ExecuteCommandLists
    m_pending_submit = true;
    m_rc->frame_rec_end();
    m_submit_count = 1U;
    if (m_readback) { wait_pending_submit(); }

    // ── ⭐ REN-38-A5: THE PRESENT PASS PRESENTS — the DXGI mirror of the Vulkan rule. ──
    // ⛔ `.present(surface)` was stored, read by the barrier scheduler to SKIP a transition, and then never acted
    // on. Declared-and-ignored, with every check green.
    //
    // After the submit, not inside a pass: presenting is acquire → copy → Present on submissions the SURFACE owns.
    // Both go to the same queue, so ordering holds without a CPU stall, and the readback loop above already left
    // every imported target in COMMON — exactly what `IPresentSurface::present` consumes.
    for (const crd::u32 pass_idx : m_order)
    {
        Pass& p = m_passes[pass_idx];
        if (p.present == nullptr) { continue; }
        IRasterTarget* src = nullptr;
        for (const Access& a : p.img_access)
        {
            if (a.handle == 0U || a.handle > m_images.size()) { continue; }
            ImageNode& n = m_images[a.handle - 1U];
            // ⛔ A GRAPH-OWNED image cannot be presented: its memory is aliased and retired once its last reader
            // is done, so the surface would copy from storage another transient already owns. Only an IMPORTED
            // target outlives the graph, so only an imported target is a legal present source.
            if (n.target != nullptr && !graph_owned(n)) { src = n.target; break; }
        }
        if (src == nullptr) { continue; }
        if (p.present->present(*src)) { ++m_present_count; }
    }
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

// ── REN-39-D2: the narrow native-handle door (see dx12_raster_context.hpp) ────────────────────────────────────
void* dx12_device_raw(IRasterContext& raster) noexcept
{
    return static_cast<void*>(static_cast<Dx12RasterContext&>(raster).dx_device());
}

void* dx12_graphics_queue_raw(IRasterContext& raster) noexcept
{
    return static_cast<void*>(static_cast<Dx12RasterContext&>(raster).frame_cmd_queue());
}

crd::u32 dx12_present_image_count(const IPresentSurface& surface) noexcept
{
    return static_cast<const Dx12PresentSurface&>(surface).image_count();
}

crd::u32 dx12_present_color_format_raw(const IPresentSurface& surface) noexcept
{
    return static_cast<crd::u32>(static_cast<const Dx12PresentSurface&>(surface).color_format());
}

} // namespace crd::gpu
