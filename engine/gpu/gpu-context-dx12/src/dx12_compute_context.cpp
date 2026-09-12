// dx12_compute_context.cpp — the D3D12 implementation of crd::gpu::IComputeContext (ADR-0100). Device + dedicated compute
// queue + dxc; per-buffer explicit-state tracking (D3D12 has no implicit access model); a shader-visible UAV heap
// sub-allocated per dispatch; RAW (ByteAddressBuffer) UAVs so no element-type is baked into the interface. Mirrors the
// Vulkan backend's recorder shape (copy / barrier / dispatch → submit_and_wait). D3D12 helpers mirror crd-kir-dx12.

#include "dx12_device_scope.hpp"
#include "dx12_execution.hpp"

#include <crd/gpu/dx12_compute_context.hpp>

#include <crd/containers/string.hpp>
#include <crd/containers/hash_set.hpp>
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
constexpr crd::u64 kPipelineCacheMagic = 0x3130435058445243ULL; // "CRDXPC01", little-endian envelope version 1.
constexpr crd::usize kPipelineCacheHeader = 32U;
constexpr crd::u64 kPipelineCacheKeys = 65536U;
constexpr crd::u64 kPipelineCacheBytes = 256U << 20U;

void cache_write_u64(crd::u8* bytes, crd::u64 value) noexcept
{
    for (crd::u32 index = 0; index < 8U; ++index) { bytes[index] = static_cast<crd::u8>(value >> (index * 8U)); }
}

crd::u64 cache_read_u64(const crd::u8* bytes) noexcept
{
    crd::u64 value = 0U;
    for (crd::u32 index = 0; index < 8U; ++index) { value |= static_cast<crd::u64>(bytes[index]) << (index * 8U); }
    return value;
}

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
    explicit Impl(crd::memory::IAllocator* allocator) : alloc(allocator), lib_blob(allocator), pipe_keys(allocator) {}

    detail::Dx12DeviceScope validation;
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
    crd::containers::Array<crd::u8>   lib_blob;      // D4: owns the warm-start blob — CreatePipelineLibrary does NOT copy it
    crd::containers::HashSet<crd::u64> pipe_keys;    // Native library has no membership query; never probe a known miss.
    ComPtr<ID3D12PipelineLibrary>     pipe_lib;      // Destroy before its borrowed lib_blob backing storage.
    UINT                              heap_incr = 0;
    UINT                              heap_next = 0; // sub-alloc cursor, reset each begin()
    HANDLE                            event     = nullptr;
    crd::u64                          fence_val = 0;
    bool                              ok        = false;

    // CGP-0: portable GPU timing. Two timestamps bracket the recorded work (queue tick-count → ms via the queue
    // frequency), resolved into a READBACK buffer and read after the fence. Best-effort: ts_ok=false ⇒ last_gpu_ms stays 0.
    ComPtr<ID3D12QueryHeap>           ts_heap;                // 2 timestamps (start @ begin, end @ submit)
    ComPtr<ID3D12Resource>           ts_readback;            // 2×u64 resolved ticks (READBACK heap, COPY_DEST)
    double                            ts_period_ms = 0.0;     // ms per tick = 1000 / GetTimestampFrequency
    double                            last_gpu_ms_v = 0.0;
    bool                              ts_ok        = false;

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
        if (!ok) { return; }
        auto& s = static_cast<BufferImpl&>(src);
        auto& d = static_cast<BufferImpl&>(dst);
        ensure_state(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ensure_state(d, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(d.res.Get(), dst_off, s.res.Get(), src_off, bytes);
    }

    void barrier(ComputeBuffer& buf, ComputeAccess /*from*/, ComputeAccess to) override
    {
        if (!ok) { return; }
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
        if (!ok) { return; }
        bind_compute(pipeline, bindings, push, push_size);
        list->Dispatch(gx > 0U ? gx : 1U, gy > 0U ? gy : 1U, gz > 0U ? gz : 1U);
    }

    // C5: the workgroup count is read from `args` (a compute-written buffer, 3 u32 {x,y,z} at `args_offset`) via ExecuteIndirect
    // — the DX12 GPU-driven dispatch. The DISPATCH command signature is device-level (no root sig), created once + cached.
    void dispatch_indirect(ComputePipeline& pipeline, crd::containers::ConstSpan<ComputeBuffer*> bindings, const void* push,
                           crd::u32 push_size, ComputeBuffer& args, crd::u64 args_offset) override
    {
        if (!ok) { return; }
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

Dx12ComputeContext::Dx12ComputeContext(crd::memory::IAllocator* alloc) : m_impl(std::make_unique<Impl>(alloc))
{
    auto& impl = *m_impl;
    impl.alloc = alloc;
    if (FAILED(impl.validation.create(impl.device))) { return; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (FAILED(impl.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&impl.queue)))) { return; }

    // CGP-0: the timestamp query heap (2 timestamps) + a 16-byte READBACK buffer for the resolved ticks. Best-effort:
    // any failure leaves ts_ok=false and last_gpu_ms() returns 0. A compute queue supports timestamps on all D3D12 tier-1+
    // hardware; the frequency (ticks/sec) is per-queue, so read it from THIS queue.
    {
        UINT64                    freq = 0U;
        D3D12_QUERY_HEAP_DESC     qhd{};
        qhd.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qhd.Count = 2U;
        if (SUCCEEDED(impl.queue->GetTimestampFrequency(&freq)) && freq != 0U
            && SUCCEEDED(impl.device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&impl.ts_heap))))
        {
            impl.ts_readback = make_buffer(impl.device.Get(), 2U * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK,
                                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
            if (impl.ts_readback)
            {
                impl.ts_period_ms = 1.0e3 / static_cast<double>(freq); // ms per tick
                impl.ts_ok        = true;
            }
        }
    }
    if (FAILED(impl.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&impl.cmd_alloc)))) { return; }
    if (FAILED(impl.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, impl.cmd_alloc.Get(), nullptr, IID_PPV_ARGS(&impl.list)))) { return; }
    if (FAILED(impl.list->Close())) { return; }
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

bool Dx12ComputeContext::valid() const noexcept
{
    return m_impl->ok && SUCCEEDED(m_impl->device->GetDeviceRemovedReason());
}
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

// The indexed envelope avoids warning-producing LoadComputePipeline probes for absent names, including after reload.
// All lengths are bounded and encoded explicitly; the checksum detects corruption, not malicious authentication.
void Dx12ComputeContext::pipeline_cache_data(crd::containers::Array<crd::u8>& out) const
{
    auto& impl = *m_impl;
    out.resize(0);
    if (!impl.pipe_lib) { return; }
    const SIZE_T sz = impl.pipe_lib->GetSerializedSize();
    if (sz == 0U || sz > kPipelineCacheBytes || impl.pipe_keys.size() > kPipelineCacheKeys) { return; }
    const crd::usize offset = kPipelineCacheHeader + impl.pipe_keys.size() * 8U;
    out.resize(offset + sz);
    cache_write_u64(out.data(), kPipelineCacheMagic);
    cache_write_u64(out.data() + 8U, impl.pipe_keys.size());
    cache_write_u64(out.data() + 16U, sz);
    crd::usize cursor = kPipelineCacheHeader;
    for (const crd::u64 key : impl.pipe_keys)
    {
        cache_write_u64(out.data() + cursor, key);
        cursor += 8U;
    }
    if (FAILED(impl.pipe_lib->Serialize(out.data() + offset, sz))) { out.resize(0); return; }
    cache_write_u64(out.data() + 24U, crd::containers::fnv1a_64(out.data() + kPipelineCacheHeader,
                                                              out.size() - kPipelineCacheHeader));
}

// Construct a replacement independently. Failed/malformed/legacy input preserves the working library; an empty input
// explicitly resets it. Release the old native library BEFORE replacing the storage it borrows.
bool Dx12ComputeContext::warm_pipeline_cache(crd::containers::ConstSpan<crd::u8> blob)
{
    auto& impl = *m_impl;
    if (!impl.device1) { return false; }
    crd::containers::Array<crd::u8> fresh_blob(impl.alloc);
    crd::containers::HashSet<crd::u64> fresh_keys(impl.alloc);
    if (!blob.empty())
    {
        if (blob.size() < kPipelineCacheHeader || cache_read_u64(blob.data()) != kPipelineCacheMagic) { return false; }
        const crd::u64 count = cache_read_u64(blob.data() + 8U);
        const crd::u64 bytes = cache_read_u64(blob.data() + 16U);
        if (count > kPipelineCacheKeys || bytes == 0U || bytes > kPipelineCacheBytes) { return false; }
        const crd::u64 offset = kPipelineCacheHeader + count * 8U;
        if (offset > blob.size() || bytes != blob.size() - offset) { return false; }
        if (cache_read_u64(blob.data() + 24U) != crd::containers::fnv1a_64(blob.data() + kPipelineCacheHeader,
                                                                         blob.size() - kPipelineCacheHeader))
        {
            return false;
        }
        fresh_keys.reserve(static_cast<crd::usize>(count));
        for (crd::u64 index = 0; index < count; ++index)
        {
            if (!fresh_keys.insert(cache_read_u64(blob.data() + kPipelineCacheHeader + index * 8U))) { return false; }
        }
        fresh_blob.resize(static_cast<crd::usize>(bytes));
        std::memcpy(fresh_blob.data(), blob.data() + offset, static_cast<crd::usize>(bytes));
    }
    ComPtr<ID3D12PipelineLibrary> fresh;
    if (FAILED(impl.device1->CreatePipelineLibrary(fresh_blob.empty() ? nullptr : fresh_blob.data(), fresh_blob.size(),
                                                  IID_PPV_ARGS(&fresh)))) { return false; }
    impl.pipe_lib.Reset();
    impl.lib_blob = std::move(fresh_blob);
    impl.pipe_keys = std::move(fresh_keys);
    impl.pipe_lib = std::move(fresh);
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
static std::unique_ptr<ComputePipeline> build_dxil_pipeline(ID3D12Device* device, ID3D12PipelineLibrary* lib,
                                                            crd::containers::HashSet<crd::u64>& keys, const void* code,
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
        const crd::u64 h = crd::containers::hash_u64(crd::containers::fnv1a_64(code, len)
            ^ (static_cast<crd::u64>(n_bindings) << 32U) ^ push_size);
        wchar_t name[24];
        swprintf(name, 24, L"p%016llx", static_cast<unsigned long long>(h));
        if (keys.contains(h))
        {
            const HRESULT result = lib->LoadComputePipeline(name, &pd, IID_PPV_ARGS(&pl->pso));
            if (FAILED(result)) { detail::dx12_execution_failure(result, "load indexed compute pipeline"); return nullptr; }
            made = true;
        }
        else if (SUCCEEDED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pl->pso))))
        {
            if (keys.size() < kPipelineCacheKeys && SUCCEEDED(lib->StorePipeline(name, pl->pso.Get()))) { keys.insert(h); }
            made = true;
        }
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
    return build_dxil_pipeline(impl.device.Get(), impl.pipe_lib.Get(), impl.pipe_keys, dxil->GetBufferPointer(), dxil->GetBufferSize(), n_bindings, push_size);
}

std::unique_ptr<ComputePipeline> Dx12ComputeContext::create_pipeline_from_dxil(crd::containers::ConstSpan<crd::u8> dxil,
                                                                               int n_bindings, crd::u32 push_size)
{
    auto& impl = *m_impl;
    if (!impl.ok || n_bindings <= 0 || dxil.empty()) { return nullptr; }
    return build_dxil_pipeline(impl.device.Get(), impl.pipe_lib.Get(), impl.pipe_keys, dxil.data(), static_cast<SIZE_T>(dxil.size()), n_bindings, push_size);
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
    if (!valid() || FAILED(detail::dx12_reset(impl.cmd_alloc.Get(), impl.list.Get())))
    {
        impl.ok = false;
        return impl;
    }
    ID3D12DescriptorHeap* heaps[] = {impl.heap.Get()};
    impl.list->SetDescriptorHeaps(1, heaps);
    impl.heap_next = 0;
    // CGP-0: start timestamp (D3D12 timestamps use EndQuery — there is no BeginQuery for the TIMESTAMP type).
    if (impl.ts_ok) { impl.list->EndQuery(impl.ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0U); }
    return impl;
}

void Dx12ComputeContext::submit_and_wait()
{
    auto& impl = *m_impl;
    if (!valid()) { impl.ok = false; return; }
    // CGP-0: end timestamp + resolve the two ticks into the READBACK buffer (both before Close).
    if (impl.ts_ok)
    {
        impl.list->EndQuery(impl.ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1U);
        impl.list->ResolveQueryData(impl.ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0U, 2U, impl.ts_readback.Get(), 0U);
    }
    bool submitted = false;
    if (FAILED(detail::dx12_submit(impl.device.Get(), impl.queue.Get(), impl.list.Get(), impl.fence.Get(),
                                  impl.fence_val, submitted))
        || FAILED(detail::dx12_wait(impl.device.Get(), impl.fence.Get(), impl.fence_val, impl.event)))
    {
        impl.ok = false;
        impl.last_gpu_ms_v = 0.0;
        return;
    }
    // CGP-0: read the resolved ticks now that the GPU has finished. end >= start ⇒ elapsed ms; otherwise leave 0.
    if (impl.ts_ok)
    {
        void*             mapped = nullptr;
        const D3D12_RANGE rr{0U, 2U * sizeof(UINT64)};
        if (SUCCEEDED(impl.ts_readback->Map(0U, &rr, &mapped)) && mapped != nullptr)
        {
            UINT64 ticks[2] = {0U, 0U};
            std::memcpy(ticks, mapped, sizeof(ticks));
            const D3D12_RANGE wrote{0U, 0U}; // CPU wrote nothing
            impl.ts_readback->Unmap(0U, &wrote);
            impl.last_gpu_ms_v = (ticks[1] >= ticks[0]) ? static_cast<double>(ticks[1] - ticks[0]) * impl.ts_period_ms : 0.0;
        }
    }
}

double Dx12ComputeContext::last_gpu_ms() const noexcept { return m_impl->last_gpu_ms_v; }

} // namespace crd::gpu
