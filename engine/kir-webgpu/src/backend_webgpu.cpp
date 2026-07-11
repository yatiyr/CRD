// backend_webgpu.cpp — Phase 3.1.6 v17-d: KirBackendWebGpu over the WebGPU C API (wgpu-native). Per kernel: emit WGSL
// → shader module → storage/uniform/readback buffers → auto-layout compute pipeline → bind group → compute pass →
// dispatch → copy-to-readback → submit → mapAsync (driven by wgpuDevicePoll) → read. Adapter/device requested via the
// async callback+Future API. ULP-tolerant vs the CPU reference (WGSL has no `precise`). ADR-0098.

#include <crd/kir/webgpu/backend_webgpu.hpp>

#include <crd/kir/ckir_wgsl.hpp>

#include <cstring>

#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h> // wgpuDevicePoll (wgpu-native extension)

namespace crd::kir
{

struct KirBackendWebGpu::Impl
{
    crd::memory::IAllocator* alloc = nullptr;
    WGPUInstance             instance = nullptr;
    WGPUAdapter              adapter = nullptr;
    WGPUDevice               device = nullptr;
    WGPUQueue                queue = nullptr;
    bool                     ok = false;
};

namespace
{
constexpr int kMaxIn = 32;

WGPUStringView sv(const char* s) { WGPUStringView v{}; v.data = s; v.length = std::strlen(s); return v; }

struct AdapterResult { WGPUAdapter adapter = nullptr; bool done = false; };
struct DeviceResult { WGPUDevice device = nullptr; bool done = false; };
struct MapResult { bool done = false; };

void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView /*msg*/, void* ud1, void* /*ud2*/)
{
    auto* r = static_cast<AdapterResult*>(ud1);
    if (status == WGPURequestAdapterStatus_Success) { r->adapter = adapter; }
    r->done = true;
}
void on_device(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView /*msg*/, void* ud1, void* /*ud2*/)
{
    auto* r = static_cast<DeviceResult*>(ud1);
    if (status == WGPURequestDeviceStatus_Success) { r->device = device; }
    r->done = true;
}
void on_map(WGPUMapAsyncStatus /*status*/, WGPUStringView /*msg*/, void* ud1, void* /*ud2*/)
{
    static_cast<MapResult*>(ud1)->done = true;
}

WGPUBuffer make_buffer(WGPUDevice dev, WGPUBufferUsage usage, crd::u64 size)
{
    WGPUBufferDescriptor bd{};
    bd.usage = usage;
    bd.size  = size;
    return wgpuDeviceCreateBuffer(dev, &bd);
}
} // namespace

KirBackendWebGpu::KirBackendWebGpu(crd::memory::IAllocator* alloc) : m_impl(std::make_unique<Impl>())
{
    auto& impl = *m_impl;
    impl.alloc = alloc;
    impl.instance = wgpuCreateInstance(nullptr);
    if (impl.instance == nullptr) { return; }

    AdapterResult                  ar;
    WGPURequestAdapterCallbackInfo aci{};
    aci.mode      = WGPUCallbackMode_AllowProcessEvents;
    aci.callback  = on_adapter;
    aci.userdata1 = &ar;
    wgpuInstanceRequestAdapter(impl.instance, nullptr, aci);
    while (!ar.done) { wgpuInstanceProcessEvents(impl.instance); }
    if (ar.adapter == nullptr) { return; }
    impl.adapter = ar.adapter;

    DeviceResult                  dr;
    WGPURequestDeviceCallbackInfo dci{};
    dci.mode      = WGPUCallbackMode_AllowProcessEvents;
    dci.callback  = on_device;
    dci.userdata1 = &dr;
    wgpuAdapterRequestDevice(impl.adapter, nullptr, dci);
    while (!dr.done) { wgpuInstanceProcessEvents(impl.instance); }
    if (dr.device == nullptr) { return; }
    impl.device = dr.device;
    impl.queue  = wgpuDeviceGetQueue(impl.device);
    impl.ok     = impl.queue != nullptr;
}

KirBackendWebGpu::~KirBackendWebGpu()
{
    auto& impl = *m_impl;
    if (impl.queue != nullptr) { wgpuQueueRelease(impl.queue); }
    if (impl.device != nullptr) { wgpuDeviceRelease(impl.device); }
    if (impl.adapter != nullptr) { wgpuAdapterRelease(impl.adapter); }
    if (impl.instance != nullptr) { wgpuInstanceRelease(impl.instance); }
}

bool KirBackendWebGpu::valid() const noexcept { return m_impl->ok; }

bool KirBackendWebGpu::run(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out)
{
    auto& impl = *m_impl;
    if (!impl.ok || n_inputs > kMaxIn) { return false; }
    auto*        dev  = impl.device;
    const KNode& outn = g.node(output);

    GlslKernel kern(impl.alloc);
    crd::u64   in_bytes[kMaxIn] = {};
    crd::u64   out_bytes        = 0;
    crd::u32   groups           = 0;
    crd::u32   consts[4]        = {0, 0, 0, 0};

    // FUSION FIRST: activation(GEMM + bias) → ONE fused kernel (epilogue in the store). WebGPU inherits the crush.
    const FuseInfo fuse  = detect_fuse(g, output, impl.alloc);
    bool           fused = false;
    if (fuse.ok && emit_contract_fused_wgsl(g, output, fuse.contract, fuse, impl.alloc, kern) && kern.n_inputs == n_inputs)
    {
        const KNode& cn = g.node(fuse.contract);
        const KNode& an = g.node(cn.a);
        const KNode& bn = g.node(cn.b);
        const int    r  = an.shape.rank;
        consts[0]       = static_cast<crd::u32>(an.shape.dims[r - 2]);             // d0 = M
        consts[1]       = static_cast<crd::u32>(bn.shape.dims[bn.shape.rank - 1]); // d1 = N
        consts[2]       = static_cast<crd::u32>(an.shape.dims[r - 1]);             // d2 = K
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
        // T2 FAST tiled GEMM (FMA, transposed-A var<workgroup>) — WebGPU inherits the crush schedule; else naive T1.
        if (outn.tier == DetTier::Fast && consts[3] == 1U && consts[0] % 64U == 0U && consts[2] % 64U == 0U && consts[1] % 8U == 0U
            && emit_contract_fast_wgsl(g, output, kern) && kern.n_inputs == n_inputs)
        {
            groups = (consts[0] / 64U) * (consts[2] / 64U);
        }
        else
        {
            if (!emit_contract_wgsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
            groups = (consts[0] * consts[2] * consts[3] + 255U) / 256U;
        }
    }
    else if (is_reduce(outn.op))
    {
        const bool fast = (outn.tier == DetTier::Fast && is_fast_reduceable(outn.op)); // T2 parallel workgroup tree-reduce
        if (fast) { if (!emit_reduce_fast_wgsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; } }
        else if (!emit_reduce_wgsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 in_numel  = static_cast<crd::u64>(g.node(outn.a).shape.numel());
        const crd::u64 out_numel = static_cast<crd::u64>(outn.shape.numel());
        consts[0]                = static_cast<crd::u32>(out_numel);
        consts[1]                = static_cast<crd::u32>(in_numel / out_numel);
        in_bytes[0]              = in_numel * sizeof(float);
        out_bytes                = out_numel * sizeof(float);
        groups                   = fast ? static_cast<crd::u32>(out_numel) : (static_cast<crd::u32>(out_numel) + 255U) / 256U; // T2: 1 WG/output
    }
    else if (outn.op == KOp::Gather)
    {
        if (!emit_gather_wgsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
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
        if (!emit_scatter_wgsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
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
    else if (outn.op == KOp::ScanSum)
    {
        const bool fast = (outn.tier == DetTier::Fast); // T2 parallel workgroup prefix-sum
        if (fast) { if (!emit_scan_fast_wgsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; } }
        else if (!emit_scan_wgsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 numel   = static_cast<crd::u64>(outn.shape.numel());
        const crd::u32 scanlen = static_cast<crd::u32>(outn.shape.dims[outn.shape.rank - 1]);
        consts[0]              = static_cast<crd::u32>(numel / scanlen); // nrows
        consts[1]              = scanlen;
        in_bytes[0]            = numel * sizeof(float);
        out_bytes             = numel * sizeof(float);         // scan KEEPS the shape
        groups                = fast ? consts[0] : (consts[0] + 255U) / 256U; // T2: 1 WG/row
    }
    // B0 fan-out: a graph carrying vec/mat/bool/struct VALUES routes to the type-aware emitter (interleaved I/O:
    // `comps` floats per element), exactly as the Vulkan/DX12 backends do.
    else if (graph_uses_vec(g, output, impl.alloc))
    {
        if (!emit_vec_wgsl(g, output, impl.alloc, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 on = static_cast<crd::u64>(outn.shape.numel());
        consts[0]         = static_cast<crd::u32>(on);
        for (int i = 0; i < n_inputs; ++i) { in_bytes[i] = on * static_cast<crd::u64>(kern.in_comps[i]) * sizeof(float); }
        out_bytes = on * static_cast<crd::u64>(kern.out_comps) * sizeof(float);
        groups    = (static_cast<crd::u32>(on) + 255U) / 256U;
    }
    else
    {
        if (!emit_elementwise_wgsl(g, output, impl.alloc, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 on = static_cast<crd::u64>(outn.shape.numel());
        consts[0]         = static_cast<crd::u32>(on);
        for (int i = 0; i < n_inputs; ++i) { in_bytes[i] = on * sizeof(float); }
        out_bytes = on * sizeof(float);
        groups    = (static_cast<crd::u32>(on) + 255U) / 256U;
    }

    WGPUShaderSourceWGSL wgsl{};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code        = sv(kern.source.c_str());
    WGPUShaderModuleDescriptor smd{};
    smd.nextInChain      = &wgsl.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(dev, &smd);
    if (module == nullptr) { return false; }

    WGPUBuffer in_buf[kMaxIn] = {};
    for (int i = 0; i < n_inputs; ++i)
    {
        in_buf[i] = make_buffer(dev, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, in_bytes[i]);
        wgpuQueueWriteBuffer(impl.queue, in_buf[i], 0, inputs[kern.input_iidx[i]], in_bytes[i]);
    }
    WGPUBuffer out_buf  = make_buffer(dev, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc, out_bytes);
    WGPUBuffer readback = make_buffer(dev, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, out_bytes);
    WGPUBuffer uniform  = make_buffer(dev, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, sizeof(consts));
    wgpuQueueWriteBuffer(impl.queue, uniform, 0, consts, sizeof(consts));

    WGPUComputePipelineDescriptor pd{};
    pd.compute.module     = module;
    pd.compute.entryPoint = sv("cs_main");
    WGPUComputePipeline pipeline = wgpuDeviceCreateComputePipeline(dev, &pd);
    if (pipeline == nullptr) { return false; }
    WGPUBindGroupLayout bgl = wgpuComputePipelineGetBindGroupLayout(pipeline, 0);

    WGPUBindGroupEntry entries[kMaxIn + 2] = {};
    for (int i = 0; i < n_inputs; ++i)
    {
        entries[i].binding = static_cast<crd::u32>(i);
        entries[i].buffer  = in_buf[i];
        entries[i].size    = in_bytes[i];
    }
    entries[n_inputs].binding     = static_cast<crd::u32>(n_inputs);
    entries[n_inputs].buffer      = out_buf;
    entries[n_inputs].size        = out_bytes;
    entries[n_inputs + 1].binding = static_cast<crd::u32>(n_inputs + 1);
    entries[n_inputs + 1].buffer  = uniform;
    entries[n_inputs + 1].size    = sizeof(consts);
    WGPUBindGroupDescriptor bgd{};
    bgd.layout     = bgl;
    bgd.entryCount = static_cast<size_t>(n_inputs) + 2U;
    bgd.entries    = entries;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(dev, &bgd);

    WGPUCommandEncoder     enc  = wgpuDeviceCreateCommandEncoder(dev, nullptr);
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, nullptr);
    wgpuComputePassEncoderSetPipeline(pass, pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, groups > 0U ? groups : 1U, 1U, 1U);
    wgpuComputePassEncoderEnd(pass);
    wgpuCommandEncoderCopyBufferToBuffer(enc, out_buf, 0, readback, 0, out_bytes);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(impl.queue, 1, &cmd);

    MapResult                 mr;
    WGPUBufferMapCallbackInfo mci{};
    mci.mode      = WGPUCallbackMode_AllowProcessEvents;
    mci.callback  = on_map;
    mci.userdata1 = &mr;
    wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, out_bytes, mci);
    while (!mr.done) { wgpuDevicePoll(impl.device, 1, nullptr); }
    const void* mapped = wgpuBufferGetMappedRange(readback, 0, out_bytes);
    bool        ok     = mapped != nullptr;
    if (ok) { std::memcpy(out, mapped, out_bytes); }
    wgpuBufferUnmap(readback);

    wgpuCommandBufferRelease(cmd);
    wgpuComputePassEncoderRelease(pass);
    wgpuCommandEncoderRelease(enc);
    wgpuBindGroupRelease(bg);
    wgpuBindGroupLayoutRelease(bgl);
    wgpuComputePipelineRelease(pipeline);
    wgpuShaderModuleRelease(module);
    wgpuBufferRelease(uniform);
    wgpuBufferRelease(readback);
    wgpuBufferRelease(out_buf);
    for (int i = 0; i < n_inputs; ++i) { wgpuBufferRelease(in_buf[i]); }
    return ok;
}

} // namespace crd::kir
