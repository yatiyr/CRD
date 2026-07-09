#pragma once

// backend_vulkan.hpp — Phase 3.1.6 v17-b/v17-i: the CKIR **Vulkan backend** — `KirBackendVulkan` implements the
// KirBackend seam over the crd-gpu-context COMPUTE layer + crd-shader. It draws a headless Vulkan compute context from
// crd-gpu-context-vulkan (NO rendering RHI, dedicated compute queue) and, per kernel, emits GLSL → compiles to SPIR-V
// (crd-shader) → dispatches through a VulkanComputeContext (pipeline CACHED by SPIR-V hash) → reads the output back.
// Gated GPU ≡ CPU-reference bit/ulp + `gpu_determinism_check` ×3. Production only — no timing/benchmark methods; perf
// measurement lives in the test harness over the VulkanComputeContext. ADR-0098 + ADR-0099 (compute/rendering split).

#include <crd/kir/backend.hpp>

#include <memory>

namespace crd::kir
{

class KirBackendVulkan final : public KirBackend
{
public:
    explicit KirBackendVulkan(crd::memory::IAllocator* alloc);
    ~KirBackendVulkan() override;
    KirBackendVulkan(const KirBackendVulkan&)            = delete;
    KirBackendVulkan& operator=(const KirBackendVulkan&) = delete;
    KirBackendVulkan(KirBackendVulkan&&)                 = delete;
    KirBackendVulkan& operator=(KirBackendVulkan&&)      = delete;

    [[nodiscard]] const char* name() const noexcept override { return "vulkan"; }
    [[nodiscard]] bool        valid() const noexcept;             // false if no Vulkan device (skip GPU tests)
    [[nodiscard]] int         validation_errors() const noexcept; // 0 unless a validation layer is enabled on the context

    [[nodiscard]] bool run(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out) override;

    // MULTI-KERNEL scheduler (v17-e): execute an arbitrary multi-op graph as a SEQUENCE of GPU kernels with on-GPU
    // intermediate buffers (no CPU round-trips between phases). Materializes graph inputs, non-fusable ops, and their
    // operands; fuses elementwise cones between them; runs every kernel in ONE command buffer. Unblocks radix sort / LBVH.
    [[nodiscard]] bool run_graph(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::kir
