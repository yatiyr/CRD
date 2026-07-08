#pragma once

// backend_webgpu.hpp — Phase 3.1.6 v17-d: the CKIR **WebGPU backend** — `KirBackendWebGpu` implements the KirBackend
// seam over the WebGPU C API (wgpu-native locally; the SAME code compiles to WASM against the browser's WebGPU — the
// portability payoff). A separate module (ADR-0096 lean-consumer). Storage buffers + a uniform for dims; WGSL has no
// `precise`, so results are ULP-tolerant vs the CPU reference (an implementation MAY fuse FMAs) — honest, not
// bit-exact like Vulkan/CUDA/DX12. ADR-0098. THE browser/everywhere path.

#include <crd/kir/backend.hpp>

#include <memory>

namespace crd::kir
{

class KirBackendWebGpu final : public KirBackend
{
public:
    explicit KirBackendWebGpu(crd::memory::IAllocator* alloc);
    ~KirBackendWebGpu() override;
    KirBackendWebGpu(const KirBackendWebGpu&)            = delete;
    KirBackendWebGpu& operator=(const KirBackendWebGpu&) = delete;
    KirBackendWebGpu(KirBackendWebGpu&&)                 = delete;
    KirBackendWebGpu& operator=(KirBackendWebGpu&&)      = delete;

    [[nodiscard]] const char* name() const noexcept override { return "webgpu"; }
    [[nodiscard]] bool        valid() const noexcept;

    [[nodiscard]] bool run(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::kir
