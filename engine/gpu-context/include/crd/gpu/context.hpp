#pragma once

// crd-gpu-context — the backend-agnostic GPU **context** foundation (ADR-0099). An `IGpuContext` is a *live GPU device*
// (instance / logical device / queues / allocator) with **no** compute or rendering semantics — `ComputeDevice` and
// `RenderDevice` (separate interfaces) wrap it. A `GpuContextManager` owns the configured set of contexts (which
// backends, headless vs windowed, shared vs separate), created by per-backend factories (e.g. `create_vulkan_gpu_context`).
// This is the deep module both the compute backends (CKIR, geometry-bvh-gpu) and the renderer draw from — independent
// concerns, composed by choice. Per ADR-0099 (supersedes the compute/rendering coupling of the RHI reuse).

#include <crd/core/types.hpp>

#include <memory>

namespace crd::gpu
{

enum class GpuBackend : crd::u8
{
    Vulkan,
    Cuda,
    Metal,
    Dx12,
    WebGpu,
    Hip,
};

// How to create a context. Compute-only defaults to `headless = true` (no swapchain/surface).
struct GpuContextConfig
{
    GpuBackend backend           = GpuBackend::Vulkan;
    bool       headless          = true;  // pure compute — no surface/swapchain
    bool       enable_validation = false; // debug layers (off by default; the compute hot path never wants them)
};

// A live GPU device foundation — backend-agnostic. Consumers that need backend handles downcast to the concrete
// backend context (e.g. `crd::gpu::VulkanGpuContext`) exactly the way `ComputeDevice`/`RenderDevice` impls do.
class IGpuContext
{
public:
    IGpuContext()                              = default;
    virtual ~IGpuContext()                     = default;
    IGpuContext(const IGpuContext&)            = delete;
    IGpuContext& operator=(const IGpuContext&) = delete;
    IGpuContext(IGpuContext&&)                 = delete;
    IGpuContext& operator=(IGpuContext&&)      = delete;

    [[nodiscard]] virtual bool        valid() const noexcept        = 0; // false ⇒ no usable device (skip / fall back)
    [[nodiscard]] virtual GpuBackend  backend() const noexcept      = 0;
    [[nodiscard]] virtual const char* adapter_name() const noexcept = 0;
};

// Owns the configured set of live contexts. Per-backend factories construct them; the manager holds + serves them, so
// exactly one place decides which backends are live and whether any are shared with the renderer. Lean by construction:
// it never pulls a backend's headers — it only stores `IGpuContext` handles the caller hands it.
class GpuContextManager
{
public:
    GpuContextManager()                                    = default;
    ~GpuContextManager()                                   = default;
    GpuContextManager(const GpuContextManager&)            = delete;
    GpuContextManager& operator=(const GpuContextManager&) = delete;
    GpuContextManager(GpuContextManager&&)                 = delete;
    GpuContextManager& operator=(GpuContextManager&&)      = delete;

    // Take ownership of a context created by a per-backend factory. Returns a borrowed pointer (the manager owns it),
    // or nullptr if the context is invalid or the registry is full.
    IGpuContext* add(std::unique_ptr<IGpuContext> ctx) noexcept
    {
        if (ctx == nullptr || !ctx->valid() || m_count >= kMax) { return nullptr; }
        m_contexts[m_count] = std::move(ctx);
        return m_contexts[m_count++].get();
    }

    // First live context for a backend, or nullptr.
    [[nodiscard]] IGpuContext* get(GpuBackend backend) noexcept
    {
        for (int i = 0; i < m_count; ++i) { if (m_contexts[i]->backend() == backend) { return m_contexts[i].get(); } }
        return nullptr;
    }

    [[nodiscard]] int count() const noexcept { return m_count; }

private:
    static constexpr int         kMax = 8; // one live context per backend is the norm; 8 leaves head-room
    std::unique_ptr<IGpuContext> m_contexts[kMax];
    int                          m_count = 0;
};

} // namespace crd::gpu
