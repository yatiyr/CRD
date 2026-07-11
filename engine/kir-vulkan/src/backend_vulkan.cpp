// backend_vulkan.cpp — Phase 3.1.6 v17-b/v17-i: KirBackendVulkan. Draws a headless Vulkan COMPUTE context from
// crd-gpu-context-vulkan (dedicated compute queue, coopmat2, NO rendering RHI, no swapchain) and dispatches a
// single-kernel graph (fused-elementwise cone / Contract / reduce / gather / scatter / scan) through the ONE unified
// dispatch surface `crd::gpu::VulkanComputeContext` (ADR-0100 — the same surface geometry uses): emit GLSL → compile_glsl
// (SPIR-V) → cache the pipeline (by GLSL hash) → host-visible in/out buffers → recorder dispatch → readback. `precise`
// GLSL + sequential order ⇒ bit-matches the CPU reference. ADR-0098 + ADR-0099 + ADR-0100 (one GPU compute manager).

#include <crd/kir/vulkan/backend_vulkan.hpp>

#include <crd/kir/ckir_glsl.hpp>

#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp> // ADR-0103: GLSL→SPIR-V now owned by the Vulkan backend, not crd-shader

namespace crd::kir
{

// A compiled+cached kernel pipeline. CKIR's use is compile-once/eval-many, so we hold the pipeline (by GLSL-source hash)
// across run() calls — this is the pipeline cache, now living in the consumer (the unified context returns owned pipelines).
struct PipeCacheEntry
{
    crd::u64                                   hash = 0;
    std::unique_ptr<crd::gpu::ComputePipeline> pipe;
    int                                        nb = 0;
};

struct KirBackendVulkan::Impl
{
    crd::memory::IAllocator*                         alloc = nullptr;
    std::unique_ptr<crd::gpu::IGpuContext>           context; // headless Vulkan compute context (owns instance/device)
    std::unique_ptr<crd::gpu::VulkanComputeContext>  compute; // the ONE unified dispatch surface (shared with geometry)
    PipeCacheEntry                                   pcache[64];
    int                                              pcache_count = 0;
    bool                                             ok = false;
};

namespace
{
// FNV-1a over the GLSL source — the pipeline-cache key (also caches the GLSL→SPIR-V compile).
[[nodiscard]] crd::u64 hash_glsl(crd::containers::StringView s) noexcept
{
    crd::u64 h = 1469598103934665603ULL;
    for (crd::usize i = 0; i < s.size(); ++i) { h = (h ^ static_cast<crd::u8>(s[i])) * 1099511628211ULL; }
    return h;
}

// Compile GLSL → SPIR-V → get-or-cache the pipeline → N host-visible inputs (upload) + 1 host-visible output → recorder
// dispatch → host-read barrier → readback. 16-byte push = kernel dims. Bit-exact + on the unified compute surface.
bool dispatch_glsl(crd::gpu::VulkanComputeContext& compute, crd::memory::IAllocator* alloc, PipeCacheEntry* pcache,
                   int* pcache_count, crd::containers::StringView glsl, int n_inputs, const int* input_iidx,
                   const crd::u64* input_bytes, crd::u64 out_bytes, const void* push16, crd::u32 groups,
                   const float* const* inputs, float* out, bool zero_out = false)
{
    const int      nb = n_inputs + 1;
    const crd::u64 h  = hash_glsl(glsl);

    crd::gpu::ComputePipeline* pipe = nullptr;
    for (int i = 0; i < *pcache_count; ++i) { if (pcache[i].hash == h) { pipe = pcache[i].pipe.get(); break; } }
    if (pipe == nullptr)
    {
        const auto cres = crd::gpu::compile_glsl_to_spirv(crd::gpu::ShaderStage::Compute, glsl, "ckir", alloc);
        if (!cres.ok) { return false; }
        auto p = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(cres.spirv.data(), cres.spirv.size()), nb, 16U);
        if (p == nullptr || *pcache_count >= 64) { return false; }
        pcache[*pcache_count].hash = h;
        pcache[*pcache_count].nb   = nb;
        pcache[*pcache_count].pipe = std::move(p);
        pipe                       = pcache[*pcache_count].pipe.get();
        ++(*pcache_count);
    }

    std::unique_ptr<crd::gpu::ComputeBuffer> in_bufs[kMaxKernelInputs];
    for (int i = 0; i < n_inputs; ++i)
    {
        in_bufs[i] = compute.create_buffer(input_bytes[i], crd::gpu::compute_usage::storage, crd::gpu::ComputeMemory::CpuToGpu);
        if (in_bufs[i] == nullptr) { return false; }
        auto* dst = static_cast<float*>(in_bufs[i]->map());
        if (dst == nullptr) { return false; }
        const float*   src = inputs[input_iidx[i]];
        const crd::u64 n   = input_bytes[i] / sizeof(float);
        for (crd::u64 e = 0; e < n; ++e) { dst[e] = src[e]; }
        in_bufs[i]->unmap();
    }
    auto out_buf = compute.create_buffer(out_bytes, crd::gpu::compute_usage::storage, crd::gpu::ComputeMemory::GpuToCpu);
    if (out_buf == nullptr) { return false; }
    if (zero_out) // atomic scatter-add ACCUMULATES into the output — it must start at zero (host-visible ⇒ zero on the CPU)
    {
        auto* z = static_cast<unsigned char*>(out_buf->map());
        if (z == nullptr) { return false; }
        for (crd::u64 e = 0; e < out_bytes; ++e) { z[e] = 0U; }
        out_buf->unmap();
    }

    crd::gpu::ComputeBuffer* binds[kMaxKernelInputs + 1];
    for (int i = 0; i < n_inputs; ++i) { binds[i] = in_bufs[i].get(); }
    binds[n_inputs] = out_buf.get();

    auto& rec = compute.begin();
    rec.dispatch(*pipe, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(binds, static_cast<crd::usize>(nb)), push16,
                 16U, groups > 0U ? groups : 1U, 1U, 1U);
    rec.barrier(*out_buf, crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::HostRead);
    compute.submit_and_wait();

    const auto* rd = static_cast<const float*>(out_buf->map());
    if (rd == nullptr) { return false; }
    const crd::u64 on = out_bytes / sizeof(float);
    for (crd::u64 e = 0; e < on; ++e) { out[e] = rd[e]; }
    out_buf->unmap();
    return true;
}

// `graph_uses_vec` now lives (inline) in `ckir_glsl.hpp` — it was duplicated here and in backend_dx12.cpp as an
// external-linkage symbol, i.e. an ODR violation that only stayed quiet because no program links both backends.
} // namespace

KirBackendVulkan::KirBackendVulkan(crd::memory::IAllocator* alloc) : m_impl(std::make_unique<Impl>())
{
    auto& impl = *m_impl;
    impl.alloc = alloc;
    crd::gpu::GpuContextConfig cfg{};
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    impl.context = crd::gpu::create_vulkan_gpu_context(cfg);
    if (impl.context == nullptr) { return; }
    auto* vk     = static_cast<crd::gpu::VulkanGpuContext*>(impl.context.get()); // backend()==Vulkan ⇒ safe
    impl.compute = std::make_unique<crd::gpu::VulkanComputeContext>(*vk, alloc);
    if (!impl.compute->valid()) { return; }
    impl.ok = true;
}

KirBackendVulkan::~KirBackendVulkan() = default;

bool KirBackendVulkan::valid() const noexcept { return m_impl->ok; }
int  KirBackendVulkan::validation_errors() const noexcept { return 0; } // validation layer optional on the context

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
        return dispatch_glsl(*impl.compute, impl.alloc, impl.pcache, &impl.pcache_count, crd::containers::to_view(kern.source),n_inputs, kern.input_iidx, in_bytes, out_bytes, &pc, groups, inputs, out);
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
        // T2 FAST tiled GEMM (FMA, transposed-A shared) — the ported crush schedule; used when the node is Fast-tier and
        // the dims are 128×128×8-tileable single-batch. Grid = (M/128)*(N/128) fills the GPU at N≥1024 (small-N ⇒ split-K,
        // task #11). The naive `precise` kernel stays the T1/default (bit-exact, L2-competitive).
        if (outn.tier == DetTier::Fast && batch == 1U && mm % 128U == 0U && nn % 128U == 0U && kk % 8U == 0U
            && emit_contract_fast_glsl(g, output, kern) && kern.n_inputs == n_inputs)
        {
            struct alignas(16) PCf { crd::u32 m, k, n, b; } pcf{mm, kk, nn, 1U};
            const crd::u64 inbf[2]  = {static_cast<crd::u64>(mm) * kk * sizeof(float), static_cast<crd::u64>(kk) * nn * sizeof(float)};
            const crd::u64 outbf    = static_cast<crd::u64>(mm) * nn * sizeof(float);
            const crd::u32 grpf     = (mm / 128U) * (nn / 128U);
            return dispatch_glsl(*impl.compute, impl.alloc, impl.pcache, &impl.pcache_count, crd::containers::to_view(kern.source),2, kern.input_iidx, inbf, outbf, &pcf, grpf, inputs, out);
        }
        (void) batch;
        if (!emit_contract_glsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        struct alignas(16) PC { crd::u32 m, k, n, b; } pc{mm, kk, nn, batch};
        const crd::u64 in_bytes[2] = {static_cast<crd::u64>(mm) * kk * batch * sizeof(float), static_cast<crd::u64>(kk) * nn * batch * sizeof(float)};
        const crd::u64 out_bytes   = static_cast<crd::u64>(mm) * nn * batch * sizeof(float);
        const crd::u32 groups      = (mm * nn * batch + 255U) / 256U;
        return dispatch_glsl(*impl.compute, impl.alloc, impl.pcache, &impl.pcache_count, crd::containers::to_view(kern.source),2, kern.input_iidx, in_bytes, out_bytes, &pc, groups, inputs, out);
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
        return dispatch_glsl(*impl.compute, impl.alloc, impl.pcache, &impl.pcache_count, crd::containers::to_view(kern.source),1, kern.input_iidx, in_bytes, out_numel * sizeof(float), &pc, groups, inputs, out);
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
        return dispatch_glsl(*impl.compute, impl.alloc, impl.pcache, &impl.pcache_count, crd::containers::to_view(kern.source),2, kern.input_iidx, in_bytes, out_numel * sizeof(float), &pc, groups, inputs, out);
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
        return dispatch_glsl(*impl.compute, impl.alloc, impl.pcache, &impl.pcache_count, crd::containers::to_view(kern.source),3, kern.input_iidx, in_bytes, out_numel * sizeof(float), &pc, groups, inputs, out);
    }

    if (outn.op == KOp::ScatterAdd) // atomic histogram: out[M]=0, then atomicAdd(out[idx[i]], upd[i]) over N inputs
    {
        if (!emit_scatteradd_glsl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64          nin  = static_cast<crd::u64>(g.node(outn.a).shape.numel()); // N inputs
        const crd::u64          mbin = static_cast<crd::u64>(outn.shape.numel());           // M bins (output)
        struct alignas(16) PC { crd::u32 n, p0, p1, p2; } pc{static_cast<crd::u32>(nin), 0U, 0U, 0U};
        const crd::u64 in_bytes[2] = {nin * sizeof(float), nin * sizeof(float)};
        const crd::u32 groups      = (static_cast<crd::u32>(nin) + 255U) / 256U;
        return dispatch_glsl(*impl.compute, impl.alloc, impl.pcache, &impl.pcache_count, crd::containers::to_view(kern.source), 2, kern.input_iidx, in_bytes, mbin * sizeof(float), &pc, groups, inputs, out, true);
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
        return dispatch_glsl(*impl.compute, impl.alloc, impl.pcache, &impl.pcache_count, crd::containers::to_view(kern.source),1, kern.input_iidx, in_bytes, numel * sizeof(float), &pc, groups, inputs, out);
    }

    // A3: vector/matrix elementwise cone → comps-aware emitter (interleaved I/O: comps floats per element).
    if (graph_uses_vec(g, output, impl.alloc))
    {
        if (!emit_vec_glsl(g, output, impl.alloc, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64          onum = static_cast<crd::u64>(outn.shape.numel()); // element count (comps is separate)
        crd::u64                in_bytes[kMaxKernelInputs];
        for (int i = 0; i < n_inputs; ++i) { in_bytes[i] = onum * static_cast<crd::u64>(kern.in_comps[i]) * sizeof(float); }
        struct alignas(16) PC { crd::u32 nn; crd::u32 pad[3]; } pc{static_cast<crd::u32>(onum), {0U, 0U, 0U}};
        const crd::u32 groups = (static_cast<crd::u32>(onum) + 255U) / 256U;
        return dispatch_glsl(*impl.compute, impl.alloc, impl.pcache, &impl.pcache_count, crd::containers::to_view(kern.source), n_inputs, kern.input_iidx, in_bytes, onum * static_cast<crd::u64>(kern.out_comps) * sizeof(float), &pc, groups, inputs, out);
    }

    // fused-elementwise cone (all same-shape ⇒ every buffer = output numel)
    if (!emit_elementwise_glsl(g, output, impl.alloc, kern) || kern.n_inputs != n_inputs) { return false; }
    const crd::u64 on = static_cast<crd::u64>(outn.shape.numel());
    crd::u64       in_bytes[kMaxKernelInputs];
    for (int i = 0; i < n_inputs; ++i) { in_bytes[i] = on * sizeof(float); }
    struct alignas(16) PC { crd::u32 n; crd::u32 pad[3]; } pc{};
    pc.n                  = static_cast<crd::u32>(on);
    const crd::u32 groups = (static_cast<crd::u32>(on) + 255U) / 256U;
    return dispatch_glsl(*impl.compute, impl.alloc, impl.pcache, &impl.pcache_count, crd::containers::to_view(kern.source),n_inputs, kern.input_iidx, in_bytes, on * sizeof(float), &pc, groups, inputs, out);
}

namespace
{
constexpr int kMaxGraphNodes = 512;

// Clone the kernel cone rooted at `root` into `kg`. A MATERIALIZED node (≠ root) becomes an Input leaf and its original
// id is recorded in boundary_by_iidx[the leaf's iidx]; fusable interior nodes are cloned (kernel fusion). Returns the
// mini-graph root id.
int build_mini(const KGraph& g, int orig, const crd::u8* materialized, int root, KGraph& kg, int* map,
               int* boundary_by_iidx, int& n_bnd)
{
    if (map[orig] >= 0) { return map[orig]; }
    if (materialized[orig] != 0 && orig != root)
    {
        const KNode& on           = g.node(orig);
        const int    id           = kg.input(on.shape, on.dtype()); // iidx == n_bnd (both count Inputs in creation order)
        boundary_by_iidx[n_bnd++] = orig;
        map[orig]                 = id;
        return id;
    }
    KNode n = g.node(orig); // copy op/shape/dtype/axes/cval/iidx; remap operands into the mini-graph
    if (n.a >= 0) { n.a = build_mini(g, n.a, materialized, root, kg, map, boundary_by_iidx, n_bnd); }
    if (n.b >= 0) { n.b = build_mini(g, n.b, materialized, root, kg, map, boundary_by_iidx, n_bnd); }
    if (n.c >= 0) { n.c = build_mini(g, n.c, materialized, root, kg, map, boundary_by_iidx, n_bnd); }
    if (n.d >= 0) { n.d = build_mini(g, n.d, materialized, root, kg, map, boundary_by_iidx, n_bnd); }
    const int id = kg.clone(n);
    map[orig]    = id;
    return id;
}
} // namespace

bool KirBackendVulkan::run_graph(const KGraph& g, int output, const float* const* inputs, int /*n_inputs*/, float* out)
{
    using crd::gpu::ComputeAccess;
    using crd::gpu::ComputeMemory;
    auto&     impl = *m_impl;
    const int n    = g.size();
    if (!impl.ok || n > kMaxGraphNodes) { return false; }

    // 1) reachability from the output.
    crd::u8 reach[kMaxGraphNodes] = {};
    int     stk[kMaxGraphNodes * 4];
    int     sp    = 0;
    stk[sp++]     = output;
    while (sp > 0)
    {
        const int i = stk[--sp];
        if (reach[i] != 0) { continue; }
        reach[i]        = 1;
        const KNode& nd = g.node(i);
        if (nd.a >= 0) { stk[sp++] = nd.a; }
        if (nd.b >= 0) { stk[sp++] = nd.b; }
        if (nd.c >= 0) { stk[sp++] = nd.c; }
        if (nd.d >= 0) { stk[sp++] = nd.d; }
    }

    // 2) materialize graph Inputs, non-fusable ops, and the operands of non-fusable ops (elementwise cones fuse between).
    crd::u8 mat[kMaxGraphNodes] = {};
    for (int i = 0; i < n; ++i)
    {
        if (reach[i] == 0) { continue; }
        const KNode& nd = g.node(i);
        if (nd.op == KOp::Input) { mat[i] = 1; }
        if (!glsl_detail::is_fusable(nd.op))
        {
            mat[i] = 1;
            if (nd.a >= 0) { mat[nd.a] = 1; }
            if (nd.b >= 0) { mat[nd.b] = 1; }
            if (nd.c >= 0) { mat[nd.c] = 1; }
            if (nd.d >= 0) { mat[nd.d] = 1; }
        }
    }
    mat[output] = 1;

    // 3) one GPU buffer per materialized node (Input=upload / output=readback / else on-GPU intermediate).
    std::unique_ptr<crd::gpu::ComputeBuffer> bufs[kMaxGraphNodes];
    for (int i = 0; i < n; ++i)
    {
        if (reach[i] == 0 || mat[i] == 0) { continue; }
        const crd::u64      bytes = static_cast<crd::u64>(g.node(i).shape.numel()) * sizeof(float);
        const ComputeMemory feed  = g.node(i).op == KOp::Input ? ComputeMemory::CpuToGpu : ComputeMemory::GpuOnly;
        const ComputeMemory mem   = (i == output) ? ComputeMemory::GpuToCpu : feed;
        bufs[i]                   = impl.compute->create_buffer(bytes, crd::gpu::compute_usage::storage, mem);
        if (bufs[i] == nullptr) { return false; }
    }
    for (int i = 0; i < n; ++i) // upload the graph inputs
    {
        if (reach[i] == 0 || g.node(i).op != KOp::Input) { continue; }
        auto* dst = static_cast<float*>(bufs[i]->map());
        if (dst == nullptr) { return false; }
        const float*   src = inputs[g.node(i).iidx];
        const crd::u64 ne  = static_cast<crd::u64>(g.node(i).shape.numel());
        for (crd::u64 e = 0; e < ne; ++e) { dst[e] = src[e]; }
        bufs[i]->unmap();
    }

    // 4) record every kernel into ONE command buffer (ascending node id = topo order; intermediates stay on-GPU).
    std::unique_ptr<crd::gpu::ComputePipeline> pipes[kMaxGraphNodes];
    int                                        npipes = 0;
    auto&                                      rec    = impl.compute->begin();
    for (int r = 0; r < n; ++r)
    {
        if (reach[r] == 0 || mat[r] == 0 || g.node(r).op == KOp::Input) { continue; } // Inputs are uploaded, no kernel
        KGraph kg(impl.alloc);
        int    map[kMaxGraphNodes];
        for (int i = 0; i < n; ++i) { map[i] = -1; }
        int       boundary[kMaxKernelInputs];
        int       n_bnd = 0;
        const int mroot = build_mini(g, r, mat, r, kg, map, boundary, n_bnd);

        GlslKernel      kern(impl.alloc);
        const KNode&    rn        = g.node(r);
        crd::u32        push[4]   = {0, 0, 0, 0};
        crd::u32        groups    = 1;
        const crd::u64  out_numel = static_cast<crd::u64>(rn.shape.numel());
        bool            ok        = false;
        if (glsl_detail::is_fusable(rn.op))
        {
            ok      = emit_elementwise_glsl(kg, mroot, impl.alloc, kern);
            push[0] = static_cast<crd::u32>(out_numel);
            groups  = (static_cast<crd::u32>(out_numel) + 255U) / 256U;
        }
        else if (rn.op == KOp::ScanSum)
        {
            ok                    = emit_scan_glsl(kg, mroot, kern);
            const crd::u32 slen   = static_cast<crd::u32>(rn.shape.dims[rn.shape.rank - 1]);
            const crd::u32 nrows  = static_cast<crd::u32>(out_numel / slen);
            push[0]               = nrows;
            push[1]               = slen;
            groups                = (nrows + 255U) / 256U;
        }
        else if (rn.op == KOp::ScatterAdd)
        {
            ok                   = emit_scatteradd_glsl(kg, mroot, kern);
            const crd::u32 nin   = static_cast<crd::u32>(g.node(rn.a).shape.numel());
            push[0]              = nin;
            groups               = (nin + 255U) / 256U;
        }
        else if (is_reduce(rn.op))
        {
            ok                     = emit_reduce_glsl(kg, mroot, kern);
            const crd::u64 nout    = out_numel;
            const crd::u64 redsize = static_cast<crd::u64>(g.node(rn.a).shape.numel()) / (nout > 0U ? nout : 1U);
            push[0]                = static_cast<crd::u32>(nout);
            push[1]                = static_cast<crd::u32>(redsize);
            groups                 = (static_cast<crd::u32>(nout) + 255U) / 256U;
        }
        else if (rn.op == KOp::Broadcast)
        {
            ok      = emit_broadcast_glsl(kg, mroot, kern);
            push[0] = static_cast<crd::u32>(out_numel);
            groups  = (static_cast<crd::u32>(out_numel) + 255U) / 256U;
        }
        else if (rn.op == KOp::Iota)
        {
            ok      = emit_iota_glsl(kg, mroot, kern);
            push[0] = static_cast<crd::u32>(out_numel);
            groups  = (static_cast<crd::u32>(out_numel) + 255U) / 256U;
        }
        else if (rn.op == KOp::Scatter)
        {
            ok                     = emit_scatter_glsl(kg, mroot, kern);
            const KNode&   base    = g.node(rn.a);
            const crd::u64 rowsize = static_cast<crd::u64>(base.shape.numel()) / static_cast<crd::u64>(base.shape.dims[0]);
            const crd::u64 mcount  = static_cast<crd::u64>(g.node(rn.b).shape.numel());
            push[0]                = static_cast<crd::u32>(out_numel);
            push[1]                = static_cast<crd::u32>(rowsize);
            push[2]                = static_cast<crd::u32>(mcount);
            groups                 = (static_cast<crd::u32>(out_numel) + 255U) / 256U;
        }
        else { return false; } // gather/contract — added when a technique needs them
        if (!ok) { return false; }

        const auto cres = crd::gpu::compile_glsl_to_spirv(crd::gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir", impl.alloc);
        if (!cres.ok) { return false; }
        auto p = impl.compute->create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(cres.spirv.data(), cres.spirv.size()), kern.n_inputs + 1, 16U);
        if (p == nullptr) { return false; }

        crd::gpu::ComputeBuffer* binds[kMaxKernelInputs + 1];
        for (int b = 0; b < kern.n_inputs; ++b) { binds[b] = bufs[boundary[kern.input_iidx[b]]].get(); }
        binds[kern.n_inputs] = bufs[r].get();
        rec.dispatch(*p, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(binds, static_cast<crd::usize>(kern.n_inputs + 1)), push, 16U,
                     groups > 0U ? groups : 1U, 1U, 1U);
        rec.barrier(*bufs[r], ComputeAccess::ShaderWrite, (r == output) ? ComputeAccess::HostRead : ComputeAccess::ShaderRead);
        pipes[npipes++] = std::move(p);
    }
    impl.compute->submit_and_wait();

    const auto* rd = static_cast<const float*>(bufs[output]->map());
    if (rd == nullptr) { return false; }
    const crd::u64 on = static_cast<crd::u64>(g.node(output).shape.numel());
    for (crd::u64 e = 0; e < on; ++e) { out[e] = rd[e]; }
    bufs[output]->unmap();
    (void) npipes;
    return true;
}

} // namespace crd::kir
