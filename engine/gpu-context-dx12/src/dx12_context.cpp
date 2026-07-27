// dx12_context.cpp — the D3D12 IGpuContext + the DX12 program-authoring seam (ADR-0099/0103 / D-008 C4-b). The DX12
// mirror of vulkan_context.cpp: mint Dx12GpuPrograms (cooked DXIL) from bytecode or from a CKIR graph (crd-kir emits
// HLSL, dxc lowers it to DXIL). The HLSL text and the DXIL bytes never escape this backend (I1/I2). A held D3D12 device
// gives the context an adapter identity + honours "a context is a live device foundation" (ADR-0099); DXIL itself is
// device-independent, so program authoring needs only dxc.

#include <crd/gpu/dx12_context.hpp>

#include <crd/kir/ckir_hlsl.hpp> // emit_stage_hlsl (+ ckir.hpp: KGraph/KEntry/KStage, and ckir_glsl.hpp: GlslKernel)

#include <cstring>

#include <d3d12.h>
#include <dxcapi.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

namespace crd::gpu
{

using Microsoft::WRL::ComPtr;

namespace
{

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
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)))) { return; }
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
        else if (!crd::kir::emit_stage_hlsl(graph, entry, m_alloc, kern)) { return nullptr; }
        const auto dxil = compile_hlsl_to_dxil(stage, crd::containers::to_view(kern.source), "ckir_stage", m_alloc);
        if (!dxil.ok) { return nullptr; }

        // B1-f: does this fragment program read SV_InnerCoverage? If so its PSO must be conservative (the raster context
        // prebuilds it conservative — the D3D12 rasterizer rejects SV_InnerCoverage with conservative OFF, and the failed
        // non-conservative PSO build is a debug-layer error). Detect the InnerCoverage builtin in the graph.
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

    crd::memory::IAllocator* m_alloc = nullptr;
    ComPtr<ID3D12Device>     m_device;
    char                     m_adapter[192] = {};
    bool                     m_ok           = false;
};

} // namespace

DxilCompileResult compile_hlsl_to_dxil(ShaderStage stage, crd::containers::StringView source,
                                       crd::containers::StringView /*name*/, crd::memory::IAllocator* a)
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
    const wchar_t* args[] = {L"-T", dxil_profile(stage), L"-E", dxil_entry(stage)}; // no -spirv ⇒ signed DXIL

    ComPtr<IDxcResult> dxc_result;
    if (FAILED(compiler->Compile(&src, args, static_cast<UINT32>(sizeof(args) / sizeof(args[0])), nullptr,
                                 IID_PPV_ARGS(&dxc_result))))
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
