// backend_cuda.cpp — Phase 3.1.6 v17-c: KirBackendCuda over the CUDA driver API + NVRTC. Owns a CUDA context + stream;
// per kernel: emit CUDA C → (module CACHE by source hash — skip the NVRTC recompile on repeat) → HtoDAsync into a
// persistent device-buffer POOL → cuLaunchKernel on the stream → DtoHAsync → stream sync. The v17-c persistent path:
// NVRTC compile (~50–200ms) happens ONCE per distinct kernel, allocations are reused, and work rides a stream.
// Bit-exact vs the CPU reference for correctly-rounded ops (incl. division). ADR-0098.

#include <crd/kir/cuda/backend_cuda.hpp>

#include <crd/kir/ckir_cuda.hpp>

#include <crd/containers/array.hpp>

#include <cstdio>
#include <cstring>

#include <cuda.h>
#include <nvrtc.h>

namespace crd::kir
{

namespace
{
constexpr int kMaxIn    = 32;
constexpr int kCacheMax = 256;
// FNV-1a over the emitted source — the module-cache key (identical kernels ⇒ one NVRTC compile, then reuse).
crd::u64 hash_src(const char* s)
{
    crd::u64 h = 1469598103934665603ULL;
    for (; *s != '\0'; ++s) { h = (h ^ static_cast<crd::u8>(*s)) * 1099511628211ULL; }
    return h;
}
struct CacheEntry
{
    crd::u64   hash = 0;
    CUmodule   mod  = nullptr;
    CUfunction fn   = nullptr;
};
} // namespace

struct KirBackendCuda::Impl
{
    crd::memory::IAllocator* alloc = nullptr;
    CUdevice                 device = 0;
    CUcontext                ctx = nullptr;
    CUstream                 stream = nullptr;
    char                     arch[16] = {}; // "sm_89" — the GPU's exact arch (CUBIN target, avoids PTX-JIT version issues)
    bool                     ok = false;
    // persistent module cache (source-hash keyed) — skips the NVRTC recompile on repeat runs of the same kernel
    CacheEntry cache[kCacheMax];
    int        cache_n = 0;
    // persistent device-buffer pool — grow-on-demand, reused across calls (no per-call cuMemAlloc/Free churn)
    CUdeviceptr pool_in[kMaxIn]       = {};
    crd::u64    pool_in_cap[kMaxIn]   = {};
    CUdeviceptr pool_out              = 0;
    crd::u64    pool_out_cap          = 0;
};

namespace
{
// NVRTC: compile CUDA C (null-terminated `src`) → a CUBIN for the GPU's exact `arch` (e.g. "sm_89"), into `cubin`.
// CUBIN loads directly (no PTX JIT) ⇒ no unsupported-PTX-version when the driver is older than the toolkit.
// Deterministic flags (--fmad=false + correctly-rounded div/sqrt). false on failure (dumps the NVRTC log).
bool compile_cubin(const char* src, const char* arch, crd::containers::Array<char>& cubin)
{
    nvrtcProgram prog = nullptr;
    if (nvrtcCreateProgram(&prog, src, "ckir.cu", 0, nullptr, nullptr) != NVRTC_SUCCESS) { return false; }
    char archopt[32];
    std::snprintf(archopt, sizeof(archopt), "--gpu-architecture=%s", arch);
    const char*       opts[] = {"--fmad=false", "--prec-div=true", "--prec-sqrt=true", archopt};
    const nvrtcResult r      = nvrtcCompileProgram(prog, 4, opts);
    if (r != NVRTC_SUCCESS)
    {
        size_t logsz = 0;
        nvrtcGetProgramLogSize(prog, &logsz);
        if (logsz > 1)
        {
            crd::containers::Array<char> log(cubin.allocator());
            log.resize(logsz, '\0');
            nvrtcGetProgramLog(prog, log.data());
            std::fprintf(stderr, "[ckir-cuda nvrtc] %s\nsrc:\n%s\n", log.data(), src);
        }
        nvrtcDestroyProgram(&prog);
        return false;
    }
    size_t sz = 0;
    if (nvrtcGetCUBINSize(prog, &sz) != NVRTC_SUCCESS || sz == 0) { nvrtcDestroyProgram(&prog); return false; }
    cubin.resize(sz, '\0');
    const nvrtcResult gr = nvrtcGetCUBIN(prog, cubin.data());
    nvrtcDestroyProgram(&prog);
    return gr == NVRTC_SUCCESS;
}

// launch: async-upload inputs into the persistent pool buffers, run a (gx,gy,1)×(bx,1,1) grid on the stream, async
// read the output back, sync the stream. Buffers persist (no free here — the pool owns them). Any CUDA error ⇒ false.
bool launch_and_readback(CUstream stream, CUfunction fn, crd::u32 gx, crd::u32 gy, crd::u32 bx, void** params,
                         CUdeviceptr* d_in, const crd::u64* in_bytes, int n_inputs, const int* input_iidx,
                         const float* const* inputs, CUdeviceptr d_out, crd::u64 out_bytes, float* out)
{
    for (int i = 0; i < n_inputs; ++i)
    {
        if (cuMemcpyHtoDAsync(d_in[i], inputs[input_iidx[i]], in_bytes[i], stream) != CUDA_SUCCESS) { return false; }
    }
    if (cuLaunchKernel(fn, gx > 0U ? gx : 1U, gy > 0U ? gy : 1U, 1U, bx, 1U, 1U, 0U, stream, params, nullptr) != CUDA_SUCCESS) { return false; }
    if (cuMemcpyDtoHAsync(out, d_out, out_bytes, stream) != CUDA_SUCCESS) { return false; }
    return cuStreamSynchronize(stream) == CUDA_SUCCESS;
}
} // namespace

KirBackendCuda::KirBackendCuda(crd::memory::IAllocator* alloc) : m_impl(std::make_unique<Impl>())
{
    auto& impl = *m_impl;
    impl.alloc = alloc;
    if (cuInit(0) != CUDA_SUCCESS) { return; }
    int count = 0;
    if (cuDeviceGetCount(&count) != CUDA_SUCCESS || count == 0) { return; }
    if (cuDeviceGet(&impl.device, 0) != CUDA_SUCCESS) { return; }
    // primary context (recommended; avoids the v2/v4 cuCtxCreate signature churn)
    if (cuDevicePrimaryCtxRetain(&impl.ctx, impl.device) != CUDA_SUCCESS) { return; }
    if (cuCtxSetCurrent(impl.ctx) != CUDA_SUCCESS) { return; }
    int major = 0;
    int minor = 0;
    cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, impl.device);
    cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, impl.device);
    std::snprintf(impl.arch, sizeof(impl.arch), "sm_%d%d", major, minor);
    if (cuStreamCreate(&impl.stream, CU_STREAM_NON_BLOCKING) != CUDA_SUCCESS) { return; }
    impl.ok = true;
}

KirBackendCuda::~KirBackendCuda()
{
    auto& impl = *m_impl;
    if (!impl.ok) { return; }
    for (int i = 0; i < impl.cache_n; ++i) { cuModuleUnload(impl.cache[i].mod); }
    for (int i = 0; i < kMaxIn; ++i) { if (impl.pool_in[i] != 0) { cuMemFree(impl.pool_in[i]); } }
    if (impl.pool_out != 0) { cuMemFree(impl.pool_out); }
    if (impl.stream != nullptr) { cuStreamDestroy(impl.stream); }
    cuDevicePrimaryCtxRelease(impl.device);
}

bool KirBackendCuda::valid() const noexcept { return m_impl->ok; }

bool KirBackendCuda::run(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out)
{
    auto& impl = *m_impl;
    if (!impl.ok || n_inputs > kMaxIn) { return false; }
    const KNode& outn = g.node(output);

    // 1. emit CUDA C + derive dims/sizes/params per kernel type
    GlslKernel kern(impl.alloc);
    crd::u64   in_bytes[kMaxIn] = {};
    crd::u64   out_bytes        = 0;
    crd::u32   groups           = 0;
    crd::u32   d0 = 0, d1 = 0, d2 = 0, d3 = 0; // scalar kernel args (n / M,K,N,batch / nout,redsize)
    bool       tiled = false;                  // WarpTiled Contract schedule chosen (2D grid, M,N,K arg order)
    bool       fused = false;                  // GEMM+epilogue fusion chosen (extra bias params, epilogue in the store)
    crd::u32   tgx = 0, tgy = 0, tbx = 0;

    // FUSION FIRST: if the output is an elementwise epilogue over a WarpTiled-eligible Contract, compile it to ONE
    // fused kernel (bias+activation in the C write — the structural crush the vendor can't fuse). Else fall through.
    const FuseInfo fuse = detect_fuse(g, output, impl.alloc);
    if (fuse.ok)
    {
        const TileSchedule sch = select_schedule(g, fuse.contract);
        if (sch.kind == Sched::WarpTiled && emit_contract_tiled_fused_cuda(g, output, fuse.contract, sch, fuse, impl.alloc, kern) && kern.n_inputs == n_inputs)
        {
            const KNode& cn = g.node(fuse.contract);
            const KNode& an = g.node(cn.a);
            const KNode& bn = g.node(cn.b);
            const int    r  = an.shape.rank;
            d0              = static_cast<crd::u32>(an.shape.dims[r - 2]); // M
            d1              = static_cast<crd::u32>(an.shape.dims[r - 1]); // K
            d2              = static_cast<crd::u32>(bn.shape.dims[bn.shape.rank - 1]); // N
            in_bytes[0]     = static_cast<crd::u64>(d0) * d1 * sizeof(float);
            in_bytes[1]     = static_cast<crd::u64>(d1) * d2 * sizeof(float);
            for (int j = 0; j < fuse.n_bias; ++j) { in_bytes[2 + j] = static_cast<crd::u64>(d2) * sizeof(float); }
            out_bytes = static_cast<crd::u64>(d0) * d2 * sizeof(float);
            tiled     = true;
            fused     = true;
            tgx       = d2 / static_cast<crd::u32>(sch.bn);
            tgy       = d0 / static_cast<crd::u32>(sch.bm);
            tbx       = static_cast<crd::u32>(sch.nt);
        }
    }

    if (fused) {} // kernel already emitted above
    else if (outn.op == KOp::Contract)
    {
        const KNode& an = g.node(outn.a);
        const KNode& bn = g.node(outn.b);
        const int    r  = an.shape.rank;
        d0              = static_cast<crd::u32>(an.shape.dims[r - 2]); // M
        d1              = static_cast<crd::u32>(an.shape.dims[r - 1]); // K
        d2              = static_cast<crd::u32>(bn.shape.dims[bn.shape.rank - 1]); // N
        d3              = 1U;
        for (int k = 0; k < r - 2; ++k) { d3 *= static_cast<crd::u32>(an.shape.dims[k]); }
        in_bytes[0] = static_cast<crd::u64>(d0) * d1 * d3 * sizeof(float);
        in_bytes[1] = static_cast<crd::u64>(d1) * d2 * d3 * sizeof(float);
        out_bytes   = static_cast<crd::u64>(d0) * d2 * d3 * sizeof(float);
        groups      = (d0 * d2 * d3 + 255U) / 256U;
        // Graph→Tile lowering: the v17-e warp-tiled schedule when its shape constraints hold; else naive (bit-exact).
        const TileSchedule sch = select_schedule(g, output);
        if (sch.kind == Sched::WarpTiled && emit_contract_tiled_cuda(g, output, sch, kern) && kern.n_inputs == n_inputs)
        {
            tiled = true;
            tgx   = d2 / static_cast<crd::u32>(sch.bn); // N / BN
            tgy   = d0 / static_cast<crd::u32>(sch.bm); // M / BM
            tbx   = static_cast<crd::u32>(sch.nt);
        }
        else if (!emit_contract_cuda(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
    }
    else if (is_reduce(outn.op))
    {
        const bool fast = (outn.tier == DetTier::Fast && is_fast_reduceable(outn.op)); // T2 parallel block tree-reduce
        if (fast) { if (!emit_reduce_fast_cuda(g, output, kern) || kern.n_inputs != n_inputs) { return false; } }
        else if (!emit_reduce_cuda(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 in_numel  = static_cast<crd::u64>(g.node(outn.a).shape.numel());
        const crd::u64 out_numel = static_cast<crd::u64>(outn.shape.numel());
        d0                       = static_cast<crd::u32>(out_numel);          // nout
        d1                       = static_cast<crd::u32>(in_numel / out_numel); // redsize
        in_bytes[0]              = in_numel * sizeof(float);
        out_bytes                = out_numel * sizeof(float);
        groups                   = fast ? static_cast<crd::u32>(out_numel) : (static_cast<crd::u32>(out_numel) + 255U) / 256U; // T2: 1 block/output
    }
    else if (outn.op == KOp::Gather)
    {
        if (!emit_gather_cuda(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const KNode&   dn         = g.node(outn.a);
        const crd::u64 out_numel  = static_cast<crd::u64>(outn.shape.numel());
        const crd::u64 data_numel = static_cast<crd::u64>(dn.shape.numel());
        const crd::u64 idx_numel  = static_cast<crd::u64>(g.node(outn.b).shape.numel());
        d0                        = static_cast<crd::u32>(out_numel);                                     // nout
        d1                        = static_cast<crd::u32>(data_numel / static_cast<crd::u64>(dn.shape.dims[0])); // rowsize
        in_bytes[0]               = data_numel * sizeof(float);
        in_bytes[1]               = idx_numel * sizeof(float);
        out_bytes                 = out_numel * sizeof(float);
        groups                    = (static_cast<crd::u32>(out_numel) + 255U) / 256U;
    }
    else if (outn.op == KOp::Scatter)
    {
        if (!emit_scatter_cuda(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const KNode&   bn         = g.node(outn.a);
        const crd::u64 out_numel  = static_cast<crd::u64>(outn.shape.numel());
        const crd::u64 base_numel = static_cast<crd::u64>(bn.shape.numel());
        const crd::u64 idx_numel  = static_cast<crd::u64>(g.node(outn.b).shape.numel());
        const crd::u64 upd_numel  = static_cast<crd::u64>(g.node(outn.c).shape.numel());
        d0                        = static_cast<crd::u32>(out_numel);                                     // nout
        d1                        = static_cast<crd::u32>(base_numel / static_cast<crd::u64>(bn.shape.dims[0])); // rowsize
        d2                        = static_cast<crd::u32>(idx_numel);                                     // M
        in_bytes[0]               = base_numel * sizeof(float);
        in_bytes[1]               = idx_numel * sizeof(float);
        in_bytes[2]               = upd_numel * sizeof(float);
        out_bytes                 = out_numel * sizeof(float);
        groups                    = (static_cast<crd::u32>(out_numel) + 255U) / 256U;
    }
    else if (outn.op == KOp::ScanSum)
    {
        const bool fast = (outn.tier == DetTier::Fast); // T2 parallel block prefix-sum
        if (fast) { if (!emit_scan_fast_cuda(g, output, kern) || kern.n_inputs != n_inputs) { return false; } }
        else if (!emit_scan_cuda(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 numel   = static_cast<crd::u64>(outn.shape.numel());
        const crd::u32 scanlen = static_cast<crd::u32>(outn.shape.dims[outn.shape.rank - 1]);
        d0                     = static_cast<crd::u32>(numel / scanlen); // nrows
        d1                     = scanlen;
        in_bytes[0]            = numel * sizeof(float);
        out_bytes              = numel * sizeof(float);          // scan KEEPS the shape
        groups                 = fast ? d0 : (d0 + 255U) / 256U; // T2: 1 block/row
    }
    else
    {
        if (!emit_elementwise_cuda(g, output, impl.alloc, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 on = static_cast<crd::u64>(outn.shape.numel());
        d0                = static_cast<crd::u32>(on); // n
        for (int i = 0; i < n_inputs; ++i) { in_bytes[i] = on * sizeof(float); }
        out_bytes = on * sizeof(float);
        groups    = (static_cast<crd::u32>(on) + 255U) / 256U;
    }

    // 2. compile + load — MODULE CACHE (source-hash keyed): skip the NVRTC recompile on repeat runs of this kernel
    const crd::u64 khash = hash_src(kern.source.c_str());
    CUfunction     fn    = nullptr;
    for (int i = 0; i < impl.cache_n; ++i) { if (impl.cache[i].hash == khash) { fn = impl.cache[i].fn; break; } }
    if (fn == nullptr)
    {
        crd::containers::Array<char> cubin(impl.alloc);
        if (!compile_cubin(kern.source.c_str(), impl.arch, cubin)) { return false; }
        CUmodule       mod = nullptr;
        const CUresult ldr = cuModuleLoadData(&mod, cubin.data());
        if (ldr != CUDA_SUCCESS) { std::fprintf(stderr, "[ckir-cuda] cuModuleLoadData failed: %d\n", static_cast<int>(ldr)); return false; }
        if (cuModuleGetFunction(&fn, mod, "ckir") != CUDA_SUCCESS) { std::fprintf(stderr, "[ckir-cuda] cuModuleGetFunction failed\n"); cuModuleUnload(mod); return false; }
        if (impl.cache_n < kCacheMax) { impl.cache[impl.cache_n++] = CacheEntry{khash, mod, fn}; }
        else { cuModuleUnload(mod); } // cache full: use once, don't leak (rare — 256 distinct kernels)
    }

    // 3. device memory — persistent POOL: grow a buffer only when the needed size exceeds its capacity, else reuse
    bool alloc_ok = true;
    for (int i = 0; i < n_inputs && alloc_ok; ++i)
    {
        if (impl.pool_in_cap[i] < in_bytes[i])
        {
            if (impl.pool_in[i] != 0) { cuMemFree(impl.pool_in[i]); impl.pool_in[i] = 0; }
            if (cuMemAlloc(&impl.pool_in[i], in_bytes[i]) != CUDA_SUCCESS) { impl.pool_in_cap[i] = 0; alloc_ok = false; }
            else { impl.pool_in_cap[i] = in_bytes[i]; }
        }
    }
    if (alloc_ok && impl.pool_out_cap < out_bytes)
    {
        if (impl.pool_out != 0) { cuMemFree(impl.pool_out); impl.pool_out = 0; }
        if (cuMemAlloc(&impl.pool_out, out_bytes) != CUDA_SUCCESS) { impl.pool_out_cap = 0; alloc_ok = false; }
        else { impl.pool_out_cap = out_bytes; }
    }

    // 4. build the kernel-arg pointer array, launch on the stream, read back (buffers persist in the pool)
    if (!alloc_ok) { return false; }
    void* params[kMaxIn + 8];
    int   np = 0;
    if (fused) // fused ckir(A, Bm, C, bias0.., M, N, K)
    {
        params[np++] = &impl.pool_in[0];
        params[np++] = &impl.pool_in[1];
        params[np++] = &impl.pool_out;
        for (int j = 2; j < n_inputs; ++j) { params[np++] = &impl.pool_in[j]; }
        params[np++] = &d0; params[np++] = &d2; params[np++] = &d1;
    }
    else
    {
        for (int i = 0; i < n_inputs; ++i) { params[np++] = &impl.pool_in[i]; }
        params[np++] = &impl.pool_out;
        if (tiled) { params[np++] = &d0; params[np++] = &d2; params[np++] = &d1; } // tiled ckir(A,Bm,C, M,N,K)
        else
        {
            params[np++] = &d0;
            if (outn.op == KOp::Contract) { params[np++] = &d1; params[np++] = &d2; params[np++] = &d3; }
            else if (is_reduce(outn.op)) { params[np++] = &d1; }
            else if (outn.op == KOp::Gather) { params[np++] = &d1; }
            else if (outn.op == KOp::Scatter) { params[np++] = &d1; params[np++] = &d2; }
            else if (outn.op == KOp::ScanSum) { params[np++] = &d1; }
        }
    }
    const crd::u32 gx = tiled ? tgx : groups;
    const crd::u32 gy = tiled ? tgy : 1U;
    const crd::u32 bx = tiled ? tbx : 256U;
    return launch_and_readback(impl.stream, fn, gx, gy, bx, params, impl.pool_in, in_bytes, n_inputs, kern.input_iidx, inputs, impl.pool_out, out_bytes, out);
}

} // namespace crd::kir
