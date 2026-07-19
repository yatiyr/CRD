// dx12_ray_tracing_context.cpp — D-007 C3/RT: DXR acceleration-structure build (BLAS/TLAS via ID3D12Device5 /
// ID3D12GraphicsCommandList4) + inline ray-query (DXR 1.1) compute dispatch. The DX12 half of the C3↔B9 RT pair; see the header.
// The HLSL the CKIR RT kernels emit binds the TLAS at `register(t0)` (a root SRV) and each storage buffer at `register(uN)`
// (a RAW UAV) — the root signature here matches exactly.

#include <crd/gpu/dx12_ray_tracing_context.hpp>

#include <crd/core/types.hpp>

#include <cstring>

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

namespace crd::gpu
{

using Microsoft::WRL::ComPtr;

namespace
{
// A committed buffer of `size` bytes on `heap` in initial `state`, with `flags` (ALLOW_UNORDERED_ACCESS for UAV/AS backing).
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
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags            = flags;
    ComPtr<ID3D12Resource> res;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&res));
    return res;
}

// The built scene keeps its BLAS + TLAS result buffers (and the TLAS device address the root SRV binds).
struct SceneImpl final : Dx12RtScene
{
    ComPtr<ID3D12Resource>    blas;
    ComPtr<ID3D12Resource>    tlas;
    D3D12_GPU_VIRTUAL_ADDRESS tlas_va = 0;
};

void barrier(ID3D12GraphicsCommandList4* list, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}
} // namespace

struct Dx12RayTracingContext::Impl
{
    ComPtr<ID3D12Device5>              device;
    ComPtr<ID3D12CommandQueue>         queue;
    ComPtr<ID3D12CommandAllocator>     cmd_alloc;
    ComPtr<ID3D12GraphicsCommandList4> list;
    ComPtr<ID3D12Fence>                fence;
    HANDLE                             event     = nullptr;
    UINT64                             fence_val = 0;
    bool                               ok        = false;

    // Close + execute the recorded list, block for the GPU, then reopen it (the AS build and the trace dispatch are one-shot).
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

Dx12RayTracingContext::Dx12RayTracingContext() : m_impl(std::make_unique<Impl>())
{
    auto&                  impl = *m_impl;
    ComPtr<ID3D12Device>   dev0;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev0)))) { return; }
    if (FAILED(dev0.As(&impl.device))) { return; } // ID3D12Device5 carries the DXR entry points
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
    if (FAILED(impl.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5)))) { return; }
    if (opt5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) { return; } // inline RayQuery in compute needs tier 1.1
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (FAILED(impl.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&impl.queue)))) { return; }
    if (FAILED(impl.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&impl.cmd_alloc)))) { return; }
    ComPtr<ID3D12GraphicsCommandList> l0;
    if (FAILED(impl.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, impl.cmd_alloc.Get(), nullptr, IID_PPV_ARGS(&l0)))) { return; }
    if (FAILED(l0.As(&impl.list))) { return; } // ID3D12GraphicsCommandList4 carries BuildRaytracingAccelerationStructure
    if (FAILED(impl.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl.fence)))) { return; }
    impl.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl.ok    = (impl.event != nullptr);
}

Dx12RayTracingContext::~Dx12RayTracingContext()
{
    if (m_impl->event != nullptr) { CloseHandle(m_impl->event); }
}

bool Dx12RayTracingContext::valid() const noexcept { return m_impl->ok; }

RtCapabilities Dx12RayTracingContext::capabilities() const noexcept
{
    RtCapabilities c;
    c.set(RtFeature::InlineQuery, m_impl->ok); // inline ray query is the only RT path wired on DX12 today
    return c;
}

std::unique_ptr<Dx12RtScene> Dx12RayTracingContext::build_scene(const float* vertices, crd::u32 ntris)
{
    const float identity[12] = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    return build_scene_instanced(vertices, ntris, identity, 1U);
}

std::unique_ptr<Dx12RtScene> Dx12RayTracingContext::build_scene_instanced(const float* vertices, crd::u32 ntris,
                                                                          const float* transforms, crd::u32 ninst)
{
    auto& impl = *m_impl;
    if (!impl.ok || ntris == 0 || ninst == 0) { return nullptr; }
    const crd::u32 nverts = ntris * 3U;
    auto           scene  = std::make_unique<SceneImpl>();

    // vertex buffer (UPLOAD heap ⇒ a GPU virtual address the BLAS build reads directly). Local ComPtrs stay alive through the
    // blocking submit_and_wait at the end, so the GPU sees them during the build.
    ComPtr<ID3D12Resource> vbuf = make_buffer(impl.device.Get(), static_cast<UINT64>(nverts) * 3U * sizeof(float),
                                              D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (vbuf == nullptr) { return nullptr; }
    void* vp = nullptr;
    vbuf->Map(0, nullptr, &vp);
    std::memcpy(vp, vertices, static_cast<size_t>(nverts) * 3U * sizeof(float));
    vbuf->Unmap(0, nullptr);

    // ── BLAS from the triangle soup (indexless: 3 verts per triangle in order) ──
    D3D12_RAYTRACING_GEOMETRY_DESC geom{};
    geom.Type                                 = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags                                = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexBuffer.StartAddress  = vbuf->GetGPUVirtualAddress();
    geom.Triangles.VertexBuffer.StrideInBytes = 3U * sizeof(float);
    geom.Triangles.VertexCount                = nverts;
    geom.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.IndexFormat                = DXGI_FORMAT_UNKNOWN;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS binputs{};
    binputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    binputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    binputs.NumDescs       = 1;
    binputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    binputs.pGeometryDescs = &geom;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bpre{};
    impl.device->GetRaytracingAccelerationStructurePrebuildInfo(&binputs, &bpre);
    ComPtr<ID3D12Resource> bscratch = make_buffer(impl.device.Get(), bpre.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    scene->blas = make_buffer(impl.device.Get(), bpre.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    if (bscratch == nullptr || scene->blas == nullptr) { return nullptr; }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bbuild{};
    bbuild.Inputs                           = binputs;
    bbuild.ScratchAccelerationStructureData = bscratch->GetGPUVirtualAddress();
    bbuild.DestAccelerationStructureData    = scene->blas->GetGPUVirtualAddress();
    impl.list->BuildRaytracingAccelerationStructure(&bbuild, 0, nullptr);
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = scene->blas.Get();
    impl.list->ResourceBarrier(1, &uav); // TLAS build reads the finished BLAS

    // ── `ninst` instances of the BLAS, each with its row-major 3×4 world transform ──
    ComPtr<ID3D12Resource> ibuf = make_buffer(impl.device.Get(), static_cast<UINT64>(ninst) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (ibuf == nullptr) { return nullptr; }
    void* ip = nullptr;
    ibuf->Map(0, nullptr, &ip);
    auto* insts = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(ip);
    for (crd::u32 n = 0; n < ninst; ++n)
    {
        D3D12_RAYTRACING_INSTANCE_DESC inst{};
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 4; ++c) { inst.Transform[r][c] = transforms[static_cast<size_t>(n) * 12U + static_cast<size_t>(r) * 4U + static_cast<size_t>(c)]; }
        }
        inst.InstanceMask          = 0xFFU;
        inst.InstanceID            = n; // surfaces as InstanceID() for per-instance data
        inst.AccelerationStructure = scene->blas->GetGPUVirtualAddress();
        insts[n]                   = inst;
    }
    ibuf->Unmap(0, nullptr);

    // ── TLAS over the `ninst` instances ──
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tinputs{};
    tinputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tinputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tinputs.NumDescs      = ninst;
    tinputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tinputs.InstanceDescs = ibuf->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tpre{};
    impl.device->GetRaytracingAccelerationStructurePrebuildInfo(&tinputs, &tpre);
    ComPtr<ID3D12Resource> tscratch = make_buffer(impl.device.Get(), tpre.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    scene->tlas = make_buffer(impl.device.Get(), tpre.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    if (tscratch == nullptr || scene->tlas == nullptr) { return nullptr; }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tbuild{};
    tbuild.Inputs                           = tinputs;
    tbuild.ScratchAccelerationStructureData = tscratch->GetGPUVirtualAddress();
    tbuild.DestAccelerationStructureData    = scene->tlas->GetGPUVirtualAddress();
    impl.list->BuildRaytracingAccelerationStructure(&tbuild, 0, nullptr);
    scene->tlas_va = scene->tlas->GetGPUVirtualAddress();

    impl.submit_and_wait(); // vbuf / ibuf / scratch stay alive on this frame through the blocking wait
    return scene;
}

bool Dx12RayTracingContext::trace_dispatch(const Dx12RtScene& scene_base, crd::containers::ConstSpan<crd::u8> dxil,
                                           crd::containers::ConstSpan<Binding> bindings, crd::u32 groups)
{
    auto&            impl  = *m_impl;
    const SceneImpl& scene = static_cast<const SceneImpl&>(scene_base);
    const crd::usize nbuf  = bindings.size();
    if (!impl.ok || scene.tlas == nullptr || nbuf == 0 || nbuf > 15) { return false; }

    UINT maxb = 0;
    for (crd::usize i = 0; i < nbuf; ++i) { if (bindings[i].binding > maxb) { maxb = bindings[i].binding; } }
    if (maxb == 0) { return false; } // buffers live at u1.. (u0 is unused; the TLAS is the root SRV t0)

    // ── root signature: [0] root SRV t0 = TLAS · [1] UAV table u1..u{maxb} ──
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors     = maxb;
    range.BaseShaderRegister = 1;
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[0].Descriptor.ShaderRegister = 0;
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
    if (FAILED(impl.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root)))) { return false; }

    // ── compute PSO from the DXIL ──
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature     = root.Get();
    pd.CS.pShaderBytecode = dxil.data();
    pd.CS.BytecodeLength  = dxil.size();
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(impl.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) { return false; }

    // ── shader-visible UAV heap (u1..u{maxb}; slot (binding-1) ↔ register u{binding}) ──
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = maxb;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    if (FAILED(impl.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) { return false; }
    const UINT incr = impl.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // one DEFAULT (UAV) buffer per binding + UPLOAD / READBACK staging as needed. Committed DEFAULT buffers are zero-initialised,
    // so a no-`upload` binding reads zeros (matches the Vulkan path's memset).
    ComPtr<ID3D12Resource> def[16]{};
    ComPtr<ID3D12Resource> up[16]{};
    ComPtr<ID3D12Resource> rb[16]{};
    for (crd::usize i = 0; i < nbuf; ++i)
    {
        def[i] = make_buffer(impl.device.Get(), bindings[i].bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        if (def[i] == nullptr) { return false; }
        if (bindings[i].upload != nullptr)
        {
            up[i] = make_buffer(impl.device.Get(), bindings[i].bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (up[i] == nullptr) { return false; }
            void* mp = nullptr;
            up[i]->Map(0, nullptr, &mp);
            std::memcpy(mp, bindings[i].upload, static_cast<size_t>(bindings[i].bytes));
            up[i]->Unmap(0, nullptr);
            barrier(impl.list.Get(), def[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            impl.list->CopyResource(def[i].Get(), up[i].Get());
            barrier(impl.list.Get(), def[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
            barrier(impl.list.Get(), def[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (bindings[i].readback != nullptr)
        {
            rb[i] = make_buffer(impl.device.Get(), bindings[i].bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
            if (rb[i] == nullptr) { return false; }
        }
        // RAW UAV at heap slot (binding-1) ⇒ register u{binding}.
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format             = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension      = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = static_cast<UINT>(bindings[i].bytes / 4U);
        ud.Buffer.Flags       = D3D12_BUFFER_UAV_FLAG_RAW;
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(bindings[i].binding - 1U) * incr;
        impl.device->CreateUnorderedAccessView(def[i].Get(), nullptr, &ud, h);
    }

    // ── dispatch ──
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    impl.list->SetDescriptorHeaps(1, heaps);
    impl.list->SetComputeRootSignature(root.Get());
    impl.list->SetComputeRootShaderResourceView(0, scene.tlas_va);        // TLAS at t0
    impl.list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    impl.list->SetPipelineState(pso.Get());
    impl.list->Dispatch(groups > 0U ? groups : 1U, 1U, 1U);

    // ── copy outputs to readback staging ──
    for (crd::usize i = 0; i < nbuf; ++i)
    {
        if (bindings[i].readback != nullptr)
        {
            barrier(impl.list.Get(), def[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            impl.list->CopyResource(rb[i].Get(), def[i].Get());
        }
    }
    impl.submit_and_wait();

    for (crd::usize i = 0; i < nbuf; ++i) // read back
    {
        if (bindings[i].readback != nullptr)
        {
            void* mp = nullptr;
            rb[i]->Map(0, nullptr, &mp);
            std::memcpy(bindings[i].readback, mp, static_cast<size_t>(bindings[i].bytes));
            rb[i]->Unmap(0, nullptr);
        }
    }
    return true;
}

} // namespace crd::gpu
