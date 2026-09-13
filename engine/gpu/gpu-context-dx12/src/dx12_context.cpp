// dx12_context.cpp — the D3D12 IGpuContext + the DX12 program-authoring seam (ADR-0099/0103 / D-008 C4-b). The DX12
// mirror of vulkan_context.cpp: mint Dx12GpuPrograms (cooked DXIL) from bytecode or from a CKIR graph (crd-kir emits
// HLSL, dxc lowers it to DXIL). The HLSL text and the DXIL bytes never escape this backend (I1/I2). A held D3D12 device
// gives the context an adapter identity + honours "a context is a live device foundation" (ADR-0099); DXIL itself is
// device-independent, so program authoring needs only dxc.

#include "dx12_device_scope.hpp"

#include "dx12_adapter_classification.hpp"
#include "dx12_execution.hpp" // B1-f: dx12_inner_coverage_route shared with the raster context

#include <crd/gpu/dx12_context.hpp>

#include <crd/kir/ckir_hlsl.hpp> // emit_stage_hlsl (+ ckir.hpp: KGraph/KEntry/KStage, and ckir_glsl.hpp: GlslKernel)
#include <crd/gpu/raster_context.hpp> // InnerCoverageRoute (B1-f): the per-provider inner-coverage decision

#include <atomic>
#include <cstring>

#include <d3d12.h>
#include <d3dkmthk.h>
#include <dxcapi.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

namespace crd::gpu
{
// B1-f: one stage compile with an explicit dxc profile; compile_hlsl_to_dxil uses the stage's default profile.
[[nodiscard]] static DxilCompileResult compile_stage_dxil(const wchar_t* profile, ShaderStage stage,
                                                          crd::containers::StringView source,
                                                          crd::memory::IAllocator* a);


using Microsoft::WRL::ComPtr;

namespace
{

class KernelAdapter
{
public:
    explicit KernelAdapter(LUID luid) noexcept
    {
        D3DKMT_OPENADAPTERFROMLUID request{};
        request.AdapterLuid = luid;
        if (D3DKMTOpenAdapterFromLuid(&request) >= 0) { m_handle = request.hAdapter; }
    }

    ~KernelAdapter() noexcept
    {
        if (m_handle != 0U)
        {
            D3DKMT_CLOSEADAPTER request{};
            request.hAdapter = m_handle;
            (void)D3DKMTCloseAdapter(&request);
        }
    }

    KernelAdapter(const KernelAdapter&) = delete;
    KernelAdapter& operator=(const KernelAdapter&) = delete;

    [[nodiscard]] bool query_type(bool& software, bool& render) const noexcept
    {
        if (m_handle == 0U) { return false; }
        D3DKMT_ADAPTERTYPE type{};
        D3DKMT_QUERYADAPTERINFO request{};
        request.hAdapter = m_handle;
        request.Type = KMTQAITYPE_ADAPTERTYPE;
        request.pPrivateDriverData = &type;
        request.PrivateDriverDataSize = sizeof(type);
        if (D3DKMTQueryAdapterInfo(&request) < 0) { return false; }
        software = type.SoftwareDevice != 0U;
        render = type.RenderSupported != 0U;
        return true;
    }

private:
    D3DKMT_HANDLE m_handle = 0;
};

[[nodiscard]] const wchar_t* dxil_profile(ShaderStage stage) noexcept
{
    switch (stage)
    {
    case ShaderStage::Vertex: return L"vs_6_0";
    case ShaderStage::Fragment: return L"ps_6_0";
    case ShaderStage::Mesh: return L"ms_6_5"; // B4: Shader Model 6.5 mesh shader
    case ShaderStage::Task: return L"as_6_5"; // B4: SM6.5 amplification shader (DispatchMesh)
    case ShaderStage::TessControl: return L"hs_6_0"; // B4-tess: hull shader
    case ShaderStage::TessEval: return L"ds_6_0";    // B4-tess: domain shader
    case ShaderStage::RayGen:       // FA-2: DXR shaders compile as a LIBRARY — the [shader("...")] attribute names the stage
    case ShaderStage::Intersection:
    case ShaderStage::AnyHit:
    case ShaderStage::ClosestHit:
    case ShaderStage::Miss:
    case ShaderStage::Callable: return L"lib_6_3";
    case ShaderStage::Compute:
    default: return L"cs_6_5"; // SM 6.5: inline RayQuery (DXR 1.1) + wave ops; a superset that still compiles cs_6_0 kernels
    }
}

[[nodiscard]] const wchar_t* dxil_entry(ShaderStage stage) noexcept
{
    return (stage == ShaderStage::Compute) ? L"cs_main" : L"main"; // compute kernels emit cs_main; raster stages use main
}

// Map a CKIR stage to the gpu-context stage. Only the raster stages have a DX12 emitter; anything else is refused.
[[nodiscard]] bool kstage_to_shader_stage(crd::kir::KStage ks, ShaderStage& out) noexcept
{
    switch (ks)
    {
    case crd::kir::KStage::Vertex: out = ShaderStage::Vertex; return true;
    case crd::kir::KStage::Fragment: out = ShaderStage::Fragment; return true;
    case crd::kir::KStage::Mesh: out = ShaderStage::Mesh; return true; // B4: DX12 mesh device path
    case crd::kir::KStage::Task: out = ShaderStage::Task; return true; // B4: DX12 amplification (task) path
    case crd::kir::KStage::TessControl: out = ShaderStage::TessControl; return true; // B4-tess: DX12 hull
    case crd::kir::KStage::TessEval: out = ShaderStage::TessEval; return true;       // B4-tess: DX12 domain
    // ⛔⛔ REN-38-A10: COMPUTE was MISSING, so `create_program(KGraph, KEntry)` refused every kernel on DX12 —
    // while the dedicated `KStage::Compute && is_kernel()` branch a few lines below, which emits the HLSL, sat
    // UNREACHABLE. Every authored compute pass on this backend failed at program creation and the caller saw
    // only a null pointer.
    // ⛔ THIS INVALIDATED 38-A2's "both backends" claim: the compute-pass gate was written on Vulkan only, so
    // nothing ever asked DX12 to lower a kernel through this entry point. The A9/A10 gates are the first that do.
    case crd::kir::KStage::Compute: out = ShaderStage::Compute; return true;
    // ⛔ REN-38-A16: the RAY-TRACING stages. `emit_rt_stage_hlsl` and the `lib_6_3` DXIL profile both existed;
    // this map did not name them, so every CKIR ray-tracing entry was refused before either could run.
    case crd::kir::KStage::RayGen:     out = ShaderStage::RayGen;     return true;
    case crd::kir::KStage::ClosestHit: out = ShaderStage::ClosestHit; return true;
    case crd::kir::KStage::Miss:       out = ShaderStage::Miss;       return true;
    case crd::kir::KStage::AnyHit:     out = ShaderStage::AnyHit;     return true;
    // REN-38-F13: the last two stages — same library profile, same state-object consumption
    case crd::kir::KStage::Intersection: out = ShaderStage::Intersection; return true;
    case crd::kir::KStage::Callable:     out = ShaderStage::Callable;     return true;
    default: return false;
    }
}

// ── Dx12GpuProgram: an opaque IGpuProgram carrying cooked DXIL ───────────────────────────────────────────────────────
class Dx12GpuProgramImpl final : public Dx12GpuProgram
{
public:
    Dx12GpuProgramImpl(ShaderStage stage, crd::containers::ConstSpan<crd::u8> cooked, crd::memory::IAllocator* alloc,
                       bool wants_conservative = false)
        : m_stage(stage), m_dxil(alloc), m_wants_conservative(wants_conservative)
    {
        m_dxil.resize(cooked.size());
        for (crd::usize i = 0U; i < cooked.size(); ++i) { m_dxil[i] = cooked[i]; }
    }
    ~Dx12GpuProgramImpl() override                             = default;
    Dx12GpuProgramImpl(const Dx12GpuProgramImpl&)              = delete;
    Dx12GpuProgramImpl& operator=(const Dx12GpuProgramImpl&)   = delete;
    Dx12GpuProgramImpl(Dx12GpuProgramImpl&&)                   = delete;
    Dx12GpuProgramImpl& operator=(Dx12GpuProgramImpl&&)        = delete;

    [[nodiscard]] bool        valid() const noexcept override { return m_dxil.size() > 0U; }
    [[nodiscard]] ShaderStage stage() const noexcept override { return m_stage; }
    [[nodiscard]] crd::containers::ConstSpan<crd::u8> dxil() const noexcept override
    {
        return {m_dxil.data(), m_dxil.size()};
    }
    [[nodiscard]] bool wants_conservative_raster() const noexcept override { return m_wants_conservative; }

private:
    ShaderStage                     m_stage;
    crd::containers::Array<crd::u8> m_dxil;
    bool                            m_wants_conservative = false;
};

// ── Dx12GpuContext: the IGpuContext (device identity + program authoring) ────────────────────────────────────────────
class Dx12GpuContext final : public IGpuContext
{
public:
    explicit Dx12GpuContext(crd::memory::IAllocator* alloc) : m_alloc(alloc)
    {
        if (FAILED(m_validation.create(m_device))) { return; }
        capture_adapter_name();
        m_ok = true;
    }
    ~Dx12GpuContext() override                          = default;
    Dx12GpuContext(const Dx12GpuContext&)               = delete;
    Dx12GpuContext& operator=(const Dx12GpuContext&)    = delete;
    Dx12GpuContext(Dx12GpuContext&&)                    = delete;
    Dx12GpuContext& operator=(Dx12GpuContext&&)         = delete;

    [[nodiscard]] bool        valid() const noexcept override { return m_ok; }
    [[nodiscard]] GpuBackend  backend() const noexcept override { return GpuBackend::Dx12; }
    [[nodiscard]] const char* adapter_name() const noexcept override { return m_adapter; }

    [[nodiscard]] std::unique_ptr<IGpuProgram>
    create_program(ShaderStage stage, crd::containers::ConstSpan<crd::u8> cooked) override
    {
        return make_dx12_program(stage, cooked, m_alloc);
    }

    [[nodiscard]] std::unique_ptr<IGpuProgram>
    create_program(const crd::kir::KGraph& graph, const crd::kir::KEntry& entry) override
    {
        ShaderStage stage{};
        if (!kstage_to_shader_stage(entry.stage, stage)) { return nullptr; } // a stage DX12 can't lower ⇒ refuse loudly

        // IR on-ramp: crd-kir emits the stage HLSL (refuses a vertex with no clip position), dxc lowers it to DXIL. The
        // HLSL text lives only across this call; the DXIL never surfaces beyond the returned opaque program.
        crd::kir::GlslKernel kern(m_alloc);
        // B1-f: does this fragment program read InnerCoverage? Then its PSO must be conservative (the raster context
        // prebuilds it conservative — the D3D12 rasterizer rejects SV_InnerCoverage with conservative OFF, and the corner
        // test only sees edge pixels under overestimate) and its emitted route follows the device (below).
        bool wants_conservative = false;
        if (entry.stage == crd::kir::KStage::Fragment)
        {
            for (int i = 0; i < graph.size(); ++i)
            {
                if (graph.node(i).op == crd::kir::KOp::Builtin
                    && static_cast<crd::kir::KBuiltin>(graph.node(i).iidx) == crd::kir::KBuiltin::InnerCoverage)
                {
                    wants_conservative = true;
                    break;
                }
            }
        }
        const wchar_t* profile = dxil_profile(stage);
        if (entry.stage == crd::kir::KStage::Mesh)
        {
            // B4: a mesh KEntry → SM6.5 mesh HLSL (SetMeshOutputCounts + out vertices/indices). emit_stage_hlsl refuses
            // non-Vertex/Fragment, so the mesh branch must precede it; the device mesh PSO + DispatchMesh live in the raster
            // context (create_mesh_program/draw_mesh).
            if (!crd::kir::emit_mesh_hlsl(graph, entry, m_alloc, kern)) { return nullptr; }
        }
        else if (entry.stage == crd::kir::KStage::Task)
        {
            // B4: a task KEntry → SM6.5 amplification HLSL (DispatchMesh + groupshared payload). The task→mesh PSO (AS+MS+PS)
            // lives in the raster context (create_task_mesh_program).
            if (!crd::kir::emit_task_hlsl(graph, entry, m_alloc, kern)) { return nullptr; }
        }
        else if (entry.stage == crd::kir::KStage::TessControl)
        {
            // B4-tess: a hull KEntry → HLSL hull shader (patch-constant tess factors + passthrough). The VS+HS+DS+PS graphics
            // PSO + DrawInstanced(PATCH_LIST) live in the raster context (create_tess_program / draw_tess).
            if (!crd::kir::emit_tesc_hlsl(graph, entry, m_alloc, kern)) { return nullptr; }
        }
        else if (entry.stage == crd::kir::KStage::TessEval)
        {
            // B4-tess: a domain KEntry → HLSL domain shader (bilerp patch_pos + displacement → SV_Position).
            if (!crd::kir::emit_tese_hlsl(graph, entry, m_alloc, kern)) { return nullptr; }
        }
        else if (entry.stage == crd::kir::KStage::RayGen || entry.stage == crd::kir::KStage::ClosestHit
                 || entry.stage == crd::kir::KStage::Miss || entry.stage == crd::kir::KStage::AnyHit
                 || entry.stage == crd::kir::KStage::Intersection || entry.stage == crd::kir::KStage::Callable)
        {
            // REN-38-A16: a CKIR ray-tracing entry → DXR HLSL (`[shader("raygeneration")]` etc.), compiled as a
            // `lib_6_3` LIBRARY — which is what a DXR state object consumes, and why the profile table already
            // had the case.
            if (!crd::kir::emit_rt_stage_hlsl(graph, entry, m_alloc, kern, false)) { return nullptr; }
        }
        else if (entry.stage == crd::kir::KStage::Compute && entry.is_kernel())
        {
            // B-cmp: an imperative shared-memory/barrier compute kernel (FFT/reduction/transpose) → the DX12 mirror of the
            // Vulkan kernel path (emit_compute_kernel_hlsl), so create_program(g, e) lowers a kernel on BOTH backends.
            if (!crd::kir::emit_compute_kernel_hlsl(graph, entry, m_alloc, kern)) { return nullptr; }
        }
        else
        {
            // B1-f: a fragment reading InnerCoverage is lowered through the route this device qualifies: the native
            // SV_InnerCoverage bit or the barycentric pixel-corner test (SV_Barycentrics, shader model 6.1). Neither
            // qualified means no program: the caller sees the refusal instead of a wrong fully-covered bit.
            crd::kir::HlslInnerCoverage inner = crd::kir::HlslInnerCoverage::Native;
            if (wants_conservative)
            {
                const InnerCoverageRoute route = detail::dx12_inner_coverage_route(m_device.Get());
                if (route == InnerCoverageRoute::Unsupported) { return nullptr; }
                if (route == InnerCoverageRoute::Barycentric)
                {
                    inner   = crd::kir::HlslInnerCoverage::Barycentric;
                    profile = L"ps_6_1"; // SV_Barycentrics
                }
            }
            if (!crd::kir::emit_stage_hlsl(graph, entry, m_alloc, kern, inner)) { return nullptr; }
        }
        const auto dxil = compile_stage_dxil(profile, stage, crd::containers::to_view(kern.source), m_alloc);
        if (!dxil.ok) { return nullptr; }
        if (dxil.dxil.size() == 0U) { return nullptr; }
        return std::make_unique<Dx12GpuProgramImpl>(
            stage, crd::containers::ConstSpan<crd::u8>(dxil.dxil.data(), dxil.dxil.size()), m_alloc, wants_conservative);
    }

private:
    void capture_adapter_name() noexcept
    {
        m_adapter[0] = '\0';
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { return; }
        ComPtr<IDXGIAdapter1> adapter;
        const LUID            luid = m_device->GetAdapterLuid();
        if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) && adapter != nullptr)
        {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)))
            {
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, m_adapter, sizeof(m_adapter), nullptr, nullptr);
            }
        }
    }

    detail::Dx12DeviceScope m_validation;
    crd::memory::IAllocator* m_alloc = nullptr;
    ComPtr<ID3D12Device>     m_device;
    char                     m_adapter[192] = {};
    bool                     m_ok           = false;
};

} // namespace

Dx12AdapterKind detail::query_dx12_adapter_kind(u32 luid_low, i32 luid_high) noexcept
{
    const LUID luid{luid_low, luid_high};
    detail::Dx12AdapterEvidence evidence{};
    const KernelAdapter kernel(luid);
    evidence.kernel_available = kernel.query_type(evidence.kernel_software, evidence.kernel_render);
    ComPtr<IDXGIFactory4> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) && adapter != nullptr)
        {
            DXGI_ADAPTER_DESC1 desc{};
            evidence.dxgi_available = SUCCEEDED(adapter->GetDesc1(&desc));
            evidence.dxgi_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U;
            evidence.dxgi_vendor = desc.VendorId;
            evidence.dxgi_device = desc.DeviceId;
        }
    }
    return detail::classify_dx12_adapter(evidence);
}

Dx12AdapterKind dx12_default_adapter_kind() noexcept
{
    // Match the contexts' default device; classify its selected identity, not the computer's display hardware.
    detail::Dx12DeviceScope validation;
    ComPtr<ID3D12Device> device;
    if (FAILED(validation.create(device)))
    {
        return Dx12AdapterKind::Unknown;
    }
    const LUID luid = device->GetAdapterLuid();
    return detail::query_dx12_adapter_kind(luid.LowPart, luid.HighPart);
}

bool dx12_default_adapter_is_software() noexcept
{
    return dx12_default_adapter_kind() == Dx12AdapterKind::Software;
}

namespace
{
// B1-f: -1 = no override, else the forced InnerCoverageRoute (dx12_override_inner_coverage_route).
std::atomic<int> s_inner_coverage_override{-1};

[[nodiscard]] bool inner_coverage_route_runnable(ID3D12Device* device, InnerCoverageRoute route) noexcept
{
    if (device == nullptr) { return false; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) { return false; }
    switch (route)
    {
    case InnerCoverageRoute::Native:
        return options.ConservativeRasterizationTier >= D3D12_CONSERVATIVE_RASTERIZATION_TIER_3;
    case InnerCoverageRoute::Barycentric:
    {
        if (options.ConservativeRasterizationTier < D3D12_CONSERVATIVE_RASTERIZATION_TIER_1) { return false; }
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3)))
            || options3.BarycentricsSupported == FALSE)
        {
            return false;
        }
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_1}; // the runtime lowers it to the highest supported
        return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)))
            && model.HighestShaderModel >= D3D_SHADER_MODEL_6_1;
    }
    default: return false;
    }
}
} // namespace

bool detail::dx12_inner_coverage_route_runnable(ID3D12Device* device, InnerCoverageRoute route) noexcept
{
    return inner_coverage_route_runnable(device, route);
}

InnerCoverageRoute detail::dx12_inner_coverage_route(ID3D12Device* device) noexcept
{
    const int forced = s_inner_coverage_override.load(std::memory_order_relaxed);
    if (forced >= 0) { return static_cast<InnerCoverageRoute>(forced); }
    if (device == nullptr) { return InnerCoverageRoute::Unsupported; }
    if (inner_coverage_route_runnable(device, InnerCoverageRoute::Native))
    {
        // Only the documented software provider is diverted: its Tier-3 claim is contradicted by the reproduced false
        // interior bit. Hardware and unclassifiable adapters keep the D3D12 contract; the pixel-corner oracle checks them.
        const LUID luid = device->GetAdapterLuid();
        if (detail::query_dx12_adapter_kind(luid.LowPart, luid.HighPart) != Dx12AdapterKind::Software)
        {
            return InnerCoverageRoute::Native;
        }
    }
    return inner_coverage_route_runnable(device, InnerCoverageRoute::Barycentric) ? InnerCoverageRoute::Barycentric
                                                                                   : InnerCoverageRoute::Unsupported;
}

InnerCoverageRoute dx12_default_inner_coverage_route() noexcept
{
    detail::Dx12DeviceScope validation;
    ComPtr<ID3D12Device> device;
    if (FAILED(validation.create(device))) { return InnerCoverageRoute::Unsupported; }
    return detail::dx12_inner_coverage_route(device.Get());
}

bool dx12_override_inner_coverage_route(InnerCoverageRoute route) noexcept
{
    detail::Dx12DeviceScope validation;
    ComPtr<ID3D12Device> device;
    if (FAILED(validation.create(device)) || !inner_coverage_route_runnable(device.Get(), route)) { return false; }
    s_inner_coverage_override.store(static_cast<int>(route), std::memory_order_relaxed);
    return true;
}

void dx12_clear_inner_coverage_route_override() noexcept
{
    s_inner_coverage_override.store(-1, std::memory_order_relaxed);
}

// The shared dxc HLSL->DXIL core: lazy-load dxcompiler.dll, compile `source` with the caller's `args` (the -T/-E flags),
// and return the signed DXIL container (or ok=false + the dxc error blob). Both the stage-profile compile and the
// CEIR-20c-1 Work Graph LIBRARY compile (lib_6_8, no -E) route through here — one dxc load, one error path.
static DxilCompileResult compile_dxil_core(crd::containers::StringView source, const wchar_t** args, UINT32 nargs,
                                           crd::memory::IAllocator* a)
{
    DxilCompileResult result(a);

    // Lazy-init dxc for process lifetime (dxcompiler.dll owns internal singletons; dxil.dll signs the container).
    static HMODULE               s_lib    = LoadLibraryW(L"dxcompiler.dll");
    static DxcCreateInstanceProc s_create = (s_lib != nullptr)
        // NOLINTNEXTLINE(clang-diagnostic-cast-function-type-strict) — GetProcAddress returns FARPROC; a direct
        // reinterpret_cast to the real proc type is the standard Win32 idiom (no void* hop).
                                                 ? reinterpret_cast<DxcCreateInstanceProc>(
                                                       GetProcAddress(s_lib, "DxcCreateInstance"))
                                                 : nullptr;
    if (s_lib == nullptr || s_create == nullptr)
    {
        result.error_message = crd::containers::String("dxcompiler.dll not loaded", a);
        return result;
    }

    ComPtr<IDxcCompiler3> compiler;
    if (FAILED(s_create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) || compiler == nullptr)
    {
        result.error_message = crd::containers::String("dxc: CLSID_DxcCompiler failed", a);
        return result;
    }

    DxcBuffer src{};
    src.Ptr      = source.data();
    src.Size     = source.size();
    src.Encoding = DXC_CP_UTF8;

    ComPtr<IDxcResult> dxc_result;
    if (FAILED(compiler->Compile(&src, args, nargs, nullptr, IID_PPV_ARGS(&dxc_result))))
    {
        result.error_message = crd::containers::String("dxc: Compile() failed", a);
        return result;
    }
    HRESULT status = S_OK;
    dxc_result->GetStatus(&status);
    if (FAILED(status))
    {
        ComPtr<IDxcBlobUtf8> errors;
        if (SUCCEEDED(dxc_result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors != nullptr
            && errors->GetStringLength() > 0U)
        {
            result.error_message =
                crd::containers::String(crd::containers::StringView(errors->GetStringPointer(),
                                                                    static_cast<crd::usize>(errors->GetStringLength())),
                                        a);
        }
        else
        {
            result.error_message = crd::containers::String("dxc: compile failed (no error blob)", a);
        }
        return result;
    }

    ComPtr<IDxcBlob> obj;
    if (FAILED(dxc_result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) || obj == nullptr
        || obj->GetBufferSize() == 0U)
    {
        result.error_message = crd::containers::String("dxc: compile succeeded but no DXIL output", a);
        return result;
    }
    const auto*      bytes = static_cast<const crd::u8*>(obj->GetBufferPointer());
    const crd::usize size  = static_cast<crd::usize>(obj->GetBufferSize());
    result.dxil.resize(size);
    for (crd::usize i = 0U; i < size; ++i) { result.dxil[i] = bytes[i]; }
    result.ok = true;
    return result;
}

DxilCompileResult compile_hlsl_to_dxil(ShaderStage stage, crd::containers::StringView source,
                                       crd::containers::StringView /*name*/, crd::memory::IAllocator* a)
{
    return compile_stage_dxil(dxil_profile(stage), stage, source, a);
}

static DxilCompileResult compile_stage_dxil(const wchar_t* profile, ShaderStage stage,
                                            crd::containers::StringView source, crd::memory::IAllocator* a)
{
    const wchar_t* args[] = {L"-T", profile, L"-E", dxil_entry(stage)}; // no -spirv ⇒ signed DXIL
    return compile_dxil_core(source, args, static_cast<UINT32>(sizeof(args) / sizeof(args[0])), a);
}

// CEIR-20c-1: compile a Work Graph NODE LIBRARY (the emit_work_graph_node_hlsl output — one or more [Shader("node")]
// functions) to DXIL. `lib_6_8` target with NO -E (a library exports all its node shaders; the state object picks the
// entry). Needs a Work-Graphs-capable dxcompiler.dll (SM 6.8 node attributes) on the DLL load path — the WG device gate
// copies the Windows SDK dxc next to its exe so LoadLibraryW finds it ahead of an older PATH dxc.
DxilCompileResult compile_work_graph_library_to_dxil(crd::containers::StringView source,
                                                     crd::containers::StringView /*name*/, crd::memory::IAllocator* a)
{
    const wchar_t* args[] = {L"-T", L"lib_6_8"};
    return compile_dxil_core(source, args, static_cast<UINT32>(sizeof(args) / sizeof(args[0])), a);
}

std::unique_ptr<IGpuProgram> make_dx12_program(ShaderStage stage, crd::containers::ConstSpan<crd::u8> cooked_dxil,
                                               crd::memory::IAllocator* alloc)
{
    if (cooked_dxil.size() == 0U) { return nullptr; }
    return std::make_unique<Dx12GpuProgramImpl>(stage, cooked_dxil, alloc);
}

std::unique_ptr<IGpuContext> create_dx12_gpu_context(crd::memory::IAllocator* alloc)
{
    auto ctx = std::make_unique<Dx12GpuContext>(alloc);
    if (!ctx->valid()) { return nullptr; }
    return ctx;
}

} // namespace crd::gpu
