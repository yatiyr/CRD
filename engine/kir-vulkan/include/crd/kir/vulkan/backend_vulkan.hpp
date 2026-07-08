#pragma once

// backend_vulkan.hpp — Phase 3.1.6 v17-b: the CKIR **Vulkan backend** — `KirBackendVulkan` implements the KirBackend
// seam over crd-rhi-compute + crd-shader. It owns a headless Vulkan instance/device (+ a ValidationCapture) and, per
// kernel, emits GLSL (the v17-b emitter) → compiles to SPIR-V (crd-shader) → builds a compute pipeline → uploads
// inputs → dispatches → reads the output back. Gated GPU ≡ CPU-reference bit/ulp + `gpu_determinism_check` ×3.
// Separate module (ADR-0096 lean-consumer): only a consumer that wants the Vulkan backend links Vulkan. ADR-0098.

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
    [[nodiscard]] int         validation_errors() const noexcept; // ValidationCapture error count (DoD: 0)

    [[nodiscard]] bool run(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::kir
