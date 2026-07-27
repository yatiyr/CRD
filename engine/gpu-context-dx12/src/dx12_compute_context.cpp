// dx12_compute_context.cpp — the D3D12 implementation of crd::gpu::IComputeContext (ADR-0100). Device + dedicated compute
// queue + dxc; per-buffer explicit-state tracking (D3D12 has no implicit access model); a shader-visible UAV heap
// sub-allocated per dispatch; RAW (ByteAddressBuffer) UAVs so no element-type is baked into the interface. Mirrors the
// Vulkan backend's recorder shape (copy / barrier / dispatch → submit_and_wait). D3D12 helpers mirror crd-kir-dx12.

#include <crd/gpu/dx12_compute_context.hpp>

#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

#include <cstring>

#include <d3d12.h>
#include <dxcapi.h>
#include <windows.h>
#include <wrl/client.h>

namespace crd::gpu
{

using Microsoft::WRL::ComPtr;

namespace
{
constexpr crd::u32 kHeapDescriptors = 8192U; // UAV descriptors per begin() window (sub-allocated per dispatch)

D3D12_RESOURCE_STATES access_state(ComputeAccess a) noexcept
{
    switch (a)
    {
    case ComputeAccess::TransferSrc: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case ComputeAccess::TransferDst: return D3D12_RESOURCE_STATE_COPY_DEST;
    case ComputeAccess::ShaderRead:
    case ComputeAccess::ShaderWrite: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS; // UAV covers both for compute
    case ComputeAccess::HostRead:    return D3D12_RESOURCE_STATE_COPY_SOURCE; // readback is a copy to a READBACK buffer
    case ComputeAccess::IndirectRead: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT; // C5: args buffer read by ExecuteIndirect
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

ComPtr<ID3D12Resource> make_buffer(ID3D12Device* dev, UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = size;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags            = flags;
    ComPtr<ID3D12Resource> res;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&res));
    return res;
}

bool compile_dxil(IDxcCompiler3* dxc, const char* src, ComPtr<IDxcBlob>& obj)
{
    DxcBuffer buf{};
    buf.Ptr               = src;
    buf.Size              = std::strlen(src);
    buf.Encoding          = DXC_CP_UTF8;
    const wchar_t* args[] = {L"-T", L"cs_6_0", L"-E", L"cs_main"};
    ComPtr<IDxcResult>    result;
    if (FAILED(dxc->Compile(&buf, args, 4, nullptr, IID_PPV_ARGS(&result)))) { return false; }
    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status)) { return false; }
    return SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) && obj != nullptr;
}
} // namespace

// ── buffer ────────────────────────────────────────────────────────────────────────────────────────────────────────
struct BufferImpl final : ComputeBuffer
{
    ComPtr<ID3D12Resource> res;
    crd::u64               bytes = 0;
    D3D12_RESOURCE_STATES  state = D3D12_RESOURCE_STATE_COMMON; // current (tracked; only DEFAULT buffers transition)
    bool                   fixed = false;                       // UPLOAD/READBACK stay in their creation state

    [[nodiscard]] void* map() noexcept override
    {
        void* p = nullptr;
        res->Map(0, nullptr, &p);
        return p;
    }
    void unmap() noexcept override { res->Unmap(0, nullptr); }
    // B4: the native ID3D12Resource*, so a compute-written INDIRECT-args buffer can drive the raster context's ExecuteIndirect
    // mesh dispatch (a buffer decays to COMMON after the compute submit, so the raster context can transition it freely).
    [[nodiscard]] void* native_handle() const noexcept override { return res.Get(); }
};

// ── pipeline ──────────────────────────────────────────────────────────────────────────────────────────────────────
struct PipelineImpl final : ComputePipeline
{
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    int                         n_bindings = 0;
    UINT                        n_consts   = 0; // push_size / 4
};

// ── context + recorder (the Impl IS the recorder, like the Vulkan backend) ──────────────────────────────────────────
struct Dx12ComputeContext::Impl final : ComputeRecorder
{
    crd::memory::IAllocator*          alloc = nullptr;
    ComPtr<ID3D12Device>              device;
    ComPtr<ID3D12CommandQueue>        queue;
    ComPtr<ID3D12CommandAllocator>    cmd_alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence>               fence;
    ComPtr<IDxcCompiler3>             dxc;
    ComPtr<ID3D12DescriptorHeap>      heap; // shader-visible CBV_SRV_UAV
    ComPtr<ID3D12CommandSignature>    dispatch_sig; // C5: lazily-created DISPATCH indirect command signature (ExecuteIndirect)
    ComPtr<ID3D12Device1>             device1;       // D4: for the pipeline library (the PSO cache)
    ComPtr<ID3D12PipelineLibrary>     pipe_lib;      // D4: persistent PSO cache across runs (null ⇒ uncached fallback)
    crd::containers::Array<crd::u8>   lib_blob;      // D4: owns the warm-start blob — CreatePipelineLibrary does NOT copy it
    UINT                              heap_incr = 0;
    UINT                              heap_next = 0; // sub-alloc cursor, reset each begin()
    HANDLE                            event     = nullptr;
    crd::u64                          fence_val = 0;
    bool                              ok        = false;

    void ensure_state(BufferImpl& b, D3D12_RESOURCE_STATES want)
    {
        if (b.fixed || b.state == want) { return; }
        D3D12_RESOURCE_BARRIER t{};
        t.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource   = b.res.Get();
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        t.Transition.StateBefore = b.state;
        t.Transition.StateAfter  = want;
        list->ResourceBarrier(1, &t);
        b.state = want;
    }

    void copy(ComputeBuffer& src, ComputeBuffer& dst, crd::u64 src_off, crd::u64 dst_off, crd::u64 bytes) override
    {
        auto& s = static_cast<BufferImpl&>(src);
        auto& d = static_cast<BufferImpl&>(dst);
        ensure_state(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ensure_state(d, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(d.res.Get(), dst_off, s.res.Get(), src_off, bytes);
    }

    void barrier(ComputeBuffer& buf, ComputeAccess /*from*/, ComputeAccess to) override
    {
        auto&      b    = static_cast<BufferImpl&>(buf);
        const auto want = access_state(to);
        if (b.fixed) { return; }
        if (b.state == want)
        {
            if (want == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) // WAW/RAW between dispatches → UAV barrier
            {
                D3D12_RESOURCE_BARRIER ub{};
                ub.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                ub.UAV.pResource = b.res.Get();
                list->ResourceBarrier(1, &ub);
            }
            return;
        }
        ensure_state(b, want);
    }

    // Bind the pipeline + UAV descriptor table + push constants for a (direct or indirect) dispatch. Shared by dispatch/_indirect.
    void bind_compute(ComputePipeline& pipeline, crd::containers::ConstSpan<ComputeBuffer*> bindings, const void* push, crd::u32 push_size)
    {
        auto&                       p   = static_cast<PipelineImpl&>(pipeline);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(heap_next) * heap_incr;
        gpu.ptr += static_cast<UINT64>(heap_next) * heap_incr;
        for (crd::u64 i = 0; i < bindings.size(); ++i)
        {
            auto& b = static_cast<BufferImpl&>(*bindings[i]);
            ensure_state(b, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format              = DXGI_FORMAT_R32_TYPELESS;
            uav.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements  = static_cast<UINT>(b.bytes / 4U);
            uav.Buffer.Flags        = D3D12_BUFFER_UAV_FLAG_RAW;
            D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
            h.ptr += static_cast<SIZE_T>(i) * heap_incr;
            device->CreateUnorderedAccessView(b.res.Get(), nullptr, &uav, h);
        }
        heap_next += static_cast<UINT>(bindings.size());

        list->SetComputeRootSignature(p.root.Get());
        list->SetComputeRootDescriptorTable(0, gpu);
        if (push_size > 0U && push != nullptr) { list->SetComputeRoot32BitConstants(1, push_size / 4U, push, 0); }
        list->SetPipelineState(p.pso.Get());
    }

    void dispatch(ComputePipeline& pipeline, crd::containers::ConstSpan<ComputeBuffer*> bindings, const void* push,
                  crd::u32 push_size, crd::u32 gx, crd::u32 gy, crd::u32 gz) override
    {
        bind_compute(pipeline, bindings, push, push_size);
        list->Dispatch(gx > 0U ? gx : 1U, gy > 0U ? gy : 1U, gz > 0U ? gz : 1U);
    }

    // C5: the workgroup count is read from `args` (a compute-written buffer, 3 u32 {x,y,z} at `args_offset`) via ExecuteIndirect
    // — the DX12 GPU-driven dispatch. The DISPATCH command signature is device-level (no root sig), created once + cached.
    void dispatch_indirect(ComputePipeline& pipeline, crd::containers::ConstSpan<ComputeBuffer*> bindings, const void* push,
                           crd::u32 push_size, ComputeBuffer& args, crd::u64 args_offset) override
    {
        if (!dispatch_sig)
        {
            D3D12_INDIRECT_ARGUMENT_DESC arg{};
            arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
            D3D12_COMMAND_SIGNATURE_DESC csd{};
            csd.ByteStride       = 3U * sizeof(crd::u32);
            csd.NumArgumentDescs = 1U;
            csd.pArgumentDescs   = &arg;
            device->CreateCommandSignature(&csd, nullptr, IID_PPV_ARGS(&dispatch_sig));
        }
        bind_compute(pipeline, bindings, push, push_size);
        auto& ab = static_cast<BufferImpl&>(args);
        ensure_state(ab, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        list->ExecuteIndirect(dispatch_sig.Get(), 1U, ab.res.Get(), static_cast<UINT64>(args_offset), nullptr, 0U);
    }
};

Dx12ComputeContext::Dx12ComputeContext(crd::memory::IAllocator* alloc) : m_impl(std::make_unique<Impl>())
{
    auto& impl = *m_impl;
    impl.alloc = alloc;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&impl.device)))) { return; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (FAILED(impl.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&impl.queue)))) { return; }
    if (FAILED(impl.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&impl.cmd_alloc)))) { return; }
    if (FAILED(impl.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, impl.cmd_alloc.Get(), nullptr, IID_PPV_ARGS(&impl.list)))) { return; }
    impl.list->Close();
    if (FAILED(impl.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl.fence)))) { return; }
    impl.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kHeapDescriptors;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(impl.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&impl.heap)))) { return; }
    impl.heap_incr = impl.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    const HMODULE dxc_dll = LoadLibraryW(L"dxcompiler.dll");
    if (dxc_dll == nullptr) { return; }
    // NOLINTNEXTLINE(clang-diagnostic-cast-function-type-strict) — GetProcAddress returns FARPROC; a direct
    // reinterpret_cast to the real proc type is the standard Win32 idiom (no void* hop, per bugprone-casting-through-void).
    auto* create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxc_dll, "DxcCreateInstance"));
    if (create == nullptr || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&impl.dxc)))) { return; }

    // D4: the PSO cache — an empty ID3D12PipelineLibrary (best-effort; unsupported ⇒ pipe_lib stays null and we create PSOs directly).
    if (SUCCEEDED(impl.device.As(&impl.device1)))
    {
        (void)impl.device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&impl.pipe_lib));
    }

    impl.ok = impl.event != nullptr;
}

Dx12ComputeContext::~Dx12ComputeContext()
{
    if (m_impl->event != nullptr) { CloseHandle(m_impl->event); }
}

bool Dx12ComputeContext::valid() const noexcept { return m_impl->ok; }
bool Dx12ComputeContext::supports_shader_int64() const noexcept { return false; } // not queried in this slice
// REN-38: the device truths the warp-synchronous kernels are built against. D3D12 pins compute-shader TGSM at
// 32 KB by spec; the wave width comes from OPTIONS1 (WaveLaneCountMin — the width WaveGetLaneCount() delivers
// on every current adapter; NV/Intel 32, AMD RDNA 32).
crd::u32 Dx12ComputeContext::subgroup_size() const noexcept
{
    if (m_impl->device == nullptr) { return 0U; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    if (FAILED(m_impl->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1)))) { return 0U; }
    return o1.WaveOps != FALSE ? o1.WaveLaneCountMin : 0U;
}
crd::u32 Dx12ComputeContext::shared_memory_bytes() const noexcept { return 32768U; } // D3D12 CS TGSM spec limit

// D4: serialize the pipeline library (the PSO cache) so it can be persisted to disk.
void Dx12ComputeContext::pipeline_cache_data(crd::containers::Array<crd::u8>& out) const
{
    auto& impl = *m_impl;
    out.resize(0);
    if (!impl.pipe_lib) { return; }
    const SIZE_T sz = impl.pipe_lib->GetSerializedSize();
    if (sz == 0) { return; }
    out.resize(sz);
    if (FAILED(impl.pipe_lib->Serialize(out.data(), sz))) { out.resize(0); }
}

// D4: reseed the pipeline library from a persisted blob — call BEFORE creating pipelines for a warm start. The blob is COPIED
// into lib_blob because CreatePipelineLibrary references it for the library's lifetime. A blob from another driver/adapter is
// rejected (version/adapter mismatch) → fall back to an empty library so creation still works.
bool Dx12ComputeContext::warm_pipeline_cache(crd::containers::ConstSpan<crd::u8> blob)
{
    auto& impl = *m_impl;
    if (!impl.device1) { return false; }
    impl.lib_blob.resize(0);
    for (crd::usize i = 0; i < blob.size(); ++i) { impl.lib_blob.push_back(blob[i]); }
    ComPtr<ID3D12PipelineLibrary> fresh;
    const bool seeded = !impl.lib_blob.empty()
        && SUCCEEDED(impl.device1->CreatePipelineLibrary(impl.lib_blob.data(), impl.lib_blob.size(), IID_PPV_ARGS(&fresh)));
    if (!seeded && FAILED(impl.device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&fresh)))) { return false; }
    impl.pipe_lib = fresh;
    return true;
}

std::unique_ptr<ComputeBuffer> Dx12ComputeContext::create_buffer(crd::u64 bytes, crd::u32 /*usage*/, ComputeMemory memory)
{
    auto& impl = *m_impl;
    if (!impl.ok || bytes == 0U) { return nullptr; }
    auto b   = std::make_unique<BufferImpl>();
    b->bytes = bytes;
    switch (memory)
    {
    case ComputeMemory::GpuOnly:
        b->res   = make_buffer(impl.device.Get(), bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        b->state = D3D12_RESOURCE_STATE_COMMON;
        b->fixed = false;
        break;
    case ComputeMemory::CpuToGpu:
        b->res   = make_buffer(impl.device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        b->state = D3D12_RESOURCE_STATE_GENERIC_READ; // ⊇ COPY_SOURCE, so a copy-up needs no transition
        b->fixed = true;
        break;
    case ComputeMemory::GpuToCpu:
        b->res   = make_buffer(impl.device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        b->state = D3D12_RESOURCE_STATE_COPY_DEST;
        b->fixed = true;
        break;
    }
    if (b->res == nullptr) { return nullptr; }
    return b;
}

// Build the root signature + compute PSO from a DXIL byte range. Shared by the HLSL (compile-then-build) and the pre-compiled
// DXIL (D2 cooked-bundle load) entry points — root layout: [0] UAV table u0..u{n-1}; [1] 32-bit root constants at b0 (push>0).
static std::unique_ptr<ComputePipeline> build_dxil_pipeline(ID3D12Device* device, ID3D12PipelineLibrary* lib, const void* code,
                                                            SIZE_T len, int n_bindings, crd::u32 push_size)
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors     = static_cast<UINT>(n_bindings);
    range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges   = &range;
    UINT nparams = 1;
    if (push_size > 0U)
    {
        rp[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[1].Constants.Num32BitValues = push_size / 4U;
        rp[1].Constants.ShaderRegister = 0;
        nparams                        = 2;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = nparams;
    rsd.pParameters   = rp;
    ComPtr<ID3DBlob> sig;
    ComPtr<ID3DBlob> serr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &serr))) { return nullptr; }
    auto pl = std::make_unique<PipelineImpl>();
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&pl->root)))) { return nullptr; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature     = pl->root.Get();
    pd.CS.pShaderBytecode = code;
    pd.CS.BytecodeLength  = len;

    // D4: create the PSO via the pipeline-library cache when available — LoadComputePipeline hits the cached ISA (a warm run),
    // a miss creates + stores it. The PSO name is an FNV-1a hash of the DXIL + layout, so identical shaders share a cache slot
    // across runs. No library ⇒ a plain CreateComputePipelineState.
    bool made = false;
    if (lib != nullptr)
    {
        crd::u64    h = 1469598103934665603ULL;
        const auto* p = static_cast<const crd::u8*>(code);
        for (SIZE_T i = 0; i < len; ++i) { h = (h ^ p[i]) * 1099511628211ULL; }
        h = (h ^ static_cast<crd::u64>(n_bindings)) * 1099511628211ULL;
        h = (h ^ push_size) * 1099511628211ULL;
        wchar_t name[24];
        swprintf(name, 24, L"p%016llx", static_cast<unsigned long long>(h));
        if (SUCCEEDED(lib->LoadComputePipeline(name, &pd, IID_PPV_ARGS(&pl->pso)))) { made = true; }
        else if (SUCCEEDED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pl->pso)))) { (void)lib->StorePipeline(name, pl->pso.Get()); made = true; }
    }
    if (!made && FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pl->pso)))) { return nullptr; }
    pl->n_bindings = n_bindings;
    pl->n_consts   = push_size / 4U;
    return pl;
}

std::unique_ptr<ComputePipeline> Dx12ComputeContext::create_pipeline_from_hlsl(crd::containers::StringView hlsl,
                                                                               int n_bindings, crd::u32 push_size)
{
    auto& impl = *m_impl;
    if (!impl.ok || n_bindings <= 0) { return nullptr; }

    const crd::containers::String src(hlsl.data(), hlsl.size(), impl.alloc); // dxc needs a null-terminated buffer
    ComPtr<IDxcBlob>              dxil;
    if (!compile_dxil(impl.dxc.Get(), src.c_str(), dxil)) { return nullptr; }
    return build_dxil_pipeline(impl.device.Get(), impl.pipe_lib.Get(), dxil->GetBufferPointer(), dxil->GetBufferSize(), n_bindings, push_size);
}

std::unique_ptr<ComputePipeline> Dx12ComputeContext::create_pipeline_from_dxil(crd::containers::ConstSpan<crd::u8> dxil,
                                                                               int n_bindings, crd::u32 push_size)
{
    auto& impl = *m_impl;
    if (!impl.ok || n_bindings <= 0 || dxil.empty()) { return nullptr; }
    return build_dxil_pipeline(impl.device.Get(), impl.pipe_lib.Get(), dxil.data(), static_cast<SIZE_T>(dxil.size()), n_bindings, push_size);
}

std::unique_ptr<ComputePipeline> Dx12ComputeContext::create_pipeline(crd::containers::StringView /*shader_dir*/,
                                                                     crd::containers::StringView /*name*/,
                                                                     int /*n_bindings*/, crd::u32 /*push_size*/)
{
    // The by-name (cooked-kernel) path is a follow-up: it needs HLSL/DXIL versions of the geometry kernels, which do not
    // exist yet. Runtime-HLSL callers (CKIR, tests) use create_pipeline_from_hlsl. Returning null keeps that honest.
    return nullptr;
}

ComputeRecorder& Dx12ComputeContext::begin()
{
    auto& impl = *m_impl;
    impl.cmd_alloc->Reset();
    impl.list->Reset(impl.cmd_alloc.Get(), nullptr);
    ID3D12DescriptorHeap* heaps[] = {impl.heap.Get()};
    impl.list->SetDescriptorHeaps(1, heaps);
    impl.heap_next = 0;
    return impl;
}

void Dx12ComputeContext::submit_and_wait()
{
    auto& impl = *m_impl;
    impl.list->Close();
    ID3D12CommandList* lists[] = {impl.list.Get()};
    impl.queue->ExecuteCommandLists(1, lists);
    ++impl.fence_val;
    impl.queue->Signal(impl.fence.Get(), impl.fence_val);
    if (impl.fence->GetCompletedValue() < impl.fence_val)
    {
        impl.fence->SetEventOnCompletion(impl.fence_val, impl.event);
        WaitForSingleObject(impl.event, INFINITE);
    }
}

} // namespace crd::gpu
