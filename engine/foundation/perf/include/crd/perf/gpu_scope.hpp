#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- GPU timestamp scope (Detour D-003 v0d).
//
// Records pairs of GPU timestamps on a command buffer; once the GPU has
// retired the work and the host has resolved the query pool, the timing
// surfaces as a Category::Gpu Sample on a dedicated "gpu" profiler track.
//
// The actual backend (VkQueryPool wrapper, multi-frame-in-flight resolve)
// lives in `crd-rhi-vulkan` and implements `IProfilerGpuBackend`. The
// macro and the public surface here are backend-agnostic -- the cmd buffer
// is opaque (`void*`); the backend casts back to its concrete type.
//
// Usage:
//
//   // Once at startup, after crd::rhi::Device is created:
//   auto gpu_backend = crd::rhi::create_vulkan_profiler_backend(device);
//   crd::perf::set_gpu_backend(gpu_backend.get());
//
//   // Per-frame, around every render pass:
//   void Renderer::render_shadow_pass(crd::rhi::CommandBuffer& cb)
//   {
//       CRD_PERF_GPU_SCOPE(&cb, "shadow_pass");
//       // ... vkCmd* recording ...
//   }
//
//   // After queue submit / before vkAcquireNextImage / once per CPU frame:
//   crd::perf::resolve_gpu_frames();
//
// `resolve_gpu_frames()` is normally driven from `frame_mark()`. Calling
// it again is a cheap no-op (each frame is resolved only once).
//
// When CRD_PERF_ENABLED == 0 every macro collapses to `((void)0)`; the
// backend is never queried.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/perf/config.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/sample.hpp>

namespace crd::perf
{

// Opaque per-span handle. The concrete backend defines its meaning
// (Vulkan: index into a VkQueryPool slot pair).
struct GpuSpanHandle
{
    crd::u32 value = 0xFFFF'FFFFU;
    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0xFFFF'FFFFU; }
};

inline constexpr GpuSpanHandle kInvalidGpuSpan{0xFFFF'FFFFU};

// Resolved span -- timestamp pair in GPU ticks plus the NameId the caller
// recorded. Backend converts to a Sample at resolve time.
struct ResolvedGpuSpan
{
    NameId   name;
    crd::u64 begin_ticks;
    crd::u64 end_ticks;
};

// Backend interface. Implemented by `crd-rhi-vulkan` (VulkanProfilerBackend).
// One backend per process. `set_gpu_backend(nullptr)` clears.
class IProfilerGpuBackend
{
public:
    virtual ~IProfilerGpuBackend() = default;

    // Called once per profiler frame, before any begin_span for that frame.
    // Resets query slots for the new frame slot. The frame_index matches
    // crd::perf::frame_count().
    virtual void begin_frame(crd::u64 frame_index) noexcept = 0;

    // Record a "begin timestamp" on `cmd_buffer`. Returns a handle for the
    // matching end_span. `cmd_buffer` is the backend-typed command buffer
    // (cast inside the backend implementation).
    [[nodiscard]] virtual GpuSpanHandle begin_span(void* cmd_buffer, NameId name) noexcept = 0;

    // Record an "end timestamp" on `cmd_buffer` for the span returned by
    // begin_span.
    virtual void end_span(void* cmd_buffer, GpuSpanHandle span) noexcept = 0;

    // Bookkeeping after all spans for this frame are recorded (called from
    // resolve_gpu_frames). Marks the in-flight frame as "pending host
    // readback."
    virtual void end_frame() noexcept = 0;

    // Poll completed frames; convert GPU-tick timestamps to ns; push
    // Category::Gpu Samples into the profiler's "gpu" track. Idempotent:
    // a frame is resolved at most once.
    virtual void resolve_completed_frames() noexcept = 0;

    // GPU "tick rate" in nanoseconds per tick. Vulkan:
    // `VkPhysicalDeviceLimits::timestampPeriod`. Backend caches at init.
    [[nodiscard]] virtual crd::f64 ns_per_tick() const noexcept = 0;
};

// Install / replace / clear the backend. Pass nullptr to clear.
void set_gpu_backend(IProfilerGpuBackend* backend) noexcept;

// Returns the currently installed backend, or nullptr if none.
[[nodiscard]] IProfilerGpuBackend* current_gpu_backend() noexcept;

// Convenience: drive the resolve step. Safe to call every frame_mark; the
// backend skips frames whose GPU work hasn't retired yet.
void resolve_gpu_frames() noexcept;

// ---- The profiler's "gpu" track ----
//
// Resolved GPU spans land on a dedicated thread track registered by the
// backend at construction time. Use this index to query the spans from
// the timeline UI. Returns 0xFF when no GPU backend is active.
[[nodiscard]] crd::u8 gpu_thread_index() noexcept;

// Backend-side helper -- write a fully-formed Sample into the gpu thread's
// ring. The backend builds the Sample after resolving the query pool and
// converting ticks to ns. Public because the backend lives in a separate
// module (crd-rhi-vulkan); not intended for application code.
void emit_gpu_sample(Sample sample) noexcept;

// ---------------------------------------------------------------------------
// GpuScopedRegion -- RAII pair of begin_span / end_span on a cmd buffer.
// ---------------------------------------------------------------------------

#if CRD_PERF_ENABLED

class GpuScopedRegion
{
public:
    GpuScopedRegion(void* cmd_buffer, NameId name) noexcept
        : m_cmd_buffer(cmd_buffer)
    {
        IProfilerGpuBackend* be = current_gpu_backend();
        if (be != nullptr && cmd_buffer != nullptr)
        {
            m_span = be->begin_span(cmd_buffer, name);
        }
    }

    ~GpuScopedRegion() noexcept
    {
        if (!m_span.is_valid())
        {
            return;
        }
        IProfilerGpuBackend* be = current_gpu_backend();
        if (be != nullptr && m_cmd_buffer != nullptr)
        {
            be->end_span(m_cmd_buffer, m_span);
        }
    }

    GpuScopedRegion(const GpuScopedRegion&)            = delete;
    GpuScopedRegion& operator=(const GpuScopedRegion&) = delete;
    GpuScopedRegion(GpuScopedRegion&&)                 = delete;
    GpuScopedRegion& operator=(GpuScopedRegion&&)      = delete;

private:
    void*         m_cmd_buffer;
    GpuSpanHandle m_span{kInvalidGpuSpan};
};

#else

class GpuScopedRegion
{
public:
    GpuScopedRegion(void*, NameId) noexcept {}
    ~GpuScopedRegion() noexcept = default;

    GpuScopedRegion(const GpuScopedRegion&)            = delete;
    GpuScopedRegion& operator=(const GpuScopedRegion&) = delete;
    GpuScopedRegion(GpuScopedRegion&&)                 = delete;
    GpuScopedRegion& operator=(GpuScopedRegion&&)      = delete;
};

#endif

} // namespace crd::perf

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

#define CRD_PERF_GPU_DETAIL_CAT_INNER(a, b) a##b
#define CRD_PERF_GPU_DETAIL_CAT(a, b) CRD_PERF_GPU_DETAIL_CAT_INNER(a, b)

#if CRD_PERF_ENABLED

// CRD_PERF_GPU_SCOPE(cmd_void_ptr, "name") -- declare a GpuScopedRegion in
// the enclosing scope. The NameId is interned once per call site via a
// TU-local static. `cmd_void_ptr` must point to the backend's concrete
// command-buffer type (e.g. `&crd::rhi::CommandBuffer` for Vulkan).
//
// Cost when no backend installed: one nullptr load + branch-not-taken
// per call site (begin + end).
#define CRD_PERF_GPU_SCOPE(cmd_void_ptr, name_literal)                                                 \
    static const ::crd::perf::NameId CRD_PERF_GPU_DETAIL_CAT(_crd_perf_gpu_name_, __LINE__) =          \
        ::crd::perf::intern_name(name_literal);                                                        \
    ::crd::perf::GpuScopedRegion CRD_PERF_GPU_DETAIL_CAT(_crd_perf_gpu_scope_, __LINE__)               \
    {                                                                                                  \
        (cmd_void_ptr), CRD_PERF_GPU_DETAIL_CAT(_crd_perf_gpu_name_, __LINE__)                         \
    }

#else

#define CRD_PERF_GPU_SCOPE(cmd_void_ptr, name_literal) ((void)0)

#endif
