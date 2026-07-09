// backend_dx12.cpp — Phase 3.1.6 v17-d: KirBackendDx12 over raw D3D12 compute + dxc (HLSL → DXIL at runtime). Per
// kernel: emit HLSL → dxc compile to DXIL (cs_6_0, `main`=cs_main) → root signature (root constants + a UAV table) →
// compute PSO → default/upload/readback buffers → copy up → dispatch → copy back → fence wait → map. `precise` HLSL
// ⇒ bit-exact vs the CPU reference for correctly-rounded ops. ADR-0098.

#include <crd/kir/dx12/backend_dx12.hpp>

#include <crd/kir/ckir_hlsl.hpp>

#include <cstring>

#include <d3d12.h>
#include <dxcapi.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

namespace crd::kir
{

using Microsoft::WRL::ComPtr;

struct KirBackendDx12::Impl
{
    crd::memory::IAllocator*           alloc = nullptr;
    ComPtr<ID3D12Device>               device;
    ComPtr<ID3D12CommandQueue>         queue;
    ComPtr<ID3D12CommandAllocator>     cmd_alloc;
    ComPtr<ID3D12GraphicsCommandList>  list;
    ComPtr<ID3D12Fence>                fence;
    ComPtr<IDxcCompiler3>              dxc;
    HANDLE                             event = nullptr;
    crd::u64                           fence_val = 0;
    bool                               ok = false;
};

namespace
{
constexpr int kMaxIn = 32;

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

void transition(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    l->ResourceBarrier(1, &b);
}

// dxc: HLSL (null-terminated `src`) → DXIL blob. false on failure.
bool compile_dxil(IDxcCompiler3* dxc, const char* src, ComPtr<IDxcBlob>& obj)
{
    DxcBuffer buf{};
    buf.Ptr      = src;
    buf.Size     = std::strlen(src);
    buf.Encoding = DXC_CP_UTF8;
    const wchar_t* args[] = {L"-T", L"cs_6_0", L"-E", L"cs_main"};
    ComPtr<IDxcResult> result;
    if (FAILED(dxc->Compile(&buf, args, 4, nullptr, IID_PPV_ARGS(&result)))) { return false; }
    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status)) { return false; }
    return SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) && obj != nullptr;
}

void make_uav(ID3D12Device* dev, ID3D12Resource* res, UINT n_elem, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format                      = DXGI_FORMAT_UNKNOWN;
    uav.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements          = n_elem;
    uav.Buffer.StructureByteStride  = sizeof(float);
    dev->CreateUnorderedAccessView(res, nullptr, &uav, h);
}

// A3: does the graph reachable from `output` use any vector value (comps > 1)? → route to the vec emitter.
[[nodiscard]] bool graph_uses_vec(const KGraph& g, int output, crd::memory::IAllocator* scratch)
{
    const int                       n = g.size();
    crd::containers::Array<crd::u8> reach(scratch);
    reach.resize(static_cast<crd::usize>(n), 0);
    crd::containers::Array<int> stk(scratch);
    stk.push_back(output);
    while (stk.size() > 0)
    {
        const int i = stk[stk.size() - 1];
        stk.resize(stk.size() - 1);
        if (reach[static_cast<crd::usize>(i)]) { continue; }
        reach[static_cast<crd::usize>(i)] = 1;
        const KNode& nd = g.node(i);
        if (nd.comps > 1 || nd.op == KOp::For || nd.op == KOp::LoopIndex || nd.op == KOp::LoopAcc) { return true; } // A4 tier-2: For graphs route to the vec emitter
        if (nd.a >= 0) { stk.push_back(nd.a); }
        if (nd.b >= 0) { stk.push_back(nd.b); }
        if (nd.c >= 0) { stk.push_back(nd.c); }
        if (nd.d >= 0) { stk.push_back(nd.d); }
    }
    return false;
}
} // namespace

KirBackendDx12::KirBackendDx12(crd::memory::IAllocator* alloc) : m_impl(std::make_unique<Impl>())
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
    const HMODULE dxc_dll = LoadLibraryW(L"dxcompiler.dll");
    if (dxc_dll == nullptr) { return; }
    auto create = reinterpret_cast<DxcCreateInstanceProc>(reinterpret_cast<void*>(GetProcAddress(dxc_dll, "DxcCreateInstance")));
    if (create == nullptr || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&impl.dxc)))) { return; }
    impl.ok = impl.event != nullptr;
}

KirBackendDx12::~KirBackendDx12()
{
    if (m_impl->event != nullptr) { CloseHandle(m_impl->event); }
}

bool KirBackendDx12::valid() const noexcept { return m_impl->ok; }

bool KirBackendDx12::run(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out)
{
    auto& impl = *m_impl;
    if (!impl.ok || n_inputs > kMaxIn) { return false; }
    auto*        dev  = impl.device.Get();
    const KNode& outn = g.node(output);

    GlslKernel kern(impl.alloc);
    crd::u64   in_bytes[kMaxIn] = {};
    crd::u64   out_bytes        = 0;
    crd::u32   groups           = 0;
    crd::u32   consts[4]        = {0, 0, 0, 0};

    // FUSION FIRST: activation(GEMM + bias) → ONE fused kernel (epilogue in the store). DX12 inherits the crush.
    const FuseInfo fuse  = detect_fuse(g, output, impl.alloc);
    bool           fused = false;
    if (fuse.ok && emit_contract_fused_hlsl(g, output, fuse.contract, fuse, impl.alloc, kern) && kern.n_inputs == n_inputs)
    {
        const KNode& cn = g.node(fuse.contract);
        const KNode& an = g.node(cn.a);
        const KNode& bn = g.node(cn.b);
        const int    r  = an.shape.rank;
        consts[0]       = static_cast<crd::u32>(an.shape.dims[r - 2]);             // M
        consts[1]       = static_cast<crd::u32>(bn.shape.dims[bn.shape.rank - 1]); // N
        consts[2]       = static_cast<crd::u32>(an.shape.dims[r - 1]);             // K
        in_bytes[0]     = static_cast<crd::u64>(consts[0]) * consts[2] * sizeof(float);
        in_bytes[1]     = static_cast<crd::u64>(consts[2]) * consts[1] * sizeof(float);
        for (int j = 0; j < fuse.n_bias; ++j) { in_bytes[2 + j] = static_cast<crd::u64>(consts[1]) * sizeof(float); }
        out_bytes = static_cast<crd::u64>(consts[0]) * consts[1] * sizeof(float);
        groups    = (consts[0] * consts[1] + 255U) / 256U;
        fused     = true;
    }

    if (fused) {} // fused kernel emitted above
    else if (outn.op == KOp::Contract)
    {
        const KNode& an = g.node(outn.a);
        const KNode& bn = g.node(outn.b);
        const int    r  = an.shape.rank;
        consts[0]       = static_cast<crd::u32>(an.shape.dims[r - 2]);
        consts[1]       = static_cast<crd::u32>(an.shape.dims[r - 1]);
        consts[2]       = static_cast<crd::u32>(bn.shape.dims[bn.shape.rank - 1]);
        consts[3]       = 1U;
        for (int k = 0; k < r - 2; ++k) { consts[3] *= static_cast<crd::u32>(an.shape.dims[k]); }
        in_bytes[0] = static_cast<crd::u64>(consts[0]) * consts[1] * consts[3] * sizeof(float);
        in_bytes[1] = static_cast<crd::u64>(consts[1]) * consts[2] * consts[3] * sizeof(float);
        out_bytes   = static_cast<crd::u64>(consts[0]) * consts[2] * consts[3] * sizeof(float);
        // T2 FAST tiled GEMM (FMA, transposed-A groupshared) — DX12 inherits the crush schedule; else naive precise T1.
        if (outn.tier == DetTier::Fast && consts[3] == 1U && consts[0] % 64U == 0U && consts[2] % 64U == 0U && consts[1] % 8U == 0U
            && emit_contract_fast_hlsl(g, output, kern) && kern.n_inputs == n_inputs)
        {
            groups = (consts[0] / 64U) * (consts[2] / 64U);
        }
        else
        {
            if (!emit_contract_hlsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
            groups = (consts[0] * consts[2] * consts[3] + 255U) / 256U;
        }
    }
    else if (is_reduce(outn.op))
    {
        const bool fast = (outn.tier == DetTier::Fast && is_fast_reduceable(outn.op)); // T2 parallel group tree-reduce
        if (fast) { if (!emit_reduce_fast_hlsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; } }
        else if (!emit_reduce_hlsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 in_numel  = static_cast<crd::u64>(g.node(outn.a).shape.numel());
        const crd::u64 out_numel = static_cast<crd::u64>(outn.shape.numel());
        consts[0]                = static_cast<crd::u32>(out_numel);
        consts[1]                = static_cast<crd::u32>(in_numel / out_numel);
        in_bytes[0]              = in_numel * sizeof(float);
        out_bytes                = out_numel * sizeof(float);
        groups                   = fast ? static_cast<crd::u32>(out_numel) : (static_cast<crd::u32>(out_numel) + 255U) / 256U; // T2: 1 group/output
    }
    else if (outn.op == KOp::Gather)
    {
        if (!emit_gather_hlsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const KNode&   dn         = g.node(outn.a);
        const crd::u64 out_numel  = static_cast<crd::u64>(outn.shape.numel());
        const crd::u64 data_numel = static_cast<crd::u64>(dn.shape.numel());
        const crd::u64 idx_numel  = static_cast<crd::u64>(g.node(outn.b).shape.numel());
        consts[0]                 = static_cast<crd::u32>(out_numel);                                     // nout
        consts[1]                 = static_cast<crd::u32>(data_numel / static_cast<crd::u64>(dn.shape.dims[0])); // rowsize
        in_bytes[0]               = data_numel * sizeof(float);
        in_bytes[1]               = idx_numel * sizeof(float);
        out_bytes                 = out_numel * sizeof(float);
        groups                    = (static_cast<crd::u32>(out_numel) + 255U) / 256U;
    }
    else if (outn.op == KOp::Scatter)
    {
        if (!emit_scatter_hlsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const KNode&   bn         = g.node(outn.a);
        const crd::u64 out_numel  = static_cast<crd::u64>(outn.shape.numel());
        const crd::u64 base_numel = static_cast<crd::u64>(bn.shape.numel());
        const crd::u64 idx_numel  = static_cast<crd::u64>(g.node(outn.b).shape.numel());
        const crd::u64 upd_numel  = static_cast<crd::u64>(g.node(outn.c).shape.numel());
        consts[0]                 = static_cast<crd::u32>(out_numel);                                     // nout
        consts[1]                 = static_cast<crd::u32>(base_numel / static_cast<crd::u64>(bn.shape.dims[0])); // rowsize
        consts[2]                 = static_cast<crd::u32>(idx_numel);                                     // M
        in_bytes[0]               = base_numel * sizeof(float);
        in_bytes[1]               = idx_numel * sizeof(float);
        in_bytes[2]               = upd_numel * sizeof(float);
        out_bytes                 = out_numel * sizeof(float);
        groups                    = (static_cast<crd::u32>(out_numel) + 255U) / 256U;
    }
    else if (outn.op == KOp::ScatterAdd) // atomic histogram: D3D12 zero-inits committed resources, then InterlockedAdd
    {
        if (!emit_scatteradd_hlsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 nin  = static_cast<crd::u64>(g.node(outn.a).shape.numel()); // N inputs
        const crd::u64 mbin = static_cast<crd::u64>(outn.shape.numel());           // M bins (output)
        consts[0]           = static_cast<crd::u32>(nin);
        in_bytes[0]         = nin * sizeof(float);
        in_bytes[1]         = nin * sizeof(float);
        out_bytes           = mbin * sizeof(float);
        groups              = (static_cast<crd::u32>(nin) + 255U) / 256U;
    }
    else if (outn.op == KOp::ScanSum)
    {
        const bool fast = (outn.tier == DetTier::Fast); // T2 parallel group prefix-sum
        if (fast) { if (!emit_scan_fast_hlsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; } }
        else if (!emit_scan_hlsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 numel   = static_cast<crd::u64>(outn.shape.numel());
        const crd::u32 scanlen = static_cast<crd::u32>(outn.shape.dims[outn.shape.rank - 1]);
        consts[0]              = static_cast<crd::u32>(numel / scanlen); // nrows
        consts[1]              = scanlen;
        in_bytes[0]            = numel * sizeof(float);
        out_bytes             = numel * sizeof(float);         // scan KEEPS the shape
        groups                = fast ? consts[0] : (consts[0] + 255U) / 256U; // T2: 1 group/row
    }
    else if (graph_uses_vec(g, output, impl.alloc)) // A3: vector elementwise cone → comps-aware HLSL emitter (interleaved I/O)
    {
        if (!emit_vec_hlsl(g, output, impl.alloc, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 on = static_cast<crd::u64>(outn.shape.numel());
        consts[0]         = static_cast<crd::u32>(on);
        for (int i = 0; i < n_inputs; ++i) { in_bytes[i] = on * static_cast<crd::u64>(kern.in_comps[i]) * sizeof(float); }
        out_bytes = on * static_cast<crd::u64>(kern.out_comps) * sizeof(float);
        groups    = (static_cast<crd::u32>(on) + 255U) / 256U;
    }
    else
    {
        if (!emit_elementwise_hlsl(g, output, impl.alloc, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 on = static_cast<crd::u64>(outn.shape.numel());
        consts[0]         = static_cast<crd::u32>(on);
        for (int i = 0; i < n_inputs; ++i) { in_bytes[i] = on * sizeof(float); }
        out_bytes = on * sizeof(float);
        groups    = (static_cast<crd::u32>(on) + 255U) / 256U;
    }

    ComPtr<IDxcBlob> dxil;
    if (!compile_dxil(impl.dxc.Get(), kern.source.c_str(), dxil)) { return false; }

    // root signature: [0] 32-bit constants (b0, 4 values), [1] UAV descriptor table (u0..u{n_inputs})
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors     = static_cast<UINT>(n_inputs + 1);
    range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 4;
    rp[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges   = &range;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters   = rp;
    ComPtr<ID3DBlob> sig;
    ComPtr<ID3DBlob> serr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &serr))) { return false; }
    ComPtr<ID3D12RootSignature> root;
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root)))) { return false; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature    = root.Get();
    pd.CS.pShaderBytecode = dxil->GetBufferPointer();
    pd.CS.BytecodeLength  = dxil->GetBufferSize();
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) { return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = static_cast<UINT>(n_inputs + 1);
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) { return false; }
    const UINT                  incr = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu  = heap->GetCPUDescriptorHandleForHeapStart();

    ComPtr<ID3D12Resource> in_def[kMaxIn];
    ComPtr<ID3D12Resource> in_up[kMaxIn];
    for (int i = 0; i < n_inputs; ++i)
    {
        in_def[i] = make_buffer(dev, in_bytes[i], D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        in_up[i]  = make_buffer(dev, in_bytes[i], D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (in_def[i] == nullptr || in_up[i] == nullptr) { return false; }
        void* p = nullptr;
        in_up[i]->Map(0, nullptr, &p);
        std::memcpy(p, inputs[kern.input_iidx[i]], in_bytes[i]);
        in_up[i]->Unmap(0, nullptr);
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += static_cast<SIZE_T>(i) * incr;
        make_uav(dev, in_def[i].Get(), static_cast<UINT>(in_bytes[i] / sizeof(float)), h);
    }
    auto out_def = make_buffer(dev, out_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    auto out_rb  = make_buffer(dev, out_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (out_def == nullptr || out_rb == nullptr) { return false; }
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += static_cast<SIZE_T>(n_inputs) * incr;
        make_uav(dev, out_def.Get(), static_cast<UINT>(out_bytes / sizeof(float)), h);
    }

    impl.cmd_alloc->Reset();
    auto* l = impl.list.Get();
    l->Reset(impl.cmd_alloc.Get(), nullptr);
    for (int i = 0; i < n_inputs; ++i)
    {
        transition(l, in_def[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        l->CopyResource(in_def[i].Get(), in_up[i].Get());
        transition(l, in_def[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    transition(l, out_def.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(root.Get());
    l->SetComputeRoot32BitConstants(0, 4, consts, 0);
    l->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    l->SetPipelineState(pso.Get());
    l->Dispatch(groups > 0U ? groups : 1U, 1U, 1U);
    transition(l, out_def.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->CopyResource(out_rb.Get(), out_def.Get());
    l->Close();

    ID3D12CommandList* lists[] = {l};
    impl.queue->ExecuteCommandLists(1, lists);
    ++impl.fence_val;
    impl.queue->Signal(impl.fence.Get(), impl.fence_val);
    if (impl.fence->GetCompletedValue() < impl.fence_val)
    {
        impl.fence->SetEventOnCompletion(impl.fence_val, impl.event);
        WaitForSingleObject(impl.event, INFINITE);
    }

    void*             map_ptr = nullptr;
    const D3D12_RANGE rrange{0, static_cast<SIZE_T>(out_bytes)};
    if (FAILED(out_rb->Map(0, &rrange, &map_ptr))) { return false; }
    std::memcpy(out, map_ptr, out_bytes);
    out_rb->Unmap(0, nullptr);
    return true;
}

} // namespace crd::kir
