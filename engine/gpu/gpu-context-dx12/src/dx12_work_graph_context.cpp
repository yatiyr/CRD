// dx12_work_graph_context.cpp — CEIR-20c-1: D3D12 Work Graphs offline rig (see the header). Mirrors the
// Dx12RayTracingContext one-shot COMPUTE queue + fence pattern; the delta is the WORK-GRAPH state object (a DXIL node
// library + a global root sig + a D3D12_WORK_GRAPH subobject) and DispatchGraph (the GPU self-schedules the node chain).

#include <crd/gpu/dx12_work_graph_context.hpp>

#include <d3d12.h>
#include <wrl/client.h>

#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace crd::gpu
{
namespace
{
// A committed buffer on `heap`, `bytes` long, in `state` (mirrors the RT context's make_buffer).
ComPtr<ID3D12Resource> make_buffer(ID3D12Device* dev, UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                                   D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = bytes != 0U ? bytes : 4U;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags            = flags;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r))))
    {
        return nullptr;
    }
    return r;
}
void uav_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    list->ResourceBarrier(1, &b);
}
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}
} // namespace

struct Dx12WorkGraphContext::Impl
{
    ComPtr<ID3D12Device9>               device;
    ComPtr<ID3D12CommandQueue>          queue;
    ComPtr<ID3D12CommandAllocator>      cmd_alloc;
    ComPtr<ID3D12GraphicsCommandList10> list;
    ComPtr<ID3D12Fence>                 fence;
    HANDLE                              event     = nullptr;
    UINT64                              fence_val = 0;
    bool                                ok        = false;

    void submit_and_wait()
    {
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        ++fence_val;
        queue->Signal(fence.Get(), fence_val);
        if (fence->GetCompletedValue() < fence_val)
        {
            fence->SetEventOnCompletion(fence_val, event);
            WaitForSingleObject(event, INFINITE);
        }
        cmd_alloc->Reset();
        list->Reset(cmd_alloc.Get(), nullptr);
    }
};

Dx12WorkGraphContext::Dx12WorkGraphContext() : m_impl(std::make_unique<Impl>())
{
    auto&                impl = *m_impl;
    ComPtr<ID3D12Device> dev0;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev0)))) { return; }
    if (FAILED(dev0.As(&impl.device))) { return; } // ID3D12Device9: CreateStateObject(work graph) + OPTIONS21
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 o21{};
    if (FAILED(impl.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &o21, sizeof(o21)))) { return; }
    if (o21.WorkGraphsTier < D3D12_WORK_GRAPHS_TIER_1_0) { return; } // no Work Graphs ⇒ valid() false, caller skips
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (FAILED(impl.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&impl.queue)))) { return; }
    if (FAILED(impl.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&impl.cmd_alloc))))
    {
        return;
    }
    ComPtr<ID3D12GraphicsCommandList> l0;
    if (FAILED(impl.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, impl.cmd_alloc.Get(), nullptr,
                                              IID_PPV_ARGS(&l0))))
    {
        return;
    }
    if (FAILED(l0.As(&impl.list))) { return; } // ID3D12GraphicsCommandList10: SetProgram + DispatchGraph
    if (FAILED(impl.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl.fence)))) { return; }
    impl.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl.ok    = (impl.event != nullptr);
}

Dx12WorkGraphContext::~Dx12WorkGraphContext()
{
    if (m_impl->event != nullptr) { CloseHandle(m_impl->event); }
}

bool Dx12WorkGraphContext::valid() const noexcept { return m_impl->ok; }

bool Dx12WorkGraphContext::dispatch_graph(crd::containers::ConstSpan<crd::u8> node_dxil, const char* program_name,
                                          crd::containers::ConstSpan<Binding> bindings)
{
    auto& impl = *m_impl;
    if (!impl.ok || node_dxil.size() == 0U || program_name == nullptr || bindings.size() == 0U) { return false; }
    const std::wstring wprog(program_name, program_name + std::strlen(program_name));

    // ── global root signature: one root UAV descriptor per binding (register u<reg>) ──
    std::vector<D3D12_ROOT_PARAMETER1> params(bindings.size());
    for (crd::u32 i = 0; i < static_cast<crd::u32>(bindings.size()); ++i)
    {
        params[i]                             = {};
        params[i].ParameterType               = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[i].Descriptor.ShaderRegister   = bindings[i].reg;
        params[i].Descriptor.RegisterSpace    = 0U;
        params[i].ShaderVisibility            = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
    rsd.Version                = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rsd.Desc_1_1.NumParameters = static_cast<UINT>(params.size());
    rsd.Desc_1_1.pParameters   = params.data();
    ComPtr<ID3DBlob> rs_blob;
    ComPtr<ID3DBlob> rs_err;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rsd, &rs_blob, &rs_err))) { return false; }
    ComPtr<ID3D12RootSignature> root_sig;
    if (FAILED(impl.device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                                IID_PPV_ARGS(&root_sig))))
    {
        return false;
    }

    // ── work-graph state object: DXIL node library + the global root sig + a WORK_GRAPH subobject (all nodes) ──
    D3D12_DXIL_LIBRARY_DESC lib{};
    lib.DXILLibrary.pShaderBytecode = node_dxil.data();
    lib.DXILLibrary.BytecodeLength  = node_dxil.size();
    D3D12_GLOBAL_ROOT_SIGNATURE grs{};
    grs.pGlobalRootSignature = root_sig.Get();
    D3D12_WORK_GRAPH_DESC wg{};
    wg.ProgramName = wprog.c_str();
    wg.Flags       = D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
    D3D12_STATE_SUBOBJECT sub[3]{};
    sub[0].Type  = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    sub[0].pDesc = &lib;
    sub[1].Type  = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    sub[1].pDesc = &grs;
    sub[2].Type  = D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH;
    sub[2].pDesc = &wg;
    D3D12_STATE_OBJECT_DESC sod{};
    sod.Type          = D3D12_STATE_OBJECT_TYPE_EXECUTABLE;
    sod.NumSubobjects = 3U;
    sod.pSubobjects   = sub;
    ComPtr<ID3D12StateObject> state;
    if (FAILED(impl.device->CreateStateObject(&sod, IID_PPV_ARGS(&state)))) { return false; }

    ComPtr<ID3D12StateObjectProperties1> props1;
    ComPtr<ID3D12WorkGraphProperties>    wgprops;
    if (FAILED(state.As(&props1)) || FAILED(state.As(&wgprops))) { return false; }
    const D3D12_PROGRAM_IDENTIFIER prog_id = props1->GetProgramIdentifier(wprog.c_str());
    const UINT                     wg_index = wgprops->GetWorkGraphIndex(wprog.c_str());
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS mem{};
    wgprops->GetWorkGraphMemoryRequirements(wg_index, &mem);
    ComPtr<ID3D12Resource> backing;
    if (mem.MaxSizeInBytes > 0U)
    {
        backing = make_buffer(impl.device.Get(), mem.MaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (backing == nullptr) { return false; }
    }

    // ── device UAV buffers (+ UPLOAD staging: memcpy the input, or zeros, then copy in) ──
    const crd::u32                      nb = static_cast<crd::u32>(bindings.size());
    std::vector<ComPtr<ID3D12Resource>> dev_bufs(nb);
    std::vector<ComPtr<ID3D12Resource>> up_bufs(nb);
    std::vector<ComPtr<ID3D12Resource>> rb_bufs(nb);
    for (crd::u32 i = 0; i < nb; ++i)
    {
        const UINT64 bytes = bindings[i].bytes != 0U ? bindings[i].bytes : 4U;
        dev_bufs[i] = make_buffer(impl.device.Get(), bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
        up_bufs[i]  = make_buffer(impl.device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
        if (dev_bufs[i] == nullptr || up_bufs[i] == nullptr) { return false; }
        void* p = nullptr;
        up_bufs[i]->Map(0, nullptr, &p);
        if (bindings[i].upload != nullptr) { std::memcpy(p, bindings[i].upload, static_cast<size_t>(bindings[i].bytes)); }
        else { std::memset(p, 0, static_cast<size_t>(bytes)); } // zero-init (the output counter starts at 0)
        up_bufs[i]->Unmap(0, nullptr);
        impl.list->CopyResource(dev_bufs[i].Get(), up_bufs[i].Get());
        transition(impl.list.Get(), dev_bufs[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // ── set the graph program (INITIALIZE the backing store) + bind UAVs + DispatchGraph the entry with one empty record ──
    impl.list->SetComputeRootSignature(root_sig.Get());
    for (crd::u32 i = 0; i < nb; ++i)
    {
        impl.list->SetComputeRootUnorderedAccessView(i, dev_bufs[i]->GetGPUVirtualAddress());
    }
    D3D12_SET_PROGRAM_DESC spd{};
    spd.Type                                = D3D12_PROGRAM_TYPE_WORK_GRAPH;
    spd.WorkGraph.ProgramIdentifier         = prog_id;
    spd.WorkGraph.Flags                     = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
    spd.WorkGraph.BackingMemory.StartAddress = backing != nullptr ? backing->GetGPUVirtualAddress() : 0U;
    spd.WorkGraph.BackingMemory.SizeInBytes  = mem.MaxSizeInBytes;
    impl.list->SetProgram(&spd);

    D3D12_DISPATCH_GRAPH_DESC dgd{};
    dgd.Mode                          = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
    dgd.NodeCPUInput.EntrypointIndex  = 0U; // the single [NodeIsProgramEntry] producer
    dgd.NodeCPUInput.NumRecords       = 1U; // one empty entry record (the producer has no input payload)
    dgd.NodeCPUInput.pRecords         = nullptr;
    dgd.NodeCPUInput.RecordStrideInBytes = 0U;
    impl.list->DispatchGraph(&dgd);

    // ── readback: UAV -> COPY_SOURCE -> a READBACK buffer, still in this one-shot list ──
    for (crd::u32 i = 0; i < nb; ++i)
    {
        if (bindings[i].readback == nullptr) { continue; }
        uav_barrier(impl.list.Get(), dev_bufs[i].Get());
        transition(impl.list.Get(), dev_bufs[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        rb_bufs[i] = make_buffer(impl.device.Get(), bindings[i].bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                 D3D12_RESOURCE_STATE_COPY_DEST);
        if (rb_bufs[i] == nullptr) { return false; }
        impl.list->CopyResource(rb_bufs[i].Get(), dev_bufs[i].Get());
    }
    impl.submit_and_wait();
    for (crd::u32 i = 0; i < nb; ++i)
    {
        if (bindings[i].readback == nullptr) { continue; }
        void*             p     = nullptr;
        const D3D12_RANGE range = {0, static_cast<SIZE_T>(bindings[i].bytes)};
        rb_bufs[i]->Map(0, &range, &p);
        std::memcpy(bindings[i].readback, p, static_cast<size_t>(bindings[i].bytes));
        rb_bufs[i]->Unmap(0, nullptr);
    }
    return true;
}
} // namespace crd::gpu
