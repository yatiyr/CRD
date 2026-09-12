// ---------------------------------------------------------------------------
// crd-perf -- GPU backend registration + sample emission (Detour D-003 v0d).
//
// Single-subscriber backend pointer (atomic acquire/release). The backend
// is supplied by `crd-rhi-vulkan` (VulkanProfilerBackend) -- crd-perf itself
// has no Vulkan dependency.
//
// emit_gpu_sample() is the bridge from "I resolved a query pool slot" to
// "now a Category::Gpu Sample lives on the gpu track." The backend
// registers a thread named "gpu" at construction (lazy on first emit if it
// forgot); resolved Samples land in that thread's ring like any CPU Sample.
// ---------------------------------------------------------------------------

#include <crd/perf/gpu_scope.hpp>

#include <crd/core/assert.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/sample.hpp>

#include <atomic>

namespace crd::perf
{

#if CRD_PERF_ENABLED

namespace
{

std::atomic<IProfilerGpuBackend*> g_gpu_backend{nullptr};
std::atomic<crd::u8>              g_gpu_thread_index{0xFFU};

void ensure_gpu_thread_registered() noexcept
{
    if (g_gpu_thread_index.load(std::memory_order_acquire) != 0xFFU)
    {
        return;
    }
    // Lazy register a "gpu" thread on first emit. The backend constructor
    // is the natural place to do this on a worker-controlled thread, but we
    // fall back to lazy in case the backend forgot.
    const crd::u8 idx = register_thread("gpu");
    g_gpu_thread_index.store(idx, std::memory_order_release);
}

} // namespace

void set_gpu_backend(IProfilerGpuBackend* backend) noexcept
{
    g_gpu_backend.store(backend, std::memory_order_release);
    if (backend != nullptr)
    {
        // Pre-register the "gpu" thread now so the resolve loop can write
        // Samples without taking the lazy-init path on a hot path.
        ensure_gpu_thread_registered();
    }
    else
    {
        // Clearing the backend leaves the gpu thread index in place --
        // resolved Samples that arrived earlier remain queryable in the
        // ring. A reinstalled backend will reuse the same index.
    }
}

[[nodiscard]] IProfilerGpuBackend* current_gpu_backend() noexcept
{
    return g_gpu_backend.load(std::memory_order_acquire);
}

void resolve_gpu_frames() noexcept
{
    IProfilerGpuBackend* be = current_gpu_backend();
    if (be == nullptr)
    {
        return;
    }
    be->resolve_completed_frames();
}

[[nodiscard]] crd::u8 gpu_thread_index() noexcept
{
    return g_gpu_thread_index.load(std::memory_order_acquire);
}

// ---- emit_gpu_sample ---------------------------------------------------
//
// Backend-side helper: build a fully-formed Sample and stamp it into the
// gpu thread's ring. The caller (VulkanProfilerBackend::resolve_completed_frames)
// is responsible for converting Vulkan tick counts to ns and assigning
// begin_ns / end_ns on the monotonic-clock scale.
//
// Implementation note: the existing push_region / pop_region path is
// thread-local; it always writes to the *current* thread's ring. The GPU
// resolve runs on the CPU thread that called resolve_gpu_frames() (the
// main thread, typically), so its t_ring points to the main thread, not
// the gpu thread. We bypass that and write directly into the gpu thread's
// ring using a dedicated helper that knows the target thread index.
//
// This is the only legitimate "external sample write" path in crd-perf.
// User code that wants to push a sample into a non-current-thread ring
// should use jobs / scope / counter macros instead.

namespace detail
{
// Forward declaration of the SPSC ring's external-write helper, defined
// in profiler.cpp where the ring's storage is private.
void write_external_sample(crd::u8 thread_index, const Sample& s) noexcept;
} // namespace detail

void emit_gpu_sample(Sample sample) noexcept
{
    ensure_gpu_thread_registered();
    const crd::u8 idx = g_gpu_thread_index.load(std::memory_order_acquire);
    if (idx == 0xFFU)
    {
        return; // Profiler not initialised; resolve is a no-op.
    }
    // Force the category + thread fields so backends can't accidentally
    // ship a Sample tagged with the wrong category onto the gpu track.
    sample.category     = static_cast<crd::u8>(Category::Gpu);
    sample.begin_thread = idx;
    sample.end_thread   = idx;
    detail::write_external_sample(idx, sample);
}

namespace detail
{

// Called by profiler.cpp::shutdown(). The profiler is tearing down the
// thread table, so any cached "gpu" thread index becomes stale. Clear it
// alongside the backend pointer so the next init() starts fresh.
void reset_gpu_state() noexcept
{
    g_gpu_backend.store(nullptr, std::memory_order_release);
    g_gpu_thread_index.store(0xFFU, std::memory_order_release);
}

} // namespace detail

#else // CRD_PERF_ENABLED == 0

void set_gpu_backend(IProfilerGpuBackend*) noexcept {}
[[nodiscard]] IProfilerGpuBackend* current_gpu_backend() noexcept { return nullptr; }
void resolve_gpu_frames() noexcept {}
[[nodiscard]] crd::u8 gpu_thread_index() noexcept { return 0xFFU; }
void emit_gpu_sample(Sample) noexcept {}

#endif

} // namespace crd::perf
