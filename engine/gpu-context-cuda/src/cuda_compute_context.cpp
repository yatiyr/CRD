// crd-gpu-context-cuda — CudaComputeContext : crd::gpu::IComputeContext (ADR-0100, user-directed 2026-08-07). CUDA
// driver API + NVRTC (CUDA C -> CUBIN). Mirrors engine/kir-cuda's proven patterns (shared primary context, CUBIN not
// PTX, event timing). See cuda_compute_context.hpp for the sharing + barrier-no-op rationale.

#include <crd/gpu/cuda_compute_context.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <cuda.h>
#include <nvrtc.h>

#include <cstdio>
#include <cstring>

namespace crd::gpu
{
namespace
{
// NVRTC: compile CUDA C (`src`, null-terminated) → a CUBIN for the device's exact `arch` (e.g. "sm_89"). CUBIN, not PTX:
// newer NVRTC emits a PTX version the driver JIT rejects (cuModuleLoadData error 222). Fast tier (--fmad=true) — this is
// a compute-dispatch surface, not the bit-exact determinism path (that lives in kir-cuda).
[[nodiscard]] bool compile_cubin(const char* src, const char* arch, crd::containers::Array<char>& cubin)
{
    nvrtcProgram prog{};
    if (nvrtcCreateProgram(&prog, src, "crd_cuda.cu", 0, nullptr, nullptr) != NVRTC_SUCCESS) { return false; }
    char archopt[64];
    std::snprintf(archopt, sizeof(archopt), "--gpu-architecture=%s", arch);
    const char*       opts[] = {"--fmad=true", archopt};
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
        if (m_pinned) { if (m_host != nullptr) { cuMemFreeHost(m_host); } }
        else if (m_dptr != 0U) { cuMemFree(m_dptr); }
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
    CudaPipeline(CUmodule mod, CUfunction fn, int n_bindings, crd::u32 /*push_size*/) noexcept
        : m_mod(mod), m_fn(fn), m_n(n_bindings) {}
    ~CudaPipeline() override { if (m_mod != nullptr) { cuModuleUnload(m_mod); } }
    CudaPipeline(const CudaPipeline&)            = delete;
    CudaPipeline& operator=(const CudaPipeline&) = delete;
    CudaPipeline(CudaPipeline&&)                 = delete;
    CudaPipeline& operator=(CudaPipeline&&)      = delete;

    [[nodiscard]] CUfunction fn() const noexcept { return m_fn; }
    [[nodiscard]] int        n_bindings() const noexcept { return m_n; }

private:
    CUmodule    m_mod  = nullptr;
    CUfunction  m_fn   = nullptr;
    int         m_n    = 0;
};

// ⭐ CUDA local-size convention: the IComputeContext dispatch surface passes only the GRID dims (gx,gy,gz) — the Vulkan
// model bakes the block size into the SPIR-V, CUDA specifies it at launch. This backend launches a fixed 256-thread 1-D
// block; a CUDA kernel for this surface indexes `blockIdx.x*blockDim.x + threadIdx.x` and guards its element count.
inline constexpr crd::u32 kCudaBlock = 256U;
// Local params cap — a compute backend has no dependency on the raster command_model's kMaxBindings.
inline constexpr crd::u32 kCudaMaxBindings = 16U;

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
            if (n > static_cast<int>(kCudaMaxBindings)) { return; }
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
            cuLaunchKernel(cp.fn(), gx > 0U ? gx : 1U, gy > 0U ? gy : 1U, gz > 0U ? gz : 1U, kCudaBlock, 1U, 1U, 0U,
                           m_c.m_stream, params, nullptr);
        }

    private:
        CudaContextImpl& m_c;
    };

public:
    explicit CudaContextImpl(crd::memory::IAllocator& alloc) noexcept : m_alloc(alloc), m_rec(*this)
    {
        if (cuInit(0) != CUDA_SUCCESS) { return; }
        int count = 0;
        if (cuDeviceGetCount(&count) != CUDA_SUCCESS || count <= 0) { return; }
        if (cuDeviceGet(&m_device, 0) != CUDA_SUCCESS) { return; }
        // Retain the device PRIMARY context — the refcounted singleton kir-cuda also retains ⇒ shared device, no dup init.
        if (cuDevicePrimaryCtxRetain(&m_ctx, m_device) != CUDA_SUCCESS) { return; }
        if (cuCtxSetCurrent(m_ctx) != CUDA_SUCCESS) { return; }
        if (cuStreamCreate(&m_stream, CU_STREAM_DEFAULT) != CUDA_SUCCESS) { return; }
        cuEventCreate(&m_ev0, CU_EVENT_DEFAULT);
        cuEventCreate(&m_ev1, CU_EVENT_DEFAULT);
        int major = 0;
        int minor = 0;
        cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, m_device);
        cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, m_device);
        std::snprintf(m_arch, sizeof(m_arch), "sm_%d%d", major, minor);
        int warp = 0;
        int shmem = 0;
        cuDeviceGetAttribute(&warp, CU_DEVICE_ATTRIBUTE_WARP_SIZE, m_device);
        cuDeviceGetAttribute(&shmem, CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK, m_device);
        m_warp  = warp > 0 ? static_cast<crd::u32>(warp) : 32U;
        m_shmem = shmem > 0 ? static_cast<crd::u32>(shmem) : 49152U;
        m_ok    = true;
    }

    ~CudaContextImpl() override
    {
        if (m_ev0 != nullptr) { cuEventDestroy(m_ev0); }
        if (m_ev1 != nullptr) { cuEventDestroy(m_ev1); }
        if (m_stream != nullptr) { cuStreamDestroy(m_stream); }
        if (m_ctx != nullptr) { cuDevicePrimaryCtxRelease(m_device); } // release our refcount on the shared primary ctx
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
        if (!m_ok || bytes == 0U) { return nullptr; }
        if (memory == ComputeMemory::GpuOnly)
        {
            CUdeviceptr d = 0U;
            if (cuMemAlloc(&d, bytes) != CUDA_SUCCESS) { return nullptr; }
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
                                                                             int n_bindings, crd::u32 push_size) override
    {
        if (!m_ok) { return nullptr; }
        crd::containers::Array<char> src(&m_alloc);
        src.resize(cuda_source.size() + 1U, '\0');
        for (crd::usize i = 0; i < cuda_source.size(); ++i) { src[i] = cuda_source[i]; }
        crd::containers::Array<char> name(&m_alloc);
        name.resize(entry.size() + 1U, '\0');
        for (crd::usize i = 0; i < entry.size(); ++i) { name[i] = entry[i]; }

        crd::containers::Array<char> cubin(&m_alloc);
        if (!compile_cubin(src.data(), m_arch, cubin)) { return nullptr; }
        CUmodule mod = nullptr;
        if (cuModuleLoadData(&mod, cubin.data()) != CUDA_SUCCESS) { return nullptr; }
        CUfunction fn = nullptr;
        if (cuModuleGetFunction(&fn, mod, name.data()) != CUDA_SUCCESS)
        {
            cuModuleUnload(mod);
            return nullptr;
        }
        return std::make_unique<CudaPipeline>(mod, fn, n_bindings, push_size);
    }

    [[nodiscard]] ComputeRecorder& begin() override
    {
        if (m_ok) { cuEventRecord(m_ev0, m_stream); } // bracket the recorded work for last_gpu_ms
        return m_rec;
    }

    void submit_and_wait() override
    {
        if (!m_ok) { return; }
        cuEventRecord(m_ev1, m_stream);
        cuStreamSynchronize(m_stream);
        float ms = 0.0F;
        if (cuEventElapsedTime(&ms, m_ev0, m_ev1) == CUDA_SUCCESS) { m_last_ms = static_cast<double>(ms); }
    }

    [[nodiscard]] crd::u32 subgroup_size() const noexcept override { return m_warp; }
    [[nodiscard]] crd::u32 shared_memory_bytes() const noexcept override { return m_shmem; }
    [[nodiscard]] double   last_gpu_ms() const noexcept override { return m_last_ms; }

private:
    crd::memory::IAllocator& m_alloc;
    Recorder                 m_rec;
    CUdevice                 m_device = 0;
    CUcontext                m_ctx    = nullptr;
    CUstream                 m_stream = nullptr;
    CUevent                  m_ev0    = nullptr;
    CUevent                  m_ev1    = nullptr;
    char                     m_arch[16] = {'s', 'm', '_', '5', '2', '\0'};
    crd::u32                 m_warp   = 32U;
    crd::u32                 m_shmem  = 49152U;
    double                   m_last_ms = 0.0;
    bool                     m_ok     = false;
};

} // namespace

std::unique_ptr<CudaComputeContext> create_cuda_compute_context(crd::memory::IAllocator& alloc)
{
    return std::make_unique<CudaContextImpl>(alloc);
}

} // namespace crd::gpu
