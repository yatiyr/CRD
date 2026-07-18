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

    void dispatch(ComputePipeline& pipeline, crd::containers::ConstSpan<ComputeBuffer*> bindings, const void* push,
                  crd::u32 push_size, crd::u32 gx, crd::u32 gy, crd::u32 gz) override
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
        list->Dispatch(gx > 0U ? gx : 1U, gy > 0U ? gy : 1U, gz > 0U ? gz : 1U);
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

    impl.ok = impl.event != nullptr;
}

Dx12ComputeContext::~Dx12ComputeContext()
{
    if (m_impl->event != nullptr) { CloseHandle(m_impl->event); }
}

bool Dx12ComputeContext::valid() const noexcept { return m_impl->ok; }
bool Dx12ComputeContext::supports_shader_int64() const noexcept { return false; } // not queried in this slice

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

std::unique_ptr<ComputePipeline> Dx12ComputeContext::create_pipeline_from_hlsl(crd::containers::StringView hlsl,
                                                                               int n_bindings, crd::u32 push_size)
{
    auto& impl = *m_impl;
    if (!impl.ok || n_bindings <= 0) { return nullptr; }

    const crd::containers::String src(hlsl.data(), hlsl.size(), impl.alloc); // dxc needs a null-terminated buffer
    ComPtr<IDxcBlob>              dxil;
    if (!compile_dxil(impl.dxc.Get(), src.c_str(), dxil)) { return nullptr; }

    // root signature: [0] UAV table u0..u{n-1}; [1] 32-bit root constants at b0 (only when push_size > 0).
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
    if (FAILED(impl.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&pl->root)))) { return nullptr; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature     = pl->root.Get();
    pd.CS.pShaderBytecode = dxil->GetBufferPointer();
    pd.CS.BytecodeLength  = dxil->GetBufferSize();
    if (FAILED(impl.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pl->pso)))) { return nullptr; }
    pl->n_bindings = n_bindings;
    pl->n_consts   = push_size / 4U;
    return pl;
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
