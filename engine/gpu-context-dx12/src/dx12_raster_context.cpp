// dx12_raster_context.cpp — the D3D12 implementation of crd::gpu::IRasterContext (ADR-0103 / D-008 C4). The DX12 mirror
// of the Vulkan raster context (C1): a graphics (DIRECT) queue + offscreen RGBA8 render targets + a CLEAR with pixel
// readback. Raw D3D12, no crd-rhi. D3D12 has no dynamic-rendering equivalent to fuss over — a bare RTV + ClearRTV is the
// whole clear path; the texture→readback copy honours the 256-byte row-pitch alignment D3D12 demands (GetCopyableFootprints).
// The shader DRAW path (a graphics PSO from a VS+FS DXIL pair + DrawInstanced) appends in C4-b.

#include <crd/gpu/dx12_raster_context.hpp>

#include <crd/gpu/dx12_context.hpp> // Dx12GpuProgram::dxil() — the VS+FS bytecode a graphics PSO is built from (C4-b)

#include <crd/core/types.hpp>

#include <d3d12.h>
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
                                               D3D12_COMPARISON_FUNC depth_func, bool conservative, UINT num_rts = 1)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature                                   = root;
    pd.VS                                               = vs;
    pd.PS                                               = fs;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; // no blend, write RGBA
    pd.SampleMask                                       = 0xFFFFFFFFU;
    pd.RasterizerState.FillMode                         = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode                         = D3D12_CULL_MODE_NONE; // attributeless triangle: winding moot
    pd.RasterizerState.DepthClipEnable                  = TRUE;
    // B1-f: conservative rasterization (a PSO-baked raster state on DX12 — no dynamic-state equivalent). Both Overestimate
    // and Underestimate map to the single D3D12 ON mode; Underestimate is realized in the FS by reading SV_InnerCoverage.
    if (conservative) { pd.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON; }
    pd.PrimitiveTopologyType                            = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets                                 = num_rts; // B5: >1 for a deferred G-buffer (MRT)
    for (UINT i = 0; i < num_rts; ++i) { pd.RTVFormats[i] = kColorFormat; }
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
                      std::unique_ptr<crd::u8[]> as = nullptr, crd::usize as_size = 0) noexcept // B4: AS (task) DXIL
        : m_device(device), m_device2(device2), m_is_mesh(is_mesh), m_root(std::move(root)), m_vs(std::move(vs)),
          m_vs_size(vs_size), m_fs(std::move(fs)), m_fs_size(fs_size), m_as(std::move(as)), m_as_size(as_size),
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
    [[nodiscard]] ID3D12RootSignature* root() const noexcept { return m_root.Get(); }

    // The PSO for a target of `samples` samples and depth/conservative config (a graphics PSO bakes ALL of them). The plain
    // 1×/no-depth/non-conservative PSO is prebuilt (also gates valid()); every other combo is built + cached lazily in a
    // small keyed cache (a handful of configs per program at most). `dsv == DXGI_FORMAT_UNKNOWN` ⇒ the depth-off colour path.
    [[nodiscard]] ID3D12PipelineState* pso_for(crd::u32 samples, DXGI_FORMAT dsv, D3D12_COMPARISON_FUNC depth_func,
                                               bool conservative, crd::u32 num_rts = 1U)
    {
        if (samples <= 1U && dsv == DXGI_FORMAT_UNKNOWN && !conservative && num_rts == 1U) { return m_pso1.Get(); }
        const crd::u32 key = (samples << 8U)
                             | (dsv != DXGI_FORMAT_UNKNOWN ? (0x80U | static_cast<crd::u32>(depth_func)) : 0U)
                             | (conservative ? 0x10000U : 0U) | (num_rts << 20U); // B5: RT count in the key (MRT G-buffer)
        for (int i = 0; i < m_cache_n; ++i) { if (m_cache[i].key == key) { return m_cache[i].pso.Get(); } }
        if (m_cache_n >= kPsoCacheCap) { return nullptr; }
        m_cache[m_cache_n].key = key;
        m_cache[m_cache_n].pso =
            m_is_mesh ? build_mesh_pso(m_device2, m_root.Get(), D3D12_SHADER_BYTECODE{m_vs.get(), m_vs_size},
                                       D3D12_SHADER_BYTECODE{m_fs.get(), m_fs_size}, samples, dsv, depth_func, num_rts,
                                       D3D12_SHADER_BYTECODE{m_as.get(), m_as_size}) // B4: AS (task) when present, else empty
                      : build_graphics_pso(m_device, m_root.Get(), D3D12_SHADER_BYTECODE{m_vs.get(), m_vs_size},
                                           D3D12_SHADER_BYTECODE{m_fs.get(), m_fs_size}, samples, dsv, depth_func,
                                           conservative, num_rts);
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
    ComPtr<ID3D12RootSignature> m_root;
    std::unique_ptr<crd::u8[]>  m_vs; // owned DXIL copies so a PSO can be rebuilt at any (samples, depth) config
    crd::usize                  m_vs_size = 0;
    std::unique_ptr<crd::u8[]>  m_fs;
    crd::usize                  m_fs_size = 0;
    std::unique_ptr<crd::u8[]>  m_as; // B4: owned AS (task) DXIL copy — empty for a plain mesh program
    crd::usize                  m_as_size = 0;
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
        param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[0].DescriptorTable.NumDescriptorRanges = 1; param[0].DescriptorTable.pDescriptorRanges = &uav_range;      param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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
        param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[0].DescriptorTable.NumDescriptorRanges = 1; param[0].DescriptorTable.pDescriptorRanges = &uav_range;      param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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
        param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param[0].DescriptorTable.NumDescriptorRanges = 1; param[0].DescriptorTable.pDescriptorRanges = &uav_range;      param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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

    void draw_storage(IRasterTarget& target, IRasterProgram& program, ClearColor clear, IStorageBuffer& storage,
                      crd::u32 vertex_count) override
    {
        if (!m_ok || m_uav_heap == nullptr) { return; }
        auto&                t   = static_cast<Dx12RasterTarget&>(target);
        auto&                p   = static_cast<Dx12RasterProgram&>(program);
        auto&                s   = static_cast<Dx12StorageBuffer&>(storage);
        ID3D12PipelineState* pso = p.pso_for(1U, DXGI_FORMAT_UNKNOWN, D3D12_COMPARISON_FUNC_LESS, false); // single-sample colour
        if (!p.valid() || pso == nullptr) { return; }

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
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter  = after;
        m_list->ResourceBarrier(1, &b);
    }

    void submit_and_wait()
    {
        m_list->Close();
        ID3D12CommandList* lists[] = {m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        ++m_fence_val;
        m_queue->Signal(m_fence.Get(), m_fence_val);
        if (m_fence->GetCompletedValue() < m_fence_val)
        {
            m_fence->SetEventOnCompletion(m_fence_val, m_event);
            WaitForSingleObject(m_event, INFINITE);
        }
    }

    ComPtr<ID3D12Device>               m_device;
    ComPtr<ID3D12Device2>              m_device2; // B4: CreatePipelineState (stream PSO) for mesh — null on an old runtime
    ComPtr<ID3D12CommandQueue>         m_queue;
    ComPtr<ID3D12CommandAllocator>     m_cmd_alloc;
    ComPtr<ID3D12GraphicsCommandList>  m_list;
    ComPtr<ID3D12GraphicsCommandList5> m_list5; // B1-e: RSSetShadingRate(Image) — null on an old runtime
    ComPtr<ID3D12GraphicsCommandList6> m_list6; // B4: DispatchMesh — null on an old runtime
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

std::unique_ptr<IRasterContext> create_dx12_raster_context(crd::memory::IAllocator* /*alloc*/)
{
    auto ctx = std::make_unique<Dx12RasterContext>();
    if (!ctx->valid()) { return nullptr; }
    return ctx;
}

} // namespace crd::gpu
