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
// `allow_fma=false` = the DETERMINISTIC flags (--fmad=false + correctly-rounded div/sqrt) ⇒ bit-exact vs the CPU oracle — the
// default for every bit-exact kernel. `allow_fma=true` = the FAST/PERF tier (T1, already ULP-tolerant, NOT bit-exact): enable
// FMA fusion, which nearly DOUBLES GEMM throughput (--fmad=false forces every a*b+c into a separate mul+add — the AS-4 vendor-gap
// cause). Only the fast-tier WarpTiled GEMM opts in. false on failure (dumps the NVRTC log).
bool compile_cubin(const char* src, const char* arch, crd::containers::Array<char>& cubin, bool allow_fma)
{
    nvrtcProgram prog = nullptr;
    if (nvrtcCreateProgram(&prog, src, "ckir.cu", 0, nullptr, nullptr) != NVRTC_SUCCESS) { return false; }
    char archopt[32];
    std::snprintf(archopt, sizeof(archopt), "--gpu-architecture=%s", arch);
    const char*       fast_opts[] = {"--fmad=true", archopt};
    const char*       det_opts[]  = {"--fmad=false", "--prec-div=true", "--prec-sqrt=true", archopt};
    const char* const* opts       = allow_fma ? fast_opts : det_opts;
    const int          nopt       = allow_fma ? 2 : 4;
    const nvrtcResult  r          = nvrtcCompileProgram(prog, nopt, opts);
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
const char* KirBackendCuda::device() const noexcept { return m_impl->arch; } // AS-6a: "sm_89" etc. — the DB device key

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
    crd::u32   d0 = 0; // scalar kernel args (n / M,K,N,batch / nout,redsize)
    crd::u32   d1 = 0;
    crd::u32   d2 = 0;
    crd::u32   d3 = 0;
    bool       tiled = false;                  // WarpTiled Contract schedule chosen (2D grid, M,N,K arg order)
    bool       fused = false;                  // GEMM+epilogue fusion chosen (extra bias params, epilogue in the store)
    bool       fast_fma = false;               // the WarpTiled fast tier (fma=true) ⇒ compile with FMA fusion (AS-4 perf)
    bool       attention = false;              // AS-4: KOp::Attention → the fused flash kernel (Q,K,V,O,S,scale args)
    float      attn_scale = 0.0F;              // the attention scale (1/√d) — a float kernel arg
    crd::u32   attn_bx = 0;                    // flash block size = the query-tile height BR
    crd::u32   tgx = 0;
    crd::u32   tgy = 0;
    crd::u32   tbx = 0;

    // FUSION FIRST: if the output is an elementwise epilogue over a WarpTiled-eligible Contract, compile it to ONE
    // fused kernel (bias+activation in the C write — the structural crush the vendor can't fuse). Else fall through.
    const FuseInfo fuse = detect_fuse(g, output, impl.alloc);
    if (fuse.ok)
    {
        const TileSchedule sch = select_schedule(g, fuse.contract, impl.arch);
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
            fast_fma  = sch.fma;
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
        const TileSchedule sch = select_schedule(g, output, impl.arch);
        if (sch.kind == Sched::WarpTiled && emit_contract_tiled_cuda(g, output, sch, kern) && kern.n_inputs == n_inputs)
        {
            tiled    = true;
            fast_fma = sch.fma;
            tgx      = d2 / static_cast<crd::u32>(sch.bn); // N / BN
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
    // AS-4 FUSION: a KOp::Attention node → the FUSED flash kernel (one tiled online-softmax pass, no S×S in DRAM). The (BR,BC) tile
    // is auto-selected for the head dim; grid = ceil(S/BR) blocks × BR threads. Q,K,V,O are all [S,D].
    else if (outn.op == KOp::Attention)
    {
        const KNode&   qn   = g.node(outn.a);
        const crd::u32 slen = static_cast<crd::u32>(qn.shape.dims[qn.shape.rank - 2]);
        const int      dim  = static_cast<int>(qn.shape.dims[qn.shape.rank - 1]);
        int            br   = 0;
        int            bc   = 0;
        select_attention_tile(dim, static_cast<int>(slen), impl.arch, br, bc); // DB-tuned (BR,BC) for (arch,S,D), else heuristic
        if (!emit_attention_flash_cuda(g, output, br, bc, kern) || kern.n_inputs != n_inputs) { return false; }
        for (int i = 0; i < 3; ++i) { in_bytes[i] = static_cast<crd::u64>(slen) * static_cast<crd::u32>(dim) * sizeof(float); }
        out_bytes  = static_cast<crd::u64>(slen) * static_cast<crd::u32>(dim) * sizeof(float);
        attention  = true;
        attn_scale = static_cast<float>(outn.cval);
        d0         = slen;
        attn_bx    = static_cast<crd::u32>(br);
        groups     = (slen + static_cast<crd::u32>(br) - 1U) / static_cast<crd::u32>(br); // ceil(S/BR) blocks
    }
    // B0 fan-out: a graph carrying vec/mat/bool/struct VALUES routes to the SCALARIZING emitter (interleaved I/O:
    // `comps` floats per element). CUDA has no native vector arithmetic, so each value becomes `comps` scalar temps.
    else if (graph_uses_vec(g, output, impl.alloc))
    {
        if (!emit_vec_cuda(g, output, impl.alloc, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 on = static_cast<crd::u64>(outn.shape.numel());
        d0                = static_cast<crd::u32>(on); // n
        for (int i = 0; i < n_inputs; ++i) { in_bytes[i] = on * static_cast<crd::u64>(kern.in_comps[i]) * sizeof(float); }
        out_bytes = on * static_cast<crd::u64>(kern.out_comps) * sizeof(float);
        groups    = (static_cast<crd::u32>(on) + 255U) / 256U;
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
        if (!compile_cubin(kern.source.c_str(), impl.arch, cubin, fast_fma)) { return false; }
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
    if (attention) // ckir(Q, K, V, O, S, scale)
    {
        params[np++] = &impl.pool_in[0];
        params[np++] = &impl.pool_in[1];
        params[np++] = &impl.pool_in[2];
        params[np++] = &impl.pool_out;
        params[np++] = &d0;         // S
        params[np++] = &attn_scale; // scale (float)
    }
    else if (fused) // fused ckir(A, Bm, C, bias0.., M, N, K)
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
            else if (outn.op == KOp::Scatter) { params[np++] = &d1; params[np++] = &d2; }
            else if (is_reduce(outn.op) || outn.op == KOp::Gather || outn.op == KOp::ScanSum) { params[np++] = &d1; } // these three take just the row size
        }
    }
    crd::u32 gx = groups;
    crd::u32 gy = 1U;
    crd::u32 bx = 256U;
    if (attention) { bx = attn_bx; }        // flash: ceil(S/BR) blocks × BR threads
    else if (tiled) { gx = tgx; gy = tgy; bx = tbx; } // WarpTiled Contract: 2-D grid
    return launch_and_readback(impl.stream, fn, gx, gy, bx, params, impl.pool_in, in_bytes, n_inputs, kern.input_iidx, inputs, impl.pool_out, out_bytes, out);
}

// AS-1b: measure ONE explicit schedule for a Contract. Self-contained (local module + buffers, freed here) so the autotuner can
// sweep hundreds of candidates without polluting run()'s persistent cache. Timing is CUDA-event (GPU timestamps) over `iters`
// launches after `warmup`, min-of-iters — upload/readback are OUTSIDE the timed region. `out` gets the last launch's result.
ContractTiming KirBackendCuda::time_contract_schedule(const KGraph& g, int output, const TileSchedule& sched,
                                                      const float* const* inputs, int n_inputs, float* out, int warmup,
                                                      int iters)
{
    ContractTiming t;
    auto&          impl = *m_impl;
    if (!impl.ok || n_inputs != 2 || iters <= 0) { return t; }
    const KNode& c = g.node(output);
    if (c.op != KOp::Contract) { return t; }
    const KNode&   an = g.node(c.a);
    const KNode&   bn = g.node(c.b);
    const int      r  = an.shape.rank;
    if (r < 2 || bn.shape.rank < 2) { return t; }
    const crd::u32 mm = static_cast<crd::u32>(an.shape.dims[r - 2]);
    const crd::u32 kk = static_cast<crd::u32>(an.shape.dims[r - 1]);
    const crd::u32 nn = static_cast<crd::u32>(bn.shape.dims[bn.shape.rank - 1]);
    crd::u32       batch = 1U;
    for (int k = 0; k < r - 2; ++k) { batch *= static_cast<crd::u32>(an.shape.dims[k]); }

    // 1. emit with the GIVEN schedule (tiled), else the naive baseline
    GlslKernel kern(impl.alloc);
    bool       tiled = false;
    if (sched.kind == Sched::WarpTiled && batch == 1U && emit_contract_tiled_cuda(g, output, sched, kern) && kern.n_inputs == n_inputs)
    {
        tiled = true;
    }
    else if (!emit_contract_cuda(g, output, kern) || kern.n_inputs != n_inputs) { return t; }

    // 2. compile locally (own module — freed at the end; no persistent-cache churn during a sweep). The fast tier (WarpTiled
    // fma=true) compiles WITH FMA fusion — the AS-4 perf lever (--fmad=false halves GEMM throughput).
    crd::containers::Array<char> cubin(impl.alloc);
    if (!compile_cubin(kern.source.c_str(), impl.arch, cubin, tiled && sched.fma)) { return t; }
    CUmodule mod = nullptr;
    if (cuModuleLoadData(&mod, cubin.data()) != CUDA_SUCCESS) { return t; }
    CUfunction fn = nullptr;
    if (cuModuleGetFunction(&fn, mod, "ckir") != CUDA_SUCCESS) { cuModuleUnload(mod); return t; }

    // 3. buffers + upload (outside the timed region)
    const crd::u64 in0_bytes = static_cast<crd::u64>(mm) * kk * batch * sizeof(float);
    const crd::u64 in1_bytes = static_cast<crd::u64>(kk) * nn * batch * sizeof(float);
    const crd::u64 out_bytes = static_cast<crd::u64>(mm) * nn * batch * sizeof(float);
    CUdeviceptr    d_in0 = 0;
    CUdeviceptr    d_in1 = 0;
    CUdeviceptr    d_out = 0;
    bool           mok   = cuMemAlloc(&d_in0, in0_bytes) == CUDA_SUCCESS && cuMemAlloc(&d_in1, in1_bytes) == CUDA_SUCCESS
                       && cuMemAlloc(&d_out, out_bytes) == CUDA_SUCCESS;
    if (mok)
    {
        mok = cuMemcpyHtoD(d_in0, inputs[kern.input_iidx[0]], in0_bytes) == CUDA_SUCCESS
           && cuMemcpyHtoD(d_in1, inputs[kern.input_iidx[1]], in1_bytes) == CUDA_SUCCESS;
    }
    if (mok)
    {
        // 4. params + grid (tiled: ckir(A,Bm,C,M,N,K); naive: ckir(A,Bm,C,M,K,N,batch))
        void*    params[8];
        int      np = 0;
        crd::u32 pm = mm;
        crd::u32 pn = nn;
        crd::u32 pk = kk;
        crd::u32 pb = batch;
        params[np++] = &d_in0;
        params[np++] = &d_in1;
        params[np++] = &d_out;
        if (tiled) { params[np++] = &pm; params[np++] = &pn; params[np++] = &pk; }
        else { params[np++] = &pm; params[np++] = &pk; params[np++] = &pn; params[np++] = &pb; }
        const crd::u32 gx = tiled ? (nn / static_cast<crd::u32>(sched.bn)) : (mm * nn * batch + 255U) / 256U;
        const crd::u32 gy = tiled ? (mm / static_cast<crd::u32>(sched.bm)) : 1U;
        const crd::u32 bx = tiled ? static_cast<crd::u32>(sched.nt) : 256U;

        // 5. warmup, then GPU-event-timed min-of-iters
        for (int w = 0; w < warmup; ++w)
        {
            cuLaunchKernel(fn, gx, gy, 1U, bx, 1U, 1U, 0U, impl.stream, params, nullptr);
        }
        cuStreamSynchronize(impl.stream);
        CUevent ev0 = nullptr;
        CUevent ev1 = nullptr;
        if (cuEventCreate(&ev0, CU_EVENT_DEFAULT) == CUDA_SUCCESS && cuEventCreate(&ev1, CU_EVENT_DEFAULT) == CUDA_SUCCESS)
        {
            double best = 1.0e30;
            bool   any  = false;
            for (int it = 0; it < iters; ++it)
            {
                cuEventRecord(ev0, impl.stream);
                const CUresult lr = cuLaunchKernel(fn, gx, gy, 1U, bx, 1U, 1U, 0U, impl.stream, params, nullptr);
                cuEventRecord(ev1, impl.stream);
                if (cuEventSynchronize(ev1) != CUDA_SUCCESS || lr != CUDA_SUCCESS) { break; }
                float ms = 0.0F;
                if (cuEventElapsedTime(&ms, ev0, ev1) == CUDA_SUCCESS) { any = true; if (ms < best) { best = static_cast<double>(ms); } }
            }
            if (any)
            {
                cuMemcpyDtoH(out, d_out, out_bytes); // readback for the oracle
                t.ok     = true;
                t.min_ms = best;
            }
            cuEventDestroy(ev0);
            cuEventDestroy(ev1);
        }
    }
    if (d_in0 != 0) { cuMemFree(d_in0); }
    if (d_in1 != 0) { cuMemFree(d_in1); }
    if (d_out != 0) { cuMemFree(d_out); }
    cuModuleUnload(mod);
    return t;
}

// AS-4 (the FUSED crush): time the fused GEMM+epilogue kernel — one launch does GEMM + per-column bias + activation, the pass
// cuBLAS must do separately. Mirrors time_contract_schedule (local module, event-timed) but emits the fused kernel + binds the
// bias inputs. Self-contained; freed here.
ContractTiming KirBackendCuda::time_fused_contract(const KGraph& g, int output, const float* const* inputs, int n_inputs,
                                                   float* out, int warmup, int iters)
{
    ContractTiming t;
    auto&          impl = *m_impl;
    if (!impl.ok || n_inputs < 2 || n_inputs > kMaxIn || iters <= 0) { return t; }
    const FuseInfo fuse = detect_fuse(g, output, impl.alloc);
    if (!fuse.ok) { return t; }
    const TileSchedule sch = select_schedule(g, fuse.contract, impl.arch);
    if (sch.kind != Sched::WarpTiled) { return t; }
    GlslKernel kern(impl.alloc);
    if (!emit_contract_tiled_fused_cuda(g, output, fuse.contract, sch, fuse, impl.alloc, kern) || kern.n_inputs != n_inputs) { return t; }

    const KNode&   cn = g.node(fuse.contract);
    const KNode&   an = g.node(cn.a);
    const KNode&   bn = g.node(cn.b);
    const int      r  = an.shape.rank;
    const crd::u32 mm = static_cast<crd::u32>(an.shape.dims[r - 2]);
    const crd::u32 kk = static_cast<crd::u32>(an.shape.dims[r - 1]);
    const crd::u32 nn = static_cast<crd::u32>(bn.shape.dims[bn.shape.rank - 1]);

    crd::containers::Array<char> cubin(impl.alloc);
    if (!compile_cubin(kern.source.c_str(), impl.arch, cubin, sch.fma)) { return t; }
    CUmodule mod = nullptr;
    if (cuModuleLoadData(&mod, cubin.data()) != CUDA_SUCCESS) { return t; }
    CUfunction fn = nullptr;
    if (cuModuleGetFunction(&fn, mod, "ckir") != CUDA_SUCCESS) { cuModuleUnload(mod); return t; }

    crd::u64    in_bytes[kMaxIn] = {};
    in_bytes[0]                  = static_cast<crd::u64>(mm) * kk * sizeof(float);
    in_bytes[1]                  = static_cast<crd::u64>(kk) * nn * sizeof(float);
    for (int j = 2; j < n_inputs; ++j) { in_bytes[j] = static_cast<crd::u64>(nn) * sizeof(float); } // per-column bias [N]
    const crd::u64 out_bytes = static_cast<crd::u64>(mm) * nn * sizeof(float);
    CUdeviceptr    d_in[kMaxIn] = {};
    CUdeviceptr    d_out        = 0;
    bool           mok          = cuMemAlloc(&d_out, out_bytes) == CUDA_SUCCESS;
    for (int i = 0; i < n_inputs && mok; ++i)
    {
        mok = cuMemAlloc(&d_in[i], in_bytes[i]) == CUDA_SUCCESS
           && cuMemcpyHtoD(d_in[i], inputs[kern.input_iidx[i]], in_bytes[i]) == CUDA_SUCCESS;
    }
    if (mok)
    {
        void*    params[kMaxIn + 8];
        int      np = 0;
        crd::u32 pm = mm;
        crd::u32 pn = nn;
        crd::u32 pk = kk;
        params[np++] = &d_in[0]; // A
        params[np++] = &d_in[1]; // Bm
        params[np++] = &d_out;   // C
        for (int j = 2; j < n_inputs; ++j) { params[np++] = &d_in[j]; } // bias0..
        params[np++] = &pm;
        params[np++] = &pn;
        params[np++] = &pk; // ckir(A,Bm,C,bias..,M,N,K)
        const crd::u32 gx = nn / static_cast<crd::u32>(sch.bn);
        const crd::u32 gy = mm / static_cast<crd::u32>(sch.bm);
        const crd::u32 bx = static_cast<crd::u32>(sch.nt);
        for (int w = 0; w < warmup; ++w) { cuLaunchKernel(fn, gx, gy, 1U, bx, 1U, 1U, 0U, impl.stream, params, nullptr); }
        cuStreamSynchronize(impl.stream);
        CUevent ev0 = nullptr;
        CUevent ev1 = nullptr;
        if (cuEventCreate(&ev0, CU_EVENT_DEFAULT) == CUDA_SUCCESS && cuEventCreate(&ev1, CU_EVENT_DEFAULT) == CUDA_SUCCESS)
        {
            double best = 1.0e30;
            bool   any  = false;
            for (int it = 0; it < iters; ++it)
            {
                cuEventRecord(ev0, impl.stream);
                const CUresult lr = cuLaunchKernel(fn, gx, gy, 1U, bx, 1U, 1U, 0U, impl.stream, params, nullptr);
                cuEventRecord(ev1, impl.stream);
                if (cuEventSynchronize(ev1) != CUDA_SUCCESS || lr != CUDA_SUCCESS) { break; }
                float ms = 0.0F;
                if (cuEventElapsedTime(&ms, ev0, ev1) == CUDA_SUCCESS) { any = true; if (ms < best) { best = static_cast<double>(ms); } }
            }
            if (any)
            {
                cuMemcpyDtoH(out, d_out, out_bytes);
                t.ok     = true;
                t.min_ms = best;
            }
            cuEventDestroy(ev0);
            cuEventDestroy(ev1);
        }
    }
    for (int i = 0; i < n_inputs; ++i) { if (d_in[i] != 0) { cuMemFree(d_in[i]); } }
    if (d_out != 0) { cuMemFree(d_out); }
    cuModuleUnload(mod);
    return t;
}

// AS-4: measure ONE flash-attention (br,bc) schedule for a KOp::Attention node — self-contained (local module + buffers, freed
// here) so the autotuner sweeps candidates without touching run()'s persistent cache. GPU-event-timed min-of-iters (upload/readback
// outside the timed region); `out` gets the last launch for the oracle. Mirrors time_contract_schedule.
ContractTiming KirBackendCuda::time_attention(const KGraph& g, int output, int br, int bc, const float* const* inputs,
                                              int n_inputs, float* out, int warmup, int iters)
{
    ContractTiming t;
    auto&          impl = *m_impl;
    if (!impl.ok || n_inputs != 3 || iters <= 0) { return t; }
    const KNode& an = g.node(output);
    if (an.op != KOp::Attention) { return t; }
    const KNode& qn = g.node(an.a);
    if (qn.shape.rank != 2) { return t; }
    const crd::u32 slen  = static_cast<crd::u32>(qn.shape.dims[0]);
    const crd::u32 dim   = static_cast<crd::u32>(qn.shape.dims[1]);
    const float    scale = static_cast<float>(an.cval);

    GlslKernel kern(impl.alloc);
    if (!emit_attention_flash_cuda(g, output, br, bc, kern) || kern.n_inputs != n_inputs) { return t; }

    crd::containers::Array<char> cubin(impl.alloc);
    if (!compile_cubin(kern.source.c_str(), impl.arch, cubin, true)) { return t; } // fast tier: FMA on
    CUmodule mod = nullptr;
    if (cuModuleLoadData(&mod, cubin.data()) != CUDA_SUCCESS) { return t; }
    CUfunction fn = nullptr;
    if (cuModuleGetFunction(&fn, mod, "ckir") != CUDA_SUCCESS) { cuModuleUnload(mod); return t; }

    const crd::u64 bytes = static_cast<crd::u64>(slen) * dim * sizeof(float);
    CUdeviceptr    d_q   = 0;
    CUdeviceptr    d_k   = 0;
    CUdeviceptr    d_v   = 0;
    CUdeviceptr    d_o   = 0;
    bool           mok   = cuMemAlloc(&d_q, bytes) == CUDA_SUCCESS && cuMemAlloc(&d_k, bytes) == CUDA_SUCCESS
                       && cuMemAlloc(&d_v, bytes) == CUDA_SUCCESS && cuMemAlloc(&d_o, bytes) == CUDA_SUCCESS;
    if (mok)
    {
        mok = cuMemcpyHtoD(d_q, inputs[kern.input_iidx[0]], bytes) == CUDA_SUCCESS
           && cuMemcpyHtoD(d_k, inputs[kern.input_iidx[1]], bytes) == CUDA_SUCCESS
           && cuMemcpyHtoD(d_v, inputs[kern.input_iidx[2]], bytes) == CUDA_SUCCESS;
    }
    if (mok)
    {
        void*    params[6];
        int      np  = 0;
        crd::u32 ps  = slen;
        float    psc = scale;
        params[np++] = &d_q;
        params[np++] = &d_k;
        params[np++] = &d_v;
        params[np++] = &d_o;
        params[np++] = &ps;
        params[np++] = &psc; // ckir(Q,K,V,O,S,scale)
        const crd::u32 gx = (slen + static_cast<crd::u32>(br) - 1U) / static_cast<crd::u32>(br);
        const crd::u32 bx = static_cast<crd::u32>(br);
        for (int w = 0; w < warmup; ++w) { cuLaunchKernel(fn, gx, 1U, 1U, bx, 1U, 1U, 0U, impl.stream, params, nullptr); }
        cuStreamSynchronize(impl.stream);
        CUevent ev0 = nullptr;
        CUevent ev1 = nullptr;
        if (cuEventCreate(&ev0, CU_EVENT_DEFAULT) == CUDA_SUCCESS && cuEventCreate(&ev1, CU_EVENT_DEFAULT) == CUDA_SUCCESS)
        {
            double best = 1.0e30;
            bool   any  = false;
            for (int it = 0; it < iters; ++it)
            {
                cuEventRecord(ev0, impl.stream);
                const CUresult lr = cuLaunchKernel(fn, gx, 1U, 1U, bx, 1U, 1U, 0U, impl.stream, params, nullptr);
                cuEventRecord(ev1, impl.stream);
                if (cuEventSynchronize(ev1) != CUDA_SUCCESS || lr != CUDA_SUCCESS) { break; }
                float ms = 0.0F;
                if (cuEventElapsedTime(&ms, ev0, ev1) == CUDA_SUCCESS) { any = true; if (ms < best) { best = static_cast<double>(ms); } }
            }
            if (any)
            {
                cuMemcpyDtoH(out, d_o, bytes); // readback for the oracle
                t.ok     = true;
                t.min_ms = best;
            }
            cuEventDestroy(ev0);
            cuEventDestroy(ev1);
        }
    }
    if (d_q != 0) { cuMemFree(d_q); }
    if (d_k != 0) { cuMemFree(d_k); }
    if (d_v != 0) { cuMemFree(d_v); }
    if (d_o != 0) { cuMemFree(d_o); }
    cuModuleUnload(mod);
    return t;
}

} // namespace crd::kir
