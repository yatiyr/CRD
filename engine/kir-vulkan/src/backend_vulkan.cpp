// backend_vulkan.cpp — Phase 3.1.6 v17-b: KirBackendVulkan. Owns a headless Vulkan instance/device; `run` dispatches
// a single-kernel graph (fused-elementwise cone OR a Contract/matmul of Input leaves) via a shared dispatch_kernel
// helper: emit GLSL → compile_glsl (SPIR-V) → pipeline → host-visible storage buffers (upload/readback) → descriptor
// set → record → graphics_queue().submit_and_wait → map. `precise` GLSL + sequential order ⇒ bit-matches the CPU
// reference for correctly-rounded ops. ADR-0098. Reuses the geometry-bvh-gpu dispatch pattern.

#include <crd/kir/vulkan/backend_vulkan.hpp>

#include <crd/kir/ckir_glsl.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/rhi/buffer.hpp>
#include <crd/rhi/command_buffer.hpp>
#include <crd/rhi/compute_pipeline.hpp>
#include <crd/rhi/descriptor.hpp>
#include <crd/rhi/device.hpp>
#include <crd/rhi/pipeline.hpp>
#include <crd/rhi/queue.hpp>
#include <crd/rhi/shader_module.hpp>
#include <crd/rhi/vulkan_backend.hpp>
#include <crd/rhi/vulkan_validation_capture.hpp>
#include <crd/shader/compile.hpp>

namespace crd::kir
{

struct KirBackendVulkan::Impl
{
    crd::memory::IAllocator*                       alloc = nullptr;
    std::unique_ptr<crd::rhi::Instance>            instance;
    std::unique_ptr<crd::rhi::ValidationCapture>   capture;
    std::unique_ptr<crd::rhi::Device>              device;
    std::unique_ptr<crd::rhi::DescriptorAllocator> desc_alloc;
    bool                                           ok = false;
};

namespace
{
// One-shot: compile GLSL → pipeline → host-visible storage buffers (upload inputs, output) → dispatch → readback.
// binding i (0..n_inputs-1) reads inputs[input_iidx[i]] (input_bytes[i]); binding n_inputs = the output (out_bytes).
// A 16-byte push constant carries the kernel's dims. All bit-exact + ValidationCapture-clean.
bool dispatch_kernel(crd::rhi::Device& dev, crd::rhi::DescriptorAllocator& desc_alloc, crd::memory::IAllocator* alloc,
                     crd::containers::StringView glsl, int n_inputs, const int* input_iidx, const crd::u64* input_bytes,
                     crd::u64 out_bytes, const void* push16, crd::u32 groups, const float* const* inputs, float* out)
{
    const auto cres = crd::shader::compile_glsl(crd::shader::Stage::Compute, glsl, "ckir", alloc);
    if (!cres.ok) { return false; }
    auto shader = dev.create_shader_module({crd::rhi::ShaderStage::Compute, "main", crd::containers::ConstSpan<crd::u8>(cres.spirv.data(), cres.spirv.size())});
    if (shader == nullptr) { return false; }

    const int                   nb = n_inputs + 1;
    crd::rhi::DescriptorBinding  bindings[kMaxKernelInputs + 1];
    for (int i = 0; i < nb; ++i) { bindings[i] = {static_cast<crd::u32>(i), crd::rhi::DescriptorType::StorageBuffer, 1, crd::rhi::ShaderStage::Compute}; }
    crd::rhi::DescriptorSetLayoutDesc sld{};
    sld.bindings    = crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(bindings, static_cast<crd::usize>(nb));
    auto set_layout = dev.create_descriptor_set_layout(sld);
    if (set_layout == nullptr) { return false; }

    const crd::rhi::DescriptorSetLayout* layouts[] = {set_layout.get()};
    crd::rhi::PushConstantRange           pcr{};
    pcr.stages = crd::rhi::ShaderStage::Compute;
    pcr.offset = 0U;
    pcr.size   = 16U;
    crd::rhi::PipelineLayoutDesc pld{};
    pld.set_layouts          = crd::containers::ConstSpan<const crd::rhi::DescriptorSetLayout*>(layouts, 1);
    pld.push_constant_ranges = crd::containers::ConstSpan<crd::rhi::PushConstantRange>(&pcr, 1);
    auto pipe_layout = dev.create_pipeline_layout(pld);
    if (pipe_layout == nullptr) { return false; }
    crd::rhi::ComputePipelineDesc cpd{};
    cpd.compute_shader  = shader.get();
    cpd.pipeline_layout = pipe_layout.get();
    auto pipeline = dev.create_compute_pipeline(cpd);
    if (pipeline == nullptr) { return false; }

    std::unique_ptr<crd::rhi::Buffer> in_bufs[kMaxKernelInputs];
    const crd::u32                    stor = crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage);
    for (int i = 0; i < n_inputs; ++i)
    {
        in_bufs[i] = dev.create_buffer({input_bytes[i], stor, crd::rhi::MemoryUsage::CpuToGpu});
        if (in_bufs[i] == nullptr) { return false; }
        auto* dst = static_cast<float*>(in_bufs[i]->map());
        if (dst == nullptr) { return false; }
        const float*   src = inputs[input_iidx[i]];
        const crd::u64 n   = input_bytes[i] / sizeof(float);
        for (crd::u64 e = 0; e < n; ++e) { dst[e] = src[e]; }
        in_bufs[i]->unmap();
    }
    auto out_buf = dev.create_buffer({out_bytes, stor, crd::rhi::MemoryUsage::GpuToCpu});
    if (out_buf == nullptr) { return false; }

    desc_alloc.begin_frame(0U);
    auto set = desc_alloc.allocate(*set_layout);
    if (set == nullptr) { return false; }
    for (int i = 0; i < n_inputs; ++i) { set->update_buffer(static_cast<crd::u32>(i), *in_bufs[i], 0U, input_bytes[i]); }
    set->update_buffer(static_cast<crd::u32>(n_inputs), *out_buf, 0U, out_bytes);

    auto cmd = dev.create_command_buffer();
    if (cmd == nullptr) { return false; }
    cmd->begin();
    cmd->bind_compute_pipeline(*pipeline);
    crd::rhi::DescriptorSet* sets[] = {set.get()};
    cmd->bind_compute_descriptor_sets(*pipe_layout, 0U, crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
    cmd->push_constants(*pipe_layout, crd::rhi::ShaderStage::Compute, 0U, 16U, push16);
    cmd->dispatch(groups > 0U ? groups : 1U, 1U, 1U);
    cmd->buffer_barrier(*out_buf, crd::rhi::BufferAccess::ComputeShaderWrite, crd::rhi::BufferAccess::TransferSrc);
    cmd->end();
    dev.graphics_queue().submit_and_wait(*cmd);

    const auto* rd = static_cast<const float*>(out_buf->map());
    if (rd == nullptr) { return false; }
    const crd::u64 on = out_bytes / sizeof(float);
    for (crd::u64 e = 0; e < on; ++e) { out[e] = rd[e]; }
    out_buf->unmap();
    return true;
}
} // namespace

KirBackendVulkan::KirBackendVulkan(crd::memory::IAllocator* alloc) : m_impl(std::make_unique<Impl>())
{
    auto& impl = *m_impl;
    impl.alloc = alloc;
    impl.instance = crd::rhi::create_vulkan_instance({});
    if (impl.instance == nullptr) { return; }
    impl.capture = std::make_unique<crd::rhi::ValidationCapture>(*impl.instance);
    impl.device  = impl.instance->create_device({});
    if (impl.device == nullptr) { return; }
    crd::rhi::DescriptorAllocatorDesc ad{};
    ad.frames_in_flight              = 2;
    ad.max_sets_per_frame            = 8;
    ad.max_storage_buffers_per_frame = 2 * (kMaxKernelInputs + 1);
    impl.desc_alloc = impl.device->create_descriptor_allocator(ad);
    if (impl.desc_alloc == nullptr) { return; }
    impl.ok = true;
}

KirBackendVulkan::~KirBackendVulkan() = default;

bool KirBackendVulkan::valid() const noexcept { return m_impl->ok; }
int  KirBackendVulkan::validation_errors() const noexcept { return m_impl->capture ? static_cast<int>(m_impl->capture->error_count()) : 0; }

bool KirBackendVulkan::run(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out)
{
    auto& impl = *m_impl;
    if (!impl.ok || n_inputs > kMaxKernelInputs) { return false; }
    const KNode& outn = g.node(output);
    GlslKernel   kern(impl.alloc);

    // FUSION FIRST: activation(GEMM + bias) → ONE fused kernel (epilogue in the store, no extra round-trip). Vulkan
    // inherits the fusion crush; falls through to the plain paths otherwise.
    const FuseInfo fuse = detect_fuse(g, output, impl.alloc);
    if (fuse.ok && emit_contract_fused_glsl(g, output, fuse.contract, fuse, impl.alloc, kern) && kern.n_inputs == n_inputs)
    {
        const KNode&   cn = g.node(fuse.contract);
        const KNode&   an = g.node(cn.a);
        const KNode&   bn = g.node(cn.b);
        const int      r  = an.shape.rank;
        const crd::u32 mm = static_cast<crd::u32>(an.shape.dims[r - 2]);
        const crd::u32 kk = static_cast<crd::u32>(an.shape.dims[r - 1]);
        const crd::u32 nn = static_cast<crd::u32>(bn.shape.dims[bn.shape.rank - 1]);
        struct alignas(16) PC { crd::u32 m, n, k, pad; } pc{mm, nn, kk, 0U};
        crd::u64       in_bytes[2 + kMaxFusedBias] = {};
        in_bytes[0] = static_cast<crd::u64>(mm) * kk * sizeof(float);
        in_bytes[1] = static_cast<crd::u64>(kk) * nn * sizeof(float);
        for (int j = 0; j < fuse.n_bias; ++j) { in_bytes[2 + j] = static_cast<crd::u64>(nn) * sizeof(float); }
        const crd::u64 out_bytes = static_cast<crd::u64>(mm) * nn * sizeof(float);
        const crd::u32 groups    = (mm * nn + 255U) / 256U;
        return dispatch_kernel(*impl.device, *impl.desc_alloc, impl.alloc, crd::containers::to_view(kern.source), n_inputs, kern.input_iidx, in_bytes, out_bytes, &pc, groups, inputs, out);
    }

    if (outn.op == KOp::Contract) // batched matmul of two Input leaves
    {
        const KNode&   an = g.node(outn.a);
        const KNode&   bn = g.node(outn.b);
        const int      r  = an.shape.rank;
        const crd::u32 mm = static_cast<crd::u32>(an.shape.dims[r - 2]);
        const crd::u32 kk = static_cast<crd::u32>(an.shape.dims[r - 1]);
        const crd::u32 nn = static_cast<crd::u32>(bn.shape.dims[bn.shape.rank - 1]);
        crd::u32       batch = 1U;
        for (int k = 0; k < r - 2; ++k) { batch *= static_cast<crd::u32>(an.shape.dims[k]); }
        // NOTE: `emit_contract_tiled_glsl` (64x64x8 block) is bit-exact but currently SLOWER than naive for L2-resident
        // sizes — too few workgroups ((M/64)*(N/64)) starves the SMs. A winning tiled GEMM needs block-size tuning +
        // occupancy profiling (Nsight); kept unrouted until then. The naive precise kernel stays the default (correct,
        // L2-competitive). See docs/hints/v17-kir-gpu-gotchas.md.
        (void) batch;
        if (!emit_contract_glsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        struct alignas(16) PC { crd::u32 m, k, n, b; } pc{mm, kk, nn, batch};
        const crd::u64 in_bytes[2] = {static_cast<crd::u64>(mm) * kk * batch * sizeof(float), static_cast<crd::u64>(kk) * nn * batch * sizeof(float)};
        const crd::u64 out_bytes   = static_cast<crd::u64>(mm) * nn * batch * sizeof(float);
        const crd::u32 groups      = (mm * nn * batch + 255U) / 256U;
        return dispatch_kernel(*impl.device, *impl.desc_alloc, impl.alloc, crd::containers::to_view(kern.source), 2, kern.input_iidx, in_bytes, out_bytes, &pc, groups, inputs, out);
    }

    if (is_reduce(outn.op)) // trailing-contiguous reduce of an Input leaf
    {
        const bool fast = (outn.tier == DetTier::Fast && is_fast_reduceable(outn.op)); // T2 parallel workgroup tree-reduce
        if (fast) { if (!emit_reduce_fast_glsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; } }
        else if (!emit_reduce_glsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 in_numel  = static_cast<crd::u64>(g.node(outn.a).shape.numel());
        const crd::u64 out_numel = static_cast<crd::u64>(outn.shape.numel());
        struct alignas(16) PC { crd::u32 nout; crd::u32 redsize; crd::u32 pad[2]; } pc{};
        pc.nout                 = static_cast<crd::u32>(out_numel);
        pc.redsize              = static_cast<crd::u32>(in_numel / out_numel);
        const crd::u64 in_bytes[1] = {in_numel * sizeof(float)};
        const crd::u32 groups      = fast ? static_cast<crd::u32>(out_numel) : (static_cast<crd::u32>(out_numel) + 255U) / 256U; // T2: 1 WG/output
        return dispatch_kernel(*impl.device, *impl.desc_alloc, impl.alloc, crd::containers::to_view(kern.source), 1, kern.input_iidx, in_bytes, out_numel * sizeof(float), &pc, groups, inputs, out);
    }

    if (outn.op == KOp::Gather) // row-gather: out[m,...] = data[idx[m],...]
    {
        if (!emit_gather_glsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const KNode&   dn         = g.node(outn.a);
        const crd::u64 out_numel  = static_cast<crd::u64>(outn.shape.numel());
        const crd::u64 data_numel = static_cast<crd::u64>(dn.shape.numel());
        const crd::u64 idx_numel  = static_cast<crd::u64>(g.node(outn.b).shape.numel());
        struct alignas(16) PC { crd::u32 nout; crd::u32 rowsize; crd::u32 pad[2]; } pc{};
        pc.nout                    = static_cast<crd::u32>(out_numel);
        pc.rowsize                 = static_cast<crd::u32>(data_numel / static_cast<crd::u64>(dn.shape.dims[0]));
        const crd::u64 in_bytes[2] = {data_numel * sizeof(float), idx_numel * sizeof(float)};
        const crd::u32 groups      = (static_cast<crd::u32>(out_numel) + 255U) / 256U;
        return dispatch_kernel(*impl.device, *impl.desc_alloc, impl.alloc, crd::containers::to_view(kern.source), 2, kern.input_iidx, in_bytes, out_numel * sizeof(float), &pc, groups, inputs, out);
    }

    if (outn.op == KOp::Scatter) // out=base, then out[idx[m],...]=updates[m,...] (last-wins)
    {
        if (!emit_scatter_glsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const KNode&   bn          = g.node(outn.a);
        const crd::u64 out_numel   = static_cast<crd::u64>(outn.shape.numel());
        const crd::u64 base_numel  = static_cast<crd::u64>(bn.shape.numel());
        const crd::u64 idx_numel   = static_cast<crd::u64>(g.node(outn.b).shape.numel());
        const crd::u64 upd_numel   = static_cast<crd::u64>(g.node(outn.c).shape.numel());
        struct alignas(16) PC { crd::u32 nout; crd::u32 rowsize; crd::u32 mcount; crd::u32 pad; } pc{};
        pc.nout                    = static_cast<crd::u32>(out_numel);
        pc.rowsize                 = static_cast<crd::u32>(base_numel / static_cast<crd::u64>(bn.shape.dims[0]));
        pc.mcount                  = static_cast<crd::u32>(idx_numel);
        const crd::u64 in_bytes[3] = {base_numel * sizeof(float), idx_numel * sizeof(float), upd_numel * sizeof(float)};
        const crd::u32 groups      = (static_cast<crd::u32>(out_numel) + 255U) / 256U;
        return dispatch_kernel(*impl.device, *impl.desc_alloc, impl.alloc, crd::containers::to_view(kern.source), 3, kern.input_iidx, in_bytes, out_numel * sizeof(float), &pc, groups, inputs, out);
    }

    if (outn.op == KOp::ScanSum) // inclusive prefix-sum along the trailing axis (one thread per row)
    {
        const bool fast = (outn.tier == DetTier::Fast); // T2 parallel workgroup prefix-sum
        if (fast) { if (!emit_scan_fast_glsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; } }
        else if (!emit_scan_glsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 numel   = static_cast<crd::u64>(outn.shape.numel());
        const crd::u32 scanlen = static_cast<crd::u32>(outn.shape.dims[outn.shape.rank - 1]);
        struct alignas(16) PC { crd::u32 nrows; crd::u32 scanlen; crd::u32 pad[2]; } pc{};
        pc.nrows                   = static_cast<crd::u32>(numel / scanlen);
        pc.scanlen                 = scanlen;
        const crd::u64 in_bytes[1] = {numel * sizeof(float)};
        const crd::u32 groups      = fast ? pc.nrows : (pc.nrows + 255U) / 256U; // T2: 1 WG/row
        return dispatch_kernel(*impl.device, *impl.desc_alloc, impl.alloc, crd::containers::to_view(kern.source), 1, kern.input_iidx, in_bytes, numel * sizeof(float), &pc, groups, inputs, out);
    }

    // fused-elementwise cone (all same-shape ⇒ every buffer = output numel)
    if (!emit_elementwise_glsl(g, output, impl.alloc, kern) || kern.n_inputs != n_inputs) { return false; }
    const crd::u64 on = static_cast<crd::u64>(outn.shape.numel());
    crd::u64       in_bytes[kMaxKernelInputs];
    for (int i = 0; i < n_inputs; ++i) { in_bytes[i] = on * sizeof(float); }
    struct alignas(16) PC { crd::u32 n; crd::u32 pad[3]; } pc{};
    pc.n                  = static_cast<crd::u32>(on);
    const crd::u32 groups = (static_cast<crd::u32>(on) + 255U) / 256U;
    return dispatch_kernel(*impl.device, *impl.desc_alloc, impl.alloc, crd::containers::to_view(kern.source), n_inputs, kern.input_iidx, in_bytes, on * sizeof(float), &pc, groups, inputs, out);
}

} // namespace crd::kir
