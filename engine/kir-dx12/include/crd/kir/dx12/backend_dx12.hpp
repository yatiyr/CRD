#pragma once

// backend_dx12.hpp — Phase 3.1.6 v17-d: the CKIR **DirectX 12 backend** — `KirBackendDx12` implements the KirBackend
// seam over raw D3D12 compute + dxc (HLSL → DXIL at runtime). NOT through crd-rhi (rhi is Vulkan). A separate module
// (ADR-0096 lean-consumer): only a consumer that wants the D3D12 backend links D3D12/dxc. Uses `RWStructuredBuffer`
// UAVs + root constants; `precise` HLSL ⇒ bit-exact vs the CPU reference for correctly-rounded ops (division is a fast
// reciprocal on the GPU ⇒ ULP, like Vulkan). ADR-0098. Third bit-exact backend from the same CKIR IR.

#include <crd/kir/backend.hpp>

#include <memory>

namespace crd::kir
{

class KirBackendDx12 final : public KirBackend
{
public:
    explicit KirBackendDx12(crd::memory::IAllocator* alloc);
    ~KirBackendDx12() override;
    KirBackendDx12(const KirBackendDx12&)            = delete;
    KirBackendDx12& operator=(const KirBackendDx12&) = delete;
    KirBackendDx12(KirBackendDx12&&)                 = delete;
    KirBackendDx12& operator=(KirBackendDx12&&)      = delete;

    [[nodiscard]] const char* name() const noexcept override { return "dx12"; }
    [[nodiscard]] bool        valid() const noexcept; // false if no D3D12 device / dxc

    [[nodiscard]] bool run(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::kir
