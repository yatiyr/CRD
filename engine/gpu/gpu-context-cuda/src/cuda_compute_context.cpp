// crd-gpu-context-cuda — CudaComputeContext : crd::gpu::IComputeContext (ADR-0100, user-directed 2026-08-07). CUDA
// driver API + NVRTC (CUDA C -> CUBIN). Mirrors engine/gpu/kir-cuda's proven patterns (shared primary context, CUBIN not
// PTX, event timing). See cuda_compute_context.hpp for the sharing + barrier-no-op rationale.

#include <crd/gpu/cuda_compute_context.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/gpu/device_check.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_cuda.hpp>
#include <crd/kir/ckir_device_check.hpp>
#include <crd/memory/allocator.hpp>

#include <cuda.h>
#include <nvrtc.h>

#include <cstdio>
#include <cstring>

namespace crd::gpu
{
namespace
{
// The NVRTC target of `device`, "sm_<major><minor>" (no context needed).
void cuda_arch(CUdevice device, char (&arch)[16]) noexcept
{
    int major = 0;
    int minor = 0;
    if (cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device) != CUDA_SUCCESS ||
        cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device) != CUDA_SUCCESS)
    {
        major = 5;
        minor = 2;
    }
    (void)std::snprintf(arch, sizeof(arch), "sm_%d%d", major, minor);
}

// The identity of `device` (no context needed): its name and the driver's CUDA version ("13.0"); CUDA reports no PCI
// device id.
[[nodiscard]] GpuAdapter cuda_adapter(CUdevice device) noexcept
{
    GpuAdapter adapter{};
    std::memcpy(adapter.backend, "cuda", sizeof("cuda"));
    if (cuDeviceGetName(adapter.name, static_cast<int>(sizeof(adapter.name)), device) != CUDA_SUCCESS)
    {
        adapter.name[0] = '\0';
    }
    adapter.name[sizeof(adapter.name) - 1U] = '\0';
    adapter.vendor = 0x10DEU;
    int version = 0;
    if (cuDriverGetVersion(&version) == CUDA_SUCCESS && version > 0)
    {
        adapter.driver = static_cast<crd::u64>(version);
        (void)std::snprintf(adapter.driver_version, sizeof(adapter.driver_version), "%d.%d", version / 1000,
                            (version % 1000) / 10);
    }
    adapter.known = true;
    return adapter;
}

// A context of its own on `device` (not the shared primary one), pushed current on this thread; cuCtxDestroy pops it.
[[nodiscard]] bool create_own_context(CUdevice device, CUcontext& ctx) noexcept
{
#if CUDA_VERSION >= 13000
    return cuCtxCreate(&ctx, nullptr, 0U, device) == CUDA_SUCCESS; // 13.x: cuCtxCreate_v4, null parameters
#else
    return cuCtxCreate(&ctx, 0U, device) == CUDA_SUCCESS;
#endif
}
// NVRTC: compile CUDA C (`src`, null-terminated) → a CUBIN for the device's exact `arch` (e.g. "sm_89"). CUBIN, not PTX:
// newer NVRTC emits a PTX version the driver JIT rejects (cuModuleLoadData error 222). ⭐ `fmad` is the PER-PIPELINE
// FP-contraction choice: `--fmad=false` yields bit-exact (oracle-matched, the CUDA-fan-out convention), `--fmad=true` fuses
// (the Fast tier). The caller picks (create_pipeline_from_cuda's `fmad` param) — no global policy.
[[nodiscard]] bool compile_cubin(const char* src, const char* arch, bool fmad, crd::containers::Array<char>& cubin)
{
    nvrtcProgram prog{};
    if (nvrtcCreateProgram(&prog, src, "crd_cuda.cu", 0, nullptr, nullptr) != NVRTC_SUCCESS)
    {
        return false;
    }
    char archopt[64];
    std::snprintf(archopt, sizeof(archopt), "--gpu-architecture=%s", arch);
    const char*       opts[] = {fmad ? "--fmad=true" : "--fmad=false", archopt};
    const nvrtcResult r      = nvrtcCompileProgram(prog, 2, opts);
    if (r != NVRTC_SUCCESS)
    {
        crd::usize logsz = 0;
        nvrtcGetProgramLogSize(prog, &logsz);
        if (logsz > 1)
        {
            crd::containers::Array<char> log(cubin.allocator());
            log.resize(logsz, '\0');
            nvrtcGetProgramLog(prog, log.data());
            std::fprintf(stderr, "[cuda-compute] NVRTC compile failed:\n%s\n", log.data());
        }
        nvrtcDestroyProgram(&prog);
        return false;
    }
    crd::usize sz = 0;
    if (nvrtcGetCUBINSize(prog, &sz) != NVRTC_SUCCESS || sz == 0)
    {
        nvrtcDestroyProgram(&prog);
        return false;
    }
    cubin.resize(sz, '\0');
    const nvrtcResult gr = nvrtcGetCUBIN(prog, cubin.data());
    nvrtcDestroyProgram(&prog);
    return gr == NVRTC_SUCCESS;
}

// Opaque CUDA buffer. GpuOnly ⇒ cuMemAlloc (host ptr null, map() returns null). CpuToGpu/GpuToCpu ⇒ pinned host memory
// (cuMemHostAlloc DEVICEMAP|PORTABLE); with UVA the HOST pointer IS the device pointer the kernel takes (not
// cuMemHostGetDevicePointer — that mismatched on this driver), so map() and the kernel argument are the same address.
class CudaBuffer final : public ComputeBuffer
{
public:
    CudaBuffer(CUdeviceptr dptr, void* host, bool pinned) noexcept : m_dptr(dptr), m_host(host), m_pinned(pinned) {}
    ~CudaBuffer() override
    {
        if (m_pinned)
        {
            if (m_host != nullptr)
            {
                cuMemFreeHost(m_host);
            }
        }
        else if (m_dptr != 0U)
        {
            cuMemFree(m_dptr);
        }
    }
    CudaBuffer(const CudaBuffer&)            = delete;
    CudaBuffer& operator=(const CudaBuffer&) = delete;
    CudaBuffer(CudaBuffer&&)                 = delete;
    CudaBuffer& operator=(CudaBuffer&&)      = delete;

    [[nodiscard]] void* map() noexcept override { return m_host; }
    void                unmap() noexcept override {}
    // A CUdeviceptr IS an integer device address; exposing it as the opaque void* native handle is the CUDA idiom
    // (mirrors Vulkan/DX12 returning their native pointer handle).
    [[nodiscard]] void* native_handle() const noexcept override
    {
        return reinterpret_cast<void*>(m_dptr); // NOLINT(performance-no-int-to-ptr) — opaque device-address handle
    }
    [[nodiscard]] CUdeviceptr dptr() const noexcept { return m_dptr; }

private:
    CUdeviceptr m_dptr = 0U;
    void*       m_host = nullptr;
    bool        m_pinned = false;
};

// Opaque CUDA pipeline: a loaded module + its kernel function + the binding/push contract.
class CudaPipeline final : public ComputePipeline
{
public:
    CudaPipeline(CUmodule mod, CUfunction fn, int n_bindings, crd::u32 block, crd::u32 /*push_size*/) noexcept
        : m_mod(mod), m_fn(fn), m_n(n_bindings), m_block(block) {}
    ~CudaPipeline() override
    {
        if (m_mod != nullptr)
        {
            cuModuleUnload(m_mod);
        }
    }
    CudaPipeline(const CudaPipeline&)            = delete;
    CudaPipeline& operator=(const CudaPipeline&) = delete;
    CudaPipeline(CudaPipeline&&)                 = delete;
    CudaPipeline& operator=(CudaPipeline&&)      = delete;

    [[nodiscard]] CUfunction fn() const noexcept { return m_fn; }
    [[nodiscard]] int        n_bindings() const noexcept { return m_n; }
    [[nodiscard]] crd::u32   block() const noexcept { return m_block; } // the 1-D blockDim.x this kernel launches with

private:
    CUmodule    m_mod  = nullptr;
    CUfunction  m_fn    = nullptr;
    int         m_n     = 0;
    crd::u32    m_block = 0U; // blockDim.x — the kernel's local_size (CEIR-13z CUDA fix: was a fixed 256, broke shared-mem kernels)
};

// ⭐ CUDA local-size convention: the IComputeContext dispatch surface passes only the GRID dims (gx,gy,gz) — the Vulkan model
// bakes the block size into the SPIR-V; CUDA specifies it at launch, so it rides the PIPELINE (`create_pipeline_from_cuda`'s
// `local_size` → `CudaPipeline::block()`, used as blockDim.x). ⛔ CEIR-13z fix (2026-08-10): this backend USED to launch a
// FIXED 256-thread block — correct for ELEMENTWISE kernels (guarded `blockIdx.x*blockDim.x+threadIdx.x`) but GARBAGE for
// SHARED-MEMORY kernels (the CKIR is_kernel path: FFT/transpose/reduce/scan) which need blockDim.x == local_size (shared
// arrays sized to it, __syncthreads() over exactly those threads). The FFT was garbage + the transpose over-launch
// segfaulted; single-workgroup reduce/scan only survived by identity-padding guarded threads (add by luck — UB that passed).
// Local params cap — a compute backend has no dependency on the raster command_model's kMaxBindings.
inline constexpr crd::u32 kCudaMaxBindings = 16U;

// CEIR-29b-2: a captured+instantiated CUDA graph. RAII — destroys the exec THEN the source graph. An invalid instance
// (m_exec==nullptr) is the capture-failed sentinel: valid() is false, launch() ignores it, the gate REQUIREs valid().
class CudaGraphImpl final : public CudaGraph
{
public:
    CudaGraphImpl(CUgraphExec exec, CUgraph graph, crd::u32 nodes) noexcept : m_exec(exec), m_graph(graph), m_nodes(nodes) {}
    ~CudaGraphImpl() override
    {
        if (m_exec != nullptr)
        {
            cuGraphExecDestroy(m_exec);
        }
        if (m_graph != nullptr) // kept past instantiate for a future cuGraphExecUpdate (29z)
        {
            cuGraphDestroy(m_graph);
        }
    }
    CudaGraphImpl(const CudaGraphImpl&)            = delete;
    CudaGraphImpl& operator=(const CudaGraphImpl&) = delete;
    CudaGraphImpl(CudaGraphImpl&&)                 = delete;
    CudaGraphImpl& operator=(CudaGraphImpl&&)      = delete;

    [[nodiscard]] bool        valid() const noexcept override { return m_exec != nullptr; }
    [[nodiscard]] crd::u32    node_count() const noexcept override { return m_nodes; }
    [[nodiscard]] CUgraphExec exec() const noexcept { return m_exec; }

private:
    CUgraphExec m_exec  = nullptr;
    CUgraph     m_graph = nullptr;
    crd::u32    m_nodes = 0U;
};

[[nodiscard]] bool cuda_checked_device(CUdevice& out);

class CudaContextImpl final : public CudaComputeContext
{
    // The recorder issues copies/dispatches straight onto the context's stream (async); submit_and_wait synchronises.
    class Recorder final : public ComputeRecorder
    {
    public:
        explicit Recorder(CudaContextImpl& c) noexcept : m_c(c) {}

        void copy(ComputeBuffer& src, ComputeBuffer& dst, crd::u64 src_off, crd::u64 dst_off, crd::u64 bytes) override
        {
            // Unified cuMemcpyAsync — infers direction via UVA, so it works for device↔device, pinned↔device and
            // pinned↔pinned alike (the strict typed variants reject a pinned pointer as a device pointer).
            const CUdeviceptr s = static_cast<CudaBuffer&>(src).dptr() + src_off;
            const CUdeviceptr d = static_cast<CudaBuffer&>(dst).dptr() + dst_off;
            cuMemcpyAsync(d, s, bytes, m_c.m_stream);
        }

        // Single CUDA stream ⇒ implicit in-order execution, so a pass-to-pass buffer barrier is a NO-OP here (a real,
        // documented difference from Vulkan/DX12's explicit barriers).
        void barrier(ComputeBuffer& /*buf*/, ComputeAccess /*from*/, ComputeAccess /*to*/) override {}

        void dispatch(ComputePipeline& pipeline, crd::containers::ConstSpan<ComputeBuffer*> bindings, const void* push,
                      crd::u32 push_size, crd::u32 gx, crd::u32 gy, crd::u32 gz) override
        {
            auto&     cp = static_cast<CudaPipeline&>(pipeline);
            const int n  = static_cast<int>(bindings.size());
            if (n > static_cast<int>(kCudaMaxBindings))
            {
                return;
            }
            CUdeviceptr dptrs[kCudaMaxBindings];
            void*       params[kCudaMaxBindings + 1];
            for (int i = 0; i < n; ++i)
            {
                dptrs[i]  = static_cast<CudaBuffer*>(bindings[i])->dptr();
                params[i] = &dptrs[i];
            }
            unsigned char pushbuf[256];
            int           nparams = n;
            if (push != nullptr && push_size > 0U && push_size <= sizeof(pushbuf))
            {
                std::memcpy(pushbuf, push, push_size); // copy out of the const source ⇒ no const_cast of the kernel arg
                params[nparams++] = pushbuf;
            }
            const crd::u32 block = cp.block() > 0U ? cp.block() : 1U; // the kernel's local_size (was a fixed kCudaBlock)
            cuLaunchKernel(cp.fn(), gx > 0U ? gx : 1U, gy > 0U ? gy : 1U, gz > 0U ? gz : 1U, block, 1U, 1U, 0U,
                           m_c.m_stream, params, nullptr);
        }

    private:
        CudaContextImpl& m_c;
    };

public:
    // The device the check chooses (cuda_checked_device), on its PRIMARY context: the refcounted singleton kir-cuda
    // also retains, so a shared device and no duplicate init.
    explicit CudaContextImpl(crd::memory::IAllocator& alloc) noexcept : m_alloc(alloc), m_rec(*this)
    {
        if (cuInit(0) != CUDA_SUCCESS || !cuda_checked_device(m_device))
        {
            return;
        }
        if (cuDevicePrimaryCtxRetain(&m_ctx, m_device) != CUDA_SUCCESS)
        {
            return;
        }
        m_retained = true;
        if (cuCtxSetCurrent(m_ctx) != CUDA_SUCCESS)
        {
            return;
        }
        open();
    }

    // HW-RESILIENCE.3: the device check's probe, on a context of its own on `device` that stays current on this thread
    // until the probe is destroyed, so the primary context a caller receives carries none of the check's work.
    CudaContextImpl(crd::memory::IAllocator& alloc, CUdevice device) noexcept : m_alloc(alloc), m_rec(*this)
    {
        m_device = device;
        if (create_own_context(m_device, m_ctx))
        {
            open();
        }
    }

    ~CudaContextImpl() override
    {
        if (m_ev0 != nullptr)
        {
            cuEventDestroy(m_ev0);
        }
        if (m_ev1 != nullptr)
        {
            cuEventDestroy(m_ev1);
        }
        if (m_stream != nullptr)
        {
            cuStreamDestroy(m_stream);
        }
        if (m_retained) // release our refcount on the shared primary ctx
        {
            cuDevicePrimaryCtxRelease(m_device);
        }
        else if (m_ctx != nullptr) // the probe's own context, popped from this thread
        {
            cuCtxDestroy(m_ctx);
        }
    }
    CudaContextImpl(const CudaContextImpl&)            = delete;
    CudaContextImpl& operator=(const CudaContextImpl&) = delete;
    CudaContextImpl(CudaContextImpl&&)                 = delete;
    CudaContextImpl& operator=(CudaContextImpl&&)      = delete;

    [[nodiscard]] bool valid() const noexcept override { return m_ok; }
    [[nodiscard]] bool supports_shader_int64() const noexcept override { return true; } // CUDA is natively 64-bit

    [[nodiscard]] std::unique_ptr<ComputeBuffer> create_buffer(crd::u64 bytes, crd::u32 /*usage*/,
                                                               ComputeMemory memory) override
    {
        if (!m_ok || bytes == 0U)
        {
            return nullptr;
        }
        if (memory == ComputeMemory::GpuOnly)
        {
            CUdeviceptr d = 0U;
            if (cuMemAlloc(&d, bytes) != CUDA_SUCCESS)
            {
                return nullptr;
            }
            return std::make_unique<CudaBuffer>(d, nullptr, /*pinned*/ false);
        }
        void* host = nullptr;
        if (cuMemHostAlloc(&host, bytes, CU_MEMHOSTALLOC_PORTABLE | CU_MEMHOSTALLOC_DEVICEMAP) != CUDA_SUCCESS)
        {
            return nullptr;
        }
        // UVA: the host pointer IS the device pointer.
        return std::make_unique<CudaBuffer>(reinterpret_cast<CUdeviceptr>(host), host, /*pinned*/ true);
    }

    [[nodiscard]] std::unique_ptr<ComputePipeline> create_pipeline(crd::containers::StringView /*shader_dir*/,
                                                                   crd::containers::StringView /*name*/,
                                                                   int /*n_bindings*/, crd::u32 /*push_size*/) override
    {
        // The by-name cooked-kernel path (`<name>.cubin`) is not wired until a cooked CUDA corpus exists — mirrors the
        // DX12 by-name stub. Runtime callers use create_pipeline_from_cuda (the source escape hatch).
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<ComputePipeline> create_pipeline_from_cuda(crd::containers::StringView cuda_source,
                                                                             crd::containers::StringView entry,
                                                                             int n_bindings, crd::u32 local_size,
                                                                             crd::u32 push_size, bool fmad) override
    {
        if (!m_ok)
        {
            return nullptr;
        }
        if (local_size == 0U || local_size > 1024U) // ⛔ blockDim.x must be a valid CUDA block (CUDA caps at 1024 threads/block)
        {
            std::fprintf(stderr, "create_pipeline_from_cuda: invalid local_size %u (must be 1..1024)\n", local_size);
            return nullptr;
        }
        crd::containers::Array<char> src(&m_alloc);
        src.resize(cuda_source.size() + 1U, '\0');
        for (crd::usize i = 0; i < cuda_source.size(); ++i)
        {
            src[i] = cuda_source[i];
        }
        crd::containers::Array<char> name(&m_alloc);
        name.resize(entry.size() + 1U, '\0');
        for (crd::usize i = 0; i < entry.size(); ++i)
        {
            name[i] = entry[i];
        }

        crd::containers::Array<char> cubin(&m_alloc);
        if (!compile_cubin(src.data(), m_arch, fmad, cubin))
        {
            return nullptr;
        }
        return load_cubin(cubin, name.data(), n_bindings, local_size, push_size);
    }

    // Load a CUBIN built for this device and cache a pipeline of its kernel `entry` (null-terminated); nullptr when the
    // driver refuses the module or the module has no such kernel.
    [[nodiscard]] std::unique_ptr<ComputePipeline> load_cubin(const crd::containers::Array<char>& cubin,
                                                              const char* entry, int n_bindings, crd::u32 local_size,
                                                              crd::u32 push_size)
    {
        CUmodule mod = nullptr;
        if (!m_ok || cuModuleLoadData(&mod, cubin.data()) != CUDA_SUCCESS)
        {
            return nullptr;
        }
        CUfunction fn = nullptr;
        if (cuModuleGetFunction(&fn, mod, entry) != CUDA_SUCCESS)
        {
            cuModuleUnload(mod);
            return nullptr;
        }
        return std::make_unique<CudaPipeline>(mod, fn, n_bindings, local_size, push_size);
    }

    [[nodiscard]] ComputeRecorder& begin() override
    {
        if (m_ok) // bracket the recorded work for last_gpu_ms
        {
            cuEventRecord(m_ev0, m_stream);
        }
        return m_rec;
    }

    void submit_and_wait() override
    {
        if (!m_ok)
        {
            return;
        }
        cuEventRecord(m_ev1, m_stream);
        cuStreamSynchronize(m_stream);
        float ms = 0.0F;
        if (cuEventElapsedTime(&ms, m_ev0, m_ev1) == CUDA_SUCCESS)
        {
            m_last_ms = static_cast<double>(ms);
        }
    }

    [[nodiscard]] crd::u32 subgroup_size() const noexcept override { return m_warp; }
    [[nodiscard]] crd::u32 shared_memory_bytes() const noexcept override { return m_shmem; }
    [[nodiscard]] double   last_gpu_ms() const noexcept override { return m_last_ms; }
    [[nodiscard]] GpuAdapter adapter() const noexcept override { return m_adapter; } // HW-RESILIENCE.2

    // CEIR-29b-2: start stream capture on the SAME stream (THREAD_LOCAL mode — scopes the capture's "no unsafe call" rule to
    // this thread, so another thread touching the driver can't invalidate it). Subsequent dispatches RECORD into the graph.
    [[nodiscard]] ComputeRecorder& begin_capture() override
    {
        // ⛔ track begin's SUCCESS: if it fails (a prior capture was invalidated + never ended, or the stream is already
        // capturing) the dispatches would run EAGERLY on the stream — end_capture() must then refuse to call
        // cuStreamEndCapture on a non-capturing stream (and never hand back a "graph" for work that already executed).
        m_capturing = m_ok && cuStreamBeginCapture(m_stream, CU_STREAM_CAPTURE_MODE_THREAD_LOCAL) == CUDA_SUCCESS;
        return m_rec;
    }

    // End capture + INSTANTIATE ONCE. Returns an INVALID handle (m_exec==nullptr) — never a partial graph — if begin_capture
    // failed, or cuStreamEndCapture returns _INVALIDATED/_UNJOINED (an unsafe call ran mid-capture) or a null graph.
    [[nodiscard]] std::unique_ptr<CudaGraph> end_capture() override
    {
        const bool was_capturing = m_capturing;
        m_capturing              = false; // clear regardless — a failed begin leaves the flag false, a good one is consumed here
        CUgraph graph = nullptr;
        if (!was_capturing || cuStreamEndCapture(m_stream, &graph) != CUDA_SUCCESS || graph == nullptr)
        {
            if (graph != nullptr)
            {
                cuGraphDestroy(graph);
            }
            return std::make_unique<CudaGraphImpl>(nullptr, nullptr, 0U);
        }
        size_t nodes = 0;
        cuGraphGetNodes(graph, nullptr, &nodes); // null array + &count = query the node count
        CUgraphExec exec = nullptr;
        if (cuGraphInstantiateWithFlags(&exec, graph, 0U) != CUDA_SUCCESS) // 11.4+ / 12.x / 13.x — version-stable form
        {
            cuGraphDestroy(graph);
            return std::make_unique<CudaGraphImpl>(nullptr, nullptr, 0U);
        }
        return std::make_unique<CudaGraphImpl>(exec, graph, static_cast<crd::u32>(nodes));
    }

    // CEIR-29b-2: launch a captured graph and WAIT, bracketed by the same events as submit_and_wait ⇒ last_gpu_ms() reports
    // graph-launch time. Reusable (replay) — the exec is not re-instantiated.
    void launch(const CudaGraph& graph) override
    {
        if (!m_ok || !graph.valid())
        {
            return;
        }
        cuEventRecord(m_ev0, m_stream);
        cuGraphLaunch(static_cast<const CudaGraphImpl&>(graph).exec(), m_stream);
        cuEventRecord(m_ev1, m_stream);
        cuStreamSynchronize(m_stream);
        float ms = 0.0F;
        if (cuEventElapsedTime(&ms, m_ev0, m_ev1) == CUDA_SUCCESS)
        {
            m_last_ms = static_cast<double>(ms);
        }
    }

    // CEIR-29z: enqueue the graph on the stream WITHOUT a bracket or wait — for a two-class begin()/submit_and_wait() bracket
    // that records eager prefix + this graph + eager suffix as ONE submission (same-stream order carries the dependency).
    void enqueue(const CudaGraph& graph) override
    {
        if (!m_ok || !graph.valid())
        {
            return;
        }
        cuGraphLaunch(static_cast<const CudaGraphImpl&>(graph).exec(), m_stream);
    }

private:
    // With m_ctx current: the stream, the timing events, the device's limits and identity.
    void open() noexcept
    {
        if (cuStreamCreate(&m_stream, CU_STREAM_DEFAULT) != CUDA_SUCCESS)
        {
            return;
        }
        cuEventCreate(&m_ev0, CU_EVENT_DEFAULT);
        cuEventCreate(&m_ev1, CU_EVENT_DEFAULT);
        cuda_arch(m_device, m_arch);
        int warp = 0;
        int shmem = 0;
        cuDeviceGetAttribute(&warp, CU_DEVICE_ATTRIBUTE_WARP_SIZE, m_device);
        cuDeviceGetAttribute(&shmem, CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK, m_device);
        m_warp = warp > 0 ? static_cast<crd::u32>(warp) : 32U;
        m_shmem = shmem > 0 ? static_cast<crd::u32>(shmem) : 49152U;
        m_ok = true;
        m_adapter = cuda_adapter(m_device);
        (void)opened_adapters().note(m_adapter); // HW-RESILIENCE.2: the machine's identity names this GPU
    }

    crd::memory::IAllocator& m_alloc;
    Recorder                 m_rec;
    CUdevice                 m_device = 0;
    CUcontext                m_ctx    = nullptr;
    CUstream                 m_stream = nullptr;
    CUevent                  m_ev0    = nullptr;
    CUevent                  m_ev1    = nullptr;
    char                     m_arch[16] = {'s', 'm', '_', '5', '2', '\0'};
    GpuAdapter               m_adapter{};
    crd::u32                 m_warp   = 32U;
    crd::u32                 m_shmem  = 49152U;
    double                   m_last_ms = 0.0;
    bool                     m_ok     = false;
    bool m_retained = false;  // m_ctx is the device's primary context, retained
    bool                     m_capturing = false; // CEIR-29b-2a: true between a SUCCESSFUL begin_capture and its end_capture
};

// HW-RESILIENCE.3: the device check of the device `*user` (a CUdevice). The kernel is lowered on the host first (one
// that does not lower leaves the device unchecked) and compiled by NVRTC for the device's own architecture. NVRTC is
// linked into this backend, so a compile that fails means the backend can build no kernel for that device: it fails as
// "build" and is passed over, unlike a host without a shader compiler on Vulkan or DX12. The CUBIN then runs on a
// probe context of its own, destroyed before the caller's primary context is retained.
DeviceCheckOutcome check_cuda_device(void* user)
{
    const CUdevice device = *static_cast<const CUdevice*>(user);
    crd::memory::IAllocator* const alloc = crd::memory::default_allocator();
    crd::kir::KGraph graph(alloc);
    const crd::kir::KEntry entry = crd::kir::build_device_check_kernel(graph);
    crd::kir::GlslKernel kernel(alloc);
    if (!crd::kir::emit_compute_kernel_cuda(graph, entry, alloc, kernel))
    {
        return DeviceCheckOutcome{nullptr, "the kernel does not lower to CUDA C"};
    }
    char arch[16] = {};
    cuda_arch(device, arch);
    crd::containers::Array<char> cubin(alloc);
    if (!compile_cubin(kernel.source.c_str(), arch, false, cubin))
    {
        return DeviceCheckOutcome{"build", nullptr};
    }
    CudaContextImpl probe(*alloc, device);
    if (!probe.valid())
    {
        return DeviceCheckOutcome{"device", nullptr};
    }
    float input[crd::kir::kDeviceCheckLanes] = {};
    for (crd::u32 i = 0U; i < crd::kir::kDeviceCheckLanes; ++i)
    {
        input[i] = crd::kir::device_check_input(i);
    }
    const std::unique_ptr<ComputePipeline> pipeline =
        probe.load_cubin(cubin, "ckir", crd::kir::kDeviceCheckBindings, entry.local_size[0], 0U);
    DeviceCheckProgram program;
    program.input = crd::containers::ConstSpan<float>(input, crd::kir::kDeviceCheckLanes);
    program.floats = crd::kir::kDeviceCheckFloats;
    program.words = crd::kir::kDeviceCheckWords;
    program.workgroups = crd::kir::kDeviceCheckGroups;
    program.verify = &crd::kir::device_check_mismatch;
    return DeviceCheckOutcome{run_device_check(probe, pipeline.get(), program), nullptr};
}

// The device a context opens: CUDA's devices in its own enumeration order (fastest first, or PCI bus order under
// CUDA_DEVICE_ORDER; CUDA_VISIBLE_DEVICES applies), each checked once per process; one that fails is passed over for
// the next. False when CUDA lists no device (nothing was chosen) or none passes (no device: the consumers' CPU path).
bool cuda_checked_device(CUdevice& out)
{
    int count = 0;
    if (cuDeviceGetCount(&count) != CUDA_SUCCESS || count <= 0)
    {
        return false;
    }
    DeviceSelection selection;
    for (int i = 0; i < count; ++i)
    {
        CUdevice device = 0;
        DeviceCheck check;
        if (cuDeviceGet(&device, i) == CUDA_SUCCESS)
        {
            check = check_device_once(cuda_adapter(device), static_cast<crd::u32>(i), &check_cuda_device, &device);
        }
        else
        {
            std::memcpy(check.adapter.backend, "cuda", sizeof("cuda")); // listed but not opened: an unknown device
            check.failed = "device";
        }
        if (i == 0)
        {
            selection.selected = check.adapter;
        }
        if (check.failed != nullptr)
        {
            if (i == 0)
            {
                selection.failed = check.failed;
                selection.injected = check.injected;
            }
            ++selection.adapters_failed;
            continue;
        }
        const DeviceCheckStatus first =
            check.unchecked != nullptr ? DeviceCheckStatus::Unchecked : DeviceCheckStatus::Passed;
        selection.status = i == 0 ? first : DeviceCheckStatus::Demoted;
        selection.runs = check.adapter;
        selection.unchecked = check.unchecked;
        device_checks().note_selection(selection);
        out = device;
        return true;
    }
    selection.status = DeviceCheckStatus::NoDevicePassed;
    device_checks().note_selection(selection);
    return false;
}

} // namespace

std::unique_ptr<CudaComputeContext> create_cuda_compute_context(crd::memory::IAllocator& alloc)
{
    return std::make_unique<CudaContextImpl>(alloc);
}

} // namespace crd::gpu
